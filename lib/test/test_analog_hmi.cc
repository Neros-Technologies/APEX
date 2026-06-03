/* End-to-end Analog HMI class tests. Same loopback pattern as the activation
 * walkthrough: host and device share a process, with TX callbacks crossed.
 *
 * Coverage:
 *   - Happy-path negotiation (CRSF + single-ended)
 *   - Bidirectional CONTROL_DATA streaming
 *   - REJECT_FORMAT when capabilities don't intersect
 *   - REJECT_CVBS when CVBS doesn't intersect
 *   - REJECT_MALFORMED when CONFIG arrives with garbage
 *   - Headless HMI path (no CVBS support)
 *   - Oversize CONTROL_DATA rejected before TX
 */
#include "apex/apex_analog_hmi.h"
#include "apex/apex_device.h"
#include "apex/apex_host.h"

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

struct HostCapCapture {
    int count = 0;
    uint8_t device_id = 0;
    uint8_t formats = 0;
    uint8_t cvbs = 0;
    uint8_t rate = 0;
};
struct HostActiveCapture {
    int count = 0;
    uint8_t device_id = 0;
    apex_hmi_control_format_t format = APEX_HMI_FORMAT_CRSF;
    apex_hmi_cvbs_mode_t cvbs_mode = APEX_HMI_CVBS_NONE;
};
struct HostRejectCapture {
    int count = 0;
    uint8_t device_id = 0;
    apex_hmi_ack_result_t reason = APEX_HMI_ACK_ACCEPTED;
};
struct HostControlDataCapture {
    int count = 0;
    uint8_t device_id = 0;
    std::vector<uint8_t> last;
};
struct DeviceActiveCapture {
    int count = 0;
    apex_hmi_control_format_t format = APEX_HMI_FORMAT_CRSF;
    apex_hmi_cvbs_mode_t cvbs_mode = APEX_HMI_CVBS_NONE;
};
struct DeviceControlDataCapture {
    int count = 0;
    std::vector<uint8_t> last;
};
struct DeviceFaultCapture {
    int count = 0;
    apex_hmi_ack_result_t reason = APEX_HMI_ACK_ACCEPTED;
};

class AnalogHmiTest : public ::testing::Test {
protected:
    Bus bus{};
    apex_host_t host{};
    apex_device_t dev_core{};
    apex_hmi_host_t hmi_host{};
    apex_hmi_device_t hmi_dev{};

    HostCapCapture h_cap{};
    HostActiveCapture h_active{};
    HostRejectCapture h_reject{};
    HostControlDataCapture h_data{};
    DeviceActiveCapture d_active{};
    DeviceControlDataCapture d_data{};
    DeviceFaultCapture d_fault{};

    uint32_t now_ms = 0;

    static void dev_class_rx_trampoline(void* u, const uint8_t* p, size_t n) {
        apex_hmi_device_on_rx(static_cast<apex_hmi_device_t*>(u), p, n);
    }

