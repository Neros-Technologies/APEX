/**
 * @file apex_host.c
 * @brief Host-side core: device table, discovery, heartbeat, HOST_STATE
 *        broadcast.
 *
 * Copyright (c) 2026 Neros Technologies. MIT License — see LICENSE.
 */
#include <string.h>

#include "apex/apex_host.h"

/* ---------------------------------------------------------------------------
 * Helpers
 * ------------------------------------------------------------------------- */

static apex_host_device_slot_t *find_slot(apex_host_t *h, uint8_t device_id)
{
    if (device_id == APEX_DEVICE_ID_UNASSIGNED ||
        device_id == APEX_DEVICE_ID_BROADCAST) return NULL;
    for (size_t i = 0; i < APEX_HOST_MAX_DEVICES; i++) {
        if (h->devices[i].device_id == device_id) return &h->devices[i];
    }
    return NULL;
}

static apex_host_device_slot_t *find_free_slot(apex_host_t *h)
{
    for (size_t i = 0; i < APEX_HOST_MAX_DEVICES; i++) {
        if (h->devices[i].device_id == APEX_DEVICE_ID_UNASSIGNED) {
            return &h->devices[i];
        }
    }
    return NULL;
}

static const apex_host_class_reg_t *find_class(const apex_host_t *h,
                                               uint8_t traffic_type)
{
    for (size_t i = 0; i < APEX_HOST_MAX_CLASSES; i++) {
        if (h->classes[i].registered &&
            h->classes[i].traffic_type == traffic_type) {
            return &h->classes[i];
        }
    }
    return NULL;
}

static void set_status(apex_host_t *h,
                       apex_host_device_slot_t *slot,
                       apex_device_status_t s)
{
    if (slot->status == s) return;
    slot->status = s;
    slot->status_since_ms = h->now_ms;
    if (h->cfg.on_device_event) {
        h->cfg.on_device_event(h->cfg.on_device_event_user, slot->device_id, s);
    }
}

/* Return a slot to the pool: clear its assigned ID and any sticky-dedup
 * reference so find_free_slot can reuse it. No event is fired — recycling is
 * internal table management; the FAULT (or, for a provisional slot, none) event
 * already conveyed the device's fate. */
static void free_slot(apex_host_t *h, apex_host_device_slot_t *slot)
{
    if (h->last_unassigned_id == slot->device_id) {
        h->last_unassigned_id = APEX_DEVICE_ID_UNASSIGNED;
    }
    slot->device_id       = APEX_DEVICE_ID_UNASSIGNED;
    slot->status          = APEX_DEV_STATUS_UNKNOWN;
    slot->device_class    = 0;
    slot->interface_flags = 0;
    slot->last_rx_ms      = 0;
    slot->status_since_ms = 0;
}

/* Promote a provisional (NEW) slot to CONNECTED on the first frame that proves
 * the device latched its assigned ID (explicit CONFIG_ACK, or any frame bearing
 * the assigned ID as a fallback — §3.3). No-op if the slot is not NEW. */
static void confirm_latch(apex_host_t *h, apex_host_device_slot_t *slot)
{
    if (!slot || slot->status != APEX_DEV_STATUS_NEW) return;
    set_status(h, slot, APEX_DEV_STATUS_CONNECTED);
    if (h->last_unassigned_id == slot->device_id) {
        h->last_unassigned_id = APEX_DEVICE_ID_UNASSIGNED;
    }
}

static uint8_t allocate_device_id(apex_host_t *h)
{
    /* Monotonic counter starting at 0x01, skipping 0xFF and any already-used
     * IDs (§3.1.3 "Assignment policy"). */
    for (int attempt = 0; attempt < 256; attempt++) {
        uint8_t candidate = h->next_assign_id;
        h->next_assign_id = (uint8_t)(h->next_assign_id + 1);
        if (h->next_assign_id == 0 || h->next_assign_id == 0xFF) {
            h->next_assign_id = 0x01;
        }
        if (candidate == 0 || candidate == 0xFF) continue;
        if (find_slot(h, candidate) == NULL) return candidate;
    }
    return APEX_DEVICE_ID_UNASSIGNED;
}

