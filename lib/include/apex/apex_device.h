/**
 * @file apex_device.h
 * @brief Device-side core (APEX wire v1): discovery client, the provisional
 *        configuration phase, heartbeat/watchdog, post-CONNECTED baud
 *        negotiation, VERSION_BEACON consumption, NAME_REPLY plumbing, and
 *        dispatch of class traffic to the device-side class handler.
 *
 * Lifecycle in one paragraph: the caller initializes an apex_device_t with a
 * declared class, interface flags, a class-version range, mass, and a TX
 * callback. apex_device_tick() drives discovery: the device beacons its
 * wire-version range periodically from power-up and, once the host's
 * beacon closes the version tier, retransmits DEVICE_INFO at the mutual
 * version until a CONFIG_REPLY arrives; once the phase has begun it drives the
 * 1 Hz implicit-heartbeat floor and the 5 s watchdog. apex_device_feed_rx()
 * pushes received UART bytes through the framer. On a terminal ACK_OK (in
 * CONFIG_REPLY or in any provisional-phase ack) the device latches its assigned
 * device_id, sends CONFIG_ACK, and transitions to CONNECTED; class traffic on
 * the declared traffic_type is then delivered to the on_class_rx callback.
 *
 * Copyright (c) 2026 Neros Technologies. MIT License — see LICENSE.
 */
#ifndef APEX_DEVICE_H
#define APEX_DEVICE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "apex/apex_core.h"
#include "apex/apex_framer.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*apex_device_tx_cb_t)(void *user, const uint8_t *bytes, size_t n);

/* Device-side link state (§3.3, §4). DISCOVERING and CONNECTED keep their v0.9
 * numeric identities because device-class layers (Activation, Analog HMI,
 * Repeater) branch on them. */
typedef enum {
    APEX_DEVICE_STATE_DISCOVERING = 0, /* Beaconing periodically; after
                                        * the host's beacon is received,
                                        * additionally retransmitting
                                        * DEVICE_INFO at the mutual version. */
    APEX_DEVICE_STATE_PROVISIONAL,     /* Got ACK_PROVISIONAL; answering the
                                        * host-driven query loop at id 0x01. */
    APEX_DEVICE_STATE_CONNECTED,       /* Latched assigned id; class traffic. */
    APEX_DEVICE_STATE_REJECTED,        /* Terminal reject — device stopped
                                        * retrying (dead). See reject reason.
                                        * Only RESET_REQUEST revives it. */
    APEX_DEVICE_STATE_INCOMPATIBLE,    /* The host's beacon named a wire-version
                                        * range disjoint from v1; beaconing
                                        * continues at <= 0.1 Hz (§3.6.2); a
                                        * compatible beacon re-opens. */
} apex_device_link_state_t;

typedef void (*apex_device_class_rx_cb_t)(void *user,
                                          const uint8_t *payload,
                                          size_t payload_len);

typedef void (*apex_device_link_event_cb_t)(void *user,
                                            apex_device_link_state_t state);

typedef void (*apex_device_host_state_cb_t)(void *user,
                                            apex_flight_state_t flight_state);

typedef void (*apex_device_name_request_cb_t)(void *user,
                                              uint8_t bytes_allocated);

/* Baud/link-rate channel (§3.4). On granted=true the link now runs at `code`:
 * the app owns the physical UART and MUST reconfigure it to `code` on this
 * callback. On granted=false the ladder was exhausted (the device reached its
 * floor without a grant) and the rate is unchanged; the app decides whether
 * that is a FAULT (§3.2.7).
 *
 * Fired at most once per apex_device_request_baud() call, and ALSO fired with
 * granted=true, code=APEX_BAUD_CODE_115200 whenever a raised session rate
 * reverts to the default — on RESET_REQUEST re-enumeration (msg 13) or a
 * watchdog-driven return to discovery (§3.4 recovery). This makes the callback
 * the device side's single "reconfigure your UART now" channel; it never fires
 * when the rate is already the default. */
typedef void (*apex_device_baud_result_cb_t)(void *user,
                                             bool granted,
                                             apex_baud_code_t code);

/* Delivery outcome of an in-flight physical update. Fired exactly once
 * per accepted apex_device_send_phys_update() call: delivered=true when any
 * PHYS_ACK for the update arrived (an advisory ACK_REJECT_PHYS is still a
 * receipt), false when the 3 x 500 ms retransmit window exhausted unacked. */
