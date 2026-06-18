/* End-to-end discovery test. We stand up a host and a device in the same
 * process and loopback-connect their TX callbacks to each other's feed_rx.
 * Then we advance both through their tick functions and verify the device
 * reaches CONNECTED with a host-assigned device_id. */
#include "apex/apex_device.h"
#include "apex/apex_host.h"

#include <gtest/gtest.h>

#include <vector>

namespace {

struct Bus {
    std::vector<uint8_t> host_to_device;
    std::vector<uint8_t> device_to_host;
};

static void host_tx(void* user, const uint8_t* bytes, size_t n) {
    auto* bus = static_cast<Bus*>(user);
    bus->host_to_device.insert(bus->host_to_device.end(), bytes, bytes + n);
}

static void device_tx(void* user, const uint8_t* bytes, size_t n) {
    auto* bus = static_cast<Bus*>(user);
    bus->device_to_host.insert(bus->device_to_host.end(), bytes, bytes + n);
}

struct ClassRxCapture {
    uint8_t device_id = 0;
    std::vector<uint8_t> last_payload;
    int call_count = 0;
};

static void host_class_rx(void* user, uint8_t device_id,
                          const uint8_t* payload, size_t payload_len) {
    auto* c = static_cast<ClassRxCapture*>(user);
    c->device_id = device_id;
    c->last_payload.assign(payload, payload + payload_len);
    c->call_count++;
}

struct DeviceClassRxCapture {
    std::vector<uint8_t> last_payload;
    int call_count = 0;
};

static void device_class_rx(void* user, const uint8_t* payload, size_t payload_len) {
    auto* c = static_cast<DeviceClassRxCapture*>(user);
    c->last_payload.assign(payload, payload + payload_len);
    c->call_count++;
}

struct DeviceEventCapture {
    uint8_t device_id = 0;
    apex_device_status_t last_status = APEX_DEV_STATUS_UNKNOWN;
    int call_count = 0;
};

static void on_dev_event(void* user, uint8_t id, apex_device_status_t status) {
    auto* c = static_cast<DeviceEventCapture*>(user);
    c->device_id = id;
    c->last_status = status;
    c->call_count++;
}

class DiscoveryTest : public ::testing::Test {
protected:
    Bus bus{};
    apex_host_t host{};
    apex_device_t dev{};
    ClassRxCapture host_rx{};
    DeviceClassRxCapture dev_rx{};
    DeviceEventCapture dev_events{};
    uint32_t now_ms = 0;

    void SetUp() override {
        apex_host_cfg_t hc{};
        hc.supported_interfaces = APEX_INTERFACE_FLAG_GPIO | APEX_INTERFACE_FLAG_USB;
        hc.host_state_period_ms = 1000;
        hc.tx = host_tx;
        hc.tx_user = &bus;
        hc.on_device_event = on_dev_event;
        hc.on_device_event_user = &dev_events;
        apex_host_init(&host, &hc);
        ASSERT_EQ(APEX_OK,
                  apex_host_register_class(&host, APEX_TRAFFIC_ACTIVATION,
                                           host_class_rx, &host_rx));

        apex_device_cfg_t dc{};
        dc.device_class = APEX_TRAFFIC_ACTIVATION;
        dc.interface_flags = APEX_INTERFACE_FLAG_GPIO;
        dc.tx = device_tx;
        dc.tx_user = &bus;
        dc.on_class_rx = device_class_rx;
        dc.on_class_rx_user = &dev_rx;
        apex_device_init(&dev, &dc);
    }

