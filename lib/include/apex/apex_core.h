/**
 * @file apex_core.h
 * @brief Core APEX constants, enums, and types for wire protocol v1. Implements
 *        the field-level and message-level definitions from APEX_Core.md §3
 *        and §4.
 *
 * This library implements APEX wire version 1 (§3.6). v1 is a breaking revision
 * of the earlier v0 protocol: all traffic_type values shift by one, the
 * unassigned device_id marker moves to 0x01, and protocol_version 0x00 becomes
 * the permanent legacy-v0 marker.
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
 * Protocol version — §3.1.1, §3.6
 *
 * v1 session frames carry protocol_version 0x01. 0x00 is the permanent
 * legacy-v0 marker and is illegal in v1+ (drop / optional dual-stack, §3.8,
 * §3.6.4). 0xFF marks a VERSION_BEACON frame only (§3.6.2). Valid session
 * versions are 0x01–0xFE; the header prefix is non-zero by construction.
 * ------------------------------------------------------------------------- */

#define APEX_PROTOCOL_VERSION         0x01u /* This build's wire version (v1). */
#define APEX_PROTOCOL_VERSION_V0      0x00u /* Legacy-v0 marker; illegal in v1. */
#define APEX_PROTOCOL_VERSION_BEACON  0xFFu /* VERSION_BEACON frames only. */
#define APEX_PROTOCOL_VERSION_MIN     0x01u /* Lowest valid session version. */
#define APEX_PROTOCOL_VERSION_MAX     0xFEu /* Highest valid session version. */

/* ---------------------------------------------------------------------------
 * Sizing — §3.1.4
 * ------------------------------------------------------------------------- */

#define APEX_HEADER_LENGTH      4u
#define APEX_CRC_LENGTH         2u
#define APEX_MAX_PAYLOAD_LENGTH 255u

/* Maximum size of a *decoded* frame: header + max payload + CRC (261). */
#define APEX_MAX_FRAME_LENGTH \
    (APEX_HEADER_LENGTH + APEX_MAX_PAYLOAD_LENGTH + APEX_CRC_LENGTH)

/* Maximum size of an *encoded* frame on the wire: COBS adds 1 + ceil(N/254)
 * overhead bytes for an N-byte input, plus one trailing 0x00 delimiter. For
 * N=261 that is 2 overhead + 1 delimiter, giving 264 (§3.1.4). */
#define APEX_MAX_ENCODED_FRAME_LENGTH \
    (APEX_MAX_FRAME_LENGTH + (APEX_MAX_FRAME_LENGTH / 254u) + 1u + 1u)

/* ---------------------------------------------------------------------------
 * Reserved device IDs — §3.1.3
 *
 * 0x00 is invalid (non-zero-header rule). 0x01 is the unassigned marker a
 * device uses until the host assigns it an ID from the 0x02–0xFE pool. 0xFF is
 * host broadcast and the VERSION_BEACON device_id.
 * ------------------------------------------------------------------------- */

#define APEX_DEVICE_ID_INVALID      0x00u
#define APEX_DEVICE_ID_UNASSIGNED   0x01u
#define APEX_DEVICE_ID_ASSIGNED_MIN 0x02u
#define APEX_DEVICE_ID_ASSIGNED_MAX 0xFEu
#define APEX_DEVICE_ID_BROADCAST    0xFFu
#define APEX_DEVICE_ID_BEACON       0xFFu /* Link-local VERSION_BEACON (§3.6.2). */

/* ---------------------------------------------------------------------------
 * Traffic types — §3.1.1; full registry lives in APEX_Device_Classes.md.
 *
 * 0x00 is invalid. 1 = CONFIG (defined here); every other value routes to a
 * device-class spec. 0xF0–0xFE are vendor-private; 0xFF is reserved.
 * ------------------------------------------------------------------------- */

typedef enum {
    APEX_TRAFFIC_CONFIG     = 1,
    APEX_TRAFFIC_ACTIVATION = 2,
    APEX_TRAFFIC_ANALOG_HMI = 3,
    APEX_TRAFFIC_WAYFINDING = 4,
    APEX_TRAFFIC_REPEATER   = 5,
    APEX_TRAFFIC_USB_FS_HUB = 6,
    APEX_TRAFFIC_MAVLINK    = 7,
} apex_traffic_type_t;

