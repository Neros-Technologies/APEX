/**
 * @file apex_framer.h
 * @brief Outer-header pack/unpack, full-frame encode/decode, and a byte-stream
 *        ingress state machine that emits one decoded frame per 0x00
 *        delimiter on the wire.
 *
 * The framer is transport-agnostic and stateless beyond the small RX context
 * struct: callers push bytes in with apex_framer_feed(), passing a callback
 * that receives each successfully decoded frame.
 *
 * Copyright (c) 2026 Neros Technologies. MIT License — see LICENSE.
 */
#ifndef APEX_FRAMER_H
#define APEX_FRAMER_H

#include <stddef.h>
#include <stdint.h>

#include "apex/apex_core.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------------------
 * Single-frame encode/decode (no I/O, pure functions)
 * ------------------------------------------------------------------------- */

/* Build a complete on-wire frame (outer header + payload + CRC + COBS + 0x00
 * delimiter) into `out`. `payload` is `payload_length` bytes; it may be NULL
 * iff `hdr->payload_length == 0` (heartbeat). `out` must hold at least
 * APEX_V0_MAX_ENCODED_FRAME_LENGTH bytes. The number of bytes actually
 * written is returned via *out_len. */
apex_status_t apex_frame_encode(const apex_v0_hdr_t *hdr,
                                const uint8_t *payload,
                                uint8_t *out,
                                size_t out_capacity,
                                size_t *out_len);

/* Decode one COBS-encoded frame (no leading or trailing 0x00 delimiter) and
 * verify its CRC. On success, *hdr_out is filled and *payload_out points into
 * a caller-supplied workspace `decoded_workspace` (size
 * APEX_V0_MAX_FRAME_LENGTH). *payload_len_out is filled from the header. */
apex_status_t apex_frame_decode(const uint8_t *cobs_bytes,
                                size_t cobs_len,
                                uint8_t *decoded_workspace,
                                size_t workspace_len,
                                apex_v0_hdr_t *hdr_out,
                                const uint8_t **payload_out,
                                size_t *payload_len_out);

/* ---------------------------------------------------------------------------
 * Byte-stream ingress
 * ------------------------------------------------------------------------- */

/* Per-frame delivery callback. `payload` is valid for the duration of the
 * callback only. */
typedef void (*apex_frame_cb_t)(void *user,
                                const apex_v0_hdr_t *hdr,
                                const uint8_t *payload,
                                size_t payload_len);

typedef struct {
    /* Encoded-byte ring for the current in-progress frame. We accumulate
     * until we see a 0x00 delimiter, then decode. */
    uint8_t cobs_buf[APEX_V0_MAX_ENCODED_FRAME_LENGTH];
    size_t cobs_len;
    /* Workspace for the decoded frame. Sized to APEX_V0_MAX_ENCODED_FRAME_LENGTH
     * rather than APEX_V0_MAX_FRAME_LENGTH so apex_cobs_decode's conservative
     * output-buffer check (output_length >= input_length) is always satisfied.
     * The actual decoded content is at most APEX_V0_MAX_FRAME_LENGTH bytes. */
    uint8_t decoded_buf[APEX_V0_MAX_ENCODED_FRAME_LENGTH];
    /* Sticky overflow flag: while set, we discard bytes until the next 0x00
     * delimiter so a too-long stretch of non-zero bytes can't corrupt a
     * subsequent frame. */
    uint8_t overflowed;
    /* RX statistics — useful for diagnostics, not load-bearing. */
    uint32_t frames_ok;
    uint32_t frames_dropped_cobs;
    uint32_t frames_dropped_crc;
    uint32_t frames_dropped_length;
    uint32_t frames_dropped_version;
} apex_framer_rx_t;

void apex_framer_rx_init(apex_framer_rx_t *rx);

/* Push received bytes through the framer. Calls `cb` synchronously for each
 * complete, CRC-validated frame. */
void apex_framer_feed(apex_framer_rx_t *rx,
                      const uint8_t *bytes,
                      size_t n,
                      apex_frame_cb_t cb,
                      void *user);

#ifdef __cplusplus
}
#endif

#endif /* APEX_FRAMER_H */
