/**
 * @file apex_host.h
 * @brief Host-side core (APEX wire v1): device table, discovery, the
 *        provisional configuration phase (class-version selection, mass and
 *        physical-data policy), heartbeat/watchdog, HOST_STATE broadcast, baud
 *        negotiation, VERSION_BEACON emission, and dispatch of class traffic.
 *
 * Lifecycle in one paragraph: the caller initializes an apex_host_t with a TX
 * callback and (optionally) policy/event callbacks, and registers each class it
 * supports (with a class-version range) via apex_host_register_class[_versioned].
 * Bytes received on the UART are pushed in with apex_host_feed_rx(). The caller
 * invokes apex_host_tick() periodically — at least once per second, ideally
 * 10 Hz or faster — to drive HOST_STATE broadcasts, the per-device 5 s watchdog,
 * and the provisional-phase query loop (PHYS_REQUEST retries). Class-specific
 * traffic is dispatched to handlers registered via apex_host_register_class().
 *
 * Copyright (c) 2026 Neros Technologies. MIT License — see LICENSE.
 */
#ifndef APEX_HOST_H
#define APEX_HOST_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "apex/apex_core.h"
#include "apex/apex_framer.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Compile-time tuning. Override these by defining them before including this
 * header (e.g., via a -D flag). */
#ifndef APEX_HOST_MAX_DEVICES
#define APEX_HOST_MAX_DEVICES 16
#endif

#ifndef APEX_HOST_MAX_CLASSES
#define APEX_HOST_MAX_CLASSES 4
#endif

/* How long a slot lingers in FAULT before the host frees it and returns the
 * slot + device_id to the pool (§3.3.1 slot recycling). "A few seconds" past the
 * 5 s heartbeat watchdog that produced the FAULT. */
#ifndef APEX_HOST_FAULT_RECYCLE_MS
#define APEX_HOST_FAULT_RECYCLE_MS 5000u
#endif

/* Provisional-phase PHYS_REQUEST retry policy (§3.2.9): 3 × 500 ms. */
#ifndef APEX_HOST_PHYS_RETRIES
#define APEX_HOST_PHYS_RETRIES 3u
#endif
#ifndef APEX_HOST_PHYS_RETRY_MS
#define APEX_HOST_PHYS_RETRY_MS 500u
#endif

/* TX callback — the library hands the caller a fully encoded on-wire frame
 * (COBS-encoded with a trailing 0x00 delimiter). The caller pushes those
 * bytes onto the UART. */
typedef void (*apex_host_tx_cb_t)(void *user, const uint8_t *bytes, size_t n);

/* Class-traffic callback — fired for each frame whose traffic_type matches a
 * previously-registered handler. */
typedef void (*apex_host_class_rx_cb_t)(void *user,
                                        uint8_t device_id,
                                        const uint8_t *payload,
                                        size_t payload_len);

/* Device-event callback — fired on every status transition. */
typedef void (*apex_host_device_event_cb_t)(void *user,
                                            uint8_t device_id,
                                            apex_device_status_t status);

/* NAME_REPLY callback. `name` is not NUL-terminated; `len` is the
 * payload-declared length. The string is ASCII per §3.2.4. */
typedef void (*apex_host_name_reply_cb_t)(void *user,
                                          uint8_t device_id,
                                          const char *name,
                                          size_t len);

/* Mass policy (§3.2.2). Return true to accept the declared payload mass, false
 * to reply ACK_REJECT_MASS. Absent (NULL) = accept any mass. */
typedef bool (*apex_host_mass_policy_cb_t)(void *user, uint16_t mass_grams);

/* Physical-data policy (§3.2.11). Called with a device's PHYS_INFO decoded
 * into host order. Absent = accept.
 *
 * Pre-latch (provisional phase): returning false replies
 * PHYS_ACK(ACK_REJECT_PHYS) as a terminal pre-commitment reject — the slot is
 * freed. `device_id` is the provisional slot's internal id (withheld from the
 * device until the terminal ACK_OK).
 *
 * Post-CONNECTED (in-flight update or re-query reply): returning
 * false replies PHYS_ACK(ACK_REJECT_PHYS) as an ADVISORY envelope signal only
 * — no session effect, the slot stays CONNECTED, and the stored physical state
 * still updates (it is reality). Eviction, if actually wanted, is
 * apex_host_evict() / RESET_REQUEST + ACK_REJECT_POLICY. */
