/**
 * @file apex_activation.h
 * @brief Activation device class (traffic_type = 1). Both Host and Device
 *        sides. See APEX_Activation_Class.md for the spec this implements.
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

typedef enum {
    APEX_ACTIVATION_STATE_RESERVED   = 0x00,
    APEX_ACTIVATION_STATE_STANDBY    = 0x01,
    APEX_ACTIVATION_STATE_VALIDATING = 0x02,
    APEX_ACTIVATION_STATE_READY      = 0x03,
    APEX_ACTIVATION_STATE_ENABLED    = 0x04,
    APEX_ACTIVATION_STATE_EXECUTING  = 0x05,
    APEX_ACTIVATION_STATE_EXHAUSTED  = 0x06,
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

/* GPIO pins usable as precondition triggers. The core 10-pin connector exposes
 * exactly two discrete-IO lines (Core §2: Pin 3 / Pin 4, the APEX_INTERFACE_
 * FLAG_GPIO pins), so a GPIO-backed precondition is always bound to one of
 * these two. The value is the connector pin number and is passed verbatim to
 * the application's gpio_read hook. */
typedef enum {
    APEX_ACTIVATION_GPIO_PIN3 = 3,
    APEX_ACTIVATION_GPIO_PIN4 = 4,
} apex_activation_gpio_pin_t;

/* On the wire (§6.3) each GPIO mapping packs the pin and its active level into a
 * single byte: the low 7 bits are the connector pin (apex_activation_gpio_pin_t,
 * 3 or 4), and bit 7 is the active level — set = active-high, clear = active-low.
 * Both fit comfortably given pins only run 3..4. */
#define APEX_ACTIVATION_GPIO_PIN_MASK        0x7Fu
#define APEX_ACTIVATION_GPIO_ACTIVE_HIGH_BIT 0x80u

typedef enum {
    APEX_ACT_MSG_CAPABILITY = 1,
    APEX_ACT_MSG_COMMAND    = 2,
    APEX_ACT_MSG_STATUS     = 3,
    APEX_ACT_MSG_ACK        = 4,
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
} apex_activation_ack_result_t;

#define APEX_ACT_FAULT_PRECONDITION_FAILED    (1u << 0)
#define APEX_ACT_FAULT_SEQUENCE_VIOLATION     (1u << 1)
#define APEX_ACT_FAULT_TRIGGER_WINDOW_EXPIRED (1u << 2)
#define APEX_ACT_FAULT_COMMS_LOST             (1u << 3)
#define APEX_ACT_FAULT_INTERNAL_ERROR         (1u << 4)

/* Latched bits force the device into FAULT; self-clearing bits do not. */
#define APEX_ACT_FAULT_LATCHED_MASK                                       \
    (APEX_ACT_FAULT_PRECONDITION_FAILED | APEX_ACT_FAULT_SEQUENCE_VIOLATION | \
     APEX_ACT_FAULT_INTERNAL_ERROR)

/* "Unlimited" sentinel for activations_remaining (§6.5). */
#define APEX_ACT_ACTIVATIONS_UNLIMITED 0xFFu

/* Periodic STATUS floor — §6.6. */
#define APEX_ACTIVATION_STATUS_PERIOD_MS 1000u

/* ---------------------------------------------------------------------------
 * Device-side capability declaration
 * ------------------------------------------------------------------------- */

/* Optional binding of a precondition to a GPIO line. When a precondition is
 * GPIO-backed, the class drives its Running → Valid transition by polling the
 * line via the gpio_read hook (see apex_activation_device_hooks_t) instead of
 * the application calling apex_activation_device_set_precondition_state(). The
 * binding does not change *when* validation starts: the precondition still has
 * to be Running (auto-start via auto_start_mask, or host-start via
 * START_PRECONDITION) before the line is polled. This stays purely
 * device-internal — the wire protocol (§6) is unchanged. */
typedef struct {
    uint8_t precondition_idx;       /* declared precondition this gates (< n_preconditions) */
    uint8_t pin;                    /* apex_activation_gpio_pin_t (Pin 3 / Pin 4) */
    bool active_high;               /* true: active level is logic high */
    /* The line must read its active level continuously for at least this many
     * milliseconds before the precondition latches to Valid. 0 = latch on the
     * first active read (no debounce). */
    uint32_t stable_ms;
} apex_activation_gpio_binding_t;

typedef struct {
    uint8_t payload_type_uuid[16];
    uint8_t class_spec_version;     /* default 0 */
    uint8_t n_preconditions;        /* 0..APEX_ACTIVATION_MAX_PRECONDITIONS */
    uint8_t n_trigger_sources;      /* 1..APEX_ACTIVATION_MAX_TRIGGER_SOURCES */
    uint8_t trigger_source_categories[APEX_ACTIVATION_MAX_TRIGGER_SOURCES];
    /* Bit i set ⇒ precondition i auto-starts on power-up (transitions to
     * Running without a host START_PRECONDITION). §4.1 "Auto-start vs.
     * host-start". */
    uint16_t auto_start_mask;
    /* Optional GPIO-backed preconditions. Each entry binds one precondition to
     * a discrete-IO line. Requires hooks.gpio_read to be set. At most one
     * binding per precondition index. */
    uint8_t n_gpio_bindings;        /* 0..APEX_ACTIVATION_MAX_PRECONDITIONS */
    apex_activation_gpio_binding_t gpio_bindings[APEX_ACTIVATION_MAX_PRECONDITIONS];
    /* Initial value of activations_remaining. Use APEX_ACT_ACTIVATIONS_UNLIMITED
     * for an indefinite count. */
    uint8_t initial_activations_remaining;
} apex_activation_device_caps_t;

/* ---------------------------------------------------------------------------
 * Device-side
 * ------------------------------------------------------------------------- */

