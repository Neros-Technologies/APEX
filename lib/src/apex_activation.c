/**
 * @file apex_activation.c
 * @brief Activation device class — both Host and Device sides.
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
    /* class_msg_id + class_spec_version + uuid(16) + n_preconditions +
     * n_trigger_sources + categories(N) +
     * n_gpio_bindings + bindings(2 each) +
     * n_gpio_trigger_bindings + trigger_bindings(2 each). */
    uint8_t buf[1 + 1 + 16 + 1 + 1 + APEX_ACTIVATION_MAX_TRIGGER_SOURCES +
                1 + 2 * APEX_ACTIVATION_MAX_PRECONDITIONS +
                1 + 2 * APEX_ACTIVATION_MAX_TRIGGER_SOURCES +
                1 + 5 * APEX_ACTIVATION_MAX_PRECONDITIONS];
    size_t i = 0;
    buf[i++] = APEX_ACT_MSG_CAPABILITY;
    buf[i++] = act->caps.class_spec_version;
    memcpy(buf + i, act->caps.payload_type_uuid, 16);
    i += 16;
    buf[i++] = act->caps.n_preconditions;
    buf[i++] = act->caps.n_trigger_sources;
    for (uint8_t s = 0; s < act->caps.n_trigger_sources; s++) {
        buf[i++] = act->caps.trigger_source_categories[s];
    }
    /* GPIO-backed precondition mappings (§6.3): which precondition each GPIO
     * line validates. Active level / debounce stay device-internal. */
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
    /* GPIO-backed trigger source mappings (§6.3): optional tail. */
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
    /* Host-condition bindings (§6.3): for each host-evaluated precondition
     * (host_condition != NONE), declare the gating flight condition + auto_trigger
     * so the host can drive START_PRECONDITION from CAPABILITY + STATUS alone,
     * without depending on the optional PRECOND_INFO exchange (§6.6). Sparse:
     * device-local / auto-started preconditions (host_condition == NONE) are omitted. */
    size_t nhc_pos = i++;          /* reserve the count byte */
    uint8_t nhc = 0;
    if (act->caps.precond_info) {
        for (uint8_t p = 0; p < act->caps.n_preconditions; p++) {
            const apex_activation_precond_info_t *info = &act->caps.precond_info[p];
            if (info->host_condition == APEX_ACT_PRECOND_NONE) continue;
            buf[i++] = p;
            buf[i++] = info->host_condition;
            buf[i++] = (uint8_t)(info->condition_param & 0xFFu);
            buf[i++] = (uint8_t)(info->condition_param >> 8);
            buf[i++] = info->auto_trigger;
            nhc++;
        }
    }
    buf[nhc_pos] = nhc;

    if (dev_send(act, buf, i) == APEX_OK) {
        act->capability_sent = true;
    }
}

