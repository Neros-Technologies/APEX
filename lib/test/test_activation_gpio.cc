/* Exercises GPIO-backed preconditions (apex_activation_gpio_binding_t) and the
 * class-version-1 rule that no trigger source fires outside ENABLED — including
 * the new transient ENABLING state.
 *
 * Same loopback harness as test_activation_walkthrough.cc. The device declares
 * two preconditions, both validated by GPIO lines rather than by the application
 * calling apex_activation_device_set_precondition_state():
 *   - precondition 0: Pin 3, active-high, no debounce, auto-start.
 *   - precondition 1: Pin 4, active-low, 50 ms stable, host-start.
 * It also declares two trigger sources (HOST_COMMAND + HARDWARE_INPUT) and a
 * non-instant enable, so the ENABLING state can be observed. */
#include "apex/apex_activation.h"
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

struct StatusCapture {
    int count = 0;
    apex_activation_status_t last{};
};
static void on_status(void* u, uint8_t, const apex_activation_status_t* s) {
    auto* p = static_cast<StatusCapture*>(u);
    p->count++;
    p->last = *s;
}

struct CapCapture {
    int count = 0;
    apex_activation_capability_t last{};
};
static void on_cap(void* u, uint8_t, const apex_activation_capability_t* c) {
    auto* p = static_cast<CapCapture*>(u);
    p->count++;
    p->last = *c;
}

struct AckCapture {
    int count = 0;
    apex_activation_ack_t last{};
};
static void on_ack(void* u, uint8_t, const apex_activation_ack_t* a) {
    auto* p = static_cast<AckCapture*>(u);
    p->count++;
    p->last = *a;
}

// Simulated GPIO lines, indexed by connector pin number.
struct GpioLines {
    bool level[16] = {false};
};
static bool gpio_read_cb(void* u, uint8_t pin) {
    return static_cast<GpioLines*>(u)->level[pin];
}

class ActivationGpio : public ::testing::Test {
protected:
    Bus bus{};
    apex_host_t host{};
    apex_device_t dev_core{};
    apex_activation_host_t act_host{};
    apex_activation_device_t act_dev{};
    StatusCapture status{};
    CapCapture cap{};
    AckCapture ack{};
    GpioLines lines{};
    apex_activation_device_caps_t caps_{};
    apex_activation_device_hooks_t dh_{};
    bool enable_began = false;
    uint32_t now_ms = 0;

    static void on_enable_begin_cb(void* u) {
        static_cast<ActivationGpio*>(u)->enable_began = true;
    }

    static void dev_class_rx_trampoline(void* u, const uint8_t* p, size_t n) {
        apex_activation_device_on_rx(static_cast<apex_activation_device_t*>(u), p, n);
    }

    void SetUp() override {
        apex_host_cfg_t hc{};
        hc.supported_interfaces = APEX_INTERFACE_FLAG_GPIO;
        hc.host_state_period_ms = 0;
        hc.tx = host_tx;
        hc.tx_user = &bus;
        apex_host_init(&host, &hc);

        apex_activation_host_hooks_t hh{};
        hh.on_status = on_status; hh.on_status_user = &status;
        hh.on_capability = on_cap; hh.on_capability_user = &cap;
        hh.on_ack = on_ack; hh.on_ack_user = &ack;
        ASSERT_EQ(APEX_OK, apex_activation_host_init(&act_host, &host, &hh));

        apex_device_cfg_t dc{};
        dc.device_class = APEX_TRAFFIC_ACTIVATION;
        dc.interface_flags = APEX_INTERFACE_FLAG_GPIO;
        dc.tx = device_tx;
        dc.tx_user = &bus;
        dc.on_class_rx = dev_class_rx_trampoline;
        dc.on_class_rx_user = &act_dev;
        apex_device_init(&dev_core, &dc);

        caps_.n_preconditions = 2;
        caps_.n_trigger_sources = 2;
        caps_.trigger_source_categories[0] = APEX_TRIGGER_HOST_COMMAND;
        caps_.trigger_source_categories[1] = APEX_TRIGGER_HARDWARE_INPUT;
        caps_.auto_start_mask = (1u << 0);   // precondition 0 auto-starts
        caps_.initial_activations_remaining = 1;
        // precondition 0: Pin 3, active-high, no debounce.
        caps_.gpio_bindings[0] = {0, APEX_ACTIVATION_GPIO_PIN3, true, 0};
        // precondition 1: Pin 4, active-low, must be stable 50 ms.
        caps_.gpio_bindings[1] = {1, APEX_ACTIVATION_GPIO_PIN4, false, 50};
        caps_.n_gpio_bindings = 2;
        // Non-instant enable so ENABLING can be observed.
        dh_.gpio_read = gpio_read_cb; dh_.gpio_read_user = &lines;
        dh_.on_enable_begin = on_enable_begin_cb; dh_.on_enable_begin_user = this;
        ASSERT_EQ(APEX_OK,
                  apex_activation_device_init(&act_dev, &dev_core, &caps_, &dh_));
    }

