/* Replays the §9 single-activation walkthrough from
 * APEX_Device_Class_Activation.md (Activation **class version 1**), asserting
 * the on-wire frames BYTE-FOR-BYTE against the spec, plus the v1 additions:
 * the ENABLING/DISABLING transient states, REJECT_BUSY, TRANSITION_FAILED,
 * instant vs non-instant enable, HOST_DISPLAY_INFO, and DISPLAY_TEXT.
 *
 * Two same-process actors loopback to each other:
 *   - Host:   apex_host_t   + apex_activation_host_t
 *   - Device: apex_device_t + apex_activation_device_t
 *
 * Every device→host and host→device frame is decoded off the wire (COBS + CRC +
 * outer header stripped) so the tests can compare the pre-CRC/COBS bytes the
 * spec §9 tables list (PV=01, TT=02, ID=02, LN, inner payload). */
#include "apex/apex_activation.h"
#include "apex/apex_core.h"
#include "apex/apex_device.h"
#include "apex/apex_framer.h"
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

/* One decoded on-wire frame: outer header + inner payload (class_msg_id+body). */
struct WireFrame {
    apex_hdr_t hdr;
    std::vector<uint8_t> payload;
};

/* Split a raw TX byte stream on 0x00 delimiters and decode each COBS frame. */
static void decode_stream(const std::vector<uint8_t>& bytes,
                          std::vector<WireFrame>& out) {
    size_t i = 0;
    while (i < bytes.size()) {
        size_t j = i;
        while (j < bytes.size() && bytes[j] != 0x00) j++;
        if (j > i) {
            uint8_t ws[APEX_MAX_FRAME_LENGTH];
            apex_hdr_t hdr{};
            const uint8_t* pl = nullptr;
            size_t pll = 0;
            if (apex_frame_decode(&bytes[i], j - i, ws, sizeof(ws), &hdr, &pl, &pll)
                == APEX_OK) {
                WireFrame f;
                f.hdr = hdr;
                if (pl && pll) f.payload.assign(pl, pl + pll);
                out.push_back(std::move(f));
            }
        }
        i = j + 1;  /* skip the delimiter */
    }
}

struct CapCapture {
    int count = 0;
    uint8_t device_id = 0;
    apex_activation_capability_t last{};
    /* When set, the host answers CAPABILITY with a HOST_DISPLAY_INFO frame
     * (§9 Step 1 — the host declares 20-char lines, 1 banner). */
    apex_activation_host_t* host = nullptr;
    bool send_display_info = false;
    uint8_t di_char_limit = 20;
    uint8_t di_banner = 1;
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
struct DisplayTextCapture {
    int count = 0;
    uint8_t last_target = 0xAB;
    std::string last_text;
};

static void on_cap(void* u, uint8_t did, const apex_activation_capability_t* c) {
    auto* p = static_cast<CapCapture*>(u);
    p->count++;
    p->device_id = did;
    p->last = *c;
    if (p->send_display_info && p->host) {
        apex_activation_host_send_display_info(p->host, did, p->di_char_limit,
                                               p->di_banner);
    }
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
static void on_display_text(void* u, uint8_t /*did*/, uint8_t target,
                            const char* text, size_t len) {
    auto* p = static_cast<DisplayTextCapture*>(u);
    p->count++;
    p->last_target = target;
    p->last_text.assign(text ? text : "", len);
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
    DisplayTextCapture dtext{};
    apex_activation_device_caps_t caps_{};
    apex_activation_device_hooks_t dh_{};
    bool on_execute_fired = false;
    bool enable_began = false;
    bool disable_began = false;
    uint32_t now_ms = 0;

    std::vector<WireFrame> dwire;  /* all device→host frames, whole test */
    std::vector<WireFrame> hwire;  /* all host→device frames, whole test */

    static void on_execute_cb(void* u) {
        static_cast<ActivationWalkthrough*>(u)->on_execute_fired = true;
    }
    static void on_enable_begin_cb(void* u) {
        static_cast<ActivationWalkthrough*>(u)->enable_began = true;
    }
    static void on_disable_begin_cb(void* u) {
        static_cast<ActivationWalkthrough*>(u)->disable_began = true;
    }

    static void dev_class_rx_trampoline(void* u, const uint8_t* p, size_t n) {
        apex_activation_device_on_rx(static_cast<apex_activation_device_t*>(u), p, n);
    }

    void SetUp() override {
        apex_host_cfg_t hc{};
        hc.supported_interfaces = APEX_INTERFACE_FLAG_GPIO;
        hc.host_state_period_ms = 0;  // disable HOST_STATE for cleaner traces
        hc.tx = host_tx;
        hc.tx_user = &bus;
        apex_host_init(&host, &hc);

        cap.host = &act_host;
        cap.send_display_info = true;
        apex_activation_host_hooks_t hh{};
        hh.on_capability = on_cap; hh.on_capability_user = &cap;
        hh.on_status = on_status; hh.on_status_user = &status;
        hh.on_ack = on_ack; hh.on_ack_user = &ack;
        hh.on_display_text = on_display_text; hh.on_display_text_user = &dtext;
        ASSERT_EQ(APEX_OK, apex_activation_host_init(&act_host, &host, &hh));

        apex_device_cfg_t dc{};
        dc.device_class = APEX_TRAFFIC_ACTIVATION;
        dc.interface_flags = APEX_INTERFACE_FLAG_GPIO;
        dc.tx = device_tx;
        dc.tx_user = &bus;
        dc.on_class_rx = dev_class_rx_trampoline;
        dc.on_class_rx_user = &act_dev;
        apex_device_init(&dev_core, &dc);

        // Activation device caps — the §9.1 example device: single-activation
        // marker, 2 preconditions, 2 trigger sources, NON-instant enable, no
        // host-condition bindings and no timing tail (22-byte CAPABILITY).
        const uint8_t uuid[16] = {
            0x7D, 0x9A, 0x2C, 0x14, 0x3E, 0x6B, 0x4F, 0x08,
            0x9A, 0x51, 0xC2, 0xE0, 0x7B, 0x18, 0xD4, 0xF6,
        };
        memcpy(caps_.payload_type_uuid, uuid, 16);
        caps_.n_preconditions = 2;
        caps_.n_trigger_sources = 2;
        caps_.trigger_source_categories[0] = APEX_TRIGGER_HOST_COMMAND;
        caps_.trigger_source_categories[1] = APEX_TRIGGER_HARDWARE_INPUT;
        caps_.auto_start_mask = (1u << 0);   // precondition 0 auto-starts
        caps_.initial_activations_remaining = 1;

        dh_.on_execute = on_execute_cb;      dh_.on_execute_user = this;
        dh_.on_enable_begin = on_enable_begin_cb;  dh_.on_enable_begin_user = this;
        dh_.on_disable_begin = on_disable_begin_cb; dh_.on_disable_begin_user = this;
        ReinitDevice();
    }

    void ReinitDevice() {
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
                auto b = std::move(bus.d2h);
                bus.d2h.clear();
                decode_stream(b, dwire);
                apex_host_feed_rx(&host, b.data(), b.size(), now_ms);
            }
            if (!bus.h2d.empty()) {
                auto b = std::move(bus.h2d);
                bus.h2d.clear();
                decode_stream(b, hwire);
                apex_device_feed_rx(&dev_core, b.data(), b.size(), now_ms);
            }
        }
    }

