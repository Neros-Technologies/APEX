/**
 * @file apex_cobs.h
 * @brief COBS (Consistent Overhead Byte Stuffing) — the framing scheme APEX
 *        uses on the wire. See APEX_Core.md §3.
 *
 * Ported from neros_common/common/cobs (Joseph Murphy, 2026-04-29), with the
 * return-code surface swapped to apex_status_t so the library has no external
 * deps. Algorithm and tests are unchanged.
 *
 * Copyright (c) 2026 Neros Technologies. MIT License — see LICENSE.
 */
#ifndef APEX_COBS_H
#define APEX_COBS_H

#include <stddef.h>
#include <stdint.h>

#include "apex/apex_status.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Maximum COBS-encoded length of a payload of N bytes. */
#define APEX_COBS_MAX_ENCODED_LENGTH(n) ((n) + ((n) / 254) + 1)

/* Maximum length of an encoded payload plus the two 0x00 framing delimiters
 * (one leading, one trailing). Note: APEX uses only a trailing delimiter, so
 * apex_framer.[ch] does not call the "_framed" helpers below — they are kept
 * for parity with the upstream implementation. */
#define APEX_COBS_MAX_FRAMED_LENGTH(n) (APEX_COBS_MAX_ENCODED_LENGTH(n) + 2)

size_t apex_cobs_max_encoded_length(size_t unencoded_length);
size_t apex_cobs_max_framed_length(size_t unencoded_length);

/* Encode `length` bytes of `input` into `output`. Writes at most
 * APEX_COBS_MAX_ENCODED_LENGTH(length) bytes; sets *written to the actual
 * count on success. */
apex_status_t apex_cobs_encode(const uint8_t *input,
                               size_t length,
                               uint8_t *output,
                               size_t output_length,
                               size_t *written);

/* Decode `length` COBS bytes (no 0x00 delimiters) into `output`. */
apex_status_t apex_cobs_decode(const uint8_t *input,
                               size_t length,
                               uint8_t *output,
                               size_t output_length,
                               size_t *written);

/* Encode plus prepend/append 0x00 framing bytes. */
apex_status_t apex_cobs_encode_framed(const uint8_t *input,
                                      size_t length,
                                      uint8_t *output,
                                      size_t output_length,
                                      size_t *written);

/* Strip leading/trailing 0x00 framing bytes, then decode. */
apex_status_t apex_cobs_decode_framed(const uint8_t *input,
                                      size_t length,
                                      uint8_t *output,
                                      size_t output_length,
                                      size_t *written);

#ifdef __cplusplus
}
#endif

#endif /* APEX_COBS_H */
