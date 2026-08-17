/**
 * @file apex_activation.c
 * @brief Activation device class (class version 1) — both Host and Device sides.
 *
 * Copyright (c) 2026 Neros Technologies. MIT License — see LICENSE.
 */
#include <string.h>

#include "apex/apex_activation.h"

/* ---------------------------------------------------------------------------
 * Device-side helpers
 * ------------------------------------------------------------------------- */

static void dev_set_state(apex_activation_device_t *act,
                          apex_activation_state_t s)
{
    if (act->state == s) return;
    act->state = s;
    act->status_dirty = true;
    if (act->hooks.on_state_change) {
        act->hooks.on_state_change(act->hooks.on_state_change_user, s);
    }
}

static void dev_clear_transition_failed(apex_activation_device_t *act)
{
    /* §6.5: TRANSITION_FAILED is self-clearing on the next successful enable/
     * disable transition. */
    if (act->fault_flags & APEX_ACT_FAULT_TRANSITION_FAILED) {
        act->fault_flags &= (uint16_t)~APEX_ACT_FAULT_TRANSITION_FAILED;
        act->status_dirty = true;
    }
}

static bool dev_all_preconditions_valid(const apex_activation_device_t *act)
{
    for (uint8_t i = 0; i < act->caps.n_preconditions; i++) {
        if (act->precondition_states[i] != APEX_PRECOND_VALID) return false;
    }
    return true;
}

static apex_status_t dev_send(apex_activation_device_t *act,
                              const uint8_t *payload, size_t len)
{
    return apex_device_send(act->core, payload, len);
}

static void dev_send_capability(apex_activation_device_t *act)
{
    /* §6.3 (class version 1 — NO class_spec_version byte):
     * class_msg_id + uuid(16) + n_preconditions + n_trigger_sources +
     * categories(N) + n_gpio_bindings + bindings(2 each) +
     * n_gpio_trigger_bindings + trigger_bindings(2 each) +
     * n_host_conditions + host_conditions(5 each) +
     * [enable_time_ms(2) + disable_time_ms(2)]  (both-or-neither timing tail). */
    uint8_t buf[1 + 16 + 1 + 1 + APEX_ACTIVATION_MAX_TRIGGER_SOURCES +
                1 + 2 * APEX_ACTIVATION_MAX_PRECONDITIONS +
                1 + 2 * APEX_ACTIVATION_MAX_TRIGGER_SOURCES +
                1 + 5 * APEX_ACTIVATION_MAX_PRECONDITIONS +
                2 + 2];
    size_t i = 0;
    buf[i++] = APEX_ACT_MSG_CAPABILITY;
    memcpy(buf + i, act->caps.payload_type_uuid, 16);
    i += 16;
    buf[i++] = act->caps.n_preconditions;
    buf[i++] = act->caps.n_trigger_sources;
    for (uint8_t s = 0; s < act->caps.n_trigger_sources; s++) {
        buf[i++] = act->caps.trigger_source_categories[s];
    }
    /* GPIO-backed precondition mappings (§6.3). */
    buf[i++] = act->caps.n_gpio_bindings;
    for (uint8_t b = 0; b < act->caps.n_gpio_bindings; b++) {
        buf[i++] = act->caps.gpio_bindings[b].precondition_idx;
        uint8_t pin_byte = (uint8_t)(act->caps.gpio_bindings[b].pin &
                                     APEX_ACTIVATION_GPIO_PIN_MASK);
        if (act->caps.gpio_bindings[b].active_high) {
            pin_byte |= APEX_ACTIVATION_GPIO_ACTIVE_HIGH_BIT;
        }
        buf[i++] = pin_byte;
    }
    /* Everything from n_gpio_trigger_bindings onward is an OPTIONAL tail (§6.3):
     * an absent tail is read as zero by the host, so we emit each count byte
     * only when it, or a later tail, actually carries data. This lets the
     * minimal device end its frame right after n_gpio_bindings — matching the
     * 22-byte §9 CAPABILITY — while still emitting an intermediate zero count
     * whenever a *later* tail is present (the host must read past it). */
    bool emit_timing = (act->caps.enable_time_ms != 0 ||
                        act->caps.disable_time_ms != 0);
    bool emit_host = (act->caps.n_host_conditions > 0) || emit_timing;
    bool emit_gpio_trig = (act->caps.n_gpio_trigger_bindings > 0) || emit_host;

    /* GPIO-backed trigger source mappings (§6.3), optional tail. */
    if (emit_gpio_trig) {
        buf[i++] = act->caps.n_gpio_trigger_bindings;
        for (uint8_t b = 0; b < act->caps.n_gpio_trigger_bindings; b++) {
            buf[i++] = act->caps.gpio_trigger_bindings[b].trigger_source_idx;
            uint8_t pin_byte = (uint8_t)(act->caps.gpio_trigger_bindings[b].pin &
                                         APEX_ACTIVATION_GPIO_PIN_MASK);
            if (act->caps.gpio_trigger_bindings[b].active_high) {
                pin_byte |= APEX_ACTIVATION_GPIO_ACTIVE_HIGH_BIT;
            }
            buf[i++] = pin_byte;
        }
    }
    /* Host-condition bindings (§6.3): declared explicitly by the device. */
    if (emit_host) {
        buf[i++] = act->caps.n_host_conditions;
        for (uint8_t p = 0; p < act->caps.n_host_conditions; p++) {
            const apex_activation_host_condition_binding_t *hc = &act->caps.host_conditions[p];
            buf[i++] = hc->precondition_idx;
            buf[i++] = hc->host_condition;
            buf[i++] = (uint8_t)(hc->condition_param & 0xFFu);
            buf[i++] = (uint8_t)(hc->condition_param >> 8);
            buf[i++] = hc->auto_trigger;
        }
    }
    /* Timing tail (§6.3): both-or-neither, emitted only if either is nonzero. */
    if (emit_timing) {
        buf[i++] = (uint8_t)(act->caps.enable_time_ms & 0xFFu);
        buf[i++] = (uint8_t)(act->caps.enable_time_ms >> 8);
        buf[i++] = (uint8_t)(act->caps.disable_time_ms & 0xFFu);
        buf[i++] = (uint8_t)(act->caps.disable_time_ms >> 8);
    }

    if (dev_send(act, buf, i) == APEX_OK) {
        act->capability_sent = true;
    }
}

