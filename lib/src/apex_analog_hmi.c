/**
 * @file apex_analog_hmi.c
 * @brief Analog HMI device class — both Host and Device sides.
 *
 * Copyright (c) 2026 Neros Technologies. MIT License — see LICENSE.
 */
#include <string.h>

#include "apex/apex_analog_hmi.h"

/* ===========================================================================
 * Device-side
 * ========================================================================= */

static apex_status_t dev_send(apex_hmi_device_t *d,
                              const uint8_t *payload, size_t len)
{
    return apex_device_send(d->core, payload, len);
}

static void dev_send_capability(apex_hmi_device_t *d)
{
    uint8_t buf[5];
    buf[0] = APEX_HMI_MSG_CAPABILITY;
    buf[1] = d->caps.class_spec_version;
    buf[2] = d->caps.supported_control_formats;
    buf[3] = d->caps.supported_cvbs_modes;
    buf[4] = d->caps.intended_rate_hz;
    if (dev_send(d, buf, sizeof(buf)) == APEX_OK) {
        d->capability_sent = true;
        d->capability_tx_ms = d->now_ms;
    }
}

static void dev_send_ack(apex_hmi_device_t *d, apex_hmi_ack_result_t result)
{
    uint8_t buf[2] = { APEX_HMI_MSG_ACK, (uint8_t)result };
    dev_send(d, buf, sizeof(buf));
}

static void dev_enter_fault(apex_hmi_device_t *d, apex_hmi_ack_result_t reason)
{
    if (d->state == APEX_HMI_STATE_FAULT) return;
    d->state = APEX_HMI_STATE_FAULT;
    if (d->hooks.on_fault) d->hooks.on_fault(d->hooks.on_fault_user, reason);
}

static void dev_reset_class_state(apex_hmi_device_t *d)
{
    d->state = APEX_HMI_STATE_WAITING_CONFIG;
    d->capability_sent = false;
    d->capability_tx_ms = 0;
    d->active_format = APEX_HMI_FORMAT_CRSF;
    d->active_cvbs_mode = APEX_HMI_CVBS_NONE;
}

static bool dev_format_supported(const apex_hmi_device_t *d, uint8_t format)
{
    if (format >= 8) return false;
    return (d->caps.supported_control_formats & (uint8_t)(1u << format)) != 0;
}

static bool dev_cvbs_supported(const apex_hmi_device_t *d, uint8_t mode)
{
    if (mode == APEX_HMI_CVBS_NONE) {
        return d->caps.supported_cvbs_modes == 0;
    }
    if (mode >= 8) return false;
    return (d->caps.supported_cvbs_modes & (uint8_t)(1u << mode)) != 0;
}

static void dev_handle_config(apex_hmi_device_t *d,
                              const uint8_t *body, size_t body_len)
{
    /* CONFIG body: selected_format(1) + selected_cvbs(1). */
    if (body_len < 2) {
        dev_send_ack(d, APEX_HMI_ACK_REJECT_MALFORMED);
        dev_enter_fault(d, APEX_HMI_ACK_REJECT_MALFORMED);
        return;
    }
    uint8_t format = body[0];
    uint8_t cvbs   = body[1];

    if (!dev_format_supported(d, format)) {
        dev_send_ack(d, APEX_HMI_ACK_REJECT_FORMAT);
        dev_enter_fault(d, APEX_HMI_ACK_REJECT_FORMAT);
        return;
    }
    if (!dev_cvbs_supported(d, cvbs)) {
        dev_send_ack(d, APEX_HMI_ACK_REJECT_CVBS);
        dev_enter_fault(d, APEX_HMI_ACK_REJECT_CVBS);
        return;
    }

    /* Spec §4.4: a second CONFIG with different parameters is malformed. */
    if (d->state == APEX_HMI_STATE_ACTIVE) {
        if ((uint8_t)d->active_format != format ||
            (uint8_t)d->active_cvbs_mode != cvbs) {
            dev_send_ack(d, APEX_HMI_ACK_REJECT_MALFORMED);
            return;
        }
        /* Idempotent repeat — re-ACK ACCEPTED. */
        dev_send_ack(d, APEX_HMI_ACK_ACCEPTED);
        return;
    }

    d->active_format = (apex_hmi_control_format_t)format;
    d->active_cvbs_mode = (apex_hmi_cvbs_mode_t)cvbs;
    d->state = APEX_HMI_STATE_ACTIVE;
    dev_send_ack(d, APEX_HMI_ACK_ACCEPTED);
    if (d->hooks.on_active) {
        d->hooks.on_active(d->hooks.on_active_user,
                           d->active_format, d->active_cvbs_mode);
    }
}

