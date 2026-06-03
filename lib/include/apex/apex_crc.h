/**
 * @file apex_crc.h
 * @brief CRC-16/CCITT-FALSE — the CRC APEX uses on each decoded frame.
 *
 * Algorithm parameters per APEX_Core.md §3.1.2:
 *   poly = 0x1021, init = 0xFFFF, no input reflection, no output reflection,
 *   final XOR = 0x0000.
 *
 * The CRC is computed over the outer header and inner payload, in transmission
 * order, prior to COBS encoding.
 *
 * Copyright (c) 2026 Neros Technologies. MIT License — see LICENSE.
 */
#ifndef APEX_CRC_H
#define APEX_CRC_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define APEX_CRC16_INIT 0xFFFFu

/* Update an in-progress CRC with one byte. Pass APEX_CRC16_INIT for the first
 * call. */
uint16_t apex_crc16_byte(uint16_t crc, uint8_t byte);

/* Update an in-progress CRC with `length` bytes. */
uint16_t apex_crc16_update(uint16_t crc, const uint8_t *data, size_t length);

/* Compute the CRC of `data[0..length)`. Equivalent to:
 *   apex_crc16_update(APEX_CRC16_INIT, data, length). */
uint16_t apex_crc16(const uint8_t *data, size_t length);

#ifdef __cplusplus
}
#endif

#endif /* APEX_CRC_H */
