/**
 * @file apex_activation.h
 * @brief Activation device class (traffic_type = 2). Both Host and Device
 *        sides. See APEX_Device_Class_Activation.md for the spec this
 *        implements — Activation **class version 1** (negotiated at discovery,
 *        §1.1; class frames carry no version byte of their own).
 *
 * Copyright (c) 2026 Neros Technologies. MIT License — see LICENSE.
 */
#ifndef APEX_ACTIVATION_H
#define APEX_ACTIVATION_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "apex/apex_core.h"
#include "apex/apex_device.h"
#include "apex/apex_host.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Compile-time tuning. */
#ifndef APEX_ACTIVATION_MAX_PRECONDITIONS
#define APEX_ACTIVATION_MAX_PRECONDITIONS 16
#endif
#ifndef APEX_ACTIVATION_MAX_TRIGGER_SOURCES
#define APEX_ACTIVATION_MAX_TRIGGER_SOURCES 16
#endif
#ifndef APEX_ACTIVATION_PAYLOAD_SPECIFIC_MAX
#define APEX_ACTIVATION_PAYLOAD_SPECIFIC_MAX 32
#endif

/* ---------------------------------------------------------------------------
 * Spec enums — §3, §4.2, §5, §6.4, §6.5
 * ------------------------------------------------------------------------- */

/* §3 state machine (class version 1). The values are renumbered from class
 * version 0 to insert the two first-class transient states ENABLING (0x04) and
 * DISABLING (0x06); progression stays monotonic so "armed or beyond" is
 * state >= ENABLED && state != FAULT. */
typedef enum {
    APEX_ACTIVATION_STATE_RESERVED   = 0x00,
    APEX_ACTIVATION_STATE_STANDBY    = 0x01,
    APEX_ACTIVATION_STATE_VALIDATING = 0x02,
    APEX_ACTIVATION_STATE_READY      = 0x03,
    APEX_ACTIVATION_STATE_ENABLING   = 0x04,  /* new in v1: enable in progress */
    APEX_ACTIVATION_STATE_ENABLED    = 0x05,
    APEX_ACTIVATION_STATE_DISABLING  = 0x06,  /* new in v1: disable in progress */
    APEX_ACTIVATION_STATE_EXECUTING  = 0x07,
    APEX_ACTIVATION_STATE_EXHAUSTED  = 0x08,
    APEX_ACTIVATION_STATE_FAULT      = 0xFF,
} apex_activation_state_t;

typedef enum {
    APEX_PRECOND_NOT_STARTED = 0,
    APEX_PRECOND_RUNNING     = 1,
    APEX_PRECOND_VALID       = 2,
    APEX_PRECOND_FAILED      = 3,
} apex_precond_state_t;

typedef enum {
    APEX_TRIGGER_HOST_COMMAND    = 0x01,
    APEX_TRIGGER_HARDWARE_INPUT  = 0x02,
    APEX_TRIGGER_TIMER           = 0x03,
    APEX_TRIGGER_EXTERNAL_SIGNAL = 0x04,
} apex_trigger_category_t;

/* GPIO pins usable as precondition / trigger lines. The core 10-pin connector
 * exposes exactly two discrete-IO lines (Core §2: Pin 3 / Pin 4, the
 * APEX_INTERFACE_FLAG_GPIO pins), so a GPIO-backed binding is always one of
 * these two. The value is the connector pin number, passed verbatim to the
 * application's gpio_read hook. */
typedef enum {
    APEX_ACTIVATION_GPIO_PIN3 = 3,
    APEX_ACTIVATION_GPIO_PIN4 = 4,
} apex_activation_gpio_pin_t;

/* On the wire (§6.3) each GPIO mapping packs the pin and its active level into a
 * single byte: the low 7 bits are the connector pin (3 or 4), bit 7 is the
 * active level — set = active-high, clear = active-low. */
#define APEX_ACTIVATION_GPIO_PIN_MASK        0x7Fu
#define APEX_ACTIVATION_GPIO_ACTIVE_HIGH_BIT 0x80u

/* §6.1 inner-payload sub-header message ids. IDs 5 and 6 were PRECOND_INFO_
 * REQUEST / PRECOND_INFO_REPLY in class version 0; they are RETIRED in v1 and
 * are **never reused** — dual-stack code can therefore never confuse a retired
 * id with a new message. */
typedef enum {
    APEX_ACT_MSG_CAPABILITY        = 1,
    APEX_ACT_MSG_COMMAND           = 2,
    APEX_ACT_MSG_STATUS            = 3,
    APEX_ACT_MSG_ACK               = 4,
    /* 5, 6 retired (class v0 PRECOND_INFO); never reused. */
    APEX_ACT_MSG_HOST_DISPLAY_INFO = 7,  /* host → device (§6.7) */
    APEX_ACT_MSG_DISPLAY_TEXT      = 8,  /* device → host (§6.8) */
} apex_activation_msg_id_t;

