/* Replays the §9.4 single-activation walkthrough from APEX_Device_Class_Activation.md.
 *
 * Two same-process actors loopback to each other:
 *   - Host: apex_host_t + apex_activation_host_t
 *   - Device: apex_device_t + apex_activation_device_t
 *
 * The test drives the device through STANDBY → VALIDATING → READY → ENABLED →
 * EXECUTING → EXHAUSTED, observing the same state transitions the spec lists
 * step-by-step. */
#include "apex/apex_activation.h"
#include "apex/apex_device.h"
#include "apex/apex_host.h"

#include <gtest/gtest.h>

#include <cstring>
#include <string>
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

struct CapCapture {
    int count = 0;
    uint8_t device_id = 0;
    apex_activation_capability_t last{};
};
struct StatusCapture {
    int count = 0;
    uint8_t device_id = 0;
    apex_activation_status_t last{};
    std::vector<apex_activation_state_t> state_history;
};
struct AckCapture {
    int count = 0;
    uint8_t device_id = 0;
    apex_activation_ack_t last{};
};

static void on_cap(void* u, uint8_t did, const apex_activation_capability_t* c) {
    auto* p = static_cast<CapCapture*>(u);
    p->count++;
    p->device_id = did;
    p->last = *c;
}
static void on_status(void* u, uint8_t did, const apex_activation_status_t* s) {
    auto* p = static_cast<StatusCapture*>(u);
    p->count++;
    p->device_id = did;
    p->last = *s;
    if (p->state_history.empty() || p->state_history.back() != s->state) {
        p->state_history.push_back(s->state);
    }
}
static void on_ack(void* u, uint8_t did, const apex_activation_ack_t* a) {
    auto* p = static_cast<AckCapture*>(u);
    p->count++;
    p->device_id = did;
    p->last = *a;
}

/* Device precond declaration: precond 0 is device-local (host_condition NONE,
 * auto-started), precond 1 is host-started on PROPS_ON_FLYING with auto_trigger.
 * Static so the lib can store it by reference for the device's lifetime. */
static const apex_activation_precond_info_t kPrecondInfo[2] = {
    /* host_condition, condition_param, auto_trigger, not_started, running, valid, failed */
    { APEX_ACT_PRECOND_NONE,            0, 0, "SELFTEST PEND", "SELFTEST",   "SELFTEST OK", "SELFTEST FAIL" },
    { APEX_ACT_PRECOND_PROPS_ON_FLYING, 0, 1, "AWAIT TAKEOFF", "CONFIRMING", "AIRBORNE",    "TAKEOFF FAIL"  },
};

struct PrecondInfoCapture {
    int count = 0;
    uint8_t last_idx = 0xFF;
    uint8_t host_condition = 0xAA;   // sentinel: should be overwritten to NONE(0)
    uint8_t auto_trigger = 0xAA;
    std::string str_not_started;
};
static void on_precond_info(void* u, uint8_t /*did*/, uint8_t idx,
                            const apex_activation_precond_info_t* info) {
    auto* p = static_cast<PrecondInfoCapture*>(u);
    p->count++;
    p->last_idx = idx;
    p->host_condition = info->host_condition;   // expected NONE — not on the wire here
    p->auto_trigger = info->auto_trigger;       // expected 0
    p->str_not_started = info->str_not_started ? info->str_not_started : "";
}

class ActivationWalkthrough : public ::testing::Test {
protected:
    Bus bus{};
    apex_host_t host{};
    apex_device_t dev_core{};
    apex_activation_host_t act_host{};
    apex_activation_device_t act_dev{};
    CapCapture cap{};
    StatusCapture status{};
    AckCapture ack{};
    PrecondInfoCapture pinfo{};
    bool on_execute_fired = false;
    uint32_t now_ms = 0;

    static void on_execute_cb(void* u) { *static_cast<bool*>(u) = true; }

    /* Device class-RX context: forwards to the activation device, but also
     * counts inbound PRECOND_INFO_REQUEST frames and can optionally swallow them
     * (so the device never replies — used to exercise the host's retry). */
    struct DevRx {
        apex_activation_device_t* act;
        int req_count;
        bool swallow;
    };
    DevRx devrx{};

    static void dev_class_rx_trampoline(void* u, const uint8_t* p, size_t n) {
        auto* d = static_cast<DevRx*>(u);
        if (n >= 1 && p[0] == APEX_ACT_MSG_PRECOND_INFO_REQUEST) {
            d->req_count++;
            if (d->swallow) return;  /* drop → device never sends a reply */
        }
        apex_activation_device_on_rx(d->act, p, n);
    }

