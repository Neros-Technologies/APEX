/* VERSION_BEACON codec tests — APEX_Core.md §3.2.12, §3.6.2. */
#include "apex/apex_framer.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

#include "apex/apex_cobs.h"
#include "apex/apex_crc.h"

namespace {

/* Assemble a decoded beacon-shaped frame [PV TT ID LN | payload...] + CRC,
 * COBS-encode it (with trailing 0x00), and return the on-wire bytes. Lets a
 * test forge malformed beacons the public build path would refuse. */
static std::vector<uint8_t> EncodeRaw(uint8_t pv, uint8_t tt, uint8_t id,
                                      const std::vector<uint8_t>& payload) {
    std::vector<uint8_t> decoded;
    decoded.push_back(pv);
    decoded.push_back(tt);
    decoded.push_back(id);
    decoded.push_back((uint8_t)payload.size());
    decoded.insert(decoded.end(), payload.begin(), payload.end());
    uint16_t crc = apex_crc16(decoded.data(), decoded.size());
    decoded.push_back((uint8_t)(crc & 0xFF));
    decoded.push_back((uint8_t)(crc >> 8));

    std::vector<uint8_t> encoded(APEX_MAX_ENCODED_FRAME_LENGTH);
    size_t enc_len = 0;
    EXPECT_EQ(APEX_OK, apex_cobs_encode(decoded.data(), decoded.size(),
                                        encoded.data(), encoded.size() - 1, &enc_len));
    encoded[enc_len++] = 0x00;
    encoded.resize(enc_len);
    return encoded;
}

/* Standard beacon inner payload: msg_id=12 + u16 LE min + u16 LE max. */
static std::vector<uint8_t> BeaconPayload(uint16_t mn, uint16_t mx) {
    return {APEX_BEACON_MSG_ID,
            (uint8_t)(mn & 0xFF), (uint8_t)(mn >> 8),
            (uint8_t)(mx & 0xFF), (uint8_t)(mx >> 8)};
}

TEST(BeaconTest, BuildParseRoundTrip) {
    uint8_t buf[APEX_MAX_ENCODED_FRAME_LENGTH];
    size_t len = 0;
    ASSERT_EQ(APEX_OK, apex_beacon_build(1, 7, buf, sizeof(buf), &len));
    ASSERT_GT(len, 0u);
    EXPECT_EQ(0x00, buf[len - 1]);  // trailing delimiter

    // Header prefix is visible verbatim / decode-free (§3.1.6).
    apex_hdr_t peek;
    ASSERT_EQ(APEX_OK, apex_framer_peek_prefix(buf, len - 1, &peek));
    EXPECT_EQ(APEX_PROTOCOL_VERSION_BEACON, peek.protocol_version);
    EXPECT_EQ(APEX_TRAFFIC_CONFIG, peek.traffic_type);
    EXPECT_EQ(APEX_DEVICE_ID_BEACON, peek.device_id);

    apex_beacon_t out;
    ASSERT_EQ(APEX_OK, apex_beacon_parse(buf, len - 1, &out));
    EXPECT_EQ(1u, out.min_version);
    EXPECT_EQ(7u, out.max_version);
}

TEST(BeaconTest, EdgeVersionValues) {
    uint8_t buf[APEX_MAX_ENCODED_FRAME_LENGTH];
    size_t len = 0;
    ASSERT_EQ(APEX_OK, apex_beacon_build(1, 65534, buf, sizeof(buf), &len));
    apex_beacon_t out;
    ASSERT_EQ(APEX_OK, apex_beacon_parse(buf, len - 1, &out));
    EXPECT_EQ(1u, out.min_version);
    EXPECT_EQ(65534u, out.max_version);

    // min == max is a legal single-version range.
    ASSERT_EQ(APEX_OK, apex_beacon_build(42, 42, buf, sizeof(buf), &len));
    ASSERT_EQ(APEX_OK, apex_beacon_parse(buf, len - 1, &out));
    EXPECT_EQ(42u, out.min_version);
    EXPECT_EQ(42u, out.max_version);
}

TEST(BeaconTest, BuildRejectsInvalidRange) {
    uint8_t buf[APEX_MAX_ENCODED_FRAME_LENGTH];
    size_t len = 0;
    EXPECT_EQ(APEX_ERR_INVALID_ARGS, apex_beacon_build(0, 7, buf, sizeof(buf), &len));      // min 0
    EXPECT_EQ(APEX_ERR_INVALID_ARGS, apex_beacon_build(1, 0xFFFF, buf, sizeof(buf), &len)); // max 0xFFFF
    EXPECT_EQ(APEX_ERR_INVALID_ARGS, apex_beacon_build(8, 7, buf, sizeof(buf), &len));      // min > max
    EXPECT_EQ(APEX_ERR_BUFFER_TOO_SMALL, apex_beacon_build(1, 7, buf, 4, &len));            // tiny buffer
}

TEST(BeaconTest, ParseRejectsWrongMsgId) {
    auto bad = BeaconPayload(1, 7);
    bad[0] = 0x0B;  // not VERSION_BEACON (12)
    auto enc = EncodeRaw(APEX_PROTOCOL_VERSION_BEACON, APEX_TRAFFIC_CONFIG,
                         APEX_DEVICE_ID_BEACON, bad);
    apex_beacon_t out;
    EXPECT_EQ(APEX_ERR_MALFORMED, apex_beacon_parse(enc.data(), enc.size() - 1, &out));
}

TEST(BeaconTest, ParseRejectsBadLength) {
    // Payload length 4 instead of 5 (truncated inner payload).
    std::vector<uint8_t> shorty = {APEX_BEACON_MSG_ID, 0x01, 0x00, 0x07};
    auto enc = EncodeRaw(APEX_PROTOCOL_VERSION_BEACON, APEX_TRAFFIC_CONFIG,
                         APEX_DEVICE_ID_BEACON, shorty);
    apex_beacon_t out;
    EXPECT_EQ(APEX_ERR_MALFORMED, apex_beacon_parse(enc.data(), enc.size() - 1, &out));

    // Payload length 6 (extra trailing byte).
    std::vector<uint8_t> longy = BeaconPayload(1, 7);
    longy.push_back(0xAA);
    auto enc2 = EncodeRaw(APEX_PROTOCOL_VERSION_BEACON, APEX_TRAFFIC_CONFIG,
                          APEX_DEVICE_ID_BEACON, longy);
    EXPECT_EQ(APEX_ERR_MALFORMED, apex_beacon_parse(enc2.data(), enc2.size() - 1, &out));
}

TEST(BeaconTest, ParseRejectsReservedVersions) {
    apex_beacon_t out;
    // min_version = 0.
    auto z = EncodeRaw(APEX_PROTOCOL_VERSION_BEACON, APEX_TRAFFIC_CONFIG,
                       APEX_DEVICE_ID_BEACON, BeaconPayload(0, 7));
    EXPECT_EQ(APEX_ERR_MALFORMED, apex_beacon_parse(z.data(), z.size() - 1, &out));
    // max_version = 0xFFFF.
    auto f = EncodeRaw(APEX_PROTOCOL_VERSION_BEACON, APEX_TRAFFIC_CONFIG,
                       APEX_DEVICE_ID_BEACON, BeaconPayload(1, 0xFFFF));
    EXPECT_EQ(APEX_ERR_MALFORMED, apex_beacon_parse(f.data(), f.size() - 1, &out));
    // min > max.
    auto o = EncodeRaw(APEX_PROTOCOL_VERSION_BEACON, APEX_TRAFFIC_CONFIG,
                       APEX_DEVICE_ID_BEACON, BeaconPayload(9, 3));
    EXPECT_EQ(APEX_ERR_MALFORMED, apex_beacon_parse(o.data(), o.size() - 1, &out));
}

TEST(BeaconTest, ParseRejectsTruncation) {
    uint8_t buf[APEX_MAX_ENCODED_FRAME_LENGTH];
    size_t len = 0;
    ASSERT_EQ(APEX_OK, apex_beacon_build(1, 7, buf, sizeof(buf), &len));
    apex_beacon_t out;
    // Chop the encoded frame short — COBS decode / length check must fail.
    EXPECT_NE(APEX_OK, apex_beacon_parse(buf, (len - 1) / 2, &out));
}

TEST(BeaconTest, NonBeaconPvNotTreatedAsBeacon) {
    // A well-formed frame with the beacon inner payload but PV != 0xFF is a
    // normal session frame, not a beacon.
    auto enc = EncodeRaw(APEX_PROTOCOL_VERSION, APEX_TRAFFIC_CONFIG,
                         APEX_DEVICE_ID_BROADCAST, BeaconPayload(1, 7));
    apex_beacon_t out;
    EXPECT_EQ(APEX_ERR_MALFORMED, apex_beacon_parse(enc.data(), enc.size() - 1, &out));
}

/* ------------------------------------------------------------------------- */
/* Framer RX routing: PV=0xFF -> beacon callback, distinct from frame cb.      */
/* ------------------------------------------------------------------------- */

struct BeaconSink {
    int count = 0;
    uint16_t mn = 0, mx = 0;
};

static void on_beacon(void* user, uint16_t mn, uint16_t mx) {
    auto* s = static_cast<BeaconSink*>(user);
    s->count++;
    s->mn = mn;
    s->mx = mx;
}

static int g_frame_calls = 0;
static void on_frame(void*, const apex_hdr_t*, const uint8_t*, size_t) {
    g_frame_calls++;
}

TEST(BeaconTest, FramerRoutesBeaconToBeaconCallback) {
    uint8_t buf[APEX_MAX_ENCODED_FRAME_LENGTH];
    size_t len = 0;
    ASSERT_EQ(APEX_OK, apex_beacon_build(2, 5, buf, sizeof(buf), &len));

    apex_framer_rx_t rx;
    apex_framer_rx_init(&rx);
    BeaconSink sink;
    apex_framer_set_beacon_cb(&rx, on_beacon, &sink);

    g_frame_calls = 0;
    apex_framer_feed(&rx, buf, len, on_frame, nullptr);

    EXPECT_EQ(1, sink.count);           // delivered to beacon path
    EXPECT_EQ(2u, sink.mn);
    EXPECT_EQ(5u, sink.mx);
    EXPECT_EQ(0, g_frame_calls);        // NOT delivered as a normal frame
    EXPECT_EQ(1u, rx.frames_beacon);
    EXPECT_EQ(0u, rx.frames_ok);
}

TEST(BeaconTest, FramerDropsMalformedBeacon) {
    // Valid CRC/COBS, PV=0xFF, but reserved max_version 0xFFFF -> dropped, not
    // delivered.
    auto enc = EncodeRaw(APEX_PROTOCOL_VERSION_BEACON, APEX_TRAFFIC_CONFIG,
                         APEX_DEVICE_ID_BEACON, BeaconPayload(1, 0xFFFF));
    apex_framer_rx_t rx;
    apex_framer_rx_init(&rx);
    BeaconSink sink;
    apex_framer_set_beacon_cb(&rx, on_beacon, &sink);
    apex_framer_feed(&rx, enc.data(), enc.size(), on_frame, nullptr);
    EXPECT_EQ(0, sink.count);
    EXPECT_EQ(0u, rx.frames_beacon);
}

}  // namespace