#define APEX_TRAFFIC_INVALID      0x00u  /* Invalid by the non-zero-header rule. */
#define APEX_TRAFFIC_VENDOR_MIN   0xF0u  /* Vendor-private range 0xF0–0xFE. */
#define APEX_TRAFFIC_VENDOR_MAX   0xFEu
#define APEX_TRAFFIC_RESERVED     0xFFu

/* ---------------------------------------------------------------------------
 * Outer header — §3.1.1 (ApexHdr_t)
 * ------------------------------------------------------------------------- */

typedef struct {
    uint8_t protocol_version;
    uint8_t traffic_type;
    uint8_t device_id;
    uint8_t payload_length;
} apex_hdr_t;

/* ---------------------------------------------------------------------------
 * CONFIG messages — §3.2
 * ------------------------------------------------------------------------- */

typedef enum {
    APEX_CFG_MSG_DEVICE_INFO         = 1,
    APEX_CFG_MSG_CONFIG_REPLY        = 2,
    APEX_CFG_MSG_NAME_REQUEST        = 3,
    APEX_CFG_MSG_NAME_REPLY          = 4,
    APEX_CFG_MSG_HOST_STATE          = 5,
    APEX_CFG_MSG_CONFIG_ACK          = 6,  /* Device→Host: confirms the device
                                            * latched its assigned device_id
                                            * (§3.2.6, §3.3). */
    APEX_CFG_MSG_BAUD_CHANGE_REQUEST = 7,  /* Device→Host (§3.2.7). */
    APEX_CFG_MSG_BAUD_CHANGE_ACK     = 8,  /* Host→Device (§3.2.8). */
    APEX_CFG_MSG_PHYS_REQUEST        = 9,  /* Host→Device (§3.2.9). */
    APEX_CFG_MSG_PHYS_INFO           = 10, /* Device→Host (§3.2.10). */
    APEX_CFG_MSG_PHYS_ACK            = 11, /* Host→Device (§3.2.11). */
    APEX_CFG_MSG_VERSION_BEACON      = 12, /* Either role; PV=0xFF only (§3.2.12,
                                            * §3.6.2). */
    APEX_CFG_MSG_RESET_REQUEST       = 13, /* Either direction; session
                                            * re-enumeration (brown-out
                                            * recovery), no fields. Host→Device
                                            * addressed/0x01/broadcast;
                                            * Device→Host announce-then-drop
                                            * (see §3.2.13). */
} apex_config_msg_id_t;

/* DEVICE_INFO interface flags — §3.2.1 */
#define APEX_INTERFACE_FLAG_I2C   (1u << 0)
#define APEX_INTERFACE_FLAG_GPIO  (1u << 1)
#define APEX_INTERFACE_FLAG_USB   (1u << 2)
#define APEX_INTERFACE_FLAG_CVBS  (1u << 3)

/* Global ack-code namespace — §3.2. One shared namespace across CONFIG_REPLY,
 * PHYS_ACK, BAUD_CHANGE_ACK and the provisional-phase acks; each reject code is
 * defined by exactly one message. */
typedef enum {
    APEX_ACK_OK                   = 0x00, /* Terminal accept; carries assigned id. */
    APEX_ACK_REJECT_CLASS         = 0x01, /* CONFIG_REPLY. */
    APEX_ACK_REJECT_INTERFACE     = 0x02, /* CONFIG_REPLY. */
    /* 0x03 is retired — was v0's ACK_REJECT_VERSION, obsolete under the
     * version-beacon kernel (§3.6.2). Never reused; no enumerator defined. */
    APEX_ACK_PROVISIONAL          = 0x04, /* Conditional accept; phase continues. */
    APEX_ACK_REJECT_CLASS_VERSION = 0x05, /* CONFIG_REPLY. */
    APEX_ACK_REJECT_MASS          = 0x06, /* CONFIG_REPLY. */
    APEX_ACK_REJECT_BAUD          = 0x07, /* BAUD_CHANGE_ACK; carries counter-offer. */
    APEX_ACK_REJECT_PHYS          = 0x08, /* PHYS_ACK. */
    APEX_ACK_REJECT_POLICY        = 0x09, /* CONFIG_REPLY. */
} apex_ack_t;

/* HOST_STATE flight_state values — §3.2.5 */
typedef enum {
    APEX_FLIGHT_STATE_UNKNOWN        = 0x00,
    APEX_FLIGHT_STATE_STANDBY        = 0x01,
    APEX_FLIGHT_STATE_PROPS_ON_GND   = 0x02,
    APEX_FLIGHT_STATE_PROPS_ON_FLYING = 0x03,
    APEX_FLIGHT_STATE_FAULT          = 0xFF,
} apex_flight_state_t;