static apex_status_t emit_frame(apex_host_t *h,
                                uint8_t traffic_type,
                                uint8_t device_id,
                                const uint8_t *payload,
                                uint8_t payload_len)
{
    uint8_t encoded[APEX_V0_MAX_ENCODED_FRAME_LENGTH];
    size_t encoded_len;
    apex_v0_hdr_t hdr = {
        .protocol_version = APEX_V0_PROTOCOL_VERSION,
        .traffic_type     = traffic_type,
        .device_id        = device_id,
        .payload_length   = payload_len,
    };
    apex_status_t s = apex_frame_encode(&hdr, payload, encoded, sizeof(encoded),
                                        &encoded_len);
    if (s != APEX_OK) return s;
    if (h->cfg.tx) h->cfg.tx(h->cfg.tx_user, encoded, encoded_len);
    return APEX_OK;
}

/* ---------------------------------------------------------------------------
 * Public lifecycle
 * ------------------------------------------------------------------------- */

void apex_host_init(apex_host_t *h, const apex_host_cfg_t *cfg)
{
    if (!h) return;
    memset(h, 0, sizeof(*h));
    if (cfg) h->cfg = *cfg;
    apex_framer_rx_init(&h->framer);
    h->next_assign_id = 0x01;
    h->flight_state = APEX_FLIGHT_STATE_UNKNOWN;
}

apex_status_t apex_host_register_class(apex_host_t *h,
                                       uint8_t traffic_type,
                                       apex_host_class_rx_cb_t cb,
                                       void *user)
{
    if (!h || traffic_type == APEX_TRAFFIC_CONFIG) return APEX_ERR_INVALID_ARGS;
    for (size_t i = 0; i < APEX_HOST_MAX_CLASSES; i++) {
        if (!h->classes[i].registered) {
            h->classes[i].traffic_type = traffic_type;
            h->classes[i].rx = cb;
            h->classes[i].user = user;
            h->classes[i].registered = true;
            return APEX_OK;
        }
    }
    return APEX_ERR_FULL;
}

/* ---------------------------------------------------------------------------
 * CONFIG message handling
 * ------------------------------------------------------------------------- */

