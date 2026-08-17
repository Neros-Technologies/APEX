#include "apex/apex_framer.h"

#include <gtest/gtest.h>

#include <cstring>
#include <vector>

#include "apex/apex_cobs.h"
#include "apex/apex_crc.h"

namespace {

struct Captured {
    apex_hdr_t hdr;
    std::vector<uint8_t> payload;
};

static void on_frame_cb(void* user, const apex_hdr_t* hdr,
                        const uint8_t* payload, size_t payload_len) {
    auto* vec = static_cast<std::vector<Captured>*>(user);
    Captured c;
    c.hdr = *hdr;
    if (payload && payload_len) c.payload.assign(payload, payload + payload_len);
    vec->push_back(std::move(c));
}

TEST(FramerTest, RoundTripBasic) {
    apex_hdr_t hdr = {APEX_PROTOCOL_VERSION, APEX_TRAFFIC_CONFIG, 0x01, 3};
    uint8_t payload[3] = {0xAA, 0x00, 0xCC};
    uint8_t encoded[APEX_MAX_ENCODED_FRAME_LENGTH];
    size_t encoded_len = 0;
    ASSERT_EQ(APEX_OK, apex_frame_encode(&hdr, payload, encoded, sizeof(encoded), &encoded_len));
    ASSERT_GT(encoded_len, 0u);
    ASSERT_EQ(0x00, encoded[encoded_len - 1]);  // trailing delimiter

    std::vector<Captured> got;
    apex_framer_rx_t rx;
    apex_framer_rx_init(&rx);
    apex_framer_feed(&rx, encoded, encoded_len, on_frame_cb, &got);

    ASSERT_EQ(1u, got.size());
    EXPECT_EQ(APEX_PROTOCOL_VERSION, got[0].hdr.protocol_version);
    EXPECT_EQ(APEX_TRAFFIC_CONFIG, got[0].hdr.traffic_type);
    EXPECT_EQ(0x01, got[0].hdr.device_id);
    EXPECT_EQ(3, got[0].hdr.payload_length);
    EXPECT_EQ(std::vector<uint8_t>({0xAA, 0x00, 0xCC}), got[0].payload);
    EXPECT_EQ(1u, rx.frames_ok);
}

TEST(FramerTest, EmptyHeartbeatFrame) {
    apex_hdr_t hdr = {APEX_PROTOCOL_VERSION, APEX_TRAFFIC_CONFIG, 0x01, 0};
    uint8_t encoded[APEX_MAX_ENCODED_FRAME_LENGTH];
    size_t encoded_len = 0;
    ASSERT_EQ(APEX_OK, apex_frame_encode(&hdr, nullptr, encoded, sizeof(encoded), &encoded_len));
    // §3.1.6: heartbeat's leading COBS code byte is exactly 4.
    EXPECT_EQ(4, encoded[0]);

    std::vector<Captured> got;
    apex_framer_rx_t rx;
    apex_framer_rx_init(&rx);
    apex_framer_feed(&rx, encoded, encoded_len, on_frame_cb, &got);
    ASSERT_EQ(1u, got.size());
    EXPECT_EQ(0, got[0].hdr.payload_length);
    EXPECT_TRUE(got[0].payload.empty());
}

TEST(FramerTest, MaxSizeFrame) {
    apex_hdr_t hdr = {APEX_PROTOCOL_VERSION, APEX_TRAFFIC_ACTIVATION, 0x42,
                      APEX_MAX_PAYLOAD_LENGTH};
    uint8_t payload[APEX_MAX_PAYLOAD_LENGTH];
    for (size_t i = 0; i < sizeof(payload); i++) payload[i] = (uint8_t)i;
    uint8_t encoded[APEX_MAX_ENCODED_FRAME_LENGTH];
    size_t encoded_len = 0;
    ASSERT_EQ(APEX_OK, apex_frame_encode(&hdr, payload, encoded, sizeof(encoded), &encoded_len));
    EXPECT_LE(encoded_len, (size_t)APEX_MAX_ENCODED_FRAME_LENGTH);

    std::vector<Captured> got;
    apex_framer_rx_t rx;
    apex_framer_rx_init(&rx);
    apex_framer_feed(&rx, encoded, encoded_len, on_frame_cb, &got);
    ASSERT_EQ(1u, got.size());
    EXPECT_EQ(0x42, got[0].hdr.device_id);
    EXPECT_EQ(APEX_MAX_PAYLOAD_LENGTH, got[0].hdr.payload_length);
    ASSERT_EQ(APEX_MAX_PAYLOAD_LENGTH, got[0].payload.size());
    for (size_t i = 0; i < got[0].payload.size(); i++) {
        EXPECT_EQ((uint8_t)i, got[0].payload[i]);
    }
}

TEST(FramerTest, CorruptedFrameDroppedByCrc) {
    apex_hdr_t hdr = {APEX_PROTOCOL_VERSION, APEX_TRAFFIC_CONFIG, 0x01, 2};
    uint8_t payload[2] = {0xAB, 0xCD};
    uint8_t encoded[APEX_MAX_ENCODED_FRAME_LENGTH];
    size_t encoded_len = 0;
    ASSERT_EQ(APEX_OK, apex_frame_encode(&hdr, payload, encoded, sizeof(encoded), &encoded_len));
    // Flip a payload byte (not the delimiter, not the COBS leading byte).
    encoded[6] ^= 0x55;

    std::vector<Captured> got;
    apex_framer_rx_t rx;
    apex_framer_rx_init(&rx);
    apex_framer_feed(&rx, encoded, encoded_len, on_frame_cb, &got);
    EXPECT_TRUE(got.empty());
    EXPECT_EQ(1u, rx.frames_dropped_crc);
}

TEST(FramerTest, MultipleFramesInOnePush) {
    apex_hdr_t h1 = {APEX_PROTOCOL_VERSION, APEX_TRAFFIC_CONFIG, 0x01, 1};
    apex_hdr_t h2 = {APEX_PROTOCOL_VERSION, APEX_TRAFFIC_CONFIG, 0x02, 0};
    uint8_t p1[1] = {0x77};
    uint8_t buf[2 * APEX_MAX_ENCODED_FRAME_LENGTH];
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
    apex_hdr_t hdr = {APEX_PROTOCOL_VERSION, APEX_TRAFFIC_CONFIG, 0x09, 4};
    uint8_t payload[4] = {0xDE, 0xAD, 0xBE, 0xEF};
    uint8_t encoded[APEX_MAX_ENCODED_FRAME_LENGTH];
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
    std::vector<uint8_t> garbage(APEX_MAX_ENCODED_FRAME_LENGTH * 2, 0xAB);
    garbage.push_back(0x00);  // resync delimiter
    apex_framer_feed(&rx, garbage.data(), garbage.size(), on_frame_cb, &got);
    EXPECT_TRUE(got.empty());
    EXPECT_GE(rx.frames_dropped_length, 1u);

    apex_hdr_t hdr = {APEX_PROTOCOL_VERSION, APEX_TRAFFIC_CONFIG, 0x01, 1};
    uint8_t p[1] = {0x42};
    uint8_t encoded[APEX_MAX_ENCODED_FRAME_LENGTH];
    size_t encoded_len = 0;
    ASSERT_EQ(APEX_OK, apex_frame_encode(&hdr, p, encoded, sizeof(encoded), &encoded_len));
    apex_framer_feed(&rx, encoded, encoded_len, on_frame_cb, &got);

    ASSERT_EQ(1u, got.size());
    EXPECT_EQ(0x42, got[0].payload[0]);
}

/* ------------------------------------------------------------------------- */
/* v1 TX non-zero-header guard — §3.1.1                                       */
/* ------------------------------------------------------------------------- */

TEST(FramerTest, EncodeRefusesZeroProtocolVersion) {
    apex_hdr_t hdr = {APEX_PROTOCOL_VERSION_V0, APEX_TRAFFIC_CONFIG, 0x01, 0};
    uint8_t encoded[APEX_MAX_ENCODED_FRAME_LENGTH];
    size_t encoded_len = 0;
    EXPECT_EQ(APEX_ERR_INVALID_ARGS,
              apex_frame_encode(&hdr, nullptr, encoded, sizeof(encoded), &encoded_len));
}

TEST(FramerTest, EncodeRefusesZeroTrafficType) {
    apex_hdr_t hdr = {APEX_PROTOCOL_VERSION, 0x00, 0x01, 0};
    uint8_t encoded[APEX_MAX_ENCODED_FRAME_LENGTH];
    size_t encoded_len = 0;
    EXPECT_EQ(APEX_ERR_INVALID_ARGS,
              apex_frame_encode(&hdr, nullptr, encoded, sizeof(encoded), &encoded_len));
}

TEST(FramerTest, EncodeRefusesZeroDeviceId) {
    apex_hdr_t hdr = {APEX_PROTOCOL_VERSION, APEX_TRAFFIC_CONFIG, 0x00, 0};
    uint8_t encoded[APEX_MAX_ENCODED_FRAME_LENGTH];
    size_t encoded_len = 0;
    EXPECT_EQ(APEX_ERR_INVALID_ARGS,
              apex_frame_encode(&hdr, nullptr, encoded, sizeof(encoded), &encoded_len));
}

/* ------------------------------------------------------------------------- */
/* RX pre-decode / version gates — §3.8                                       */
/* ------------------------------------------------------------------------- */

TEST(FramerTest, DropsLeadingCodeByteBelowFour) {
    apex_framer_rx_t rx;
    apex_framer_rx_init(&rx);
    std::vector<Captured> got;
    // Leading code byte 3 (< 4) cannot be a legal v1 frame; dropped pre-decode.
    uint8_t frame[] = {0x03, 0x11, 0x22, 0x00};
    apex_framer_feed(&rx, frame, sizeof(frame), on_frame_cb, &got);
    EXPECT_TRUE(got.empty());
    EXPECT_EQ(1u, rx.frames_dropped_prefix);
}

TEST(FramerTest, DropsLegacyV0Frame) {
    // Assemble a CRC-valid decoded frame whose PV byte is 0x00 (v0 marker) and
    // COBS-encode it. Because PV=0x00 sits at decoded index 0, its leading COBS
    // code byte is 1 (< 4), so a genuine v0 frame is dropped by the pre-decode
    // §3.1.6 gate (prefix counter) and never reaches the decoder. The framer is
    // v1-only; the post-decode PV=0x00 branch is the documented optional v0
    // dual-stack hook (§3.6.4).
    uint8_t decoded[APEX_HEADER_LENGTH + 2 + APEX_CRC_LENGTH];
    decoded[0] = APEX_PROTOCOL_VERSION_V0;  // v0 marker
    decoded[1] = APEX_TRAFFIC_CONFIG;
    decoded[2] = 0x05;
    decoded[3] = 2;
    decoded[4] = 0x10;
    decoded[5] = 0x20;
    uint16_t crc = apex_crc16(decoded, 6);
    decoded[6] = (uint8_t)(crc & 0xFF);
    decoded[7] = (uint8_t)(crc >> 8);
    uint8_t encoded[APEX_MAX_ENCODED_FRAME_LENGTH];
    size_t enc_len = 0;
    ASSERT_EQ(APEX_OK, apex_cobs_encode(decoded, sizeof(decoded), encoded,
                                        sizeof(encoded) - 1, &enc_len));
    ASSERT_LT(encoded[0], 4);  // PV=0 forces a leading code byte < 4
    encoded[enc_len++] = 0x00;

    apex_framer_rx_t rx;
    apex_framer_rx_init(&rx);
    std::vector<Captured> got;
    apex_framer_feed(&rx, encoded, enc_len, on_frame_cb, &got);
    EXPECT_TRUE(got.empty());
    EXPECT_EQ(1u, rx.frames_dropped_prefix);
    EXPECT_EQ(0u, rx.frames_ok);
}

/* ------------------------------------------------------------------------- */
/* Decode-free header-prefix peek — §3.1.6                                    */
/* ------------------------------------------------------------------------- */

TEST(FramerTest, PeekPrefixReadsRoutingBytes) {
    apex_hdr_t hdr = {APEX_PROTOCOL_VERSION, APEX_TRAFFIC_REPEATER, 0x33, 4};
    uint8_t payload[4] = {0x00, 0x01, 0x00, 0x02};  // contains 0x00 bytes
    uint8_t encoded[APEX_MAX_ENCODED_FRAME_LENGTH];
    size_t encoded_len = 0;
    ASSERT_EQ(APEX_OK, apex_frame_encode(&hdr, payload, encoded, sizeof(encoded), &encoded_len));

    apex_hdr_t peek;
    // encoded_len includes the trailing 0x00; peek wants the COBS block length.
    ASSERT_EQ(APEX_OK, apex_framer_peek_prefix(encoded, encoded_len - 1, &peek));
    EXPECT_EQ(APEX_PROTOCOL_VERSION, peek.protocol_version);
    EXPECT_EQ(APEX_TRAFFIC_REPEATER, peek.traffic_type);
    EXPECT_EQ(0x33, peek.device_id);
}

TEST(FramerTest, PeekPrefixGatesReject) {
    apex_hdr_t peek;
    // Leading code byte < 4.
    uint8_t bad_code[] = {0x03, 0x01, 0x01, 0x01, 0x99};
    EXPECT_EQ(APEX_ERR_MALFORMED, apex_framer_peek_prefix(bad_code, sizeof(bad_code), &peek));
    // Too short.
    uint8_t too_short[] = {0x05, 0x01, 0x01, 0x01};
    EXPECT_EQ(APEX_ERR_MALFORMED, apex_framer_peek_prefix(too_short, sizeof(too_short), &peek));
    // PV = 0x00 (v0).
    uint8_t v0[] = {0x05, 0x00, 0x01, 0x01, 0x99};
    EXPECT_EQ(APEX_ERR_MALFORMED, apex_framer_peek_prefix(v0, sizeof(v0), &peek));
}

/* ------------------------------------------------------------------------- */
/* Property: header-prefix transparency across a corpus — §3.1.6              */
/* ------------------------------------------------------------------------- */

TEST(FramerTest, HeaderPrefixTransparencyProperty) {
    struct Case { uint8_t pv, tt, id, len; };
    const std::vector<Case> corpus = {
        {APEX_PROTOCOL_VERSION, APEX_TRAFFIC_CONFIG,     0x01, 0},    // heartbeat
        {APEX_PROTOCOL_VERSION, APEX_TRAFFIC_ACTIVATION, 0x02, 1},
        {APEX_PROTOCOL_VERSION, APEX_TRAFFIC_ANALOG_HMI, 0x7F, 5},
        {APEX_PROTOCOL_VERSION, APEX_TRAFFIC_REPEATER,   0xFE, 32},
        {0xFE /*max session ver*/, APEX_TRAFFIC_MAVLINK, 0xFF, 200},
        {APEX_PROTOCOL_VERSION, APEX_TRAFFIC_WAYFINDING, 0x10,
         (uint8_t)APEX_MAX_PAYLOAD_LENGTH},                          // max length
    };

    for (const auto& c : corpus) {
        apex_hdr_t hdr = {c.pv, c.tt, c.id, c.len};
        std::vector<uint8_t> payload(c.len);
        // Fill with a pattern that deliberately includes 0x00 bytes.
        for (size_t i = 0; i < payload.size(); i++)
            payload[i] = (i % 3 == 0) ? 0x00 : (uint8_t)(i + 1);

        uint8_t encoded[APEX_MAX_ENCODED_FRAME_LENGTH];
        size_t encoded_len = 0;
        ASSERT_EQ(APEX_OK, apex_frame_encode(&hdr, c.len ? payload.data() : nullptr,
                                             encoded, sizeof(encoded), &encoded_len))
            << "tt=" << (int)c.tt << " len=" << (int)c.len;

        // Leading COBS code byte is >= 4.
        EXPECT_GE(encoded[0], 4) << "tt=" << (int)c.tt;
        // Encoded bytes 1,2,3 are PV/TT/ID verbatim.
        EXPECT_EQ(c.pv, encoded[1]);
        EXPECT_EQ(c.tt, encoded[2]);
        EXPECT_EQ(c.id, encoded[3]);

        // The peek helper agrees.
        apex_hdr_t peek;
        ASSERT_EQ(APEX_OK, apex_framer_peek_prefix(encoded, encoded_len - 1, &peek));
        EXPECT_EQ(c.pv, peek.protocol_version);
        EXPECT_EQ(c.tt, peek.traffic_type);
        EXPECT_EQ(c.id, peek.device_id);
    }
}

}  // namespace