typedef enum {
    APEX_ACT_CMD_START_PRECONDITION = 1,
    APEX_ACT_CMD_SET_ENABLED        = 2,
    APEX_ACT_CMD_SET_DISABLED       = 3,
    APEX_ACT_CMD_TRIGGER            = 4,
} apex_activation_command_t;

typedef enum {
    APEX_ACT_ACCEPTED             = 0x00,
    APEX_ACT_REJECT_WRONG_STATE   = 0x01,
    APEX_ACT_REJECT_BAD_INDEX     = 0x02,
    APEX_ACT_REJECT_PRECONDITION  = 0x03,
    APEX_ACT_REJECT_NOT_SUPPORTED = 0x04,
    APEX_ACT_REJECT_MALFORMED     = 0x05,
    /* new in v1 (§6.4): valid, understood command that the device temporarily
     * cannot comply with — an irreversible transition is in progress (e.g. a
     * mid-transition reversal it cannot honor right now). Not a fault; the host
     * retries after the next state-transition STATUS. */
    APEX_ACT_REJECT_BUSY          = 0x06,
} apex_activation_ack_result_t;

typedef enum {
    APEX_ACT_PRECOND_NONE              = 0x00,  /* not evaluable by host */
    APEX_ACT_PRECOND_HOVER             = 0x01,  /* low velocity + low altitude rate */
    APEX_ACT_PRECOND_ALT_ABOVE         = 0x02,  /* altitude > condition_param metres */
    APEX_ACT_PRECOND_ALT_BELOW         = 0x03,  /* altitude < condition_param metres */
    APEX_ACT_PRECOND_GPS_FIX           = 0x04,  /* 3D GPS fix acquired */
    APEX_ACT_PRECOND_PROPS_ON_FLYING   = 0x05,  /* HOST_STATE == PROPS_ON_FLYING */
    APEX_ACT_PRECOND_PROPS_ON_GROUND   = 0x06,  /* props at idle on ground */
    APEX_ACT_PRECOND_PROPS_WITH_THROTTLE = 0x07, /* props + throttle >= threshold */
    APEX_ACT_PRECOND_PROPS_ON_FLYING_TIMER = 0x08, /* condition_param s after PROPS_ON_FLYING */
    APEX_ACT_PRECOND_CUSTOM            = 0xFF,  /* host cannot evaluate; manual only */
} apex_activation_host_condition_t;

#define APEX_ACT_FAULT_PRECONDITION_FAILED    (1u << 0)
#define APEX_ACT_FAULT_SEQUENCE_VIOLATION     (1u << 1)
#define APEX_ACT_FAULT_TRIGGER_WINDOW_EXPIRED (1u << 2)
#define APEX_ACT_FAULT_COMMS_LOST             (1u << 3)
#define APEX_ACT_FAULT_INTERNAL_ERROR         (1u << 4)
/* new in v1 (§6.5 bit 5): an enable/disable transition failed but the device
 * returned safely to READY. Self-clearing — cleared on the next successful
 * transition; does NOT by itself force FAULT. Reserved is now bits 6..15. */
#define APEX_ACT_FAULT_TRANSITION_FAILED      (1u << 5)

/* Latched bits force the device into FAULT; self-clearing bits do not. */
#define APEX_ACT_FAULT_LATCHED_MASK                                       \
    (APEX_ACT_FAULT_PRECONDITION_FAILED | APEX_ACT_FAULT_SEQUENCE_VIOLATION | \
     APEX_ACT_FAULT_INTERNAL_ERROR)

/* "Unlimited" sentinel for activations_remaining (§6.5). */
#define APEX_ACT_ACTIVATIONS_UNLIMITED 0xFFu

/* Periodic STATUS floor — §6.6. */
#define APEX_ACTIVATION_STATUS_PERIOD_MS 1000u

/* ---------------------------------------------------------------------------
 * DISPLAY_TEXT / HOST_DISPLAY_INFO — §6.7, §6.8
 * ------------------------------------------------------------------------- */

/* HOST_DISPLAY_INFO defaults. When the device has received NO HOST_DISPLAY_INFO
 * it assumes char_limit = 32 and n_banner_lines = 1 (§6.7). Note the asymmetry:
 * a received char_limit of 0 means 32, but a received n_banner_lines of 0 means
 * "banner text unsupported" — the default of 1 applies only when the frame was
 * never received at all. */
#define APEX_ACT_DISPLAY_CHAR_LIMIT_DEFAULT 32u
#define APEX_ACT_DISPLAY_BANNER_LINES_DEFAULT 1u

/* DISPLAY_TEXT target ranges (§6.8). */
#define APEX_ACT_DISPLAY_TARGET_PRECOND_MIN 0x00u
#define APEX_ACT_DISPLAY_TARGET_PRECOND_MAX 0x0Fu
#define APEX_ACT_DISPLAY_TARGET_BANNER_MIN  0xF0u
#define APEX_ACT_DISPLAY_TARGET_BANNER_MAX  0xFEu