static void dev_send_status(apex_activation_device_t *act)
{
    /* §6.5: msg_id + state + activations_remaining + last_trigger_source +
     * fault_flags(u16) = 6 bytes, plus n_preconditions + payload_specific. */
    uint8_t buf[6 + APEX_ACTIVATION_MAX_PRECONDITIONS + APEX_ACTIVATION_PAYLOAD_SPECIFIC_MAX];
    size_t i = 0;
    buf[i++] = APEX_ACT_MSG_STATUS;
    buf[i++] = (uint8_t)act->state;
    buf[i++] = act->activations_remaining;
    buf[i++] = act->last_trigger_source;
    buf[i++] = (uint8_t)(act->fault_flags & 0xFFu);
    buf[i++] = (uint8_t)((act->fault_flags >> 8) & 0xFFu);
    for (uint8_t p = 0; p < act->caps.n_preconditions; p++) {
        buf[i++] = act->precondition_states[p];
    }
    if (act->payload_specific_len > 0) {
        memcpy(buf + i, act->payload_specific, act->payload_specific_len);
        i += act->payload_specific_len;
    }
    if (dev_send(act, buf, i) == APEX_OK) {
        act->last_status_tx_ms = act->now_ms;
        act->status_dirty = false;
    }
}

static void dev_send_ack(apex_activation_device_t *act,
                         uint8_t acked_command,
                         apex_activation_ack_result_t result)
{
    uint8_t buf[4] = {
        APEX_ACT_MSG_ACK,
        acked_command,
        (uint8_t)result,
        (uint8_t)act->state,
    };
    dev_send(act, buf, sizeof(buf));
}

/* Map a DISPLAY_TEXT target byte to its rate-cap slot. Returns -1 for a
 * reserved / invalid target. */
static int dev_display_target_slot(uint8_t target)
{
    if (target <= APEX_ACT_DISPLAY_TARGET_PRECOND_MAX) {
        return (int)target;  /* 0..15 */
    }
    if (target >= APEX_ACT_DISPLAY_TARGET_BANNER_MIN &&
        target <= APEX_ACT_DISPLAY_TARGET_BANNER_MAX) {
        return (int)APEX_ACT_DISPLAY_N_PRECOND_TARGETS +
               (int)(target - APEX_ACT_DISPLAY_TARGET_BANNER_MIN);  /* 16..30 */
    }
    return -1;
}

apex_status_t apex_activation_device_push_text(apex_activation_device_t *act,
                                               uint8_t target,
                                               const char *str)
{
    if (!act) return APEX_ERR_INVALID_ARGS;
    if (apex_device_link_state(act->core) != APEX_DEVICE_STATE_CONNECTED) {
        return APEX_ERR_BAD_STATE;
    }
    int slot = dev_display_target_slot(target);
    if (slot < 0) return APEX_ERR_INVALID_ARGS;

    /* Banner target must fall within the host's declared banner lines (§6.8). */
    if (target >= APEX_ACT_DISPLAY_TARGET_BANNER_MIN) {
        uint8_t banner_idx = (uint8_t)(target - APEX_ACT_DISPLAY_TARGET_BANNER_MIN);
        if (banner_idx >= act->host_n_banner_lines) return APEX_ERR_UNSUPPORTED;
    }

    /* §6.6 rate cap: reject an over-rate push to the same target. */
    if (act->display_used[slot] &&
        (uint32_t)(act->now_ms - act->display_last_ms[slot]) < APEX_ACT_DISPLAY_MIN_INTERVAL_MS) {
        return APEX_ERR_BAD_STATE;
    }

    /* Truncate to the host's char_limit (§6.7) and the u8 payload_length cap. */
    uint8_t limit = act->host_char_limit;
    if (limit == 0) limit = APEX_ACT_DISPLAY_CHAR_LIMIT_DEFAULT;
    if (limit > APEX_ACT_DISPLAY_TEXT_MAX) limit = APEX_ACT_DISPLAY_TEXT_MAX;

    uint8_t text_len = 0;
    if (str) {
        while (str[text_len] && text_len < limit) text_len++;
    }

    uint8_t buf[3 + APEX_ACT_DISPLAY_TEXT_MAX];
    size_t i = 0;
    buf[i++] = APEX_ACT_MSG_DISPLAY_TEXT;
    buf[i++] = target;
    buf[i++] = text_len;
    for (uint8_t c = 0; c < text_len; c++) buf[i++] = (uint8_t)str[c];

    apex_status_t r = dev_send(act, buf, i);
    if (r == APEX_OK) {
        act->display_used[slot] = true;
        act->display_last_ms[slot] = act->now_ms;
    }
    return r;
}

/* ---------------------------------------------------------------------------
 * Device-side state machine
 * ------------------------------------------------------------------------- */

static void dev_maybe_advance_to_ready(apex_activation_device_t *act)
{
    if (act->state == APEX_ACTIVATION_STATE_VALIDATING &&
        dev_all_preconditions_valid(act)) {
        dev_set_state(act, APEX_ACTIVATION_STATE_READY);
    }
}

static void dev_reset_session_state(apex_activation_device_t *act)
{
    /* Called on core-link transitions (RESET_REQUEST re-enumeration, watchdog
     * return to discovery, reconnect). §7.4: this clears only HOST-SESSION
     * artifacts. The armed region is handled separately by the caller — a
     * session loss self-issues SET_DISABLE (dev_self_disable) so the device does
     * not carry ENABLED/ENABLING across the gap. Everything else device-internal
     * — latched preconditions, activations_remaining, fault flags, GPIO debounce
     * streaks, and the post-disarm state (DISABLING/READY, or a surviving
     * EXHAUSTED) — persists; the only way back to STANDBY is a device reset, i.e.
     * a fresh apex_activation_device_init (§3). */
    act->host_char_limit = APEX_ACT_DISPLAY_CHAR_LIMIT_DEFAULT;   /* §6.7 */
    act->host_n_banner_lines = APEX_ACT_DISPLAY_BANNER_LINES_DEFAULT;
    memset(act->display_used, 0, sizeof(act->display_used));      /* §6.8 */
    memset(act->display_last_ms, 0, sizeof(act->display_last_ms));
    act->capability_sent = false;    /* CAPABILITY re-emits after re-discovery */
    act->status_dirty = false;
    act->last_status_tx_ms = 0;
}