    void Pump(uint32_t advance_ms = 0, int rounds = 1) {
        for (int i = 0; i < rounds; i++) {
            now_ms += advance_ms;
            apex_device_tick(&dev_core, now_ms);
            apex_activation_device_tick(&act_dev, now_ms);
            apex_host_tick(&host, now_ms);
            if (!bus.d2h.empty()) {
                auto b = std::move(bus.d2h); bus.d2h.clear();
                apex_host_feed_rx(&host, b.data(), b.size(), now_ms);
            }
            if (!bus.h2d.empty()) {
                auto b = std::move(bus.h2d); bus.h2d.clear();
                apex_device_feed_rx(&dev_core, b.data(), b.size(), now_ms);
            }
        }
    }
    void PumpUntilQuiet(int rounds = 16) { for (int i = 0; i < rounds; i++) Pump(1); }

    // Drive both GPIO preconditions to Valid, leaving the device in READY.
    void DriveToReady() {
        lines.level[APEX_ACTIVATION_GPIO_PIN3] = true;   // precond 0 active-high
        Pump(1);
        uint8_t dev_id = apex_device_get_id(&dev_core);
        (void)apex_activation_host_start_precondition(&act_host, dev_id, 1);
        PumpUntilQuiet();
        lines.level[APEX_ACTIVATION_GPIO_PIN4] = false;  // precond 1 active-low
        Pump(60);
        PumpUntilQuiet();
    }
};

// Precondition rejects init with a binding to an undeclared precondition.
TEST_F(ActivationGpio, InitRejectsBadBindingIndex) {
    apex_activation_device_t bad{};
    apex_activation_device_caps_t caps{};
    caps.n_preconditions = 1;
    caps.n_trigger_sources = 1;
    caps.trigger_source_categories[0] = APEX_TRIGGER_HOST_COMMAND;
    caps.n_gpio_bindings = 1;
    caps.gpio_bindings[0] = {5, APEX_ACTIVATION_GPIO_PIN3, true, 0};  // idx 5 >= 1
    apex_activation_device_hooks_t dh{};
    dh.gpio_read = gpio_read_cb; dh.gpio_read_user = &lines;
    EXPECT_EQ(APEX_ERR_INVALID_ARGS,
              apex_activation_device_init(&bad, &dev_core, &caps, &dh));
}

// Init requires a gpio_read hook when bindings are declared.
TEST_F(ActivationGpio, InitRequiresGpioReadHook) {
    apex_activation_device_t bad{};
    apex_activation_device_caps_t caps{};
    caps.n_preconditions = 1;
    caps.n_trigger_sources = 1;
    caps.trigger_source_categories[0] = APEX_TRIGGER_HOST_COMMAND;
    caps.n_gpio_bindings = 1;
    caps.gpio_bindings[0] = {0, APEX_ACTIVATION_GPIO_PIN3, true, 0};
    apex_activation_device_hooks_t dh{};  // no gpio_read
    EXPECT_EQ(APEX_ERR_INVALID_ARGS,
              apex_activation_device_init(&bad, &dev_core, &caps, &dh));
}

// The CAPABILITY frame declares which precondition each GPIO line validates, and
// the host learns the mapping during discovery.
TEST_F(ActivationGpio, CapabilityDeclaresGpioMapping) {
    PumpUntilQuiet();
    ASSERT_EQ(1, cap.count);
    ASSERT_EQ(2, cap.last.n_gpio_bindings);
    EXPECT_EQ(0, cap.last.gpio_bindings[0].precondition_idx);
    EXPECT_EQ(APEX_ACTIVATION_GPIO_PIN3, cap.last.gpio_bindings[0].pin);
    EXPECT_TRUE(cap.last.gpio_bindings[0].active_high);   // Pin 3 active-high
    EXPECT_EQ(1, cap.last.gpio_bindings[1].precondition_idx);
    EXPECT_EQ(APEX_ACTIVATION_GPIO_PIN4, cap.last.gpio_bindings[1].pin);
    EXPECT_FALSE(cap.last.gpio_bindings[1].active_high);  // Pin 4 active-low
}