    void SetUp() override {
        apex_host_cfg_t hc{};
        hc.supported_interfaces = APEX_INTERFACE_FLAG_GPIO;
        hc.host_state_period_ms = 0;  // disable HOST_STATE for cleaner traces
        hc.tx = host_tx;
        hc.tx_user = &bus;
        apex_host_init(&host, &hc);

        apex_activation_host_hooks_t hh{};
        hh.on_capability = on_cap; hh.on_capability_user = &cap;
        hh.on_status = on_status; hh.on_status_user = &status;
        hh.on_ack = on_ack; hh.on_ack_user = &ack;
        hh.on_precond_info = on_precond_info; hh.on_precond_info_user = &pinfo;
        ASSERT_EQ(APEX_OK, apex_activation_host_init(&act_host, &host, &hh));

        // Device core.
        apex_device_cfg_t dc{};
        dc.device_class = APEX_TRAFFIC_ACTIVATION;
        dc.interface_flags = APEX_INTERFACE_FLAG_GPIO;
        dc.tx = device_tx;
        dc.tx_user = &bus;
        dc.on_class_rx = dev_class_rx_trampoline;
        devrx.act = &act_dev;
        dc.on_class_rx_user = &devrx;
        apex_device_init(&dev_core, &dc);

        // Activation device caps — the §9.1 example device.
        apex_activation_device_caps_t caps{};
        const uint8_t uuid[16] = {
            0x7D, 0x9A, 0x2C, 0x14, 0x3E, 0x6B, 0x4F, 0x08,
            0x9A, 0x51, 0xC2, 0xE0, 0x7B, 0x18, 0xD4, 0xF6,
        };
        memcpy(caps.payload_type_uuid, uuid, 16);
        caps.class_spec_version = 0;
        caps.n_preconditions = 2;
        caps.n_trigger_sources = 2;
        caps.trigger_source_categories[0] = APEX_TRIGGER_HOST_COMMAND;
        caps.trigger_source_categories[1] = APEX_TRIGGER_HARDWARE_INPUT;
        caps.auto_start_mask = (1u << 0);   // precondition 0 auto-starts
        caps.initial_activations_remaining = 1;
        caps.precond_info = kPrecondInfo;   // declares host_condition + strings

        apex_activation_device_hooks_t dh{};
        dh.on_execute = on_execute_cb;
        dh.on_execute_user = &on_execute_fired;
        ASSERT_EQ(APEX_OK,
                  apex_activation_device_init(&act_dev, &dev_core, &caps, &dh));
    }

