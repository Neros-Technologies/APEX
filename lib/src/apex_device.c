/**
 * @file apex_device.c
 * @brief Device-side core: discovery client, heartbeat.
 *
 * Copyright (c) 2026 Neros Technologies. MIT License — see LICENSE.
 */
#include <string.h>

#include "apex/apex_device.h"

/* ---------------------------------------------------------------------------
 * Helpers
 * ------------------------------------------------------------------------- */

static apex_status_t emit_frame(apex_device_t *d,
                                uint8_t traffic_type,
                                const uint8_t *payload,
                                uint8_t payload_len)
{
    uint8_t encoded[APEX_V0_MAX_ENCODED_FRAME_LENGTH];
    size_t encoded_len;
    apex_v0_hdr_t hdr = {
        .protocol_version = APEX_V0_PROTOCOL_VERSION,
        .traffic_type     = traffic_type,
        .device_id        = d->assigned_device_id,
        .payload_length   = payload_len,
    };
    apex_status_t s = apex_frame_encode(&hdr, payload, encoded, sizeof(encoded),
                                        &encoded_len);
    if (s != APEX_OK) return s;
    if (d->cfg.tx) d->cfg.tx(d->cfg.tx_user, encoded, encoded_len);
    d->last_tx_ms = d->now_ms;
    d->ever_sent = true;
    return APEX_OK;
}

static void set_link(apex_device_t *d, apex_device_link_state_t s)
{
    if (d->link == s) return;
    d->link = s;
    if (d->cfg.on_link_event) {
        d->cfg.on_link_event(d->cfg.on_link_event_user, s);
    }
}

static void send_device_info(apex_device_t *d)
{
    uint8_t payload[3] = {
        APEX_CFG_MSG_DEVICE_INFO,
        d->cfg.device_class,
        d->cfg.interface_flags,
    };
    emit_frame(d, APEX_TRAFFIC_CONFIG, payload, sizeof(payload));
    d->last_discovery_tx_ms = d->now_ms;
}

static void send_heartbeat(apex_device_t *d)
{
    /* Empty CONFIG frame, payload_length = 0 (§3.5). */
    emit_frame(d, APEX_TRAFFIC_CONFIG, NULL, 0);
}

static void send_config_ack(apex_device_t *d)
{
    /* §3.2.6: confirm we latched the assigned ID. The outer device_id is now
     * our assigned ID (emit_frame uses d->assigned_device_id); the body echoes
     * it so the host can validate. */
    uint8_t payload[2] = { APEX_CFG_MSG_CONFIG_ACK, d->assigned_device_id };
    emit_frame(d, APEX_TRAFFIC_CONFIG, payload, sizeof(payload));
}

static void reset_to_discovery(apex_device_t *d)
{
    d->assigned_device_id = APEX_DEVICE_ID_UNASSIGNED;
    set_link(d, APEX_DEVICE_STATE_DISCOVERING);
}

/* ---------------------------------------------------------------------------
 * CONFIG handling
 * ------------------------------------------------------------------------- */

static void handle_config_reply(apex_device_t *d,
                                const apex_v0_hdr_t *hdr,
                                const uint8_t *body,
                                size_t body_len)
{
    if (body_len < 2) return;
    uint8_t ack = body[0];
    uint8_t assigned = body[1];

    /* §3.2.2: the host sets CONFIG_REPLY's outer device_id to match what the
     * device sent in DEVICE_INFO — which is 0 for an unassigned device on its
     * first try, or its assigned ID on a multi-class follow-up. */
    if (hdr->device_id != d->assigned_device_id) return;

    switch (ack) {
    case APEX_ACK_OK:
        d->assigned_device_id = assigned;
        set_link(d, APEX_DEVICE_STATE_CONNECTED);
        /* §3.3: confirm the latch so the host can promote our slot from
         * provisional (NEW) to CONNECTED immediately. */
        send_config_ack(d);
        break;
    case APEX_ACK_REJECT_CLASS:
        set_link(d, APEX_DEVICE_STATE_REJECTED_CLASS);
        break;
    case APEX_ACK_REJECT_INTERFACE:
        /* §3.3: device may retry with reduced flags. We leave it to the caller
         * to update cfg.interface_flags; for v1 we keep retrying with the
         * current flags. */
        break;
    case APEX_ACK_REJECT_VERSION:
        set_link(d, APEX_DEVICE_STATE_REJECTED_VERSION);
        break;
    default:
        /* Unknown ack: §3.2.2 says treat as REJECT_CLASS. */
        set_link(d, APEX_DEVICE_STATE_REJECTED_CLASS);
        break;
    }
}

static void handle_name_request(apex_device_t *d,
                                const uint8_t *body,
                                size_t body_len)
{
    if (body_len < 1) return;
    if (d->cfg.on_name_request) {
        d->cfg.on_name_request(d->cfg.on_name_request_user, body[0]);
    }
}

static void handle_host_state(apex_device_t *d,
                              const uint8_t *body,
                              size_t body_len)
{
    if (body_len < 1) return;
    if (d->cfg.on_host_state) {
        d->cfg.on_host_state(d->cfg.on_host_state_user,
                             (apex_flight_state_t)body[0]);
    }
}

