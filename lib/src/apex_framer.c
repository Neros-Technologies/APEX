/**
 * @file apex_framer.c
 * @brief Outer-header pack/unpack, a 0x00-delimited byte-stream framer, the
 *        decode-free header-prefix peek (§3.1.6), and the VERSION_BEACON codec
 *        (§3.6.2).
 *
 * Copyright (c) 2026 Neros Technologies. MIT License — see LICENSE.
 */
#include <string.h>

#include "apex/apex_cobs.h"
#include "apex/apex_crc.h"
#include "apex/apex_framer.h"

/* ---------------------------------------------------------------------------
 * Encode
 * ------------------------------------------------------------------------- */

apex_status_t apex_frame_encode(const apex_hdr_t *hdr,
                                const uint8_t *payload,
                                uint8_t *out,
                                size_t out_capacity,
                                size_t *out_len)
{
    uint8_t decoded[APEX_MAX_FRAME_LENGTH];
    size_t decoded_len;
    uint16_t crc;
    size_t encoded_len;
    apex_status_t status;

    if (out_len) *out_len = 0;
    if (!hdr || !out) return APEX_ERR_INVALID_ARGS;
    if (hdr->payload_length > 0 && !payload) return APEX_ERR_INVALID_ARGS;
    if (out_capacity < APEX_MAX_ENCODED_FRAME_LENGTH) return APEX_ERR_BUFFER_TOO_SMALL;

    /* §3.1.1: the three header-prefix bytes are non-zero by construction. Refuse
     * to build a frame that would violate this (protocol_version 0x00 is the
     * legacy-v0 marker; traffic_type / device_id 0x00 are invalid). PV 0xFF
     * beacons are built via apex_beacon_build(), not here. */
    if (hdr->protocol_version == APEX_PROTOCOL_VERSION_V0 ||
        hdr->traffic_type == APEX_TRAFFIC_INVALID ||
        hdr->device_id == APEX_DEVICE_ID_INVALID) {
        return APEX_ERR_INVALID_ARGS;
    }

    /* Pack header (little-endian — see §3.1). All header fields are single
     * bytes so endianness is trivial; we still write byte-by-byte to stay
     * portable. */
    decoded[0] = hdr->protocol_version;
    decoded[1] = hdr->traffic_type;
    decoded[2] = hdr->device_id;
    decoded[3] = hdr->payload_length;

    if (hdr->payload_length > 0) {
        memcpy(decoded + APEX_HEADER_LENGTH, payload, hdr->payload_length);
    }
    decoded_len = APEX_HEADER_LENGTH + hdr->payload_length;

    /* CRC over header + payload, in TX order, before COBS (§3.1.2). */
    crc = apex_crc16(decoded, decoded_len);
    decoded[decoded_len + 0] = (uint8_t)(crc & 0xFFu);
    decoded[decoded_len + 1] = (uint8_t)((crc >> 8) & 0xFFu);
    decoded_len += APEX_CRC_LENGTH;

    /* COBS-encode into `out`, leaving room for the trailing 0x00 delimiter. */
    status = apex_cobs_encode(decoded, decoded_len, out, out_capacity - 1, &encoded_len);
    if (status != APEX_OK) return status;

    out[encoded_len] = 0x00;
    if (out_len) *out_len = encoded_len + 1;
    return APEX_OK;
}

/* ---------------------------------------------------------------------------
 * Decode
 * ------------------------------------------------------------------------- */

