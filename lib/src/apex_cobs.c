/**
 * @file apex_cobs.c
 * @brief COBS encode/decode — a standard Consistent Overhead Byte Stuffing
 *        implementation.
 *
 * Copyright (c) 2026 Neros Technologies. MIT License — see LICENSE.
 */
#include "apex/apex_cobs.h"

size_t apex_cobs_max_encoded_length(size_t unencoded_length)
{
    return unencoded_length + (unencoded_length / 254) + 1;
}

size_t apex_cobs_max_framed_length(size_t unencoded_length)
{
    return apex_cobs_max_encoded_length(unencoded_length) + 2;
}

apex_status_t apex_cobs_encode(const uint8_t *input,
                               size_t length,
                               uint8_t *output,
                               size_t output_length,
                               size_t *written)
{
    size_t read_index = 0;
    size_t write_index = 1;
    size_t code_index = 0;
    uint8_t code = 1;

    if (written) *written = 0;

    if (output_length < APEX_COBS_MAX_ENCODED_LENGTH(length)) {
        return APEX_ERR_BUFFER_TOO_SMALL;
    }

    while (read_index < length) {
        if (input[read_index] == 0) {
            output[code_index] = code;
            code = 1;
            code_index = write_index++;
            read_index++;
        } else {
            output[write_index++] = input[read_index++];
            code++;
            if (code == 0xFF) {
                output[code_index] = code;
                code = 1;
                code_index = write_index++;
            }
        }
    }
    output[code_index] = code;
    if (written) *written = write_index;
    return APEX_OK;
}

apex_status_t apex_cobs_decode(const uint8_t *input,
                               size_t length,
                               uint8_t *output,
                               size_t output_length,
                               size_t *written)
{
    size_t read_index = 0;
    size_t write_index = 0;
    uint8_t code;
    uint8_t i;

    if (written) *written = 0;

    if (length == 0) return APEX_ERR_INVALID_ARGS;

    if (output_length < length) {
        return APEX_ERR_BUFFER_TOO_SMALL;
    }

    while (read_index < length) {
        code = input[read_index];

        if (code == 0) {
            return APEX_ERR_MALFORMED;
        }

        if (read_index + code > length && code != 1) {
            return APEX_ERR_MALFORMED;
        }

        read_index++;

        for (i = 1; i < code; i++) {
            if (write_index >= output_length) return APEX_ERR_BUFFER_TOO_SMALL;
            if (read_index >= length) return APEX_ERR_MALFORMED;
            if (input[read_index] == 0) return APEX_ERR_MALFORMED;
            output[write_index++] = input[read_index++];
        }

        if (code < 0xFF && read_index < length) {
            if (write_index >= output_length) return APEX_ERR_BUFFER_TOO_SMALL;
            output[write_index++] = 0;
        }
    }

    if (written) *written = write_index;
    return APEX_OK;
}

apex_status_t apex_cobs_encode_framed(const uint8_t *input,
                                      size_t length,
                                      uint8_t *output,
                                      size_t output_length,
                                      size_t *written)
{
    size_t encoded_length;
    apex_status_t status;

    if (written) *written = 0;

    if (output_length < APEX_COBS_MAX_FRAMED_LENGTH(length)) {
        return APEX_ERR_BUFFER_TOO_SMALL;
    }

    output[0] = 0x00;

    status = apex_cobs_encode(input,
                              length,
                              output + 1,
                              output_length - 2,
                              &encoded_length);

    if (status != APEX_OK) {
        return status;
    }

    output[encoded_length + 1] = 0x00;
    if (written) *written = encoded_length + 2;
    return APEX_OK;
}

apex_status_t apex_cobs_decode_framed(const uint8_t *input,
                                      size_t length,
                                      uint8_t *output,
                                      size_t output_length,
                                      size_t *written)
{
    size_t i, j;
    int32_t difference;

    if (written) *written = 0;

    if (length < 2) {
        return APEX_ERR_INVALID_ARGS;
    }

    if (output_length < length) {
        return APEX_ERR_BUFFER_TOO_SMALL;
    }

    i = 0;
    while (i < length && input[i] == 0x00) {
        i++;
    }
    if (length - i <= 1) {
        return APEX_ERR_MALFORMED;
    }

    j = length - 1;
    while (input[j] == 0x00 && j > i) {
        j--;
    }
    difference = (int32_t)j - (int32_t)i;
    if (difference < 0) {
        return APEX_ERR_MALFORMED;
    }
    return apex_cobs_decode(input + i, j - i + 1, output, output_length, written);
}