static void handle_device_info(apex_host_t *h,
                               uint8_t src_device_id,
                               const uint8_t *body,
                               size_t body_len)
{
    /* §3.2.1: payload is class_msg_id(1) + device_class_req(1) +
     * interface_flags_req(1) = 3 bytes. Body here is post-class_msg_id, so 2. */
    if (body_len < 2) return;

    uint8_t device_class_req   = body[0];
    uint8_t interface_flags_req = body[1];
    uint8_t reply[3];
    uint8_t reply_device_id = src_device_id;

    reply[0] = APEX_CFG_MSG_CONFIG_REPLY;

    /* §3.2.2: validate class then interfaces. */
    if (device_class_req == APEX_TRAFFIC_CONFIG ||
        find_class(h, device_class_req) == NULL) {
        reply[1] = APEX_ACK_REJECT_CLASS;
        reply[2] = 0x00;
        emit_frame(h, APEX_TRAFFIC_CONFIG, reply_device_id, reply, sizeof(reply));
        return;
    }

    if ((interface_flags_req & (uint8_t)~h->cfg.supported_interfaces) != 0) {
        reply[1] = APEX_ACK_REJECT_INTERFACE;
        reply[2] = 0x00;
        emit_frame(h, APEX_TRAFFIC_CONFIG, reply_device_id, reply, sizeof(reply));
        return;
    }

    /* Accept. Allocate or reuse a slot for this device. */
    apex_host_device_slot_t *slot = NULL;
    if (src_device_id == APEX_DEVICE_ID_UNASSIGNED) {
        /* Sticky dedup: a device whose CONFIG_REPLY was lost keeps sending
         * DEVICE_INFO(id=0). While the slot we already assigned is still NEW
         * (provisional — the device has not yet confirmed its ID), resend the
         * same CONFIG_REPLY and refresh that slot's liveness instead of burning
         * a new slot. A device thus occupies at most one slot until it latches. */
        if (h->last_unassigned_id != APEX_DEVICE_ID_UNASSIGNED) {
            apex_host_device_slot_t *prev = find_slot(h, h->last_unassigned_id);
            if (prev != NULL && prev->status == APEX_DEV_STATUS_NEW) {
                prev->last_rx_ms = h->now_ms;  /* still actively trying */
                reply[1] = APEX_ACK_OK;
                reply[2] = h->last_unassigned_id;
                emit_frame(h, APEX_TRAFFIC_CONFIG, APEX_DEVICE_ID_UNASSIGNED,
                           reply, sizeof(reply));
                return;
            }
            /* Pending slot vanished or already latched — drop the stale ref. */
            h->last_unassigned_id = APEX_DEVICE_ID_UNASSIGNED;
        }

        slot = find_free_slot(h);
        if (!slot) {
            reply[1] = APEX_ACK_REJECT_CLASS;  /* no slot — treat as unsupported */
            reply[2] = 0x00;
            emit_frame(h, APEX_TRAFFIC_CONFIG, reply_device_id, reply, sizeof(reply));
            return;
        }
        uint8_t new_id = allocate_device_id(h);
        if (new_id == APEX_DEVICE_ID_UNASSIGNED) {
            reply[1] = APEX_ACK_REJECT_CLASS;
            reply[2] = 0x00;
            emit_frame(h, APEX_TRAFFIC_CONFIG, reply_device_id, reply, sizeof(reply));
            return;
        }
        /* Provisional: reserve the slot as NEW but do NOT mark CONNECTED. The
         * device confirms by sending CONFIG_ACK (or any frame bearing the
         * assigned ID); confirm_latch() then promotes it. No event fires for NEW. */
        slot->device_id       = new_id;
        slot->status          = APEX_DEV_STATUS_NEW;
        slot->device_class    = device_class_req;
        slot->interface_flags = interface_flags_req;
        slot->last_rx_ms      = h->now_ms;
        slot->status_since_ms = h->now_ms;
        h->last_unassigned_id = new_id;  /* arm sticky dedup */
        reply_device_id = APEX_DEVICE_ID_UNASSIGNED;  /* per §3.2.2 */
        reply[1] = APEX_ACK_OK;
        reply[2] = new_id;
        emit_frame(h, APEX_TRAFFIC_CONFIG, reply_device_id, reply, sizeof(reply));
        return;  /* stay NEW; promotion happens on confirmation */
    } else {
        /* DEVICE_INFO from a device with an already-assigned ID — this is the
         * multi-class discovery path (§3.7.4). Same physical device, new
         * logical class. Allocate another slot but set interface_flags = 0
         * (per §3.7.4: subsequent class discovery must have flags = 0). */
        if (interface_flags_req != 0) {
            reply[1] = APEX_ACK_REJECT_INTERFACE;
            reply[2] = 0x00;
            emit_frame(h, APEX_TRAFFIC_CONFIG, src_device_id, reply, sizeof(reply));
            return;
        }
        slot = find_free_slot(h);
        if (!slot) {
            reply[1] = APEX_ACK_REJECT_CLASS;
            reply[2] = 0x00;
            emit_frame(h, APEX_TRAFFIC_CONFIG, src_device_id, reply, sizeof(reply));
            return;
        }
        uint8_t new_id = allocate_device_id(h);
        slot->device_id       = new_id;
        slot->status          = APEX_DEV_STATUS_NEW;
        slot->device_class    = device_class_req;
        slot->interface_flags = 0;
        slot->last_rx_ms      = h->now_ms;
        reply_device_id = src_device_id;  /* host echoes for already-assigned */
        reply[1] = APEX_ACK_OK;
        reply[2] = new_id;
    }

    /* Send CONFIG_REPLY first, then transition to CONNECTED so any
     * downstream class handler subscribing to on_device_event sees the
     * post-reply state. */
    emit_frame(h, APEX_TRAFFIC_CONFIG, reply_device_id, reply, sizeof(reply));
    set_status(h, slot, APEX_DEV_STATUS_CONNECTED);
}

static void handle_config_ack(apex_host_t *h,
                              uint8_t src_device_id,
                              const uint8_t *body,
                              size_t body_len)
{
    /* §3.2.6: CONFIG_ACK confirms the device latched its assigned ID. The body
     * echoes that ID; it must match the frame's outer device_id and a slot we
     * assigned. Promotion (NEW → CONNECTED) is otherwise identical to the
     * fallback path in on_frame — confirm_latch() is idempotent. */
    if (body_len < 1) return;
    uint8_t echoed_id = body[0];
    if (echoed_id != src_device_id) return;  /* malformed / mismatched — drop */
    confirm_latch(h, find_slot(h, src_device_id));
}