    void PumpUntilQuiet(int max_rounds = 16) {
        for (int i = 0; i < max_rounds; i++) Pump(1);
    }

    // Does a device→host frame with these exact bytes exist? Header is asserted
    // as PV=01, TT=02, ID=`id`, LN=payload.size() (payload_length == the vector
    // length by construction), giving a byte-for-byte match with the spec table.
    bool HasDevFrame(const std::vector<uint8_t>& payload, uint8_t id = 0x02) {
        for (auto& f : dwire) {
            if (f.hdr.protocol_version == 0x01 && f.hdr.traffic_type == 0x02 &&
                f.hdr.device_id == id && f.payload == payload) {
                return true;
            }
        }
        return false;
    }
    bool HasHostFrame(const std::vector<uint8_t>& payload, uint8_t id = 0x02) {
        for (auto& f : hwire) {
            if (f.hdr.protocol_version == 0x01 && f.hdr.traffic_type == 0x02 &&
                f.hdr.device_id == id && f.payload == payload) {
                return true;
            }
        }
        return false;
    }

    void ValidateBothPreconditions() {
        apex_activation_device_set_precondition_state(&act_dev, 0, APEX_PRECOND_VALID);
        apex_activation_device_set_precondition_state(&act_dev, 1, APEX_PRECOND_VALID);
        PumpUntilQuiet();
    }
};

