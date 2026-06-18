/* End-to-end Repeater class tests. Same loopback pattern as the activation /
 * analog-HMI walkthroughs: host and device share a process, TX callbacks crossed.
 *
 * Focus is the per-antenna rework (§3.2): a positionally-indexed antenna list,
 * per-link antenna assignment in CONFIG_REPORT, and per-antenna pointing in
 * TELEMETRY / ANTENNA_CMD.
 *
 * Coverage:
 *   - Discovery -> first TELEMETRY -> GET_CONFIG / CONFIG_REPORT round-trip on a
 *     dual-band, mixed-protocol, video + 2-antenna device; per-link antenna_id and
 *     the antenna list survive the encode/decode round-trip.
 *   - Per-antenna directionality round-trips (aimable antenna 0 + omni antenna 1).
 *   - ANTENNA_CMD: aimable antenna -> ACCEPTED; non-aimable -> REJECT_WRONG_STATE;
 *     out-of-range id -> REJECT_INVALID_VALUE; FAULT -> REJECT_WRONG_STATE.
 *   - SET_CONFIG per-antenna bearing_ref via host_set_antenna_ref; bad id rejected;
 *     generic host_set_config refuses the antenna bit.
 *   - DISTAL_TLM still tagged with the originating link's protocol.
 */
#include "apex/apex_device.h"
#include "apex/apex_host.h"
#include "apex/apex_repeater.h"

#include <gtest/gtest.h>

#include <cstring>
#include <vector>

namespace {

struct Bus {
    std::vector<uint8_t> h2d;
    std::vector<uint8_t> d2h;
};

static void host_tx(void* u, const uint8_t* b, size_t n) {
    auto* bus = static_cast<Bus*>(u);
    bus->h2d.insert(bus->h2d.end(), b, b + n);
}
static void device_tx(void* u, const uint8_t* b, size_t n) {
    auto* bus = static_cast<Bus*>(u);
    bus->d2h.insert(bus->d2h.end(), b, b + n);
}

struct HostTelemetryCapture {
    int count = 0;
    uint8_t device_id = 0;
    apex_repeater_telemetry_t last{};
};
struct HostConfigCapture {
    int count = 0;
    uint8_t device_id = 0;
    apex_repeater_config_t last{};
};
struct HostAckCapture {
    int count = 0;
    uint8_t device_id = 0;
    apex_repeater_ack_t last{};
};
struct HostDistalCapture {
    int count = 0;
    uint8_t link_index = 0;
    uint8_t c2_protocol = 0;
    std::vector<uint8_t> bytes;
};
struct DeviceSetConfigCapture {
    int count = 0;
    uint8_t update_mask = 0;
    apex_repeater_config_t proposed{};
    apex_status_t verdict = APEX_OK;  // returned to the lib
};
struct DeviceAntennaCmdCapture {
    int count = 0;
    uint8_t antenna_id = 0;
    uint16_t bearing = 0;
    apex_status_t verdict = APEX_OK;
};

class RepeaterTest : public ::testing::Test {
protected:
    Bus bus{};
    apex_host_t host{};
    apex_device_t dev_core{};
    apex_repeater_host_t rpt_host{};
    apex_repeater_device_t rpt_dev{};

    HostTelemetryCapture h_tlm{};
    HostConfigCapture h_cfg{};
    HostAckCapture h_ack{};
    HostDistalCapture h_distal{};
    DeviceSetConfigCapture d_set{};
    DeviceAntennaCmdCapture d_ant{};

    uint32_t now_ms = 0;

    static void dev_class_rx_trampoline(void* u, const uint8_t* p, size_t n) {
        apex_repeater_device_on_rx(static_cast<apex_repeater_device_t*>(u), p, n);
    }

