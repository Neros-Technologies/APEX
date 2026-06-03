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

}  // namespace