typedef bool (*apex_host_phys_policy_cb_t)(void *user,
                                           uint8_t device_id,
                                           const apex_phys_t *phys);

/* Physical-update notification: an unsolicited post-CONNECTED PHYS_INFO
 * (or a re-query reply) updated the stored physical state for `device_id`.
 * Latest-wins; `phys` is the complete current state including mass. */
typedef void (*apex_host_phys_update_cb_t)(void *user,
                                           uint8_t device_id,
                                           const apex_phys_t *phys);

/* Baud grant policy (§3.4). Return true to grant a device's proposed baud code,
 * false to reject with a counter-offer hint. Absent = grant iff
 * requested <= cfg.max_baud_code. */
typedef bool (*apex_host_baud_policy_cb_t)(void *user,
                                           uint8_t device_id,
                                           apex_baud_code_t requested);

/* Rate-switch notification (§3.4). Fired after the host transmits a granting
 * BAUD_CHANGE_ACK; the host app owns the UART and MUST switch it to `code`. */
typedef void (*apex_host_baud_switch_cb_t)(void *user,
                                           uint8_t device_id,
                                           apex_baud_code_t code);

typedef struct {
    /* OR of APEX_INTERFACE_FLAG_* that this host can grant a device. */
    uint8_t supported_interfaces;

    /* HOST_STATE broadcast period. 1000 ms is the §3.2.5 floor; setting to 0
     * disables HOST_STATE broadcast. */
    uint32_t host_state_period_ms;

    /* Whether to open the provisional configuration phase to run a PHYS query
     * before committing (§3.3). When true, an otherwise-acceptable DEVICE_INFO
     * is answered with ACK_PROVISIONAL and the host runs PHYS_REQUEST. */
    bool run_phys_provisional;

    /* When true, a device that never answers PHYS_REQUEST (retries exhaust) is
     * denied: the host may send an unsolicited PHYS_ACK(ACK_REJECT_PHYS), frees
     * the slot, and answers that device's next DEVICE_INFO with
     * ACK_REJECT_POLICY (§3.2.11). When false, a phys timeout concludes the
     * phase with a terminal ACK_OK anyway. Only meaningful with
     * run_phys_provisional. */
    bool phys_required;

    /* Highest baud code the host can grant (§3.4). 0 = default only. */
    uint8_t max_baud_code;

    /* Suppress the boot sweep (RESET_REQUEST re-enumeration, msg 13). Default
     * false = the host broadcasts RESET_REQUEST 3x within its first second of
     * ticks after init, sweeping stale sessions from a previous host
     * incarnation (brown-out recovery). Remaining sweeps are cancelled early
     * once a device enumerates with this incarnation (the sweep's work on this
     * link is provably done). Set true to disable entirely. */
    bool suppress_boot_sweep;

    /* VERSION_BEACON emission period while the port is unlinked. Defaults
     * to 1000 ms (1 Hz) if 0; legal bounds 10..1000 ms (100 Hz..1 Hz). The host
     * beacons whenever it holds no device slot (no session on this port) and
     * CONTINUES beaconing after the device's beacon is received (tolerating
     * lost beacons); it stops as soon as a slot exists (PROVISIONAL counts) and
     * resumes — with the peer-receipt gate reset — when the port unlinks again
     * (all slots freed). While the peer's advertised range is disjoint from v1
     * the cadence drops to 0.1 Hz. */
    uint32_t beacon_period_ms;

    /* Required: TX callback. */
    apex_host_tx_cb_t tx;
    void *tx_user;

    /* Optional policy hooks. */
    apex_host_mass_policy_cb_t mass_policy_cb;
    void *mass_policy_user;
    apex_host_phys_policy_cb_t phys_policy_cb;
    void *phys_policy_user;
    apex_host_phys_update_cb_t on_phys_update;
    void *on_phys_update_user;
    apex_host_baud_policy_cb_t baud_policy_cb;
    void *baud_policy_user;
    apex_host_baud_switch_cb_t baud_switch_cb;
    void *baud_switch_user;

    /* Optional event hooks. */
    apex_host_device_event_cb_t on_device_event;
    void *on_device_event_user;
    apex_host_name_reply_cb_t on_name_reply;
    void *on_name_reply_user;
} apex_host_cfg_t;