    // A dual-band, mixed-protocol, video + 2-antenna repeater.
    //   cap = C2_0 | C2_1 | VIDEO | ANTENNAS = 0x33
    //   antenna 0: aimable (responds to ANTENNA_CMD); antenna 1: passive DOA
    //   C2 link 0 (CRSF) -> antenna 0; C2 link 1 (MAVLink) -> antenna 0; video -> antenna 1
    static apex_repeater_config_t MakeConfig() {
        apex_repeater_config_t c{};
        c.capability_flags = APEX_RPT_CAP_C2_LINK_0 | APEX_RPT_CAP_C2_LINK_1 |
                             APEX_RPT_CAP_VIDEO | APEX_RPT_CAP_ANTENNAS;

        c.c2[0].c2_protocol = APEX_RPT_C2_PROTOCOL_CRSF;
        c.c2[0].c2_config_version = 0;
        c.c2[0].c2_config_len = 4;
        c.c2[0].c2_config_blob[0] = 0xC8; c.c2[0].c2_config_blob[1] = 0x00;
        c.c2[0].c2_config_blob[2] = 0x01; c.c2[0].c2_config_blob[3] = 0x0A;
        c.c2[0].antenna_id = 0;

        c.c2[1].c2_protocol = APEX_RPT_C2_PROTOCOL_MAVLINK;
        c.c2[1].c2_config_version = 0;
        c.c2[1].c2_config_len = 4;
        c.c2[1].c2_config_blob[0] = 0x03; c.c2[1].c2_config_blob[1] = 0x00;
        c.c2[1].c2_config_blob[2] = 0x01; c.c2[1].c2_config_blob[3] = 0x05;
        c.c2[1].antenna_id = 0;

        c.video.rx_freq_mhz = 5808;
        c.video.rx_bw_mhz = 20;       // occupied band 5798..5818 MHz
        c.video.rx_format = APEX_RPT_VIDEO_FORMAT_ANALOG_PAL;
        c.video.tx_freq_mhz = 5840;
        c.video.tx_bw_mhz = 20;       // occupied band 5830..5850 MHz
        c.video.tx_power_dbm = 28;
        c.video.tx_format = APEX_RPT_VIDEO_FORMAT_ANALOG_PAL;
        c.video.antenna_id = 1;

        c.antenna_count = 2;
        c.antennas[0].antenna_type = APEX_RPT_ANTENNA_DIRECTIONAL_AIMABLE;
        c.antennas[0].antenna_bearing_ref = APEX_RPT_BEARING_REF_MAGNETIC;
        c.antennas[1].antenna_type = APEX_RPT_ANTENNA_DIRECTIONAL_DOA;
        c.antennas[1].antenna_bearing_ref = APEX_RPT_BEARING_REF_MAGNETIC;

        c.global.encryption_state = APEX_RPT_ENCRYPTION_NONE;
        c.global.distal_tlm_rate_hz = 20;
        return c;
    }

    void SetUp() override {
        apex_host_cfg_t hc{};
        hc.supported_interfaces = 0;
        hc.host_state_period_ms = 0;
        hc.tx = host_tx;
        hc.tx_user = &bus;
        apex_host_init(&host, &hc);

        apex_repeater_host_hooks_t hh{};
        hh.on_telemetry = +[](void* u, uint8_t did, const apex_repeater_telemetry_t* t) {
            auto* p = static_cast<HostTelemetryCapture*>(u);
            p->count++; p->device_id = did; p->last = *t;
        };
        hh.on_telemetry_user = &h_tlm;
        hh.on_config_report = +[](void* u, uint8_t did, const apex_repeater_config_t* c) {
            auto* p = static_cast<HostConfigCapture*>(u);
            p->count++; p->device_id = did; p->last = *c;
        };
        hh.on_config_report_user = &h_cfg;
        hh.on_ack = +[](void* u, uint8_t did, const apex_repeater_ack_t* a) {
            auto* p = static_cast<HostAckCapture*>(u);
            p->count++; p->device_id = did; p->last = *a;
        };
        hh.on_ack_user = &h_ack;
        hh.on_distal_tlm = +[](void* u, uint8_t /*did*/, uint8_t li, uint8_t proto,
                               const uint8_t* b, size_t n) {
            auto* p = static_cast<HostDistalCapture*>(u);
            p->count++; p->link_index = li; p->c2_protocol = proto;
            p->bytes.assign(b, b + n);
        };
        hh.on_distal_tlm_user = &h_distal;
        ASSERT_EQ(APEX_OK, apex_repeater_host_init(&rpt_host, &host, &hh));

        apex_device_cfg_t dc{};
        dc.device_class = APEX_TRAFFIC_REPEATER;
        dc.interface_flags = 0;
        dc.tx = device_tx;
        dc.tx_user = &bus;
        dc.on_class_rx = dev_class_rx_trampoline;
        dc.on_class_rx_user = &rpt_dev;
        apex_device_init(&dev_core, &dc);

        apex_repeater_device_hooks_t dh{};
        dh.on_set_config = +[](void* u, uint8_t mask, const apex_repeater_config_t* c) {
            auto* p = static_cast<DeviceSetConfigCapture*>(u);
            p->count++; p->update_mask = mask; p->proposed = *c;
            return p->verdict;
        };
        dh.on_set_config_user = &d_set;
        dh.on_antenna_cmd = +[](void* u, uint8_t aid, uint16_t bearing) {
            auto* p = static_cast<DeviceAntennaCmdCapture*>(u);
            p->count++; p->antenna_id = aid; p->bearing = bearing;
            return p->verdict;
        };
        dh.on_antenna_cmd_user = &d_ant;

        apex_repeater_config_t cfg = MakeConfig();
        ASSERT_EQ(APEX_OK,
                  apex_repeater_device_init(&rpt_dev, &dev_core, &cfg, &dh));
    }