/* §7.4 safety-deferral hook installed on the core device cfg: a host
 * RESET_REQUEST is deferred while the class is EXECUTING (the action runs to
 * completion, never aborted by re-enumeration) and permitted from every other
 * state. Composed conservatively with any app-installed hook: BOTH must permit.
 * The core re-checks each tick, so a deferred reset is honored on the first
 * tick after the EXECUTING → ENABLED/EXHAUSTED transition. */
static bool dev_reenum_permitted_hook(void *user)
{
    apex_activation_device_t *act = (apex_activation_device_t *)user;
    if (act->state == APEX_ACTIVATION_STATE_EXECUTING) return false;
    if (act->app_reenum_permitted) {
        return act->app_reenum_permitted(act->app_reenum_permitted_user);
    }
    return true;
}

static void dev_arm_after_connect(apex_activation_device_t *act)
{
    /* Initial arming only: after a re-enumeration the machine has already left
     * STANDBY and its surviving state must not be disturbed (§7.4) — in
     * particular latched Valid preconditions must not be knocked back to
     * Running, and an EXHAUSTED device stays EXHAUSTED. */
    if (act->state != APEX_ACTIVATION_STATE_STANDBY) return;

    /* Apply the auto_start_mask: auto-started preconditions begin in Running. */
    for (uint8_t i = 0; i < act->caps.n_preconditions; i++) {
        if (act->caps.auto_start_mask & (uint16_t)(1u << i)) {
            act->precondition_states[i] = APEX_PRECOND_RUNNING;
        }
    }
    if (act->caps.n_preconditions == 0) {
        /* §3 STANDBY → READY with no preconditions. */
        dev_set_state(act, APEX_ACTIVATION_STATE_READY);
    } else if (act->caps.auto_start_mask != 0) {
        dev_set_state(act, APEX_ACTIVATION_STATE_VALIDATING);
        dev_maybe_advance_to_ready(act);
    }
}

/* Enter the enable transition from a state that has decided to enable. Returns
 * true if the on_enable_begin hook still needs firing (the caller fires it AFTER
 * sending the ACK, so the ACK truthfully reports ENABLING even if the app
 * completes the transition synchronously in the hook). NON-instant → ENABLING;
 * instant (no hook) → straight to ENABLED, clearing TRANSITION_FAILED. */
static bool dev_enter_enable(apex_activation_device_t *act)
{
    /* A fresh, host-commanded arm defines a new arming episode within the
     * current session — clear any self-disarm owed to an earlier lost one. */
    act->self_disarm_pending = false;
    if (act->hooks.on_enable_begin) {
        dev_set_state(act, APEX_ACTIVATION_STATE_ENABLING);
        return true;
    }
    dev_clear_transition_failed(act);
    dev_set_state(act, APEX_ACTIVATION_STATE_ENABLED);
    return false;
}

/* Symmetric to dev_enter_enable for the disable transition. NON-instant →
 * DISABLING; instant → straight to READY. Leaving ENABLED here is what enforces
 * the §3 safety invariant: once SET_DISABLED is accepted the device is no longer
 * in ENABLED, so no trigger can be honored from that moment. */
static bool dev_enter_disable(apex_activation_device_t *act)
{
    if (act->hooks.on_disable_begin) {
        dev_set_state(act, APEX_ACTIVATION_STATE_DISABLING);
        return true;
    }
    dev_clear_transition_failed(act);
    dev_set_state(act, APEX_ACTIVATION_STATE_READY);
    return false;
}

/* Self-issued SET_DISABLE on session loss (§7.4). Mirrors the device-side effect
 * of a host SET_DISABLED (dev_handle_set_disabled) but emits NO ACK — the session
 * is gone, there is no host to answer. Honored from ENABLED, and from ENABLING
 * where the device permits an abort; a no-op in every other state (DISABLING,
 * READY, EXHAUSTED, EXECUTING, and the pre-arm states). A device that self-issues
 * SET_DISABLE MUST NOT assume the resulting state: a non-instant device lands in
 * DISABLING and reports it in the first STATUS after re-discovery — only an
 * instant device (no on_disable_begin hook) reaches READY synchronously. This is
 * the safety default that keeps a successor host from inheriting a payload it did
 * not itself arm. States that cannot stand down here — ENABLING that cannot abort,
 * and EXECUTING — do not lose the disarm: the caller sets self_disarm_pending, and
 * transition_complete / complete_execution honor it when they land in ENABLED. */
static void dev_self_disable(apex_activation_device_t *act)
{
    bool fire = false;
    switch (act->state) {
    case APEX_ACTIVATION_STATE_ENABLED:
        fire = dev_enter_disable(act);
        break;
    case APEX_ACTIVATION_STATE_ENABLING:
        if (act->caps.can_abort_enabling) fire = dev_enter_disable(act);
        break;
    default:
        return;
    }
    if (fire) act->hooks.on_disable_begin(act->hooks.on_disable_begin_user);
}

void apex_activation_device_set_precondition_state(apex_activation_device_t *act,
                                                   uint8_t idx,
                                                   apex_precond_state_t new_state)
{
    if (!act || idx >= act->caps.n_preconditions) return;
    apex_precond_state_t cur = (apex_precond_state_t)act->precondition_states[idx];
    if (cur == APEX_PRECOND_VALID) return;  /* latched (§4.1) */
    if (cur == new_state) return;
    act->precondition_states[idx] = (uint8_t)new_state;
    act->status_dirty = true;

    if (new_state == APEX_PRECOND_RUNNING &&
        act->state == APEX_ACTIVATION_STATE_STANDBY) {
        dev_set_state(act, APEX_ACTIVATION_STATE_VALIDATING);
    }
    if (new_state == APEX_PRECOND_VALID) {
        dev_maybe_advance_to_ready(act);
    }
}