/* Number of distinct DISPLAY_TEXT targets we rate-limit independently: the 16
 * precondition lines (0x00..0x0F) plus the 15 banner lines (0xF0..0xFE). */
#define APEX_ACT_DISPLAY_N_PRECOND_TARGETS 16u
#define APEX_ACT_DISPLAY_N_BANNER_TARGETS  15u
#define APEX_ACT_DISPLAY_N_TARGETS \
    (APEX_ACT_DISPLAY_N_PRECOND_TARGETS + APEX_ACT_DISPLAY_N_BANNER_TARGETS)

/* §6.6 DISPLAY_TEXT rate cap: <= 5 Hz sustained per target => >= 200 ms between
 * accepted pushes to the same target. This library REJECTS an over-rate push
 * (apex_activation_device_push_text returns APEX_ERR_BAD_STATE) rather than
 * queueing it — latest-wins semantics (§6.8) make dropping an intermediate
 * update safe, and rejecting keeps the device from ever displacing STATUS/ACK
 * timing with a DISPLAY_TEXT backlog. */
#define APEX_ACT_DISPLAY_MIN_INTERVAL_MS 200u

/* Largest text length we will place on the wire in one DISPLAY_TEXT frame. The
 * inner payload is class_msg_id + target + text_len + text, and payload_length
 * is a u8 (Core §3.1.1), so text is capped at 252 bytes regardless of a large
 * host char_limit. */
#define APEX_ACT_DISPLAY_TEXT_MAX 252u

/* ---------------------------------------------------------------------------
 * Shared: host-condition binding (§6.3)
 * ------------------------------------------------------------------------- */

/* Host-condition binding declared in CAPABILITY (§6.3): for a host-started
 * precondition the host evaluates against a flight condition, the gating
 * condition and whether the host should send START_PRECONDITION autonomously.
 * Used on BOTH sides — the device declares them in its caps, the host receives
 * them in the CAPABILITY callback. Carried in CAPABILITY (never on the advisory
 * DISPLAY_TEXT channel) so the host's auto-start logic depends only on the
 * reliably-delivered CAPABILITY + STATUS frames (§6.3). */
typedef struct {
    uint8_t  precondition_idx;
    uint8_t  host_condition;   /* apex_activation_host_condition_t */
    uint16_t condition_param;
    uint8_t  auto_trigger;     /* 1 = host fires START_PRECONDITION autonomously */
} apex_activation_host_condition_binding_t;

/* ---------------------------------------------------------------------------
 * Device-side capability declaration
 * ------------------------------------------------------------------------- */

/* Optional binding of a precondition to a GPIO line. When a precondition is
 * GPIO-backed, the class drives its Running → Valid transition by polling the
 * line via the gpio_read hook instead of the application calling
 * apex_activation_device_set_precondition_state(). The binding does not change
 * *when* validation starts: the precondition still has to be Running (auto-start
 * via auto_start_mask, or host-start via START_PRECONDITION) before the line is
 * polled. */
typedef struct {
    uint8_t precondition_idx;       /* declared precondition this gates (< n_preconditions) */
    uint8_t pin;                    /* apex_activation_gpio_pin_t (Pin 3 / Pin 4) */
    bool active_high;               /* true: active level is logic high */
    /* The line must read its active level continuously for at least this many
     * milliseconds before the precondition latches to Valid. 0 = latch on the
     * first active read (no debounce). */
    uint32_t stable_ms;
} apex_activation_gpio_binding_t;

/* Binding of a HARDWARE_INPUT trigger source to a discrete GPIO line. The device
 * fires the trigger when the line reaches its active level. Debounce and
 * edge-detection behaviour are device-internal. */
typedef struct {
    uint8_t trigger_source_idx;  /* declared trigger source this activates (< n_trigger_sources) */
    uint8_t pin;                 /* apex_activation_gpio_pin_t (Pin 3 / Pin 4) */
    bool    active_high;         /* true: active level is logic high */
} apex_activation_gpio_trigger_binding_t;

