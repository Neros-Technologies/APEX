/**
 * @file apex_framer.h
 * @brief Outer-header pack/unpack, full-frame encode/decode, a byte-stream
 *        ingress state machine that emits one decoded frame per 0x00 delimiter,
 *        the decode-free header-prefix peek (§3.1.6), and the VERSION_BEACON
 *        codec (§3.6.2).
 *
 * The framer is transport-agnostic and stateless beyond the small RX context
 * struct: callers push bytes in with apex_framer_feed(), passing a callback
 * that receives each successfully decoded session frame. VERSION_BEACON frames
 * (protocol_version 0xFF) are delivered on a separate beacon callback.
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
 * APEX_MAX_ENCODED_FRAME_LENGTH bytes. The number of bytes actually written is
 * returned via *out_len.
 *
 * The header prefix must be non-zero by construction (§3.1.1): a frame whose
 * protocol_version, traffic_type, or device_id is 0x00 is refused with
 * APEX_ERR_INVALID_ARGS. (protocol_version 0xFF for VERSION_BEACON is built via
 * apex_beacon_build(), not this function.) */
apex_status_t apex_frame_encode(const apex_hdr_t *hdr,
                                const uint8_t *payload,
                                uint8_t *out,
                                size_t out_capacity,
                                size_t *out_len);

/* Decode one COBS-encoded frame (no leading or trailing 0x00 delimiter) and
 * verify its CRC. On success, *hdr_out is filled and *payload_out points into
 * a caller-supplied workspace `decoded_workspace` (size APEX_MAX_FRAME_LENGTH).
 * *payload_len_out is filled from the header. */
apex_status_t apex_frame_decode(const uint8_t *cobs_bytes,
                                size_t cobs_len,
                                uint8_t *decoded_workspace,
                                size_t workspace_len,
                                apex_hdr_t *hdr_out,
                                const uint8_t **payload_out,
                                size_t *payload_len_out);

/* ---------------------------------------------------------------------------
 * Decode-free header-prefix peek — §3.1.6, §3.7.5
 * ------------------------------------------------------------------------- */

/* Read protocol_version / traffic_type / device_id straight off an *encoded*
 * frame without COBS-decoding it, exploiting the header-prefix transparency
 * property (§3.1.6): in every legal v1 frame the leading code byte is >= 4 and
 * encoded offsets 1,2,3 are PV,TT,ID verbatim.
 *
 * `encoded` points at the COBS block (the leading code byte), `len` is its
 * length excluding the trailing 0x00 delimiter. Applies the pre-decode sanity
 * gates (§3.7.5): leading code byte >= 4, encoded length >= 5, and non-zero
 * PV/TT/ID. On pass, fills out_prefix->{protocol_version,traffic_type,device_id}
 * (payload_length is left 0) and returns APEX_OK; otherwise APEX_ERR_MALFORMED.
 * A PV of 0xFF (VERSION_BEACON) passes the gates and is identifiable here. */
apex_status_t apex_framer_peek_prefix(const uint8_t *encoded,
                                      size_t len,
                                      apex_hdr_t *out_prefix);

/* ---------------------------------------------------------------------------
 * VERSION_BEACON codec — §3.2.12, §3.6.2
 * ------------------------------------------------------------------------- */

typedef struct {
    uint16_t min_version;
    uint16_t max_version;
} apex_beacon_t;

/* Build the full encoded on-wire VERSION_BEACON frame (COBS + trailing 0x00)
 * for the supported wire-version range [min_version, max_version] into `buf`
 * (capacity >= APEX_MAX_ENCODED_FRAME_LENGTH). Both versions must be in
 * 1..65534 and min <= max, else APEX_ERR_INVALID_ARGS. The written length is
 * returned via *out_len. */
apex_status_t apex_beacon_build(uint16_t min_version,
                                uint16_t max_version,
                                uint8_t *buf,
                                size_t cap,
                                size_t *out_len);

