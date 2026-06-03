/**
 * @file apex_device.h
 * @brief Device-side core: discovery client, heartbeat, NAME_REPLY plumbing,
 *        and dispatch of class traffic to the device-side handler for its
 *        declared class.
 *
 * Lifecycle in one paragraph: the caller initializes an apex_device_t with
 * a declared class + interface flags and a TX callback. apex_device_tick()
 * drives the discovery handshake (retransmitting DEVICE_INFO until CONFIG_REPLY
 * arrives) and the 1 Hz implicit-heartbeat floor. apex_device_feed_rx() pushes
 * received UART bytes through the framer. On CONFIG_REPLY ACK_OK, the device
 * transitions to CONNECTED and class traffic on the declared traffic_type is
 * delivered to a callback the caller can wire to the Activation-class
 * implementation (or any other class).
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

typedef enum {
    APEX_DEVICE_STATE_DISCOVERING = 0,  /* Sending DEVICE_INFO, waiting for CONFIG_REPLY */
    APEX_DEVICE_STATE_CONNECTED,
    APEX_DEVICE_STATE_REJECTED_CLASS,
    APEX_DEVICE_STATE_REJECTED_VERSION,
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

typedef struct {
    /* What this device wants to be — see APEX_Device_Classes.md. */
    uint8_t device_class;       /* traffic_type to request */
    uint8_t interface_flags;    /* OR of APEX_INTERFACE_FLAG_* requested */

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

    /* Discovery retransmit period (defaults to APEX_DISCOVERY_RETRY_MS if 0). */
    uint32_t discovery_retry_ms;
} apex_device_cfg_t;

typedef struct apex_device {
    apex_device_cfg_t cfg;
    apex_framer_rx_t framer;
    apex_device_link_state_t link;
    uint8_t assigned_device_id;       /* 0 = unassigned */
    uint32_t now_ms;
    uint32_t last_discovery_tx_ms;
    uint32_t last_tx_ms;              /* any frame TX, for 1 Hz heartbeat */
    uint32_t last_rx_ms;              /* any frame RX, for 5 s watchdog */
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

#ifdef __cplusplus
}
#endif

#endif /* APEX_DEVICE_H */