// ─────────────────────────────────────────────────────────────────────────────
//  §9.4 single-activation walkthrough — byte-for-byte
// ─────────────────────────────────────────────────────────────────────────────
TEST_F(ActivationWalkthrough, FullSingleActivationLifecycleByteExact) {
    PumpUntilQuiet();
    ASSERT_EQ(APEX_DEVICE_STATE_CONNECTED, apex_device_link_state(&dev_core));
    uint8_t dev_id = apex_device_get_id(&dev_core);
    ASSERT_EQ(0x02, dev_id) << "spec §9 assigns device_id 0x02";

    // ── Step 1: CAPABILITY (22-byte inner payload, LN=0x16) ─────────────────
    ASSERT_EQ(1, cap.count);
    EXPECT_EQ(2, cap.last.n_preconditions);
    EXPECT_EQ(2, cap.last.n_trigger_sources);
    EXPECT_EQ(0, cap.last.n_host_conditions);
    EXPECT_EQ(0, cap.last.enable_time_ms);
    EXPECT_TRUE(HasDevFrame({
        0x01,  // class_msg_id = CAPABILITY
        0x7D, 0x9A, 0x2C, 0x14, 0x3E, 0x6B, 0x4F, 0x08,
        0x9A, 0x51, 0xC2, 0xE0, 0x7B, 0x18, 0xD4, 0xF6,  // uuid
        0x02,  // n_preconditions
        0x02,  // n_trigger_sources
        0x01, 0x02,  // categories: HOST_COMMAND, HARDWARE_INPUT
        0x00,  // n_gpio_bindings = 0 (frame ends here)
    })) << "CAPABILITY must match §9 Step 1 (LN=0x16)";

    // ── Step 1: host answers with HOST_DISPLAY_INFO (07 14 01, LN=03) ───────
    EXPECT_TRUE(HasHostFrame({0x07, 0x14, 0x01}))
        << "HOST_DISPLAY_INFO char_limit=20, n_banner_lines=1";

    // ── Step 2: STATUS (VALIDATING, precond 0 Running) — 03 02 01 FF 00 00 01 00
    EXPECT_TRUE(HasDevFrame({0x03, 0x02, 0x01, 0xFF, 0x00, 0x00, 0x01, 0x00}));

    // ── Step 2: DISPLAY_TEXT push during VALIDATING (target 0, LN=0x13) ─────
    ASSERT_EQ(APEX_OK,
              apex_activation_device_push_text(&act_dev, 0x00, "SELF TEST 2/3..."));
    PumpUntilQuiet();
    EXPECT_TRUE(HasDevFrame({
        0x08, 0x00, 0x10,  // DISPLAY_TEXT, target 0, text_len 16
        0x53, 0x45, 0x4C, 0x46, 0x20, 0x54, 0x45, 0x53, 0x54, 0x20,
        0x32, 0x2F, 0x33, 0x2E, 0x2E, 0x2E,  // "SELF TEST 2/3..."
    })) << "DISPLAY_TEXT must match §9 Step 2 (LN=0x13)";
    EXPECT_EQ(1, dtext.count);
    EXPECT_EQ(0x00, dtext.last_target);
    EXPECT_EQ("SELF TEST 2/3...", dtext.last_text);

    // ── Step 3: precondition 0 → Valid — 03 02 01 FF 00 00 02 00 ────────────
    apex_activation_device_set_precondition_state(&act_dev, 0, APEX_PRECOND_VALID);
    PumpUntilQuiet();
    EXPECT_TRUE(HasDevFrame({0x03, 0x02, 0x01, 0xFF, 0x00, 0x00, 0x02, 0x00}));

    // ── Step 4: host START_PRECONDITION(1) → ACK (04 01 00 02) ──────────────
    ASSERT_EQ(APEX_OK, apex_activation_host_start_precondition(&act_host, dev_id, 1));
    PumpUntilQuiet();
    EXPECT_TRUE(HasHostFrame({0x02, 0x01, 0x01}));       // COMMAND START_PRECOND idx 1
    EXPECT_TRUE(HasDevFrame({0x04, 0x01, 0x00, 0x02}));  // ACK ACCEPTED, VALIDATING
    EXPECT_EQ(APEX_ACT_ACCEPTED, ack.last.result);
    EXPECT_EQ(APEX_ACTIVATION_STATE_VALIDATING, ack.last.current_state);

    // ── Step 5: precondition 1 → Valid → READY — 03 03 01 FF 00 00 02 02 ────
    apex_activation_device_set_precondition_state(&act_dev, 1, APEX_PRECOND_VALID);
    PumpUntilQuiet();
    EXPECT_TRUE(HasDevFrame({0x03, 0x03, 0x01, 0xFF, 0x00, 0x00, 0x02, 0x02}));
    EXPECT_EQ(APEX_ACTIVATION_STATE_READY, status.last.state);

    // ── Step 6: SET_ENABLED → ENABLING → ENABLED ───────────────────────────
    ASSERT_EQ(APEX_OK, apex_activation_host_set_enabled(&act_host, dev_id));
    PumpUntilQuiet();
    EXPECT_TRUE(HasHostFrame({0x02, 0x02}));             // COMMAND SET_ENABLED (LN=02)
    EXPECT_TRUE(HasDevFrame({0x04, 0x02, 0x00, 0x04}));  // ACK ACCEPTED, ENABLING
    EXPECT_EQ(APEX_ACTIVATION_STATE_ENABLING, ack.last.current_state);
    EXPECT_TRUE(enable_began);
    EXPECT_TRUE(HasDevFrame({0x03, 0x04, 0x01, 0xFF, 0x00, 0x00, 0x02, 0x02}))
        << "STATUS on entering ENABLING";
    EXPECT_EQ(APEX_ACTIVATION_STATE_ENABLING, status.last.state);

    // Arming circuit finishes → device completes the transition to ENABLED.
    apex_activation_device_transition_complete(&act_dev);
    PumpUntilQuiet();
    EXPECT_TRUE(HasDevFrame({0x03, 0x05, 0x01, 0xFF, 0x00, 0x00, 0x02, 0x02}))
        << "STATUS on reaching ENABLED";
    EXPECT_EQ(APEX_ACTIVATION_STATE_ENABLED, status.last.state);

    // ── Step 7: TRIGGER → EXECUTING ────────────────────────────────────────
    ASSERT_EQ(APEX_OK, apex_activation_host_trigger(&act_host, dev_id));
    PumpUntilQuiet();
    EXPECT_TRUE(HasHostFrame({0x02, 0x04}));             // COMMAND TRIGGER
    EXPECT_TRUE(HasDevFrame({0x04, 0x04, 0x00, 0x07}));  // ACK ACCEPTED, EXECUTING
    EXPECT_TRUE(HasDevFrame({0x03, 0x07, 0x01, 0x00, 0x00, 0x00, 0x02, 0x02}))
        << "STATUS EXECUTING, last_trigger_source 0";
    EXPECT_TRUE(on_execute_fired);

    // ── Step 8: complete → EXHAUSTED — 03 08 00 00 00 00 02 02 ──────────────
    apex_activation_device_complete_execution(&act_dev);
    PumpUntilQuiet();
    EXPECT_TRUE(HasDevFrame({0x03, 0x08, 0x00, 0x00, 0x00, 0x00, 0x02, 0x02}));
    EXPECT_EQ(APEX_ACTIVATION_STATE_EXHAUSTED, status.last.state);
    EXPECT_EQ(0, status.last.activations_remaining);

    // §7.3: host core lifecycle → EXPENDED.
    const apex_host_device_slot_t* slot = apex_host_get_device(&host, dev_id);
    ASSERT_NE(nullptr, slot);
    EXPECT_EQ(APEX_DEV_STATUS_EXPENDED, slot->status);

    // Full transition history observed (allowing periodic frames between).
    bool v = false, r = false, en = false, ed = false, ex = false, exh = false;
    for (auto s : status.state_history) {
        if (s == APEX_ACTIVATION_STATE_VALIDATING) v = true;
        if (s == APEX_ACTIVATION_STATE_READY)      r = true;
        if (s == APEX_ACTIVATION_STATE_ENABLING)   en = true;
        if (s == APEX_ACTIVATION_STATE_ENABLED)    ed = true;
        if (s == APEX_ACTIVATION_STATE_EXECUTING)  ex = true;
        if (s == APEX_ACTIVATION_STATE_EXHAUSTED)  exh = true;
    }
    EXPECT_TRUE(v && r && en && ed && ex && exh);
}

// §9.5 multi-activation variation — after the first cycle the device returns to
// ENABLED (0x05) with activations_remaining decremented: 03 05 02 00 00 00 02 02.
TEST_F(ActivationWalkthrough, MultiActivationReturnsToEnabled) {
    caps_.initial_activations_remaining = 3;
    ReinitDevice();
    PumpUntilQuiet();
    uint8_t dev_id = apex_device_get_id(&dev_core);

    ValidateBothPreconditions();
    ASSERT_EQ(APEX_OK, apex_activation_host_set_enabled(&act_host, dev_id));
    PumpUntilQuiet();
    apex_activation_device_transition_complete(&act_dev);  // ENABLING → ENABLED
    PumpUntilQuiet();
    ASSERT_EQ(APEX_ACTIVATION_STATE_ENABLED, status.last.state);

    ASSERT_EQ(APEX_OK, apex_activation_host_trigger(&act_host, dev_id));
    PumpUntilQuiet();
    ASSERT_EQ(APEX_ACTIVATION_STATE_EXECUTING, status.last.state);

    apex_activation_device_complete_execution(&act_dev);
    PumpUntilQuiet();
    EXPECT_TRUE(HasDevFrame({0x03, 0x05, 0x02, 0x00, 0x00, 0x00, 0x02, 0x02}))
        << "§9.5: back to ENABLED with activations_remaining 2";
    EXPECT_EQ(APEX_ACTIVATION_STATE_ENABLED, status.last.state);
    EXPECT_EQ(2, status.last.activations_remaining);
    // Not EXPENDED yet — activations remain.
    const apex_host_device_slot_t* slot = apex_host_get_device(&host, dev_id);
    ASSERT_NE(nullptr, slot);
    EXPECT_NE(APEX_DEV_STATUS_EXPENDED, slot->status);
}