typedef struct {
    uint8_t payload_type_uuid[16];
    uint8_t n_preconditions;        /* 0..APEX_ACTIVATION_MAX_PRECONDITIONS */
    uint8_t n_trigger_sources;      /* 1..APEX_ACTIVATION_MAX_TRIGGER_SOURCES */
    uint8_t trigger_source_categories[APEX_ACTIVATION_MAX_TRIGGER_SOURCES];
    /* Bit i set ⇒ precondition i auto-starts on power-up (transitions to Running
     * without a host START_PRECONDITION). §4.1 "Auto-start vs. host-start". */
    uint16_t auto_start_mask;
    /* Optional GPIO-backed preconditions. Each entry binds one precondition to a
     * discrete-IO line. Requires hooks.gpio_read. At most one per precondition. */
    uint8_t n_gpio_bindings;        /* 0..APEX_ACTIVATION_MAX_PRECONDITIONS */
    apex_activation_gpio_binding_t gpio_bindings[APEX_ACTIVATION_MAX_PRECONDITIONS];
    /* Optional GPIO-backed trigger sources. Parallel to gpio_bindings but for
     * HARDWARE_INPUT trigger sources rather than preconditions. */
    uint8_t n_gpio_trigger_bindings; /* 0..APEX_ACTIVATION_MAX_TRIGGER_SOURCES */
    apex_activation_gpio_trigger_binding_t gpio_trigger_bindings[APEX_ACTIVATION_MAX_TRIGGER_SOURCES];
    /* Optional host-condition bindings (§6.3): declares which preconditions are
     * host-started against a flight condition and whether they auto-trigger. A
     * precondition not listed here is device-started by definition; a plain
     * host-start precondition the host drives by observing NotStarted need not
     * appear either. Emitted as the CAPABILITY host-condition tail. */
    uint8_t n_host_conditions;      /* 0..APEX_ACTIVATION_MAX_PRECONDITIONS */
    apex_activation_host_condition_binding_t host_conditions[APEX_ACTIVATION_MAX_PRECONDITIONS];
    /* Optional timing tail (§6.3): worst-case ENABLING / DISABLING durations in
     * milliseconds; 0 = unspecified. The tail is emitted **both-or-neither** and
     * only when either value is nonzero. Advisory only — it does not change the
     * transition model (that is decided by the on_enable_begin / on_disable_begin
     * hooks); it lets the host apply a timeout policy (§7.1). */
    uint16_t enable_time_ms;
    uint16_t disable_time_ms;
    /* Mid-transition reversal capability (§3, §5), device-dependent:
     *   can_abort_enabling  — SET_DISABLED while ENABLING is honored (→ DISABLING)
     *                         instead of REJECT_BUSY.
     *   can_reverse_disabling — SET_ENABLED while DISABLING is honored (→ ENABLING)
     *                         instead of REJECT_BUSY. */
    bool can_abort_enabling;
    bool can_reverse_disabling;
    /* Initial value of activations_remaining. APEX_ACT_ACTIVATIONS_UNLIMITED for
     * an indefinite count. */
    uint8_t initial_activations_remaining;
} apex_activation_device_caps_t;

/* ---------------------------------------------------------------------------
 * Device-side
 * ------------------------------------------------------------------------- */

/* Application hooks. Called from apex_activation_device_on_rx() or
 * apex_activation_device_tick() context. */
typedef struct {
    /* Optional: called when the host asks to start precondition `idx`. Return
     * APEX_OK to accept (the lib ACKs ACCEPTED and sets the precondition to
     * Running), or non-APEX_OK to reject with REJECT_PRECONDITION. If unset, all
     * START_PRECONDITION commands for a declared, non-terminal index are accepted. */
    apex_status_t (*on_start_precondition)(void *user, uint8_t idx);
    void *on_start_precondition_user;

    /* Optional: a trigger requires device SOFTWARE to perform the activation
     * now. Fired on the software path — apex_activation_device_trigger() and a
     * host TRIGGER command — as the device enters EXECUTING. The application
     * performs its action and calls apex_activation_device_complete_execution()
     * when done. `source_idx` is the declared trigger source that fired.
     * A device whose activation is performed entirely in hardware never needs
     * this hook: it reports the fact with apex_activation_device_report_execution()
     * and observes on_executed instead. */
    void (*on_execute_request)(void *user, uint8_t source_idx);
    void *on_execute_request_user;

    /* Optional: neutral notification that an activation has COMPLETED — whether
     * performed in software or already done in hardware. Fired once per
     * activation: at apex_activation_device_complete_execution() for the software
     * path, and inside apex_activation_device_report_execution() for the hardware
     * path. Ideal for telemetry, logging, and OSD. `source_idx` is the source
     * that fired. */
    void (*on_executed)(void *user, uint8_t source_idx);
    void *on_executed_user;

    /* Optional: state transition observer. */
    void (*on_state_change)(void *user, apex_activation_state_t s);
    void *on_state_change_user;

    /* Optional: precondition state-change observer. Fired whenever a
     * precondition's reported state changes — including the initial auto-start
     * transition to Running and every autonomous or host-driven change
     * thereafter. Well suited to driving DISPLAY_TEXT prompts/progress (§6.8) as
     * a precondition advances. `idx` is the precondition index, `new_state` its
     * new value. */
    void (*on_precondition_change)(void *user, uint8_t idx,
                                   apex_precond_state_t new_state);
    void *on_precondition_change_user;

    /* Required when caps.n_gpio_bindings > 0: read the current logic level of a
     * GPIO line. Returns true for logic high, false for logic low. Polled from
     * apex_activation_device_tick(). */
    bool (*gpio_read)(void *user, uint8_t pin);
    void *gpio_read_user;

    /* Optional: transition hooks (§3). When on_enable_begin is set the device is
     * NON-INSTANT for enable: an accepted SET_ENABLED from READY enters ENABLING
     * and fires this hook; the app performs its enable work and later calls
     * apex_activation_device_transition_complete() to reach ENABLED (or
     * apex_activation_device_transition_failed() to fall safely back to READY).
     * When on_enable_begin is NULL, enable is INSTANT: SET_ENABLED goes directly
     * READY → ENABLED, exactly as in class version 0. on_disable_begin is the
     * symmetric hook for the disable transition (ENABLED → DISABLING → READY, or
     * instant ENABLED → READY when NULL). A device may be instant for one
     * direction and non-instant for the other. */
    void (*on_enable_begin)(void *user);
    void *on_enable_begin_user;
    void (*on_disable_begin)(void *user);
    void *on_disable_begin_user;
} apex_activation_device_hooks_t;

