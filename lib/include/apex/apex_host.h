/**
 * @file apex_host.h
 * @brief Host-side core: device table, discovery, heartbeat, HOST_STATE
 *        broadcast, NAME_REQUEST/REPLY plumbing, and dispatch of class
 *        traffic to registered handlers.
 *
 * Lifecycle in one paragraph: the caller initializes an apex_host_t with a
 * TX callback and (optionally) event callbacks. Bytes received on the UART
 * are pushed in with apex_host_feed_rx(). The caller invokes apex_host_tick()
 * periodically — at least once per second, ideally 10 Hz or faster — to
 * drive HOST_STATE broadcasts and the per-device 5 s watchdog. Class-specific
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
 * slot + device_id to the pool (§3.3 slot recycling). "A few seconds" past the
 * 5 s heartbeat watchdog that produced the FAULT. */
#ifndef APEX_HOST_FAULT_RECYCLE_MS
#define APEX_HOST_FAULT_RECYCLE_MS 5000u
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

typedef struct {
    /* OR of APEX_INTERFACE_FLAG_* that this host can grant a device. */
    uint8_t supported_interfaces;

    /* HOST_STATE broadcast period. 1000 ms is the §3.2.5 floor; setting to 0
     * disables HOST_STATE broadcast. */
    uint32_t host_state_period_ms;

    /* Required: TX callback. */
    apex_host_tx_cb_t tx;
    void *tx_user;

    /* Optional event hooks. */
    apex_host_device_event_cb_t on_device_event;
    void *on_device_event_user;
    apex_host_name_reply_cb_t on_name_reply;
    void *on_name_reply_user;
} apex_host_cfg_t;

typedef struct {
    uint8_t device_id;          /* assigned ID; 0 = slot empty */
    apex_device_status_t status;
    uint8_t device_class;       /* the traffic_type the device declared */
    uint8_t interface_flags;
    uint32_t last_rx_ms;        /* last inbound frame attributable to this slot */
    uint32_t status_since_ms;   /* when the slot entered its current status */
} apex_host_device_slot_t;

typedef struct {
    uint8_t traffic_type;
    bool registered;
    apex_host_class_rx_cb_t rx;
    void *user;
} apex_host_class_reg_t;

typedef struct apex_host {
    apex_host_cfg_t cfg;
    apex_framer_rx_t framer;
    apex_host_device_slot_t devices[APEX_HOST_MAX_DEVICES];
    apex_host_class_reg_t classes[APEX_HOST_MAX_CLASSES];
    uint8_t next_assign_id;      /* monotonic counter starting at 0x01 */
    uint8_t flight_state;
    uint32_t now_ms;
    uint32_t last_host_state_tx_ms;
    bool host_state_ever_sent;
    /* Sticky discovery dedup: the device_id of the most recent provisional (NEW)
     * assignment made for a DEVICE_INFO(id=0). While that slot is still NEW the
     * host resends the same CONFIG_REPLY on each repeated DEVICE_INFO(id=0)
     * instead of allocating a fresh slot, so a slow-to-latch device never burns
     * more than one slot. Cleared when the slot is promoted or recycled.
     * 0 (UNASSIGNED) = no pending assignment. */
    uint8_t last_unassigned_id;
} apex_host_t;

/* ---------------------------------------------------------------------------
 * Lifecycle
 * ------------------------------------------------------------------------- */

void apex_host_init(apex_host_t *h, const apex_host_cfg_t *cfg);

/* Register a handler for class traffic with the given traffic_type.
 * Returns APEX_ERR_FULL if APEX_HOST_MAX_CLASSES is exceeded, or
 * APEX_ERR_INVALID_ARGS for traffic_type == 0 (CONFIG, reserved). */
apex_status_t apex_host_register_class(apex_host_t *h,
                                       uint8_t traffic_type,
                                       apex_host_class_rx_cb_t cb,
                                       void *user);

/* Feed received UART bytes. `now_ms` should be a monotonic millisecond tick. */
void apex_host_feed_rx(apex_host_t *h,
                       const uint8_t *bytes,
                       size_t n,
                       uint32_t now_ms);

/* Drive timers: HOST_STATE broadcast, per-device watchdog. */
void apex_host_tick(apex_host_t *h, uint32_t now_ms);

/* ---------------------------------------------------------------------------
 * Outbound API
 * ------------------------------------------------------------------------- */

/* Change the host's flight state. Broadcast in the next HOST_STATE frame. */
void apex_host_set_flight_state(apex_host_t *h, apex_flight_state_t state);

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

/* Mark a device as EXPENDED (§7.3 — called by an Activation host when a device
 * reports EXHAUSTED). */
void apex_host_mark_expended(apex_host_t *h, uint8_t device_id);

/* ---------------------------------------------------------------------------
 * Inspection
 * ------------------------------------------------------------------------- */

const apex_host_device_slot_t *apex_host_get_device(const apex_host_t *h,
                                                    uint8_t device_id);

/* Number of occupied slots (device_id != UNASSIGNED), including provisional
 * (NEW) and FAULT slots not yet recycled. */
size_t apex_host_device_count(const apex_host_t *h);

#ifdef __cplusplus
}
#endif

#endif /* APEX_HOST_H */
