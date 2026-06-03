/**
 * @file apex_core.h
 * @brief Core APEX V0 constants, enums, and types. Implements the field-level
 *        and message-level definitions from APEX_Core.md §3 and §4.
 *
 * Copyright (c) 2026 Neros Technologies. MIT License — see LICENSE.
 */
#ifndef APEX_CORE_H
#define APEX_CORE_H

#include <stddef.h>
#include <stdint.h>

#include "apex/apex_status.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------------------
 * Protocol-version & sizing — §3.1, §3.1.4
 * ------------------------------------------------------------------------- */

#define APEX_V0_PROTOCOL_VERSION   0u
#define APEX_V0_HEADER_LENGTH      4u
#define APEX_V0_CRC_LENGTH         2u
#define APEX_V0_MAX_PAYLOAD_LENGTH 255u

/* Maximum size of a *decoded* frame: header + max payload + CRC. */
#define APEX_V0_MAX_FRAME_LENGTH \
    (APEX_V0_HEADER_LENGTH + APEX_V0_MAX_PAYLOAD_LENGTH + APEX_V0_CRC_LENGTH)

/* Maximum size of an *encoded* frame on the wire: COBS overhead is
 * 1 + ceil(N/254) bytes, plus one 0x00 trailing delimiter. For N=261
 * that's 3 + 1 = 4 bytes of overhead. */
#define APEX_V0_MAX_ENCODED_FRAME_LENGTH (APEX_V0_MAX_FRAME_LENGTH + 4u)

/* ---------------------------------------------------------------------------
 * Reserved device IDs — §3.1.3
 * ------------------------------------------------------------------------- */

#define APEX_DEVICE_ID_UNASSIGNED 0x00u
#define APEX_DEVICE_ID_BROADCAST  0xFFu

/* ---------------------------------------------------------------------------
 * Traffic types — §3.1.1; full registry lives in APEX_Device_Classes.md.
 * Only CONFIG (and the first device class) is wired up here; additional
 * classes can be registered at runtime by callers via apex_host_register_class.
 * ------------------------------------------------------------------------- */

typedef enum {
    APEX_TRAFFIC_CONFIG     = 0,
    APEX_TRAFFIC_ACTIVATION = 1,
    APEX_TRAFFIC_ANALOG_HMI = 2,
    APEX_TRAFFIC_WAYFINDING = 3,
    APEX_TRAFFIC_REPEATER   = 4,
    APEX_TRAFFIC_USB_FS_HUB = 5,
    APEX_TRAFFIC_USB_HS_HUB = 6,
} apex_traffic_type_t;

/* ---------------------------------------------------------------------------
 * Outer header — §3.1.1
 * ------------------------------------------------------------------------- */

typedef struct {
    uint8_t protocol_version;
    uint8_t traffic_type;
    uint8_t device_id;
    uint8_t payload_length;
} apex_v0_hdr_t;

/* ---------------------------------------------------------------------------
 * CONFIG messages — §3.2
 * ------------------------------------------------------------------------- */

typedef enum {
    APEX_CFG_MSG_DEVICE_INFO   = 1,
    APEX_CFG_MSG_CONFIG_REPLY  = 2,
    APEX_CFG_MSG_NAME_REQUEST  = 3,
    APEX_CFG_MSG_NAME_REPLY    = 4,
    APEX_CFG_MSG_HOST_STATE    = 5,
} apex_config_msg_id_t;

/* DEVICE_INFO interface flags — §3.2.1 */
#define APEX_INTERFACE_FLAG_I2C   (1u << 0)
#define APEX_INTERFACE_FLAG_GPIO  (1u << 1)
#define APEX_INTERFACE_FLAG_USB   (1u << 2)
#define APEX_INTERFACE_FLAG_CVBS  (1u << 3)

/* CONFIG_REPLY ack values — §3.2.2 */
typedef enum {
    APEX_ACK_OK                   = 0x00,
    APEX_ACK_REJECT_CLASS         = 0x01,
    APEX_ACK_REJECT_INTERFACE     = 0x02,
    APEX_ACK_REJECT_VERSION       = 0x03,
} apex_ack_t;

/* HOST_STATE values — §3.2.5 */
typedef enum {
    APEX_FLIGHT_STATE_UNKNOWN        = 0x00,
    APEX_FLIGHT_STATE_STANDBY        = 0x01,
    APEX_FLIGHT_STATE_PROPS_ON_GND   = 0x02,
    APEX_FLIGHT_STATE_PROPS_ON_FLYING = 0x03,
    APEX_FLIGHT_STATE_FAULT          = 0xFF,
} apex_flight_state_t;

/* ---------------------------------------------------------------------------
 * Host-managed device lifecycle — §4
 * ------------------------------------------------------------------------- */

typedef enum {
    APEX_DEV_STATUS_UNKNOWN   = 0x00,
    APEX_DEV_STATUS_NEW       = 0x01,
    APEX_DEV_STATUS_CONNECTED = 0x02,
    APEX_DEV_STATUS_EXPENDED  = 0x03,
    APEX_DEV_STATUS_FAULT     = 0xFF,
} apex_device_status_t;

/* ---------------------------------------------------------------------------
 * Timing — §3.5
 * ------------------------------------------------------------------------- */

/* Each side must send at least one frame per second. */
#define APEX_HEARTBEAT_TX_PERIOD_MS  1000u
/* The watchdog fires after 5 s with no inbound frames. */
#define APEX_HEARTBEAT_WATCHDOG_MS   5000u
/* Device-side discovery retransmit floor (§3.3): "at least once per second".
 * We pick a default of 200 ms (5 Hz) so a slow-booting host is picked up
 * promptly without spamming. */
#define APEX_DISCOVERY_RETRY_MS      200u

#ifdef __cplusplus
}
#endif

#endif /* APEX_CORE_H */