static void dev_handle_control_data(apex_hmi_device_t *d,
                                    const uint8_t *body, size_t body_len)
{
    if (d->state != APEX_HMI_STATE_ACTIVE) return;
    if (d->hooks.on_control_data) {
        d->hooks.on_control_data(d->hooks.on_control_data_user, body, body_len);
    }
}

void apex_hmi_device_on_rx(apex_hmi_device_t *d,
                           const uint8_t *payload, size_t payload_len)
{
    if (!d || payload_len < 1) return;
    uint8_t class_msg_id = payload[0];
    const uint8_t *body = payload + 1;
    size_t body_len = payload_len - 1;

    switch (class_msg_id) {
    case APEX_HMI_MSG_CONFIG:
        dev_handle_config(d, body, body_len);
        break;
    case APEX_HMI_MSG_CONTROL_DATA:
        dev_handle_control_data(d, body, body_len);
        break;
    default:
        /* CAPABILITY and ACK are device→host; drop on device receive. */
        break;
    }
}

apex_status_t apex_hmi_device_init(apex_hmi_device_t *d,
                                   apex_device_t *core,
                                   const apex_hmi_device_caps_t *caps,
                                   const apex_hmi_device_hooks_t *hooks)
{
    if (!d || !core || !caps) return APEX_ERR_INVALID_ARGS;
    if (caps->supported_control_formats == 0) return APEX_ERR_INVALID_ARGS;
    memset(d, 0, sizeof(*d));
    d->core = core;
    d->caps = *caps;
    if (hooks) d->hooks = *hooks;
    d->state = APEX_HMI_STATE_WAITING_CONFIG;
    d->active_cvbs_mode = APEX_HMI_CVBS_NONE;
    d->last_link = APEX_DEVICE_STATE_DISCOVERING;
    return APEX_OK;
}

void apex_hmi_device_tick(apex_hmi_device_t *d, uint32_t now_ms)
{
    if (!d) return;
    d->now_ms = now_ms;

    apex_device_link_state_t link = apex_device_link_state(d->core);
    if (link != d->last_link) {
        if (link == APEX_DEVICE_STATE_CONNECTED ||
            (d->last_link == APEX_DEVICE_STATE_CONNECTED &&
             link != APEX_DEVICE_STATE_CONNECTED)) {
            dev_reset_class_state(d);
        }
        d->last_link = link;
    }

    if (link != APEX_DEVICE_STATE_CONNECTED) return;

    if (!d->capability_sent) {
        dev_send_capability(d);
        return;
    }

    if (d->state == APEX_HMI_STATE_WAITING_CONFIG) {
        if ((uint32_t)(now_ms - d->capability_tx_ms) >= APEX_HMI_CONFIG_TIMEOUT_MS) {
            dev_enter_fault(d, APEX_HMI_ACK_REJECT_MALFORMED);
        }
    }
}