static void handle_config_frame(apex_device_t *d,
                                const apex_v0_hdr_t *hdr,
                                const uint8_t *payload,
                                size_t payload_len)
{
    if (payload_len == 0) return;  /* heartbeat */

    uint8_t msg_id = payload[0];
    const uint8_t *body = payload + 1;
    size_t body_len = payload_len - 1;

    switch (msg_id) {
    case APEX_CFG_MSG_CONFIG_REPLY:
        handle_config_reply(d, hdr, body, body_len);
        break;
    case APEX_CFG_MSG_NAME_REQUEST:
        handle_name_request(d, body, body_len);
        break;
    case APEX_CFG_MSG_HOST_STATE:
        handle_host_state(d, body, body_len);
        break;
    default:
        /* DEVICE_INFO, NAME_REPLY are device→host; dropped on device receive. */
        break;
    }
}

/* ---------------------------------------------------------------------------
 * Frame dispatch
 * ------------------------------------------------------------------------- */

static void on_frame(void *user,
                     const apex_v0_hdr_t *hdr,
                     const uint8_t *payload,
                     size_t payload_len)
{
    apex_device_t *d = (apex_device_t *)user;

    /* §3.8 address filter: act only on frames whose device_id matches our
     * assigned ID, broadcast, or 0x00 while still pre-discovery. */
    bool addr_match =
        hdr->device_id == d->assigned_device_id ||
        hdr->device_id == APEX_DEVICE_ID_BROADCAST ||
        (d->assigned_device_id == APEX_DEVICE_ID_UNASSIGNED &&
         hdr->device_id == APEX_DEVICE_ID_UNASSIGNED);
    if (!addr_match) return;

    d->last_rx_ms = d->now_ms;

    if (hdr->traffic_type == APEX_TRAFFIC_CONFIG) {
        handle_config_frame(d, hdr, payload, payload_len);
        return;
    }

    if (d->link != APEX_DEVICE_STATE_CONNECTED) return;
    if (hdr->traffic_type != d->cfg.device_class) return;

    if (d->cfg.on_class_rx) {
        d->cfg.on_class_rx(d->cfg.on_class_rx_user, payload, payload_len);
    }
}

/* ---------------------------------------------------------------------------
 * Public lifecycle
 * ------------------------------------------------------------------------- */

void apex_device_init(apex_device_t *d, const apex_device_cfg_t *cfg)
{
    if (!d) return;
    memset(d, 0, sizeof(*d));
    if (cfg) d->cfg = *cfg;
    if (d->cfg.discovery_retry_ms == 0) {
        d->cfg.discovery_retry_ms = APEX_DISCOVERY_RETRY_MS;
    }
    apex_framer_rx_init(&d->framer);
    d->link = APEX_DEVICE_STATE_DISCOVERING;
}

void apex_device_feed_rx(apex_device_t *d,
                         const uint8_t *bytes,
                         size_t n,
                         uint32_t now_ms)
{
    if (!d) return;
    d->now_ms = now_ms;
    apex_framer_feed(&d->framer, bytes, n, on_frame, d);
}

void apex_device_tick(apex_device_t *d, uint32_t now_ms)
{
    if (!d) return;
    d->now_ms = now_ms;

    if (d->link == APEX_DEVICE_STATE_DISCOVERING) {
        bool first_try = !d->ever_sent;
        bool due = (uint32_t)(now_ms - d->last_discovery_tx_ms) >=
                   d->cfg.discovery_retry_ms;
        if (first_try || due) {
            send_device_info(d);
        }
        return;
    }

    if (d->link == APEX_DEVICE_STATE_CONNECTED) {
        /* 1 Hz transmit floor (§3.5). */
        if ((uint32_t)(now_ms - d->last_tx_ms) >= APEX_HEARTBEAT_TX_PERIOD_MS) {
            send_heartbeat(d);
        }
        /* 5 s watchdog (§3.5) — host has gone silent; restart discovery. */
        if ((uint32_t)(now_ms - d->last_rx_ms) >= APEX_HEARTBEAT_WATCHDOG_MS) {
            reset_to_discovery(d);
        }
    }
}

/* ---------------------------------------------------------------------------
 * Outbound
 * ------------------------------------------------------------------------- */

apex_status_t apex_device_send(apex_device_t *d,
                               const uint8_t *payload,
                               size_t payload_len)
{
    if (!d) return APEX_ERR_INVALID_ARGS;
    if (d->link != APEX_DEVICE_STATE_CONNECTED) return APEX_ERR_BAD_STATE;
    if (payload_len > APEX_V0_MAX_PAYLOAD_LENGTH) return APEX_ERR_INVALID_ARGS;
    return emit_frame(d, d->cfg.device_class, payload, (uint8_t)payload_len);
}

apex_status_t apex_device_send_name_reply(apex_device_t *d,
                                          const char *name,
                                          size_t name_len)
{
    if (!d) return APEX_ERR_INVALID_ARGS;
    if (name_len > APEX_V0_MAX_PAYLOAD_LENGTH - 1) return APEX_ERR_INVALID_ARGS;
    uint8_t payload[APEX_V0_MAX_PAYLOAD_LENGTH];
    payload[0] = APEX_CFG_MSG_NAME_REPLY;
    if (name_len > 0) memcpy(payload + 1, name, name_len);
    return emit_frame(d, APEX_TRAFFIC_CONFIG, payload, (uint8_t)(1 + name_len));
}