apex_status_t apex_frame_decode(const uint8_t *cobs_bytes,
                                size_t cobs_len,
                                uint8_t *decoded_workspace,
                                size_t workspace_len,
                                apex_hdr_t *hdr_out,
                                const uint8_t **payload_out,
                                size_t *payload_len_out)
{
    size_t decoded_len;
    apex_status_t status;
    uint16_t expected_crc, received_crc;
    size_t payload_len;

    if (!cobs_bytes || !decoded_workspace || !hdr_out) return APEX_ERR_INVALID_ARGS;
    if (workspace_len < APEX_MAX_FRAME_LENGTH) return APEX_ERR_BUFFER_TOO_SMALL;

    status = apex_cobs_decode(cobs_bytes, cobs_len,
                              decoded_workspace, workspace_len,
                              &decoded_len);
    if (status != APEX_OK) return status;

    /* Minimum legal frame: header + CRC, payload_length=0 (heartbeat). */
    if (decoded_len < APEX_HEADER_LENGTH + APEX_CRC_LENGTH) {
        return APEX_ERR_MALFORMED;
    }

    hdr_out->protocol_version = decoded_workspace[0];
    hdr_out->traffic_type     = decoded_workspace[1];
    hdr_out->device_id        = decoded_workspace[2];
    hdr_out->payload_length   = decoded_workspace[3];

    payload_len = hdr_out->payload_length;
    if (decoded_len != APEX_HEADER_LENGTH + payload_len + APEX_CRC_LENGTH) {
        /* payload_length doesn't match the actual decoded length — corrupted
         * header or truncated frame. */
        return APEX_ERR_MALFORMED;
    }

    /* Verify CRC. */
    received_crc =
        (uint16_t)decoded_workspace[APEX_HEADER_LENGTH + payload_len] |
        ((uint16_t)decoded_workspace[APEX_HEADER_LENGTH + payload_len + 1] << 8);
    expected_crc = apex_crc16(decoded_workspace, APEX_HEADER_LENGTH + payload_len);
    if (received_crc != expected_crc) return APEX_ERR_BAD_CRC;

    if (payload_out) {
        *payload_out = (payload_len > 0)
            ? decoded_workspace + APEX_HEADER_LENGTH
            : NULL;
    }
    if (payload_len_out) *payload_len_out = payload_len;
    return APEX_OK;
}

/* ---------------------------------------------------------------------------
 * Decode-free header-prefix peek — §3.1.6, §3.7.5
 * ------------------------------------------------------------------------- */

apex_status_t apex_framer_peek_prefix(const uint8_t *encoded,
                                      size_t len,
                                      apex_hdr_t *out_prefix)
{
    if (!encoded || !out_prefix) return APEX_ERR_INVALID_ARGS;

    /* Sanity gates before trusting the peek (§3.7.5). */
    if (len < 5) return APEX_ERR_MALFORMED;           /* encoded length >= 5 */
    if (encoded[0] < 4) return APEX_ERR_MALFORMED;    /* leading code byte >= 4 */
    if (encoded[1] == APEX_PROTOCOL_VERSION_V0)        /* PV != 0x00 (drop v0) */
        return APEX_ERR_MALFORMED;
    if (encoded[2] == APEX_TRAFFIC_INVALID) return APEX_ERR_MALFORMED;   /* TT != 0 */
    if (encoded[3] == APEX_DEVICE_ID_INVALID) return APEX_ERR_MALFORMED; /* ID != 0 */

    out_prefix->protocol_version = encoded[1];
    out_prefix->traffic_type     = encoded[2];
    out_prefix->device_id        = encoded[3];
    out_prefix->payload_length   = 0;
    return APEX_OK;
}

/* ---------------------------------------------------------------------------
 * VERSION_BEACON codec — §3.2.12, §3.6.2
 * ------------------------------------------------------------------------- */

static int beacon_version_valid(uint16_t v)
{
    return v >= APEX_BEACON_VERSION_MIN && v <= APEX_BEACON_VERSION_MAX;
}

