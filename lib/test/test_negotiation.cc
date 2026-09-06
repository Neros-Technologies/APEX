/* Negotiation tests for APEX wire v1: VERSION_BEACON exchange (§3.6.2) and the
 * post-CONNECTED baud ladder (§3.4), including the rate-switch callbacks. */
#include "apex/apex_device.h"
#include "apex/apex_framer.h"
#include "apex/apex_host.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

namespace {

struct Bus {
    std::vector<uint8_t> host_to_device;
    std::vector<uint8_t> device_to_host;
};
void host_tx(void* u, const uint8_t* b, size_t n) {
    auto* bus = static_cast<Bus*>(u);
    bus->host_to_device.insert(bus->host_to_device.end(), b, b + n);
}
void device_tx(void* u, const uint8_t* b, size_t n) {
    auto* bus = static_cast<Bus*>(u);
    bus->device_to_host.insert(bus->device_to_host.end(), b, b + n);
}

/* Baud outcome capture. */
struct BaudResult { bool fired = false; bool granted = false; apex_baud_code_t code{}; };
BaudResult g_dev_result;
void on_dev_baud(void* u, bool granted, apex_baud_code_t code) {
    (void)u;
    g_dev_result.fired = true;
    g_dev_result.granted = granted;
    g_dev_result.code = code;
}
struct SwitchCap { bool fired = false; uint8_t id = 0; apex_baud_code_t code{}; };
SwitchCap g_host_switch;
void on_host_switch(void* u, uint8_t id, apex_baud_code_t code) {
    (void)u;
    g_host_switch.fired = true;
    g_host_switch.id = id;
    g_host_switch.code = code;
}

struct Link {
    Bus bus{};
    apex_host_t host{};
    apex_device_t dev{};
    uint32_t now_ms = 0;