/* Application hooks. Called from apex_activation_device_on_rx() or
 * apex_activation_device_tick() context. */
typedef struct {
    /* Optional: called when the host asks to start precondition `idx`. The
     * application returns APEX_OK to accept (the lib then ACKs ACCEPTED and
     * sets the precondition to Running), or APEX_ERR_BAD_STATE / similar to
     * reject. If unset, all START_PRECONDITION commands are accepted. */
    apex_status_t (*on_start_precondition)(void *user, uint8_t idx);
    void *on_start_precondition_user;

    /* Optional: fired when the device transitions into EXECUTING. The
     * application starts its action and eventually calls
     * apex_activation_device_complete_execution(). */
    void (*on_execute)(void *user);
    void *on_execute_user;

    /* Optional: state transition observer. */
    void (*on_state_change)(void *user, apex_activation_state_t s);
    void *on_state_change_user;

    /* Required when caps.n_gpio_bindings > 0: read the current logic level of a
     * GPIO line. `pin` is the binding's apex_activation_gpio_pin_t value.
     * Returns true for logic high, false for logic low. Polled from
     * apex_activation_device_tick(). */
    bool (*gpio_read)(void *user, uint8_t pin);
    void *gpio_read_user;
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
    apex_device_link_state_t last_link;

    /* Per-GPIO-binding debounce tracking (parallel to caps.gpio_bindings).
     * gpio_active_timing[b] is true while the line has been continuously active;
     * gpio_active_since_ms[b] is the now_ms at which that streak began. */
    bool gpio_active_timing[APEX_ACTIVATION_MAX_PRECONDITIONS];
    uint32_t gpio_active_since_ms[APEX_ACTIVATION_MAX_PRECONDITIONS];
} apex_activation_device_t;

/* Initialize. The caller is responsible for wiring this into the core
 * device's on_class_rx callback (forwarding to apex_activation_device_on_rx). */
apex_status_t apex_activation_device_init(apex_activation_device_t *act,
                                          apex_device_t *core,
                                          const apex_activation_device_caps_t *caps,
                                          const apex_activation_device_hooks_t *hooks);

/* Drive STATUS/CAPABILITY timing and react to core-link transitions. */
void apex_activation_device_tick(apex_activation_device_t *act, uint32_t now_ms);

/* Pass inbound Activation-class payload (the class_msg_id byte plus body).
 * Wire this into the core device's on_class_rx callback. */
void apex_activation_device_on_rx(apex_activation_device_t *act,
                                  const uint8_t *payload,
                                  size_t payload_len);

/* Drive a precondition's state directly (used for both auto-start and
 * host-start preconditions once validation is running). */
void apex_activation_device_set_precondition_state(apex_activation_device_t *act,
                                                   uint8_t idx,
                                                   apex_precond_state_t new_state);

/* Fire a hardware/timer/external trigger. `source_idx` must be a declared
 * trigger source. No effect if not in ENABLED. */
void apex_activation_device_trigger(apex_activation_device_t *act,
                                    uint8_t source_idx);

/* Called by the application when the EXECUTING action has finished. The
 * device decrements activations_remaining and transitions to ENABLED (if
 * more activations remain) or EXHAUSTED (otherwise). */
void apex_activation_device_complete_execution(apex_activation_device_t *act);

/* Set a fault flag. Latched flags also transition the device to FAULT. */
void apex_activation_device_set_fault_flag(apex_activation_device_t *act,
                                           uint16_t flag);

/* Clear a self-clearing fault flag (TRIGGER_WINDOW_EXPIRED or COMMS_LOST).
 * Latched flags are not cleared by this function; they require a reset. */
void apex_activation_device_clear_fault_flag(apex_activation_device_t *act,
                                             uint16_t flag);

/* Set the payload_specific region of subsequent STATUS frames. Length must
 * be <= APEX_ACTIVATION_PAYLOAD_SPECIFIC_MAX. */
apex_status_t apex_activation_device_set_payload_specific(apex_activation_device_t *act,
                                                          const uint8_t *bytes,
                                                          size_t len);

static inline apex_activation_state_t apex_activation_device_state(const apex_activation_device_t *act)
{
    return act->state;
}

/* ---------------------------------------------------------------------------
 * Host-side
 * ------------------------------------------------------------------------- */

/* Wire-level GPIO mapping declared in the CAPABILITY frame (§6.3): "precondition
 * `precondition_idx` is validated by GPIO `pin`, active at `active_high`". Any
 * debounce interval is device-internal and not carried on the wire. */
typedef struct {
    uint8_t precondition_idx;
    uint8_t pin;                    /* apex_activation_gpio_pin_t (3 or 4) */
    bool active_high;               /* decoded from the muxed pin byte (§6.3) */
} apex_activation_gpio_map_t;

typedef struct {
    uint8_t class_spec_version;
    uint8_t payload_type_uuid[16];
    uint8_t n_preconditions;
    uint8_t n_trigger_sources;
    uint8_t trigger_source_categories[APEX_ACTIVATION_MAX_TRIGGER_SOURCES];
    uint8_t n_gpio_bindings;
    apex_activation_gpio_map_t gpio_bindings[APEX_ACTIVATION_MAX_PRECONDITIONS];
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
} apex_activation_host_hooks_t;

typedef struct {
    apex_host_t *core;                                  /* required */
    apex_activation_host_hooks_t hooks;
    /* Per-device known n_preconditions, captured from CAPABILITY frames. We
     * need this to parse STATUS frames (which carry per-precondition state
     * but no count). 0 means "no CAPABILITY seen yet — STATUS unparseable". */
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

#ifdef __cplusplus
}
#endif

#endif /* APEX_ACTIVATION_H */