// CAPABILITY host-condition tail + timing tail encode/parse round-trip. This
// device is the §9 device plus a host-condition binding for precond 1
// (PROPS_ON_FLYING, auto_trigger) and a timing tail, so its CAPABILITY is longer
// than the 22-byte §9 frame.
TEST_F(ActivationWalkthrough, CapabilityHostConditionAndTimingTails) {
    caps_.n_host_conditions = 1;
    caps_.host_conditions[0] = {1, APEX_ACT_PRECOND_PROPS_ON_FLYING, 0, 1};
    caps_.enable_time_ms = 0x0102;   // 258 ms
    caps_.disable_time_ms = 0x0304;  // 772 ms
    ReinitDevice();
    PumpUntilQuiet();

    ASSERT_GE(cap.count, 1);
    ASSERT_EQ(1, cap.last.n_host_conditions);
    EXPECT_EQ(1, cap.last.host_conditions[0].precondition_idx);
    EXPECT_EQ(APEX_ACT_PRECOND_PROPS_ON_FLYING, cap.last.host_conditions[0].host_condition);
    EXPECT_EQ(1, cap.last.host_conditions[0].auto_trigger);
    EXPECT_EQ(0x0102, cap.last.enable_time_ms);
    EXPECT_EQ(0x0304, cap.last.disable_time_ms);

    // Byte-exact: … n_gpio=00, n_gpio_trig=00, n_host_cond=01, {01 05 0000 01},
    // then timing tail 02 01 (enable LE) 04 03 (disable LE). Inner payload 33 B.
    EXPECT_TRUE(HasDevFrame({
        0x01,
        0x7D, 0x9A, 0x2C, 0x14, 0x3E, 0x6B, 0x4F, 0x08,
        0x9A, 0x51, 0xC2, 0xE0, 0x7B, 0x18, 0xD4, 0xF6,
        0x02, 0x02, 0x01, 0x02,
        0x00,                    // n_gpio_bindings
        0x00,                    // n_gpio_trigger_bindings
        0x01,                    // n_host_conditions
        0x01, 0x05, 0x00, 0x00, 0x01,  // precond 1, PROPS_ON_FLYING, param 0, auto 1
        0x02, 0x01,              // enable_time_ms = 0x0102 LE
        0x04, 0x03,              // disable_time_ms = 0x0304 LE
    }));
}

// A TRIGGER from a non-ENABLED state is rejected REJECT_WRONG_STATE.
TEST_F(ActivationWalkthrough, RejectsTriggerWhenNotEnabled) {
    PumpUntilQuiet();
    uint8_t dev_id = apex_device_get_id(&dev_core);
    ValidateBothPreconditions();
    ASSERT_EQ(APEX_ACTIVATION_STATE_READY, status.last.state);

    int ack_before = ack.count;
    ASSERT_EQ(APEX_OK, apex_activation_host_trigger(&act_host, dev_id));
    PumpUntilQuiet();
    EXPECT_EQ(ack_before + 1, ack.count);
    EXPECT_EQ(APEX_ACT_REJECT_WRONG_STATE, ack.last.result);
}

TEST_F(ActivationWalkthrough, BadPreconditionIndexRejected) {
    PumpUntilQuiet();
    uint8_t dev_id = apex_device_get_id(&dev_core);
    int ack_before = ack.count;
    ASSERT_EQ(APEX_OK, apex_activation_host_start_precondition(&act_host, dev_id, 99));
    PumpUntilQuiet();
    EXPECT_EQ(ack_before + 1, ack.count);
    EXPECT_EQ(APEX_ACT_REJECT_BAD_INDEX, ack.last.result);
}

// ─────────────────────────────────────────────────────────────────────────────
//  v1 state-machine additions
// ─────────────────────────────────────────────────────────────────────────────

// SET_DISABLED during ENABLING with !can_abort_enabling → REJECT_BUSY, and no
// trigger is honored while ENABLING.
TEST_F(ActivationWalkthrough, SetDisabledDuringEnablingRejectBusyWhenNoAbort) {
    // Default caps_: can_abort_enabling == false.
    PumpUntilQuiet();
    uint8_t dev_id = apex_device_get_id(&dev_core);
    ValidateBothPreconditions();

    ASSERT_EQ(APEX_OK, apex_activation_host_set_enabled(&act_host, dev_id));
    PumpUntilQuiet();
    ASSERT_EQ(APEX_ACTIVATION_STATE_ENABLING, status.last.state);

    // SET_DISABLED while ENABLING → REJECT_BUSY, still ENABLING.
    ASSERT_EQ(APEX_OK, apex_activation_host_set_disabled(&act_host, dev_id));
    PumpUntilQuiet();
    EXPECT_EQ(APEX_ACT_CMD_SET_DISABLED, ack.last.acked_command);
    EXPECT_EQ(APEX_ACT_REJECT_BUSY, ack.last.result);
    EXPECT_EQ(APEX_ACTIVATION_STATE_ENABLING, ack.last.current_state);

    // No trigger fires while ENABLING: host TRIGGER → REJECT_WRONG_STATE, and an
    // internal source is silently ignored.
    ASSERT_EQ(APEX_OK, apex_activation_host_trigger(&act_host, dev_id));
    PumpUntilQuiet();
    EXPECT_EQ(APEX_ACT_CMD_TRIGGER, ack.last.acked_command);
    EXPECT_EQ(APEX_ACT_REJECT_WRONG_STATE, ack.last.result);
    apex_activation_device_trigger(&act_dev, 1);  // HARDWARE_INPUT source
    EXPECT_EQ(APEX_ACTIVATION_STATE_ENABLING, apex_activation_device_state(&act_dev));
}

// SET_DISABLED during ENABLING with can_abort_enabling → ACCEPTED (→ DISABLING),
// and the safety invariant holds: no trigger fires from the accept onward.
TEST_F(ActivationWalkthrough, SetDisabledDuringEnablingAbortsWhenPermitted) {
    caps_.can_abort_enabling = true;
    ReinitDevice();
    PumpUntilQuiet();
    uint8_t dev_id = apex_device_get_id(&dev_core);
    ValidateBothPreconditions();

    ASSERT_EQ(APEX_OK, apex_activation_host_set_enabled(&act_host, dev_id));
    PumpUntilQuiet();
    ASSERT_EQ(APEX_ACTIVATION_STATE_ENABLING, status.last.state);

    disable_began = false;
    ASSERT_EQ(APEX_OK, apex_activation_host_set_disabled(&act_host, dev_id));
    PumpUntilQuiet();
    EXPECT_EQ(APEX_ACT_CMD_SET_DISABLED, ack.last.acked_command);
    EXPECT_EQ(APEX_ACT_ACCEPTED, ack.last.result);
    EXPECT_EQ(APEX_ACTIVATION_STATE_DISABLING, ack.last.current_state);
    EXPECT_TRUE(disable_began);  // aborts via the disable-begin hook path
    EXPECT_EQ(APEX_ACTIVATION_STATE_DISABLING, status.last.state);

    // Safety invariant: no trigger from the accept onward (still DISABLING).
    apex_activation_device_trigger(&act_dev, 1);
    EXPECT_EQ(APEX_ACTIVATION_STATE_DISABLING, apex_activation_device_state(&act_dev));
    ASSERT_EQ(APEX_OK, apex_activation_host_trigger(&act_host, dev_id));
    PumpUntilQuiet();
    EXPECT_EQ(APEX_ACT_REJECT_WRONG_STATE, ack.last.result);
}