    void Pump(uint32_t advance_ms = 1, int rounds = 1) {
        for (int i = 0; i < rounds; i++) {
            now_ms += advance_ms;
            apex_device_tick(&dev, now_ms);
            apex_host_tick(&host, now_ms);
            if (!bus.device_to_host.empty()) {
                auto b = std::move(bus.device_to_host); bus.device_to_host.clear();
                apex_host_feed_rx(&host, b.data(), b.size(), now_ms);
            }
            if (!bus.host_to_device.empty()) {
                auto b = std::move(bus.host_to_device); bus.host_to_device.clear();
                apex_device_feed_rx(&dev, b.data(), b.size(), now_ms);
            }
        }
    }
};

/* Bring a link to CONNECTED with the given host baud ceiling. */
void Connect(Link& l, uint8_t max_baud_code) {
    apex_host_cfg_t hc{};
    hc.supported_interfaces = APEX_INTERFACE_FLAG_GPIO;
    hc.host_state_period_ms = 1000;
    hc.max_baud_code = max_baud_code;
    hc.baud_switch_cb = on_host_switch;
    hc.tx = host_tx; hc.tx_user = &l.bus;
    apex_host_init(&l.host, &hc);
    apex_host_register_class(&l.host, APEX_TRAFFIC_ACTIVATION, nullptr, nullptr);

    apex_device_cfg_t dc{};
    dc.device_class = APEX_TRAFFIC_ACTIVATION;
    dc.interface_flags = APEX_INTERFACE_FLAG_GPIO;
    dc.class_version_min = 1; dc.class_version_max = 1;
    dc.mass_grams = 500;
    dc.on_baud_result = on_dev_baud;
    dc.tx = device_tx; dc.tx_user = &l.bus;
    apex_device_init(&l.dev, &dc);

    l.Pump(1, 8);
    ASSERT_EQ(APEX_DEVICE_STATE_CONNECTED, apex_device_link_state(&l.dev));
}

/* ------------------------------------------------------------------------- */
/* VERSION_BEACON negotiation (proactive mutual beaconing).              */
/* ------------------------------------------------------------------------- */

apex_device_cfg_t LoneDeviceCfg(Bus& bus) {
    apex_device_cfg_t dc{};
    dc.device_class = APEX_TRAFFIC_ACTIVATION;
    dc.interface_flags = APEX_INTERFACE_FLAG_GPIO;
    dc.class_version_min = 1; dc.class_version_max = 1;
    dc.mass_grams = 500;
    dc.tx = device_tx; dc.tx_user = &bus;
    return dc;
}

/* Scan an encoded byte stream: count beacons and collect CONFIG msg_ids. */
struct StreamStats {
    int beacons = 0;
    std::vector<uint8_t> msg_ids;
};
StreamStats ScanStream(const std::vector<uint8_t>& enc) {
    StreamStats st;
    if (enc.empty()) return st;
    apex_framer_rx_t rx;
    apex_framer_rx_init(&rx);
    apex_framer_set_beacon_cb(
        &rx,
        [](void* u, uint16_t, uint16_t) { static_cast<StreamStats*>(u)->beacons++; },
        &st);
    apex_framer_feed(
        &rx, enc.data(), enc.size(),
        [](void* u, const apex_hdr_t* hdr, const uint8_t* p, size_t n) {
            auto* s = static_cast<StreamStats*>(u);
            if (hdr->traffic_type == APEX_TRAFFIC_CONFIG && n >= 1) {
                s->msg_ids.push_back(p[0]);
            }
        },
        &st);
    return st;
}
bool HasMsg(const StreamStats& st, uint8_t id) {
    for (uint8_t m : st.msg_ids) if (m == id) return true;
    return false;
}

/* Forge an encoded device->host CONFIG frame (PV=1) at the given source id. */
std::vector<uint8_t> ForgeDeviceFrame(uint8_t device_id, uint8_t pv,
                                      const std::vector<uint8_t>& payload) {
    apex_hdr_t hdr{};
    hdr.protocol_version = pv;
    hdr.traffic_type = APEX_TRAFFIC_CONFIG;
    hdr.device_id = device_id;
    hdr.payload_length = (uint8_t)payload.size();
    uint8_t buf[APEX_MAX_ENCODED_FRAME_LENGTH];
    size_t len = 0;
    EXPECT_EQ(APEX_OK, apex_frame_encode(&hdr,
                                         payload.empty() ? nullptr : payload.data(),
                                         buf, sizeof(buf), &len));
    return std::vector<uint8_t>(buf, buf + len);
}

/* Mutual-receipt gate, device side: beacons flow from power-up at ~1 Hz,
 * but NO DEVICE_INFO until the host's beacon closes the version tier; then
 * the hello opens at the mutual version min(own_max, their_max). */
TEST(BeaconNeg, DeviceSendsNoHelloBeforeHostBeacon) {
    Bus bus{};
    apex_device_t dev{};
    apex_device_cfg_t dc = LoneDeviceCfg(bus);
    apex_device_init(&dev, &dc);

    for (int i = 0; i <= 30; i++) apex_device_tick(&dev, (uint32_t)i * 100);
    StreamStats st = ScanStream(bus.device_to_host);
    EXPECT_GE(st.beacons, 3);            // ~1 Hz over 3 s
    EXPECT_LE(st.beacons, 5);
    EXPECT_TRUE(st.msg_ids.empty());     // no session traffic at all
    EXPECT_EQ(APEX_DEVICE_STATE_DISCOVERING, apex_device_link_state(&dev));

    // Host beacon [1,5] arrives: tier closes at mutual min(1,5) = 1 and the
    // hello goes out immediately.
    bus.device_to_host.clear();
    uint8_t bcn[APEX_MAX_ENCODED_FRAME_LENGTH];
    size_t bl = 0;
    ASSERT_EQ(APEX_OK, apex_beacon_build(1, 5, bcn, sizeof(bcn), &bl));
    apex_device_feed_rx(&dev, bcn, bl, 3100);

    st = ScanStream(bus.device_to_host);
    ASSERT_TRUE(HasMsg(st, APEX_CFG_MSG_DEVICE_INFO));
    EXPECT_TRUE(dev.peer_beacon_seen);
    EXPECT_EQ(1u, dev.peer_min_version);
    EXPECT_EQ(5u, dev.peer_max_version);
    EXPECT_EQ(1u, dev.mutual_version);   // min(own_max=1, their_max=5)
}

/* Mutual-receipt gate, host side: a DEVICE_INFO arriving before the
 * device's beacon is dropped silently (no CONFIG_REPLY, no slot); after the
 * beacon it is answered. */
TEST(BeaconNeg, HostAnswersNoHelloBeforeDeviceBeacon) {
    Bus bus{};
    apex_host_t host{};
    apex_host_cfg_t hc{};
    hc.supported_interfaces = APEX_INTERFACE_FLAG_GPIO;
    hc.suppress_boot_sweep = true;
    hc.host_state_period_ms = 0;
    hc.tx = host_tx; hc.tx_user = &bus;
    apex_host_init(&host, &hc);
    ASSERT_EQ(APEX_OK, apex_host_register_class(&host, APEX_TRAFFIC_ACTIVATION,
                                                nullptr, nullptr));

    auto di = ForgeDeviceFrame(APEX_DEVICE_ID_UNASSIGNED, APEX_PROTOCOL_VERSION,
                               {APEX_CFG_MSG_DEVICE_INFO, APEX_TRAFFIC_ACTIVATION,
                                APEX_INTERFACE_FLAG_GPIO, 1, 1, 100, 0});
    apex_host_feed_rx(&host, di.data(), di.size(), 10);
    EXPECT_TRUE(bus.host_to_device.empty());     // dropped silently
    EXPECT_EQ(0u, apex_host_device_count(&host));

    uint8_t bcn[APEX_MAX_ENCODED_FRAME_LENGTH];
    size_t bl = 0;
    ASSERT_EQ(APEX_OK, apex_beacon_build(1, 1, bcn, sizeof(bcn), &bl));
    apex_host_feed_rx(&host, bcn, bl, 20);
    apex_host_feed_rx(&host, di.data(), di.size(), 30);

    StreamStats st = ScanStream(bus.host_to_device);
    EXPECT_TRUE(HasMsg(st, APEX_CFG_MSG_CONFIG_REPLY));
    EXPECT_EQ(1u, apex_host_device_count(&host));
}

/* The point itself: receiver discipline stays pure — garbage or an
 * unsupported-PV frame produces NO transmission whatsoever; emission is only
 * ever the periodic schedule (tick-driven). */
TEST(BeaconNeg, ReceiverDisciplineNoTransmissionOnUnintelligibleInput) {
    Bus bus{};
    apex_host_t host{};
    apex_host_cfg_t hc{};
    hc.supported_interfaces = APEX_INTERFACE_FLAG_GPIO;
    hc.suppress_boot_sweep = true;
    hc.host_state_period_ms = 0;
    hc.tx = host_tx; hc.tx_user = &bus;
    apex_host_init(&host, &hc);
    ASSERT_EQ(APEX_OK, apex_host_register_class(&host, APEX_TRAFFIC_ACTIVATION,
                                                nullptr, nullptr));

    // (a) Line noise / non-APEX garbage.
    uint8_t junk[] = {0x00, 0x03, 0x12, 0x34, 0x00, 0x01, 0xFF, 0x00};
    apex_host_feed_rx(&host, junk, sizeof(junk), 5);
    EXPECT_TRUE(bus.host_to_device.empty());

    // (b) A well-formed frame at unsupported session version PV=5: dropped,
    // counted, and — the point — answered with NOTHING.
    auto stale = ForgeDeviceFrame(APEX_DEVICE_ID_UNASSIGNED, 5,
                                  {APEX_CFG_MSG_DEVICE_INFO,
                                   APEX_TRAFFIC_ACTIVATION, 0});
    apex_host_feed_rx(&host, stale.data(), stale.size(), 6);
    apex_host_feed_rx(&host, stale.data(), stale.size(), 7);
    EXPECT_TRUE(bus.host_to_device.empty());
    EXPECT_EQ(2u, host.unsupported_pv_frames);

    // (c) Emission happens only on the periodic schedule: the first tick
    // beacons (port unlinked) — one beacon, nothing else.
    apex_host_tick(&host, 10);
    StreamStats st = ScanStream(bus.host_to_device);
    EXPECT_EQ(1, st.beacons);
    EXPECT_TRUE(st.msg_ids.empty());
    // And the cadence holds: another tick inside the period emits nothing.
    bus.host_to_device.clear();
    apex_host_tick(&host, 500);
    EXPECT_TRUE(bus.host_to_device.empty());
}

/* Lost-beacon tolerance: the host's first beacon is dropped; the periodic
 * retransmit closes the tier one cadence period later and discovery
 * completes. */
TEST(BeaconNeg, LostBeaconRetransmitClosesTier) {
    Link l;
    apex_host_cfg_t hc{};
    hc.supported_interfaces = APEX_INTERFACE_FLAG_GPIO;
    hc.host_state_period_ms = 1000;
    hc.tx = host_tx; hc.tx_user = &l.bus;
    apex_host_init(&l.host, &hc);
    ASSERT_EQ(APEX_OK, apex_host_register_class(&l.host, APEX_TRAFFIC_ACTIVATION,
                                                nullptr, nullptr));
    apex_device_cfg_t dc{};
    dc.device_class = APEX_TRAFFIC_ACTIVATION;
    dc.interface_flags = APEX_INTERFACE_FLAG_GPIO;
    dc.mass_grams = 500;
    dc.tx = device_tx; dc.tx_user = &l.bus;
    apex_device_init(&l.dev, &dc);

    // Round 1: both beacon; the host's beacon is LOST on the way down.
    l.now_ms = 1;
    apex_device_tick(&l.dev, l.now_ms);
    apex_host_tick(&l.host, l.now_ms);
    {
        auto b = std::move(l.bus.device_to_host); l.bus.device_to_host.clear();
        apex_host_feed_rx(&l.host, b.data(), b.size(), l.now_ms);
    }
    l.bus.host_to_device.clear();  // host's first beacon lost
    EXPECT_EQ(APEX_DEVICE_STATE_DISCOVERING, apex_device_link_state(&l.dev));
    EXPECT_FALSE(l.dev.peer_beacon_seen);

    // The next periodic host beacon (~1 s) closes the tier; discovery runs.
    l.Pump(200, 12);
    EXPECT_EQ(APEX_DEVICE_STATE_CONNECTED, apex_device_link_state(&l.dev));
    EXPECT_EQ(1u, apex_host_device_count(&l.host));
}

/* Disjoint ranges: INCOMPATIBLE, no hello ever, beaconing throttled to the
 * 0.1 Hz backoff; a compatible beacon revives discovery at once. */
TEST(BeaconNeg, DisjointBeaconIncompatibleBackoffThenRevive) {
    Bus bus{};
    apex_device_t dev{};
    apex_device_cfg_t dc = LoneDeviceCfg(bus);
    apex_device_init(&dev, &dc);
    apex_device_tick(&dev, 0);  // power-up beacon
    bus.device_to_host.clear();

    uint8_t bcn[APEX_MAX_ENCODED_FRAME_LENGTH];
    size_t bl = 0;
    ASSERT_EQ(APEX_OK, apex_beacon_build(2, 5, bcn, sizeof(bcn), &bl));
    apex_device_feed_rx(&dev, bcn, bl, 100);
    EXPECT_EQ(APEX_DEVICE_STATE_INCOMPATIBLE, apex_device_link_state(&dev));
    EXPECT_TRUE(bus.device_to_host.empty());  // no reactive output

    // Backoff: nothing before the 10 s window...
    apex_device_tick(&dev, 5000);
    EXPECT_TRUE(bus.device_to_host.empty());
    EXPECT_EQ(APEX_DEVICE_STATE_INCOMPATIBLE, apex_device_link_state(&dev));

    // ...then one beacon (and still no DEVICE_INFO — no mutual version).
    apex_device_tick(&dev, 10200);
    StreamStats st = ScanStream(bus.device_to_host);
    EXPECT_EQ(1, st.beacons);
    EXPECT_TRUE(st.msg_ids.empty());
    EXPECT_EQ(APEX_DEVICE_STATE_INCOMPATIBLE, apex_device_link_state(&dev));

    // A compatible beacon revives discovery immediately.
    bus.device_to_host.clear();
    ASSERT_EQ(APEX_OK, apex_beacon_build(1, 2, bcn, sizeof(bcn), &bl));
    apex_device_feed_rx(&dev, bcn, bl, 10300);
    EXPECT_EQ(APEX_DEVICE_STATE_DISCOVERING, apex_device_link_state(&dev));
    st = ScanStream(bus.device_to_host);
    EXPECT_TRUE(HasMsg(st, APEX_CFG_MSG_DEVICE_INFO));
    EXPECT_EQ(1u, dev.mutual_version);  // min(own_max=1, their_max=2)
}

/* Beaconing MUST stop once the session is established — and restarts on
 * re-enumeration (both sides). */
TEST(BeaconNeg, BeaconingStopsAtSessionAndRestartsOnReenum) {
    Link l;
    Connect(l, /*max_baud_code=*/0);
    ASSERT_EQ(APEX_DEVICE_STATE_CONNECTED, apex_device_link_state(&l.dev));
    l.bus.device_to_host.clear();
    l.bus.host_to_device.clear();

    // 5 s of established session: zero beacons in either direction.
    int beacons_up = 0, beacons_down = 0;
    for (int i = 0; i < 10; i++) {
        l.now_ms += 500;
        apex_device_tick(&l.dev, l.now_ms);
        apex_host_tick(&l.host, l.now_ms);
        beacons_up += ScanStream(l.bus.device_to_host).beacons;
        beacons_down += ScanStream(l.bus.host_to_device).beacons;
        auto u = std::move(l.bus.device_to_host); l.bus.device_to_host.clear();
        apex_host_feed_rx(&l.host, u.data(), u.size(), l.now_ms);
        auto d = std::move(l.bus.host_to_device); l.bus.host_to_device.clear();
        apex_device_feed_rx(&l.dev, d.data(), d.size(), l.now_ms);
    }
    EXPECT_EQ(0, beacons_up);
    EXPECT_EQ(0, beacons_down);

    // Re-enumeration: both sides return to discovery and beacon again.
    ASSERT_EQ(APEX_OK, apex_host_request_reenumeration(
                           &l.host, APEX_DEVICE_ID_BROADCAST));
    beacons_up = beacons_down = 0;
    for (int i = 0; i < 4; i++) {
        l.now_ms += 1;
        apex_device_tick(&l.dev, l.now_ms);
        apex_host_tick(&l.host, l.now_ms);
        beacons_up += ScanStream(l.bus.device_to_host).beacons;
        beacons_down += ScanStream(l.bus.host_to_device).beacons;
        auto u = std::move(l.bus.device_to_host); l.bus.device_to_host.clear();
        apex_host_feed_rx(&l.host, u.data(), u.size(), l.now_ms);
        auto d = std::move(l.bus.host_to_device); l.bus.host_to_device.clear();
        apex_device_feed_rx(&l.dev, d.data(), d.size(), l.now_ms);
    }
    EXPECT_GE(beacons_up, 1);
    EXPECT_GE(beacons_down, 1);

    // And the pair converges again.
    l.Pump(1, 6);
    EXPECT_EQ(APEX_DEVICE_STATE_CONNECTED, apex_device_link_state(&l.dev));
    EXPECT_EQ(1u, apex_host_device_count(&l.host));
}

/* ------------------------------------------------------------------------- */
/* Baud ladder (§3.4).                                                        */
/* ------------------------------------------------------------------------- */

TEST(BaudNeg, GrantEchoesCodeAndFiresSwitchCallbacks) {
    g_dev_result = BaudResult{}; g_host_switch = SwitchCap{};
    Link l;
    Connect(l, /*max_baud_code=*/APEX_BAUD_CODE_921600);  // host supports up to 2
    uint8_t id = apex_device_get_id(&l.dev);

    ASSERT_EQ(APEX_OK, apex_device_request_baud(&l.dev, APEX_BAUD_CODE_460800,
                                                APEX_BAUD_CODE_115200));
    l.Pump(1, 3);

    ASSERT_TRUE(g_dev_result.fired);
    EXPECT_TRUE(g_dev_result.granted);
    EXPECT_EQ(APEX_BAUD_CODE_460800, g_dev_result.code);  // echoed
    ASSERT_TRUE(g_host_switch.fired);
    EXPECT_EQ(id, g_host_switch.id);
    EXPECT_EQ(APEX_BAUD_CODE_460800, g_host_switch.code);
}

TEST(BaudNeg, CounterOfferHintLaddersDownToGrant) {
    g_dev_result = BaudResult{}; g_host_switch = SwitchCap{};
    Link l;
    Connect(l, /*max_baud_code=*/APEX_BAUD_CODE_460800);  // host supports up to 1

    // Ask for 2 (921600); floor is 1 (460800). Host rejects 2 with hint 1; the
    // device re-requests 1 and the host grants it.
    ASSERT_EQ(APEX_OK, apex_device_request_baud(&l.dev, APEX_BAUD_CODE_921600,
                                                APEX_BAUD_CODE_460800));
    l.Pump(1, 5);

    ASSERT_TRUE(g_dev_result.fired);
    EXPECT_TRUE(g_dev_result.granted);
    EXPECT_EQ(APEX_BAUD_CODE_460800, g_dev_result.code);
    ASSERT_TRUE(g_host_switch.fired);
    EXPECT_EQ(APEX_BAUD_CODE_460800, g_host_switch.code);
}

TEST(BaudNeg, RejectAtFloorReportsFailureAndNoSwitch) {
    g_dev_result = BaudResult{}; g_host_switch = SwitchCap{};
    Link l;
    Connect(l, /*max_baud_code=*/APEX_BAUD_CODE_115200);  // host supports default only

    // Ask for 2 with a floor of 2: the host's hint (0) is below the floor, so
    // the device gives up. No switch on either side.
    ASSERT_EQ(APEX_OK, apex_device_request_baud(&l.dev, APEX_BAUD_CODE_921600,
                                                APEX_BAUD_CODE_921600));
    l.Pump(1, 4);

    ASSERT_TRUE(g_dev_result.fired);
    EXPECT_FALSE(g_dev_result.granted);
    EXPECT_EQ(APEX_BAUD_CODE_115200, g_dev_result.code);  // still at default
    EXPECT_FALSE(g_host_switch.fired);
}

TEST(BaudNeg, RequestRejectedWhenNotConnected) {
    Bus bus{};
    apex_device_t dev{};
    apex_device_cfg_t dc = LoneDeviceCfg(bus);
    apex_device_init(&dev, &dc);
    // Still DISCOVERING — a baud request is a bad-state error.
    EXPECT_EQ(APEX_ERR_BAD_STATE,
              apex_device_request_baud(&dev, APEX_BAUD_CODE_460800,
                                       APEX_BAUD_CODE_115200));
}

/* ------------------------------------------------------------------------- */
/* RESET_REQUEST (msg 13) x raised baud: a session reset reverts the link to  */
/* the default rate on BOTH sides, via the existing rate-switch callbacks.    */
/* ------------------------------------------------------------------------- */

TEST(ReenumBaud, ResetRevertsRaisedRateBothSides) {
    g_dev_result = BaudResult{}; g_host_switch = SwitchCap{};
    Link l;
    Connect(l, /*max_baud_code=*/APEX_BAUD_CODE_921600);
    uint8_t id = apex_device_get_id(&l.dev);

    // Raise the session rate.
    ASSERT_EQ(APEX_OK, apex_device_request_baud(&l.dev, APEX_BAUD_CODE_921600,
                                                APEX_BAUD_CODE_115200));
    l.Pump(1, 3);
    ASSERT_TRUE(g_dev_result.fired);
    ASSERT_TRUE(g_dev_result.granted);
    ASSERT_EQ(APEX_BAUD_CODE_921600, g_dev_result.code);
    ASSERT_TRUE(g_host_switch.fired);
    ASSERT_EQ(APEX_BAUD_CODE_921600, g_host_switch.code);

    // Reset the session. The host reverts (and notifies) immediately after
    // transmitting the reset; the device reverts on receipt.
    g_dev_result = BaudResult{}; g_host_switch = SwitchCap{};
    ASSERT_EQ(APEX_OK, apex_host_request_reenumeration(
                           &l.host, APEX_DEVICE_ID_BROADCAST));
    ASSERT_TRUE(g_host_switch.fired);
    EXPECT_EQ(id, g_host_switch.id);  // the session that had raised the rate
    EXPECT_EQ(APEX_BAUD_CODE_115200, g_host_switch.code);

    l.Pump(1, 2);
    ASSERT_TRUE(g_dev_result.fired);
    EXPECT_TRUE(g_dev_result.granted);
    EXPECT_EQ(APEX_BAUD_CODE_115200, g_dev_result.code);

    // The pair re-enumerates back to CONNECTED at the default rate.
    l.Pump(1, 6);
    EXPECT_EQ(APEX_DEVICE_STATE_CONNECTED, apex_device_link_state(&l.dev));
    EXPECT_EQ(1u, apex_host_device_count(&l.host));
}

/* ------------------------------------------------------------------------- */
/* In-flight physical updates + advisory PHYS semantics.          */
/* ------------------------------------------------------------------------- */

struct PhysUpdateCap {
    int count = 0;
    uint8_t device_id = 0;
    apex_phys_t last{};
};
PhysUpdateCap g_phys_update;
void on_host_phys_update(void* u, uint8_t id, const apex_phys_t* p) {
    (void)u;
    g_phys_update.count++;
    g_phys_update.device_id = id;
    g_phys_update.last = *p;
}

struct PhysResultCap { int count = 0; bool last = false; };
PhysResultCap g_phys_result;
void on_dev_phys_result(void* u, bool delivered) {
    (void)u;
    g_phys_result.count++;
    g_phys_result.last = delivered;
}

int g_phys_advisory = 0;
void on_dev_phys_advisory(void* u) { (void)u; g_phys_advisory++; }

bool g_host_phys_accept = true;
bool host_phys_policy(void* u, uint8_t id, const apex_phys_t* p) {
    (void)u; (void)id; (void)p;
    return g_host_phys_accept;
}

struct HostClassCap { int count = 0; };
HostClassCap g_class_rx;
void host_class_counter(void* u, uint8_t id, const uint8_t* p, size_t n) {
    (void)u; (void)id; (void)p; (void)n;
    g_class_rx.count++;
}

/* Connect a PHYS-capable device (config-time mass 1200 g). */
void ConnectPhys(Link& l) {
    g_phys_update = PhysUpdateCap{};
    g_phys_result = PhysResultCap{};
    g_phys_advisory = 0;
    g_class_rx = HostClassCap{};

    apex_host_cfg_t hc{};
    hc.supported_interfaces = APEX_INTERFACE_FLAG_GPIO;
    hc.host_state_period_ms = 1000;
    hc.phys_policy_cb = host_phys_policy;
    hc.on_phys_update = on_host_phys_update;
    hc.tx = host_tx; hc.tx_user = &l.bus;
    apex_host_init(&l.host, &hc);
    ASSERT_EQ(APEX_OK, apex_host_register_class(&l.host, APEX_TRAFFIC_ACTIVATION,
                                                host_class_counter, &g_class_rx));

    apex_device_cfg_t dc{};
    dc.device_class = APEX_TRAFFIC_ACTIVATION;
    dc.interface_flags = APEX_INTERFACE_FLAG_GPIO;
    dc.mass_grams = 1200;
    dc.has_phys = true;
    dc.phys.cg_offset_z_mm = 10;
    dc.phys.ixx = 1000;
    dc.on_phys_update_result = on_dev_phys_result;
    dc.on_phys_advisory = on_dev_phys_advisory;
    dc.tx = device_tx; dc.tx_user = &l.bus;
    apex_device_init(&l.dev, &dc);

    l.Pump(1, 8);
    ASSERT_EQ(APEX_DEVICE_STATE_CONNECTED, apex_device_link_state(&l.dev));
}

apex_phys_t EdgePhys() {
    apex_phys_t p{};
    p.mass_grams = 65535;
    p.cg_offset_x_mm = -32768;
    p.cg_offset_y_mm = 32767;
    p.cg_offset_z_mm = -1;
    p.ixx = INT32_MIN;
    p.iyy = INT32_MAX;
    p.izz = -1;
    p.pxy = -2147483647;
    p.pxz = 2147483647;
    p.pyz = -12345678;
    return p;
}

TEST(PhysUpdate, RoundTrip33BytesSignedEdgesAndStore) {
    g_host_phys_accept = true;
    Link l;
    ConnectPhys(l);
    uint8_t id = apex_device_get_id(&l.dev);

    apex_phys_t p = EdgePhys();
    ASSERT_EQ(APEX_OK, apex_device_send_phys_update(&l.dev, &p));
    l.Pump(1, 3);

    // Host notification carried the exact values (33-byte LE round trip).
    ASSERT_EQ(1, g_phys_update.count);
    EXPECT_EQ(id, g_phys_update.device_id);
    EXPECT_EQ(65535u, g_phys_update.last.mass_grams);
    EXPECT_EQ(-32768, g_phys_update.last.cg_offset_x_mm);
    EXPECT_EQ(32767, g_phys_update.last.cg_offset_y_mm);
    EXPECT_EQ(-1, g_phys_update.last.cg_offset_z_mm);
    EXPECT_EQ(INT32_MIN, g_phys_update.last.ixx);
    EXPECT_EQ(INT32_MAX, g_phys_update.last.iyy);
    EXPECT_EQ(-1, g_phys_update.last.izz);
    EXPECT_EQ(-2147483647, g_phys_update.last.pxy);
    EXPECT_EQ(2147483647, g_phys_update.last.pxz);
    EXPECT_EQ(-12345678, g_phys_update.last.pyz);

    // Stored on the slot, latest-wins, mass mirrored.
    const apex_host_device_slot_t* slot = apex_host_get_device(&l.host, id);
    ASSERT_NE(nullptr, slot);
    EXPECT_TRUE(slot->has_phys);
    EXPECT_EQ(65535u, slot->mass_grams);
    EXPECT_EQ(INT32_MIN, slot->phys.ixx);

    // Receipt reached the device; no advisory; session intact.
    ASSERT_EQ(1, g_phys_result.count);
    EXPECT_TRUE(g_phys_result.last);
    EXPECT_EQ(0, g_phys_advisory);
    EXPECT_EQ(APEX_DEV_STATUS_CONNECTED, slot->status);
}

TEST(PhysUpdate, RetransmitUntilAck) {
    g_host_phys_accept = true;
    Link l;
    ConnectPhys(l);

    apex_phys_t p = EdgePhys();
    ASSERT_EQ(APEX_OK, apex_device_send_phys_update(&l.dev, &p));

    // Deliver the update to the host but DROP its PHYS_ACK.
    l.now_ms += 1;
    apex_device_tick(&l.dev, l.now_ms);
    apex_host_tick(&l.host, l.now_ms);
    {
        auto b = std::move(l.bus.device_to_host); l.bus.device_to_host.clear();
        apex_host_feed_rx(&l.host, b.data(), b.size(), l.now_ms);
    }
    l.bus.host_to_device.clear();  // ack lost
    EXPECT_EQ(1, g_phys_update.count);
    EXPECT_EQ(0, g_phys_result.count);  // no receipt yet

    // ~500 ms later the device retransmits; this time the ack gets through.
    l.Pump(500, 1);
    l.Pump(1, 2);
    EXPECT_EQ(2, g_phys_update.count);   // host saw the retransmit
    ASSERT_EQ(1, g_phys_result.count);   // exactly one outcome per update
    EXPECT_TRUE(g_phys_result.last);
}

TEST(PhysUpdate, FailureAfterThreeAttempts) {
    g_host_phys_accept = true;
    Link l;
    ConnectPhys(l);

    apex_phys_t p = EdgePhys();
    ASSERT_EQ(APEX_OK, apex_device_send_phys_update(&l.dev, &p));

    // Every host->device byte is lost from here: acks never arrive.
    for (int i = 0; i < 4; i++) {
        l.now_ms += 500;
        apex_device_tick(&l.dev, l.now_ms);
        apex_host_tick(&l.host, l.now_ms);
        auto b = std::move(l.bus.device_to_host); l.bus.device_to_host.clear();
        apex_host_feed_rx(&l.host, b.data(), b.size(), l.now_ms);
        l.bus.host_to_device.clear();
    }

    EXPECT_EQ(3, g_phys_update.count);   // 3 sends total (1 + 2 retransmits)
    ASSERT_EQ(1, g_phys_result.count);
    EXPECT_FALSE(g_phys_result.last);    // reported undelivered
    // Still CONNECTED — delivery failure is not a session event.
    EXPECT_EQ(APEX_DEVICE_STATE_CONNECTED, apex_device_link_state(&l.dev));
}

TEST(PhysUpdate, RateCapAndSingleInFlight) {
    g_host_phys_accept = true;
    Link l;
    ConnectPhys(l);

    apex_phys_t p = EdgePhys();
    ASSERT_EQ(APEX_OK, apex_device_send_phys_update(&l.dev, &p));
    // A second update while the first is in flight is rejected, not queued.
    EXPECT_EQ(APEX_ERR_BAD_STATE, apex_device_send_phys_update(&l.dev, &p));

    l.Pump(1, 3);  // first update acked
    ASSERT_EQ(1, g_phys_result.count);

    // Acked, but still inside the 500 ms rate window -> rejected (<= 2 Hz).
    EXPECT_EQ(APEX_ERR_BAD_STATE, apex_device_send_phys_update(&l.dev, &p));

    // Past the window -> accepted.
    l.Pump(600, 1);
    EXPECT_EQ(APEX_OK, apex_device_send_phys_update(&l.dev, &p));
}

TEST(PhysUpdate, AdvisoryRejectKeepsSessionAndState) {
    g_host_phys_accept = false;  // host policy dislikes the new state
    Link l;
    ConnectPhys(l);
    uint8_t id = apex_device_get_id(&l.dev);

    apex_phys_t p = EdgePhys();
    ASSERT_EQ(APEX_OK, apex_device_send_phys_update(&l.dev, &p));
    l.Pump(1, 3);

    // Stored state still updated (it is reality), notification fired.
    const apex_host_device_slot_t* slot = apex_host_get_device(&l.host, id);
    ASSERT_NE(nullptr, slot);
    EXPECT_EQ(1, g_phys_update.count);
    EXPECT_TRUE(slot->has_phys);
    EXPECT_EQ(65535u, slot->mass_grams);

    // Device saw the advisory; the reject is still a delivery receipt.
    EXPECT_EQ(1, g_phys_advisory);
    ASSERT_EQ(1, g_phys_result.count);
    EXPECT_TRUE(g_phys_result.last);

    // No session effect on either side; class traffic still flows.
    EXPECT_EQ(APEX_DEVICE_STATE_CONNECTED, apex_device_link_state(&l.dev));
    EXPECT_EQ(APEX_DEV_STATUS_CONNECTED, slot->status);
    uint8_t msg[2] = {0xAB, 0xCD};
    ASSERT_EQ(APEX_OK, apex_device_send(&l.dev, msg, sizeof(msg)));
    l.Pump(1, 2);
    EXPECT_EQ(1, g_class_rx.count);
    g_host_phys_accept = true;
}

TEST(PhysUpdate, ReenumerationRedeclaresCurrentMass) {
    g_host_phys_accept = true;
    Link l;
    ConnectPhys(l);
    uint8_t id = apex_device_get_id(&l.dev);
    ASSERT_EQ(1200u, apex_host_get_device(&l.host, id)->mass_grams);

    // The dispenser drops half its load...
    apex_phys_t p = EdgePhys();
    p.mass_grams = 600;
    ASSERT_EQ(APEX_OK, apex_device_send_phys_update(&l.dev, &p));
    l.Pump(1, 3);
    ASSERT_EQ(600u, apex_host_get_device(&l.host, id)->mass_grams);

    // ...then the session resets (brown-out). The re-discovery DEVICE_INFO
    // must declare the CURRENT 600 g, not the config-time 1200 g.
    ASSERT_EQ(APEX_OK, apex_host_request_reenumeration(&l.host, id));
    l.Pump(1, 8);
    ASSERT_EQ(APEX_DEVICE_STATE_CONNECTED, apex_device_link_state(&l.dev));
    uint8_t new_id = apex_device_get_id(&l.dev);
    const apex_host_device_slot_t* slot = apex_host_get_device(&l.host, new_id);
    ASSERT_NE(nullptr, slot);
    EXPECT_EQ(600u, slot->mass_grams);  // DEVICE_INFO carried current mass
}

}  // namespace
