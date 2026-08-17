/**
 * @file apex_repeater.h
 * @brief Repeater device class (traffic_type = 5). Both Host and Device sides.
 *        See APEX_Device_Class_Repeater.md for the wire protocol.
 *
 * The Repeater class covers RF relay nodes that extend C2 and video links
 * between a ground controller and a distal drone. The host observes live RF
 * metrics via periodic TELEMETRY frames, requests a full config snapshot via
 * GET_CONFIG / CONFIG_REPORT, and may update runtime parameters via SET_CONFIG.
 *
 * Copyright (c) 2026 Neros Technologies. MIT License — see LICENSE.
 */
#ifndef APEX_REPEATER_H
#define APEX_REPEATER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "apex/apex_core.h"
#include "apex/apex_device.h"
#include "apex/apex_host.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Compile-time tuning. */
/* Max opaque radio-config blob bytes per C2 link. 48 is the working bound the
 * class assumes (§3.2): it keeps CONFIG_REPORT ≤ 255 bytes in the worst case —
 * all four C2 links fully populated, video present, AND the full 8-antenna list
 * (each C2 section is 3 header bytes + 48 blob + 1 antenna_id = 52; total
 * 2 hdr + 4×52 + 12 video + (1 + 8×2) antennas + 2 global = 241). */
#ifndef APEX_REPEATER_C2_CONFIG_MAX
#define APEX_REPEATER_C2_CONFIG_MAX 48u
#endif
/* Maximum antennas in the list. 8 is the NORMATIVE spec ceiling (§3.2), and
 * support for 8 is GUARANTEED: the antenna_id field is a full u8, but a 255-byte
 * payload cannot hold anywhere near 256 antennas (each costs 7 bytes per
 * TELEMETRY frame + 2 in CONFIG_REPORT), so the spec caps antenna_count at 8 —
 * a count that always fits (worst-case TELEMETRY 85 bytes, CONFIG_REPORT 237).
 * Frames with antenna_count > this value are dropped as malformed. A
 * memory-constrained build MAY lower this; it MUST NOT raise it above 8 (an
 * 8-antenna peer would then overflow this build's fixed-size arrays). */
#ifndef APEX_REPEATER_MAX_ANTENNAS
#define APEX_REPEATER_MAX_ANTENNAS 8u
#endif
/* Max raw bytes in a single DISTAL_TLM frame
 * (class_msg_id + link_index + c2_protocol consume 3). */
#ifndef APEX_REPEATER_DISTAL_TLM_MAX
#define APEX_REPEATER_DISTAL_TLM_MAX (APEX_MAX_PAYLOAD_LENGTH - 3u)
#endif

/* Timing constants (§6.9). */
#define APEX_REPEATER_TELEMETRY_FIRST_MS   100u   /* first TELEMETRY after class active */
#define APEX_REPEATER_TELEMETRY_PERIOD_MS  1000u  /* periodic TELEMETRY floor */
#define APEX_REPEATER_CONFIG_REPLY_MS      500u   /* CONFIG_REPORT after GET_CONFIG */

/* ---------------------------------------------------------------------------
 * Spec enums and constants — §4, §6
 * ------------------------------------------------------------------------- */

typedef enum {
    APEX_RPT_MSG_TELEMETRY     = 1,
    APEX_RPT_MSG_DISTAL_TLM    = 2,
    APEX_RPT_MSG_CONFIG_REPORT = 3,
    APEX_RPT_MSG_ACK           = 4,
    APEX_RPT_MSG_GET_CONFIG    = 5,
    APEX_RPT_MSG_SET_CONFIG    = 6,
    APEX_RPT_MSG_ANTENNA_CMD   = 7,
} apex_repeater_msg_id_t;

typedef enum {
    APEX_RPT_STATE_ACTIVE = 0x01,
    APEX_RPT_STATE_FAULT  = 0xFF,
} apex_repeater_state_t;

typedef enum {
    APEX_RPT_ACK_ACCEPTED              = 0x00,
    APEX_RPT_ACK_REJECT_INVALID_VALUE  = 0x01,
    APEX_RPT_ACK_REJECT_MALFORMED      = 0x02,
    APEX_RPT_ACK_REJECT_WRONG_STATE    = 0x03,
} apex_repeater_ack_result_t;