// Instant device: no on_enable_begin hook ⇒ SET_ENABLED goes READY → ENABLED
// directly (ACK current_state = ENABLED), never reporting ENABLING.
TEST_F(ActivationWalkthrough, InstantDeviceEnablesDirectly) {
    dh_.on_enable_begin = nullptr;
    dh_.on_disable_begin = nullptr;
    ReinitDevice();
    PumpUntilQuiet();
    uint8_t dev_id = apex_device_get_id(&dev_core);
    ValidateBothPreconditions();
    ASSERT_EQ(APEX_ACTIVATION_STATE_READY, status.last.state);

    ASSERT_EQ(APEX_OK, apex_activation_host_set_enabled(&act_host, dev_id));
    PumpUntilQuiet();
    EXPECT_EQ(APEX_ACT_CMD_SET_ENABLED, ack.last.acked_command);
    EXPECT_EQ(APEX_ACT_ACCEPTED, ack.last.result);
    EXPECT_EQ(APEX_ACTIVATION_STATE_ENABLED, ack.last.current_state)
        << "instant device ACKs ENABLED directly";
    EXPECT_TRUE(HasDevFrame({0x04, 0x02, 0x00, 0x05}));  // ACK current_state=ENABLED
    EXPECT_FALSE(enable_began);
    for (auto s : status.state_history) {
        EXPECT_NE(APEX_ACTIVATION_STATE_ENABLING, s)
            << "instant device must never report ENABLING";
    }
    EXPECT_EQ(APEX_ACTIVATION_STATE_ENABLED, status.last.state);

    // Instant disable: ENABLED → READY directly.
    ASSERT_EQ(APEX_OK, apex_activation_host_set_disabled(&act_host, dev_id));
    PumpUntilQuiet();
    EXPECT_EQ(APEX_ACT_ACCEPTED, ack.last.result);
    EXPECT_EQ(APEX_ACTIVATION_STATE_READY, ack.last.current_state);
}

// TRANSITION_FAILED (bit 5): a safe transition failure returns to READY and sets
// the flag; the next successful transition clears it.
TEST_F(ActivationWalkthrough, TransitionFailedSetsBit5ThenClears) {
    PumpUntilQuiet();
    uint8_t dev_id = apex_device_get_id(&dev_core);
    ValidateBothPreconditions();

    ASSERT_EQ(APEX_OK, apex_activation_host_set_enabled(&act_host, dev_id));
    PumpUntilQuiet();
    ASSERT_EQ(APEX_ACTIVATION_STATE_ENABLING, status.last.state);

    // Enable fails safely → READY + TRANSITION_FAILED, NOT FAULT.
    apex_activation_device_transition_failed(&act_dev);
    PumpUntilQuiet();
    EXPECT_EQ(APEX_ACTIVATION_STATE_READY, status.last.state);
    EXPECT_TRUE(status.last.fault_flags & APEX_ACT_FAULT_TRANSITION_FAILED);
    EXPECT_EQ(0, (status.last.fault_flags & APEX_ACT_FAULT_LATCHED_MASK));

    // Retry; a successful enable clears TRANSITION_FAILED.
    ASSERT_EQ(APEX_OK, apex_activation_host_set_enabled(&act_host, dev_id));
    PumpUntilQuiet();
    ASSERT_EQ(APEX_ACTIVATION_STATE_ENABLING, status.last.state);
    apex_activation_device_transition_complete(&act_dev);
    PumpUntilQuiet();
    EXPECT_EQ(APEX_ACTIVATION_STATE_ENABLED, status.last.state);
    EXPECT_EQ(0, (status.last.fault_flags & APEX_ACT_FAULT_TRANSITION_FAILED));
}

// SET_DISABLED in READY is an idempotent no-op ACCEPTED (retransmit safety).
TEST_F(ActivationWalkthrough, SetDisabledInReadyIsAcceptedNoop) {
    PumpUntilQuiet();
    uint8_t dev_id = apex_device_get_id(&dev_core);
    ValidateBothPreconditions();
    ASSERT_EQ(APEX_ACTIVATION_STATE_READY, status.last.state);

    ASSERT_EQ(APEX_OK, apex_activation_host_set_disabled(&act_host, dev_id));
    PumpUntilQuiet();
    EXPECT_EQ(APEX_ACT_CMD_SET_DISABLED, ack.last.acked_command);
    EXPECT_EQ(APEX_ACT_ACCEPTED, ack.last.result);
    EXPECT_EQ(APEX_ACTIVATION_STATE_READY, ack.last.current_state);
    EXPECT_EQ(APEX_ACTIVATION_STATE_READY, apex_activation_device_state(&act_dev));
}

// DISPLAY_TEXT §6.6 rate cap: a second push to the same target inside 200 ms is
// rejected; after the interval it is accepted again. A different target in the
// same window is independent.
TEST_F(ActivationWalkthrough, DisplayTextRateCapPerTarget) {
    PumpUntilQuiet();  // now_ms advances during connect

    EXPECT_EQ(APEX_OK, apex_activation_device_push_text(&act_dev, 0x00, "A"));
    // Same target, no time advance → over-rate → rejected.
    EXPECT_EQ(APEX_ERR_BAD_STATE,
              apex_activation_device_push_text(&act_dev, 0x00, "B"));
    // Different target in the same window → independent → accepted.
    EXPECT_EQ(APEX_OK, apex_activation_device_push_text(&act_dev, 0xF0, "banner"));

    // Advance past the 5 Hz window → target 0 accepted again.
    Pump(200);
    EXPECT_EQ(APEX_OK, apex_activation_device_push_text(&act_dev, 0x00, "C"));

    // A banner beyond the host's declared n_banner_lines (1) is unsupported.
    EXPECT_EQ(APEX_ERR_UNSUPPORTED,
              apex_activation_device_push_text(&act_dev, 0xF1, "x"));
    // A reserved target byte is invalid.
    EXPECT_EQ(APEX_ERR_INVALID_ARGS,
              apex_activation_device_push_text(&act_dev, 0x10, "x"));
}