void apex_activation_device_trigger(apex_activation_device_t *act,
                                    uint8_t source_idx)
{
    if (!act) return;
    /* §3: triggers are honored only in ENABLED. In ENABLING/DISABLING (and after
     * an accepted SET_DISABLED) the device is not in ENABLED, so it does not
     * fire — this is the enforcement point for internal trigger sources. */
    if (act->state != APEX_ACTIVATION_STATE_ENABLED) return;
    if (source_idx >= act->caps.n_trigger_sources) return;
    act->last_trigger_source = source_idx;
    dev_set_state(act, APEX_ACTIVATION_STATE_EXECUTING);
    if (act->hooks.on_execute) {
        act->hooks.on_execute(act->hooks.on_execute_user);
    }
}

void apex_activation_device_complete_execution(apex_activation_device_t *act)
{
    if (!act) return;
    if (act->state != APEX_ACTIVATION_STATE_EXECUTING) return;
    if (act->activations_remaining != APEX_ACT_ACTIVATIONS_UNLIMITED &&
        act->activations_remaining > 0) {
        act->activations_remaining--;
    }
    if (act->activations_remaining == 0) {
        dev_set_state(act, APEX_ACTIVATION_STATE_EXHAUSTED);
    } else {
        dev_set_state(act, APEX_ACTIVATION_STATE_ENABLED);
        /* §7.4: EXECUTING runs to completion even across a session loss (a
         * RESET_REQUEST is deferred while EXECUTING; the watchdog is not). If a
         * session was lost during this action, the device owes a self-disarm
         * (self_disarm_pending) — honor it now rather than re-arm into the gap,
         * so no successor host inherits a payload it did not itself arm. This
         * holds even if a new session re-formed mid-action. An ordinary
         * multi-activation return to ENABLED (no lost session) leaves it armed. */
        if (act->self_disarm_pending) {
            dev_self_disable(act);
        }
    }
}

void apex_activation_device_transition_complete(apex_activation_device_t *act)
{
    if (!act) return;
    if (act->state == APEX_ACTIVATION_STATE_ENABLING) {
        dev_clear_transition_failed(act);        /* successful transition (§6.5) */
        dev_set_state(act, APEX_ACTIVATION_STATE_ENABLED);
        /* §7.4: an ENABLING that could not abort a mid-transition session loss
         * completes here into ENABLED still owing a self-disarm — honor it now
         * (symmetric with complete_execution) rather than re-arm into the gap. */
        if (act->self_disarm_pending) {
            dev_self_disable(act);
        }
    } else if (act->state == APEX_ACTIVATION_STATE_DISABLING) {
        dev_clear_transition_failed(act);
        dev_set_state(act, APEX_ACTIVATION_STATE_READY);
    }
}

void apex_activation_device_transition_failed(apex_activation_device_t *act)
{
    if (!act) return;
    if (act->state != APEX_ACTIVATION_STATE_ENABLING &&
        act->state != APEX_ACTIVATION_STATE_DISABLING) {
        return;
    }
    /* §3 / §6.5: safe failure returns to READY and sets self-clearing
     * TRANSITION_FAILED (bit 5). Does NOT force FAULT. */
    act->fault_flags |= APEX_ACT_FAULT_TRANSITION_FAILED;
    act->status_dirty = true;
    dev_set_state(act, APEX_ACTIVATION_STATE_READY);
}

void apex_activation_device_set_fault_flag(apex_activation_device_t *act,
                                           uint16_t flag)
{
    if (!act) return;
    act->fault_flags |= flag;
    act->status_dirty = true;
    if (flag & APEX_ACT_FAULT_LATCHED_MASK) {
        dev_set_state(act, APEX_ACTIVATION_STATE_FAULT);
    }
}

void apex_activation_device_clear_fault_flag(apex_activation_device_t *act,
                                             uint16_t flag)
{
    if (!act) return;
    /* §6.5: latched bits do not clear without a reset. */
    flag &= (uint16_t)~APEX_ACT_FAULT_LATCHED_MASK;
    if (flag && (act->fault_flags & flag)) {
        act->fault_flags &= (uint16_t)~flag;
        act->status_dirty = true;
    }
}

apex_status_t apex_activation_device_set_payload_specific(apex_activation_device_t *act,
                                                          const uint8_t *bytes,
                                                          size_t len)
{
    if (!act) return APEX_ERR_INVALID_ARGS;
    if (len > APEX_ACTIVATION_PAYLOAD_SPECIFIC_MAX) return APEX_ERR_BUFFER_TOO_SMALL;
    if (len > 0) memcpy(act->payload_specific, bytes, len);
    act->payload_specific_len = (uint8_t)len;
    act->status_dirty = true;
    return APEX_OK;
}

/* ---------------------------------------------------------------------------
 * Device-side command handling
 * ------------------------------------------------------------------------- */

static void dev_handle_start_precondition(apex_activation_device_t *act,
                                          const uint8_t *body, size_t body_len)
{
    if (body_len < 1) {
        dev_send_ack(act, APEX_ACT_CMD_START_PRECONDITION, APEX_ACT_REJECT_MALFORMED);
        return;
    }
    uint8_t idx = body[0];
    if (idx >= act->caps.n_preconditions) {
        dev_send_ack(act, APEX_ACT_CMD_START_PRECONDITION, APEX_ACT_REJECT_BAD_INDEX);
        return;
    }
    if (act->state != APEX_ACTIVATION_STATE_STANDBY &&
        act->state != APEX_ACTIVATION_STATE_VALIDATING) {
        dev_send_ack(act, APEX_ACT_CMD_START_PRECONDITION, APEX_ACT_REJECT_WRONG_STATE);
        return;
    }
    apex_precond_state_t cur = (apex_precond_state_t)act->precondition_states[idx];
    if (cur == APEX_PRECOND_VALID) {
        /* Idempotent: already validated. */
        dev_send_ack(act, APEX_ACT_CMD_START_PRECONDITION, APEX_ACT_ACCEPTED);
        return;
    }
    if (act->hooks.on_start_precondition) {
        apex_status_t r = act->hooks.on_start_precondition(
            act->hooks.on_start_precondition_user, idx);
        if (r != APEX_OK) {
            dev_send_ack(act, APEX_ACT_CMD_START_PRECONDITION, APEX_ACT_REJECT_PRECONDITION);
            return;
        }
    }
    /* Accept: bring this precondition to Running. */
    apex_activation_device_set_precondition_state(act, idx, APEX_PRECOND_RUNNING);
    dev_send_ack(act, APEX_ACT_CMD_START_PRECONDITION, APEX_ACT_ACCEPTED);
}