/* Capability flags (§3) — bitmask in every TELEMETRY and CONFIG_REPORT. */
#define APEX_RPT_CAP_C2_LINK_0   (1u << 0)
#define APEX_RPT_CAP_C2_LINK_1   (1u << 1)
#define APEX_RPT_CAP_C2_LINK_2   (1u << 2)
#define APEX_RPT_CAP_C2_LINK_3   (1u << 3)
#define APEX_RPT_CAP_VIDEO       (1u << 4)
/* Bit 5: device exposes an antenna list (§3.2) — a positionally-indexed
 * inventory of its physical antennas, the per-link antenna assignment carried
 * in CONFIG_REPORT, and a per-antenna pointing block in TELEMETRY. */
#define APEX_RPT_CAP_ANTENNAS    (1u << 5)
/* Mask of bits that carry link blocks (bits 0–4). */
#define APEX_RPT_CAP_LINK_MASK   (0x1Fu)

/* Link block flags (§6.1). */
#define APEX_RPT_LINK_RX_ACTIVE (1u << 0)
#define APEX_RPT_LINK_TX_ACTIVE (1u << 1)

/* SET_CONFIG update_mask bit assignments (§6.6, parallel to capability_flags). */
#define APEX_RPT_UPDATE_C2_LINK_0 (1u << 0)
#define APEX_RPT_UPDATE_C2_LINK_1 (1u << 1)
#define APEX_RPT_UPDATE_C2_LINK_2 (1u << 2)
#define APEX_RPT_UPDATE_C2_LINK_3 (1u << 3)
#define APEX_RPT_UPDATE_VIDEO_TX  (1u << 4)
#define APEX_RPT_UPDATE_ANTENNA   (1u << 5)
#define APEX_RPT_UPDATE_GLOBAL    (1u << 6)

/* Special bearing value: return to auto / stow position. */
#define APEX_RPT_BEARING_AUTO 0xFFFFu

/* RSSI sentinel: no signal. */
#define APEX_RPT_RSSI_NO_SIGNAL ((int8_t)(-128))

typedef enum {
    APEX_RPT_VIDEO_FORMAT_ANALOG_PAL  = 0x00,
    APEX_RPT_VIDEO_FORMAT_ANALOG_NTSC = 0x01,
    APEX_RPT_VIDEO_FORMAT_DIGITAL     = 0x02,
} apex_repeater_video_format_t;

typedef enum {
    APEX_RPT_ANTENNA_OMNI              = 0x00,
    APEX_RPT_ANTENNA_DIRECTIONAL_DOA   = 0x01, /* passive DOA / GPS-derived bearing */
    APEX_RPT_ANTENNA_DIRECTIONAL_AIMABLE = 0x02, /* physical gimbal; responds to ANTENNA_CMD */
} apex_repeater_antenna_type_t;

typedef enum {
    APEX_RPT_BEARING_REF_MAGNETIC  = 0x00,
    APEX_RPT_BEARING_REF_DRONE_HDG = 0x01,
} apex_repeater_bearing_ref_t;

typedef enum {
    APEX_RPT_ENCRYPTION_NONE             = 0x00,
    APEX_RPT_ENCRYPTION_ENABLED          = 0x01,
    APEX_RPT_ENCRYPTION_ENABLED_VERIFIED = 0x02,
} apex_repeater_encryption_state_t;

/* C2 link protocol (§3.1) — the command/telemetry framing a link carries
 * end-to-end. Radio-agnostic: the underlying radio (ELRS, custom FSK/LoRa,
 * serial modem, …) is independent of this and lives in the opaque config blob. */
typedef enum {
    APEX_RPT_C2_PROTOCOL_CRSF    = 0x00,
    APEX_RPT_C2_PROTOCOL_MAVLINK = 0x01,
    APEX_RPT_C2_PROTOCOL_OPAQUE  = 0xFF, /* unknown/other; host treats DISTAL_TLM as raw */
} apex_repeater_c2_protocol_t;

/* ---------------------------------------------------------------------------
 * Sub-structures — shared by device and host sides
 * ------------------------------------------------------------------------- */