    void Pump(uint32_t advance_ms = 0, int rounds = 1) {
        for (int i = 0; i < rounds; i++) {
            now_ms += advance_ms;
            apex_device_tick(&dev_core, now_ms);
            apex_activation_device_tick(&act_dev, now_ms);
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
    }

    void PumpUntilQuiet(int max_rounds = 16) {
        // Always pump some rounds — both transports may need to round-trip
        // multiple times for tick-driven side effects (CAPABILITY emission,
        // STATUS dirty flush) to land.
        for (int i = 0; i < max_rounds; i++) {
            Pump(1);
        }
    }
};

TEST_F(ActivationWalkthrough, FullSingleActivationLifecycle) {
    // ─── Discovery ─────────────────────────────────────────────────────────
    PumpUntilQuiet();
    ASSERT_EQ(APEX_DEVICE_STATE_CONNECTED, apex_device_link_state(&dev_core));

    // ─── Step 1: CAPABILITY ────────────────────────────────────────────────
    ASSERT_EQ(1, cap.count);
    EXPECT_EQ(2, cap.last.n_preconditions);
    EXPECT_EQ(2, cap.last.n_trigger_sources);
    EXPECT_EQ(APEX_TRIGGER_HOST_COMMAND, cap.last.trigger_source_categories[0]);
    EXPECT_EQ(APEX_TRIGGER_HARDWARE_INPUT, cap.last.trigger_source_categories[1]);
    // UUID check
    EXPECT_EQ(0x7D, cap.last.payload_type_uuid[0]);
    EXPECT_EQ(0xF6, cap.last.payload_type_uuid[15]);

    // ─── Step 2: STATUS — VALIDATING, precond 0 Running ────────────────────
    // First STATUS frame from device after CAPABILITY.
    ASSERT_GE(status.count, 1);
    EXPECT_EQ(APEX_ACTIVATION_STATE_VALIDATING, status.last.state);
    EXPECT_EQ(1, status.last.activations_remaining);
    EXPECT_EQ(0xFF, status.last.last_trigger_source);
    EXPECT_EQ(0, status.last.fault_flags);
    EXPECT_EQ(APEX_PRECOND_RUNNING, status.last.precondition_states[0]);
    EXPECT_EQ(APEX_PRECOND_NOT_STARTED, status.last.precondition_states[1]);

    // ─── Step 3: precondition 0 reaches Valid ──────────────────────────────
    apex_activation_device_set_precondition_state(&act_dev, 0, APEX_PRECOND_VALID);
    PumpUntilQuiet();
    EXPECT_EQ(APEX_PRECOND_VALID, status.last.precondition_states[0]);
    EXPECT_EQ(APEX_PRECOND_NOT_STARTED, status.last.precondition_states[1]);
    EXPECT_EQ(APEX_ACTIVATION_STATE_VALIDATING, status.last.state);

    // ─── Step 4: host issues START_PRECONDITION(1) ─────────────────────────
    uint8_t dev_id = apex_device_get_id(&dev_core);
    ASSERT_EQ(APEX_OK,
              apex_activation_host_start_precondition(&act_host, dev_id, 1));
    PumpUntilQuiet();
    ASSERT_GE(ack.count, 1);
    EXPECT_EQ(APEX_ACT_CMD_START_PRECONDITION, ack.last.acked_command);
    EXPECT_EQ(APEX_ACT_ACCEPTED, ack.last.result);
    EXPECT_EQ(APEX_ACTIVATION_STATE_VALIDATING, ack.last.current_state);
    EXPECT_EQ(APEX_PRECOND_RUNNING, status.last.precondition_states[1]);

    // ─── Step 5: precondition 1 reaches Valid → READY ──────────────────────
    apex_activation_device_set_precondition_state(&act_dev, 1, APEX_PRECOND_VALID);
    PumpUntilQuiet();
    EXPECT_EQ(APEX_ACTIVATION_STATE_READY, status.last.state);

    // ─── Step 6: host issues SET_ENABLED → ENABLED ────────────────────────
    int ack_before = ack.count;
    ASSERT_EQ(APEX_OK, apex_activation_host_set_enabled(&act_host, dev_id));
    PumpUntilQuiet();
    EXPECT_EQ(ack_before + 1, ack.count);
    EXPECT_EQ(APEX_ACT_CMD_SET_ENABLED, ack.last.acked_command);
    EXPECT_EQ(APEX_ACT_ACCEPTED, ack.last.result);
    EXPECT_EQ(APEX_ACTIVATION_STATE_ENABLED, ack.last.current_state);
    EXPECT_EQ(APEX_ACTIVATION_STATE_ENABLED, status.last.state);

    // ─── Step 7: host issues TRIGGER → EXECUTING ──────────────────────────
    ASSERT_EQ(APEX_OK, apex_activation_host_trigger(&act_host, dev_id));
    PumpUntilQuiet();
    EXPECT_EQ(APEX_ACT_CMD_TRIGGER, ack.last.acked_command);
    EXPECT_EQ(APEX_ACT_ACCEPTED, ack.last.result);
    EXPECT_EQ(APEX_ACTIVATION_STATE_EXECUTING, ack.last.current_state);
    EXPECT_EQ(APEX_ACTIVATION_STATE_EXECUTING, status.last.state);
    EXPECT_EQ(0, status.last.last_trigger_source);  // HOST_COMMAND is index 0
    EXPECT_EQ(1, status.last.activations_remaining);  // not yet decremented
    EXPECT_TRUE(on_execute_fired);

    // ─── Step 8: action completes → EXHAUSTED ──────────────────────────────
    apex_activation_device_complete_execution(&act_dev);
    PumpUntilQuiet();
    EXPECT_EQ(APEX_ACTIVATION_STATE_EXHAUSTED, status.last.state);
    EXPECT_EQ(0, status.last.activations_remaining);

    // §7.3: host core lifecycle should be EXPENDED.
    const apex_host_device_slot_t* slot = apex_host_get_device(&host, dev_id);
    ASSERT_NE(nullptr, slot);
    EXPECT_EQ(APEX_DEV_STATUS_EXPENDED, slot->status);

    // State history should include each expected transition (allowing
    // periodic STATUS frames between them).
    bool seen_validating = false, seen_ready = false, seen_enabled = false,
         seen_executing = false, seen_exhausted = false;
    for (auto s : status.state_history) {
        if (s == APEX_ACTIVATION_STATE_VALIDATING) seen_validating = true;
        if (s == APEX_ACTIVATION_STATE_READY)      seen_ready = true;
        if (s == APEX_ACTIVATION_STATE_ENABLED)    seen_enabled = true;
        if (s == APEX_ACTIVATION_STATE_EXECUTING)  seen_executing = true;
        if (s == APEX_ACTIVATION_STATE_EXHAUSTED)  seen_exhausted = true;
    }
    EXPECT_TRUE(seen_validating);
    EXPECT_TRUE(seen_ready);
    EXPECT_TRUE(seen_enabled);
    EXPECT_TRUE(seen_executing);
    EXPECT_TRUE(seen_exhausted);
}

TEST_F(ActivationWalkthrough, RejectsTriggerWhenNotEnabled) {
    PumpUntilQuiet();
    uint8_t dev_id = apex_device_get_id(&dev_core);
    ASSERT_NE(APEX_DEVICE_ID_UNASSIGNED, dev_id);

    // Don't enable. TRIGGER from READY (well, VALIDATING in this case) should
    // be rejected. We'll go through to READY first to make it more interesting.
    apex_activation_device_set_precondition_state(&act_dev, 0, APEX_PRECOND_VALID);
    apex_activation_device_set_precondition_state(&act_dev, 1, APEX_PRECOND_VALID);
    PumpUntilQuiet();
    ASSERT_EQ(APEX_ACTIVATION_STATE_READY, status.last.state);

    int ack_before = ack.count;
    ASSERT_EQ(APEX_OK, apex_activation_host_trigger(&act_host, dev_id));
    PumpUntilQuiet();
    EXPECT_EQ(ack_before + 1, ack.count);
    EXPECT_EQ(APEX_ACT_CMD_TRIGGER, ack.last.acked_command);
    EXPECT_EQ(APEX_ACT_REJECT_WRONG_STATE, ack.last.result);
}

TEST_F(ActivationWalkthrough, BadPreconditionIndexRejected) {
    PumpUntilQuiet();
    uint8_t dev_id = apex_device_get_id(&dev_core);

    int ack_before = ack.count;
    ASSERT_EQ(APEX_OK,
              apex_activation_host_start_precondition(&act_host, dev_id, 99));
    PumpUntilQuiet();
    EXPECT_EQ(ack_before + 1, ack.count);
    EXPECT_EQ(APEX_ACT_REJECT_BAD_INDEX, ack.last.result);
}

/* The host-start hint travels in CAPABILITY, sparse: only host-evaluated
 * preconditions appear. Precond 1 (PROPS_ON_FLYING) is listed; precond 0
 * (host_condition NONE, device-local) is omitted. */
TEST_F(ActivationWalkthrough, CapabilityCarriesHostConditionBindings) {
    PumpUntilQuiet();
    ASSERT_GE(cap.count, 1);
    ASSERT_EQ(1, cap.last.n_host_conditions);
    EXPECT_EQ(1, cap.last.host_conditions[0].precondition_idx);
    EXPECT_EQ(APEX_ACT_PRECOND_PROPS_ON_FLYING, cap.last.host_conditions[0].host_condition);
    EXPECT_EQ(1, cap.last.host_conditions[0].auto_trigger);
}

/* PRECOND_INFO_REPLY is display-strings only now: the callback delivers the
 * strings, and the host_condition/auto_trigger fields are NOT taken from the
 * wire (they're zeroed — the real values are in CAPABILITY, asserted above). */
TEST_F(ActivationWalkthrough, PrecondInfoReplyIsStringsOnly) {
    PumpUntilQuiet();
    uint8_t did = apex_device_get_id(&dev_core);
    ASSERT_NE(APEX_DEVICE_ID_UNASSIGNED, did);

    ASSERT_EQ(APEX_OK, apex_activation_host_request_precond_info(&act_host, did, 1, 24));
    PumpUntilQuiet();

    ASSERT_GE(pinfo.count, 1);
    EXPECT_EQ(1, pinfo.last_idx);
    EXPECT_EQ("AWAIT TAKEOFF", pinfo.str_not_started);
    EXPECT_EQ(APEX_ACT_PRECOND_NONE, pinfo.host_condition);  // not carried here
    EXPECT_EQ(0, pinfo.auto_trigger);
}

/* A dropped PRECOND_INFO_REPLY self-heals: while the reply is suppressed the
 * host re-requests on each STATUS; once the reply gets through, it caches. */
TEST_F(ActivationWalkthrough, PrecondInfoRetriedUntilCached) {
    PumpUntilQuiet();
    uint8_t did = apex_device_get_id(&dev_core);
    ASSERT_NE(APEX_DEVICE_ID_UNASSIGNED, did);

    devrx.swallow = true;  // device receives requests but never replies
    int before = devrx.req_count;
    ASSERT_EQ(APEX_OK, apex_activation_host_request_precond_info(&act_host, did, 1, 24));

    // Advance several STATUS periods (1 Hz). Each STATUS should re-trigger a request.
    for (int i = 0; i < 4; i++) Pump(1100, 1);
    EXPECT_GE(devrx.req_count - before, 3);  // initial + repeated retries
    EXPECT_EQ(0, pinfo.count);               // nothing cached while swallowed

    // Let the reply through — the next retry round caches it and stops.
    devrx.swallow = false;
    for (int i = 0; i < 3; i++) Pump(1100, 1);
    EXPECT_GE(pinfo.count, 1);
    EXPECT_EQ("AWAIT TAKEOFF", pinfo.str_not_started);
}

}  // namespace