typedef struct apex_activation_device {
    apex_device_t *core;                     /* required */
    apex_activation_device_caps_t caps;
    apex_activation_device_hooks_t hooks;

    apex_activation_state_t state;
    uint8_t activations_remaining;
    uint8_t last_trigger_source;            /* 0xFF = none yet */
    uint16_t fault_flags;
    uint8_t precondition_states[APEX_ACTIVATION_MAX_PRECONDITIONS];
    uint8_t payload_specific[APEX_ACTIVATION_PAYLOAD_SPECIFIC_MAX];
    uint8_t payload_specific_len;

    uint32_t now_ms;
    uint32_t last_status_tx_ms;
    bool capability_sent;
    bool status_dirty;                       /* transition happened; flush ASAP */
    bool self_disarm_pending;                /* §7.4: a session was lost during
                                              * this arming episode while the state
                                              * could not yet self-disarm (EXECUTING);
                                              * disarm on reaching ENABLED. Cleared
                                              * by a fresh SET_ENABLED. */
    apex_device_link_state_t last_link;

    /* Host display real estate learned from HOST_DISPLAY_INFO (§6.7). Defaults
     * of 32 / 1 apply until a frame is received. */
    uint8_t host_char_limit;
    uint8_t host_n_banner_lines;

    /* Per-DISPLAY_TEXT-target rate-cap tracking (§6.6). Index via
     * dev_display_target_slot(): 0..15 = precondition lines, 16..30 = banner
     * lines. */
    bool     display_used[APEX_ACT_DISPLAY_N_TARGETS];
    uint32_t display_last_ms[APEX_ACT_DISPLAY_N_TARGETS];

    /* Per-GPIO-binding debounce tracking (parallel to caps.gpio_bindings). */
    bool gpio_active_timing[APEX_ACTIVATION_MAX_PRECONDITIONS];
    uint32_t gpio_active_since_ms[APEX_ACTIVATION_MAX_PRECONDITIONS];

    /* Application-owned reenum_permitted hook saved from the core cfg at init
     * (§7.4 wiring). apex_activation_device_init() installs the class's own
     * deferral hook on core->cfg — false while EXECUTING — and composes it with
     * any hook the app had already set: BOTH must permit for the reset to be
     * honored. */
    bool (*app_reenum_permitted)(void *user);
    void *app_reenum_permitted_user;
} apex_activation_device_t;

/* Initialize. The caller is responsible for wiring this into the core device's
 * on_class_rx callback (forwarding to apex_activation_device_on_rx).
 *
 * Re-enumeration (§7.4): init installs a reenum_permitted hook on the core
 * device's cfg that DEFERS a host RESET_REQUEST while the class state is
 * EXECUTING (the action always runs to completion) and permits it from every
 * other state. An app hook already present on core->cfg is preserved and
 * composed conservatively — both the class and the app must permit. A session
 * loss self-issues SET_DISABLE (§7.4): an ENABLED/ENABLING device is stood down
 * so no successor host inherits an assumed-armed payload, and the resulting
 * state (DISABLING or, on an instant device, READY) is reported in the first
 * STATUS after re-discovery, never assumed. All other device-internal state
 * SURVIVES (latched preconditions, activations_remaining, EXHAUSTED, faults);
 * only host-session artifacts reset on re-discovery (CAPABILITY re-emits,
 * display caps revert to 32 / 1). */
apex_status_t apex_activation_device_init(apex_activation_device_t *act,
                                          apex_device_t *core,
                                          const apex_activation_device_caps_t *caps,
                                          const apex_activation_device_hooks_t *hooks);

/* Drive STATUS/CAPABILITY timing and react to core-link transitions. */
void apex_activation_device_tick(apex_activation_device_t *act, uint32_t now_ms);

/* Pass inbound Activation-class payload (the class_msg_id byte plus body). Wire
 * this into the core device's on_class_rx callback. */
void apex_activation_device_on_rx(apex_activation_device_t *act,
                                  const uint8_t *payload,
                                  size_t payload_len);

/* Drive a precondition's state directly — the primitive beneath the named
 * helpers below. Latched Valid never reverts (§4.1). A change fires
 * on_precondition_change. */
void apex_activation_device_set_precondition_state(apex_activation_device_t *act,
                                                   uint8_t idx,
                                                   apex_precond_state_t new_state);