/* Provisional-phase query state for a PROVISIONAL slot (§3.3). */
typedef enum {
    APEX_HOST_PROV_NONE = 0, /* Simple ACK_OK accept: nothing to query. */
    APEX_HOST_PROV_PHYS,     /* Running the PHYS_REQUEST retry loop. */
} apex_host_prov_query_t;

typedef struct {
    uint8_t device_id;          /* assigned ID; 0x01 (UNASSIGNED) = slot empty */
    apex_device_status_t status;
    uint8_t device_class;       /* the traffic_type the device declared */
    uint8_t interface_flags;
    uint8_t selected_class_version;  /* version this session will run (§3.6.1) */
    uint8_t host_class_min;     /* host range decided for this slot (diagnostic) */
    uint8_t host_class_max;
    uint16_t mass_grams;        /* current mass (DEVICE_INFO at discovery;
                                 * updated by PHYS_INFO, latest-wins) */
    bool has_phys;              /* a PHYS_INFO has been stored for this slot */
    apex_phys_t phys;           /* stored complete physical state:
                                 * config-time query result and/or the latest
                                 * in-flight update — one apex_phys_t per
                                 * physical unit, latest-wins */
    uint8_t reply_ack;          /* the CONFIG_REPLY ack we sent (ACK_OK /
                                 * ACK_PROVISIONAL) — replayed on dedup. */
    apex_host_prov_query_t prov_query;  /* outstanding provisional query, if any */
    uint8_t  phys_reqs_sent;    /* PHYS_REQUESTs sent so far this phase */
    uint32_t phys_next_ms;      /* when the next PHYS_REQUEST / timeout is due */
    uint32_t last_rx_ms;        /* last inbound frame attributable to this slot */
    uint32_t status_since_ms;   /* when the slot entered its current status */
} apex_host_device_slot_t;

typedef struct {
    uint8_t traffic_type;
    bool registered;
    uint8_t class_version_min;  /* host-supported class-version range (§3.6) */
    uint8_t class_version_max;
    apex_host_class_rx_cb_t rx;
    void *user;
} apex_host_class_reg_t;

