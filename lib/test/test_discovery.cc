/* End-to-end discovery / provisional-configuration tests for APEX wire v1.
 *
 * We stand up a host and a device in the same process and loopback-connect
 * their TX callbacks to each other's feed_rx, then advance both through their
 * tick functions. Covers: the simple three-leg handshake, the provisional
 * phase (PHYS accepted / rejected / timeout-denied), class-version selection,
 * mass policy, dedup + promotion, and slot recycling.
 */
#include "apex/apex_device.h"
#include "apex/apex_host.h"

#include <gtest/gtest.h>

#include <vector>

namespace {

/* First id the host hands out (monotonic from the 0x02–0xFE pool). */
constexpr uint8_t kFirstId = APEX_DEVICE_ID_ASSIGNED_MIN;  /* 0x02 */

struct Bus {
    std::vector<uint8_t> host_to_device;
    std::vector<uint8_t> device_to_host;
};

void host_tx(void* user, const uint8_t* bytes, size_t n) {
    auto* bus = static_cast<Bus*>(user);
    bus->host_to_device.insert(bus->host_to_device.end(), bytes, bytes + n);
}
void device_tx(void* user, const uint8_t* bytes, size_t n) {
    auto* bus = static_cast<Bus*>(user);
    bus->device_to_host.insert(bus->device_to_host.end(), bytes, bytes + n);
}

struct ClassRxCapture {
    uint8_t device_id = 0;
    std::vector<uint8_t> last_payload;
    int call_count = 0;
};
void host_class_rx(void* user, uint8_t device_id,
                   const uint8_t* payload, size_t payload_len) {
    auto* c = static_cast<ClassRxCapture*>(user);
    c->device_id = device_id;
    c->last_payload.assign(payload, payload + payload_len);
    c->call_count++;
}

struct DeviceEventCapture {
    uint8_t device_id = 0;
    apex_device_status_t last_status = APEX_DEV_STATUS_UNKNOWN;
    int connected_count = 0;
    int call_count = 0;
};
void on_dev_event(void* user, uint8_t id, apex_device_status_t status) {
    auto* c = static_cast<DeviceEventCapture*>(user);
    c->device_id = id;
    c->last_status = status;
    c->call_count++;
    if (status == APEX_DEV_STATUS_CONNECTED) c->connected_count++;
}

/* A configurable host+device loopback link. Set the knobs, call Init(), then
 * Pump(). */
struct Link {
    Bus bus{};
    apex_host_t host{};
    apex_device_t dev{};
    ClassRxCapture host_rx{};
    DeviceEventCapture dev_events{};
    uint32_t now_ms = 0;

    void Init(const apex_host_cfg_t& hc, const apex_device_cfg_t& dc) {
        apex_host_init(&host, &hc);
        apex_device_init(&dev, &dc);
    }

    void Pump(uint32_t advance_ms = 1, int rounds = 1) {
        for (int i = 0; i < rounds; i++) {
            now_ms += advance_ms;
            apex_device_tick(&dev, now_ms);
            apex_host_tick(&host, now_ms);
            if (!bus.device_to_host.empty()) {
                std::vector<uint8_t> b = std::move(bus.device_to_host);
                bus.device_to_host.clear();
                apex_host_feed_rx(&host, b.data(), b.size(), now_ms);
            }
            if (!bus.host_to_device.empty()) {
                std::vector<uint8_t> b = std::move(bus.host_to_device);
                bus.host_to_device.clear();
                apex_device_feed_rx(&dev, b.data(), b.size(), now_ms);
            }
        }
    }