    void Pump() {
        now_ms += 1;
        apex_device_tick(&dev_core, now_ms);
        apex_repeater_device_tick(&rpt_dev, now_ms);
        apex_host_tick(&host, now_ms);
        if (!bus.d2h.empty()) {
            auto b = std::move(bus.d2h);
            bus.d2h.clear();
            apex_host_feed_rx(&host, b.data(), b.size(), now_ms);
        }
        if (!bus.h2d.empty()) {
            auto b = std::move(bus.h2d);
            bus.h2d.clear();
            apex_device_feed_rx(&dev_core, b.data(), b.size(), now_ms);
        }
    }
    void PumpRounds(int n = 16) { for (int i = 0; i < n; i++) Pump(); }

    uint8_t Connect() {
        PumpRounds();
        EXPECT_EQ(APEX_DEVICE_STATE_CONNECTED, apex_device_link_state(&dev_core));
        uint8_t id = apex_device_get_id(&dev_core);
        EXPECT_NE(APEX_DEVICE_ID_UNASSIGNED, id);
        return id;
    }
};

TEST_F(RepeaterTest, ConfigReportRoundTripCarriesAntennaListAndPerLinkMapping) {
    uint8_t id = Connect();
    ASSERT_GE(h_tlm.count, 1);  // TELEMETRY started on its own

    ASSERT_EQ(APEX_OK, apex_repeater_host_get_config(&rpt_host, id));
    PumpRounds(4);

    ASSERT_EQ(1, h_cfg.count);
    const apex_repeater_config_t& c = h_cfg.last;
    EXPECT_EQ(APEX_RPT_CAP_C2_LINK_0 | APEX_RPT_CAP_C2_LINK_1 |
                  APEX_RPT_CAP_VIDEO | APEX_RPT_CAP_ANTENNAS,
              c.capability_flags);

    // Per-link protocol + antenna assignment survived.
    EXPECT_EQ(APEX_RPT_C2_PROTOCOL_CRSF, c.c2[0].c2_protocol);
    EXPECT_EQ(0, c.c2[0].antenna_id);
    EXPECT_EQ(APEX_RPT_C2_PROTOCOL_MAVLINK, c.c2[1].c2_protocol);
    EXPECT_EQ(0, c.c2[1].antenna_id);
    EXPECT_EQ(1, c.video.antenna_id);
    EXPECT_EQ(5840, c.video.tx_freq_mhz);
    EXPECT_EQ(20, c.video.tx_bw_mhz);   // center + bandwidth round-trips
    EXPECT_EQ(5808, c.video.rx_freq_mhz);
    EXPECT_EQ(20, c.video.rx_bw_mhz);

    // Antenna list survived, positionally.
    ASSERT_EQ(2, c.antenna_count);
    EXPECT_EQ(APEX_RPT_ANTENNA_DIRECTIONAL_AIMABLE, c.antennas[0].antenna_type);
    EXPECT_EQ(APEX_RPT_ANTENNA_DIRECTIONAL_DOA, c.antennas[1].antenna_type);

    // C2 blob bytes intact.
    EXPECT_EQ(4, c.c2[0].c2_config_len);
    EXPECT_EQ(0xC8, c.c2[0].c2_config_blob[0]);
    EXPECT_EQ(0x05, c.c2[1].c2_config_blob[3]);
}

TEST_F(RepeaterTest, PerAntennaDirectionalityRoundTrips) {
    Connect();

    apex_repeater_telemetry_t t{};
    t.device_state = APEX_RPT_STATE_ACTIVE;
    t.capability_flags = APEX_RPT_CAP_C2_LINK_0 | APEX_RPT_CAP_C2_LINK_1 |
                         APEX_RPT_CAP_VIDEO | APEX_RPT_CAP_ANTENNAS;
    t.link_blocks[0] = {(int8_t)-60, 92, 15, 27, APEX_RPT_LINK_RX_ACTIVE | APEX_RPT_LINK_TX_ACTIVE};
    t.link_blocks[1] = {(int8_t)-70, 85, 10, 30, APEX_RPT_LINK_RX_ACTIVE | APEX_RPT_LINK_TX_ACTIVE};
    t.link_blocks[4] = {(int8_t)-45, 98, 25, 28, APEX_RPT_LINK_RX_ACTIVE | APEX_RPT_LINK_TX_ACTIVE};
    t.antenna_count = 2;
    // Antenna 0 (aimable) is tracking; antenna 1 (DOA) has no fix yet.
    t.antenna_dir[0] = {135, 135, 2800, 85};
    t.antenna_dir[1] = {0xFFFF, 0xFFFF, 0xFFFF, 0};
    apex_repeater_device_update_telemetry(&rpt_dev, &t);
    PumpRounds(4);

    ASSERT_GE(h_tlm.count, 1);
    const apex_repeater_telemetry_t& got = h_tlm.last;
    ASSERT_EQ(2, got.antenna_count);
    EXPECT_EQ(135, got.antenna_dir[0].antenna_bearing_deg);
    EXPECT_EQ(2800, got.antenna_dir[0].distal_distance_m);
    EXPECT_EQ(85, got.antenna_dir[0].confidence);
    EXPECT_EQ(0xFFFF, got.antenna_dir[1].antenna_bearing_deg);
    EXPECT_EQ(0, got.antenna_dir[1].confidence);
    // Link blocks unaffected by the antenna rework.
    EXPECT_EQ((int8_t)-45, got.link_blocks[4].rssi_dbm);
}

TEST_F(RepeaterTest, AntennaCmdToAimableAntennaAccepted) {
    uint8_t id = Connect();
    ASSERT_EQ(APEX_OK, apex_repeater_host_antenna_cmd(&rpt_host, id, /*antenna_id=*/0, 135));
    PumpRounds(4);

    ASSERT_EQ(1, d_ant.count);
    EXPECT_EQ(0, d_ant.antenna_id);
    EXPECT_EQ(135, d_ant.bearing);
    ASSERT_GE(h_ack.count, 1);
    EXPECT_EQ(APEX_RPT_MSG_ANTENNA_CMD, h_ack.last.acked_msg);
    EXPECT_EQ(APEX_RPT_ACK_ACCEPTED, h_ack.last.result);
}

TEST_F(RepeaterTest, AntennaCmdToNonAimableAntennaRejectedWrongState) {
    uint8_t id = Connect();
    // Antenna 1 is passive DOA, not aimable.
    ASSERT_EQ(APEX_OK, apex_repeater_host_antenna_cmd(&rpt_host, id, /*antenna_id=*/1, 90));
    PumpRounds(4);

    EXPECT_EQ(0, d_ant.count);  // hook never fires for a non-aimable antenna
    ASSERT_GE(h_ack.count, 1);
    EXPECT_EQ(APEX_RPT_ACK_REJECT_WRONG_STATE, h_ack.last.result);
}

TEST_F(RepeaterTest, AntennaCmdToUnknownAntennaRejectedInvalidValue) {
    uint8_t id = Connect();
    ASSERT_EQ(APEX_OK, apex_repeater_host_antenna_cmd(&rpt_host, id, /*antenna_id=*/5, 90));
    PumpRounds(4);

    EXPECT_EQ(0, d_ant.count);
    ASSERT_GE(h_ack.count, 1);
    EXPECT_EQ(APEX_RPT_ACK_REJECT_INVALID_VALUE, h_ack.last.result);
}

TEST_F(RepeaterTest, AntennaCmdInFaultRejectedWrongState) {
    uint8_t id = Connect();
    apex_repeater_device_set_fault(&rpt_dev);
    PumpRounds(2);
    ASSERT_EQ(APEX_OK, apex_repeater_host_antenna_cmd(&rpt_host, id, /*antenna_id=*/0, 135));
    PumpRounds(4);

    EXPECT_EQ(0, d_ant.count);
    ASSERT_GE(h_ack.count, 1);
    EXPECT_EQ(APEX_RPT_ACK_REJECT_WRONG_STATE, h_ack.last.result);
}

TEST_F(RepeaterTest, SetAntennaRefUpdatesNamedAntenna) {
    uint8_t id = Connect();
    ASSERT_EQ(APEX_OK, apex_repeater_host_set_antenna_ref(
                           &rpt_host, id, /*antenna_id=*/1, APEX_RPT_BEARING_REF_DRONE_HDG));
    PumpRounds(4);

    ASSERT_EQ(1, d_set.count);
    EXPECT_EQ(APEX_RPT_UPDATE_ANTENNA, d_set.update_mask);
    EXPECT_EQ(APEX_RPT_BEARING_REF_DRONE_HDG, d_set.proposed.antennas[1].antenna_bearing_ref);
    // Antenna 0 untouched.
    EXPECT_EQ(APEX_RPT_BEARING_REF_MAGNETIC, d_set.proposed.antennas[0].antenna_bearing_ref);
    ASSERT_GE(h_ack.count, 1);
    EXPECT_EQ(APEX_RPT_ACK_ACCEPTED, h_ack.last.result);

    // Confirm it stuck via a fresh CONFIG_REPORT.
    ASSERT_EQ(APEX_OK, apex_repeater_host_get_config(&rpt_host, id));
    PumpRounds(4);
    ASSERT_EQ(1, h_cfg.count);
    EXPECT_EQ(APEX_RPT_BEARING_REF_DRONE_HDG, h_cfg.last.antennas[1].antenna_bearing_ref);
}

TEST_F(RepeaterTest, SetAntennaRefBadIdRejected) {
    uint8_t id = Connect();
    ASSERT_EQ(APEX_OK, apex_repeater_host_set_antenna_ref(
                           &rpt_host, id, /*antenna_id=*/9, APEX_RPT_BEARING_REF_DRONE_HDG));
    PumpRounds(4);
    ASSERT_GE(h_ack.count, 1);
    EXPECT_EQ(APEX_RPT_MSG_SET_CONFIG, h_ack.last.acked_msg);
    EXPECT_EQ(APEX_RPT_ACK_REJECT_INVALID_VALUE, h_ack.last.result);
}

TEST_F(RepeaterTest, GenericSetConfigRefusesAntennaBit) {
    apex_repeater_config_t c = MakeConfig();
    EXPECT_EQ(APEX_ERR_INVALID_ARGS,
              apex_repeater_host_set_config(&rpt_host, 1, APEX_RPT_UPDATE_ANTENNA, &c));
}

TEST_F(RepeaterTest, SetConfigVideoStillWorks) {
    uint8_t id = Connect();
    apex_repeater_config_t c = MakeConfig();
    c.video.tx_power_dbm = 24;  // reduce video TX power
    c.video.tx_bw_mhz = 17;     // and narrow the TX channel
    ASSERT_EQ(APEX_OK, apex_repeater_host_set_config(
                           &rpt_host, id, APEX_RPT_UPDATE_VIDEO_TX, &c));
    PumpRounds(4);
    ASSERT_EQ(1, d_set.count);
    EXPECT_EQ(24, d_set.proposed.video.tx_power_dbm);
    EXPECT_EQ(17, d_set.proposed.video.tx_bw_mhz);
    ASSERT_GE(h_ack.count, 1);
    EXPECT_EQ(APEX_RPT_ACK_ACCEPTED, h_ack.last.result);
}

// The spec (§3.2) guarantees 8 antennas always fit, even in the worst-case
// configuration: four C2 links with full 48-byte blobs + video + 8 antennas.
// This drives that exact frame through real COBS/CRC framing both ways.
TEST_F(RepeaterTest, EightAntennaWorstCaseRoundTrips) {
    // Rebuild the device with the maximal config before discovery.
    apex_repeater_config_t c{};
    c.capability_flags = APEX_RPT_CAP_C2_LINK_0 | APEX_RPT_CAP_C2_LINK_1 |
                         APEX_RPT_CAP_C2_LINK_2 | APEX_RPT_CAP_C2_LINK_3 |
                         APEX_RPT_CAP_VIDEO | APEX_RPT_CAP_ANTENNAS;
    for (int b = 0; b < 4; b++) {
        c.c2[b].c2_protocol = (b & 1) ? APEX_RPT_C2_PROTOCOL_MAVLINK
                                      : APEX_RPT_C2_PROTOCOL_CRSF;
        c.c2[b].c2_config_version = 0;
        c.c2[b].c2_config_len = APEX_REPEATER_C2_CONFIG_MAX;  // 48-byte blob
        for (unsigned k = 0; k < APEX_REPEATER_C2_CONFIG_MAX; k++)
            c.c2[b].c2_config_blob[k] = (uint8_t)(b * 16 + k);
        c.c2[b].antenna_id = (uint8_t)b;        // each C2 link on its own antenna
    }
    c.video.rx_freq_mhz = 5808; c.video.tx_freq_mhz = 5840;
    c.video.tx_power_dbm = 28; c.video.antenna_id = 4;  // video on antenna 4
    c.antenna_count = APEX_REPEATER_MAX_ANTENNAS;        // 8 — the guaranteed max
    for (uint8_t a = 0; a < APEX_REPEATER_MAX_ANTENNAS; a++) {
        c.antennas[a].antenna_type = (a < 4) ? APEX_RPT_ANTENNA_DIRECTIONAL_AIMABLE
                                             : APEX_RPT_ANTENNA_OMNI;
        c.antennas[a].antenna_bearing_ref = APEX_RPT_BEARING_REF_MAGNETIC;
    }
    c.global.distal_tlm_rate_hz = 20;

    apex_repeater_device_hooks_t dh{};  // no hooks needed for this test
    // Re-init the device core + class with the maximal config.
    apex_device_cfg_t dc{};
    dc.device_class = APEX_TRAFFIC_REPEATER;
    dc.tx = device_tx; dc.tx_user = &bus;
    dc.on_class_rx = dev_class_rx_trampoline; dc.on_class_rx_user = &rpt_dev;
    apex_device_init(&dev_core, &dc);
    ASSERT_EQ(APEX_OK, apex_repeater_device_init(&rpt_dev, &dev_core, &c, &dh));

    uint8_t id = Connect();

    // CONFIG_REPORT (worst case 237 bytes) survives framing + parse.
    ASSERT_EQ(APEX_OK, apex_repeater_host_get_config(&rpt_host, id));
    PumpRounds(4);
    ASSERT_EQ(1, h_cfg.count);
    ASSERT_EQ(8, h_cfg.last.antenna_count);
    EXPECT_EQ(3, h_cfg.last.c2[3].antenna_id);
    EXPECT_EQ(4, h_cfg.last.video.antenna_id);
    EXPECT_EQ(APEX_REPEATER_C2_CONFIG_MAX, h_cfg.last.c2[2].c2_config_len);
    EXPECT_EQ((uint8_t)(2 * 16 + 7), h_cfg.last.c2[2].c2_config_blob[7]);
    EXPECT_EQ(APEX_RPT_ANTENNA_OMNI, h_cfg.last.antennas[7].antenna_type);

    // TELEMETRY (worst case 85 bytes) carries all 8 directionality blocks.
    apex_repeater_telemetry_t t{};
    t.device_state = APEX_RPT_STATE_ACTIVE;
    t.capability_flags = c.capability_flags;
    t.antenna_count = 8;
    for (uint8_t a = 0; a < 8; a++)
        t.antenna_dir[a] = {(uint16_t)(10 * a), (uint16_t)(20 * a),
                            (uint16_t)(100 * a), (uint8_t)(a * 10)};
    apex_repeater_device_update_telemetry(&rpt_dev, &t);
    PumpRounds(4);
    ASSERT_GE(h_tlm.count, 1);
    ASSERT_EQ(8, h_tlm.last.antenna_count);
    EXPECT_EQ(70, h_tlm.last.antenna_dir[7].antenna_bearing_deg);
    EXPECT_EQ(700, h_tlm.last.antenna_dir[7].distal_distance_m);
}

TEST_F(RepeaterTest, DistalTlmTaggedWithLinkProtocol) {
    Connect();
    // MAVLink bytes arriving on C2 link 1.
    uint8_t mav[] = {0xFD, 0x09, 0x00, 0x00, 0x00, 0x01, 0x01};
    ASSERT_EQ(APEX_OK, apex_repeater_device_send_distal_tlm(&rpt_dev, /*link=*/1,
                                                            mav, sizeof(mav)));
    PumpRounds(4);
    ASSERT_GE(h_distal.count, 1);
    EXPECT_EQ(1, h_distal.link_index);
    EXPECT_EQ(APEX_RPT_C2_PROTOCOL_MAVLINK, h_distal.c2_protocol);
    EXPECT_EQ(std::vector<uint8_t>(mav, mav + sizeof(mav)), h_distal.bytes);
}

}  // namespace