apex_status_t apex_hmi_device_send_control(apex_hmi_device_t *d,
                                           const uint8_t *bytes, size_t len)
{
    if (!d) return APEX_ERR_INVALID_ARGS;
    if (d->state != APEX_HMI_STATE_ACTIVE) return APEX_ERR_BAD_STATE;
    if (len > APEX_HMI_MAX_CONTROL_FRAME_BYTES) return APEX_ERR_BUFFER_TOO_SMALL;
    /* Build inner payload: class_msg_id + frame. */
    uint8_t buf[1 + APEX_HMI_MAX_CONTROL_FRAME_BYTES];
    buf[0] = APEX_HMI_MSG_CONTROL_DATA;
    if (len > 0) memcpy(buf + 1, bytes, len);
    return dev_send(d, buf, 1 + len);
}

/* ===========================================================================
 * Host-side
 * ========================================================================= */

/* Pick the lowest set bit in `intersection` that is also in `priority_mask`,
 * or just the lowest set bit if `priority_mask == 0`. Returns -1 if no bit
 * is set. */
static int pick_negotiated_bit(uint8_t intersection, uint8_t priority_mask)
{
    uint8_t pool = priority_mask ? (uint8_t)(intersection & priority_mask)
                                 : intersection;
    if (pool == 0) pool = intersection;
    if (pool == 0) return -1;
    for (int b = 0; b < 8; b++) {
        if (pool & (uint8_t)(1u << b)) return b;
    }
    return -1;
}

static apex_status_t host_send(apex_hmi_host_t *h, uint8_t device_id,
                               const uint8_t *payload, size_t len)
{
    return apex_host_send(h->core, device_id,
                          APEX_TRAFFIC_ANALOG_HMI, payload, len);
}

static void host_send_config(apex_hmi_host_t *h, uint8_t device_id,
                             uint8_t format, uint8_t cvbs)
{
    uint8_t buf[3] = { APEX_HMI_MSG_CONFIG, format, cvbs };
    host_send(h, device_id, buf, sizeof(buf));
}

static void host_handle_capability(apex_hmi_host_t *h, uint8_t device_id,
                                   const uint8_t *body, size_t body_len)
{
    /* class_spec_version(1) + supported_formats(1) + supported_cvbs(1) +
     * intended_rate(1) = 4 bytes. */
    if (body_len < 4) return;
    /* We don't act on class_spec_version in v1; forward fields we know. */
    uint8_t dev_formats = body[1];
    uint8_t dev_cvbs    = body[2];
    uint8_t rate        = body[3];

    if (h->hooks.on_capability) {
        h->hooks.on_capability(h->hooks.on_capability_user, device_id,
                               dev_formats, dev_cvbs, rate);
    }

    apex_hmi_host_device_slot_t *slot = &h->devices[device_id];
    slot->state = APEX_HMI_STATE_WAITING_CONFIG;

    /* Validate: supported_control_formats must be non-zero. */
    if (dev_formats == 0) {
        host_send_config(h, device_id, 0xFF, 0xFF);  /* will be REJECTed */
        return;
    }

    /* Negotiate format. */
    int fmt_bit = pick_negotiated_bit((uint8_t)(dev_formats & h->caps.supported_control_formats),
                                      h->caps.format_priority);
    if (fmt_bit < 0) {
        /* Send a CONFIG with an unsupported format so the device replies
         * REJECT_FORMAT. We pick its first declared format so the reject is
         * cleanly attributable. */
        uint8_t bad_fmt = 0xFE;  /* guaranteed unsupported */
        host_send_config(h, device_id, bad_fmt, 0);
        return;
    }

    /* Negotiate CVBS. The "no CVBS" path: device declared 0 modes ⇒ host
     * sends CVBS_NONE. */
    int cvbs_choice;
    if (dev_cvbs == 0) {
        cvbs_choice = APEX_HMI_CVBS_NONE;
    } else {
        int cvbs_bit = pick_negotiated_bit((uint8_t)(dev_cvbs & h->caps.supported_cvbs_modes),
                                           h->caps.cvbs_priority);
        if (cvbs_bit < 0) {
            host_send_config(h, device_id, (uint8_t)fmt_bit, 0xFE);
            return;
        }
        cvbs_choice = cvbs_bit;
    }

    slot->format = (apex_hmi_control_format_t)fmt_bit;
    slot->cvbs_mode = (apex_hmi_cvbs_mode_t)cvbs_choice;
    host_send_config(h, device_id, (uint8_t)fmt_bit, (uint8_t)cvbs_choice);
}

