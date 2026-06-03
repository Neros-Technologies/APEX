#include "apex/apex_framer.h"

#include <gtest/gtest.h>

#include <cstring>
#include <vector>

namespace {

struct Captured {
    apex_v0_hdr_t hdr;
    std::vector<uint8_t> payload;
};

static void on_frame_cb(void* user, const apex_v0_hdr_t* hdr,
                        const uint8_t* payload, size_t payload_len) {
    auto* vec = static_cast<std::vector<Captured>*>(user);
    Captured c;
    c.hdr = *hdr;
    if (payload && payload_len) c.payload.assign(payload, payload + payload_len);
    vec->push_back(std::move(c));
}

TEST(FramerTest, RoundTripBasic) {
    apex_v0_hdr_t hdr = {0, APEX_TRAFFIC_CONFIG, 0x01, 3};
    uint8_t payload[3] = {0xAA, 0x00, 0xCC};
    uint8_t encoded[APEX_V0_MAX_ENCODED_FRAME_LENGTH];
    size_t encoded_len = 0;
    ASSERT_EQ(APEX_OK, apex_frame_encode(&hdr, payload, encoded, sizeof(encoded), &encoded_len));
    ASSERT_GT(encoded_len, 0u);
    ASSERT_EQ(0x00, encoded[encoded_len - 1]);  // trailing delimiter

    std::vector<Captured> got;
    apex_framer_rx_t rx;
    apex_framer_rx_init(&rx);
    apex_framer_feed(&rx, encoded, encoded_len, on_frame_cb, &got);

    ASSERT_EQ(1u, got.size());
    EXPECT_EQ(0, got[0].hdr.protocol_version);
    EXPECT_EQ(APEX_TRAFFIC_CONFIG, got[0].hdr.traffic_type);
    EXPECT_EQ(0x01, got[0].hdr.device_id);
    EXPECT_EQ(3, got[0].hdr.payload_length);
    EXPECT_EQ(std::vector<uint8_t>({0xAA, 0x00, 0xCC}), got[0].payload);
    EXPECT_EQ(1u, rx.frames_ok);
}

TEST(FramerTest, EmptyHeartbeatFrame) {
    apex_v0_hdr_t hdr = {0, APEX_TRAFFIC_CONFIG, 0x01, 0};
    uint8_t encoded[APEX_V0_MAX_ENCODED_FRAME_LENGTH];
    size_t encoded_len = 0;
    ASSERT_EQ(APEX_OK, apex_frame_encode(&hdr, nullptr, encoded, sizeof(encoded), &encoded_len));

    std::vector<Captured> got;
    apex_framer_rx_t rx;
    apex_framer_rx_init(&rx);
    apex_framer_feed(&rx, encoded, encoded_len, on_frame_cb, &got);
    ASSERT_EQ(1u, got.size());
    EXPECT_EQ(0, got[0].hdr.payload_length);
    EXPECT_TRUE(got[0].payload.empty());
}

TEST(FramerTest, MaxSizeFrame) {
    apex_v0_hdr_t hdr = {0, APEX_TRAFFIC_ACTIVATION, 0x42, APEX_V0_MAX_PAYLOAD_LENGTH};
    uint8_t payload[APEX_V0_MAX_PAYLOAD_LENGTH];
    for (size_t i = 0; i < sizeof(payload); i++) payload[i] = (uint8_t)i;
    uint8_t encoded[APEX_V0_MAX_ENCODED_FRAME_LENGTH];
    size_t encoded_len = 0;
    ASSERT_EQ(APEX_OK, apex_frame_encode(&hdr, payload, encoded, sizeof(encoded), &encoded_len));
    EXPECT_LE(encoded_len, (size_t)APEX_V0_MAX_ENCODED_FRAME_LENGTH);

    std::vector<Captured> got;
    apex_framer_rx_t rx;
    apex_framer_rx_init(&rx);
    apex_framer_feed(&rx, encoded, encoded_len, on_frame_cb, &got);
    ASSERT_EQ(1u, got.size());
    EXPECT_EQ(0x42, got[0].hdr.device_id);
    EXPECT_EQ(APEX_V0_MAX_PAYLOAD_LENGTH, got[0].hdr.payload_length);
    ASSERT_EQ(APEX_V0_MAX_PAYLOAD_LENGTH, got[0].payload.size());
    for (size_t i = 0; i < got[0].payload.size(); i++) {
        EXPECT_EQ((uint8_t)i, got[0].payload[i]);
    }
}

TEST(FramerTest, CorruptedFrameDroppedByCrc) {
    apex_v0_hdr_t hdr = {0, APEX_TRAFFIC_CONFIG, 0x01, 2};
    uint8_t payload[2] = {0xAB, 0xCD};
    uint8_t encoded[APEX_V0_MAX_ENCODED_FRAME_LENGTH];
    size_t encoded_len = 0;
    ASSERT_EQ(APEX_OK, apex_frame_encode(&hdr, payload, encoded, sizeof(encoded), &encoded_len));
    // Flip a payload byte (not the delimiter, not the COBS leading byte).
    encoded[3] ^= 0x55;

    std::vector<Captured> got;
    apex_framer_rx_t rx;
    apex_framer_rx_init(&rx);
    apex_framer_feed(&rx, encoded, encoded_len, on_frame_cb, &got);
    EXPECT_TRUE(got.empty());
    EXPECT_EQ(1u, rx.frames_dropped_crc);
}

TEST(FramerTest, MultipleFramesInOnePush) {
    apex_v0_hdr_t h1 = {0, APEX_TRAFFIC_CONFIG, 0x01, 1};
    apex_v0_hdr_t h2 = {0, APEX_TRAFFIC_CONFIG, 0x02, 0};
    uint8_t p1[1] = {0x77};
    uint8_t buf[2 * APEX_V0_MAX_ENCODED_FRAME_LENGTH];
    size_t a = 0, b = 0;
    ASSERT_EQ(APEX_OK, apex_frame_encode(&h1, p1, buf, sizeof(buf), &a));
    ASSERT_EQ(APEX_OK, apex_frame_encode(&h2, nullptr, buf + a, sizeof(buf) - a, &b));

    std::vector<Captured> got;
    apex_framer_rx_t rx;
    apex_framer_rx_init(&rx);
    apex_framer_feed(&rx, buf, a + b, on_frame_cb, &got);
    ASSERT_EQ(2u, got.size());
    EXPECT_EQ(0x01, got[0].hdr.device_id);
    EXPECT_EQ(0x02, got[1].hdr.device_id);
}

TEST(FramerTest, ByteAtATimeIngress) {
    apex_v0_hdr_t hdr = {0, APEX_TRAFFIC_CONFIG, 0x09, 4};
    uint8_t payload[4] = {0xDE, 0xAD, 0xBE, 0xEF};
    uint8_t encoded[APEX_V0_MAX_ENCODED_FRAME_LENGTH];
    size_t encoded_len = 0;
    ASSERT_EQ(APEX_OK, apex_frame_encode(&hdr, payload, encoded, sizeof(encoded), &encoded_len));

    std::vector<Captured> got;
    apex_framer_rx_t rx;
    apex_framer_rx_init(&rx);
    for (size_t i = 0; i < encoded_len; i++) {
        apex_framer_feed(&rx, &encoded[i], 1, on_frame_cb, &got);
    }
    ASSERT_EQ(1u, got.size());
    EXPECT_EQ(std::vector<uint8_t>({0xDE, 0xAD, 0xBE, 0xEF}), got[0].payload);
}

TEST(FramerTest, OverflowRecoversAtNextDelimiter) {
    apex_framer_rx_t rx;
    apex_framer_rx_init(&rx);
    std::vector<Captured> got;

    // A run of non-zero bytes longer than any legal frame forces the framer
    // into overflow. A subsequent 0x00 clears the overflow flag; the frame
    // *after* that delimiter must decode normally.
    std::vector<uint8_t> garbage(APEX_V0_MAX_ENCODED_FRAME_LENGTH * 2, 0xAB);
    garbage.push_back(0x00);  // resync delimiter
    apex_framer_feed(&rx, garbage.data(), garbage.size(), on_frame_cb, &got);
    EXPECT_TRUE(got.empty());
    EXPECT_GE(rx.frames_dropped_length, 1u);

    apex_v0_hdr_t hdr = {0, APEX_TRAFFIC_CONFIG, 0x01, 1};
    uint8_t p[1] = {0x42};
    uint8_t encoded[APEX_V0_MAX_ENCODED_FRAME_LENGTH];
    size_t encoded_len = 0;
    ASSERT_EQ(APEX_OK, apex_frame_encode(&hdr, p, encoded, sizeof(encoded), &encoded_len));
    apex_framer_feed(&rx, encoded, encoded_len, on_frame_cb, &got);

    ASSERT_EQ(1u, got.size());
    EXPECT_EQ(0x42, got[0].payload[0]);
}

}  // namespace