    void Pump(uint32_t advance_ms = 0, int rounds = 1) {
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
};

TEST_F(DiscoveryTest, DeviceReachesConnectedWithAssignedId) {
    Pump(0);  // first tick — device sends DEVICE_INFO immediately
    Pump(1);  // host processes it, replies CONFIG_REPLY
    Pump(1);  // device processes CONFIG_REPLY

    EXPECT_EQ(APEX_DEVICE_STATE_CONNECTED, apex_device_link_state(&dev));
    EXPECT_NE(APEX_DEVICE_ID_UNASSIGNED, apex_device_get_id(&dev));
    EXPECT_EQ(0x01, apex_device_get_id(&dev));

    const apex_host_device_slot_t* slot =
        apex_host_get_device(&host, apex_device_get_id(&dev));
    ASSERT_NE(nullptr, slot);
    EXPECT_EQ(APEX_DEV_STATUS_CONNECTED, slot->status);
    EXPECT_EQ(APEX_TRAFFIC_ACTIVATION, slot->device_class);
    EXPECT_EQ(APEX_INTERFACE_FLAG_GPIO, slot->interface_flags);

    EXPECT_EQ(1, dev_events.call_count);
    EXPECT_EQ(APEX_DEV_STATUS_CONNECTED, dev_events.last_status);
}

TEST_F(DiscoveryTest, HostStateBroadcastReachesDeviceAfterConnect) {
    // First, complete discovery.
    Pump(0); Pump(1); Pump(1);
    ASSERT_EQ(APEX_DEVICE_STATE_CONNECTED, apex_device_link_state(&dev));

    // Now advance time past the HOST_STATE period and check that the device
    // receives the broadcast.
    bool host_state_seen = false;
    apex_flight_state_t seen_state = APEX_FLIGHT_STATE_UNKNOWN;

    struct HsCap { bool* seen; apex_flight_state_t* state; };
    HsCap cap{&host_state_seen, &seen_state};
    // Reinstall the device's on_host_state to capture (rebuild dev).
    apex_device_cfg_t dc{};
    dc.device_class = APEX_TRAFFIC_ACTIVATION;
    dc.interface_flags = APEX_INTERFACE_FLAG_GPIO;
    dc.tx = device_tx;
    dc.tx_user = &bus;
    dc.on_class_rx = device_class_rx;
    dc.on_class_rx_user = &dev_rx;
    dc.on_host_state = [](void* user, apex_flight_state_t s) {
        auto* c = static_cast<HsCap*>(user);
        *c->seen = true;
        *c->state = s;
    };
    dc.on_host_state_user = &cap;
    apex_device_init(&dev, &dc);
    bus.host_to_device.clear();
    bus.device_to_host.clear();
    dev_events = {};
    now_ms = 0;

    Pump(0); Pump(1); Pump(1);
    ASSERT_EQ(APEX_DEVICE_STATE_CONNECTED, apex_device_link_state(&dev));

    apex_host_set_flight_state(&host, APEX_FLIGHT_STATE_PROPS_ON_FLYING);
    // Advance enough for the next HOST_STATE broadcast.
    Pump(1100, 1);
    Pump(1, 2);

    EXPECT_TRUE(host_state_seen);
    EXPECT_EQ(APEX_FLIGHT_STATE_PROPS_ON_FLYING, seen_state);
}

TEST_F(DiscoveryTest, DeviceWatchdogResetsOnHostSilence) {
    Pump(0); Pump(1); Pump(1);
    ASSERT_EQ(APEX_DEVICE_STATE_CONNECTED, apex_device_link_state(&dev));

    // Drop host->device traffic by clearing the bus before the device sees it.
    for (int i = 0; i < 6; i++) {
        now_ms += 1000;
        apex_host_tick(&host, now_ms);
        bus.host_to_device.clear();  // simulate host gone silent
        apex_device_tick(&dev, now_ms);
    }
    EXPECT_EQ(APEX_DEVICE_STATE_DISCOVERING, apex_device_link_state(&dev));
}

TEST_F(DiscoveryTest, HostWatchdogMarksDeviceFault) {
    Pump(0); Pump(1); Pump(1);
    uint8_t did = apex_device_get_id(&dev);
    ASSERT_NE(APEX_DEVICE_ID_UNASSIGNED, did);

    // Advance the host without any device→host traffic.
    for (int i = 0; i < 6; i++) {
        now_ms += 1000;
        bus.device_to_host.clear();
        apex_host_tick(&host, now_ms);
    }
    const apex_host_device_slot_t* slot = apex_host_get_device(&host, did);
    ASSERT_NE(nullptr, slot);
    EXPECT_EQ(APEX_DEV_STATUS_FAULT, slot->status);
}

TEST_F(DiscoveryTest, RejectClassWhenHostHasNoHandler) {
    // Reinit without registering APEX_TRAFFIC_ACTIVATION.
    apex_host_cfg_t hc{};
    hc.supported_interfaces = APEX_INTERFACE_FLAG_GPIO;
    hc.tx = host_tx;
    hc.tx_user = &bus;
    apex_host_init(&host, &hc);

    bus.host_to_device.clear();
    bus.device_to_host.clear();
    apex_device_cfg_t dc{};
    dc.device_class = APEX_TRAFFIC_ACTIVATION;
    dc.interface_flags = APEX_INTERFACE_FLAG_GPIO;
    dc.tx = device_tx;
    dc.tx_user = &bus;
    dc.on_class_rx = device_class_rx;
    dc.on_class_rx_user = &dev_rx;
    apex_device_init(&dev, &dc);
    now_ms = 0;

    Pump(0); Pump(1); Pump(1);
    EXPECT_EQ(APEX_DEVICE_STATE_REJECTED_CLASS, apex_device_link_state(&dev));
}

// A device whose CONFIG_REPLY is lost keeps sending DEVICE_INFO(id=0). The host
// must dedup these onto a single provisional slot rather than allocating a fresh
// one each time (which used to exhaust the table and cascade to FAULT).
TEST_F(DiscoveryTest, RepeatedDeviceInfoDedupsToOneSlot) {
    // Boot: device sends DEVICE_INFO(0); host assigns a provisional slot.
    now_ms = 0;
    apex_device_tick(&dev, now_ms);
    apex_host_tick(&host, now_ms);
    {
        auto b = std::move(bus.device_to_host); bus.device_to_host.clear();
        apex_host_feed_rx(&host, b.data(), b.size(), now_ms);
    }
    bus.host_to_device.clear();  // DROP the CONFIG_REPLY — device never latches

    ASSERT_EQ(1u, apex_host_device_count(&host));
    const apex_host_device_slot_t* slot = apex_host_get_device(&host, 0x01);
    ASSERT_NE(nullptr, slot);
    EXPECT_EQ(APEX_DEV_STATUS_NEW, slot->status);  // provisional, not CONNECTED

    // Device keeps retrying DEVICE_INFO(0) for ~2.5 s; replies keep being dropped.
    for (int i = 0; i < 10; i++) {
        now_ms += 250;  // > 200 ms discovery retry
        apex_device_tick(&dev, now_ms);
        apex_host_tick(&host, now_ms);
        auto b = std::move(bus.device_to_host); bus.device_to_host.clear();
        apex_host_feed_rx(&host, b.data(), b.size(), now_ms);
        bus.host_to_device.clear();  // still dropping replies
        // Never burns more than the single deduped slot.
        EXPECT_EQ(1u, apex_host_device_count(&host));
    }
    EXPECT_EQ(APEX_DEVICE_STATE_DISCOVERING, apex_device_link_state(&dev));

    // Now let a reply through: device latches, CONFIG_ACK promotes the slot.
    now_ms += 250;
    apex_device_tick(&dev, now_ms);
    apex_host_tick(&host, now_ms);
    Pump(1, 3);  // shuttle reply -> ack -> promotion
    EXPECT_EQ(APEX_DEVICE_STATE_CONNECTED, apex_device_link_state(&dev));
    EXPECT_EQ(1u, apex_host_device_count(&host));
    slot = apex_host_get_device(&host, apex_device_get_id(&dev));
    ASSERT_NE(nullptr, slot);
    EXPECT_EQ(APEX_DEV_STATUS_CONNECTED, slot->status);
}

// A provisional slot whose device vanishes mid-handshake (got assigned, never
// confirmed, then went silent) is recycled so the slot returns to the pool.
TEST_F(DiscoveryTest, ProvisionalSlotRecycledWhenDeviceVanishes) {
    now_ms = 0;
    apex_device_tick(&dev, now_ms);
    apex_host_tick(&host, now_ms);
    {
        auto b = std::move(bus.device_to_host); bus.device_to_host.clear();
        apex_host_feed_rx(&host, b.data(), b.size(), now_ms);
    }
    bus.host_to_device.clear();  // drop reply; device assigned but not latched
    ASSERT_EQ(1u, apex_host_device_count(&host));
    ASSERT_EQ(APEX_DEV_STATUS_NEW, apex_host_get_device(&host, 0x01)->status);

    // Device disappears entirely: no more device->host frames. After the 5 s
    // watchdog the provisional slot is freed.
    for (int i = 0; i < 6; i++) {
        now_ms += 1000;
        bus.device_to_host.clear();
        apex_host_tick(&host, now_ms);
    }
    EXPECT_EQ(0u, apex_host_device_count(&host));
    EXPECT_EQ(nullptr, apex_host_get_device(&host, 0x01));
}

// A CONNECTED device that goes silent faults, and the FAULT slot is recycled a
// few seconds later so the slot + device_id return to the pool.
TEST_F(DiscoveryTest, FaultSlotRecycledAfterDelay) {
    Pump(0); Pump(1); Pump(1);
    uint8_t did = apex_device_get_id(&dev);
    ASSERT_NE(APEX_DEVICE_ID_UNASSIGNED, did);
    ASSERT_EQ(APEX_DEV_STATUS_CONNECTED, apex_host_get_device(&host, did)->status);
    ASSERT_EQ(1u, apex_host_device_count(&host));

    // Silence the device. ~6 s -> FAULT.
    for (int i = 0; i < 6; i++) {
        now_ms += 1000;
        bus.device_to_host.clear();
        apex_host_tick(&host, now_ms);
    }
    const apex_host_device_slot_t* slot = apex_host_get_device(&host, did);
    ASSERT_NE(nullptr, slot);
    EXPECT_EQ(APEX_DEV_STATUS_FAULT, slot->status);
    EXPECT_EQ(1u, apex_host_device_count(&host));  // still occupies its slot

    // A few more seconds past FAULT_RECYCLE_MS -> slot freed, ID reusable.
    for (int i = 0; i < 6; i++) {
        now_ms += 1000;
        bus.device_to_host.clear();
        apex_host_tick(&host, now_ms);
    }
    EXPECT_EQ(0u, apex_host_device_count(&host));
    EXPECT_EQ(nullptr, apex_host_get_device(&host, did));
}

// The host stays provisional until the device confirms its ID. The explicit
// CONFIG_ACK promotes it; if that ACK is lost, the device's first heartbeat
// (any frame bearing the assigned ID) promotes it as a fallback.
TEST_F(DiscoveryTest, ConfigAckLostHeartbeatFallbackPromotes) {
    now_ms = 0;
    apex_device_tick(&dev, now_ms);                 // DEVICE_INFO(0)
    apex_host_tick(&host, now_ms);
    {
        auto b = std::move(bus.device_to_host); bus.device_to_host.clear();
        apex_host_feed_rx(&host, b.data(), b.size(), now_ms);  // host assigns NEW + reply
    }
    {
        auto b = std::move(bus.host_to_device); bus.host_to_device.clear();
        apex_device_feed_rx(&dev, b.data(), b.size(), now_ms); // device latches + emits CONFIG_ACK
    }
    ASSERT_EQ(APEX_DEVICE_STATE_CONNECTED, apex_device_link_state(&dev));
    uint8_t did = apex_device_get_id(&dev);

    // Drop the CONFIG_ACK. Host slot must remain provisional (NEW).
    bus.device_to_host.clear();
    EXPECT_EQ(APEX_DEV_STATUS_NEW, apex_host_get_device(&host, did)->status);

    // ~1 s later the device sends a heartbeat; the host promotes on it.
    now_ms += 1000;
    apex_device_tick(&dev, now_ms);                 // heartbeat (assigned ID)
    apex_host_tick(&host, now_ms);
    {
        auto b = std::move(bus.device_to_host); bus.device_to_host.clear();
        apex_host_feed_rx(&host, b.data(), b.size(), now_ms);
    }
    EXPECT_EQ(APEX_DEV_STATUS_CONNECTED, apex_host_get_device(&host, did)->status);
}

TEST_F(DiscoveryTest, TableSupportsSixteenSlots) {
    EXPECT_EQ(16, APEX_HOST_MAX_DEVICES);
}

TEST_F(DiscoveryTest, RejectInterfaceWhenHostMissingPins) {
    apex_host_cfg_t hc{};
    hc.supported_interfaces = APEX_INTERFACE_FLAG_GPIO;  // no USB
    hc.tx = host_tx;
    hc.tx_user = &bus;
    apex_host_init(&host, &hc);
    ASSERT_EQ(APEX_OK, apex_host_register_class(&host, APEX_TRAFFIC_ACTIVATION,
                                                host_class_rx, &host_rx));

    bus.host_to_device.clear();
    bus.device_to_host.clear();
    apex_device_cfg_t dc{};
    dc.device_class = APEX_TRAFFIC_ACTIVATION;
    dc.interface_flags = APEX_INTERFACE_FLAG_USB;  // host doesn't support
    dc.tx = device_tx;
    dc.tx_user = &bus;
    dc.on_class_rx = device_class_rx;
    dc.on_class_rx_user = &dev_rx;
    apex_device_init(&dev, &dc);
    now_ms = 0;

    Pump(0); Pump(1); Pump(1);
    // REJECT_INTERFACE — device may retry; we just confirm it didn't connect.
    EXPECT_NE(APEX_DEVICE_STATE_CONNECTED, apex_device_link_state(&dev));
}

/* ------------------------------------------------------------------------- */
/* Multi-device ("hub") discovery: one host, devices sharing the bus like a    */
/* passthrough node — host downlink broadcast to all devices, device uplinks   */
/* merged to the host. Each device filters by address, so connected devices    */
/* ignore id=0-addressed replies meant for a still-enumerating peer.           */
/* ------------------------------------------------------------------------- */

struct HubBus {
    std::vector<uint8_t> downlink;  // host -> all devices
    std::vector<uint8_t> uplink;    // all devices -> host (merged)
};
static void hub_host_tx(void* u, const uint8_t* b, size_t n) {
    auto* bus = static_cast<HubBus*>(u);
    bus->downlink.insert(bus->downlink.end(), b, b + n);
}
static void hub_dev_tx(void* u, const uint8_t* b, size_t n) {
    auto* bus = static_cast<HubBus*>(u);
    bus->uplink.insert(bus->uplink.end(), b, b + n);
}
static void hub_init_host(apex_host_t* host, HubBus* bus) {
    apex_host_cfg_t hc{};
    hc.supported_interfaces = APEX_INTERFACE_FLAG_GPIO;
    hc.host_state_period_ms = 1000;  // keeps connected devices' rx watchdog fed
    hc.tx = hub_host_tx;
    hc.tx_user = bus;
    apex_host_init(host, &hc);
    // Register the class so DEVICE_INFO(class=ACTIVATION) is accepted; no rx needed.
    apex_host_register_class(host, APEX_TRAFFIC_ACTIVATION, nullptr, nullptr);
}
static void hub_init_device(apex_device_t* d, HubBus* bus) {
    apex_device_cfg_t dc{};
    dc.device_class = APEX_TRAFFIC_ACTIVATION;
    dc.interface_flags = APEX_INTERFACE_FLAG_GPIO;
    dc.tx = hub_dev_tx;
    dc.tx_user = bus;
    apex_device_init(d, &dc);
}

// Two devices enumerating one-after-another must receive DISTINCT device_ids.
// (The v1 sticky-dedup would have replayed device A's assignment to device B;
// the CONFIG_ACK handshake is what lets the host tell them apart.)
TEST(DiscoveryHub, TwoDevicesGetDistinctIds) {
    HubBus bus{};
    apex_host_t host{};
    hub_init_host(&host, &bus);
    apex_device_t devA{}, devB{};
    hub_init_device(&devA, &bus);
    hub_init_device(&devB, &bus);

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

    // Phase 1: device A enumerates alone.
    for (int i = 0; i < 16; i++) pump(false);
    ASSERT_EQ(APEX_DEVICE_STATE_CONNECTED, apex_device_link_state(&devA));
    uint8_t idA = apex_device_get_id(&devA);

    // Phase 2: device B joins — must get its own id, not a replay of A's.
    for (int i = 0; i < 16; i++) pump(true);
    ASSERT_EQ(APEX_DEVICE_STATE_CONNECTED, apex_device_link_state(&devB));
    uint8_t idB = apex_device_get_id(&devB);

    EXPECT_NE(APEX_DEVICE_ID_UNASSIGNED, idA);
    EXPECT_NE(APEX_DEVICE_ID_UNASSIGNED, idB);
    EXPECT_NE(idA, idB);
    EXPECT_EQ(APEX_DEVICE_STATE_CONNECTED, apex_device_link_state(&devA));  // A still up
    EXPECT_EQ(2u, apex_host_device_count(&host));
}

// A stale DEVICE_INFO(id=0) arriving after a device has connected (a buffered
// frame from a vanished talker) gets an isolated provisional slot that recycles
// — it never disturbs the connected device's slot or id.
TEST(DiscoveryHub, StaleDeviceInfoAfterConnectIsIsolated) {
    HubBus bus{};
    apex_host_t host{};
    hub_init_host(&host, &bus);
    apex_device_t devA{}, ghost{};
    hub_init_device(&devA, &bus);
    hub_init_device(&ghost, &bus);

    uint32_t now = 0;
    auto step = [&](bool tickGhost, bool feedGhost) {
        now += 1;
        apex_device_tick(&devA, now);
        if (tickGhost) apex_device_tick(&ghost, now);
        apex_host_tick(&host, now);
        if (!bus.uplink.empty()) {
            auto b = std::move(bus.uplink); bus.uplink.clear();
            apex_host_feed_rx(&host, b.data(), b.size(), now);
        }
        if (!bus.downlink.empty()) {
            auto b = std::move(bus.downlink); bus.downlink.clear();
            apex_device_feed_rx(&devA, b.data(), b.size(), now);
            if (feedGhost) apex_device_feed_rx(&ghost, b.data(), b.size(), now);
        }
    };

    // Device A connects.
    for (int i = 0; i < 16; i++) step(/*tickGhost=*/false, /*feedGhost=*/false);
    ASSERT_EQ(APEX_DEVICE_STATE_CONNECTED, apex_device_link_state(&devA));
    uint8_t idA = apex_device_get_id(&devA);
    ASSERT_EQ(1u, apex_host_device_count(&host));

    // The ghost emits one DEVICE_INFO(id=0) but never receives the reply (its
    // downlink is dropped), so it never latches → the host holds a provisional
    // phantom slot.
    apex_device_tick(&ghost, ++now);            // ghost -> DEVICE_INFO(id=0)
    {
        auto b = std::move(bus.uplink); bus.uplink.clear();
        apex_host_feed_rx(&host, b.data(), b.size(), now);
    }
    bus.downlink.clear();                        // drop the reply to the ghost

    // The real device is untouched, and the phantom is a separate slot.
    EXPECT_EQ(APEX_DEVICE_STATE_CONNECTED, apex_device_link_state(&devA));
    const apex_host_device_slot_t* slotA = apex_host_get_device(&host, idA);
    ASSERT_NE(nullptr, slotA);
    EXPECT_EQ(APEX_DEV_STATUS_CONNECTED, slotA->status);
    EXPECT_EQ(2u, apex_host_device_count(&host));  // real + provisional phantom

    // Phantom recycles after the watchdog; the real device persists throughout
    // (HOST_STATE + its heartbeats keep both watchdogs fed).
    for (int i = 0; i < 8; i++) step(/*tickGhost=*/false, /*feedGhost=*/false), now += 999;
    EXPECT_EQ(APEX_DEVICE_STATE_CONNECTED, apex_device_link_state(&devA));
    EXPECT_EQ(idA, apex_device_get_id(&devA));
    EXPECT_EQ(1u, apex_host_device_count(&host));   // phantom gone, real device intact
    EXPECT_NE(nullptr, apex_host_get_device(&host, idA));
}

}  // namespace