apex_status_t apex_beacon_build(uint16_t min_version,
                                uint16_t max_version,
                                uint8_t *buf,
                                size_t cap,
                                size_t *out_len)
{
    apex_hdr_t hdr;
    uint8_t payload[APEX_BEACON_PAYLOAD_LENGTH];
    uint8_t decoded[APEX_HEADER_LENGTH + APEX_BEACON_PAYLOAD_LENGTH + APEX_CRC_LENGTH];
    size_t decoded_len;
    uint16_t crc;
    size_t encoded_len;
    apex_status_t status;

    if (out_len) *out_len = 0;
    if (!buf) return APEX_ERR_INVALID_ARGS;
    if (cap < APEX_MAX_ENCODED_FRAME_LENGTH) return APEX_ERR_BUFFER_TOO_SMALL;
    if (!beacon_version_valid(min_version) || !beacon_version_valid(max_version) ||
        min_version > max_version) {
        return APEX_ERR_INVALID_ARGS;
    }

    hdr.protocol_version = APEX_PROTOCOL_VERSION_BEACON;
    hdr.traffic_type     = APEX_TRAFFIC_CONFIG;
    hdr.device_id        = APEX_DEVICE_ID_BEACON;
    hdr.payload_length   = APEX_BEACON_PAYLOAD_LENGTH;

    payload[0] = APEX_BEACON_MSG_ID;
    payload[1] = (uint8_t)(min_version & 0xFFu);
    payload[2] = (uint8_t)((min_version >> 8) & 0xFFu);
    payload[3] = (uint8_t)(max_version & 0xFFu);
    payload[4] = (uint8_t)((max_version >> 8) & 0xFFu);

    /* The beacon header carries PV=0xFF, which apex_frame_encode() refuses by
     * the non-zero-prefix guard, so pack it directly here. */
    decoded[0] = hdr.protocol_version;
    decoded[1] = hdr.traffic_type;
    decoded[2] = hdr.device_id;
    decoded[3] = hdr.payload_length;
    memcpy(decoded + APEX_HEADER_LENGTH, payload, APEX_BEACON_PAYLOAD_LENGTH);
    decoded_len = APEX_HEADER_LENGTH + APEX_BEACON_PAYLOAD_LENGTH;

    crc = apex_crc16(decoded, decoded_len);
    decoded[decoded_len + 0] = (uint8_t)(crc & 0xFFu);
    decoded[decoded_len + 1] = (uint8_t)((crc >> 8) & 0xFFu);
    decoded_len += APEX_CRC_LENGTH;

    status = apex_cobs_encode(decoded, decoded_len, buf, cap - 1, &encoded_len);
    if (status != APEX_OK) return status;

    buf[encoded_len] = 0x00;
    if (out_len) *out_len = encoded_len + 1;
    return APEX_OK;
}

apex_status_t apex_beacon_validate(const apex_hdr_t *hdr,
                                   const uint8_t *payload,
                                   size_t payload_len,
                                   apex_beacon_t *out)
{
    uint16_t min_version, max_version;

    if (!hdr || !payload || !out) return APEX_ERR_INVALID_ARGS;

    if (hdr->protocol_version != APEX_PROTOCOL_VERSION_BEACON ||
        hdr->traffic_type != APEX_TRAFFIC_CONFIG ||
        hdr->device_id != APEX_DEVICE_ID_BEACON ||
        hdr->payload_length != APEX_BEACON_PAYLOAD_LENGTH ||
        payload_len != APEX_BEACON_PAYLOAD_LENGTH ||
        payload[0] != APEX_BEACON_MSG_ID) {
        return APEX_ERR_MALFORMED;
    }

    min_version = (uint16_t)(payload[1] | ((uint16_t)payload[2] << 8));
    max_version = (uint16_t)(payload[3] | ((uint16_t)payload[4] << 8));

    if (!beacon_version_valid(min_version) || !beacon_version_valid(max_version) ||
        min_version > max_version) {
        return APEX_ERR_MALFORMED;
    }

    out->min_version = min_version;
    out->max_version = max_version;
    return APEX_OK;
}

apex_status_t apex_beacon_parse(const uint8_t *encoded,
                                size_t len,
                                apex_beacon_t *out)
{
    uint8_t workspace[APEX_MAX_FRAME_LENGTH];
    apex_hdr_t hdr;
    const uint8_t *payload = NULL;
    size_t payload_len = 0;
    apex_status_t status;

    if (!encoded || !out) return APEX_ERR_INVALID_ARGS;

    status = apex_frame_decode(encoded, len, workspace, sizeof(workspace),
                               &hdr, &payload, &payload_len);
    if (status != APEX_OK) return status;

    return apex_beacon_validate(&hdr, payload, payload_len, out);
}

/* ---------------------------------------------------------------------------
 * Ingress state machine
 * ------------------------------------------------------------------------- */