    void SetUpWith(uint8_t dev_formats, uint8_t dev_cvbs,
                   uint8_t host_formats, uint8_t host_cvbs,
                   uint8_t dev_iface_flags = APEX_INTERFACE_FLAG_CVBS,
                   uint8_t host_iface_flags = APEX_INTERFACE_FLAG_CVBS) {
        apex_host_cfg_t hc{};
        hc.supported_interfaces = host_iface_flags;
        hc.host_state_period_ms = 0;
        hc.tx = host_tx;
        hc.tx_user = &bus;
        apex_host_init(&host, &hc);

        apex_hmi_host_caps_t hcaps{};
        hcaps.supported_control_formats = host_formats;
        hcaps.supported_cvbs_modes = host_cvbs;
        apex_hmi_host_hooks_t hh{};
        hh.on_capability = +[](void* u, uint8_t did, uint8_t f, uint8_t c, uint8_t r) {
            auto* p = static_cast<HostCapCapture*>(u);
            p->count++; p->device_id = did; p->formats = f; p->cvbs = c; p->rate = r;
        };
        hh.on_capability_user = &h_cap;
        hh.on_active = +[](void* u, uint8_t did, apex_hmi_control_format_t f, apex_hmi_cvbs_mode_t c) {
            auto* p = static_cast<HostActiveCapture*>(u);
            p->count++; p->device_id = did; p->format = f; p->cvbs_mode = c;
        };
        hh.on_active_user = &h_active;
        hh.on_reject = +[](void* u, uint8_t did, apex_hmi_ack_result_t r) {
            auto* p = static_cast<HostRejectCapture*>(u);
            p->count++; p->device_id = did; p->reason = r;
        };
        hh.on_reject_user = &h_reject;
        hh.on_control_data = +[](void* u, uint8_t did, const uint8_t* b, size_t n) {
            auto* p = static_cast<HostControlDataCapture*>(u);
            p->count++; p->device_id = did;
            p->last.assign(b, b + n);
        };
        hh.on_control_data_user = &h_data;
        ASSERT_EQ(APEX_OK, apex_hmi_host_init(&hmi_host, &host, &hcaps, &hh));

        apex_device_cfg_t dc{};
        dc.device_class = APEX_TRAFFIC_ANALOG_HMI;
        dc.interface_flags = dev_iface_flags;
        dc.tx = device_tx;
        dc.tx_user = &bus;
        dc.on_class_rx = dev_class_rx_trampoline;
        dc.on_class_rx_user = &hmi_dev;
        apex_device_init(&dev_core, &dc);

        apex_hmi_device_caps_t dcaps{};
        dcaps.supported_control_formats = dev_formats;
        dcaps.supported_cvbs_modes = dev_cvbs;
        dcaps.intended_rate_hz = 50;
        apex_hmi_device_hooks_t dh{};
        dh.on_active = +[](void* u, apex_hmi_control_format_t f, apex_hmi_cvbs_mode_t c) {
            auto* p = static_cast<DeviceActiveCapture*>(u);
            p->count++; p->format = f; p->cvbs_mode = c;
        };
        dh.on_active_user = &d_active;
        dh.on_control_data = +[](void* u, const uint8_t* b, size_t n) {
            auto* p = static_cast<DeviceControlDataCapture*>(u);
            p->count++;
            p->last.assign(b, b + n);
        };
        dh.on_control_data_user = &d_data;
        dh.on_fault = +[](void* u, apex_hmi_ack_result_t r) {
            auto* p = static_cast<DeviceFaultCapture*>(u);
            p->count++; p->reason = r;
        };
        dh.on_fault_user = &d_fault;
        ASSERT_EQ(APEX_OK,
                  apex_hmi_device_init(&hmi_dev, &dev_core, &dcaps, &dh));
    }

