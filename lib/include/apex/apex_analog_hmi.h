/**
 * @file apex_analog_hmi.h
 * @brief Analog HMI device class (traffic_type = 3). Both Host and Device
 *        sides. See spec/APEX_Device_Class_Analog_HMI.md for the wire protocol.
 *
 * Operating model: payload streams control packets (CRSF or MAVLink 2) to the
 * host unprompted at 25-100 Hz typical. Host configures wire format and CVBS
 * pin mode once at startup, then may stream back at a lower rate (telemetry).
 *
 * Copyright (c) 2026 Neros Technologies. MIT License — see LICENSE.
 */
#ifndef APEX_ANALOG_HMI_H
#define APEX_ANALOG_HMI_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "apex/apex_core.h"
#include "apex/apex_device.h"
#include "apex/apex_host.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------------------
 * Spec enums — §3, §4.1, §4.5
 * ------------------------------------------------------------------------- */

typedef enum {
    APEX_HMI_FORMAT_CRSF     = 0,
    APEX_HMI_FORMAT_MAVLINK2 = 1,
} apex_hmi_control_format_t;

/* Bitmask helpers for supported_control_formats / supported_cvbs_modes. */
#define APEX_HMI_FORMAT_CRSF_BIT      (1u << APEX_HMI_FORMAT_CRSF)
#define APEX_HMI_FORMAT_MAVLINK2_BIT  (1u << APEX_HMI_FORMAT_MAVLINK2)

typedef enum {
    APEX_HMI_CVBS_SINGLE_ENDED = 0,
    APEX_HMI_CVBS_DIFFERENTIAL = 1,
    /* Sentinel sent by the Host in CONFIG when the Device declared no CVBS
     * support (supported_cvbs_modes == 0). The Device must accept this. */
    APEX_HMI_CVBS_NONE         = 0xFF,
} apex_hmi_cvbs_mode_t;

#define APEX_HMI_CVBS_SINGLE_ENDED_BIT (1u << APEX_HMI_CVBS_SINGLE_ENDED)
#define APEX_HMI_CVBS_DIFFERENTIAL_BIT (1u << APEX_HMI_CVBS_DIFFERENTIAL)

typedef enum {
    APEX_HMI_MSG_CAPABILITY    = 1,
    APEX_HMI_MSG_CONFIG        = 2,
    APEX_HMI_MSG_ACK           = 3,
    APEX_HMI_MSG_CONTROL_DATA  = 4,
} apex_hmi_msg_id_t;

typedef enum {
    APEX_HMI_ACK_ACCEPTED         = 0x00,
    APEX_HMI_ACK_REJECT_FORMAT    = 0x01,
    APEX_HMI_ACK_REJECT_CVBS      = 0x02,
    APEX_HMI_ACK_REJECT_MALFORMED = 0x03,
} apex_hmi_ack_result_t;

typedef enum {
    APEX_HMI_STATE_WAITING_CONFIG = 0x01,
    APEX_HMI_STATE_ACTIVE         = 0x02,
    APEX_HMI_STATE_FAULT          = 0xFF,
} apex_hmi_state_t;

/* §4.8 — Device transitions to FAULT if no CONFIG arrives within this window
 * after CAPABILITY is sent. */
#define APEX_HMI_CONFIG_TIMEOUT_MS 5000u

/* Max control-frame body that fits in one APEX V0 inner payload.
 * One byte is consumed by the class_msg_id (CONTROL_DATA = 4). */
#define APEX_HMI_MAX_CONTROL_FRAME_BYTES (APEX_MAX_PAYLOAD_LENGTH - 1u)

/* ---------------------------------------------------------------------------
 * Device-side
 * ------------------------------------------------------------------------- */

typedef struct {
    /* Bitmask of supported control formats (§3.1). At least one bit must
     * be set. */
    uint8_t supported_control_formats;
    /* Bitmask of supported CVBS modes (§3.2). May be 0 for a headless HMI
     * that does not consume CVBS. */
    uint8_t supported_cvbs_modes;
    /* Intended CONTROL_DATA TX rate (Hz). Informational. 0 = event-driven. */
    uint8_t intended_rate_hz;
} apex_hmi_device_caps_t;

typedef struct {
    /* Fired when the Host's CONFIG is accepted and the class enters ACTIVE.
     * `format` and `cvbs_mode` are the selected values. */
    void (*on_active)(void *user,
                      apex_hmi_control_format_t format,
                      apex_hmi_cvbs_mode_t cvbs_mode);
    void *on_active_user;

    /* Fired for each inbound CONTROL_DATA frame from the Host (bidi traffic).
     * The bytes are the raw control frame in the selected format. */
    void (*on_control_data)(void *user, const uint8_t *bytes, size_t len);
    void *on_control_data_user;

    /* Fired when the Device transitions to FAULT (REJECT_*, malformed config,
     * or the §4.8 CONFIG-wait timer expires). */
    void (*on_fault)(void *user, apex_hmi_ack_result_t reason);
    void *on_fault_user;
} apex_hmi_device_hooks_t;