typedef struct apex_host {
    apex_host_cfg_t cfg;
    apex_framer_rx_t framer;
    apex_host_device_slot_t devices[APEX_HOST_MAX_DEVICES];
    apex_host_class_reg_t classes[APEX_HOST_MAX_CLASSES];
    uint8_t next_assign_id;      /* monotonic counter over the 0x02–0xFE pool */
    uint8_t flight_state;
    uint8_t warnings;            /* §3.2.5 advisory warnings bitfield, orthogonal to flight_state */
    uint32_t now_ms;
    uint32_t last_host_state_tx_ms;
    bool host_state_ever_sent;
    /* Sticky discovery dedup: the device_id of the PROVISIONAL slot the host is
     * currently holding for the (at most one, §3.3.1) unassigned device on the
     * link. While that slot is PROVISIONAL the host attributes device_id=0x01
     * frames to it and resends its CONFIG_REPLY on repeated DEVICE_INFO instead
     * of allocating a fresh slot. 0x01 (UNASSIGNED) = no pending assignment. */
    uint8_t last_unassigned_id;
    /* Policy deny latch (§3.2.11). Armed on a phys-required PHYS-query
     * exhaustion or by apex_host_evict(): every DEVICE_INFO from an unassigned
     * device requesting deny_class is answered ACK_REJECT_POLICY. Lifetime:
     * the latch stays armed while matching DEVICE_INFOs keep arriving (so
     * duplicates already in flight when the first deny lands are denied too,
     * not accepted) and disarms after 2 x APEX_HEARTBEAT_WATCHDOG_MS of latch
     * silence — long enough to cover a phys-timeout-denied device sitting out
     * one full watchdog period before its consuming re-discovery arrives. A
     * denied device goes REJECTED and stops, so ~10 s later the port is free
     * for a genuinely different payload of the same class. Cleared early by
     * host reset. */
    bool     deny_pending;
    uint8_t  deny_class;
    uint32_t deny_last_ms;   /* armed / last matched — drives the disarm timer */
    /* Proactive beaconing + mutual-receipt gate. The host beacons
     * periodically while the port is UNLINKED and answers DEVICE_INFO only
     * after the device's beacon closed the version tier. A port is
     * "linked" while a PROVISIONAL or CONNECTED slot is bound to it — a FAULT
     * slot does NOT hold the link (the watchdog has already spoken), so
     * beaconing for a possible hot-swapped replacement resumes at the FAULT
     * transition, not at slot recycling. Gate + cadence reset on the
     * linked->unlinked transition (detected in tick). A newcomer's beacon
     * never kills a live session: while linked, inbound beacons are ignored —
     * the watchdog is the sole arbiter of the old session's death. */
    bool     port_was_linked;       /* previous tick's link state (transition
                                     * detector) */
    bool     beacon_ever_sent;      /* emission cadence anchor validity */
    uint32_t last_beacon_tx_ms;
    bool     device_beacon_seen;    /* gate: peer beacon received, ranges meet */
    bool     peer_incompatible;     /* peer range disjoint from v1 — beacon at
                                     * <= 0.1 Hz until a compatible one arrives */
    uint16_t peer_min_version;      /* device's advertised wire-version range */
    uint16_t peer_max_version;
    uint32_t beacons_seen;          /* inbound beacons (diagnostic) */
    uint32_t beacons_incompatible;  /* inbound beacons with disjoint ranges */
    uint32_t unsupported_pv_frames; /* non-v1 session frames dropped (
                                     * silent — no reactive transmission) */
    /* Boot sweep (msg 13): broadcast RESET_REQUEST 3x within the first second
     * of ticks, unless cfg.suppress_boot_sweep. */
    bool     boot_sweep_anchored;
    uint32_t boot_sweep_t0_ms;
    uint8_t  boot_sweeps_sent;
    /* Link rate the host granted (§3.4) and which device's session raised it —
     * used to revert to the default and notify via baud_switch_cb when that
     * session resets (msg 13) or the slot recycles. Point-to-point link scope. */
    uint8_t  link_baud_code;
    uint8_t  link_baud_owner;    /* device_id that negotiated the raise; 0 = n/a */
    /* Reactive RESET_REQUEST rate-limit (≤ 1 Hz per unknown assigned id).
     * Small LRU keyed by device_id; sized to the device table. */
    struct {
        uint8_t  device_id;      /* 0 = entry unused */
        uint32_t last_tx_ms;
    } reactive_rl[APEX_HOST_MAX_DEVICES];
    uint32_t unknown_id_frames;  /* §3.8 unknown-id drops (diagnostic "log"). */
} apex_host_t;

/* ---------------------------------------------------------------------------
 * Lifecycle
 * ------------------------------------------------------------------------- */

void apex_host_init(apex_host_t *h, const apex_host_cfg_t *cfg);

/* Register a handler for class traffic with the given traffic_type, supporting
 * class versions [class_version_min, class_version_max] (contiguous, §3.6).
 * Returns APEX_ERR_FULL if APEX_HOST_MAX_CLASSES is exceeded, or
 * APEX_ERR_INVALID_ARGS for traffic_type == CONFIG or an inverted range. */
apex_status_t apex_host_register_class_versioned(apex_host_t *h,
                                                 uint8_t traffic_type,
                                                 uint8_t class_version_min,
                                                 uint8_t class_version_max,
                                                 apex_host_class_rx_cb_t cb,
                                                 void *user);

/* Convenience wrapper: registers the class supporting only class version 1
 * (the sole v1 default). */
apex_status_t apex_host_register_class(apex_host_t *h,
                                       uint8_t traffic_type,
                                       apex_host_class_rx_cb_t cb,
                                       void *user);