// ─────────────────────────────────────────────────────────────────────────────
//  §7.4 re-enumeration (RESET_REQUEST)
// ─────────────────────────────────────────────────────────────────────────────

// (a) RESET_REQUEST during EXECUTING is DEFERRED: the device stays in session
// and keeps reporting EXECUTING until the action completes, then honors the reset
// at the completion transition. Because the completion lands in ENABLED, honoring
// the reset (leaving CONNECTED) self-issues SET_DISABLE (§7.4): the device stands
// itself down (ENABLED → DISABLING) rather than carrying the armed state into the
// new session. The survivors — latched preconditions and the decremented
// activations_remaining — persist; display caps revert to defaults.
TEST_F(ActivationWalkthrough, ResetRequestDeferredDuringExecuting) {
    cap.send_display_info = false;  // manual display info so reverts are visible
    caps_.initial_activations_remaining = 2;  // survives with 1 remaining
    ReinitDevice();
    PumpUntilQuiet();
    uint8_t dev_id = apex_device_get_id(&dev_core);
    ASSERT_EQ(0x02, dev_id);

    // Establish non-default display caps for this session.
    ASSERT_EQ(APEX_OK,
              apex_activation_host_send_display_info(&act_host, dev_id, 20, 1));
    PumpUntilQuiet();
    ASSERT_EQ(20, act_dev.host_char_limit);

    ValidateBothPreconditions();
    ASSERT_EQ(APEX_OK, apex_activation_host_set_enabled(&act_host, dev_id));
    PumpUntilQuiet();
    apex_activation_device_transition_complete(&act_dev);
    PumpUntilQuiet();
    ASSERT_EQ(APEX_OK, apex_activation_host_trigger(&act_host, dev_id));
    PumpUntilQuiet();
    ASSERT_EQ(APEX_ACTIVATION_STATE_EXECUTING, apex_activation_device_state(&act_dev));

    // Host commands re-enumeration mid-action (frees its slot immediately).
    auto count_executing_status = [&]() {
        int n = 0;
        for (auto& f : dwire) {
            if (f.payload.size() >= 2 && f.payload[0] == 0x03 && f.payload[1] == 0x07) n++;
        }
        return n;
    };
    int exec_status_before = count_executing_status();
    ASSERT_EQ(APEX_OK, apex_host_request_reenumeration(&host, dev_id));

    // ~2 s of deferral: the device stays CONNECTED, remains EXECUTING, and keeps
    // emitting periodic EXECUTING STATUS frames (repeated reactive resets from
    // the host just re-latch the deferral).
    for (int i = 0; i < 20; i++) Pump(100);
    EXPECT_EQ(APEX_DEVICE_STATE_CONNECTED, apex_device_link_state(&dev_core));
    EXPECT_EQ(APEX_ACTIVATION_STATE_EXECUTING, apex_activation_device_state(&act_dev));
    EXPECT_GT(count_executing_status(), exec_status_before)
        << "STATUS EXECUTING must continue during the deferral";

    // Action completes → EXECUTING leaves; the deferred reset is honored on the
    // next tick.
    int cap_before = cap.count;
    apex_activation_device_complete_execution(&act_dev);
    PumpUntilQuiet(32);

    // Re-discovered: new session, CAPABILITY re-emitted.
    ASSERT_EQ(APEX_DEVICE_STATE_CONNECTED, apex_device_link_state(&dev_core));
    uint8_t new_id = apex_device_get_id(&dev_core);
    ASSERT_NE(APEX_DEVICE_ID_UNASSIGNED, new_id);
    EXPECT_EQ(cap_before + 1, cap.count) << "CAPABILITY re-emits after re-discovery";

    // The new session's first STATUS reports the ACTUAL (self-disarming) state,
    // never assumed: honoring the reset from ENABLED self-issued SET_DISABLE, so
    // the non-instant device is DISABLING — not ENABLED. Survivors persist: one
    // activation consumed (1 remaining) and both preconditions still latched Valid.
    EXPECT_EQ(APEX_ACTIVATION_STATE_DISABLING, status.last.state);
    EXPECT_NE(APEX_ACTIVATION_STATE_ENABLED, status.last.state)
        << "the armed state must not survive re-enumeration (§7.4)";
    EXPECT_EQ(1, status.last.activations_remaining);
    EXPECT_EQ(APEX_PRECOND_VALID, status.last.precondition_states[0]);
    EXPECT_EQ(APEX_PRECOND_VALID, status.last.precondition_states[1]);

    // The physical stand-down completes → READY, reported in the next STATUS.
    apex_activation_device_transition_complete(&act_dev);
    PumpUntilQuiet();
    EXPECT_EQ(APEX_ACTIVATION_STATE_READY, apex_activation_device_state(&act_dev));
    EXPECT_EQ(APEX_ACTIVATION_STATE_READY, status.last.state);
    EXPECT_EQ(1, status.last.activations_remaining);

    // Host-session artifacts reset: display caps back to defaults (32 / 1).
    EXPECT_EQ(32, act_dev.host_char_limit);
    EXPECT_EQ(1, act_dev.host_n_banner_lines);
}