typedef struct apex_hmi_device {
    apex_device_t *core;
    apex_hmi_device_caps_t caps;
    apex_hmi_device_hooks_t hooks;

    apex_hmi_state_t state;
    apex_hmi_control_format_t active_format;
    apex_hmi_cvbs_mode_t active_cvbs_mode;

    uint32_t now_ms;
    uint32_t capability_tx_ms;
    bool capability_sent;
    apex_device_link_state_t last_link;
} apex_hmi_device_t;

/* Initialize. Caller is responsible for forwarding the core device's
 * on_class_rx callback to apex_hmi_device_on_rx. */
apex_status_t apex_hmi_device_init(apex_hmi_device_t *d,
                                   apex_device_t *core,
                                   const apex_hmi_device_caps_t *caps,
                                   const apex_hmi_device_hooks_t *hooks);

/* Drive CAPABILITY emission and the CONFIG-wait timeout. */
void apex_hmi_device_tick(apex_hmi_device_t *d, uint32_t now_ms);

/* Pass inbound Analog-HMI-class payload (class_msg_id + body). */
void apex_hmi_device_on_rx(apex_hmi_device_t *d,
                           const uint8_t *payload,
                           size_t payload_len);

/* Stream a control frame upstream. Must be ACTIVE. `bytes` is a complete
 * control frame in the active format; the lib prepends class_msg_id and emits
 * one APEX frame. Returns APEX_ERR_BUFFER_TOO_SMALL if len exceeds
 * APEX_HMI_MAX_CONTROL_FRAME_BYTES. */
apex_status_t apex_hmi_device_send_control(apex_hmi_device_t *d,
                                           const uint8_t *bytes,
                                           size_t len);

static inline apex_hmi_state_t apex_hmi_device_state(const apex_hmi_device_t *d)
{
    return d->state;
}

/* ---------------------------------------------------------------------------
 * Host-side
 * ------------------------------------------------------------------------- */

typedef struct {
    /* Bitmask of control formats this host can accept. */
    uint8_t supported_control_formats;
    /* Bitmask of CVBS modes this host can drive. */
    uint8_t supported_cvbs_modes;
    /* Priority ordering: when multiple formats are supported on both sides,
     * the host picks the lowest-numbered set bit in `format_priority` that
     * is also set in (supported_control_formats & device_supported). If 0,
     * picks the lowest set bit of the intersection. Same logic for CVBS. */
    uint8_t format_priority;
    uint8_t cvbs_priority;
} apex_hmi_host_caps_t;

typedef struct {
    /* Fired when a device emits CAPABILITY. The host then picks a config and
     * the lib sends CONFIG automatically. This hook is observational. */
    void (*on_capability)(void *user, uint8_t device_id,
                          uint8_t supported_control_formats,
                          uint8_t supported_cvbs_modes,
                          uint8_t intended_rate_hz);
    void *on_capability_user;

    /* Fired when a device transitions to ACTIVE (acked the host's CONFIG). */
    void (*on_active)(void *user, uint8_t device_id,
                      apex_hmi_control_format_t format,
                      apex_hmi_cvbs_mode_t cvbs_mode);
    void *on_active_user;

    /* Fired when a device rejected the host's CONFIG. The session is dead. */
    void (*on_reject)(void *user, uint8_t device_id,
                      apex_hmi_ack_result_t reason);
    void *on_reject_user;

    /* Fired for every CONTROL_DATA frame from a device. Raw bytes are the
     * control frame in the selected format. */
    void (*on_control_data)(void *user, uint8_t device_id,
                            const uint8_t *bytes, size_t len);
    void *on_control_data_user;
} apex_hmi_host_hooks_t;

/* Per-device host-side bookkeeping. */
typedef struct {
    apex_hmi_state_t state;
    apex_hmi_control_format_t format;
    apex_hmi_cvbs_mode_t cvbs_mode;
} apex_hmi_host_device_slot_t;

typedef struct {
    apex_host_t *core;
    apex_hmi_host_caps_t caps;
    apex_hmi_host_hooks_t hooks;
    apex_hmi_host_device_slot_t devices[256];
} apex_hmi_host_t;

apex_status_t apex_hmi_host_init(apex_hmi_host_t *h,
                                 apex_host_t *core,
                                 const apex_hmi_host_caps_t *caps,
                                 const apex_hmi_host_hooks_t *hooks);

/* Send a control frame downstream to a specific ACTIVE device. */
apex_status_t apex_hmi_host_send_control(apex_hmi_host_t *h,
                                         uint8_t device_id,
                                         const uint8_t *bytes,
                                         size_t len);

#ifdef __cplusplus
}
#endif

#endif /* APEX_ANALOG_HMI_H */