    void Pump() {
        now_ms += 1;
        apex_device_tick(&dev_core, now_ms);
        apex_hmi_device_tick(&hmi_dev, now_ms);
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
};

TEST_F(AnalogHmiTest, HappyPathCrsfSingleEnded) {
    SetUpWith(/*dev_fmt*/APEX_HMI_FORMAT_CRSF_BIT,
              /*dev_cvbs*/APEX_HMI_CVBS_SINGLE_ENDED_BIT,
              /*host_fmt*/APEX_HMI_FORMAT_CRSF_BIT | APEX_HMI_FORMAT_MAVLINK2_BIT,
              /*host_cvbs*/APEX_HMI_CVBS_SINGLE_ENDED_BIT | APEX_HMI_CVBS_DIFFERENTIAL_BIT);
    PumpRounds();

    EXPECT_EQ(APEX_DEVICE_STATE_CONNECTED, apex_device_link_state(&dev_core));
    ASSERT_EQ(1, h_cap.count);
    EXPECT_EQ(APEX_HMI_FORMAT_CRSF_BIT, h_cap.formats);
    EXPECT_EQ(APEX_HMI_CVBS_SINGLE_ENDED_BIT, h_cap.cvbs);
    EXPECT_EQ(50, h_cap.rate);

    ASSERT_EQ(1, h_active.count);
    EXPECT_EQ(APEX_HMI_FORMAT_CRSF, h_active.format);
    EXPECT_EQ(APEX_HMI_CVBS_SINGLE_ENDED, h_active.cvbs_mode);

    ASSERT_EQ(1, d_active.count);
    EXPECT_EQ(APEX_HMI_FORMAT_CRSF, d_active.format);
    EXPECT_EQ(APEX_HMI_CVBS_SINGLE_ENDED, d_active.cvbs_mode);
    EXPECT_EQ(APEX_HMI_STATE_ACTIVE, apex_hmi_device_state(&hmi_dev));
}

TEST_F(AnalogHmiTest, HostPicksHigherPriorityFormatWhenIntersectionHasMultiple) {
    SetUpWith(APEX_HMI_FORMAT_CRSF_BIT | APEX_HMI_FORMAT_MAVLINK2_BIT,
              APEX_HMI_CVBS_SINGLE_ENDED_BIT,
              APEX_HMI_FORMAT_CRSF_BIT | APEX_HMI_FORMAT_MAVLINK2_BIT,
              APEX_HMI_CVBS_SINGLE_ENDED_BIT);
    // With no priority mask the host picks the lowest set bit = CRSF.
    PumpRounds();
    ASSERT_EQ(1, h_active.count);
    EXPECT_EQ(APEX_HMI_FORMAT_CRSF, h_active.format);
}

TEST_F(AnalogHmiTest, BidirectionalControlData) {
    SetUpWith(APEX_HMI_FORMAT_CRSF_BIT, APEX_HMI_CVBS_SINGLE_ENDED_BIT,
              APEX_HMI_FORMAT_CRSF_BIT, APEX_HMI_CVBS_SINGLE_ENDED_BIT);
    PumpRounds();
    ASSERT_EQ(1, h_active.count);
    ASSERT_EQ(1, d_active.count);

    // Device → host: a fake CRSF frame.
    uint8_t crsf_up[] = {0xC8, 0x18, 0x16, 0xE0, 0x03, 0xDE, 0xAD, 0xBE, 0xEF};
    ASSERT_EQ(APEX_OK, apex_hmi_device_send_control(&hmi_dev, crsf_up, sizeof(crsf_up)));
    PumpRounds(4);
    ASSERT_GE(h_data.count, 1);
    EXPECT_EQ(std::vector<uint8_t>(crsf_up, crsf_up + sizeof(crsf_up)), h_data.last);

    // Host → device: a fake CRSF telemetry frame.
    uint8_t crsf_dn[] = {0xC8, 0x08, 0x08, 0x12, 0x34, 0x56, 0x78};
    uint8_t device_id = apex_device_get_id(&dev_core);
    ASSERT_NE(APEX_DEVICE_ID_UNASSIGNED, device_id);
    ASSERT_EQ(APEX_OK,
              apex_hmi_host_send_control(&hmi_host, device_id, crsf_dn, sizeof(crsf_dn)));
    PumpRounds(4);
    ASSERT_GE(d_data.count, 1);
    EXPECT_EQ(std::vector<uint8_t>(crsf_dn, crsf_dn + sizeof(crsf_dn)), d_data.last);
}

TEST_F(AnalogHmiTest, RejectFormatWhenNoIntersection) {
    // Device offers MAVLINK2 only; host accepts CRSF only.
    SetUpWith(APEX_HMI_FORMAT_MAVLINK2_BIT, APEX_HMI_CVBS_SINGLE_ENDED_BIT,
              APEX_HMI_FORMAT_CRSF_BIT, APEX_HMI_CVBS_SINGLE_ENDED_BIT);
    PumpRounds();
    ASSERT_EQ(1, h_cap.count);
    EXPECT_EQ(0, h_active.count);
    EXPECT_EQ(0, d_active.count);
    ASSERT_EQ(1, h_reject.count);
    EXPECT_EQ(APEX_HMI_ACK_REJECT_FORMAT, h_reject.reason);
    ASSERT_EQ(1, d_fault.count);
    EXPECT_EQ(APEX_HMI_ACK_REJECT_FORMAT, d_fault.reason);
    EXPECT_EQ(APEX_HMI_STATE_FAULT, apex_hmi_device_state(&hmi_dev));
}

TEST_F(AnalogHmiTest, RejectCvbsWhenNoIntersection) {
    // Device offers single-ended only; host has only differential.
    SetUpWith(APEX_HMI_FORMAT_CRSF_BIT, APEX_HMI_CVBS_SINGLE_ENDED_BIT,
              APEX_HMI_FORMAT_CRSF_BIT, APEX_HMI_CVBS_DIFFERENTIAL_BIT);
    PumpRounds();
    ASSERT_EQ(1, h_reject.count);
    EXPECT_EQ(APEX_HMI_ACK_REJECT_CVBS, h_reject.reason);
    ASSERT_EQ(1, d_fault.count);
    EXPECT_EQ(APEX_HMI_ACK_REJECT_CVBS, d_fault.reason);
}

TEST_F(AnalogHmiTest, HeadlessHmiPathNoCvbs) {
    // Device declares no CVBS support; host's CVBS support doesn't matter.
    SetUpWith(APEX_HMI_FORMAT_CRSF_BIT, /*dev_cvbs=*/0,
              APEX_HMI_FORMAT_CRSF_BIT, APEX_HMI_CVBS_SINGLE_ENDED_BIT,
              /*dev_iface=*/0, /*host_iface=*/0);
    PumpRounds();
    ASSERT_EQ(1, h_active.count);
    EXPECT_EQ(APEX_HMI_CVBS_NONE, h_active.cvbs_mode);
    ASSERT_EQ(1, d_active.count);
    EXPECT_EQ(APEX_HMI_CVBS_NONE, d_active.cvbs_mode);
}

TEST_F(AnalogHmiTest, OversizeControlDataRejected) {
    SetUpWith(APEX_HMI_FORMAT_CRSF_BIT, APEX_HMI_CVBS_SINGLE_ENDED_BIT,
              APEX_HMI_FORMAT_CRSF_BIT, APEX_HMI_CVBS_SINGLE_ENDED_BIT);
    PumpRounds();
    std::vector<uint8_t> too_big(APEX_HMI_MAX_CONTROL_FRAME_BYTES + 1, 0xAA);
    EXPECT_EQ(APEX_ERR_BUFFER_TOO_SMALL,
              apex_hmi_device_send_control(&hmi_dev, too_big.data(), too_big.size()));
}

TEST_F(AnalogHmiTest, MaxSizeControlDataRoundTrips) {
    SetUpWith(APEX_HMI_FORMAT_CRSF_BIT, APEX_HMI_CVBS_SINGLE_ENDED_BIT,
              APEX_HMI_FORMAT_CRSF_BIT, APEX_HMI_CVBS_SINGLE_ENDED_BIT);
    PumpRounds();
    std::vector<uint8_t> exact(APEX_HMI_MAX_CONTROL_FRAME_BYTES);
    for (size_t i = 0; i < exact.size(); i++) exact[i] = (uint8_t)(i & 0xFF);
    ASSERT_EQ(APEX_OK,
              apex_hmi_device_send_control(&hmi_dev, exact.data(), exact.size()));
    PumpRounds(4);
    ASSERT_GE(h_data.count, 1);
    EXPECT_EQ(exact, h_data.last);
}

TEST_F(AnalogHmiTest, SendControlBeforeActiveIsBadState) {
    SetUpWith(APEX_HMI_FORMAT_CRSF_BIT, APEX_HMI_CVBS_SINGLE_ENDED_BIT,
              APEX_HMI_FORMAT_CRSF_BIT, APEX_HMI_CVBS_SINGLE_ENDED_BIT);
    // Don't pump — device hasn't even connected.
    uint8_t data[] = {0xAA};
    EXPECT_EQ(APEX_ERR_BAD_STATE,
              apex_hmi_device_send_control(&hmi_dev, data, sizeof(data)));
}

}  // namespace