static void dev_handle_set_enabled(apex_activation_device_t *act)
{
    switch (act->state) {
    case APEX_ACTIVATION_STATE_READY: {
        /* READY → ENABLING (non-instant) or → ENABLED (instant). */
        bool fire = dev_enter_enable(act);
        dev_send_ack(act, APEX_ACT_CMD_SET_ENABLED, APEX_ACT_ACCEPTED);
        if (fire) act->hooks.on_enable_begin(act->hooks.on_enable_begin_user);
        return;
    }
    case APEX_ACTIVATION_STATE_ENABLING:
    case APEX_ACTIVATION_STATE_ENABLED:
        /* §5 idempotent no-op — the enable is already in place / in progress. */
        dev_send_ack(act, APEX_ACT_CMD_SET_ENABLED, APEX_ACT_ACCEPTED);
        return;
    case APEX_ACTIVATION_STATE_DISABLING:
        /* §5 reversal request back toward ENABLED — device-capability-dependent. */
        if (act->caps.can_reverse_disabling) {
            bool fire = dev_enter_enable(act);
            dev_send_ack(act, APEX_ACT_CMD_SET_ENABLED, APEX_ACT_ACCEPTED);
            if (fire) act->hooks.on_enable_begin(act->hooks.on_enable_begin_user);
        } else {
            dev_send_ack(act, APEX_ACT_CMD_SET_ENABLED, APEX_ACT_REJECT_BUSY);
        }
        return;
    default:
        dev_send_ack(act, APEX_ACT_CMD_SET_ENABLED, APEX_ACT_REJECT_WRONG_STATE);
        return;
    }
}

static void dev_handle_set_disabled(apex_activation_device_t *act)
{
    switch (act->state) {
    case APEX_ACTIVATION_STATE_ENABLED: {
        /* ENABLED → DISABLING (non-instant) or → READY (instant). Leaving ENABLED
         * enforces the §3 safety invariant (no trigger past this point). */
        bool fire = dev_enter_disable(act);
        dev_send_ack(act, APEX_ACT_CMD_SET_DISABLED, APEX_ACT_ACCEPTED);
        if (fire) act->hooks.on_disable_begin(act->hooks.on_disable_begin_user);
        return;
    }
    case APEX_ACTIVATION_STATE_DISABLING:
    case APEX_ACTIVATION_STATE_READY:
        /* §5 idempotent no-op (READY joins the accept set for retransmit safety). */
        dev_send_ack(act, APEX_ACT_CMD_SET_DISABLED, APEX_ACT_ACCEPTED);
        return;
    case APEX_ACTIVATION_STATE_ENABLING:
        /* §5 abort request back toward READY — device-capability-dependent. */
        if (act->caps.can_abort_enabling) {
            bool fire = dev_enter_disable(act);
            dev_send_ack(act, APEX_ACT_CMD_SET_DISABLED, APEX_ACT_ACCEPTED);
            if (fire) act->hooks.on_disable_begin(act->hooks.on_disable_begin_user);
        } else {
            dev_send_ack(act, APEX_ACT_CMD_SET_DISABLED, APEX_ACT_REJECT_BUSY);
        }
        return;
    default:
        dev_send_ack(act, APEX_ACT_CMD_SET_DISABLED, APEX_ACT_REJECT_WRONG_STATE);
        return;
    }
}

static void dev_handle_trigger(apex_activation_device_t *act)
{
    /* Locate a HOST_COMMAND trigger source (§4.2). */
    uint8_t host_cmd_idx = 0xFF;
    for (uint8_t s = 0; s < act->caps.n_trigger_sources; s++) {
        if (act->caps.trigger_source_categories[s] == APEX_TRIGGER_HOST_COMMAND) {
            host_cmd_idx = s;
            break;
        }
    }
    if (host_cmd_idx == 0xFF) {
        dev_send_ack(act, APEX_ACT_CMD_TRIGGER, APEX_ACT_REJECT_NOT_SUPPORTED);
        return;
    }
    if (act->state == APEX_ACTIVATION_STATE_EXECUTING) {
        /* Idempotent. */
        dev_send_ack(act, APEX_ACT_CMD_TRIGGER, APEX_ACT_ACCEPTED);
        return;
    }
    /* §3, §5: honored only in ENABLED — notably NOT in ENABLING. */
    if (act->state != APEX_ACTIVATION_STATE_ENABLED) {
        dev_send_ack(act, APEX_ACT_CMD_TRIGGER, APEX_ACT_REJECT_WRONG_STATE);
        return;
    }
    /* Send the ACK *before* firing on_execute so the host sees the EXECUTING
     * transition (§9.4 Step 7 — the ACK carries current_state=EXECUTING). */
    act->last_trigger_source = host_cmd_idx;
    dev_set_state(act, APEX_ACTIVATION_STATE_EXECUTING);
    dev_send_ack(act, APEX_ACT_CMD_TRIGGER, APEX_ACT_ACCEPTED);
    if (act->hooks.on_execute) {
        act->hooks.on_execute(act->hooks.on_execute_user);
    }
}

static void dev_handle_host_display_info(apex_activation_device_t *act,
                                         const uint8_t *body, size_t body_len)
{
    /* §6.7: char_limit(1) + n_banner_lines(1). A received char_limit of 0 means
     * 32; a received n_banner_lines of 0 means banner text unsupported (the
     * default of 1 applies only when the frame is never received). */
    if (body_len < 2) return;
    uint8_t char_limit = body[0];
    act->host_char_limit = (char_limit == 0)
                           ? APEX_ACT_DISPLAY_CHAR_LIMIT_DEFAULT : char_limit;
    act->host_n_banner_lines = body[1];
}