// (b) Same deferral, but the final action EXHAUSTS the device: after the
// honored reset and re-discovery the first STATUS reports EXHAUSTED and the
// host re-marks the (new) slot EXPENDED (§7.4 / §7.3).
TEST_F(ActivationWalkthrough, ResetDuringExecutingExhaustedSurvives) {
    PumpUntilQuiet();
    uint8_t dev_id = apex_device_get_id(&dev_core);
    ValidateBothPreconditions();
    ASSERT_EQ(APEX_OK, apex_activation_host_set_enabled(&act_host, dev_id));
    PumpUntilQuiet();
    apex_activation_device_transition_complete(&act_dev);
    PumpUntilQuiet();
    ASSERT_EQ(APEX_OK, apex_activation_host_trigger(&act_host, dev_id));
    PumpUntilQuiet();
    ASSERT_EQ(APEX_ACTIVATION_STATE_EXECUTING, apex_activation_device_state(&act_dev));

    ASSERT_EQ(APEX_OK, apex_host_request_reenumeration(&host, dev_id));
    PumpUntilQuiet();
    EXPECT_EQ(APEX_DEVICE_STATE_CONNECTED, apex_device_link_state(&dev_core));

    // Final activation completes → EXHAUSTED; reset honored; re-discovery.
    apex_activation_device_complete_execution(&act_dev);
    PumpUntilQuiet(32);
    ASSERT_EQ(APEX_DEVICE_STATE_CONNECTED, apex_device_link_state(&dev_core));
    uint8_t new_id = apex_device_get_id(&dev_core);

    EXPECT_EQ(APEX_ACTIVATION_STATE_EXHAUSTED, apex_activation_device_state(&act_dev));
    EXPECT_EQ(APEX_ACTIVATION_STATE_EXHAUSTED, status.last.state);
    EXPECT_EQ(0, status.last.activations_remaining);
    EXPECT_TRUE(HasDevFrame({0x03, 0x08, 0x00, 0x00, 0x00, 0x00, 0x02, 0x02}, new_id));

    const apex_host_device_slot_t* slot = apex_host_get_device(&host, new_id);
    ASSERT_NE(nullptr, slot);
    EXPECT_EQ(APEX_DEV_STATUS_EXPENDED, slot->status)
        << "host re-marks EXPENDED from the re-reported EXHAUSTED STATUS";
}

// (c) RESET_REQUEST in a non-EXECUTING state (ENABLED) is honored immediately,
// and the session loss self-issues SET_DISABLE (§7.4): the armed state does NOT
// survive into the new session — the device stands itself down (ENABLED →
// DISABLING → READY), and neither side assumes the post-command state. The
// survivors (activations_remaining, latched preconditions) persist.
TEST_F(ActivationWalkthrough, ReenumInEnabledSelfDisarms) {
    caps_.initial_activations_remaining = 2;
    ReinitDevice();
    PumpUntilQuiet();
    uint8_t dev_id = apex_device_get_id(&dev_core);
    ValidateBothPreconditions();
    ASSERT_EQ(APEX_OK, apex_activation_host_set_enabled(&act_host, dev_id));
    PumpUntilQuiet();
    apex_activation_device_transition_complete(&act_dev);
    PumpUntilQuiet();
    ASSERT_EQ(APEX_ACTIVATION_STATE_ENABLED, apex_activation_device_state(&act_dev));

    int cap_before = cap.count;
    disable_began = false;
    ASSERT_EQ(APEX_OK, apex_host_request_reenumeration(&host, dev_id));
    // Deliver the RESET_REQUEST bytes directly: the core honors it at RX time.
    ASSERT_FALSE(bus.h2d.empty());
    {
        auto b = std::move(bus.h2d);
        bus.h2d.clear();
        decode_stream(b, hwire);
        apex_device_feed_rx(&dev_core, b.data(), b.size(), now_ms);
    }
    EXPECT_EQ(APEX_DEVICE_STATE_DISCOVERING, apex_device_link_state(&dev_core))
        << "non-EXECUTING device honors RESET_REQUEST immediately";

    // The class self-disarms when it observes the session loss on its next tick:
    // a non-instant device goes ENABLED → DISABLING (on_disable_begin fires).
    Pump(1);
    EXPECT_TRUE(disable_began) << "a lost session self-issues SET_DISABLE (§7.4)";
    EXPECT_EQ(APEX_ACTIVATION_STATE_DISABLING, apex_activation_device_state(&act_dev));
    EXPECT_NE(APEX_ACTIVATION_STATE_ENABLED, apex_activation_device_state(&act_dev))
        << "the armed state must not survive re-enumeration";

    // The physical stand-down completes during discovery → READY.
    apex_activation_device_transition_complete(&act_dev);
    EXPECT_EQ(APEX_ACTIVATION_STATE_READY, apex_activation_device_state(&act_dev));

    PumpUntilQuiet(32);
    ASSERT_EQ(APEX_DEVICE_STATE_CONNECTED, apex_device_link_state(&dev_core));
    EXPECT_EQ(cap_before + 1, cap.count);
    // The first STATUS after re-discovery reports the ACTUAL (disarmed) state —
    // never ENABLED; the host reads it rather than assuming.
    EXPECT_EQ(APEX_ACTIVATION_STATE_READY, status.last.state);
    // Survivors: the activation budget and latched preconditions persist.
    EXPECT_EQ(2, status.last.activations_remaining);
    EXPECT_EQ(APEX_PRECOND_VALID, status.last.precondition_states[0]);
    EXPECT_EQ(APEX_PRECOND_VALID, status.last.precondition_states[1]);
}

// (d) An instant device (no on_disable_begin hook) reaches READY synchronously
// when the session loss self-issues SET_DISABLE — there is no DISABLING dwell.
TEST_F(ActivationWalkthrough, ReenumInEnabledInstantDeviceGoesReady) {
    dh_.on_enable_begin = nullptr;   // instant enable and disable
    dh_.on_disable_begin = nullptr;
    caps_.initial_activations_remaining = 2;
    ReinitDevice();
    PumpUntilQuiet();
    uint8_t dev_id = apex_device_get_id(&dev_core);
    ValidateBothPreconditions();
    ASSERT_EQ(APEX_OK, apex_activation_host_set_enabled(&act_host, dev_id));
    PumpUntilQuiet();
    ASSERT_EQ(APEX_ACTIVATION_STATE_ENABLED, apex_activation_device_state(&act_dev));

    ASSERT_EQ(APEX_OK, apex_host_request_reenumeration(&host, dev_id));
    PumpUntilQuiet(32);
    ASSERT_EQ(APEX_DEVICE_STATE_CONNECTED, apex_device_link_state(&dev_core));
    // Instant stand-down: ENABLED → READY directly, no DISABLING transient to
    // complete. The first STATUS after re-discovery reports READY.
    EXPECT_EQ(APEX_ACTIVATION_STATE_READY, apex_activation_device_state(&act_dev));
    EXPECT_EQ(APEX_ACTIVATION_STATE_READY, status.last.state);
    EXPECT_EQ(2, status.last.activations_remaining);
}