/* Feed received UART bytes. `now_ms` should be a monotonic millisecond tick. */
void apex_host_feed_rx(apex_host_t *h,
                       const uint8_t *bytes,
                       size_t n,
                       uint32_t now_ms);

/* Drive timers: HOST_STATE broadcast, per-device watchdog, provisional query. */
void apex_host_tick(apex_host_t *h, uint32_t now_ms);

/* ---------------------------------------------------------------------------
 * Outbound API
 * ------------------------------------------------------------------------- */

/* Change the host's flight state. Broadcast in the next HOST_STATE frame. */
void apex_host_set_flight_state(apex_host_t *h, apex_flight_state_t state);

/* Set the host's advisory warnings bitfield (§3.2.5), orthogonal to flight_state
 * — an OR of APEX_HOST_WARNING_* bits, or 0 to clear. Broadcast in the next
 * HOST_STATE frame. */
void apex_host_set_warnings(apex_host_t *h, uint8_t warnings);

/* Force an immediate HOST_STATE broadcast. */
apex_status_t apex_host_send_host_state(apex_host_t *h);

/* Send a class-specific frame to one device. */
apex_status_t apex_host_send(apex_host_t *h,
                             uint8_t device_id,
                             uint8_t traffic_type,
                             const uint8_t *payload,
                             size_t payload_len);

/* Send a NAME_REQUEST to a CONNECTED device. */
apex_status_t apex_host_request_name(apex_host_t *h,
                                     uint8_t device_id,
                                     uint8_t bytes_allocated);

/* Send a PHYS_REQUEST to a CONNECTED device (§3.2.9 post-CONNECTED re-query). */
apex_status_t apex_host_request_phys(apex_host_t *h, uint8_t device_id);

/* Send a RESET_REQUEST (msg 13 — session re-enumeration) and apply the sender
 * obligations: free the targeted slot(s) (ids return to the pool; each freed
 * slot fires on_device_event with UNKNOWN), revert a raised link rate to the
 * default (baud_switch_cb fires with APEX_BAUD_CODE_115200), and expect the
 * device(s) to re-appear via DEVICE_INFO. No ack exists; if a device never
 * returns, the Pin 9 power-cycle fallback is app policy (§3.5).
 *
 * `device_id` may be an assigned id (that one device), the unassigned marker
 * APEX_DEVICE_ID_UNASSIGNED / 0x01 (un-wedge a stuck pre-CONNECTED device on
 * this link), or APEX_DEVICE_ID_BROADCAST / 0xFF (bus-wide re-enumeration).
 * Sending to an id the host holds no slot for is permitted (manual reactive
 * reset). Returns APEX_ERR_INVALID_ARGS on id 0x00. */
apex_status_t apex_host_request_reenumeration(apex_host_t *h, uint8_t device_id);

/* Evict a known device (the ONE post-latch eviction mechanism). Arms the
 * ACK_REJECT_POLICY deny latch for the device's declared class, then performs
 * apex_host_request_reenumeration(device_id): the device resets its session,
 * re-discovers, and its DEVICE_INFO is answered with a terminal
 * ACK_REJECT_POLICY — the device stops retrying, honestly evicted. The latch
 * lifetime matches the deny machinery (consumed by that next re-discovery).
 * Returns APEX_ERR_NOT_FOUND if the host holds no slot for `device_id`. */
apex_status_t apex_host_evict(apex_host_t *h, uint8_t device_id);

/* Mark a device as EXPENDED (§7.3 — called by an Activation host when a device
 * reports EXHAUSTED). */
void apex_host_mark_expended(apex_host_t *h, uint8_t device_id);

/* ---------------------------------------------------------------------------
 * Inspection
 * ------------------------------------------------------------------------- */

const apex_host_device_slot_t *apex_host_get_device(const apex_host_t *h,
                                                    uint8_t device_id);

/* Number of occupied slots (device_id != UNASSIGNED), including provisional
 * and FAULT slots not yet recycled. */
size_t apex_host_device_count(const apex_host_t *h);

#ifdef __cplusplus
}
#endif

#endif /* APEX_HOST_H */
