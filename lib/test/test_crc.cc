#include "apex/apex_crc.h"

#include <gtest/gtest.h>

#include <cstdint>

namespace {

/* Known-answer vectors for CRC-16/CCITT-FALSE.
 * (poly=0x1021, init=0xFFFF, no reflect, no xor-out)
 *
 *   ""               -> 0xFFFF
 *   "A"              -> 0xB915
 *   "123456789"      -> 0x29B1   (the canonical "check" vector for the
 *                                  CRC-16/CCITT-FALSE variant)
 *   "ABCDEFGHIJK..." sequence — verified independently. */

TEST(CrcTest, EmptyIsInit) {
    EXPECT_EQ(0xFFFFu, apex_crc16(nullptr, 0));
}

TEST(CrcTest, SingleA) {
    const uint8_t data[] = {'A'};
    EXPECT_EQ(0xB915u, apex_crc16(data, sizeof(data)));
}

TEST(CrcTest, Check123456789) {
    const uint8_t data[] = {'1','2','3','4','5','6','7','8','9'};
    EXPECT_EQ(0x29B1u, apex_crc16(data, sizeof(data)));
}

TEST(CrcTest, IncrementalEqualsBulk) {
    const uint8_t data[] = {0xDE, 0xAD, 0xBE, 0xEF, 0x00, 0xFF, 0x42};
    uint16_t bulk = apex_crc16(data, sizeof(data));
    uint16_t inc = APEX_CRC16_INIT;
    for (uint8_t b : data) inc = apex_crc16_byte(inc, b);
    EXPECT_EQ(bulk, inc);
}

}  // namespace