/* 5-byte link block (§6.1). One per set bit in capability_flags bits 0–4. */
typedef struct {
    int8_t  rssi_dbm;       /* RSSI_NO_SIGNAL if no signal */
    uint8_t lq_percent;     /* 0–100; 0 if not receiving */
    int8_t  snr_db;         /* RSSI_NO_SIGNAL if not available */
    int8_t  tx_power_dbm;   /* 0 if not transmitting */
    uint8_t flags;          /* APEX_RPT_LINK_* bitmask */
} apex_repeater_link_block_t;

/* 7-byte per-antenna directionality sub-structure (§6.2). One appears per listed
 * antenna when CAP_ANTENNAS is set, in antenna-index order. An omni antenna
 * reports all-0xFFFF fields with confidence 0. */
typedef struct {
    uint16_t antenna_bearing_deg; /* current gimbal bearing; 0xFFFF if N/A or not aimable */
    uint16_t distal_bearing_deg;  /* RF/GPS-derived bearing to distal drone; 0xFFFF if N/A */
    uint16_t distal_distance_m;   /* derived distance in metres; 0xFFFF if N/A */
    uint8_t  confidence;          /* 0–100; 0 when derived fields are 0xFFFF */
} apex_repeater_directionality_t;

/* Parsed TELEMETRY frame (§6.2).
 * link_blocks[b] is valid when bit b is set in capability_flags (b 0–3 = C2, b 4 = video).
 * When APEX_RPT_CAP_ANTENNAS is set, antenna_dir[0..antenna_count-1] carries the
 * per-antenna pointing state, positionally keyed to the CONFIG_REPORT antenna list. */
typedef struct {
    uint8_t device_state;    /* apex_repeater_state_t */
    uint8_t capability_flags;
    apex_repeater_link_block_t   link_blocks[5]; /* indexed by bit position 0–4 */
    uint8_t                      antenna_count;  /* number of valid antenna_dir entries */
    apex_repeater_directionality_t antenna_dir[APEX_REPEATER_MAX_ANTENNAS];
} apex_repeater_telemetry_t;

/* Configuration for one C2 link (§6.3). */
typedef struct {
    uint8_t c2_protocol;     /* apex_repeater_c2_protocol_t; read-only via SET_CONFIG */
    uint8_t c2_config_version;
    uint8_t c2_config_len;
    uint8_t c2_config_blob[APEX_REPEATER_C2_CONFIG_MAX]; /* opaque radio config */
    uint8_t antenna_id;      /* listed antenna this link uses; valid when CAP_ANTENNAS set.
                              * Read-only via SET_CONFIG (a hardware/provisioning fact). */
} apex_repeater_c2_config_t;

/* Video link configuration (§6.3). Frequencies are reported as center + bandwidth
 * so the occupied band / keep-out zone is [center − bw/2, center + bw/2]. */
typedef struct {
    uint16_t rx_freq_mhz;    /* receive center freq from distal drone */
    uint16_t rx_bw_mhz;      /* receive occupied bandwidth; read-only via SET_CONFIG */
    uint8_t  rx_format;      /* apex_repeater_video_format_t; read-only via SET_CONFIG */
    uint16_t tx_freq_mhz;    /* re-broadcast center freq toward ground */
    uint16_t tx_bw_mhz;      /* re-broadcast occupied bandwidth */
    int8_t   tx_power_dbm;
    uint8_t  tx_format;      /* apex_repeater_video_format_t */
    uint8_t  antenna_id;     /* listed antenna the video link uses; valid when CAP_ANTENNAS
                              * set. Read-only via SET_CONFIG. */
} apex_repeater_video_config_t;

/* One entry in the antenna list (§6.3). Indexed positionally by antenna_id. */
typedef struct {
    uint8_t antenna_type;        /* apex_repeater_antenna_type_t; hardware-fixed */
    uint8_t antenna_bearing_ref; /* apex_repeater_bearing_ref_t; updatable via SET_CONFIG */
} apex_repeater_antenna_config_t;

