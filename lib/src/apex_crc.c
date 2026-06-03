/**
 * @file apex_crc.c
 * @brief Bit-by-bit CRC-16/CCITT-FALSE. ~30 LOC; no lookup table.
 *
 * Copyright (c) 2026 Neros Technologies. MIT License — see LICENSE.
 */
#include "apex/apex_crc.h"

uint16_t apex_crc16_byte(uint16_t crc, uint8_t byte)
{
    crc ^= (uint16_t)byte << 8;
    for (int i = 0; i < 8; i++) {
        if (crc & 0x8000u) {
            crc = (uint16_t)((crc << 1) ^ 0x1021u);
        } else {
            crc = (uint16_t)(crc << 1);
        }
    }
    return crc;
}

uint16_t apex_crc16_update(uint16_t crc, const uint8_t *data, size_t length)
{
    for (size_t i = 0; i < length; i++) {
        crc = apex_crc16_byte(crc, data[i]);
    }
    return crc;
}

uint16_t apex_crc16(const uint8_t *data, size_t length)
{
    return apex_crc16_update(APEX_CRC16_INIT, data, length);
}