typedef void (*apex_device_phys_update_result_cb_t)(void *user, bool delivered);

/* Advisory envelope signal: a post-CONNECTED PHYS_ACK(ACK_REJECT_PHYS)
 * arrived — the host judges the declared physical state outside its flight
 * envelope. Advisory ONLY: no session effect, the link stays CONNECTED and
 * class traffic continues. What to do with the signal is payload-defined
 * (e.g. an articulating payload might retract). */
typedef void (*apex_device_phys_advisory_cb_t)(void *user);

typedef struct {
    /* What this device wants to be — see APEX_Device_Classes.md. */
    uint8_t device_class;       /* traffic_type to request */
    uint8_t interface_flags;    /* OR of APEX_INTERFACE_FLAG_* requested */

    /* Class-version range advertised in DEVICE_INFO (§3.2.1). Support MUST be a
     * contiguous range min..max (§3.6). If left 0/0, the device declares
     * class version 1..1 (the sole v1 default). */
    uint8_t class_version_min;
    uint8_t class_version_max;

    /* Payload mass in grams (§3.2.1). A device MUST NOT report 0 to mean
     * "unknown". Multi-class first-class-instance rule (§3.7.4) is the caller's
     * concern: a physical unit reports real mass only in its first class's
     * DEVICE_INFO and 0 ("no additional mass") for subsequent class instances —
     * the caller sets this field accordingly per apex_device_t it stands up. */
    uint16_t mass_grams;

    /* Optional physical declaration (§3.2.10). When has_phys is true the
     * device answers PHYS_REQUEST with a PHYS_INFO built from its CURRENT
     * physical state (seeded from `phys` at init, thereafter updated by
     * apex_device_send_phys_update()); when false it silently ignores the
     * whole PHYS message family (a KNOWN msg_id it simply does not answer —
     * non-support is detected by the host's timeout, §3.2.9).
     *
     * `phys.mass_grams` is IGNORED at init: the config-time mass is
     * `mass_grams` above (PHYS_INFO's mass duplicates DEVICE_INFO's at config
     * time). Like mass, `phys` describes the whole physical unit
     * (§3.7.4); in-flight updates likewise go out on the first class
     * instance's session only — the caller's concern. */
    bool has_phys;
    apex_phys_t phys;

    /* Required: TX callback. */
    apex_device_tx_cb_t tx;
    void *tx_user;

    /* Required: class-traffic RX callback. */
    apex_device_class_rx_cb_t on_class_rx;
    void *on_class_rx_user;

    /* Optional: link state transitions. */
    apex_device_link_event_cb_t on_link_event;
    void *on_link_event_user;

    /* Optional: HOST_STATE delivery. */
    apex_device_host_state_cb_t on_host_state;
    void *on_host_state_user;

    /* Optional: NAME_REQUEST. The device's response, if any, is sent via
     * apex_device_send_name_reply(). */
    apex_device_name_request_cb_t on_name_request;
    void *on_name_request_user;

    /* Optional: baud-negotiation result (§3.4). */
    apex_device_baud_result_cb_t on_baud_result;
    void *on_baud_result_user;

    /* Optional: physical-update delivery outcome. */
    apex_device_phys_update_result_cb_t on_phys_update_result;
    void *on_phys_update_result_user;

    /* Optional: advisory envelope signal. */
    apex_device_phys_advisory_cb_t on_phys_advisory;
    void *on_phys_advisory_user;

    /* Optional: safety-deferral hook for re-enumeration (RESET_REQUEST, msg 13).
     * Consulted when a host-sent RESET_REQUEST arrives, and re-checked on every
     * tick while a reset is pending. NULL = always permitted. While it returns
     * false the device DEFERS the reset: it latches it and continues normal
     * operation (heartbeats, class traffic) until the hook first returns true,
     * then honors it. Deferral must be bounded — honor at the next safe state
     * (class layers wire this; e.g. Activation defers only while EXECUTING). */
    bool (*reenum_permitted)(void *user);
    void *reenum_permitted_user;

    /* Discovery retransmit period (defaults to APEX_DISCOVERY_RETRY_MS if 0). */
    uint32_t discovery_retry_ms;

    /* VERSION_BEACON emission period while unlinked. Defaults to 1000 ms
     * (1 Hz) if 0; legal bounds match DEVICE_INFO retransmission: 10..1000 ms
     * (100 Hz..1 Hz). The device beacons from power-up and CONTINUES beaconing
     * through DISCOVERING even after the host's beacon is received (tolerating
     * lost beacons); it stops once the handshake progresses past discovery
     * (first CONFIG_REPLY -> PROVISIONAL/CONNECTED) and restarts whenever it
     * returns to discovery. While INCOMPATIBLE the cadence drops to 0.1 Hz. */
    uint32_t beacon_period_ms;
} apex_device_cfg_t;