/* Global configuration (§6.3). */
typedef struct {
    uint8_t encryption_state;    /* apex_repeater_encryption_state_t; read-only via SET_CONFIG */
    uint8_t distal_tlm_rate_hz;  /* forwarding rate cap; updatable via SET_CONFIG */
} apex_repeater_global_config_t;

/* Aggregate configuration snapshot (CONFIG_REPORT payload / SET_CONFIG source). */
typedef struct {
    uint8_t capability_flags;
    apex_repeater_c2_config_t     c2[4];    /* indexed by C2 link 0–3 */
    apex_repeater_video_config_t  video;
    uint8_t                       antenna_count; /* valid when CAP_ANTENNAS set */
    apex_repeater_antenna_config_t antennas[APEX_REPEATER_MAX_ANTENNAS]; /* indexed by antenna_id */
    apex_repeater_global_config_t  global;
} apex_repeater_config_t;

/* ACK payload (§6.4). */
typedef struct {
    uint8_t acked_msg;               /* class_msg_id being acknowledged */
    apex_repeater_ack_result_t result;
    uint8_t failed_section;          /* update_mask bit of failing section; 0xFF if N/A */
} apex_repeater_ack_t;

/* ---------------------------------------------------------------------------
 * Device-side
 * ------------------------------------------------------------------------- */

typedef struct {
    /* Called when a valid SET_CONFIG is received. `update_mask` identifies
     * which sections changed; `new_cfg` carries the full proposed config.
     * Return APEX_OK to accept (lib applies and ACKs ACCEPTED). Any other
     * value rejects with REJECT_INVALID_VALUE and leaves config unchanged. */
    apex_status_t (*on_set_config)(void *user, uint8_t update_mask,
                                   const apex_repeater_config_t *new_cfg);
    void *on_set_config_user;

    /* Called when ANTENNA_CMD arrives. Only fired when `antenna_id` names a
     * listed antenna whose antenna_type is DIRECTIONAL_AIMABLE and the device is
     * not in FAULT. `bearing_deg` is 0–359 or APEX_RPT_BEARING_AUTO. Return
     * APEX_OK to accept. */
    apex_status_t (*on_antenna_cmd)(void *user, uint8_t antenna_id, uint16_t bearing_deg);
    void *on_antenna_cmd_user;
} apex_repeater_device_hooks_t;

typedef struct apex_repeater_device {
    apex_device_t *core;
    apex_repeater_device_hooks_t hooks;

    /* Live RF metrics — updated by the application via
     * apex_repeater_device_update_telemetry(). */
    apex_repeater_telemetry_t telemetry;
    bool telemetry_dirty;

    /* Stored configuration — provided at init; updated by SET_CONFIG. */
    apex_repeater_config_t config;

    uint32_t now_ms;
    uint32_t last_telemetry_tx_ms;
    bool     first_telemetry_sent;
    apex_device_link_state_t last_link;
} apex_repeater_device_t;

/* Initialize. `initial_config` is copied in; `telemetry` starts zeroed with
 * device_state = ACTIVE and capability_flags from initial_config.
 * Caller wires the core device's on_class_rx to apex_repeater_device_on_rx. */
apex_status_t apex_repeater_device_init(apex_repeater_device_t *d,
                                        apex_device_t *core,
                                        const apex_repeater_config_t *initial_config,
                                        const apex_repeater_device_hooks_t *hooks);

/* Drive periodic TELEMETRY emission. Call from the application's main loop. */
void apex_repeater_device_tick(apex_repeater_device_t *d, uint32_t now_ms);

/* Pass inbound Repeater-class payload (class_msg_id byte + body). */
void apex_repeater_device_on_rx(apex_repeater_device_t *d,
                                const uint8_t *payload, size_t payload_len);

/* Update live RF metrics. Marks telemetry dirty — next tick flushes immediately.
 * capability_flags in `tlm` must match d->config.capability_flags. */
void apex_repeater_device_update_telemetry(apex_repeater_device_t *d,
                                           const apex_repeater_telemetry_t *tlm);

/* Forward raw telemetry bytes received from the distal drone on `link_index`
 * (0–3). `bytes` are emitted verbatim in a DISTAL_TLM frame (§6.7), tagged with
 * that link's configured `c2_protocol` so the Host can route them. */