/* Validate an already-decoded frame as a VERSION_BEACON and extract its range.
 * Checks PV=0xFF, TT=CONFIG, ID=0xFF, payload_length=5, msg_id=12, and that
 * both versions are in 1..65534 with min <= max. Returns APEX_OK and fills
 * *out on success; APEX_ERR_MALFORMED otherwise. */
apex_status_t apex_beacon_validate(const apex_hdr_t *hdr,
                                   const uint8_t *payload,
                                   size_t payload_len,
                                   apex_beacon_t *out);

/* Parse a full encoded on-wire beacon frame (COBS block, no trailing 0x00),
 * CRC-checking and validating it, and extract its range. Convenience wrapper
 * around apex_frame_decode + apex_beacon_validate. */
apex_status_t apex_beacon_parse(const uint8_t *encoded,
                                size_t len,
                                apex_beacon_t *out);

/* ---------------------------------------------------------------------------
 * Byte-stream ingress
 * ------------------------------------------------------------------------- */

/* Per-frame delivery callback for normal session frames (PV 0x01–0xFE).
 * `payload` is valid for the duration of the callback only. */
typedef void (*apex_frame_cb_t)(void *user,
                                const apex_hdr_t *hdr,
                                const uint8_t *payload,
                                size_t payload_len);

/* VERSION_BEACON delivery callback (PV 0xFF). Delivers the validated wire-
 * version range named by the beacon (§3.6.2). */
typedef void (*apex_beacon_cb_t)(void *user,
                                 uint16_t min_version,
                                 uint16_t max_version);

typedef struct {
    /* Encoded-byte ring for the current in-progress frame. We accumulate
     * until we see a 0x00 delimiter, then decode. */
    uint8_t cobs_buf[APEX_MAX_ENCODED_FRAME_LENGTH];
    size_t cobs_len;
    /* Workspace for the decoded frame. Sized to APEX_MAX_ENCODED_FRAME_LENGTH
     * rather than APEX_MAX_FRAME_LENGTH so apex_cobs_decode's conservative
     * output-buffer check (output_length >= input_length) is always satisfied.
     * The actual decoded content is at most APEX_MAX_FRAME_LENGTH bytes. */
    uint8_t decoded_buf[APEX_MAX_ENCODED_FRAME_LENGTH];
    /* Sticky overflow flag: while set, we discard bytes until the next 0x00
     * delimiter so a too-long stretch of non-zero bytes can't corrupt a
     * subsequent frame. */
    uint8_t overflowed;
    /* Optional VERSION_BEACON sink (§3.6.2). Set via apex_framer_set_beacon_cb;
     * NULL means beacons are validated then dropped. */
    apex_beacon_cb_t on_beacon;
    void *beacon_user;
    /* RX statistics — useful for diagnostics, not load-bearing. */
    uint32_t frames_ok;
    uint32_t frames_beacon;
    uint32_t frames_dropped_cobs;
    uint32_t frames_dropped_crc;
    uint32_t frames_dropped_length;
    uint32_t frames_dropped_prefix;  /* Leading COBS code byte < 4 (§3.1.6). */
    uint32_t frames_dropped_version; /* protocol_version 0x00 (legacy v0). */
} apex_framer_rx_t;

void apex_framer_rx_init(apex_framer_rx_t *rx);

/* Register the VERSION_BEACON callback (§3.6.2). Optional; if never set,
 * beacons are still validated and counted but not delivered. */
void apex_framer_set_beacon_cb(apex_framer_rx_t *rx,
                               apex_beacon_cb_t cb,
                               void *user);

/* Push received bytes through the framer. Calls `cb` synchronously for each
 * complete, CRC-validated session frame (PV 0x01–0xFE), and the registered
 * beacon callback for each valid VERSION_BEACON (PV 0xFF). */
void apex_framer_feed(apex_framer_rx_t *rx,
                      const uint8_t *bytes,
                      size_t n,
                      apex_frame_cb_t cb,
                      void *user);

#ifdef __cplusplus
}
#endif

#endif /* APEX_FRAMER_H */