void apex_activation_device_on_rx(apex_activation_device_t *act,
                                  const uint8_t *payload,
                                  size_t payload_len)
{
    if (!act || payload_len < 1) return;
    uint8_t msg_id = payload[0];

    if (msg_id == APEX_ACT_MSG_HOST_DISPLAY_INFO) {
        dev_handle_host_display_info(act, payload + 1, payload_len - 1);
        return;
    }

    /* §6.1: silently ignore any class_msg_id we do not recognize (forward-
     * compatibility). This also covers the retired ids 5 / 6. */
    if (msg_id != APEX_ACT_MSG_COMMAND) return;
    if (payload_len < 2) {
        dev_send_ack(act, 0, APEX_ACT_REJECT_MALFORMED);
        return;
    }
    uint8_t cmd = payload[1];
    const uint8_t *body = payload + 2;
    size_t body_len = payload_len - 2;
    switch (cmd) {
    case APEX_ACT_CMD_START_PRECONDITION:
        dev_handle_start_precondition(act, body, body_len);
        break;
    case APEX_ACT_CMD_SET_ENABLED:
        dev_handle_set_enabled(act);
        break;
    case APEX_ACT_CMD_SET_DISABLED:
        dev_handle_set_disabled(act);
        break;
    case APEX_ACT_CMD_TRIGGER:
        dev_handle_trigger(act);
        break;
    default:
        dev_send_ack(act, cmd, APEX_ACT_REJECT_MALFORMED);
        break;
    }
}

/* ---------------------------------------------------------------------------
 * Device-side init + tick
 * ------------------------------------------------------------------------- */

apex_status_t apex_activation_device_init(apex_activation_device_t *act,
                                          apex_device_t *core,
                                          const apex_activation_device_caps_t *caps,
                                          const apex_activation_device_hooks_t *hooks)
{
    if (!act || !core || !caps) return APEX_ERR_INVALID_ARGS;
    if (caps->n_preconditions > APEX_ACTIVATION_MAX_PRECONDITIONS) return APEX_ERR_INVALID_ARGS;
    if (caps->n_trigger_sources == 0 ||
        caps->n_trigger_sources > APEX_ACTIVATION_MAX_TRIGGER_SOURCES) return APEX_ERR_INVALID_ARGS;
    if (caps->n_gpio_bindings > APEX_ACTIVATION_MAX_PRECONDITIONS) return APEX_ERR_INVALID_ARGS;
    if (caps->n_host_conditions > APEX_ACTIVATION_MAX_PRECONDITIONS) return APEX_ERR_INVALID_ARGS;
    if (caps->n_gpio_bindings > 0 && (!hooks || !hooks->gpio_read)) return APEX_ERR_INVALID_ARGS;
    for (uint8_t b = 0; b < caps->n_gpio_bindings; b++) {
        const apex_activation_gpio_binding_t *g = &caps->gpio_bindings[b];
        if (g->precondition_idx >= caps->n_preconditions) return APEX_ERR_INVALID_ARGS;
        if (g->pin != APEX_ACTIVATION_GPIO_PIN3 &&
            g->pin != APEX_ACTIVATION_GPIO_PIN4) return APEX_ERR_INVALID_ARGS;
    }
    for (uint8_t p = 0; p < caps->n_host_conditions; p++) {
        if (caps->host_conditions[p].precondition_idx >= caps->n_preconditions) {
            return APEX_ERR_INVALID_ARGS;
        }
    }
    /* Capture any app-installed reenum_permitted hook BEFORE memset so it can
     * be composed (§7.4, both-must-permit). If the core was already wired by a
     * previous apex_activation_device_init (re-init over the same core),
     * recover the original app hook from the previous wrapper's user pointer
     * instead of composing with our own wrapper. */
    bool (*app_hook)(void *) = core->cfg.reenum_permitted;
    void *app_user = core->cfg.reenum_permitted_user;
    if (app_hook == dev_reenum_permitted_hook) {
        const apex_activation_device_t *prev =
            (const apex_activation_device_t *)app_user;
        app_hook = prev->app_reenum_permitted;
        app_user = prev->app_reenum_permitted_user;
    }

    memset(act, 0, sizeof(*act));
    act->core = core;
    act->caps = *caps;
    if (hooks) act->hooks = *hooks;
    act->state = APEX_ACTIVATION_STATE_STANDBY;
    act->activations_remaining = caps->initial_activations_remaining;
    act->last_trigger_source = 0xFF;
    act->host_char_limit = APEX_ACT_DISPLAY_CHAR_LIMIT_DEFAULT;
    act->host_n_banner_lines = APEX_ACT_DISPLAY_BANNER_LINES_DEFAULT;
    act->last_link = APEX_DEVICE_STATE_DISCOVERING;

    /* §7.4 wiring: install the class deferral hook on the core cfg (stored by
     * value in apex_device_t, so mutating it here is the composition point). */
    act->app_reenum_permitted = app_hook;
    act->app_reenum_permitted_user = app_user;
    core->cfg.reenum_permitted = dev_reenum_permitted_hook;
    core->cfg.reenum_permitted_user = act;
    return APEX_OK;
}

/* Poll every GPIO-backed precondition that is currently Running and latch it to
 * Valid once its line has held the active level for the binding's stable_ms.
 * §4.1: validation only proceeds while a precondition is Running, so auto/host
 * start gating is honored without extra logic. */
static void dev_poll_gpio_preconditions(apex_activation_device_t *act)
{
    if (!act->hooks.gpio_read) return;
    for (uint8_t b = 0; b < act->caps.n_gpio_bindings; b++) {
        const apex_activation_gpio_binding_t *g = &act->caps.gpio_bindings[b];
        uint8_t idx = g->precondition_idx;
        if (act->precondition_states[idx] != APEX_PRECOND_RUNNING) {
            act->gpio_active_timing[b] = false;
            continue;
        }
        bool level = act->hooks.gpio_read(act->hooks.gpio_read_user, g->pin);
        bool active = (level == g->active_high);
        if (!active) {
            act->gpio_active_timing[b] = false;
            continue;
        }
        if (!act->gpio_active_timing[b]) {
            act->gpio_active_timing[b] = true;
            act->gpio_active_since_ms[b] = act->now_ms;
        }
        if ((uint32_t)(act->now_ms - act->gpio_active_since_ms[b]) >= g->stable_ms) {
            apex_activation_device_set_precondition_state(act, idx, APEX_PRECOND_VALID);
        }
    }
}