TEST_F(ActivationGpio, GpioDrivesPreconditionsToReady) {
    PumpUntilQuiet();
    ASSERT_EQ(APEX_DEVICE_STATE_CONNECTED, apex_device_link_state(&dev_core));

    // precondition 0 auto-started (Running); precondition 1 is host-start so it
    // stays NotStarted and its Pin 4 line is not yet polled.
    ASSERT_GE(status.count, 1);
    EXPECT_EQ(APEX_ACTIVATION_STATE_VALIDATING, status.last.state);
    EXPECT_EQ(APEX_PRECOND_RUNNING, status.last.precondition_states[0]);
    EXPECT_EQ(APEX_PRECOND_NOT_STARTED, status.last.precondition_states[1]);

    // Drive Pin 3 high: precondition 0 latches Valid immediately (no debounce).
    lines.level[APEX_ACTIVATION_GPIO_PIN3] = true;
    Pump(1);
    EXPECT_EQ(APEX_PRECOND_VALID, status.last.precondition_states[0]);
    EXPECT_EQ(APEX_ACTIVATION_STATE_VALIDATING, status.last.state);

    // Pin 4 is already low (its active level), but precondition 1 is NotStarted.
    PumpUntilQuiet();
    EXPECT_EQ(APEX_PRECOND_NOT_STARTED, status.last.precondition_states[1]);

    // Host starts precondition 1 → Running, polling of Pin 4 begins.
    uint8_t dev_id = apex_device_get_id(&dev_core);
    ASSERT_EQ(APEX_OK, apex_activation_host_start_precondition(&act_host, dev_id, 1));
    PumpUntilQuiet();
    EXPECT_EQ(APEX_PRECOND_RUNNING, status.last.precondition_states[1]);

    // Pin 4 low = active. It must hold 50 ms before latching. 20 ms is not enough.
    Pump(20);
    EXPECT_EQ(APEX_PRECOND_RUNNING, status.last.precondition_states[1]);

    // A glitch high resets the streak.
    lines.level[APEX_ACTIVATION_GPIO_PIN4] = true;
    Pump(1);
    lines.level[APEX_ACTIVATION_GPIO_PIN4] = false;
    Pump(20);
    EXPECT_EQ(APEX_PRECOND_RUNNING, status.last.precondition_states[1]);

    // Hold active past the 50 ms window → Valid → READY (renumbered 0x03).
    Pump(60);
    EXPECT_EQ(APEX_PRECOND_VALID, status.last.precondition_states[1]);
    EXPECT_EQ(APEX_ACTIVATION_STATE_READY, status.last.state);
}

// A hardware/GPIO trigger source must NOT fire while the device is in the
// transient ENABLING state (§3): neither an internal source nor the host TRIGGER
// command. Once the enable completes (ENABLED) the trigger is honored.
TEST_F(ActivationGpio, GpioTriggerDoesNotFireDuringEnabling) {
    PumpUntilQuiet();
    uint8_t dev_id = apex_device_get_id(&dev_core);
    DriveToReady();
    ASSERT_EQ(APEX_ACTIVATION_STATE_READY, status.last.state);

    // Enable → non-instant → ENABLING.
    ASSERT_EQ(APEX_OK, apex_activation_host_set_enabled(&act_host, dev_id));
    PumpUntilQuiet();
    ASSERT_EQ(APEX_ACTIVATION_STATE_ENABLING, status.last.state);
    ASSERT_TRUE(enable_began);

    // Internal HARDWARE_INPUT source (index 1) during ENABLING → ignored.
    apex_activation_device_trigger(&act_dev, 1);
    EXPECT_EQ(APEX_ACTIVATION_STATE_ENABLING, apex_activation_device_state(&act_dev));

    // Host TRIGGER during ENABLING → REJECT_WRONG_STATE.
    ASSERT_EQ(APEX_OK, apex_activation_host_trigger(&act_host, dev_id));
    PumpUntilQuiet();
    EXPECT_EQ(APEX_ACT_CMD_TRIGGER, ack.last.acked_command);
    EXPECT_EQ(APEX_ACT_REJECT_WRONG_STATE, ack.last.result);
    EXPECT_EQ(APEX_ACTIVATION_STATE_ENABLING, apex_activation_device_state(&act_dev));

    // Complete the enable; now the same hardware trigger fires.
    apex_activation_device_transition_complete(&act_dev);
    PumpUntilQuiet();
    ASSERT_EQ(APEX_ACTIVATION_STATE_ENABLED, status.last.state);
    apex_activation_device_trigger(&act_dev, 1);
    EXPECT_EQ(APEX_ACTIVATION_STATE_EXECUTING, apex_activation_device_state(&act_dev));
    PumpUntilQuiet();
    EXPECT_EQ(1, status.last.last_trigger_source);  // HARDWARE_INPUT source index
}

}  // namespace