typedef struct apex_device {
    apex_device_cfg_t cfg;
    apex_framer_rx_t framer;
    apex_device_link_state_t link;
    uint8_t assigned_device_id;       /* 0x01 (UNASSIGNED) until latched. */
    uint8_t selected_class_version;   /* Session class version (§3.6.1). */
    uint8_t reject_ack;               /* ack code that drove REJECTED (0 = n/a). */
    uint8_t host_class_min;           /* Host's class range, from CONFIG_REPLY. */
    uint8_t host_class_max;           /* Diagnostic on ACK_REJECT_CLASS_VERSION. */
    bool phase_started;               /* Set at first CONFIG_REPLY (§3.5). */
    bool reset_pending;               /* RESET_REQUEST deferred by the
                                       * reenum_permitted hook; re-checked
                                       * each tick. */

    /* Baud negotiation (§3.4). */
    bool     baud_pending;            /* Awaiting a BAUD_CHANGE_ACK. */
    uint8_t  baud_current;            /* Current operating code (0 = 115200). */
    uint8_t  baud_request;            /* Code currently proposed. */
    uint8_t  baud_min;                /* Lowest code the app will accept. */

    /* Current physical state — mass + inertia. Seeded from cfg at init
     * (phys fields from cfg.phys, mass from cfg.mass_grams); updated by
     * apex_device_send_phys_update(). NOT session state: it survives
     * re-enumeration, so DEVICE_INFO and PHYS_INFO always declare current
     * values. */
    apex_phys_t phys;

    /* In-flight physical update: retransmit-until-acked, 3 x 500 ms. */
    bool     phys_update_pending;     /* Awaiting a PHYS_ACK receipt. */
    bool     phys_update_ever;        /* Rate-cap anchor validity. */
    uint8_t  phys_update_attempts;    /* Sends so far for the pending update. */
    uint32_t phys_update_next_ms;     /* Next retransmit / give-up due time. */
    uint32_t phys_update_last_req_ms; /* Last accepted call (<= 2 Hz cap). */

    /* Version-tier negotiation (proactive mutual beaconing). */
    bool     peer_beacon_seen;        /* Compatible host beacon held. Gates
                                       * DEVICE_INFO emission AND —
                                       * ALL input: until the tier closes, the
                                       * device consumes nothing but beacons,
                                       * broadcasts included. Version agreement
                                       * is per device pair: a hot-swapped
                                       * device was never party to its
                                       * predecessor's negotiated version, so
                                       * in-flight session traffic is void for
                                       * it. Kept across a session reset (a
                                       * re-enumerating device WAS party to the
                                       * current pair agreement); cleared at
                                       * init and on a disjoint beacon. */
    uint16_t peer_min_version;        /* Host's advertised wire-version range. */
    uint16_t peer_max_version;
    uint16_t mutual_version;          /* min(own_max, peer_max); 0 = unknown. */
    bool     beacon_sent_once;
    uint32_t last_beacon_tx_ms;

    uint32_t now_ms;
    uint32_t last_discovery_tx_ms;
    uint32_t last_tx_ms;              /* any frame TX, for 1 Hz heartbeat */
    uint32_t last_rx_ms;              /* host-liveliness feed for the 5 s
                                       * watchdog. Pre-CONNECTED only addressed
                                       * frames (id 0x01 / assigned) refresh it
                                       * — broadcasts do not (ruling);
                                       * post-CONNECTED any frame counts. */
    bool ever_sent;
} apex_device_t;

/* ---------------------------------------------------------------------------
 * Lifecycle
 * ------------------------------------------------------------------------- */

void apex_device_init(apex_device_t *d, const apex_device_cfg_t *cfg);