void apex_activation_device_tick(apex_activation_device_t *act, uint32_t now_ms)
{
    if (!act) return;
    act->now_ms = now_ms;

    apex_device_link_state_t link = apex_device_link_state(act->core);
    if (link != act->last_link) {
        if (link == APEX_DEVICE_STATE_CONNECTED ||
            act->last_link == APEX_DEVICE_STATE_CONNECTED) {
            /* Session boundary (re-enumeration / watchdog / reconnect).
             * Leaving CONNECTED is a session loss: the device self-issues
             * SET_DISABLE (§7.4) so an armed payload is stood down and no
             * successor host inherits a state it did not itself set. The
             * resulting state is reported in the first STATUS after
             * re-discovery, never assumed. Device-internal state that is NOT
             * the armed region — latched preconditions, activations_remaining,
             * EXHAUSTED, fault flags — still survives; only host-session
             * artifacts are cleared here. CAPABILITY re-emits below once
             * CONNECTED again. */
            if (act->last_link == APEX_DEVICE_STATE_CONNECTED) {
                /* Disarm now if in an armed state; if the armed region can't be
                 * left yet (EXECUTING runs to completion), owe the disarm so it
                 * fires when the action completes into ENABLED — even if a new
                 * session has formed by then. Cleared by a fresh SET_ENABLED. */
                act->self_disarm_pending = true;
                dev_self_disable(act);
            }
            dev_reset_session_state(act);
        }
        act->last_link = link;
    }

    if (link != APEX_DEVICE_STATE_CONNECTED) return;

    /* §6.3: CAPABILITY is emitted unprompted immediately after the class becomes
     * active. */
    if (!act->capability_sent) {
        dev_send_capability(act);
        if (!act->capability_sent) return;  /* TX failed — try next tick */
        dev_arm_after_connect(act);
        act->status_dirty = true;  /* first STATUS so the host has something to read */
    }

    dev_poll_gpio_preconditions(act);

    bool periodic_due =
        act->last_status_tx_ms == 0 ||
        (uint32_t)(now_ms - act->last_status_tx_ms) >= APEX_ACTIVATION_STATUS_PERIOD_MS;
    if (act->status_dirty || periodic_due) {
        dev_send_status(act);
    }
}

/* ===========================================================================
 * Host-side
 * ========================================================================= */

static void host_handle_capability(apex_activation_host_t *h,
                                   uint8_t device_id,
                                   const uint8_t *body, size_t body_len)
{
    /* §6.3 (class version 1 — NO class_spec_version byte):
     * uuid(16) + n_preconditions(1) + n_trigger_sources(1) +
     * categories(n_trigger_sources) + optional tails. */
    if (body_len < 16 + 1 + 1) return;
    apex_activation_capability_t cap;
    memset(&cap, 0, sizeof(cap));
    size_t off = 0;
    memcpy(cap.payload_type_uuid, body + off, 16);
    off += 16;
    cap.n_preconditions = body[off++];
    cap.n_trigger_sources = body[off++];
    if (cap.n_trigger_sources > APEX_ACTIVATION_MAX_TRIGGER_SOURCES) return;
    if (body_len < off + cap.n_trigger_sources) return;
    for (uint8_t i = 0; i < cap.n_trigger_sources; i++) {
        cap.trigger_source_categories[i] = body[off + i];
    }
    off += cap.n_trigger_sources;
    /* GPIO-backed precondition mappings — optional tail. */
    if (body_len >= off + 1) {
        uint8_t ng = body[off++];
        if (ng > APEX_ACTIVATION_MAX_PRECONDITIONS) return;
        if (body_len < off + (size_t)ng * 2) return;
        cap.n_gpio_bindings = ng;
        for (uint8_t i = 0; i < ng; i++) {
            cap.gpio_bindings[i].precondition_idx = body[off++];
            uint8_t pin_byte = body[off++];
            cap.gpio_bindings[i].pin = pin_byte & APEX_ACTIVATION_GPIO_PIN_MASK;
            cap.gpio_bindings[i].active_high =
                (pin_byte & APEX_ACTIVATION_GPIO_ACTIVE_HIGH_BIT) != 0;
        }
    }
    /* GPIO-backed trigger source mappings — optional tail. */
    if (body_len >= off + 1) {
        uint8_t nt = body[off++];
        if (nt > APEX_ACTIVATION_MAX_TRIGGER_SOURCES) return;
        if (body_len < off + (size_t)nt * 2) return;
        cap.n_gpio_trigger_bindings = nt;
        for (uint8_t i = 0; i < nt; i++) {
            cap.gpio_trigger_bindings[i].trigger_source_idx = body[off++];
            uint8_t pin_byte = body[off++];
            cap.gpio_trigger_bindings[i].pin = pin_byte & APEX_ACTIVATION_GPIO_PIN_MASK;
            cap.gpio_trigger_bindings[i].active_high =
                (pin_byte & APEX_ACTIVATION_GPIO_ACTIVE_HIGH_BIT) != 0;
        }
    }
    /* Host-condition bindings — optional tail (§6.3). 5 bytes each. */
    if (body_len >= off + 1) {
        uint8_t nhc = body[off++];
        if (nhc > APEX_ACTIVATION_MAX_PRECONDITIONS) return;
        if (body_len < off + (size_t)nhc * 5) return;
        cap.n_host_conditions = nhc;
        for (uint8_t i = 0; i < nhc; i++) {
            cap.host_conditions[i].precondition_idx = body[off++];
            cap.host_conditions[i].host_condition   = body[off++];
            cap.host_conditions[i].condition_param =
                (uint16_t)body[off] | ((uint16_t)body[off + 1] << 8);
            off += 2;
            cap.host_conditions[i].auto_trigger     = body[off++];
        }
    }
    /* Timing tail — optional, both-or-neither (§6.3). Absent ⇒ both 0. */
    if (body_len >= off + 4) {
        cap.enable_time_ms =
            (uint16_t)body[off] | ((uint16_t)body[off + 1] << 8);
        off += 2;
        cap.disable_time_ms =
            (uint16_t)body[off] | ((uint16_t)body[off + 1] << 8);
        off += 2;
    }

    h->n_precond_per_device[device_id] = cap.n_preconditions;
    if (h->hooks.on_capability) {
        h->hooks.on_capability(h->hooks.on_capability_user, device_id, &cap);
    }
}