static void host_handle_ack(apex_hmi_host_t *h, uint8_t device_id,
                            const uint8_t *body, size_t body_len)
{
    if (body_len < 1) return;
    apex_hmi_ack_result_t result = (apex_hmi_ack_result_t)body[0];
    apex_hmi_host_device_slot_t *slot = &h->devices[device_id];

    if (result == APEX_HMI_ACK_ACCEPTED) {
        slot->state = APEX_HMI_STATE_ACTIVE;
        if (h->hooks.on_active) {
            h->hooks.on_active(h->hooks.on_active_user, device_id,
                               slot->format, slot->cvbs_mode);
        }
    } else {
        slot->state = APEX_HMI_STATE_FAULT;
        if (h->hooks.on_reject) {
            h->hooks.on_reject(h->hooks.on_reject_user, device_id, result);
        }
    }
}

static void host_handle_control_data(apex_hmi_host_t *h, uint8_t device_id,
                                     const uint8_t *body, size_t body_len)
{
    apex_hmi_host_device_slot_t *slot = &h->devices[device_id];
    if (slot->state != APEX_HMI_STATE_ACTIVE) return;
    if (h->hooks.on_control_data) {
        h->hooks.on_control_data(h->hooks.on_control_data_user, device_id,
                                 body, body_len);
    }
}

static void host_class_rx(void *user, uint8_t device_id,
                          const uint8_t *payload, size_t payload_len)
{
    apex_hmi_host_t *h = (apex_hmi_host_t *)user;
    if (payload_len < 1) return;
    uint8_t class_msg_id = payload[0];
    const uint8_t *body = payload + 1;
    size_t body_len = payload_len - 1;
    switch (class_msg_id) {
    case APEX_HMI_MSG_CAPABILITY:
        host_handle_capability(h, device_id, body, body_len);
        break;
    case APEX_HMI_MSG_ACK:
        host_handle_ack(h, device_id, body, body_len);
        break;
    case APEX_HMI_MSG_CONTROL_DATA:
        host_handle_control_data(h, device_id, body, body_len);
        break;
    default:
        /* CONFIG is host→device; drop on host receive. */
        break;
    }
}

apex_status_t apex_hmi_host_init(apex_hmi_host_t *h,
                                 apex_host_t *core,
                                 const apex_hmi_host_caps_t *caps,
                                 const apex_hmi_host_hooks_t *hooks)
{
    if (!h || !core || !caps) return APEX_ERR_INVALID_ARGS;
    memset(h, 0, sizeof(*h));
    h->core = core;
    h->caps = *caps;
    if (hooks) h->hooks = *hooks;
    return apex_host_register_class(core, APEX_TRAFFIC_ANALOG_HMI,
                                    host_class_rx, h);
}

apex_status_t apex_hmi_host_send_control(apex_hmi_host_t *h,
                                         uint8_t device_id,
                                         const uint8_t *bytes, size_t len)
{
    if (!h) return APEX_ERR_INVALID_ARGS;
    if (h->devices[device_id].state != APEX_HMI_STATE_ACTIVE) return APEX_ERR_BAD_STATE;
    if (len > APEX_HMI_MAX_CONTROL_FRAME_BYTES) return APEX_ERR_BUFFER_TOO_SMALL;
    uint8_t buf[1 + APEX_HMI_MAX_CONTROL_FRAME_BYTES];
    buf[0] = APEX_HMI_MSG_CONTROL_DATA;
    if (len > 0) memcpy(buf + 1, bytes, len);
    return host_send(h, device_id, buf, 1 + len);
}