/* Autonomous precondition drive (§4.1). A device that validates a precondition
 * on its own — without waiting for a host START_PRECONDITION — declares it in
 * caps.auto_start_mask and drives it with these: start it, then report the
 * outcome when its own monitoring completes. They are thin, self-documenting
 * wrappers over set_precondition_state.
 *   _start → Running   (begin/retry validating; moves STANDBY → VALIDATING)
 *   _pass  → Valid      (latch success; advances to READY when all are Valid)
 *   _fail  → Failed     (retriable or terminal per the per-payload profile)
 * Return APEX_ERR_INVALID_ARGS for an undeclared index, APEX_ERR_BAD_STATE if the
 * precondition is already latched Valid, else APEX_OK. */
apex_status_t apex_activation_device_precondition_start(apex_activation_device_t *act,
                                                        uint8_t idx);
apex_status_t apex_activation_device_precondition_pass(apex_activation_device_t *act,
                                                       uint8_t idx);
apex_status_t apex_activation_device_precondition_fail(apex_activation_device_t *act,
                                                       uint8_t idx);

/* Fire a trigger whose activation is performed in SOFTWARE. `source_idx` must be
 * a declared trigger source (any category). Honored only in ENABLED (§3 —
 * triggers are honored only in ENABLED, and never once a SET_DISABLED has been
 * accepted). The device enters EXECUTING and fires on_execute_request; the
 * application performs the action and calls apex_activation_device_complete_execution().
 * on_execute_request is a NOTIFICATION, not the only completion path: an app that
 * leaves it NULL must still complete the EXECUTING state itself (by its own logic,
 * calling complete_execution) — a device that enters EXECUTING and never completes
 * it stays there and blocks re-enumeration (§7.4). For an activation that already
 * occurred in hardware, prefer apex_activation_device_report_execution(), which
 * completes in one call. Returns APEX_OK if the trigger was honored,
 * APEX_ERR_BAD_STATE if not in ENABLED, or APEX_ERR_INVALID_ARGS for an
 * undeclared source. */
apex_status_t apex_activation_device_trigger(apex_activation_device_t *act,
                                             uint8_t source_idx);

/* Report an activation that has ALREADY occurred in hardware (the trigger and
 * the action are one and the same — nothing for software to perform). `source_idx`
 * must be a declared trigger source. Honored only in ENABLED. The device passes
 * through EXECUTING and completes the activation in a single call:
 * activations_remaining is decremented, on_executed fires, and the device settles
 * in ENABLED (more remain) or EXHAUSTED. Do NOT also call complete_execution().
 * Returns APEX_OK, APEX_ERR_BAD_STATE (not ENABLED), or APEX_ERR_INVALID_ARGS. */
apex_status_t apex_activation_device_report_execution(apex_activation_device_t *act,
                                                      uint8_t source_idx);

/* Called by the application when a SOFTWARE EXECUTING action (started via
 * on_execute_request) has finished. The device decrements activations_remaining,
 * fires on_executed, and transitions to ENABLED (if more activations remain) or
 * EXHAUSTED (otherwise). No effect outside EXECUTING. */
void apex_activation_device_complete_execution(apex_activation_device_t *act);

/* Device-initiated disarm — the self-issued equivalent of a host SET_DISABLED
 * (§3, §5), for a device that must stand itself down (a safety condition, a
 * trigger-window expiry, …). From ENABLED the device enters DISABLING (or goes
 * straight to READY on an instant device); from ENABLING it aborts toward READY
 * when caps.can_abort_enabling permits. In DISABLING/READY it is an idempotent
 * no-op. Fires on_disable_begin on a non-instant device (complete the transition
 * with apex_activation_device_transition_complete()).
 *
 * There is intentionally NO device self-ENABLE: arming is host-commanded only
 * (SET_ENABLED), a deliberate safety invariant (§3). A device drives itself only
 * downward (disarm) and sideways (preconditions, faults), never into the armed
 * region on its own.
 *
 * Returns APEX_OK if the device is now disarming or already disarmed, or
 * APEX_ERR_BAD_STATE from a state where disarm does not apply (STANDBY,
 * VALIDATING, EXECUTING, EXHAUSTED, FAULT, or ENABLING without abort support). */
apex_status_t apex_activation_device_disable(apex_activation_device_t *act);

/* Convenience for the §4.1 trigger-window pattern on the ENABLED state: the
 * declared window elapsed with no trigger, so return to a safer state and flag
 * it. Atomically sets the self-clearing TRIGGER_WINDOW_EXPIRED fault flag and
 * self-disarms (disable()), avoiding the split-brain of setting one without the
 * other. Returns disable()'s status (the flag is set regardless). A payload whose
 * "safer state" is not READY (e.g. a window gating a precondition) should compose
 * the primitives itself. */
apex_status_t apex_activation_device_trigger_window_expired(apex_activation_device_t *act);

/* Non-instant transition completion (§3). Call from the on_enable_begin /
 * on_disable_begin path once the enable/disable work is done: ENABLING → ENABLED,
 * DISABLING → READY. A successful transition clears the self-clearing
 * TRANSITION_FAILED fault flag. No effect outside ENABLING/DISABLING. */
