/* Standard COBS test cases, with return codes expressed as apex_status_t.
 * Same algorithm under test, same expected outputs. */
#include "apex/apex_cobs.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

namespace {

class CobsTest : public ::testing::Test {
protected:
    void TestEncodeDecode(const std::vector<uint8_t>& unencoded,
                          const std::vector<uint8_t>& expected_encoded) {
        size_t max_encoded_len = apex_cobs_max_encoded_length(unencoded.size());
        EXPECT_GE(max_encoded_len, expected_encoded.size());

        std::vector<uint8_t> encoded(max_encoded_len);
        size_t encoded_len = 0;
        EXPECT_EQ(APEX_OK,
                  apex_cobs_encode(unencoded.data(), unencoded.size(),
                                   encoded.data(), encoded.size(), &encoded_len));
        EXPECT_EQ(encoded_len, expected_encoded.size());
        encoded.resize(encoded_len);
        EXPECT_EQ(encoded, expected_encoded);

        std::vector<uint8_t> decoded(unencoded.size() + 10);
        size_t decoded_len = 0;
        EXPECT_EQ(APEX_OK,
                  apex_cobs_decode(encoded.data(), encoded.size(),
                                   decoded.data(), decoded.size(), &decoded_len));
        EXPECT_EQ(decoded_len, unencoded.size());
        decoded.resize(decoded_len);
        EXPECT_EQ(decoded, unencoded);
    }
};

TEST_F(CobsTest, EmptyPayload) {
    TestEncodeDecode({}, {0x01});
}

TEST_F(CobsTest, SingleZero) {
    TestEncodeDecode({0x00}, {0x01, 0x01});
}

TEST_F(CobsTest, TwoZeros) {
    TestEncodeDecode({0x00, 0x00}, {0x01, 0x01, 0x01});
}

TEST_F(CobsTest, NoZeros) {
    TestEncodeDecode({0x11, 0x22, 0x33, 0x44}, {0x05, 0x11, 0x22, 0x33, 0x44});
}

TEST_F(CobsTest, ZerosAtEnds) {
    TestEncodeDecode({0x00, 0x11, 0x22, 0x00}, {0x01, 0x03, 0x11, 0x22, 0x01});
}

TEST_F(CobsTest, ZeroInMiddle) {
    TestEncodeDecode({0x11, 0x00, 0x22}, {0x02, 0x11, 0x02, 0x22});
}

TEST_F(CobsTest, BlockSize254) {
    std::vector<uint8_t> unencoded;
    for (int i = 1; i <= 254; ++i) unencoded.push_back(i);
    std::vector<uint8_t> encoded;
    encoded.push_back(0xFF);
    for (int i = 1; i <= 254; ++i) encoded.push_back(i);
    encoded.push_back(0x01);
    TestEncodeDecode(unencoded, encoded);
}

TEST_F(CobsTest, OutputBufferTooSmall) {
    std::vector<uint8_t> unencoded = {0x11, 0x22, 0x33};
    std::vector<uint8_t> encoded(3);
    size_t len = 0;
    EXPECT_EQ(APEX_ERR_BUFFER_TOO_SMALL,
              apex_cobs_encode(unencoded.data(), unencoded.size(),
                               encoded.data(), encoded.size(), &len));
}

TEST_F(CobsTest, DecodeMalformedZeroInPayload) {
    std::vector<uint8_t> encoded = {0x03, 0x11, 0x00};
    std::vector<uint8_t> decoded(10);
    size_t len = 0;
    EXPECT_EQ(APEX_ERR_MALFORMED,
              apex_cobs_decode(encoded.data(), encoded.size(),
                               decoded.data(), decoded.size(), &len));
}

TEST_F(CobsTest, DecodeMalformedUnexpectedEnd) {
    std::vector<uint8_t> encoded = {0x05, 0x11, 0x22};
    std::vector<uint8_t> decoded(10);
    size_t len = 0;
    EXPECT_EQ(APEX_ERR_MALFORMED,
              apex_cobs_decode(encoded.data(), encoded.size(),
                               decoded.data(), decoded.size(), &len));
}

}  // namespace