static void dev_send_status(apex_activation_device_t *act)
{
    /* Header: msg_id + state + activations_remaining + last_trigger_source
     * + fault_flags(u16) = 6 bytes, plus n_preconditions + payload_specific. */
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

static void dev_send_precond_info_reply(apex_activation_device_t *act,
                                        uint8_t precond_idx,
                                        uint8_t display_char_limit)
{
    if (!act->caps.precond_info) return;
    if (precond_idx >= act->caps.n_preconditions) return;

    const apex_activation_precond_info_t *info = &act->caps.precond_info[precond_idx];
    /* Effective per-string limit: request value if non-zero, else the spec max. */
    uint8_t max_len = (display_char_limit > 0 && display_char_limit <= APEX_ACT_PRECOND_STR_MAX)
                      ? display_char_limit : APEX_ACT_PRECOND_STR_MAX;

    /* class_msg_id(1) + precond_idx(1) + 4x(len_byte + up to max_len chars).
     * Display strings only — the host_condition/auto_trigger hint moved to the
     * CAPABILITY frame (§6.3) so it is reliably delivered; PRECOND_INFO is now
     * purely cosmetic and non-blocking (§6.6). */
    uint8_t buf[2 + 4 * (1 + APEX_ACT_PRECOND_STR_MAX)];
    size_t i = 0;

    buf[i++] = APEX_ACT_MSG_PRECOND_INFO_REPLY;
    buf[i++] = precond_idx;

    const char *strs[4] = {
        info->str_not_started, info->str_running,
        info->str_valid,       info->str_failed,
    };
    for (int s = 0; s < 4; s++) {
        const char *str = strs[s];
        uint8_t len = 0;
        if (str) {
            while (str[len] && len < max_len) len++;
        }
        buf[i++] = len;
        for (uint8_t c = 0; c < len; c++) buf[i++] = (uint8_t)str[c];
    }
    dev_send(act, buf, i);
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

static void dev_reset_class_state(apex_activation_device_t *act)
{
    /* Called when the core link drops. Re-arm everything. */
    act->state = APEX_ACTIVATION_STATE_STANDBY;
    act->activations_remaining = act->caps.initial_activations_remaining;
    act->last_trigger_source = 0xFF;
    act->fault_flags = 0;
    memset(act->precondition_states, APEX_PRECOND_NOT_STARTED,
           sizeof(act->precondition_states));
    memset(act->gpio_active_timing, 0, sizeof(act->gpio_active_timing));
    memset(act->gpio_active_since_ms, 0, sizeof(act->gpio_active_since_ms));
    act->capability_sent = false;
    act->status_dirty = false;
    act->last_status_tx_ms = 0;
}

static void dev_arm_after_connect(apex_activation_device_t *act)
{
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
    }
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
    /* Spec §6.5: latched bits do not clear without a reset. */
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

void apex_activation_device_set_precond_info(apex_activation_device_t *act,
                                             const apex_activation_precond_info_t *info)
{
    if (!act) return;
    act->caps.precond_info = info;
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
    if (act->state == APEX_ACTIVATION_STATE_ENABLED) {
        /* §5 idempotency. */
        dev_send_ack(act, APEX_ACT_CMD_SET_ENABLED, APEX_ACT_ACCEPTED);
        return;
    }
    if (act->state != APEX_ACTIVATION_STATE_READY) {
        dev_send_ack(act, APEX_ACT_CMD_SET_ENABLED, APEX_ACT_REJECT_WRONG_STATE);
        return;
    }
    dev_set_state(act, APEX_ACTIVATION_STATE_ENABLED);
    dev_send_ack(act, APEX_ACT_CMD_SET_ENABLED, APEX_ACT_ACCEPTED);
}

static void dev_handle_set_disabled(apex_activation_device_t *act)
{
    if (act->state == APEX_ACTIVATION_STATE_READY) {
        dev_send_ack(act, APEX_ACT_CMD_SET_DISABLED, APEX_ACT_ACCEPTED);
        return;
    }
    if (act->state != APEX_ACTIVATION_STATE_ENABLED) {
        dev_send_ack(act, APEX_ACT_CMD_SET_DISABLED, APEX_ACT_REJECT_WRONG_STATE);
        return;
    }
    dev_set_state(act, APEX_ACTIVATION_STATE_READY);
    dev_send_ack(act, APEX_ACT_CMD_SET_DISABLED, APEX_ACT_ACCEPTED);
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

void apex_activation_device_on_rx(apex_activation_device_t *act,
                                  const uint8_t *payload,
                                  size_t payload_len)
{
    if (!act || payload_len < 1) return;
    uint8_t msg_id = payload[0];

    /* Handle PRECOND_INFO_REQUEST (no ACK required). */
    if (msg_id == APEX_ACT_MSG_PRECOND_INFO_REQUEST) {
        if (payload_len >= 3) {
            dev_send_precond_info_reply(act, payload[1], payload[2]);
        }
        return;
    }

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
    if (caps->n_gpio_bindings > 0 && (!hooks || !hooks->gpio_read)) return APEX_ERR_INVALID_ARGS;
    for (uint8_t b = 0; b < caps->n_gpio_bindings; b++) {
        const apex_activation_gpio_binding_t *g = &caps->gpio_bindings[b];
        if (g->precondition_idx >= caps->n_preconditions) return APEX_ERR_INVALID_ARGS;
        if (g->pin != APEX_ACTIVATION_GPIO_PIN3 &&
            g->pin != APEX_ACTIVATION_GPIO_PIN4) return APEX_ERR_INVALID_ARGS;
    }
    memset(act, 0, sizeof(*act));
    act->core = core;
    act->caps = *caps;
    if (hooks) act->hooks = *hooks;
    act->state = APEX_ACTIVATION_STATE_STANDBY;
    act->activations_remaining = caps->initial_activations_remaining;
    act->last_trigger_source = 0xFF;
    act->last_link = APEX_DEVICE_STATE_DISCOVERING;
    return APEX_OK;
}

/* Poll every GPIO-backed precondition that is currently Running and latch it to
 * Valid once its line has held the active level for the binding's stable_ms.
 * §4.1: validation only proceeds while a precondition is Running, so auto-start
 * / host-start gating is honored without any extra logic here. */
static void dev_poll_gpio_preconditions(apex_activation_device_t *act)
{
    if (!act->hooks.gpio_read) return;
    for (uint8_t b = 0; b < act->caps.n_gpio_bindings; b++) {
        const apex_activation_gpio_binding_t *g = &act->caps.gpio_bindings[b];
        uint8_t idx = g->precondition_idx;
        if (act->precondition_states[idx] != APEX_PRECOND_RUNNING) {
            /* Not validating (yet, or already latched/failed): drop any streak. */
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
        if (link == APEX_DEVICE_STATE_CONNECTED) {
            dev_reset_class_state(act);
        } else if (act->last_link == APEX_DEVICE_STATE_CONNECTED) {
            /* Link went away; come back into STANDBY on reconnect. */
            dev_reset_class_state(act);
        }
        act->last_link = link;
    }

    if (link != APEX_DEVICE_STATE_CONNECTED) return;

    /* §6.3: CAPABILITY is emitted unprompted immediately after the class
     * becomes active. */
    if (!act->capability_sent) {
        dev_send_capability(act);
        if (!act->capability_sent) return;  /* TX failed — try next tick */
        dev_arm_after_connect(act);
        /* Send the first STATUS so the host has something to read. */
        act->status_dirty = true;
    }

    /* Drive GPIO-backed precondition validation before deciding whether to emit
     * STATUS, so a latch this tick flushes promptly via status_dirty. */
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
    /* class_spec_version(1) + uuid(16) + n_preconditions(1) +
     * n_trigger_sources(1) + categories(n_trigger_sources) +
     * n_gpio_bindings(1) + gpio_bindings(2 each). */
    if (body_len < 1 + 16 + 1 + 1) return;
    apex_activation_capability_t cap;
    memset(&cap, 0, sizeof(cap));
    size_t off = 0;
    cap.class_spec_version = body[off++];
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
    /* GPIO-backed trigger source mappings — optional tail after precond bindings. */
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
    /* Host-condition bindings — optional tail (§6.3). 5 bytes each:
     * precondition_idx, host_condition, condition_param(2), auto_trigger. */
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
    h->n_precond_per_device[device_id] = cap.n_preconditions;
    /* Fresh session: drop any stale outstanding PRECOND_INFO requests. The
     * integrator re-requests in its on_capability hook below, which re-arms them. */
    h->precond_info_pending[device_id] = 0;
    if (h->hooks.on_capability) {
        h->hooks.on_capability(h->hooks.on_capability_user, device_id, &cap);
    }
}

static void host_handle_status(apex_activation_host_t *h,
                               uint8_t device_id,
                               const uint8_t *body, size_t body_len)
{
    /* state(1) + activations_remaining(1) + last_trigger_source(1) +
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

    /* §7.3: when device reports EXHAUSTED, host transitions core lifecycle to
     * EXPENDED. */
    if (st.state == APEX_ACTIVATION_STATE_EXHAUSTED && h->core) {
        apex_host_mark_expended(h->core, device_id);
    }

    if (h->hooks.on_status) {
        h->hooks.on_status(h->hooks.on_status_user, device_id, &st);
    }

    /* Re-request any PRECOND_INFO whose reply hasn't arrived yet. STATUS is
     * emitted ≥1 Hz, so a dropped PRECOND_INFO_REPLY self-heals within ~1 s.
     * Display-only; never gates validation (§6.6). */
    uint16_t pending = h->precond_info_pending[device_id];
    for (uint8_t idx = 0; pending && idx < APEX_ACTIVATION_MAX_PRECONDITIONS; idx++) {
        if (pending & (uint16_t)(1u << idx)) {
            apex_activation_host_request_precond_info(
                h, device_id, idx, h->precond_info_char_limit);
        }
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

static void host_handle_precond_info_reply(apex_activation_host_t *h,
                                           uint8_t device_id,
                                           const uint8_t *body, size_t body_len)
{
    /* precond_idx(1) + 4 length bytes minimum = 5 bytes. PRECOND_INFO_REPLY now
     * carries only the display strings; the host_condition/auto_trigger hint
     * moved to CAPABILITY (§6.3). */
    if (body_len < 5) return;

    size_t off = 0;
    uint8_t precond_idx   = body[off++];
    /* Reply landed — clear the outstanding-request bit so STATUS retry stops. */
    if (precond_idx < APEX_ACTIVATION_MAX_PRECONDITIONS) {
        h->precond_info_pending[device_id] &= (uint16_t)~(1u << precond_idx);
    }
    apex_activation_precond_info_t info;
    /* Hint fields are no longer on the wire here — the host reads them from
     * apex_activation_capability_t.host_conditions. Zero them so a consumer that
     * inspects this struct can't read stale values. */
    info.host_condition   = APEX_ACT_PRECOND_NONE;
    info.condition_param  = 0;
    info.auto_trigger     = 0;

    /* Parse four length-prefixed strings. They are NOT NUL-terminated on the
     * wire (each is immediately followed by the next string's length byte), so
     * copy each into a NUL-terminated stack buffer before exposing it to the
     * callback — the apex_activation_precond_info_t str_* fields are documented
     * as C-strings valid for the duration of the (synchronous) callback. The
     * buffers live until on_precond_info returns, which matches that contract. */
    char strbuf[4][APEX_ACT_PRECOND_STR_MAX + 1];
    const char *strs[4] = { NULL, NULL, NULL, NULL };
    for (int s = 0; s < 4; s++) {
        if (off >= body_len) return;
        uint8_t len = body[off++];
        if (off + len > body_len) return;
        uint8_t copy = (len <= APEX_ACT_PRECOND_STR_MAX) ? len : APEX_ACT_PRECOND_STR_MAX;
        memcpy(strbuf[s], body + off, copy);
        strbuf[s][copy] = '\0';
        strs[s] = (len > 0) ? strbuf[s] : NULL;
        off += len;
    }
    info.str_not_started = strs[0];
    info.str_running     = strs[1];
    info.str_valid       = strs[2];
    info.str_failed      = strs[3];

    if (h->hooks.on_precond_info) {
        h->hooks.on_precond_info(h->hooks.on_precond_info_user,
                                 device_id, precond_idx, &info);
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
    case APEX_ACT_MSG_PRECOND_INFO_REPLY:
        host_handle_precond_info_reply(h, device_id, body, body_len);
        break;
    default:
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

apex_status_t apex_activation_host_request_precond_info(
    apex_activation_host_t *h,
    uint8_t device_id,
    uint8_t precondition_idx,
    uint8_t display_char_limit)
{
    /* Mark this index outstanding so STATUS-driven retry re-requests it until
     * the reply lands; remember the char limit for those re-requests. */
    if (precondition_idx < APEX_ACTIVATION_MAX_PRECONDITIONS) {
        h->precond_info_pending[device_id] |= (uint16_t)(1u << precondition_idx);
    }
    h->precond_info_char_limit = display_char_limit;
    uint8_t buf[3] = {
        APEX_ACT_MSG_PRECOND_INFO_REQUEST,
        precondition_idx,
        display_char_limit,
    };
    return apex_host_send(h->core, device_id, APEX_TRAFFIC_ACTIVATION,
                          buf, sizeof(buf));
}