/* HOST_STATE warnings bitfield — §3.2.5. Advisory, ORTHOGONAL to flight_state:
 * each bit is a condition (e.g. RC link loss) that may hold during any flight
 * phase, so warnings are a separate field — not flight_state values — and any
 * number may be set at once. `0` = no warnings; a device that has never received
 * HOST_STATE treats warnings as 0. */
#define APEX_HOST_WARNING_RC_LINK_LOSS  (1u << 0)  /* host has lost its RC/command link */
/* bits 1..7 reserved for future advisory conditions */

/* Baud-rate codes — §3.4 */
typedef enum {
    APEX_BAUD_CODE_115200 = 0,
    APEX_BAUD_CODE_460800 = 1,
    APEX_BAUD_CODE_921600 = 2,
} apex_baud_code_t;

/* ---------------------------------------------------------------------------
 * Physical declaration — §3.2.10 PHYS_INFO (the canonical complete state)
 *
 * The complete current physical state of the payload: mass plus the full
 * 10-parameter rigid-body spatial inertia about the APEX dovetail datum — CG
 * offset (first moment) and the symmetric inertia tensor, in the reference
 * frame and units of §2.1. At config time the mass duplicates DEVICE_INFO's
 * declaration; after an in-flight physical update (unsolicited post-CONNECTED
 * PHYS_INFO) it is the current value. The product-of-inertia fields carry
 * the *un-negated* integral form (`pxy = +∫xy dm`, etc., §3.2.10); the
 * definitional minus (`Ixy = −pxy`) is applied exactly once by host consumers,
 * not here.
 *
 * This is the in-memory (host-order) representation of the PHYS_INFO wire
 * payload; the device/host builders serialize/parse it little-endian, in wire
 * field order.
 * ------------------------------------------------------------------------- */
typedef struct {
    uint16_t mass_grams;    /* Current payload mass, g (§3.2.1 rules). */
    int16_t cg_offset_x_mm; /* CG offset from datum along +X, mm. */
    int16_t cg_offset_y_mm; /* CG offset along +Y, mm. */
    int16_t cg_offset_z_mm; /* CG offset along +Z, mm. */
    int32_t ixx;            /* Moment about datum X, g·cm². */
    int32_t iyy;            /* Moment about datum Y, g·cm². */
    int32_t izz;            /* Moment about datum Z, g·cm². */
    int32_t pxy;            /* Product +∫xy dm, g·cm² (un-negated). */
    int32_t pxz;            /* Product +∫xz dm, g·cm². */
    int32_t pyz;            /* Product +∫yz dm, g·cm². */
} apex_phys_t;

/* Wire-payload lengths for CONFIG messages that carry a fixed layout (§3.2). */
#define APEX_DEVICE_INFO_PAYLOAD_LENGTH  7u   /* §3.2.1 */
#define APEX_CONFIG_REPLY_PAYLOAD_LENGTH 6u   /* §3.2.2 */
#define APEX_PHYS_INFO_PAYLOAD_LENGTH    33u  /* §3.2.10 (+mass_grams) */

/* ---------------------------------------------------------------------------
 * Host-managed device lifecycle — §4
 *
 * apex_device_status_t (ApexDeviceStatus_t) is declared in apex_status.h.
 * ------------------------------------------------------------------------- */

/* ---------------------------------------------------------------------------
 * VERSION_BEACON — §3.2.12, §3.6.2
 *
 * The immutable versioning kernel. Decoded, before CRC/COBS:
 *   FF 01 FF 05  0C min_lo min_hi max_lo max_hi
 * i.e. PV=0xFF, TT=CONFIG(1), ID=0xFF, LN=5, msg_id=12, then u16 LE min/max
 * wire-version range. Version values are valid 1–65534 (0x0000 and 0xFFFF are
 * reserved and never appear).
 * ------------------------------------------------------------------------- */

#define APEX_BEACON_MSG_ID          12u
#define APEX_BEACON_PAYLOAD_LENGTH  5u   /* msg_id + u16 min + u16 max. */
#define APEX_BEACON_VERSION_MIN     1u
#define APEX_BEACON_VERSION_MAX     65534u

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
