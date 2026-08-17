/**
 * @file apex_host.c
 * @brief Host-side core (APEX wire v1): device table, discovery, the
 *        provisional configuration phase, heartbeat/watchdog, HOST_STATE
 *        broadcast, baud negotiation, and VERSION_BEACON emission.
 *
 * Copyright (c) 2026 Neros Technologies. MIT License — see LICENSE.
 */
#include <string.h>

#include "apex/apex_host.h"

/* ---------------------------------------------------------------------------
 * Little-endian parse helpers
 * ------------------------------------------------------------------------- */

static uint16_t get_u16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static int16_t get_i16(const uint8_t *p)
{
    return (int16_t)get_u16(p);
}

static int32_t get_i32(const uint8_t *p)
{
    uint32_t u = (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
                 ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
    return (int32_t)u;
}

/* ---------------------------------------------------------------------------
 * Slot / class table helpers
 * ------------------------------------------------------------------------- */

static apex_host_device_slot_t *find_slot(apex_host_t *h, uint8_t device_id)
{
    if (device_id == APEX_DEVICE_ID_UNASSIGNED ||
        device_id == APEX_DEVICE_ID_BROADCAST) return NULL;
    for (size_t i = 0; i < APEX_HOST_MAX_DEVICES; i++) {
        if (h->devices[i].device_id == device_id) return &h->devices[i];
    }
    return NULL;
}

static apex_host_device_slot_t *find_free_slot(apex_host_t *h)
{
    for (size_t i = 0; i < APEX_HOST_MAX_DEVICES; i++) {
        if (h->devices[i].device_id == APEX_DEVICE_ID_UNASSIGNED) {
            return &h->devices[i];
        }
    }
    return NULL;
}

static const apex_host_class_reg_t *find_class(const apex_host_t *h,
                                               uint8_t traffic_type)
{
    for (size_t i = 0; i < APEX_HOST_MAX_CLASSES; i++) {
        if (h->classes[i].registered &&
            h->classes[i].traffic_type == traffic_type) {
            return &h->classes[i];
        }
    }
    return NULL;
}

static void set_status(apex_host_t *h,
                       apex_host_device_slot_t *slot,
                       apex_device_status_t s)
{
    if (slot->status == s) return;
    slot->status = s;
    slot->status_since_ms = h->now_ms;
    if (h->cfg.on_device_event) {
        h->cfg.on_device_event(h->cfg.on_device_event_user, slot->device_id, s);
    }
}

/* Per-port link state: a port is linked while a PROVISIONAL or CONNECTED
 * slot is bound to it. FAULT and EXPENDED slots do NOT hold the link — for a
 * faulted session the watchdog has already spoken, so the port beacons for a
 * possible hot-swapped replacement while the stale slot awaits recycling. */
static bool host_port_linked(const apex_host_t *h)
{
    for (size_t i = 0; i < APEX_HOST_MAX_DEVICES; i++) {
        if (h->devices[i].device_id == APEX_DEVICE_ID_UNASSIGNED) continue;
        if (h->devices[i].status == APEX_DEV_STATUS_PROVISIONAL ||
            h->devices[i].status == APEX_DEV_STATUS_CONNECTED) {
            return true;
        }
    }
    return false;
}

/* Revert a raised link rate to the default (§3.4, msg 13): the host app owns
 * the UART, so baud_switch_cb fires with APEX_BAUD_CODE_115200 and the id of
 * the device whose session had raised the rate. No-op at the default rate. */
static void host_revert_link_rate(apex_host_t *h)
{
    if (h->link_baud_code == APEX_BAUD_CODE_115200) return;
    uint8_t owner = h->link_baud_owner;
    h->link_baud_code  = APEX_BAUD_CODE_115200;
    h->link_baud_owner = 0;
    if (h->cfg.baud_switch_cb) {
        h->cfg.baud_switch_cb(h->cfg.baud_switch_user, owner,
                              APEX_BAUD_CODE_115200);
    }
}

/* Return a slot to the pool: clear its assigned ID and any sticky-dedup
 * reference so find_free_slot can reuse it. No event is fired — recycling is
 * internal table management. If this slot's session had raised the link rate,
 * the link reverts to the default (with the baud_switch_cb notification). */
static void free_slot(apex_host_t *h, apex_host_device_slot_t *slot)
{
    bool was_occupied = (slot->device_id != APEX_DEVICE_ID_UNASSIGNED);
    if (h->last_unassigned_id == slot->device_id) {
        h->last_unassigned_id = APEX_DEVICE_ID_UNASSIGNED;
    }
    if (was_occupied && slot->device_id == h->link_baud_owner) {
        host_revert_link_rate(h);
    }
    memset(slot, 0, sizeof(*slot));
    slot->device_id = APEX_DEVICE_ID_UNASSIGNED;
    slot->status    = APEX_DEV_STATUS_UNKNOWN;
    /* The linked->unlinked transition (no PROVISIONAL/CONNECTED slot
     * remains) is detected in apex_host_tick(), which resets the
     * mutual-receipt gate and the beacon cadence there — it must also catch
     * the CONNECTED->FAULT transition, which frees no slot. */
}

/* Promote a PROVISIONAL slot to CONNECTED on the first frame that proves the
 * device latched its assigned ID (explicit CONFIG_ACK, or any frame bearing the
 * assigned ID as a fallback — §3.3). No-op if the slot is not PROVISIONAL. */
static void confirm_latch(apex_host_t *h, apex_host_device_slot_t *slot)
{
    if (!slot || slot->status != APEX_DEV_STATUS_PROVISIONAL) return;
    slot->prov_query = APEX_HOST_PROV_NONE;
    if (h->last_unassigned_id == slot->device_id) {
        h->last_unassigned_id = APEX_DEVICE_ID_UNASSIGNED;
    }
    set_status(h, slot, APEX_DEV_STATUS_CONNECTED);
}

static uint8_t allocate_device_id(apex_host_t *h)
{
    /* Monotonic counter over the 0x02–0xFE assignable pool, skipping the
     * reserved 0x00 / 0x01 / 0xFF and any already-used IDs (§3.1.3). */
    for (int attempt = 0; attempt < 256; attempt++) {
        uint8_t candidate = h->next_assign_id;
        h->next_assign_id = (uint8_t)(h->next_assign_id + 1);
        if (h->next_assign_id < APEX_DEVICE_ID_ASSIGNED_MIN ||
            h->next_assign_id > APEX_DEVICE_ID_ASSIGNED_MAX) {
            h->next_assign_id = APEX_DEVICE_ID_ASSIGNED_MIN;
        }
        if (candidate < APEX_DEVICE_ID_ASSIGNED_MIN ||
            candidate > APEX_DEVICE_ID_ASSIGNED_MAX) continue;
        if (find_slot(h, candidate) == NULL) return candidate;
    }
    return APEX_DEVICE_ID_UNASSIGNED;
}

/* ---------------------------------------------------------------------------
 * Frame emission
 * ------------------------------------------------------------------------- */

static apex_status_t emit_frame(apex_host_t *h,
                                uint8_t traffic_type,
                                uint8_t device_id,
                                const uint8_t *payload,
                                uint8_t payload_len)
{
    uint8_t encoded[APEX_MAX_ENCODED_FRAME_LENGTH];
    size_t encoded_len;
    apex_hdr_t hdr = {
        .protocol_version = APEX_PROTOCOL_VERSION,
        .traffic_type     = traffic_type,
        .device_id        = device_id,
        .payload_length   = payload_len,
    };
    apex_status_t s = apex_frame_encode(&hdr, payload, encoded, sizeof(encoded),
                                        &encoded_len);
    if (s != APEX_OK) return s;
    if (h->cfg.tx) h->cfg.tx(h->cfg.tx_user, encoded, encoded_len);
    return APEX_OK;
}

static void send_config_reply(apex_host_t *h, uint8_t dest, uint8_t ack,
                              uint8_t assigned, uint8_t sel_cv,
                              uint8_t hmin, uint8_t hmax)
{
    uint8_t p[APEX_CONFIG_REPLY_PAYLOAD_LENGTH] = {
        APEX_CFG_MSG_CONFIG_REPLY, ack, assigned, sel_cv, hmin, hmax,
    };
    emit_frame(h, APEX_TRAFFIC_CONFIG, dest, p, sizeof(p));
}

static void send_phys_request(apex_host_t *h, uint8_t dest)
{
    uint8_t p[1] = { APEX_CFG_MSG_PHYS_REQUEST };
    emit_frame(h, APEX_TRAFFIC_CONFIG, dest, p, sizeof(p));
}

static void send_phys_ack(apex_host_t *h, uint8_t dest, uint8_t ack,
                          uint8_t assigned)
{
    uint8_t p[3] = { APEX_CFG_MSG_PHYS_ACK, ack, assigned };
    emit_frame(h, APEX_TRAFFIC_CONFIG, dest, p, sizeof(p));
}

static void send_baud_ack(apex_host_t *h, uint8_t dest, uint8_t ack, uint8_t code)
{
    uint8_t p[3] = { APEX_CFG_MSG_BAUD_CHANGE_ACK, ack, code };
    emit_frame(h, APEX_TRAFFIC_CONFIG, dest, p, sizeof(p));
}

static void send_reset_request(apex_host_t *h, uint8_t dest)
{
    uint8_t p[1] = { APEX_CFG_MSG_RESET_REQUEST };
    emit_frame(h, APEX_TRAFFIC_CONFIG, dest, p, sizeof(p));
}

/* Emit one VERSION_BEACON naming this build's wire-version range [1,1] (
 * unconditional discovery-phase emission from the tick loop only — never
 * triggered by received input). */
static void send_beacon(apex_host_t *h)
{
    uint8_t buf[APEX_MAX_ENCODED_FRAME_LENGTH];
    size_t len = 0;
    if (apex_beacon_build(APEX_PROTOCOL_VERSION, APEX_PROTOCOL_VERSION,
                          buf, sizeof(buf), &len) == APEX_OK && h->cfg.tx) {
        h->cfg.tx(h->cfg.tx_user, buf, len);
        h->beacon_ever_sent = true;
        h->last_beacon_tx_ms = h->now_ms;
    }
}

/* ---------------------------------------------------------------------------
 * Public lifecycle
 * ------------------------------------------------------------------------- */

static void on_beacon(void *user, uint16_t min_version, uint16_t max_version)
{
    apex_host_t *h = (apex_host_t *)user;
    h->beacons_seen++;

    /* Rule 4: ignored while the port is linked (a PROVISIONAL or
     * CONNECTED slot is bound) — a newcomer's beacon must not kill a live
     * session; the watchdog is the sole arbiter of the old session's death.
     * A lingering FAULT/EXPENDED slot does not hold the link. */
    if (host_port_linked(h)) return;

    /* Mutual-receipt gate: this build speaks only wire v1 (range [1,1]);
     * intersection with [min,max] is non-empty iff min <= 1 <= max. */
    if (min_version <= APEX_PROTOCOL_VERSION && APEX_PROTOCOL_VERSION <= max_version) {
        h->device_beacon_seen = true;
        h->peer_incompatible  = false;
        h->peer_min_version   = min_version;
        h->peer_max_version   = max_version;
    } else {
        h->beacons_incompatible++;
        h->peer_incompatible  = true;
        h->device_beacon_seen = false;
    }
}

void apex_host_init(apex_host_t *h, const apex_host_cfg_t *cfg)
{
    if (!h) return;
    memset(h, 0, sizeof(*h));
    if (cfg) h->cfg = *cfg;
    apex_framer_rx_init(&h->framer);
    apex_framer_set_beacon_cb(&h->framer, on_beacon, h);
    if (h->cfg.beacon_period_ms == 0) {
        h->cfg.beacon_period_ms = 1000u;  /* 1 Hz default (bounds 10..1000) */
    }
    h->next_assign_id = APEX_DEVICE_ID_ASSIGNED_MIN;
    h->flight_state = APEX_FLIGHT_STATE_UNKNOWN;
    h->last_unassigned_id = APEX_DEVICE_ID_UNASSIGNED;
    for (size_t i = 0; i < APEX_HOST_MAX_DEVICES; i++) {
        h->devices[i].device_id = APEX_DEVICE_ID_UNASSIGNED;
    }
}

apex_status_t apex_host_register_class_versioned(apex_host_t *h,
                                                 uint8_t traffic_type,
                                                 uint8_t class_version_min,
                                                 uint8_t class_version_max,
                                                 apex_host_class_rx_cb_t cb,
                                                 void *user)
{
    if (!h || traffic_type == APEX_TRAFFIC_CONFIG) return APEX_ERR_INVALID_ARGS;
    if (class_version_min == 0) class_version_min = 1;
    if (class_version_max == 0) class_version_max = class_version_min;
    if (class_version_min > class_version_max) return APEX_ERR_INVALID_ARGS;
    for (size_t i = 0; i < APEX_HOST_MAX_CLASSES; i++) {
        if (!h->classes[i].registered) {
            h->classes[i].traffic_type = traffic_type;
            h->classes[i].class_version_min = class_version_min;
            h->classes[i].class_version_max = class_version_max;
            h->classes[i].rx = cb;
            h->classes[i].user = user;
            h->classes[i].registered = true;
            return APEX_OK;
        }
    }
    return APEX_ERR_FULL;
}

apex_status_t apex_host_register_class(apex_host_t *h,
                                       uint8_t traffic_type,
                                       apex_host_class_rx_cb_t cb,
                                       void *user)
{
    return apex_host_register_class_versioned(h, traffic_type, 1, 1, cb, user);
}

/* ---------------------------------------------------------------------------
 * DEVICE_INFO — accept/reject decision and provisional-phase opening (§3.2.2,
 * §3.3)
 * ------------------------------------------------------------------------- */

static void handle_device_info(apex_host_t *h,
                               uint8_t src_device_id,
                               const uint8_t *body,
                               size_t body_len)
{
    /* Every pre-assignment DEVICE_INFO — including each class of a multi-class
     * unit, which discovers serially — arrives at the unassigned marker 0x01
     * (§3.3, §3.7.4). A DEVICE_INFO under an already-assigned ID is not part of
     * the v1 flow; drop it (§3.8). */
    if (src_device_id != APEX_DEVICE_ID_UNASSIGNED) return;

    /* Mutual-receipt gate: the version tier must close before session
     * traffic — a DEVICE_INFO arriving before the device's beacon is dropped
     * silently; the periodic beacons close the gap within one cadence
     * period. The gate persists while a slot is held (the dedup and
     * provisional paths below stay reachable) and resets when the port
     * unlinks. */
    if (!h->device_beacon_seen) return;

    /* DEVICE_INFO body (post-msg_id): class, flags, cv_min, cv_max, mass(u16).
     * Optional-tail parse (§3.6): absent tail bytes read as 0. */
    if (body_len < 2) return;
    uint8_t class_req  = body[0];
    uint8_t flags_req  = body[1];
    uint8_t dev_cv_min = (body_len > 2) ? body[2] : 0;
    uint8_t dev_cv_max = (body_len > 3) ? body[3] : 0;
    uint16_t mass      = (body_len > 5) ? get_u16(&body[4]) : 0;

    /* Policy deny latch (§3.2.11): a denied/evicted device gets
     * ACK_REJECT_POLICY on its DEVICE_INFO, terminating its retry loop
     * honestly. The latch stays armed while matching DEVICE_INFOs keep
     * arriving — duplicates already in flight when the first deny lands must
     * be denied too, not accepted into a fresh slot — and disarms after 5 s
     * of latch silence (tick loop; see the struct comment). */
    if (h->deny_pending && h->deny_class == class_req) {
        const apex_host_class_reg_t *reg = find_class(h, class_req);
        send_config_reply(h, APEX_DEVICE_ID_UNASSIGNED, APEX_ACK_REJECT_POLICY,
                          0, 0, reg ? reg->class_version_min : 0,
                          reg ? reg->class_version_max : 0);
        h->deny_last_ms = h->now_ms;
        return;
    }

    /* Dedup (§3.3.1): while a PROVISIONAL slot is already held for the (single)
     * unassigned device, refresh its recycle timer and resend the same
     * CONFIG_REPLY rather than burning a new slot. */
    if (h->last_unassigned_id != APEX_DEVICE_ID_UNASSIGNED) {
        apex_host_device_slot_t *prev = find_slot(h, h->last_unassigned_id);
        if (prev && prev->status == APEX_DEV_STATUS_PROVISIONAL) {
            prev->last_rx_ms = h->now_ms;
            uint8_t assigned = (prev->reply_ack == APEX_ACK_OK)
                                   ? prev->device_id : 0;
            send_config_reply(h, APEX_DEVICE_ID_UNASSIGNED, prev->reply_ack,
                              assigned, prev->selected_class_version,
                              prev->host_class_min, prev->host_class_max);
            return;
        }
        h->last_unassigned_id = APEX_DEVICE_ID_UNASSIGNED; /* stale ref */
    }

    /* Fresh discovery. Validate class, then interfaces, then class version,
     * then mass (§3.2.2 ordering). */
    const apex_host_class_reg_t *reg = find_class(h, class_req);
    if (class_req == APEX_TRAFFIC_CONFIG || reg == NULL) {
        send_config_reply(h, APEX_DEVICE_ID_UNASSIGNED, APEX_ACK_REJECT_CLASS,
                          0, 0, 0, 0);
        return;
    }
    if ((flags_req & (uint8_t)~h->cfg.supported_interfaces) != 0) {
        send_config_reply(h, APEX_DEVICE_ID_UNASSIGNED, APEX_ACK_REJECT_INTERFACE,
                          0, 0, reg->class_version_min, reg->class_version_max);
        return;
    }
    uint8_t hmin = reg->class_version_min;
    uint8_t hmax = reg->class_version_max;
    if (dev_cv_max < hmin || dev_cv_min > hmax) {
        /* Disjoint class-version ranges (§3.2.2). */
        send_config_reply(h, APEX_DEVICE_ID_UNASSIGNED,
                          APEX_ACK_REJECT_CLASS_VERSION, 0, 0, hmin, hmax);
        return;
    }
    uint8_t selected = (dev_cv_max < hmax) ? dev_cv_max : hmax; /* min of maxes */

    if (h->cfg.mass_policy_cb &&
        !h->cfg.mass_policy_cb(h->cfg.mass_policy_user, mass)) {
        send_config_reply(h, APEX_DEVICE_ID_UNASSIGNED, APEX_ACK_REJECT_MASS,
                          0, 0, hmin, hmax);
        return;
    }

    /* Accept. Allocate a slot + id and hold it PROVISIONAL (§3.3.1). */
    apex_host_device_slot_t *slot = find_free_slot(h);
    uint8_t new_id = slot ? allocate_device_id(h) : APEX_DEVICE_ID_UNASSIGNED;
    if (!slot || new_id == APEX_DEVICE_ID_UNASSIGNED) {
        /* No slot / id pool exhausted — host declines this device (§3.2.2). */
        send_config_reply(h, APEX_DEVICE_ID_UNASSIGNED, APEX_ACK_REJECT_POLICY,
                          0, 0, hmin, hmax);
        return;
    }
    free_slot(h, slot);                   /* clear any residue */
    slot->device_id              = new_id;
    slot->status                 = APEX_DEV_STATUS_PROVISIONAL;
    slot->device_class           = class_req;
    slot->interface_flags        = flags_req;
    slot->selected_class_version = selected;
    slot->host_class_min         = hmin;
    slot->host_class_max         = hmax;
    slot->mass_grams             = mass;
    slot->last_rx_ms             = h->now_ms;
    slot->status_since_ms        = h->now_ms;
    slot->prov_query             = APEX_HOST_PROV_NONE;
    slot->phys_reqs_sent         = 0;
    h->last_unassigned_id        = new_id;

    if (h->cfg.run_phys_provisional) {
        /* Conditional accept: id withheld; open the host-driven query loop. */
        slot->reply_ack = APEX_ACK_PROVISIONAL;
        send_config_reply(h, APEX_DEVICE_ID_UNASSIGNED, APEX_ACK_PROVISIONAL,
                          0, selected, hmin, hmax);
        slot->prov_query     = APEX_HOST_PROV_PHYS;
        slot->phys_reqs_sent = 1;
        slot->phys_next_ms   = h->now_ms + APEX_HOST_PHYS_RETRY_MS;
        send_phys_request(h, APEX_DEVICE_ID_UNASSIGNED);
    } else {
        /* Terminal accept: expose the id now; promote on CONFIG_ACK. */
        slot->reply_ack = APEX_ACK_OK;
        send_config_reply(h, APEX_DEVICE_ID_UNASSIGNED, APEX_ACK_OK, new_id,
                          selected, hmin, hmax);
    }
}

/* ---------------------------------------------------------------------------
 * PHYS_INFO — provisional-phase policy gate, post-CONNECTED in-flight update /
 * re-query reply (§3.2.10, §3.2.11)
 * ------------------------------------------------------------------------- */

/* Latest-wins store of the canonical complete physical state: one
 * apex_phys_t per physical unit, fed identically by the config-time query and
 * in-flight updates. */
static void store_phys(apex_host_device_slot_t *slot, const apex_phys_t *phys)
{
    slot->phys       = *phys;
    slot->has_phys   = true;
    slot->mass_grams = phys->mass_grams;
}

static void handle_phys_info(apex_host_t *h,
                             uint8_t src_device_id,
                             const uint8_t *body,
                             size_t body_len)
{
    /* Parse the 32-byte (post-msg_id) PHYS payload — mass at offset 0, then CG
     * and the tensor (§3.2.10, 33 bytes with msg_id) — zero-filling a short
     * tail (§3.6). */
    uint8_t buf[APEX_PHYS_INFO_PAYLOAD_LENGTH - 1];
    memset(buf, 0, sizeof(buf));
    if (body_len > sizeof(buf)) body_len = sizeof(buf);
    memcpy(buf, body, body_len);
    apex_phys_t phys;
    phys.mass_grams     = get_u16(&buf[0]);
    phys.cg_offset_x_mm = get_i16(&buf[2]);
    phys.cg_offset_y_mm = get_i16(&buf[4]);
    phys.cg_offset_z_mm = get_i16(&buf[6]);
    phys.ixx = get_i32(&buf[8]);
    phys.iyy = get_i32(&buf[12]);
    phys.izz = get_i32(&buf[16]);
    phys.pxy = get_i32(&buf[20]);
    phys.pxz = get_i32(&buf[24]);
    phys.pyz = get_i32(&buf[28]);

    if (src_device_id == APEX_DEVICE_ID_UNASSIGNED) {
        /* Provisional phase (§3.3): attribute to the held PROVISIONAL slot.
         * Pre-latch, the policy gate is TERMINAL (permits gates only
         * here — before the id certificate is issued). */
        apex_host_device_slot_t *slot =
            (h->last_unassigned_id != APEX_DEVICE_ID_UNASSIGNED)
                ? find_slot(h, h->last_unassigned_id) : NULL;
        if (!slot || slot->status != APEX_DEV_STATUS_PROVISIONAL ||
            slot->prov_query != APEX_HOST_PROV_PHYS) {
            return; /* Unsolicited / late PHYS_INFO — drop (§3.8). */
        }
        bool accept = !h->cfg.phys_policy_cb ||
                      h->cfg.phys_policy_cb(h->cfg.phys_policy_user,
                                            slot->device_id, &phys);
        slot->prov_query = APEX_HOST_PROV_NONE;
        if (accept) {
            /* Terminal ACK_OK concludes the phase and delivers the id. Mark the
             * slot so the dedup path now re-delivers via CONFIG_REPLY(ACK_OK). */
            store_phys(slot, &phys);
            slot->reply_ack = APEX_ACK_OK;
            send_phys_ack(h, APEX_DEVICE_ID_UNASSIGNED, APEX_ACK_OK,
                          slot->device_id);
        } else {
            /* Pre-commitment reject — no teardown; free the slot (§3.2.11). */
            send_phys_ack(h, APEX_DEVICE_ID_UNASSIGNED, APEX_ACK_REJECT_PHYS, 0);
            free_slot(h, slot);
        }
        return;
    }

    /* Post-CONNECTED: an in-flight physical update or the reply to a
     * post-CONNECTED re-query — the same canonical frame either way. The
     * stored state ALWAYS updates (this is a notification of reality) and the
     * ack is a receipt; a policy rejection is ADVISORY only (no
     * host-imposed post-latch terminals — no session effect, the slot stays
     * CONNECTED). Eviction, if wanted, is apex_host_evict(). */
    apex_host_device_slot_t *slot = find_slot(h, src_device_id);
    if (!slot || slot->status != APEX_DEV_STATUS_CONNECTED) return;
    store_phys(slot, &phys);
    if (h->cfg.on_phys_update) {
        h->cfg.on_phys_update(h->cfg.on_phys_update_user, src_device_id, &phys);
    }
    bool accept = !h->cfg.phys_policy_cb ||
                  h->cfg.phys_policy_cb(h->cfg.phys_policy_user,
                                        slot->device_id, &phys);
    send_phys_ack(h, src_device_id,
                  accept ? APEX_ACK_OK : APEX_ACK_REJECT_PHYS,
                  accept ? src_device_id : 0);
}

/* ---------------------------------------------------------------------------
 * BAUD_CHANGE_REQUEST (§3.4)
 * ------------------------------------------------------------------------- */

static void handle_baud_request(apex_host_t *h,
                                uint8_t src_device_id,
                                const uint8_t *body,
                                size_t body_len)
{
    if (body_len < 1) return;
    apex_host_device_slot_t *slot = find_slot(h, src_device_id);
    if (!slot || slot->status != APEX_DEV_STATUS_CONNECTED) return;
    uint8_t requested = body[0];

    bool grant = h->cfg.baud_policy_cb
                     ? h->cfg.baud_policy_cb(h->cfg.baud_policy_user,
                                             src_device_id,
                                             (apex_baud_code_t)requested)
                     : (requested <= h->cfg.max_baud_code);
    if (grant) {
        send_baud_ack(h, src_device_id, APEX_ACK_OK, requested);
        /* Host switches its UART immediately after transmitting the ack. Track
         * which session raised the rate so a reset / recycle can revert it. */
        h->link_baud_code  = requested;
        h->link_baud_owner = (requested != APEX_BAUD_CODE_115200)
                                 ? src_device_id : 0;
        if (h->cfg.baud_switch_cb) {
            h->cfg.baud_switch_cb(h->cfg.baud_switch_user, src_device_id,
                                  (apex_baud_code_t)requested);
        }
    } else {
        /* Counter-offer hint: highest supported code strictly below the request
         * (0 = nothing above the default). */
        uint8_t hint = 0;
        if (requested > 0) {
            hint = (uint8_t)(requested - 1);
            if (hint > h->cfg.max_baud_code) hint = h->cfg.max_baud_code;
        }
        send_baud_ack(h, src_device_id, APEX_ACK_REJECT_BAUD, hint);
    }
}

/* Device-initiated RESET_REQUEST announce (msg 13): the device is about to
 * drop its session voluntarily. Free its slot (id returns to the pool; fires
 * on_device_event(UNKNOWN)), revert a raised link rate, expect DEVICE_INFO.
 * From the unassigned marker it targets the held provisional slot, if any. */
static void handle_reset_announce(apex_host_t *h, uint8_t src_device_id)
{
    apex_host_device_slot_t *slot = NULL;
    if (src_device_id == APEX_DEVICE_ID_UNASSIGNED) {
        if (h->last_unassigned_id != APEX_DEVICE_ID_UNASSIGNED) {
            slot = find_slot(h, h->last_unassigned_id);
        }
    } else {
        slot = find_slot(h, src_device_id);
    }
    if (!slot) return;
    set_status(h, slot, APEX_DEV_STATUS_UNKNOWN);
    free_slot(h, slot);  /* also reverts the link rate if this session raised it */
}

static void handle_config_ack(apex_host_t *h,
                              uint8_t src_device_id,
                              const uint8_t *body,
                              size_t body_len)
{
    /* §3.2.6: CONFIG_ACK's body echoes the adopted id and must equal the outer
     * device_id and a slot we assigned. Promotion is otherwise identical to the
     * lost-ACK fallback in on_frame — confirm_latch() is idempotent. */
    if (body_len < 1) return;
    if (body[0] != src_device_id) return;
    confirm_latch(h, find_slot(h, src_device_id));
}

static void handle_name_reply(apex_host_t *h,
                              uint8_t src_device_id,
                              const uint8_t *body,
                              size_t body_len)
{
    if (!h->cfg.on_name_reply) return;
    h->cfg.on_name_reply(h->cfg.on_name_reply_user, src_device_id,
                         (const char *)body, body_len);
}

static void handle_config_frame(apex_host_t *h,
                                uint8_t src_device_id,
                                const uint8_t *payload,
                                size_t payload_len)
{
    if (payload_len == 0) return;  /* implicit heartbeat (§3.5) */

    uint8_t msg_id = payload[0];
    const uint8_t *body = payload + 1;
    size_t body_len = payload_len - 1;

    switch (msg_id) {
    case APEX_CFG_MSG_DEVICE_INFO:
        handle_device_info(h, src_device_id, body, body_len);
        break;
    case APEX_CFG_MSG_CONFIG_ACK:
        handle_config_ack(h, src_device_id, body, body_len);
        break;
    case APEX_CFG_MSG_PHYS_INFO:
        handle_phys_info(h, src_device_id, body, body_len);
        break;
    case APEX_CFG_MSG_BAUD_CHANGE_REQUEST:
        handle_baud_request(h, src_device_id, body, body_len);
        break;
    case APEX_CFG_MSG_NAME_REPLY:
        handle_name_reply(h, src_device_id, body, body_len);
        break;
    case APEX_CFG_MSG_RESET_REQUEST:
        handle_reset_announce(h, src_device_id);
        break;
    default:
        /* CONFIG_REPLY, NAME_REQUEST, HOST_STATE, PHYS_REQUEST, PHYS_ACK,
         * BAUD_CHANGE_ACK are host→device; unknown msg_ids are ignored (§3.6,
         * §3.8). */
        break;
    }
}

/* ---------------------------------------------------------------------------
 * Reactive RESET_REQUEST — §3.8 unknown-id row (amended): drop + log + SHOULD
 * send an addressed RESET_REQUEST, rate-limited to ≤ 1 Hz per id. A stale
 * device transmits at ≥ 1 Hz, so this alone recovers the host-swap (devkit)
 * scenario within ~1 s even if the boot sweep was missed.
 * ------------------------------------------------------------------------- */

static void maybe_reactive_reset(apex_host_t *h, uint8_t unknown_id)
{
    /* Per-id rate limit via a small table keyed by device_id (entry 0 =
     * unused; 0x00 is never a valid id). Reuse the id's entry if present,
     * else claim an unused one, else evict the stalest. */
    size_t victim = 0;
    uint32_t victim_age = 0;
    for (size_t i = 0; i < APEX_HOST_MAX_DEVICES; i++) {
        if (h->reactive_rl[i].device_id == unknown_id) {
            if ((uint32_t)(h->now_ms - h->reactive_rl[i].last_tx_ms) < 1000u) {
                return;  /* rate-limited */
            }
            h->reactive_rl[i].last_tx_ms = h->now_ms;
            send_reset_request(h, unknown_id);
            return;
        }
        uint32_t age = (h->reactive_rl[i].device_id == 0)
                           ? UINT32_MAX
                           : (uint32_t)(h->now_ms - h->reactive_rl[i].last_tx_ms);
        if (age >= victim_age) {
            victim_age = age;
            victim = i;
        }
    }
    h->reactive_rl[victim].device_id  = unknown_id;
    h->reactive_rl[victim].last_tx_ms = h->now_ms;
    send_reset_request(h, unknown_id);
}

/* ---------------------------------------------------------------------------
 * Frame dispatch
 * ------------------------------------------------------------------------- */

static void on_frame(void *user,
                     const apex_hdr_t *hdr,
                     const uint8_t *payload,
                     size_t payload_len)
{
    apex_host_t *h = (apex_host_t *)user;

    /* Unsupported (non-v1) session version: drop silently, keep counting
     * (receiver discipline — an endpoint never transmits because it received
     * something unintelligible; version resolution is the unconditional
     * periodic beacon exchange). */
    if (hdr->protocol_version != APEX_PROTOCOL_VERSION) {
        h->unsupported_pv_frames++;
        return;
    }

    /* §3.8: a device may never send under the broadcast id. */
    if (hdr->device_id == APEX_DEVICE_ID_BROADCAST) return;

    if (hdr->device_id == APEX_DEVICE_ID_UNASSIGNED) {
        /* Discovery / provisional-phase frame. Attribute to the held
         * PROVISIONAL slot (if any) to refresh its recycle timer (§3.3.1); do
         * NOT promote (the device is still at 0x01). Class traffic is gated. */
        if (h->last_unassigned_id != APEX_DEVICE_ID_UNASSIGNED) {
            apex_host_device_slot_t *pslot = find_slot(h, h->last_unassigned_id);
            if (pslot && pslot->status == APEX_DEV_STATUS_PROVISIONAL) {
                pslot->last_rx_ms = h->now_ms;
            }
        }
        if (hdr->traffic_type == APEX_TRAFFIC_CONFIG) {
            handle_config_frame(h, APEX_DEVICE_ID_UNASSIGNED, payload, payload_len);
        }
        return;
    }

    /* Assigned-id frame from an id the host holds no slot for: a device that
     * reset without re-discovering, or a stale session from a previous host
     * incarnation. §3.8 (amended): drop + log + reactive RESET_REQUEST
     * (≤ 1 Hz per id) so the stale device re-enumerates. */
    apex_host_device_slot_t *slot = find_slot(h, hdr->device_id);
    if (!slot) {
        h->unknown_id_frames++;
        maybe_reactive_reset(h, hdr->device_id);
        return;
    }

    /* Known device. Stamp liveness and treat any frame bearing the id as
     * confirmation of the latch (lost-CONFIG_ACK fallback, §3.3). */
    slot->last_rx_ms = h->now_ms;
    confirm_latch(h, slot);

    if (hdr->traffic_type == APEX_TRAFFIC_CONFIG) {
        handle_config_frame(h, hdr->device_id, payload, payload_len);
        return;
    }

    /* Class traffic. Deliver only to a registered handler for a CONNECTED
     * device. */
    if (slot->status != APEX_DEV_STATUS_CONNECTED) return;
    const apex_host_class_reg_t *reg = find_class(h, hdr->traffic_type);
    if (reg && reg->rx) {
        reg->rx(reg->user, hdr->device_id, payload, payload_len);
    }
}

void apex_host_feed_rx(apex_host_t *h,
                       const uint8_t *bytes,
                       size_t n,
                       uint32_t now_ms)
{
    if (!h) return;
    h->now_ms = now_ms;
    apex_framer_feed(&h->framer, bytes, n, on_frame, h);
}

/* ---------------------------------------------------------------------------
 * Provisional-phase PHYS query engine (§3.2.9, §3.2.11)
 * ------------------------------------------------------------------------- */

static void drive_provisional_phys(apex_host_t *h, apex_host_device_slot_t *slot)
{
    if (slot->prov_query != APEX_HOST_PROV_PHYS) return;
    if ((int32_t)(h->now_ms - slot->phys_next_ms) < 0) return;

    if (slot->phys_reqs_sent < APEX_HOST_PHYS_RETRIES) {
        send_phys_request(h, APEX_DEVICE_ID_UNASSIGNED);
        slot->phys_reqs_sent++;
        slot->phys_next_ms = h->now_ms + APEX_HOST_PHYS_RETRY_MS;
        return;
    }

    /* Retries exhausted — the device never answered PHYS_REQUEST (§3.2.11). */
    slot->prov_query = APEX_HOST_PROV_NONE;
    if (h->cfg.phys_required) {
        /* Best-effort courtesy reject, then deny this device's next discovery
         * and free the slot. */
        send_phys_ack(h, APEX_DEVICE_ID_UNASSIGNED, APEX_ACK_REJECT_PHYS, 0);
        h->deny_pending = true;
        h->deny_class   = slot->device_class;
        h->deny_last_ms = h->now_ms;
        free_slot(h, slot);
    } else {
        /* Phys optional: conclude the phase unprompted with a terminal ACK_OK
         * (a fresh CONFIG_REPLY, which every device understands — §3.3). */
        slot->reply_ack = APEX_ACK_OK;
        send_config_reply(h, APEX_DEVICE_ID_UNASSIGNED, APEX_ACK_OK,
                          slot->device_id, slot->selected_class_version,
                          slot->host_class_min, slot->host_class_max);
    }
}

/* ---------------------------------------------------------------------------
 * Tick — heartbeats, watchdogs, provisional query
 * ------------------------------------------------------------------------- */

void apex_host_tick(apex_host_t *h, uint32_t now_ms)
{
    if (!h) return;
    h->now_ms = now_ms;

    /* Boot sweep (msg 13): broadcast RESET_REQUEST 3x within the first second
     * of ticks after init, sweeping stale sessions from a previous host
     * incarnation (brown-out recovery). Runs before/while serving discovery;
     * suppressible via cfg.suppress_boot_sweep. Each sweep carries the full
     * sender obligations (free slots, revert rate) via
     * apex_host_request_reenumeration(). The remaining sweeps are CANCELLED as
     * soon as this incarnation enumerates a device: a fresh slot proves the
     * sweep already did its job on this link, and re-sweeping would only churn
     * sessions this host itself just created. A stale talker never occupies a
     * slot, so it stays covered by the remaining sweeps or, failing that, the
     * reactive reset path. */
    if (!h->cfg.suppress_boot_sweep && h->boot_sweeps_sent < 3) {
        if (apex_host_device_count(h) != 0) {
            h->boot_sweeps_sent = 3;  /* sweep done — a device enumerated */
        } else {
            if (!h->boot_sweep_anchored) {
                h->boot_sweep_anchored = true;
                h->boot_sweep_t0_ms = now_ms;
            }
            if ((uint32_t)(now_ms - h->boot_sweep_t0_ms) >=
                (uint32_t)h->boot_sweeps_sent * 333u) {
                apex_host_request_reenumeration(h, APEX_DEVICE_ID_BROADCAST);
                h->boot_sweeps_sent++;
            }
        }
    }

    /* Deny-latch disarm (§3.2.11, see the struct comment). The window is TWICE
     * the heartbeat watchdog: a phys-timeout-denied device sits silent for one
     * full watchdog period before its re-discovery arrives to consume the
     * latch, so a single-watchdog window would expire at exactly that moment
     * and re-admit it. Serving a deny refreshes the timer. */
    if (h->deny_pending &&
        (uint32_t)(now_ms - h->deny_last_ms) >= 2u * APEX_HEARTBEAT_WATCHDOG_MS) {
        h->deny_pending = false;
    }

    /* Port link-state transition: "linked" = a PROVISIONAL or CONNECTED
     * slot is bound. On the linked->unlinked transition (session freed,
     * faulted, or expended) reset the mutual-receipt gate and the beacon
     * cadence so a fresh exchange starts immediately — a hot-swapped
     * replacement device consumes nothing until it hears our beacon. */
    bool port_linked = host_port_linked(h);
    if (h->port_was_linked && !port_linked) {
        h->device_beacon_seen = false;
        h->peer_incompatible  = false;
        h->beacon_ever_sent   = false;
    }
    h->port_was_linked = port_linked;

    /* Proactive beaconing while unlinked: advertise our wire-version
     * range periodically. Continues after the device's beacon is received
     * (lost-beacon tolerance); stops while a PROVISIONAL/CONNECTED slot is
     * bound; resumes when the port unlinks again — including at a session's
     * FAULT transition, so a hot-swapped replacement can discover while the
     * stale slot awaits recycling. A peer with a disjoint range throttles the
     * cadence to <= 0.1 Hz. */
    if (!port_linked) {
        uint32_t period = h->peer_incompatible ? 10000u
                                               : h->cfg.beacon_period_ms;
        if (!h->beacon_ever_sent ||
            (uint32_t)(now_ms - h->last_beacon_tx_ms) >= period) {
            send_beacon(h);
        }
    }

    /* HOST_STATE broadcast. */
    if (h->cfg.host_state_period_ms > 0) {
        bool due = !h->host_state_ever_sent ||
                   (uint32_t)(now_ms - h->last_host_state_tx_ms)
                       >= h->cfg.host_state_period_ms;
        if (due) apex_host_send_host_state(h);
    }

    for (size_t i = 0; i < APEX_HOST_MAX_DEVICES; i++) {
        apex_host_device_slot_t *slot = &h->devices[i];
        if (slot->device_id == APEX_DEVICE_ID_UNASSIGNED) continue;

        switch (slot->status) {
        case APEX_DEV_STATUS_PROVISIONAL:
            /* Recycle a provisional slot whose device went silent (§3.3.1). Its
             * recycle timer is refreshed by any in-phase frame. */
            if ((uint32_t)(now_ms - slot->last_rx_ms) >= APEX_HEARTBEAT_WATCHDOG_MS) {
                free_slot(h, slot);
                break;
            }
            drive_provisional_phys(h, slot);
            break;
        case APEX_DEV_STATUS_CONNECTED:
            if ((uint32_t)(now_ms - slot->last_rx_ms) >= APEX_HEARTBEAT_WATCHDOG_MS) {
                set_status(h, slot, APEX_DEV_STATUS_FAULT);
            }
            break;
        case APEX_DEV_STATUS_FAULT:
            if ((uint32_t)(now_ms - slot->status_since_ms) >= APEX_HOST_FAULT_RECYCLE_MS) {
                free_slot(h, slot);
            }
            break;
        default:
            /* EXPENDED / UNKNOWN: no watchdog (§4). */
            break;
        }
    }
}

/* ---------------------------------------------------------------------------
 * Outbound API
 * ------------------------------------------------------------------------- */

void apex_host_set_flight_state(apex_host_t *h, apex_flight_state_t state)
{
    if (!h) return;
    h->flight_state = (uint8_t)state;
}

apex_status_t apex_host_send_host_state(apex_host_t *h)
{
    if (!h) return APEX_ERR_INVALID_ARGS;
    uint8_t payload[2] = { APEX_CFG_MSG_HOST_STATE, h->flight_state };
    apex_status_t s = emit_frame(h, APEX_TRAFFIC_CONFIG,
                                 APEX_DEVICE_ID_BROADCAST,
                                 payload, sizeof(payload));
    if (s == APEX_OK) {
        h->last_host_state_tx_ms = h->now_ms;
        h->host_state_ever_sent = true;
    }
    return s;
}

apex_status_t apex_host_send(apex_host_t *h,
                             uint8_t device_id,
                             uint8_t traffic_type,
                             const uint8_t *payload,
                             size_t payload_len)
{
    if (!h || traffic_type == APEX_TRAFFIC_CONFIG) return APEX_ERR_INVALID_ARGS;
    if (payload_len > APEX_MAX_PAYLOAD_LENGTH) return APEX_ERR_INVALID_ARGS;
    apex_host_device_slot_t *slot = find_slot(h, device_id);
    if (!slot || slot->status != APEX_DEV_STATUS_CONNECTED) return APEX_ERR_NOT_FOUND;
    return emit_frame(h, traffic_type, device_id, payload, (uint8_t)payload_len);
}

apex_status_t apex_host_request_name(apex_host_t *h,
                                     uint8_t device_id,
                                     uint8_t bytes_allocated)
{
    if (!h) return APEX_ERR_INVALID_ARGS;
    apex_host_device_slot_t *slot = find_slot(h, device_id);
    if (!slot || slot->status != APEX_DEV_STATUS_CONNECTED) return APEX_ERR_NOT_FOUND;
    uint8_t payload[2] = { APEX_CFG_MSG_NAME_REQUEST, bytes_allocated };
    return emit_frame(h, APEX_TRAFFIC_CONFIG, device_id, payload, sizeof(payload));
}

apex_status_t apex_host_request_phys(apex_host_t *h, uint8_t device_id)
{
    if (!h) return APEX_ERR_INVALID_ARGS;
    apex_host_device_slot_t *slot = find_slot(h, device_id);
    if (!slot || slot->status != APEX_DEV_STATUS_CONNECTED) return APEX_ERR_NOT_FOUND;
    send_phys_request(h, device_id);
    return APEX_OK;
}

apex_status_t apex_host_request_reenumeration(apex_host_t *h, uint8_t device_id)
{
    if (!h || device_id == APEX_DEVICE_ID_INVALID) return APEX_ERR_INVALID_ARGS;

    /* Transmit first (at the still-current link rate, so a raised-rate session
     * actually hears it), then apply the sender obligations: free slot(s),
     * revert the link rate, expect DEVICE_INFO. No ack exists (msg 13). */
    send_reset_request(h, device_id);

    if (device_id == APEX_DEVICE_ID_BROADCAST) {
        /* Bus-wide re-enumeration: every session is gone. */
        for (size_t i = 0; i < APEX_HOST_MAX_DEVICES; i++) {
            apex_host_device_slot_t *slot = &h->devices[i];
            if (slot->device_id == APEX_DEVICE_ID_UNASSIGNED) continue;
            set_status(h, slot, APEX_DEV_STATUS_UNKNOWN);
            free_slot(h, slot);
        }
        host_revert_link_rate(h);
        return APEX_OK;
    }

    if (device_id == APEX_DEVICE_ID_UNASSIGNED) {
        /* Un-wedge a stuck pre-CONNECTED device: drop the held provisional
         * slot, if any, so discovery restarts clean. */
        if (h->last_unassigned_id != APEX_DEVICE_ID_UNASSIGNED) {
            apex_host_device_slot_t *slot = find_slot(h, h->last_unassigned_id);
            if (slot) free_slot(h, slot);
        }
        return APEX_OK;
    }

    /* Addressed. A missing slot is fine — that is exactly the manual variant
     * of the reactive reset (a stale id the host never assigned). */
    apex_host_device_slot_t *slot = find_slot(h, device_id);
    if (slot) {
        set_status(h, slot, APEX_DEV_STATUS_UNKNOWN);
        free_slot(h, slot);  /* reverts the link rate if this session raised it */
    }
    return APEX_OK;
}

apex_status_t apex_host_evict(apex_host_t *h, uint8_t device_id)
{
    if (!h) return APEX_ERR_INVALID_ARGS;
    apex_host_device_slot_t *slot = find_slot(h, device_id);
    if (!slot) return APEX_ERR_NOT_FOUND;
    /* Eviction composition: arm the ACK_REJECT_POLICY deny latch for the
     * device's declared class, then reset its session. Its re-discovery
     * DEVICE_INFO consumes the latch and receives the terminal policy reject. */
    h->deny_pending = true;
    h->deny_class   = slot->device_class;
    h->deny_last_ms = h->now_ms;
    return apex_host_request_reenumeration(h, device_id);
}

void apex_host_mark_expended(apex_host_t *h, uint8_t device_id)
{
    if (!h) return;
    apex_host_device_slot_t *slot = find_slot(h, device_id);
    if (!slot) return;
    set_status(h, slot, APEX_DEV_STATUS_EXPENDED);
}

const apex_host_device_slot_t *apex_host_get_device(const apex_host_t *h,
                                                    uint8_t device_id)
{
    if (!h || device_id == APEX_DEVICE_ID_UNASSIGNED) return NULL;
    for (size_t i = 0; i < APEX_HOST_MAX_DEVICES; i++) {
        if (h->devices[i].device_id == device_id) return &h->devices[i];
    }
    return NULL;
}

size_t apex_host_device_count(const apex_host_t *h)
{
    if (!h) return 0;
    size_t n = 0;
    for (size_t i = 0; i < APEX_HOST_MAX_DEVICES; i++) {
        if (h->devices[i].device_id != APEX_DEVICE_ID_UNASSIGNED) n++;
    }
    return n;
}