void apex_activation_device_transition_complete(apex_activation_device_t *act);

/* Report a transition that failed but left the device in a SAFE condition (§3):
 * from ENABLING or DISABLING the device returns to READY and sets the
 * self-clearing TRANSITION_FAILED fault flag (bit 5). A failure that leaves the
 * device UNSAFE is not this call — use apex_activation_device_set_fault_flag()
 * with a latched bit to force FAULT. No effect outside ENABLING/DISABLING. */
void apex_activation_device_transition_failed(apex_activation_device_t *act);

/* Set a fault flag. Latched flags also transition the device to FAULT (for an
 * explicit, unconditional fault from any state, prefer apex_activation_device_fault()).
 * NOTE: do not set the transition flag TRANSITION_FAILED through this call — it
 * would set the bit without the accompanying return-to-READY. Report a failed
 * enable/disable via apex_activation_device_transition_failed() instead, which
 * does both. */
void apex_activation_device_set_fault_flag(apex_activation_device_t *act,
                                           uint16_t flag);

/* Clear a self-clearing fault flag (TRIGGER_WINDOW_EXPIRED, COMMS_LOST, or
 * TRANSITION_FAILED). Latched flags are not cleared; they require a reset. */
void apex_activation_device_clear_fault_flag(apex_activation_device_t *act,
                                             uint16_t flag);

/* Force the device into the terminal FAULT state (§3) from ANY state, recording
 * `flags` in fault_flags as the reason — pass APEX_ACT_FAULT_INTERNAL_ERROR for a
 * generic self-check/hardware failure, or any payload-relevant fault bit(s).
 * Unlike set_fault_flag(), this always transitions to FAULT whether or not the
 * flags are latched. FAULT is terminal in this class version; only a device reset
 * (a fresh apex_activation_device_init) recovers. */
void apex_activation_device_fault(apex_activation_device_t *act, uint16_t flags);

/* Set the payload_specific region of subsequent STATUS frames. Length must be
 * <= APEX_ACTIVATION_PAYLOAD_SPECIFIC_MAX. */
apex_status_t apex_activation_device_set_payload_specific(apex_activation_device_t *act,
                                                          const uint8_t *bytes,
                                                          size_t len);

/* Push an advisory DISPLAY_TEXT line (§6.8). `target` is a precondition line
 * (0x00..0x0F) or a banner line (0xF0..0xFE). `str` is a NUL-terminated ASCII
 * string; pass NULL or "" to CLEAR the target (text_len = 0). The string is
 * truncated to the host's char_limit (from HOST_DISPLAY_INFO, default 32).
 * Latest-wins per target.
 *
 * Returns:
 *   APEX_OK              — frame sent (or cleared).
 *   APEX_ERR_BAD_STATE   — device not CONNECTED, or the §6.6 5 Hz-per-target
 *                          rate cap would be exceeded (push rejected; the caller
 *                          relies on latest-wins and may retry later).
 *   APEX_ERR_INVALID_ARGS— reserved/invalid target byte.
 *   APEX_ERR_UNSUPPORTED — banner target but the host declared no banner line
 *                          for it (n_banner_lines too small / banner unsupported). */
apex_status_t apex_activation_device_push_text(apex_activation_device_t *act,
                                               uint8_t target,
                                               const char *str);

static inline apex_activation_state_t apex_activation_device_state(const apex_activation_device_t *act)
{
    return act->state;
}

/* Activations left before the device becomes EXHAUSTED. */
static inline uint8_t apex_activation_device_activations_remaining(const apex_activation_device_t *act)
{
    return act->activations_remaining;
}

/* Current state of precondition `idx` (APEX_PRECOND_NOT_STARTED for an
 * out-of-range index). */
static inline apex_precond_state_t apex_activation_device_precondition_state(const apex_activation_device_t *act,
                                                                            uint8_t idx)
{
    return (idx < act->caps.n_preconditions)
           ? (apex_precond_state_t)act->precondition_states[idx]
           : APEX_PRECOND_NOT_STARTED;
}

/* Current fault_flags bitfield (§6.5). */
static inline uint16_t apex_activation_device_fault_flags(const apex_activation_device_t *act)
{
    return act->fault_flags;
}

/* Index of the trigger source that caused the most recent EXECUTING transition
 * (0xFF before any activation) — the value reported in the STATUS frame (§6.5). */
static inline uint8_t apex_activation_device_last_trigger_source(const apex_activation_device_t *act)
{
    return act->last_trigger_source;
}

/* ---------------------------------------------------------------------------
 * Host-side
 * ------------------------------------------------------------------------- */

/* Wire-level GPIO mapping declared in the CAPABILITY frame (§6.3): "precondition
 * `precondition_idx` is validated by GPIO `pin`, active at `active_high`". */
typedef struct {
    uint8_t precondition_idx;
    uint8_t pin;                    /* apex_activation_gpio_pin_t (3 or 4) */
    bool active_high;               /* decoded from the muxed pin byte (§6.3) */
} apex_activation_gpio_map_t;