// (e) Watchdog during EXECUTING: the session is lost mid-action, and the
// watchdog — unlike RESET_REQUEST — is NOT deferrable. EXECUTING still runs to
// completion, but because a session was lost while armed the device OWES a
// self-disarm (self_disarm_pending); it applies it when the action completes
// into ENABLED rather than re-arming into the gap. This closes the case that a
// bare "disarm on leaving CONNECTED" would miss (self_disable is a no-op in
// EXECUTING).
TEST_F(ActivationWalkthrough, WatchdogDuringExecutingSelfDisarmsOnCompletion) {
    caps_.initial_activations_remaining = 2;
    ReinitDevice();
    PumpUntilQuiet();
    uint8_t dev_id = apex_device_get_id(&dev_core);
    ValidateBothPreconditions();
    ASSERT_EQ(APEX_OK, apex_activation_host_set_enabled(&act_host, dev_id));
    PumpUntilQuiet();
    apex_activation_device_transition_complete(&act_dev);
    PumpUntilQuiet();
    ASSERT_EQ(APEX_OK, apex_activation_host_trigger(&act_host, dev_id));
    PumpUntilQuiet();
    ASSERT_EQ(APEX_ACTIVATION_STATE_EXECUTING, apex_activation_device_state(&act_dev));

    // Host falls silent → the device's heartbeat watchdog trips while EXECUTING.
    disable_began = false;
    for (int i = 0; i < 7; i++) {
        now_ms += 1000;
        apex_device_tick(&dev_core, now_ms);
        apex_activation_device_tick(&act_dev, now_ms);
        bus.h2d.clear();   // host silent
        bus.d2h.clear();
    }
    ASSERT_EQ(APEX_DEVICE_STATE_DISCOVERING, apex_device_link_state(&dev_core));
    EXPECT_EQ(APEX_ACTIVATION_STATE_EXECUTING, apex_activation_device_state(&act_dev))
        << "EXECUTING runs to completion; the disarm is owed, not yet applied";
    EXPECT_FALSE(disable_began);

    // The action completes → ENABLED, but the owed self-disarm fires: the device
    // stands down (→ DISABLING) rather than re-arm. One activation consumed.
    apex_activation_device_complete_execution(&act_dev);
    EXPECT_TRUE(disable_began) << "owed self-disarm fires on completion (§7.4)";
    EXPECT_EQ(APEX_ACTIVATION_STATE_DISABLING, apex_activation_device_state(&act_dev));
    EXPECT_NE(APEX_ACTIVATION_STATE_ENABLED, apex_activation_device_state(&act_dev));
    EXPECT_EQ(1, act_dev.activations_remaining);
}

// (f) Session loss while ENABLING on a device that CANNOT abort the enable: the
// transition can't be stood down mid-flight, so the disarm is owed and applied
// when ENABLING completes into ENABLED — the device does not settle armed.
TEST_F(ActivationWalkthrough, ReenumWhileEnablingCannotAbortDisarmsOnComplete) {
    // Default caps_: can_abort_enabling == false; non-instant enable.
    caps_.initial_activations_remaining = 2;
    ReinitDevice();
    PumpUntilQuiet();
    uint8_t dev_id = apex_device_get_id(&dev_core);
    ValidateBothPreconditions();
    ASSERT_EQ(APEX_OK, apex_activation_host_set_enabled(&act_host, dev_id));
    PumpUntilQuiet();
    ASSERT_EQ(APEX_ACTIVATION_STATE_ENABLING, apex_activation_device_state(&act_dev));

    // Re-enumerate mid-ENABLING (not deferred — only EXECUTING defers). Deliver
    // the reset bytes directly; the class observes the loss on its next tick.
    disable_began = false;
    ASSERT_EQ(APEX_OK, apex_host_request_reenumeration(&host, dev_id));
    ASSERT_FALSE(bus.h2d.empty());
    {
        auto b = std::move(bus.h2d);
        bus.h2d.clear();
        decode_stream(b, hwire);
        apex_device_feed_rx(&dev_core, b.data(), b.size(), now_ms);
    }
    Pump(1);
    // Can't abort → still ENABLING, but a self-disarm is now owed.
    EXPECT_EQ(APEX_ACTIVATION_STATE_ENABLING, apex_activation_device_state(&act_dev));
    EXPECT_FALSE(disable_began);

    // ENABLING completes → ENABLED, and the owed disarm fires → DISABLING.
    apex_activation_device_transition_complete(&act_dev);
    EXPECT_TRUE(disable_began) << "owed self-disarm fires when ENABLING completes";
    EXPECT_EQ(APEX_ACTIVATION_STATE_DISABLING, apex_activation_device_state(&act_dev));
    EXPECT_NE(APEX_ACTIVATION_STATE_ENABLED, apex_activation_device_state(&act_dev));
}

// An app-installed reenum_permitted hook composes conservatively with the class
// hook: BOTH must permit. With the class in a permitting state (READY), an app
// hook returning false still defers the reset until it flips to true.
static bool app_reenum_hook(void* u) { return *static_cast<bool*>(u); }

TEST_F(ActivationWalkthrough, AppReenumHookComposesBothMustPermit) {
    bool app_permit = false;
    dev_core.cfg.reenum_permitted = app_reenum_hook;
    dev_core.cfg.reenum_permitted_user = &app_permit;
    ReinitDevice();  // recomposes: class hook wraps the app hook
    PumpUntilQuiet();
    uint8_t dev_id = apex_device_get_id(&dev_core);
    ValidateBothPreconditions();
    ASSERT_EQ(APEX_ACTIVATION_STATE_READY, apex_activation_device_state(&act_dev));

    ASSERT_EQ(APEX_OK, apex_host_request_reenumeration(&host, dev_id));
    for (int i = 0; i < 10; i++) Pump(100);
    EXPECT_EQ(APEX_DEVICE_STATE_CONNECTED, apex_device_link_state(&dev_core))
        << "app hook forbids: reset deferred even though the class permits";

    app_permit = true;
    // One core tick re-checks the latched reset and honors it (asserted before
    // any Pump round, which would already complete re-discovery).
    apex_device_tick(&dev_core, ++now_ms);
    EXPECT_EQ(APEX_DEVICE_STATE_DISCOVERING, apex_device_link_state(&dev_core))
        << "both permit: deferred reset honored";
    PumpUntilQuiet(32);
    EXPECT_EQ(APEX_DEVICE_STATE_CONNECTED, apex_device_link_state(&dev_core));
    EXPECT_EQ(APEX_ACTIVATION_STATE_READY, status.last.state);
}

}  // namespace