static void handle_name_reply(apex_host_t *h,
                              uint8_t src_device_id,
                              const uint8_t *body,
                              size_t body_len)
{
    if (!h->cfg.on_name_reply) return;
    h->cfg.on_name_reply(h->cfg.on_name_reply_user,
                         src_device_id,
                         (const char *)body,
                         body_len);
}

static void handle_config_frame(apex_host_t *h,
                                uint8_t src_device_id,
                                const uint8_t *payload,
                                size_t payload_len)
{
    if (payload_len == 0) {
        /* Implicit heartbeat (§3.5) — nothing further to do; last_rx_ms was
         * already stamped in the caller. */
        return;
    }

    uint8_t msg_id = payload[0];
    const uint8_t *body = payload + 1;
    size_t body_len = payload_len - 1;

    switch (msg_id) {
    case APEX_CFG_MSG_DEVICE_INFO:
        handle_device_info(h, src_device_id, body, body_len);
        break;
    case APEX_CFG_MSG_CONFIG_ACK:
        handle_config_ack(h, src_device_id, body, body_len);
        break;
    case APEX_CFG_MSG_NAME_REPLY:
        handle_name_reply(h, src_device_id, body, body_len);
        break;
    default:
        /* CONFIG_REPLY, NAME_REQUEST, HOST_STATE are host→device; the host
         * does not act on them as a receiver. Drop silently per §3.8. */
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
    apex_host_t *h = (apex_host_t *)user;

    /* §3.8: drop frames from device_id = 0xFF (only valid as a broadcast
     * destination from the host) and from unknown IDs (post-discovery). */
    if (hdr->device_id == APEX_DEVICE_ID_BROADCAST) return;

    /* Stamp last_rx for any known device, and treat any frame bearing an
     * assigned ID as confirmation that the device latched it: promote a
     * provisional (NEW) slot to CONNECTED. This is the fallback that covers a
     * lost CONFIG_ACK or a device that does not send one — the explicit
     * CONFIG_ACK (handled below) just does this sooner (§3.3). */
    apex_host_device_slot_t *slot = find_slot(h, hdr->device_id);
    if (slot) {
        slot->last_rx_ms = h->now_ms;
        confirm_latch(h, slot);
    }

    if (hdr->traffic_type == APEX_TRAFFIC_CONFIG) {
        handle_config_frame(h, hdr->device_id, payload, payload_len);
        return;
    }

    /* Class traffic. Only deliver to a registered handler when the device is
     * known and connected. */
    if (!slot || slot->status != APEX_DEV_STATUS_CONNECTED) return;
    const apex_host_class_reg_t *reg = find_class(h, hdr->traffic_type);
    if (reg && reg->rx) {
        reg->rx(reg->user, hdr->device_id, payload, payload_len);
    }
}

void apex_host_feed_rx(apex_host_t *h,
                       const uint8_t *bytes,
                       size_t n,
                       uint32_t now_ms)
{
    if (!h) return;
    h->now_ms = now_ms;
    apex_framer_feed(&h->framer, bytes, n, on_frame, h);
}

/* ---------------------------------------------------------------------------
 * Tick — heartbeats and watchdogs
 * ------------------------------------------------------------------------- */

void apex_host_tick(apex_host_t *h, uint32_t now_ms)
{
    if (!h) return;
    h->now_ms = now_ms;

    /* HOST_STATE broadcast. */
    if (h->cfg.host_state_period_ms > 0) {
        bool due = !h->host_state_ever_sent ||
                   (uint32_t)(now_ms - h->last_host_state_tx_ms)
                       >= h->cfg.host_state_period_ms;
        if (due) {
            apex_host_send_host_state(h);
        }
    }

    /* Per-device watchdog + slot recycling. */
    for (size_t i = 0; i < APEX_HOST_MAX_DEVICES; i++) {
        apex_host_device_slot_t *slot = &h->devices[i];
        if (slot->device_id == APEX_DEVICE_ID_UNASSIGNED) continue;

        switch (slot->status) {
        case APEX_DEV_STATUS_NEW:
            /* Provisional slot. The device was assigned an ID but has not yet
             * confirmed it. An actively-retrying device keeps last_rx_ms fresh
             * via the dedup path; one that vanishes mid-handshake goes silent
             * and is recycled so its slot returns to the pool. */
            if ((uint32_t)(now_ms - slot->last_rx_ms) >= APEX_HEARTBEAT_WATCHDOG_MS) {
                free_slot(h, slot);
            }
            break;
        case APEX_DEV_STATUS_CONNECTED:
            if ((uint32_t)(now_ms - slot->last_rx_ms) >= APEX_HEARTBEAT_WATCHDOG_MS) {
                set_status(h, slot, APEX_DEV_STATUS_FAULT);
            }
            break;
        case APEX_DEV_STATUS_FAULT:
            /* Recycle a faulted slot a few seconds after it faulted so the slot
             * and its device_id return to the pool (a recovered device
             * re-discovers into a fresh slot). status_since_ms — not last_rx_ms —
             * so a device still emitting on an asymmetric link still recycles. */
            if ((uint32_t)(now_ms - slot->status_since_ms) >= APEX_HOST_FAULT_RECYCLE_MS) {
                free_slot(h, slot);
            }
            break;
        default:
            /* EXPENDED / UNKNOWN: no watchdog (§4 — heartbeat tracking off). */
            break;
        }
    }
}

/* ---------------------------------------------------------------------------
 * Outbound API
 * ------------------------------------------------------------------------- */

void apex_host_set_flight_state(apex_host_t *h, apex_flight_state_t state)
{
    if (!h) return;
    h->flight_state = (uint8_t)state;
}

apex_status_t apex_host_send_host_state(apex_host_t *h)
{
    if (!h) return APEX_ERR_INVALID_ARGS;
    uint8_t payload[2] = { APEX_CFG_MSG_HOST_STATE, h->flight_state };
    apex_status_t s = emit_frame(h, APEX_TRAFFIC_CONFIG,
                                 APEX_DEVICE_ID_BROADCAST,
                                 payload, sizeof(payload));
    if (s == APEX_OK) {
        h->last_host_state_tx_ms = h->now_ms;
        h->host_state_ever_sent = true;
    }
    return s;
}

apex_status_t apex_host_send(apex_host_t *h,
                             uint8_t device_id,
                             uint8_t traffic_type,
                             const uint8_t *payload,
                             size_t payload_len)
{
    if (!h || traffic_type == APEX_TRAFFIC_CONFIG) return APEX_ERR_INVALID_ARGS;
    if (payload_len > APEX_V0_MAX_PAYLOAD_LENGTH) return APEX_ERR_INVALID_ARGS;
    apex_host_device_slot_t *slot = find_slot(h, device_id);
    if (!slot || slot->status != APEX_DEV_STATUS_CONNECTED) return APEX_ERR_NOT_FOUND;
    return emit_frame(h, traffic_type, device_id, payload, (uint8_t)payload_len);
}

apex_status_t apex_host_request_name(apex_host_t *h,
                                     uint8_t device_id,
                                     uint8_t bytes_allocated)
{
    if (!h) return APEX_ERR_INVALID_ARGS;
    apex_host_device_slot_t *slot = find_slot(h, device_id);
    if (!slot || slot->status != APEX_DEV_STATUS_CONNECTED) return APEX_ERR_NOT_FOUND;
    uint8_t payload[2] = { APEX_CFG_MSG_NAME_REQUEST, bytes_allocated };
    return emit_frame(h, APEX_TRAFFIC_CONFIG, device_id, payload, sizeof(payload));
}

void apex_host_mark_expended(apex_host_t *h, uint8_t device_id)
{
    if (!h) return;
    apex_host_device_slot_t *slot = find_slot(h, device_id);
    if (!slot) return;
    set_status(h, slot, APEX_DEV_STATUS_EXPENDED);
}

const apex_host_device_slot_t *apex_host_get_device(const apex_host_t *h,
                                                    uint8_t device_id)
{
    if (!h) return NULL;
    for (size_t i = 0; i < APEX_HOST_MAX_DEVICES; i++) {
        if (h->devices[i].device_id == device_id) return &h->devices[i];
    }
    return NULL;
}

size_t apex_host_device_count(const apex_host_t *h)
{
    if (!h) return 0;
    size_t n = 0;
    for (size_t i = 0; i < APEX_HOST_MAX_DEVICES; i++) {
        if (h->devices[i].device_id != APEX_DEVICE_ID_UNASSIGNED) n++;
    }
    return n;
}