apex_status_t apex_repeater_device_send_distal_tlm(apex_repeater_device_t *d,
                                                    uint8_t link_index,
                                                    const uint8_t *bytes, size_t len);

/* Transition to FAULT. Subsequent TELEMETRY frames will carry state=FAULT.
 * Recovery requires a device reset. */
void apex_repeater_device_set_fault(apex_repeater_device_t *d);

static inline apex_repeater_state_t apex_repeater_device_state(
    const apex_repeater_device_t *d)
{
    return (apex_repeater_state_t)d->telemetry.device_state;
}

/* ---------------------------------------------------------------------------
 * Host-side
 * ------------------------------------------------------------------------- */

typedef struct {
    /* Fired when a TELEMETRY frame arrives. */
    void (*on_telemetry)(void *user, uint8_t device_id,
                         const apex_repeater_telemetry_t *tlm);
    void *on_telemetry_user;

    /* Fired when a CONFIG_REPORT arrives (response to GET_CONFIG). */
    void (*on_config_report)(void *user, uint8_t device_id,
                             const apex_repeater_config_t *config);
    void *on_config_report_user;

    /* Fired when an ACK arrives for SET_CONFIG or ANTENNA_CMD. */
    void (*on_ack)(void *user, uint8_t device_id,
                   const apex_repeater_ack_t *ack);
    void *on_ack_user;

    /* Fired when a DISTAL_TLM frame arrives. `bytes` are raw telemetry from
     * the distal drone, in the protocol given by `c2_protocol`
     * (apex_repeater_c2_protocol_t); route to the matching parser. `bytes` are
     * valid only during the callback. */
    void (*on_distal_tlm)(void *user, uint8_t device_id,
                          uint8_t link_index, uint8_t c2_protocol,
                          const uint8_t *bytes, size_t len);
    void *on_distal_tlm_user;
} apex_repeater_host_hooks_t;

typedef struct {
    apex_host_t *core;
    apex_repeater_host_hooks_t hooks;
} apex_repeater_host_t;

apex_status_t apex_repeater_host_init(apex_repeater_host_t *h,
                                      apex_host_t *core,
                                      const apex_repeater_host_hooks_t *hooks);

/* Send GET_CONFIG; device replies with CONFIG_REPORT (§6.5). */
apex_status_t apex_repeater_host_get_config(apex_repeater_host_t *h,
                                            uint8_t device_id);

/* Send SET_CONFIG. Only sections with their bit set in `update_mask` are
 * included in the frame; others are untouched on the device. The device
 * replies with ACK. Caller should follow up with GET_CONFIG to verify (§8.1).
 *
 * The antenna section (APEX_RPT_UPDATE_ANTENNA, bit 5) is per-antenna and is
 * NOT accepted here — passing it returns APEX_ERR_INVALID_ARGS. Use
 * apex_repeater_host_set_antenna_ref() instead. */
apex_status_t apex_repeater_host_set_config(apex_repeater_host_t *h,
                                            uint8_t device_id,
                                            uint8_t update_mask,
                                            const apex_repeater_config_t *config);

/* Send SET_CONFIG for the antenna section (bit 5): update the bearing reference
 * of the listed antenna `antenna_id`. The device ACKs; out-of-range ids are
 * rejected with REJECT_INVALID_VALUE. `bearing_ref` is an apex_repeater_bearing_ref_t. */
apex_status_t apex_repeater_host_set_antenna_ref(apex_repeater_host_t *h,
                                                 uint8_t device_id,
                                                 uint8_t antenna_id,
                                                 uint8_t bearing_ref);

/* Send ANTENNA_CMD targeting a particular listed antenna. Accepted only by
 * devices whose antenna `antenna_id` has antenna_type DIRECTIONAL_AIMABLE.
 * bearing_deg: 0–359, or APEX_RPT_BEARING_AUTO to return to stow/auto. */
apex_status_t apex_repeater_host_antenna_cmd(apex_repeater_host_t *h,
                                             uint8_t device_id,
                                             uint8_t antenna_id,
                                             uint16_t bearing_deg);

#ifdef __cplusplus
}
#endif

#endif /* APEX_REPEATER_H */