/* Wire-level GPIO mapping for a HARDWARE_INPUT trigger source (§6.3). */
typedef struct {
    uint8_t trigger_source_idx;
    uint8_t pin;                    /* apex_activation_gpio_pin_t (3 or 4) */
    bool    active_high;
} apex_activation_gpio_trigger_map_t;

typedef struct {
    uint8_t payload_type_uuid[16];
    uint8_t n_preconditions;
    uint8_t n_trigger_sources;
    uint8_t trigger_source_categories[APEX_ACTIVATION_MAX_TRIGGER_SOURCES];
    uint8_t n_gpio_bindings;
    apex_activation_gpio_map_t gpio_bindings[APEX_ACTIVATION_MAX_PRECONDITIONS];
    /* Optional: trigger source GPIO bindings. 0 if the device omitted them. */
    uint8_t n_gpio_trigger_bindings;
    apex_activation_gpio_trigger_map_t gpio_trigger_bindings[APEX_ACTIVATION_MAX_TRIGGER_SOURCES];
    /* Host-condition bindings (§6.3): which preconditions are host-started and
     * under what flight condition. Indexed 0..n_host_conditions-1 — each entry
     * names its own precondition_idx. */
    uint8_t n_host_conditions;
    apex_activation_host_condition_binding_t host_conditions[APEX_ACTIVATION_MAX_PRECONDITIONS];
    /* Optional timing tail (§6.3). Worst-case ENABLING / DISABLING durations in
     * milliseconds; 0 = unspecified (also the value when the tail was absent). */
    uint16_t enable_time_ms;
    uint16_t disable_time_ms;
} apex_activation_capability_t;

typedef struct {
    apex_activation_state_t state;
    uint8_t activations_remaining;
    uint8_t last_trigger_source;
    uint16_t fault_flags;
    uint8_t n_preconditions;
    uint8_t precondition_states[APEX_ACTIVATION_MAX_PRECONDITIONS];
    const uint8_t *payload_specific;
    size_t payload_specific_len;
} apex_activation_status_t;

typedef struct {
    uint8_t acked_command;
    apex_activation_ack_result_t result;
    apex_activation_state_t current_state;
} apex_activation_ack_t;

typedef struct {
    void (*on_capability)(void *user, uint8_t device_id, const apex_activation_capability_t *cap);
    void *on_capability_user;
    void (*on_status)(void *user, uint8_t device_id, const apex_activation_status_t *st);
    void *on_status_user;
    void (*on_ack)(void *user, uint8_t device_id, const apex_activation_ack_t *ack);
    void *on_ack_user;
    /* Optional: fired on each inbound DISPLAY_TEXT (§6.8). `text` points into the
     * frame buffer (valid only during the callback; copy to persist) and is NOT
     * NUL-terminated; `text_len` is its length (0 = the target was cleared).
     * `target` is the raw target byte (precondition 0x00..0x0F / banner
     * 0xF0..0xFE). Advisory only — nothing functional may depend on it. */
    void (*on_display_text)(void *user, uint8_t device_id,
                            uint8_t target, const char *text, size_t text_len);
    void *on_display_text_user;
} apex_activation_host_hooks_t;

typedef struct {
    apex_host_t *core;                                  /* required */
    apex_activation_host_hooks_t hooks;
    /* Per-device known n_preconditions, captured from CAPABILITY frames. Needed
     * to parse STATUS frames (which carry per-precondition state but no count).
     * 0 means "no CAPABILITY seen yet — STATUS unparseable". */
    uint8_t n_precond_per_device[256];
} apex_activation_host_t;

apex_status_t apex_activation_host_init(apex_activation_host_t *h,
                                        apex_host_t *core,
                                        const apex_activation_host_hooks_t *hooks);

apex_status_t apex_activation_host_start_precondition(apex_activation_host_t *h,
                                                      uint8_t device_id,
                                                      uint8_t precondition_idx);
apex_status_t apex_activation_host_set_enabled(apex_activation_host_t *h, uint8_t device_id);
apex_status_t apex_activation_host_set_disabled(apex_activation_host_t *h, uint8_t device_id);
apex_status_t apex_activation_host_trigger(apex_activation_host_t *h, uint8_t device_id);

/* Send one HOST_DISPLAY_INFO frame (§6.7) declaring the host's display real
 * estate. The host should send this once, immediately after receiving
 * CAPABILITY, and may resend it if its display configuration changes.
 *   char_limit     — max chars rendered per line; 0 is sent verbatim and the
 *                    device interprets it as 32.
 *   n_banner_lines — free banner lines available to this device; 0 = banner
 *                    text unsupported. */
apex_status_t apex_activation_host_send_display_info(apex_activation_host_t *h,
                                                     uint8_t device_id,
                                                     uint8_t char_limit,
                                                     uint8_t n_banner_lines);

#ifdef __cplusplus
}
#endif

#endif /* APEX_ACTIVATION_H */