static void host_handle_status(apex_activation_host_t *h,
                               uint8_t device_id,
                               const uint8_t *body, size_t body_len)
{
    /* §6.5: state(1) + activations_remaining(1) + last_trigger_source(1) +
     * fault_flags(2) + precondition_states(n) + payload_specific(?). */
    if (body_len < 5) return;
    uint8_t n_precond = h->n_precond_per_device[device_id];
    if (n_precond > APEX_ACTIVATION_MAX_PRECONDITIONS) return;
    if (body_len < (size_t)(5 + n_precond)) return;

    apex_activation_status_t st;
    memset(&st, 0, sizeof(st));
    size_t off = 0;
    st.state                 = (apex_activation_state_t)body[off++];
    st.activations_remaining = body[off++];
    st.last_trigger_source   = body[off++];
    st.fault_flags = (uint16_t)body[off] | ((uint16_t)body[off + 1] << 8);
    off += 2;
    st.n_preconditions = n_precond;
    for (uint8_t i = 0; i < n_precond; i++) {
        st.precondition_states[i] = body[off + i];
    }
    off += n_precond;
    if (body_len > off) {
        st.payload_specific = body + off;
        st.payload_specific_len = body_len - off;
    }

    /* §7.3: EXHAUSTED activation state drives the core lifecycle to EXPENDED. */
    if (st.state == APEX_ACTIVATION_STATE_EXHAUSTED && h->core) {
        apex_host_mark_expended(h->core, device_id);
    }

    if (h->hooks.on_status) {
        h->hooks.on_status(h->hooks.on_status_user, device_id, &st);
    }
}

static void host_handle_ack(apex_activation_host_t *h,
                            uint8_t device_id,
                            const uint8_t *body, size_t body_len)
{
    if (body_len < 3) return;
    apex_activation_ack_t ack;
    ack.acked_command  = body[0];
    ack.result         = (apex_activation_ack_result_t)body[1];
    ack.current_state  = (apex_activation_state_t)body[2];
    if (h->hooks.on_ack) {
        h->hooks.on_ack(h->hooks.on_ack_user, device_id, &ack);
    }
}

static void host_handle_display_text(apex_activation_host_t *h,
                                     uint8_t device_id,
                                     const uint8_t *body, size_t body_len)
{
    /* §6.8: target(1) + text_len(1) + text(text_len). */
    if (body_len < 2) return;
    uint8_t target = body[0];
    uint8_t text_len = body[1];
    if (body_len < (size_t)(2 + text_len)) return;  /* malformed → ignore */
    const char *text = (text_len > 0) ? (const char *)(body + 2) : NULL;
    if (h->hooks.on_display_text) {
        h->hooks.on_display_text(h->hooks.on_display_text_user, device_id,
                                 target, text, text_len);
    }
}

static void host_class_rx(void *user, uint8_t device_id,
                          const uint8_t *payload, size_t payload_len)
{
    apex_activation_host_t *h = (apex_activation_host_t *)user;
    if (payload_len < 1) return;
    uint8_t class_msg_id = payload[0];
    const uint8_t *body = payload + 1;
    size_t body_len = payload_len - 1;
    switch (class_msg_id) {
    case APEX_ACT_MSG_CAPABILITY:
        host_handle_capability(h, device_id, body, body_len);
        break;
    case APEX_ACT_MSG_STATUS:
        host_handle_status(h, device_id, body, body_len);
        break;
    case APEX_ACT_MSG_ACK:
        host_handle_ack(h, device_id, body, body_len);
        break;
    case APEX_ACT_MSG_DISPLAY_TEXT:
        host_handle_display_text(h, device_id, body, body_len);
        break;
    default:
        /* §6.1: silently ignore unrecognized ids (incl. retired 5 / 6). */
        break;
    }
}

apex_status_t apex_activation_host_init(apex_activation_host_t *h,
                                        apex_host_t *core,
                                        const apex_activation_host_hooks_t *hooks)
{
    if (!h || !core) return APEX_ERR_INVALID_ARGS;
    memset(h, 0, sizeof(*h));
    h->core = core;
    if (hooks) h->hooks = *hooks;
    return apex_host_register_class(core, APEX_TRAFFIC_ACTIVATION,
                                    host_class_rx, h);
}

/* Outbound command helpers. */
static apex_status_t host_send_cmd(apex_activation_host_t *h,
                                   uint8_t device_id,
                                   uint8_t cmd,
                                   const uint8_t *arg, size_t arg_len)
{
    uint8_t buf[8];
    if (arg_len > sizeof(buf) - 2) return APEX_ERR_INVALID_ARGS;
    size_t i = 0;
    buf[i++] = APEX_ACT_MSG_COMMAND;
    buf[i++] = cmd;
    if (arg_len > 0) {
        memcpy(buf + i, arg, arg_len);
        i += arg_len;
    }
    return apex_host_send(h->core, device_id, APEX_TRAFFIC_ACTIVATION, buf, i);
}

apex_status_t apex_activation_host_start_precondition(apex_activation_host_t *h,
                                                      uint8_t device_id,
                                                      uint8_t precondition_idx)
{
    return host_send_cmd(h, device_id, APEX_ACT_CMD_START_PRECONDITION,
                         &precondition_idx, 1);
}

apex_status_t apex_activation_host_set_enabled(apex_activation_host_t *h,
                                               uint8_t device_id)
{
    return host_send_cmd(h, device_id, APEX_ACT_CMD_SET_ENABLED, NULL, 0);
}

apex_status_t apex_activation_host_set_disabled(apex_activation_host_t *h,
                                                uint8_t device_id)
{
    return host_send_cmd(h, device_id, APEX_ACT_CMD_SET_DISABLED, NULL, 0);
}

apex_status_t apex_activation_host_trigger(apex_activation_host_t *h,
                                           uint8_t device_id)
{
    return host_send_cmd(h, device_id, APEX_ACT_CMD_TRIGGER, NULL, 0);
}

apex_status_t apex_activation_host_send_display_info(apex_activation_host_t *h,
                                                     uint8_t device_id,
                                                     uint8_t char_limit,
                                                     uint8_t n_banner_lines)
{
    uint8_t buf[3] = {
        APEX_ACT_MSG_HOST_DISPLAY_INFO,
        char_limit,
        n_banner_lines,
    };
    return apex_host_send(h->core, device_id, APEX_TRAFFIC_ACTIVATION,
                          buf, sizeof(buf));
}