void apex_device_feed_rx(apex_device_t *d,
                         const uint8_t *bytes,
                         size_t n,
                         uint32_t now_ms);

void apex_device_tick(apex_device_t *d, uint32_t now_ms);

/* ---------------------------------------------------------------------------
 * Outbound
 * ------------------------------------------------------------------------- */

/* Send class-specific bytes to the host. Must be CONNECTED. */
apex_status_t apex_device_send(apex_device_t *d,
                               const uint8_t *payload,
                               size_t payload_len);

/* Reply to a NAME_REQUEST with `name` (ASCII, length <= bytes_allocated). */
apex_status_t apex_device_send_name_reply(apex_device_t *d,
                                          const char *name,
                                          size_t name_len);

/* Request a post-CONNECTED baud-rate change (§3.4). Legal only while CONNECTED.
 * `preferred` is the first code proposed; `min` is the lowest code the app will
 * accept. On ACK_REJECT_BAUD the device ladders down toward `min` (jumping to
 * the host's counter-offer hint when usable) and reports the outcome via
 * cfg.on_baud_result. Recommending it first-thing-after-CONNECTED is the app's
 * call. Returns APEX_ERR_BAD_STATE if not CONNECTED or a negotiation is already
 * in flight, or APEX_ERR_INVALID_ARGS on a bad code. */
apex_status_t apex_device_request_baud(apex_device_t *d,
                                       apex_baud_code_t preferred,
                                       apex_baud_code_t min);

/* Voluntarily re-enumerate (RESET_REQUEST, msg 13 — e.g. before a hot firmware
 * update): transmit the announcement under the current id, then discard all
 * session state (assigned id → 0x01, selected class version, any raised baud —
 * cfg.on_baud_result fires with the default code if the rate was raised) and
 * resume DEVICE_INFO retransmission. Valid while PROVISIONAL or CONNECTED. The
 * reenum_permitted deferral hook is NOT consulted — the app initiated this.
 * Returns APEX_ERR_BAD_STATE otherwise. */
apex_status_t apex_device_request_reenumeration(apex_device_t *d);

/* In-flight physical update: push the device's new quasi-static physical
 * state (complete: mass + inertia) as an unsolicited PHYS_INFO. CONNECTED
 * only; requires cfg.has_phys. Updates the stored current state immediately —
 * a later re-enumeration re-declares these values (DEVICE_INFO carries the
 * current mass). The update is retransmitted until a PHYS_ACK receipt arrives
 * (3 x 500 ms); the outcome is reported once via cfg.on_phys_update_result.
 *
 * Rate cap (<= 2 Hz sustained): a call within 500 ms of the previous accepted
 * call — or while a previous update is still awaiting its receipt — is
 * REJECTED with APEX_ERR_BAD_STATE (nothing is queued; the caller coalesces
 * and retries, reporting the latest state). Returns APEX_ERR_BAD_STATE when
 * not CONNECTED, APEX_ERR_UNSUPPORTED without cfg.has_phys, and
 * APEX_ERR_INVALID_ARGS on NULL args. */
apex_status_t apex_device_send_phys_update(apex_device_t *d,
                                           const apex_phys_t *phys);

/* ---------------------------------------------------------------------------
 * Inspection
 * ------------------------------------------------------------------------- */

static inline apex_device_link_state_t apex_device_link_state(const apex_device_t *d)
{
    return d->link;
}

static inline uint8_t apex_device_get_id(const apex_device_t *d)
{
    return d->assigned_device_id;
}

/* Class version the session settled on (§3.6.1). Valid once PROVISIONAL or
 * CONNECTED (both imply version acceptance); 0 before the first CONFIG_REPLY. */
static inline uint8_t apex_device_selected_class_version(const apex_device_t *d)
{
    return d->selected_class_version;
}

/* The ack code that drove the device into APEX_DEVICE_STATE_REJECTED, drawn
 * from the global namespace (apex_ack_t). 0 if the device is not rejected. On
 * APEX_ACK_REJECT_CLASS_VERSION, host_class_min/host_class_max name the range
 * that would work (§3.2.2). */
static inline uint8_t apex_device_reject_reason(const apex_device_t *d)
{
    return d->reject_ack;
}

#ifdef __cplusplus
}
#endif

#endif /* APEX_DEVICE_H */
