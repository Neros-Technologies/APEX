/**
 * @file apex_framer.c
 * @brief Outer-header pack/unpack and a 0x00-delimited byte-stream framer.
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

apex_status_t apex_frame_encode(const apex_v0_hdr_t *hdr,
                                const uint8_t *payload,
                                uint8_t *out,
                                size_t out_capacity,
                                size_t *out_len)
{
    uint8_t decoded[APEX_V0_MAX_FRAME_LENGTH];
    size_t decoded_len;
    uint16_t crc;
    size_t encoded_len;
    apex_status_t status;

    if (out_len) *out_len = 0;
    if (!hdr || !out) return APEX_ERR_INVALID_ARGS;
    if (hdr->payload_length > 0 && !payload) return APEX_ERR_INVALID_ARGS;
    if (out_capacity < APEX_V0_MAX_ENCODED_FRAME_LENGTH) return APEX_ERR_BUFFER_TOO_SMALL;

    /* Pack header (little-endian — see §3.1). All header fields are single
     * bytes so endianness is trivial; we still write byte-by-byte to stay
     * portable. */
    decoded[0] = hdr->protocol_version;
    decoded[1] = hdr->traffic_type;
    decoded[2] = hdr->device_id;
    decoded[3] = hdr->payload_length;

    if (hdr->payload_length > 0) {
        memcpy(decoded + APEX_V0_HEADER_LENGTH, payload, hdr->payload_length);
    }
    decoded_len = APEX_V0_HEADER_LENGTH + hdr->payload_length;

    /* CRC over header + payload, in TX order, before COBS (§3.1.2). */
    crc = apex_crc16(decoded, decoded_len);
    decoded[decoded_len + 0] = (uint8_t)(crc & 0xFFu);
    decoded[decoded_len + 1] = (uint8_t)((crc >> 8) & 0xFFu);
    decoded_len += APEX_V0_CRC_LENGTH;

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
                                apex_v0_hdr_t *hdr_out,
                                const uint8_t **payload_out,
                                size_t *payload_len_out)
{
    size_t decoded_len;
    apex_status_t status;
    uint16_t expected_crc, received_crc;
    size_t payload_len;

    if (!cobs_bytes || !decoded_workspace || !hdr_out) return APEX_ERR_INVALID_ARGS;
    if (workspace_len < APEX_V0_MAX_FRAME_LENGTH) return APEX_ERR_BUFFER_TOO_SMALL;

    status = apex_cobs_decode(cobs_bytes, cobs_len,
                              decoded_workspace, workspace_len,
                              &decoded_len);
    if (status != APEX_OK) return status;

    /* Minimum legal frame: header + CRC, payload_length=0 (heartbeat). */
    if (decoded_len < APEX_V0_HEADER_LENGTH + APEX_V0_CRC_LENGTH) {
        return APEX_ERR_MALFORMED;
    }

    hdr_out->protocol_version = decoded_workspace[0];
    hdr_out->traffic_type     = decoded_workspace[1];
    hdr_out->device_id        = decoded_workspace[2];
    hdr_out->payload_length   = decoded_workspace[3];

    payload_len = hdr_out->payload_length;
    if (decoded_len != APEX_V0_HEADER_LENGTH + payload_len + APEX_V0_CRC_LENGTH) {
        /* payload_length doesn't match the actual decoded length — corrupted
         * header or truncated frame. */
        return APEX_ERR_MALFORMED;
    }

    /* Verify CRC. */
    received_crc =
        (uint16_t)decoded_workspace[APEX_V0_HEADER_LENGTH + payload_len] |
        ((uint16_t)decoded_workspace[APEX_V0_HEADER_LENGTH + payload_len + 1] << 8);
    expected_crc = apex_crc16(decoded_workspace, APEX_V0_HEADER_LENGTH + payload_len);
    if (received_crc != expected_crc) return APEX_ERR_BAD_CRC;

    if (payload_out) {
        *payload_out = (payload_len > 0)
            ? decoded_workspace + APEX_V0_HEADER_LENGTH
            : NULL;
    }
    if (payload_len_out) *payload_len_out = payload_len;
    return APEX_OK;
}

/* ---------------------------------------------------------------------------
 * Ingress state machine
 * ------------------------------------------------------------------------- */

void apex_framer_rx_init(apex_framer_rx_t *rx)
{
    if (!rx) return;
    memset(rx, 0, sizeof(*rx));
}

static void deliver_one(apex_framer_rx_t *rx, apex_frame_cb_t cb, void *user)
{
    apex_v0_hdr_t hdr;
    const uint8_t *payload = NULL;
    size_t payload_len = 0;
    apex_status_t status;

    if (rx->cobs_len == 0) {
        /* A bare 0x00 delimiter with no preceding bytes — common at the start
         * of a stream. Ignore silently. */
        return;
    }

    status = apex_frame_decode(rx->cobs_buf, rx->cobs_len,
                               rx->decoded_buf, sizeof(rx->decoded_buf),
                               &hdr, &payload, &payload_len);

    if (status == APEX_OK) {
        if (hdr.protocol_version != APEX_V0_PROTOCOL_VERSION) {
            /* §3.6: drop unknown-version frames. (For DEVICE_INFO the host
             * must reply ACK_REJECT_VERSION; that lives in apex_host.c.) */
            rx->frames_dropped_version++;
        } else {
            rx->frames_ok++;
            if (cb) cb(user, &hdr, payload, payload_len);
        }
    } else if (status == APEX_ERR_BAD_CRC) {
        rx->frames_dropped_crc++;
    } else if (status == APEX_ERR_MALFORMED) {
        rx->frames_dropped_length++;
    } else {
        rx->frames_dropped_cobs++;
    }
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
                /* Frame is longer than any legal APEX V0 frame. Drop until the
                 * next 0x00 delimiter. */
                rx->overflowed = 1;
                rx->cobs_len = 0;
                continue;
            }
            rx->cobs_buf[rx->cobs_len++] = b;
        }
    }
}