void apex_framer_rx_init(apex_framer_rx_t *rx)
{
    if (!rx) return;
    memset(rx, 0, sizeof(*rx));
}

void apex_framer_set_beacon_cb(apex_framer_rx_t *rx,
                               apex_beacon_cb_t cb,
                               void *user)
{
    if (!rx) return;
    rx->on_beacon = cb;
    rx->beacon_user = user;
}

static void deliver_one(apex_framer_rx_t *rx, apex_frame_cb_t cb, void *user)
{
    apex_hdr_t hdr;
    const uint8_t *payload = NULL;
    size_t payload_len = 0;
    apex_status_t status;

    if (rx->cobs_len == 0) {
        /* A bare 0x00 delimiter with no preceding bytes — common at the start
         * of a stream. Ignore silently. */
        return;
    }

    /* §3.1.6 / §3.8 pre-decode gate: any encoded frame whose leading COBS code
     * byte is < 4 cannot be a legal v1 frame; drop before decoding. */
    if (rx->cobs_buf[0] < 4) {
        rx->frames_dropped_prefix++;
        return;
    }

    status = apex_frame_decode(rx->cobs_buf, rx->cobs_len,
                               rx->decoded_buf, sizeof(rx->decoded_buf),
                               &hdr, &payload, &payload_len);

    if (status != APEX_OK) {
        if (status == APEX_ERR_BAD_CRC) {
            rx->frames_dropped_crc++;
        } else if (status == APEX_ERR_MALFORMED) {
            rx->frames_dropped_length++;
        } else {
            rx->frames_dropped_cobs++;
        }
        return;
    }

    /* Post-decode protocol_version triage (§3.8). */
    if (hdr.protocol_version == APEX_PROTOCOL_VERSION_V0) {
        /* Legacy-v0 marker — illegal in v1; drop.
         *
         * HOOK: optional v0 dual-stack (§3.6.4). A transition-era host MAY
         * instead route this decoded frame to a v0 message handler here; that
         * stack is out of scope for the v1 core transport and is left
         * unimplemented deliberately. */
        rx->frames_dropped_version++;
        return;
    }

    if (hdr.protocol_version == APEX_PROTOCOL_VERSION_BEACON) {
        /* VERSION_BEACON (§3.6.2): distinct path from normal session frames. */
        apex_beacon_t beacon;
        if (apex_beacon_validate(&hdr, payload, payload_len, &beacon) == APEX_OK) {
            rx->frames_beacon++;
            if (rx->on_beacon) {
                rx->on_beacon(rx->beacon_user, beacon.min_version, beacon.max_version);
            }
        } else {
            rx->frames_dropped_length++;
        }
        return;
    }

    /* Session frame with a valid, non-zero session version (0x01–0xFE). Version
     * support policy and beacon emission for unsupported versions are
     * higher-layer concerns (§3.6.2, §3.8) — the transport delivers the frame. */
    rx->frames_ok++;
    if (cb) cb(user, &hdr, payload, payload_len);
}

void apex_framer_feed(apex_framer_rx_t *rx,
                      const uint8_t *bytes,
                      size_t n,
                      apex_frame_cb_t cb,
                      void *user)
{
    if (!rx || (!bytes && n > 0)) return;

    for (size_t i = 0; i < n; i++) {
        uint8_t b = bytes[i];
        if (b == 0x00) {
            /* End-of-frame delimiter. */
            if (!rx->overflowed) {
                deliver_one(rx, cb, user);
            } else {
                /* We dropped bytes mid-frame; treat as overflow drop. */
                rx->frames_dropped_length++;
            }
            rx->cobs_len = 0;
            rx->overflowed = 0;
        } else {
            if (rx->overflowed) continue;
            if (rx->cobs_len >= sizeof(rx->cobs_buf)) {
                /* Frame is longer than any legal APEX frame. Drop until the
                 * next 0x00 delimiter. */
                rx->overflowed = 1;
                rx->cobs_len = 0;
                continue;
            }
            rx->cobs_buf[rx->cobs_len++] = b;
        }
    }
}