    /* Advance the host alone (device vanished / silent). */
    void PumpHostOnly(uint32_t advance_ms, int rounds) {
        for (int i = 0; i < rounds; i++) {
            now_ms += advance_ms;
            bus.device_to_host.clear();
            apex_host_tick(&host, now_ms);
            bus.host_to_device.clear();
        }
    }
};

apex_host_cfg_t BaseHostCfg(Link& l) {
    apex_host_cfg_t hc{};
    hc.supported_interfaces = APEX_INTERFACE_FLAG_GPIO | APEX_INTERFACE_FLAG_USB;
    hc.host_state_period_ms = 1000;
    hc.tx = host_tx;
    hc.tx_user = &l.bus;
    hc.on_device_event = on_dev_event;
    hc.on_device_event_user = &l.dev_events;
    return hc;
}

apex_device_cfg_t BaseDeviceCfg(Link& l) {
    apex_device_cfg_t dc{};
    dc.device_class = APEX_TRAFFIC_ACTIVATION;
    dc.interface_flags = APEX_INTERFACE_FLAG_GPIO;
    dc.class_version_min = 1;
    dc.class_version_max = 1;
    dc.mass_grams = 1200;
    dc.tx = device_tx;
    dc.tx_user = &l.bus;
    return dc;
}

/* Close the version tier by hand: both sides beacon on their first tick;
 * each receives the other's. The device opens immediately on the host's
 * beacon, so afterwards bus.device_to_host holds its first DEVICE_INFO. */
void CloseVersionTier(Link& l) {
    apex_device_tick(&l.dev, l.now_ms);
    apex_host_tick(&l.host, l.now_ms);
    {
        auto b = std::move(l.bus.device_to_host); l.bus.device_to_host.clear();
        apex_host_feed_rx(&l.host, b.data(), b.size(), l.now_ms);
    }
    {
        auto b = std::move(l.bus.host_to_device); l.bus.host_to_device.clear();
        apex_device_feed_rx(&l.dev, b.data(), b.size(), l.now_ms);
    }
}

/* ------------------------------------------------------------------------- */
/* Simple three-leg handshake (no provisional phase).                        */
/* ------------------------------------------------------------------------- */

TEST(DiscoveryV1, SimpleHandshakeReachesConnected) {
    Link l;
    apex_host_cfg_t hc = BaseHostCfg(l);
    apex_device_cfg_t dc = BaseDeviceCfg(l);
    l.Init(hc, dc);
    ASSERT_EQ(APEX_OK, apex_host_register_class(&l.host, APEX_TRAFFIC_ACTIVATION,
                                                host_class_rx, &l.host_rx));

    l.Pump(1, 6);

    EXPECT_EQ(APEX_DEVICE_STATE_CONNECTED, apex_device_link_state(&l.dev));
    EXPECT_EQ(kFirstId, apex_device_get_id(&l.dev));
    EXPECT_EQ(1u, apex_device_selected_class_version(&l.dev));

    const apex_host_device_slot_t* slot = apex_host_get_device(&l.host, kFirstId);
    ASSERT_NE(nullptr, slot);
    EXPECT_EQ(APEX_DEV_STATUS_CONNECTED, slot->status);
    EXPECT_EQ(APEX_TRAFFIC_ACTIVATION, slot->device_class);
    EXPECT_EQ(APEX_INTERFACE_FLAG_GPIO, slot->interface_flags);
    EXPECT_EQ(1u, slot->selected_class_version);
    EXPECT_EQ(1200u, slot->mass_grams);
    EXPECT_EQ(1, l.dev_events.connected_count);
}

TEST(DiscoveryV1, ClassTrafficFlowsAfterConnect) {
    Link l;
    apex_host_cfg_t hc = BaseHostCfg(l);
    apex_device_cfg_t dc = BaseDeviceCfg(l);
    l.Init(hc, dc);
    ASSERT_EQ(APEX_OK, apex_host_register_class(&l.host, APEX_TRAFFIC_ACTIVATION,
                                                host_class_rx, &l.host_rx));
    l.Pump(1, 6);
    ASSERT_EQ(APEX_DEVICE_STATE_CONNECTED, apex_device_link_state(&l.dev));

    uint8_t msg[3] = {0xAA, 0xBB, 0xCC};
    ASSERT_EQ(APEX_OK, apex_device_send(&l.dev, msg, sizeof(msg)));
    l.Pump(1, 2);
    EXPECT_EQ(1, l.host_rx.call_count);
    EXPECT_EQ(kFirstId, l.host_rx.device_id);
    ASSERT_EQ(3u, l.host_rx.last_payload.size());
    EXPECT_EQ(0xAA, l.host_rx.last_payload[0]);
}

/* ------------------------------------------------------------------------- */
/* HOST_STATE + watchdogs.                                                    */
/* ------------------------------------------------------------------------- */

TEST(DiscoveryV1, HostStateBroadcastReachesDevice) {
    Link l;
    bool seen = false;
    apex_flight_state_t seen_state = APEX_FLIGHT_STATE_UNKNOWN;
    struct Cap { bool* seen; apex_flight_state_t* st; } cap{&seen, &seen_state};

    apex_host_cfg_t hc = BaseHostCfg(l);
    apex_device_cfg_t dc = BaseDeviceCfg(l);
    dc.on_host_state = [](void* u, apex_flight_state_t s) {
        auto* c = static_cast<Cap*>(u);
        *c->seen = true; *c->st = s;
    };
    dc.on_host_state_user = &cap;
    l.Init(hc, dc);
    ASSERT_EQ(APEX_OK, apex_host_register_class(&l.host, APEX_TRAFFIC_ACTIVATION,
                                                host_class_rx, &l.host_rx));
    l.Pump(1, 6);
    ASSERT_EQ(APEX_DEVICE_STATE_CONNECTED, apex_device_link_state(&l.dev));

    apex_host_set_flight_state(&l.host, APEX_FLIGHT_STATE_PROPS_ON_FLYING);
    l.Pump(1100, 1);
    l.Pump(1, 2);
    EXPECT_TRUE(seen);
    EXPECT_EQ(APEX_FLIGHT_STATE_PROPS_ON_FLYING, seen_state);
}

TEST(DiscoveryV1, DeviceWatchdogResetsOnHostSilence) {
    Link l;
    apex_host_cfg_t hc = BaseHostCfg(l);
    apex_device_cfg_t dc = BaseDeviceCfg(l);
    l.Init(hc, dc);
    ASSERT_EQ(APEX_OK, apex_host_register_class(&l.host, APEX_TRAFFIC_ACTIVATION,
                                                host_class_rx, &l.host_rx));
    l.Pump(1, 6);
    ASSERT_EQ(APEX_DEVICE_STATE_CONNECTED, apex_device_link_state(&l.dev));

    for (int i = 0; i < 6; i++) {
        l.now_ms += 1000;
        apex_host_tick(&l.host, l.now_ms);
        l.bus.host_to_device.clear();  // host gone silent
        apex_device_tick(&l.dev, l.now_ms);
    }
    EXPECT_EQ(APEX_DEVICE_STATE_DISCOVERING, apex_device_link_state(&l.dev));
}

TEST(DiscoveryV1, HostWatchdogMarksDeviceFault) {
    Link l;
    apex_host_cfg_t hc = BaseHostCfg(l);
    apex_device_cfg_t dc = BaseDeviceCfg(l);
    l.Init(hc, dc);
    ASSERT_EQ(APEX_OK, apex_host_register_class(&l.host, APEX_TRAFFIC_ACTIVATION,
                                                host_class_rx, &l.host_rx));
    l.Pump(1, 6);
    uint8_t did = apex_device_get_id(&l.dev);
    ASSERT_EQ(kFirstId, did);

    l.PumpHostOnly(1000, 6);
    const apex_host_device_slot_t* slot = apex_host_get_device(&l.host, did);
    ASSERT_NE(nullptr, slot);
    EXPECT_EQ(APEX_DEV_STATUS_FAULT, slot->status);
}

TEST(DiscoveryV1, FaultSlotRecycledAfterDelay) {
    Link l;
    apex_host_cfg_t hc = BaseHostCfg(l);
    apex_device_cfg_t dc = BaseDeviceCfg(l);
    l.Init(hc, dc);
    ASSERT_EQ(APEX_OK, apex_host_register_class(&l.host, APEX_TRAFFIC_ACTIVATION,
                                                host_class_rx, &l.host_rx));
    l.Pump(1, 6);
    uint8_t did = apex_device_get_id(&l.dev);
    ASSERT_EQ(1u, apex_host_device_count(&l.host));

    l.PumpHostOnly(1000, 6);
    EXPECT_EQ(APEX_DEV_STATUS_FAULT, apex_host_get_device(&l.host, did)->status);

    l.PumpHostOnly(1000, 6);
    EXPECT_EQ(0u, apex_host_device_count(&l.host));
    EXPECT_EQ(nullptr, apex_host_get_device(&l.host, did));
}

/* ------------------------------------------------------------------------- */
/* Terminal rejects.                                                          */
/* ------------------------------------------------------------------------- */

TEST(DiscoveryV1, RejectClassWhenHostHasNoHandler) {
    Link l;
    apex_host_cfg_t hc = BaseHostCfg(l);
    apex_device_cfg_t dc = BaseDeviceCfg(l);
    l.Init(hc, dc);  // no class registered
    l.Pump(1, 4);
    EXPECT_EQ(APEX_DEVICE_STATE_REJECTED, apex_device_link_state(&l.dev));
    EXPECT_EQ(APEX_ACK_REJECT_CLASS, apex_device_reject_reason(&l.dev));
}

TEST(DiscoveryV1, RejectInterfaceIsRetryable) {
    Link l;
    apex_host_cfg_t hc = BaseHostCfg(l);
    hc.supported_interfaces = APEX_INTERFACE_FLAG_GPIO;  // no USB
    apex_device_cfg_t dc = BaseDeviceCfg(l);
    dc.interface_flags = APEX_INTERFACE_FLAG_USB;        // host cannot grant
    l.Init(hc, dc);
    ASSERT_EQ(APEX_OK, apex_host_register_class(&l.host, APEX_TRAFFIC_ACTIVATION,
                                                host_class_rx, &l.host_rx));
    l.Pump(1, 4);
    // Retryable: device stays DISCOVERING (not a terminal REJECTED).
    EXPECT_EQ(APEX_DEVICE_STATE_DISCOVERING, apex_device_link_state(&l.dev));
    EXPECT_EQ(APEX_ACK_REJECT_INTERFACE, apex_device_reject_reason(&l.dev));
}

TEST(DiscoveryV1, RejectClassVersionOnDisjointRanges) {
    Link l;
    apex_host_cfg_t hc = BaseHostCfg(l);
    apex_device_cfg_t dc = BaseDeviceCfg(l);
    dc.class_version_min = 5;
    dc.class_version_max = 7;
    l.Init(hc, dc);
    // Host supports class versions 1..3 — disjoint from the device's 5..7.
    ASSERT_EQ(APEX_OK, apex_host_register_class_versioned(
                           &l.host, APEX_TRAFFIC_ACTIVATION, 1, 3,
                           host_class_rx, &l.host_rx));
    l.Pump(1, 4);
    EXPECT_EQ(APEX_DEVICE_STATE_REJECTED, apex_device_link_state(&l.dev));
    EXPECT_EQ(APEX_ACK_REJECT_CLASS_VERSION, apex_device_reject_reason(&l.dev));
    // Diagnostic host range surfaced to the device.
    EXPECT_EQ(1u, l.dev.host_class_min);
    EXPECT_EQ(3u, l.dev.host_class_max);
}

TEST(DiscoveryV1, ClassVersionSelectionPicksMinOfMaxes) {
    Link l;
    apex_host_cfg_t hc = BaseHostCfg(l);
    apex_device_cfg_t dc = BaseDeviceCfg(l);
    dc.class_version_min = 2;
    dc.class_version_max = 6;         // device supports 2..6
    l.Init(hc, dc);
    ASSERT_EQ(APEX_OK, apex_host_register_class_versioned(
                           &l.host, APEX_TRAFFIC_ACTIVATION, 3, 4,
                           host_class_rx, &l.host_rx));  // host 3..4
    l.Pump(1, 6);
    ASSERT_EQ(APEX_DEVICE_STATE_CONNECTED, apex_device_link_state(&l.dev));
    // min(dev_max=6, host_max=4) = 4.
    EXPECT_EQ(4u, apex_device_selected_class_version(&l.dev));
    EXPECT_EQ(4u, apex_host_get_device(&l.host, kFirstId)->selected_class_version);
}

static bool reject_heavy(void* user, uint16_t mass_grams) {
    (void)user;
    return mass_grams <= 1000;  // reject anything over 1 kg
}

TEST(DiscoveryV1, RejectMassViaPolicy) {
    Link l;
    apex_host_cfg_t hc = BaseHostCfg(l);
    hc.mass_policy_cb = reject_heavy;
    apex_device_cfg_t dc = BaseDeviceCfg(l);
    dc.mass_grams = 5000;  // 5 kg — over the limit
    l.Init(hc, dc);
    ASSERT_EQ(APEX_OK, apex_host_register_class(&l.host, APEX_TRAFFIC_ACTIVATION,
                                                host_class_rx, &l.host_rx));
    l.Pump(1, 4);
    EXPECT_EQ(APEX_DEVICE_STATE_REJECTED, apex_device_link_state(&l.dev));
    EXPECT_EQ(APEX_ACK_REJECT_MASS, apex_device_reject_reason(&l.dev));
    EXPECT_EQ(0u, apex_host_device_count(&l.host));
}

/* ------------------------------------------------------------------------- */
/* Dedup + promotion.                                                         */
/* ------------------------------------------------------------------------- */

TEST(DiscoveryV1, RepeatedDeviceInfoDedupsToOneSlot) {
    Link l;
    apex_host_cfg_t hc = BaseHostCfg(l);
    apex_device_cfg_t dc = BaseDeviceCfg(l);
    l.Init(hc, dc);
    ASSERT_EQ(APEX_OK, apex_host_register_class(&l.host, APEX_TRAFFIC_ACTIVATION,
                                                host_class_rx, &l.host_rx));

    // Mutual beacons close the version tier; the device's first
    // DEVICE_INFO is now on the wire. The host assigns a provisional slot.
    CloseVersionTier(l);
    {
        auto b = std::move(l.bus.device_to_host); l.bus.device_to_host.clear();
        apex_host_feed_rx(&l.host, b.data(), b.size(), l.now_ms);
    }
    l.bus.host_to_device.clear();  // DROP the CONFIG_REPLY — device never latches

    ASSERT_EQ(1u, apex_host_device_count(&l.host));
    EXPECT_EQ(APEX_DEV_STATUS_PROVISIONAL,
              apex_host_get_device(&l.host, kFirstId)->status);

    for (int i = 0; i < 10; i++) {
        l.now_ms += 250;
        apex_device_tick(&l.dev, l.now_ms);
        apex_host_tick(&l.host, l.now_ms);
        auto b = std::move(l.bus.device_to_host); l.bus.device_to_host.clear();
        apex_host_feed_rx(&l.host, b.data(), b.size(), l.now_ms);
        l.bus.host_to_device.clear();  // still dropping replies
        EXPECT_EQ(1u, apex_host_device_count(&l.host));  // one slot, ever
    }
    EXPECT_EQ(APEX_DEVICE_STATE_DISCOVERING, apex_device_link_state(&l.dev));

    // Let a reply through: the device's next retransmit is answered, it
    // latches, and CONFIG_ACK promotes the slot.
    l.Pump(250, 5);
    EXPECT_EQ(APEX_DEVICE_STATE_CONNECTED, apex_device_link_state(&l.dev));
    EXPECT_EQ(1u, apex_host_device_count(&l.host));
    EXPECT_EQ(APEX_DEV_STATUS_CONNECTED,
              apex_host_get_device(&l.host, apex_device_get_id(&l.dev))->status);
}

TEST(DiscoveryV1, ConfigAckLostHeartbeatFallbackPromotes) {
    Link l;
    apex_host_cfg_t hc = BaseHostCfg(l);
    apex_device_cfg_t dc = BaseDeviceCfg(l);
    l.Init(hc, dc);
    ASSERT_EQ(APEX_OK, apex_host_register_class(&l.host, APEX_TRAFFIC_ACTIVATION,
                                                host_class_rx, &l.host_rx));

    CloseVersionTier(l);  // mutual beacons; DEVICE_INFO now on the wire
    {
        auto b = std::move(l.bus.device_to_host); l.bus.device_to_host.clear();
        apex_host_feed_rx(&l.host, b.data(), b.size(), l.now_ms);  // assign + reply
    }
    {
        auto b = std::move(l.bus.host_to_device); l.bus.host_to_device.clear();
        apex_device_feed_rx(&l.dev, b.data(), b.size(), l.now_ms);  // latch + CONFIG_ACK
    }
    ASSERT_EQ(APEX_DEVICE_STATE_CONNECTED, apex_device_link_state(&l.dev));
    uint8_t did = apex_device_get_id(&l.dev);

    l.bus.device_to_host.clear();  // DROP the CONFIG_ACK
    EXPECT_EQ(APEX_DEV_STATUS_PROVISIONAL,
              apex_host_get_device(&l.host, did)->status);

    // ~1 s later the device's heartbeat (assigned id) promotes it.
    l.now_ms += 1000;
    apex_device_tick(&l.dev, l.now_ms);
    apex_host_tick(&l.host, l.now_ms);
    {
        auto b = std::move(l.bus.device_to_host); l.bus.device_to_host.clear();
        apex_host_feed_rx(&l.host, b.data(), b.size(), l.now_ms);
    }
    EXPECT_EQ(APEX_DEV_STATUS_CONNECTED,
              apex_host_get_device(&l.host, did)->status);
}

TEST(DiscoveryV1, ProvisionalSlotRecycledWhenDeviceVanishes) {
    Link l;
    apex_host_cfg_t hc = BaseHostCfg(l);
    apex_device_cfg_t dc = BaseDeviceCfg(l);
    l.Init(hc, dc);
    ASSERT_EQ(APEX_OK, apex_host_register_class(&l.host, APEX_TRAFFIC_ACTIVATION,
                                                host_class_rx, &l.host_rx));

    CloseVersionTier(l);  // mutual beacons; DEVICE_INFO now on the wire
    {
        auto b = std::move(l.bus.device_to_host); l.bus.device_to_host.clear();
        apex_host_feed_rx(&l.host, b.data(), b.size(), l.now_ms);
    }
    l.bus.host_to_device.clear();  // drop reply; assigned but not latched
    ASSERT_EQ(1u, apex_host_device_count(&l.host));
    ASSERT_EQ(APEX_DEV_STATUS_PROVISIONAL,
              apex_host_get_device(&l.host, kFirstId)->status);

    // Device vanishes entirely. After the 5 s watchdog the slot is freed.
    l.PumpHostOnly(1000, 6);
    EXPECT_EQ(0u, apex_host_device_count(&l.host));
    EXPECT_EQ(nullptr, apex_host_get_device(&l.host, kFirstId));
}

/* ------------------------------------------------------------------------- */
/* Provisional configuration phase with a PHYS query.                         */
/* ------------------------------------------------------------------------- */

apex_phys_t SamplePhys() {
    apex_phys_t p{};
    p.cg_offset_x_mm = 1; p.cg_offset_y_mm = -2; p.cg_offset_z_mm = 30;
    p.ixx = 100000; p.iyy = 200000; p.izz = 300000;
    p.pxy = -50; p.pxz = 60; p.pyz = -70;
    return p;
}

static apex_phys_t g_seen_phys{};
static int g_phys_calls = 0;
static bool accept_phys(void* user, uint8_t id, const apex_phys_t* p) {
    (void)user; (void)id;
    g_seen_phys = *p;
    g_phys_calls++;
    return true;
}
static bool reject_phys(void* user, uint8_t id, const apex_phys_t* p) {
    (void)user; (void)id; (void)p;
    g_phys_calls++;
    return false;
}

TEST(DiscoveryV1, ProvisionalPhaseWithPhysAccepted) {
    g_phys_calls = 0; g_seen_phys = apex_phys_t{};
    Link l;
    apex_host_cfg_t hc = BaseHostCfg(l);
    hc.run_phys_provisional = true;
    hc.phys_policy_cb = accept_phys;
    apex_device_cfg_t dc = BaseDeviceCfg(l);
    dc.has_phys = true;
    dc.phys = SamplePhys();
    l.Init(hc, dc);
    ASSERT_EQ(APEX_OK, apex_host_register_class(&l.host, APEX_TRAFFIC_ACTIVATION,
                                                host_class_rx, &l.host_rx));

    l.Pump(1, 10);

    EXPECT_EQ(APEX_DEVICE_STATE_CONNECTED, apex_device_link_state(&l.dev));
    EXPECT_EQ(kFirstId, apex_device_get_id(&l.dev));
    EXPECT_EQ(APEX_DEV_STATUS_CONNECTED,
              apex_host_get_device(&l.host, kFirstId)->status);
    EXPECT_EQ(1, g_phys_calls);
    // The 33-byte PHYS values round-tripped (LE serialize/parse, signed
    // fields). At config time PHYS_INFO's mass duplicates DEVICE_INFO's
    // — cfg.phys.mass_grams is ignored, cfg.mass_grams rules.
    EXPECT_EQ(1200u, g_seen_phys.mass_grams);
    EXPECT_EQ(1, g_seen_phys.cg_offset_x_mm);
    EXPECT_EQ(-2, g_seen_phys.cg_offset_y_mm);
    EXPECT_EQ(30, g_seen_phys.cg_offset_z_mm);
    EXPECT_EQ(100000, g_seen_phys.ixx);
    EXPECT_EQ(300000, g_seen_phys.izz);
    EXPECT_EQ(-50, g_seen_phys.pxy);
    EXPECT_EQ(-70, g_seen_phys.pyz);
    // The host stored the canonical state on the slot (latest-wins consumer).
    const apex_host_device_slot_t* slot = apex_host_get_device(&l.host, kFirstId);
    ASSERT_NE(nullptr, slot);
    EXPECT_TRUE(slot->has_phys);
    EXPECT_EQ(1200u, slot->mass_grams);
    EXPECT_EQ(-50, slot->phys.pxy);
}

TEST(DiscoveryV1, ProvisionalPhaseWithPhysRejectedValues) {
    g_phys_calls = 0;
    Link l;
    apex_host_cfg_t hc = BaseHostCfg(l);
    hc.run_phys_provisional = true;
    hc.phys_policy_cb = reject_phys;
    apex_device_cfg_t dc = BaseDeviceCfg(l);
    dc.has_phys = true;
    dc.phys = SamplePhys();
    l.Init(hc, dc);
    ASSERT_EQ(APEX_OK, apex_host_register_class(&l.host, APEX_TRAFFIC_ACTIVATION,
                                                host_class_rx, &l.host_rx));

    l.Pump(1, 10);

    EXPECT_EQ(1, g_phys_calls);
    EXPECT_EQ(APEX_DEVICE_STATE_REJECTED, apex_device_link_state(&l.dev));
    EXPECT_EQ(APEX_ACK_REJECT_PHYS, apex_device_reject_reason(&l.dev));
    // Pre-commitment reject frees the slot immediately (no teardown).
    EXPECT_EQ(0u, apex_host_device_count(&l.host));
}

TEST(DiscoveryV1, PhysTimeoutDeniedThenPolicyRejectOnRediscovery) {
    Link l;
    apex_host_cfg_t hc = BaseHostCfg(l);
    // HOST_STATE broadcasts stay ON (1 Hz, BaseHostCfg default): per ruling
    // Pre-CONNECTED broadcasts do not feed the device's host-liveliness
    // watchdog, so the abandoned device must time out even while the host
    // keeps broadcasting.
    hc.run_phys_provisional = true;
    hc.phys_required = true;      // device MUST answer PHYS
    apex_device_cfg_t dc = BaseDeviceCfg(l);
    dc.has_phys = false;          // device does not implement PHYS → never answers
    l.Init(hc, dc);
    ASSERT_EQ(APEX_OK, apex_host_register_class(&l.host, APEX_TRAFFIC_ACTIVATION,
                                                host_class_rx, &l.host_rx));

    // Discover → ACK_PROVISIONAL → host retries PHYS_REQUEST 3 × 500 ms →
    // exhausts, sends the courtesy PHYS_ACK(REJECT_PHYS) (which this non-PHYS
    // device ignores), arms the deny latch, and frees the slot.
    l.Pump(200, 12);
    EXPECT_EQ(0u, apex_host_device_count(&l.host));  // slot freed

    // The host now sends the device nothing but broadcasts; ~5 s after the
    // last addressed frame the device's watchdog resets it to discovery. Its
    // next DEVICE_INFO is met with a terminal ACK_REJECT_POLICY.
    l.Pump(200, 40);
    EXPECT_EQ(APEX_DEVICE_STATE_REJECTED, apex_device_link_state(&l.dev));
    EXPECT_EQ(APEX_ACK_REJECT_POLICY, apex_device_reject_reason(&l.dev));
    EXPECT_EQ(0u, apex_host_device_count(&l.host));
}

TEST(DiscoveryV1, PhysTimeoutOptionalConcludesOk) {
    Link l;
    apex_host_cfg_t hc = BaseHostCfg(l);
    hc.run_phys_provisional = true;
    hc.phys_required = false;  // phys optional: timeout still concludes OK
    apex_device_cfg_t dc = BaseDeviceCfg(l);
    dc.has_phys = false;
    l.Init(hc, dc);
    ASSERT_EQ(APEX_OK, apex_host_register_class(&l.host, APEX_TRAFFIC_ACTIVATION,
                                                host_class_rx, &l.host_rx));

    l.Pump(200, 14);
    EXPECT_EQ(APEX_DEVICE_STATE_CONNECTED, apex_device_link_state(&l.dev));
    EXPECT_EQ(APEX_DEV_STATUS_CONNECTED,
              apex_host_get_device(&l.host, kFirstId)->status);
}

/* ------------------------------------------------------------------------- */
/* Ruling: pre-CONNECTED, only addressed frames feed the device's         */
/* host-liveliness watchdog; broadcasts do not.                               */
/* ------------------------------------------------------------------------- */

/* Forge an encoded host→device CONFIG frame with the given destination id. */
std::vector<uint8_t> ForgeHostFrame(uint8_t device_id,
                                    const std::vector<uint8_t>& payload) {
    apex_hdr_t hdr{};
    hdr.protocol_version = APEX_PROTOCOL_VERSION;
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

/* Stand a device up alone and drive it into PROVISIONAL with a forged
 * CONFIG_REPLY(ACK_PROVISIONAL). */
void EnterProvisional(Link& l) {
    apex_device_cfg_t dc = BaseDeviceCfg(l);
    apex_device_init(&l.dev, &dc);
    apex_device_tick(&l.dev, 0);  // power-up beacon
    // A fresh device consumes NOTHING before the host's beacon — close
    // the version tier first, or the forged CONFIG_REPLY below is dropped.
    {
        uint8_t bcn[APEX_MAX_ENCODED_FRAME_LENGTH];
        size_t bl = 0;
        ASSERT_EQ(APEX_OK, apex_beacon_build(1, 1, bcn, sizeof(bcn), &bl));
        apex_device_feed_rx(&l.dev, bcn, bl, 0);
    }
    l.bus.device_to_host.clear();  // discard the beacon + immediate DEVICE_INFO
    auto reply = ForgeHostFrame(
        APEX_DEVICE_ID_UNASSIGNED,
        {APEX_CFG_MSG_CONFIG_REPLY, APEX_ACK_PROVISIONAL, 0, 1, 1, 1});
    apex_device_feed_rx(&l.dev, reply.data(), reply.size(), 0);
    ASSERT_EQ(APEX_DEVICE_STATE_PROVISIONAL, apex_device_link_state(&l.dev));
}

TEST(DiscoveryV1, BroadcastsDoNotFeedProvisionalWatchdog) {
    // Part 1: a provisional device on a broadcast-only diet for > 5 s reverts
    // to discovery — HOST_STATE at 0xFF does not count as host liveliness.
    Link l;
    EnterProvisional(l);
    auto bcast = ForgeHostFrame(
        APEX_DEVICE_ID_BROADCAST,
        {APEX_CFG_MSG_HOST_STATE, APEX_FLIGHT_STATE_STANDBY});
    for (int s = 1; s <= 6; s++) {
        uint32_t now = (uint32_t)s * 1000;
        apex_device_feed_rx(&l.dev, bcast.data(), bcast.size(), now);
        apex_device_tick(&l.dev, now);
        l.bus.device_to_host.clear();
    }
    EXPECT_EQ(APEX_DEVICE_STATE_DISCOVERING, apex_device_link_state(&l.dev));

    // Part 2: the same diet plus addressed frames (id 0x01) keeps the device
    // in PROVISIONAL — addressed traffic still feeds the watchdog.
    Link l2;
    EnterProvisional(l2);
    auto addressed = ForgeHostFrame(APEX_DEVICE_ID_UNASSIGNED, {});  // heartbeat
    for (int s = 1; s <= 6; s++) {
        uint32_t now = (uint32_t)s * 1000;
        apex_device_feed_rx(&l2.dev, bcast.data(), bcast.size(), now);
        apex_device_feed_rx(&l2.dev, addressed.data(), addressed.size(), now);
        apex_device_tick(&l2.dev, now);
        l2.bus.device_to_host.clear();
    }
    EXPECT_EQ(APEX_DEVICE_STATE_PROVISIONAL, apex_device_link_state(&l2.dev));
}

/* ------------------------------------------------------------------------- */
/* Multi-device ("hub") discovery: serial enumeration yields distinct ids.    */
/* ------------------------------------------------------------------------- */

struct HubBus {
    std::vector<uint8_t> downlink;  // host -> all devices
    std::vector<uint8_t> uplink;    // all devices -> host (merged)
};
void hub_host_tx(void* u, const uint8_t* b, size_t n) {
    auto* bus = static_cast<HubBus*>(u);
    bus->downlink.insert(bus->downlink.end(), b, b + n);
}
void hub_dev_tx(void* u, const uint8_t* b, size_t n) {
    auto* bus = static_cast<HubBus*>(u);
    bus->uplink.insert(bus->uplink.end(), b, b + n);
}

TEST(DiscoveryHubV1, TwoDevicesGetDistinctIds) {
    HubBus bus{};
    apex_host_t host{};
    apex_host_cfg_t hc{};
    hc.supported_interfaces = APEX_INTERFACE_FLAG_GPIO;
    hc.host_state_period_ms = 1000;
    hc.tx = hub_host_tx; hc.tx_user = &bus;
    apex_host_init(&host, &hc);
    apex_host_register_class(&host, APEX_TRAFFIC_ACTIVATION, nullptr, nullptr);

    apex_device_t devA{}, devB{};
    apex_device_cfg_t da{}, db{};
    da.device_class = db.device_class = APEX_TRAFFIC_ACTIVATION;
    da.interface_flags = db.interface_flags = APEX_INTERFACE_FLAG_GPIO;
    da.class_version_min = db.class_version_min = 1;
    da.class_version_max = db.class_version_max = 1;
    da.mass_grams = 100; db.mass_grams = 200;
    da.tx = db.tx = hub_dev_tx; da.tx_user = db.tx_user = &bus;
    apex_device_init(&devA, &da);
    apex_device_init(&devB, &db);

    uint32_t now = 0;
    auto pump = [&](bool tickB) {
        now += 1;
        apex_device_tick(&devA, now);
        if (tickB) apex_device_tick(&devB, now);
        apex_host_tick(&host, now);
        if (!bus.uplink.empty()) {
            auto b = std::move(bus.uplink); bus.uplink.clear();
            apex_host_feed_rx(&host, b.data(), b.size(), now);
        }
        if (!bus.downlink.empty()) {
            auto b = std::move(bus.downlink); bus.downlink.clear();
            apex_device_feed_rx(&devA, b.data(), b.size(), now);
            if (tickB) apex_device_feed_rx(&devB, b.data(), b.size(), now);
        }
    };

    for (int i = 0; i < 16; i++) pump(false);
    ASSERT_EQ(APEX_DEVICE_STATE_CONNECTED, apex_device_link_state(&devA));
    uint8_t idA = apex_device_get_id(&devA);

    // The host beacons per-port only while unlinked, so B — joining after
    // A's session started — hears its beacon from its own passthrough leg (the
    // flat test bus can't model that port). Seed B with that leg's beacon; B
    // opens immediately and its beacon + DEVICE_INFO ride the next uplink.
    {
        uint8_t bcn[APEX_MAX_ENCODED_FRAME_LENGTH];
        size_t bl = 0;
        ASSERT_EQ(APEX_OK, apex_beacon_build(1, 1, bcn, sizeof(bcn), &bl));
        apex_device_feed_rx(&devB, bcn, bl, now);
    }

    for (int i = 0; i < 16; i++) pump(true);
    ASSERT_EQ(APEX_DEVICE_STATE_CONNECTED, apex_device_link_state(&devB));
    uint8_t idB = apex_device_get_id(&devB);

    EXPECT_GE(idA, kFirstId);
    EXPECT_GE(idB, kFirstId);
    EXPECT_NE(idA, idB);
    EXPECT_EQ(APEX_DEVICE_STATE_CONNECTED, apex_device_link_state(&devA));
    EXPECT_EQ(2u, apex_host_device_count(&host));
}

TEST(DiscoveryV1, TableSupportsSixteenSlots) {
    EXPECT_EQ(16, APEX_HOST_MAX_DEVICES);
}

/* ------------------------------------------------------------------------- */
/* RESET_REQUEST re-enumeration (msg 13) — brown-out recovery.                 */
/* Wire rules & design: see APEX_Core.md §3.2.13.                            */
/* ------------------------------------------------------------------------- */

/* Count RESET_REQUEST frames addressed to `id` in an encoded byte stream. */
int CountResetsTo(const std::vector<uint8_t>& enc, uint8_t id) {
    if (enc.empty()) return 0;
    struct Cap { uint8_t id; int n; } cap{id, 0};
    apex_framer_rx_t rx;
    apex_framer_rx_init(&rx);
    apex_framer_feed(
        &rx, enc.data(), enc.size(),
        [](void* u, const apex_hdr_t* hdr, const uint8_t* p, size_t n) {
            auto* c = static_cast<Cap*>(u);
            if (hdr->traffic_type == APEX_TRAFFIC_CONFIG && n >= 1 &&
                p[0] == APEX_CFG_MSG_RESET_REQUEST && hdr->device_id == c->id) {
                c->n++;
            }
        },
        &cap);
    return cap.n;
}

/* Devkit regression, boot-sweep variant: device fully CONNECTED to host A;
 * host is reflashed and reboots as host B on the same link; B's boot sweep
 * broadcasts RESET_REQUEST; the stale device re-enumerates and reconnects. */
TEST(ReenumV1, DevkitHostSwapBootSweepRecovers) {
    Link l;
    apex_host_cfg_t hc = BaseHostCfg(l);
    apex_device_cfg_t dc = BaseDeviceCfg(l);
    l.Init(hc, dc);
    ASSERT_EQ(APEX_OK, apex_host_register_class(&l.host, APEX_TRAFFIC_ACTIVATION,
                                                host_class_rx, &l.host_rx));
    l.Pump(1, 6);
    ASSERT_EQ(APEX_DEVICE_STATE_CONNECTED, apex_device_link_state(&l.dev));

    // "Reflash": re-init the host in place — fresh state, same link. In-flight
    // bytes are lost with the reboot.
    l.bus.host_to_device.clear();
    l.bus.device_to_host.clear();
    apex_host_cfg_t hcB = BaseHostCfg(l);
    apex_host_init(&l.host, &hcB);
    ASSERT_EQ(APEX_OK, apex_host_register_class(&l.host, APEX_TRAFFIC_ACTIVATION,
                                                host_class_rx, &l.host_rx));

    // B's boot sweep fires on its first ticks; the device honors the broadcast
    // reset, re-discovers, and reconnects to B.
    l.Pump(50, 20);  // ~1 s
    EXPECT_EQ(APEX_DEVICE_STATE_CONNECTED, apex_device_link_state(&l.dev));
    uint8_t id = apex_device_get_id(&l.dev);
    EXPECT_GE(id, kFirstId);
    const apex_host_device_slot_t* slot = apex_host_get_device(&l.host, id);
    ASSERT_NE(nullptr, slot);
    EXPECT_EQ(APEX_DEV_STATUS_CONNECTED, slot->status);
    EXPECT_EQ(1u, apex_host_device_count(&l.host));
}

/* Devkit regression, reactive-only variant: same swap, but B's boot sweep is
 * suppressed. The stale device's >= 1 Hz traffic at an id B never assigned
 * triggers B's addressed reactive RESET_REQUEST; the device recovers. */
TEST(ReenumV1, DevkitHostSwapReactiveOnlyRecovers) {
    Link l;
    apex_host_cfg_t hc = BaseHostCfg(l);
    apex_device_cfg_t dc = BaseDeviceCfg(l);
    l.Init(hc, dc);
    ASSERT_EQ(APEX_OK, apex_host_register_class(&l.host, APEX_TRAFFIC_ACTIVATION,
                                                host_class_rx, &l.host_rx));
    l.Pump(1, 6);
    ASSERT_EQ(APEX_DEVICE_STATE_CONNECTED, apex_device_link_state(&l.dev));
    uint8_t stale_id = apex_device_get_id(&l.dev);

    l.bus.host_to_device.clear();
    l.bus.device_to_host.clear();
    apex_host_cfg_t hcB = BaseHostCfg(l);
    hcB.suppress_boot_sweep = true;   // reactive path only
    apex_host_init(&l.host, &hcB);
    ASSERT_EQ(APEX_OK, apex_host_register_class(&l.host, APEX_TRAFFIC_ACTIVATION,
                                                host_class_rx, &l.host_rx));

    // The device still believes it is CONNECTED at stale_id and heartbeats at
    // >= 1 Hz; B reactive-resets it and it reconnects within a few seconds.
    l.Pump(200, 25);  // ~5 s
    EXPECT_EQ(APEX_DEVICE_STATE_CONNECTED, apex_device_link_state(&l.dev));
    uint8_t id = apex_device_get_id(&l.dev);
    EXPECT_GE(id, kFirstId);
    const apex_host_device_slot_t* slot = apex_host_get_device(&l.host, id);
    ASSERT_NE(nullptr, slot);
    EXPECT_EQ(APEX_DEV_STATUS_CONNECTED, slot->status);
    EXPECT_EQ(1u, apex_host_device_count(&l.host));
    (void)stale_id;
}

/* The reactive RESET_REQUEST is rate-limited to <= 1 Hz per unknown id. */
TEST(ReenumV1, ReactiveResetIsRateLimited) {
    Link l;
    apex_host_cfg_t hc = BaseHostCfg(l);
    hc.suppress_boot_sweep = true;
    apex_device_cfg_t dc = BaseDeviceCfg(l);
    l.Init(hc, dc);  // device unused; we forge the stale talker
    ASSERT_EQ(APEX_OK, apex_host_register_class(&l.host, APEX_TRAFFIC_ACTIVATION,
                                                host_class_rx, &l.host_rx));

    // A stale talker heartbeats at 10 Hz from an id the host never assigned.
    auto stale = ForgeHostFrame(0x42, {});  // empty CONFIG = heartbeat
    int resets = 0;
    for (int i = 0; i < 10; i++) {  // t = 0 .. 900 ms
        apex_host_feed_rx(&l.host, stale.data(), stale.size(), (uint32_t)i * 100);
        resets += CountResetsTo(l.bus.host_to_device, 0x42);
        l.bus.host_to_device.clear();
    }
    EXPECT_EQ(1, resets);  // one reset in the first second

    // After >= 1 s another is allowed.
    apex_host_feed_rx(&l.host, stale.data(), stale.size(), 1100);
    EXPECT_EQ(1, CountResetsTo(l.bus.host_to_device, 0x42));
}

/* Broadcast reset with two connected devices: both re-enumerate and reconnect
 * with valid ids; the host table stays consistent. Recovery is staggered (one
 * device re-discovers at a time) per the one-unassigned-device rule (§3.7.4). */
TEST(ReenumV1, BroadcastResetTwoDevicesReenumerate) {
    HubBus bus{};
    apex_host_t host{};
    apex_host_cfg_t hc{};
    hc.supported_interfaces = APEX_INTERFACE_FLAG_GPIO;
    hc.host_state_period_ms = 1000;
    hc.tx = hub_host_tx; hc.tx_user = &bus;
    apex_host_init(&host, &hc);
    apex_host_register_class(&host, APEX_TRAFFIC_ACTIVATION, nullptr, nullptr);

    apex_device_t devA{}, devB{};
    apex_device_cfg_t da{}, db{};
    da.device_class = db.device_class = APEX_TRAFFIC_ACTIVATION;
    da.interface_flags = db.interface_flags = APEX_INTERFACE_FLAG_GPIO;
    da.mass_grams = 100; db.mass_grams = 200;
    da.tx = db.tx = hub_dev_tx; da.tx_user = db.tx_user = &bus;
    apex_device_init(&devA, &da);
    apex_device_init(&devB, &db);

    uint32_t now = 0;
    auto pump = [&](bool withA, bool withB) {
        now += 1;
        if (withA) apex_device_tick(&devA, now);
        if (withB) apex_device_tick(&devB, now);
        apex_host_tick(&host, now);
        if (!bus.uplink.empty()) {
            auto b = std::move(bus.uplink); bus.uplink.clear();
            apex_host_feed_rx(&host, b.data(), b.size(), now);
        }
        if (!bus.downlink.empty()) {
            auto b = std::move(bus.downlink); bus.downlink.clear();
            if (withA) apex_device_feed_rx(&devA, b.data(), b.size(), now);
            if (withB) apex_device_feed_rx(&devB, b.data(), b.size(), now);
        }
    };

    // Serial enumeration: A alone, then B (seeded with its port's beacon —
    // see TwoDevicesGetDistinctIds for why the flat test bus needs this).
    for (int i = 0; i < 16; i++) pump(true, false);
    ASSERT_EQ(APEX_DEVICE_STATE_CONNECTED, apex_device_link_state(&devA));
    {
        uint8_t bcn[APEX_MAX_ENCODED_FRAME_LENGTH];
        size_t bl = 0;
        ASSERT_EQ(APEX_OK, apex_beacon_build(1, 1, bcn, sizeof(bcn), &bl));
        apex_device_feed_rx(&devB, bcn, bl, now);
    }
    for (int i = 0; i < 16; i++) pump(true, true);
    ASSERT_EQ(APEX_DEVICE_STATE_CONNECTED, apex_device_link_state(&devB));
    ASSERT_EQ(2u, apex_host_device_count(&host));

    // Bus-wide re-enumeration: broadcast RESET_REQUEST reaches both devices.
    ASSERT_EQ(APEX_OK,
              apex_host_request_reenumeration(&host, APEX_DEVICE_ID_BROADCAST));
    {
        auto b = std::move(bus.downlink); bus.downlink.clear();
        apex_device_feed_rx(&devA, b.data(), b.size(), now);
        apex_device_feed_rx(&devB, b.data(), b.size(), now);
    }
    EXPECT_EQ(APEX_DEVICE_STATE_DISCOVERING, apex_device_link_state(&devA));
    EXPECT_EQ(APEX_DEVICE_STATE_DISCOVERING, apex_device_link_state(&devB));
    EXPECT_EQ(0u, apex_host_device_count(&host));

    // Staggered recovery: A first, then B.
    for (int i = 0; i < 16; i++) pump(true, false);
    ASSERT_EQ(APEX_DEVICE_STATE_CONNECTED, apex_device_link_state(&devA));
    for (int i = 0; i < 16; i++) pump(true, true);
    ASSERT_EQ(APEX_DEVICE_STATE_CONNECTED, apex_device_link_state(&devB));

    uint8_t idA = apex_device_get_id(&devA);
    uint8_t idB = apex_device_get_id(&devB);
    EXPECT_GE(idA, kFirstId);
    EXPECT_GE(idB, kFirstId);
    EXPECT_NE(idA, idB);
    EXPECT_EQ(2u, apex_host_device_count(&host));
    EXPECT_EQ(APEX_DEV_STATUS_CONNECTED, apex_host_get_device(&host, idA)->status);
    EXPECT_EQ(APEX_DEV_STATUS_CONNECTED, apex_host_get_device(&host, idB)->status);
}

/* Device-initiated re-enumeration: announce -> host frees the slot -> device
 * re-discovers; the old id returns to the pool. */
TEST(ReenumV1, DeviceInitiatedReenumeration) {
    Link l;
    apex_host_cfg_t hc = BaseHostCfg(l);
    apex_device_cfg_t dc = BaseDeviceCfg(l);
    l.Init(hc, dc);
    ASSERT_EQ(APEX_OK, apex_host_register_class(&l.host, APEX_TRAFFIC_ACTIVATION,
                                                host_class_rx, &l.host_rx));

    // Not valid while still discovering.
    EXPECT_EQ(APEX_ERR_BAD_STATE, apex_device_request_reenumeration(&l.dev));

    l.Pump(1, 6);
    ASSERT_EQ(APEX_DEVICE_STATE_CONNECTED, apex_device_link_state(&l.dev));
    uint8_t old_id = apex_device_get_id(&l.dev);

    ASSERT_EQ(APEX_OK, apex_device_request_reenumeration(&l.dev));
    EXPECT_EQ(APEX_DEVICE_STATE_DISCOVERING, apex_device_link_state(&l.dev));

    l.Pump(1, 2);  // announcement reaches the host -> slot freed
    EXPECT_EQ(nullptr, apex_host_get_device(&l.host, old_id));

    l.Pump(1, 6);  // re-discovery completes
    EXPECT_EQ(APEX_DEVICE_STATE_CONNECTED, apex_device_link_state(&l.dev));
    uint8_t new_id = apex_device_get_id(&l.dev);
    EXPECT_GE(new_id, kFirstId);
    EXPECT_EQ(1u, apex_host_device_count(&l.host));
    EXPECT_EQ(APEX_DEV_STATUS_CONNECTED,
              apex_host_get_device(&l.host, new_id)->status);
}

/* Safety deferral: while reenum_permitted returns false a RESET_REQUEST is
 * held pending — the device stays CONNECTED and keeps heartbeating. Flipping
 * the hook to true honors the reset on the next tick. */
bool g_reenum_permit = true;
bool reenum_permit_hook(void* user) {
    (void)user;
    return g_reenum_permit;
}
std::vector<apex_device_link_state_t> g_link_events;
void capture_link_event(void* user, apex_device_link_state_t s) {
    (void)user;
    g_link_events.push_back(s);
}

TEST(ReenumV1, DeferralHoldsResetUntilPermitted) {
    g_reenum_permit = true;
    g_link_events.clear();
    Link l;
    apex_host_cfg_t hc = BaseHostCfg(l);
    apex_device_cfg_t dc = BaseDeviceCfg(l);
    dc.reenum_permitted = reenum_permit_hook;
    dc.on_link_event = capture_link_event;
    l.Init(hc, dc);
    ASSERT_EQ(APEX_OK, apex_host_register_class(&l.host, APEX_TRAFFIC_ACTIVATION,
                                                host_class_rx, &l.host_rx));
    l.Pump(1, 6);
    ASSERT_EQ(APEX_DEVICE_STATE_CONNECTED, apex_device_link_state(&l.dev));
    uint8_t id = apex_device_get_id(&l.dev);

    // Forbid re-enumeration, then reset the device from the host.
    g_reenum_permit = false;
    g_link_events.clear();
    ASSERT_EQ(APEX_OK, apex_host_request_reenumeration(&l.host, id));

    // ~3 s: the device defers — it stays CONNECTED and keeps heartbeating
    // (further resets, reactive or broadcast, are deferred the same way).
    l.Pump(500, 6);
    EXPECT_EQ(APEX_DEVICE_STATE_CONNECTED, apex_device_link_state(&l.dev));
    for (apex_device_link_state_t s : g_link_events) {
        EXPECT_NE(APEX_DEVICE_STATE_DISCOVERING, s);
    }

    // Permit: the pending reset is honored on the next tick and the device
    // re-enumerates back to CONNECTED.
    g_reenum_permit = true;
    l.Pump(500, 6);
    EXPECT_EQ(APEX_DEVICE_STATE_CONNECTED, apex_device_link_state(&l.dev));
    bool discovered = false;
    for (apex_device_link_state_t s : g_link_events) {
        if (s == APEX_DEVICE_STATE_DISCOVERING) discovered = true;
    }
    EXPECT_TRUE(discovered);
    EXPECT_EQ(1u, apex_host_device_count(&l.host));
}

/* ------------------------------------------------------------------------- */
/* Hotplug: pre-closure input gate + device swap.                          */
/* ------------------------------------------------------------------------- */

/* Scan an encoded byte stream: count beacons and session frames. */
struct WireStats {
    int beacons = 0;
    int session_frames = 0;
};
WireStats ScanWire(const std::vector<uint8_t>& enc) {
    WireStats st;
    if (enc.empty()) return st;
    apex_framer_rx_t rx;
    apex_framer_rx_init(&rx);
    apex_framer_set_beacon_cb(
        &rx,
        [](void* u, uint16_t, uint16_t) { static_cast<WireStats*>(u)->beacons++; },
        &st);
    apex_framer_feed(
        &rx, enc.data(), enc.size(),
        [](void* u, const apex_hdr_t*, const uint8_t*, size_t) {
            static_cast<WireStats*>(u)->session_frames++;
        },
        &st);
    return st;
}

int g_hs_seen = 0;
void count_host_state(void* u, apex_flight_state_t s) {
    (void)u; (void)s;
    g_hs_seen++;
}

/* Devkit hot-swap, device side: device A fully CONNECTED is replaced by a
 * freshly powered device B while the host still holds A's session. B emits
 * nothing but beacons and consumes nothing (pre-closure gate) — not even
 * HOST_STATE broadcasts or A-addressed traffic; the host ignores B's beacons
 * (the watchdog is the sole arbiter of A's death). When A's slot faults, the
 * port unlinks, the host resumes beaconing, and B closes the tier and
 * connects — within the watchdog window + about one beacon period. */
TEST(HotplugV1, DeviceSwapMidSessionRecoversViaWatchdog) {
    g_hs_seen = 0;
    Link l;
    apex_host_cfg_t hc = BaseHostCfg(l);
    apex_device_cfg_t dc = BaseDeviceCfg(l);
    l.Init(hc, dc);
    ASSERT_EQ(APEX_OK, apex_host_register_class(&l.host, APEX_TRAFFIC_ACTIVATION,
                                                host_class_rx, &l.host_rx));
    l.Pump(1, 6);
    ASSERT_EQ(APEX_DEVICE_STATE_CONNECTED, apex_device_link_state(&l.dev));
    uint8_t idA = apex_device_get_id(&l.dev);

    // Hot-swap: B replaces A on the same link; in-flight bytes are lost.
    l.bus.host_to_device.clear();
    l.bus.device_to_host.clear();
    apex_device_cfg_t db = BaseDeviceCfg(l);
    db.mass_grams = 777;  // marks B's session on the host side
    db.on_host_state = count_host_state;
    apex_device_init(&l.dev, &db);
    uint32_t swap_t = l.now_ms;

    // Phase 1 (~3 s, safely inside A's 5 s watchdog): host keeps A's session,
    // HOST_STATE broadcasts and A-addressed class traffic keep flowing.
    for (int i = 0; i < 6; i++) {
        l.now_ms += 500;
        apex_device_tick(&l.dev, l.now_ms);
        (void)apex_host_send(&l.host, idA, APEX_TRAFFIC_ACTIVATION,
                             (const uint8_t*)"x", 1);
        apex_host_tick(&l.host, l.now_ms);

        // B emits beacons ONLY.
        WireStats up = ScanWire(l.bus.device_to_host);
        EXPECT_EQ(0, up.session_frames);
        auto u = std::move(l.bus.device_to_host); l.bus.device_to_host.clear();
        apex_host_feed_rx(&l.host, u.data(), u.size(), l.now_ms);
        auto d = std::move(l.bus.host_to_device); l.bus.host_to_device.clear();
        apex_device_feed_rx(&l.dev, d.data(), d.size(), l.now_ms);

        // B consumed nothing: still gated, host state never delivered.
        EXPECT_EQ(APEX_DEVICE_STATE_DISCOVERING, apex_device_link_state(&l.dev));
        EXPECT_FALSE(l.dev.peer_beacon_seen);
        EXPECT_EQ(0, g_hs_seen);
        // A's slot intact: the host ignored B's beacons.
        const apex_host_device_slot_t* a = apex_host_get_device(&l.host, idA);
        ASSERT_NE(nullptr, a);
        EXPECT_EQ(APEX_DEV_STATUS_CONNECTED, a->status);
    }

    // Phase 2: A's watchdog fires; the port unlinks; the host beacons; B
    // closes the tier and connects.
    int guard = 0;
    while (apex_device_link_state(&l.dev) != APEX_DEVICE_STATE_CONNECTED &&
           guard++ < 60) {
        l.now_ms += 100;
        apex_device_tick(&l.dev, l.now_ms);
        apex_host_tick(&l.host, l.now_ms);
        auto u = std::move(l.bus.device_to_host); l.bus.device_to_host.clear();
        apex_host_feed_rx(&l.host, u.data(), u.size(), l.now_ms);
        auto d = std::move(l.bus.host_to_device); l.bus.host_to_device.clear();
        apex_device_feed_rx(&l.dev, d.data(), d.size(), l.now_ms);
    }
    ASSERT_EQ(APEX_DEVICE_STATE_CONNECTED, apex_device_link_state(&l.dev));

    // Swap latency <= watchdog window + ~1 beacon period.
    EXPECT_LE(l.now_ms - swap_t, 5000u + 1000u + 500u);

    // Let B's in-flight CONFIG_ACK reach the host (slot promotion).
    l.Pump(1, 2);

    // It is B's session (new id — A's faulted slot still awaits recycling —
    // and B's declared mass).
    uint8_t idB = apex_device_get_id(&l.dev);
    EXPECT_NE(idA, idB);
    const apex_host_device_slot_t* b = apex_host_get_device(&l.host, idB);
    ASSERT_NE(nullptr, b);
    EXPECT_EQ(APEX_DEV_STATUS_CONNECTED, b->status);
    EXPECT_EQ(777u, b->mass_grams);
}

/* Idle-port fast path: a port that has been unlinked (and beaconing) for a
 * while accepts a freshly plugged device within about one beacon period. */
TEST(HotplugV1, IdlePortHotplugConnectsWithinBeaconPeriod) {
    Link l;
    apex_host_cfg_t hc = BaseHostCfg(l);
    apex_device_cfg_t dc = BaseDeviceCfg(l);
    l.Init(hc, dc);
    ASSERT_EQ(APEX_OK, apex_host_register_class(&l.host, APEX_TRAFFIC_ACTIVATION,
                                                host_class_rx, &l.host_rx));

    // Idle port: the host beacons into the void for ~3 s (nothing attached).
    for (int i = 0; i < 6; i++) {
        l.now_ms += 500;
        apex_host_tick(&l.host, l.now_ms);
        WireStats down = ScanWire(l.bus.host_to_device);
        EXPECT_GE(down.beacons + down.session_frames, 0);  // stream well-formed
        l.bus.host_to_device.clear();
    }

    // Fresh device plugs in ("powers up" at plug time).
    apex_device_init(&l.dev, &dc);
    uint32_t plug_t = l.now_ms;
    int guard = 0;
    while (apex_device_link_state(&l.dev) != APEX_DEVICE_STATE_CONNECTED &&
           guard++ < 30) {
        l.now_ms += 100;
        apex_device_tick(&l.dev, l.now_ms);
        apex_host_tick(&l.host, l.now_ms);
        auto u = std::move(l.bus.device_to_host); l.bus.device_to_host.clear();
        apex_host_feed_rx(&l.host, u.data(), u.size(), l.now_ms);
        auto d = std::move(l.bus.host_to_device); l.bus.host_to_device.clear();
        apex_device_feed_rx(&l.dev, d.data(), d.size(), l.now_ms);
    }
    ASSERT_EQ(APEX_DEVICE_STATE_CONNECTED, apex_device_link_state(&l.dev));
    EXPECT_LE(l.now_ms - plug_t, 1000u + 500u);  // ~1 beacon period
    EXPECT_EQ(1u, apex_host_device_count(&l.host));
}

/* Eviction composition — the ONE post-latch eviction mechanism: evicting a
 * CONNECTED device = RESET_REQUEST -> its re-discovery -> ACK_REJECT_POLICY ->
 * device REJECTED (stops retrying). No bespoke teardown message exists. */
TEST(ReenumV1, EvictCompositionEndsInRejectPolicy) {
    Link l;
    apex_host_cfg_t hc = BaseHostCfg(l);
    apex_device_cfg_t dc = BaseDeviceCfg(l);
    l.Init(hc, dc);
    ASSERT_EQ(APEX_OK, apex_host_register_class(&l.host, APEX_TRAFFIC_ACTIVATION,
                                                host_class_rx, &l.host_rx));
    l.Pump(1, 6);
    ASSERT_EQ(APEX_DEVICE_STATE_CONNECTED, apex_device_link_state(&l.dev));
    uint8_t id = apex_device_get_id(&l.dev);

    // Unknown ids are NOT_FOUND; eviction needs a held slot (for its class).
    EXPECT_EQ(APEX_ERR_NOT_FOUND, apex_host_evict(&l.host, 0x77));

    ASSERT_EQ(APEX_OK, apex_host_evict(&l.host, id));
    l.Pump(1, 8);

    EXPECT_EQ(APEX_DEVICE_STATE_REJECTED, apex_device_link_state(&l.dev));
    EXPECT_EQ(APEX_ACK_REJECT_POLICY, apex_device_reject_reason(&l.dev));
    EXPECT_EQ(0u, apex_host_device_count(&l.host));
}

}  // namespace
