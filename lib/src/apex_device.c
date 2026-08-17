/**
 * @file apex_device.c
 * @brief Device-side core (APEX wire v1): discovery client, the provisional
 *        configuration phase, heartbeat/watchdog, post-CONNECTED baud
 *        negotiation, and VERSION_BEACON consumption.
 *
 * Copyright (c) 2026 Neros Technologies. MIT License — see LICENSE.
 */
#include <string.h>

#include "apex/apex_device.h"

/* ---------------------------------------------------------------------------
 * Little-endian serialization helpers
 * ------------------------------------------------------------------------- */

static void put_u16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
}

static void put_i32(uint8_t *p, int32_t v)
{
    uint32_t u = (uint32_t)v;
    p[0] = (uint8_t)(u & 0xFFu);
    p[1] = (uint8_t)((u >> 8) & 0xFFu);
    p[2] = (uint8_t)((u >> 16) & 0xFFu);
    p[3] = (uint8_t)((u >> 24) & 0xFFu);
}

/* ---------------------------------------------------------------------------
 * Frame emission
 * ------------------------------------------------------------------------- */

static apex_status_t emit_frame(apex_device_t *d,
                                uint8_t traffic_type,
                                const uint8_t *payload,
                                uint8_t payload_len)
{
    uint8_t encoded[APEX_MAX_ENCODED_FRAME_LENGTH];
    size_t encoded_len;
    apex_hdr_t hdr = {
        .protocol_version = APEX_PROTOCOL_VERSION,
        .traffic_type     = traffic_type,
        .device_id        = d->assigned_device_id,
        .payload_length   = payload_len,
    };
    apex_status_t s = apex_frame_encode(&hdr, payload, encoded, sizeof(encoded),
                                        &encoded_len);
    if (s != APEX_OK) return s;
    if (d->cfg.tx) d->cfg.tx(d->cfg.tx_user, encoded, encoded_len);
    d->last_tx_ms = d->now_ms;
    d->ever_sent = true;
    return APEX_OK;
}

static void set_link(apex_device_t *d, apex_device_link_state_t s)
{
    if (d->link == s) return;
    d->link = s;
    if (d->cfg.on_link_event) {
        d->cfg.on_link_event(d->cfg.on_link_event_user, s);
    }
}

static void send_device_info(apex_device_t *d)
{
    uint8_t payload[APEX_DEVICE_INFO_PAYLOAD_LENGTH] = {
        APEX_CFG_MSG_DEVICE_INFO,
        d->cfg.device_class,
        d->cfg.interface_flags,
        d->cfg.class_version_min,
        d->cfg.class_version_max,
        0, 0,
    };
    /* CURRENT mass: a device that dispensed half its load and then
     * re-enumerates declares what it weighs now, not the config-time value. */
    put_u16(&payload[5], d->phys.mass_grams);
    emit_frame(d, APEX_TRAFFIC_CONFIG, payload, sizeof(payload));
    d->last_discovery_tx_ms = d->now_ms;
}

static void send_heartbeat(apex_device_t *d)
{
    /* Empty CONFIG frame, payload_length = 0 (§3.5). */
    emit_frame(d, APEX_TRAFFIC_CONFIG, NULL, 0);
}

static void send_config_ack(apex_device_t *d)
{
    /* §3.2.6: confirm we latched the assigned ID. emit_frame uses
     * d->assigned_device_id (now the assigned ID) as the outer device_id; the
     * body echoes it so the host can validate. */
    uint8_t payload[2] = { APEX_CFG_MSG_CONFIG_ACK, d->assigned_device_id };
    emit_frame(d, APEX_TRAFFIC_CONFIG, payload, sizeof(payload));
}

static void send_phys_info(apex_device_t *d)
{
    /* 33-byte canonical complete physical state (§3.2.10): mass at
     * offset 1, then CG offsets and the inertia tensor. Built from the CURRENT
     * state — the same frame serves the provisional-phase reply, a
     * post-CONNECTED re-query reply, and the in-flight update. */
    uint8_t payload[APEX_PHYS_INFO_PAYLOAD_LENGTH];
    payload[0] = APEX_CFG_MSG_PHYS_INFO;
    put_u16(&payload[1], d->phys.mass_grams);
    put_u16(&payload[3], (uint16_t)d->phys.cg_offset_x_mm);
    put_u16(&payload[5], (uint16_t)d->phys.cg_offset_y_mm);
    put_u16(&payload[7], (uint16_t)d->phys.cg_offset_z_mm);
    put_i32(&payload[9],  d->phys.ixx);
    put_i32(&payload[13], d->phys.iyy);
    put_i32(&payload[17], d->phys.izz);
    put_i32(&payload[21], d->phys.pxy);
    put_i32(&payload[25], d->phys.pxz);
    put_i32(&payload[29], d->phys.pyz);
    emit_frame(d, APEX_TRAFFIC_CONFIG, payload, sizeof(payload));
}

static void send_baud_request(apex_device_t *d, uint8_t code)
{
    uint8_t payload[2] = { APEX_CFG_MSG_BAUD_CHANGE_REQUEST, code };
    emit_frame(d, APEX_TRAFFIC_CONFIG, payload, sizeof(payload));
}

/* Emit one VERSION_BEACON naming this build's wire-version range [1,1] (
 * unconditional discovery-phase emission — never triggered by received
 * input). */
static void send_beacon(apex_device_t *d)
{
    uint8_t buf[APEX_MAX_ENCODED_FRAME_LENGTH];
    size_t len = 0;
    if (apex_beacon_build(APEX_PROTOCOL_VERSION, APEX_PROTOCOL_VERSION,
                          buf, sizeof(buf), &len) == APEX_OK && d->cfg.tx) {
        d->cfg.tx(d->cfg.tx_user, buf, len);
        d->beacon_sent_once  = true;
        d->last_beacon_tx_ms = d->now_ms;
    }
}

/* Discard all session state and resume discovery — the shared path for the
 * watchdog (§3.5) and RESET_REQUEST re-enumeration (msg 13). If the session
 * rate was raised, on_baud_result fires with granted=true and the default code:
 * the app must move its UART back to 115 200 (this callback is the device
 * side's single rate-change channel — documented on
 * apex_device_baud_result_cb_t). */
static void reset_to_discovery(apex_device_t *d)
{
    bool rate_was_raised = (d->baud_current != APEX_BAUD_CODE_115200);
    /* A physical update still awaiting its receipt is settled as undelivered —
     * the session is gone. The physical STATE itself persists (d->phys is not
     * session state) and is re-declared at re-discovery. */
    if (d->phys_update_pending) {
        d->phys_update_pending = false;
        if (d->cfg.on_phys_update_result) {
            d->cfg.on_phys_update_result(d->cfg.on_phys_update_result_user,
                                         false);
        }
    }
    d->assigned_device_id     = APEX_DEVICE_ID_UNASSIGNED;
    d->selected_class_version = 0;
    d->phase_started          = false;
    d->baud_pending           = false;
    d->baud_current           = APEX_BAUD_CODE_115200;
    d->reset_pending          = false;
    d->reject_ack             = 0;
    /* Beacon EMISSION restarts with the session (fact: re-enumeration
     * restarts beaconing): the next tick beacons immediately, and — because
     * the tick emits the beacon before the gated DEVICE_INFO — the peer's
     * mutual-receipt gate re-opens in the same byte stream, so re-discovery
     * converges in ticks, not beacon periods.
     *
     * The peer's RECEIVED range is deliberately KEPT: it is link-layer
     * knowledge, not session state. If the peer actually changed (host swap
     * to a different wire version), our v1 DEVICE_INFO is silently dropped
     * there and the peer's own periodic beacon corrects our stale assumption
     * within one cadence period. */
    d->beacon_sent_once  = false;
    set_link(d, APEX_DEVICE_STATE_DISCOVERING);
    /* Re-open immediately so a lost session recovers within a retry period. */
    d->last_discovery_tx_ms = d->now_ms - d->cfg.discovery_retry_ms;
    if (rate_was_raised && d->cfg.on_baud_result) {
        d->cfg.on_baud_result(d->cfg.on_baud_result_user, true,
                              APEX_BAUD_CODE_115200);
    }
}

/* RESET_REQUEST (msg 13) received from the host — session re-enumeration
 * (brown-out recovery). Any addressing that
 * passed the device's address filter (assigned id, unassigned marker 0x01
 * pre-assignment, or broadcast) triggers it. Consult the safety-deferral hook:
 * while it forbids, latch the reset and carry on (heartbeats and class traffic
 * continue); the tick loop re-checks each tick and honors as soon as
 * permitted. */
static void handle_reset_request(apex_device_t *d)
{
    /* Already in discovery: there is no session to discard, and re-clearing
     * the beacon-exchange state would only wipe a mutual receipt that may have
     * just closed the version tier — e.g. a host's reactive reset racing
     * our own re-open. DEVICE_INFO retransmission is already running (or gated
     * on the pending beacon exchange); nothing to do. */
    if (d->link == APEX_DEVICE_STATE_DISCOVERING) return;

    if (d->cfg.reenum_permitted &&
        !d->cfg.reenum_permitted(d->cfg.reenum_permitted_user)) {
        d->reset_pending = true;
        return;
    }
    reset_to_discovery(d);
}

/* Latch a terminal ACK_OK from any ack-bearing message and complete the
 * handshake (§3.3): adopt the id, record the session class version, send
 * CONFIG_ACK under the new id, go CONNECTED. `sel_cv` of 0 (an older/shorter
 * reply that omitted the field) leaves the previously recorded value. */
static void latch_connected(apex_device_t *d, uint8_t assigned, uint8_t sel_cv)
{
    if (assigned < APEX_DEVICE_ID_ASSIGNED_MIN ||
        assigned > APEX_DEVICE_ID_ASSIGNED_MAX) {
        return; /* Malformed assigned id — ignore, keep retrying. */
    }
    d->assigned_device_id = assigned;
    if (sel_cv != 0) d->selected_class_version = sel_cv;
    set_link(d, APEX_DEVICE_STATE_CONNECTED);
    send_config_ack(d);
}

static void go_rejected(apex_device_t *d, uint8_t ack)
{
    d->reject_ack = ack;
    set_link(d, APEX_DEVICE_STATE_REJECTED);
}

/* ---------------------------------------------------------------------------
 * CONFIG message handling
 * ------------------------------------------------------------------------- */

static void handle_config_reply(apex_device_t *d,
                                const uint8_t *body,
                                size_t body_len)
{
    /* CONFIG_REPLY is meaningful only while discovering or inside the
     * provisional phase. A terminally rejected device has stopped retrying
     * (§3.3) and must not be re-opened by an in-flight duplicate reply that
     * was already on the wire when the terminal reject landed. */
    if (d->link != APEX_DEVICE_STATE_DISCOVERING &&
        d->link != APEX_DEVICE_STATE_PROVISIONAL) {
        return;
    }
    if (body_len < 1) return;
    /* Optional-tail parse (§3.6): absent tail bytes read as 0. */
    uint8_t ack       = body[0];
    uint8_t assigned  = (body_len > 1) ? body[1] : 0;
    uint8_t sel_cv    = (body_len > 2) ? body[2] : 0;
    uint8_t hclass_lo = (body_len > 3) ? body[3] : 0;
    uint8_t hclass_hi = (body_len > 4) ? body[4] : 0;

    d->phase_started = true;   /* Liveliness rules apply from here (§3.5). */
    d->host_class_min = hclass_lo;
    d->host_class_max = hclass_hi;

    switch (ack) {
    case APEX_ACK_OK:
        latch_connected(d, assigned, sel_cv);
        break;
    case APEX_ACK_PROVISIONAL:
        /* Conditional accept: class + class-version settled; id withheld. Stay
         * at 0x01 and answer the host-driven query loop (§3.3). */
        if (sel_cv != 0) d->selected_class_version = sel_cv;
        set_link(d, APEX_DEVICE_STATE_PROVISIONAL);
        break;
    case APEX_ACK_REJECT_INTERFACE:
        /* The one retryable reject: the app may lower cfg.interface_flags and we
         * keep retransmitting DEVICE_INFO. Stay DISCOVERING (§3.2.2). */
        d->reject_ack = ack;
        break;
    case APEX_ACK_REJECT_CLASS:
    case APEX_ACK_REJECT_CLASS_VERSION:
    case APEX_ACK_REJECT_MASS:
    case APEX_ACK_REJECT_POLICY:
    default:
        /* Terminal reject (named or unknown-cause, §3.2). Stop retrying. */
        go_rejected(d, ack);
        break;
    }
}

static void handle_phys_request(apex_device_t *d)
{
    /* PHYS_REQUEST is a KNOWN msg_id. A device with physical data answers with
     * PHYS_INFO; one without simply does not reply (non-support detected by the
     * host's timeout, §3.2.9). Legal in the provisional phase and while
     * CONNECTED (a later re-query). */
    if (d->link != APEX_DEVICE_STATE_PROVISIONAL &&
        d->link != APEX_DEVICE_STATE_CONNECTED) {
        return;
    }
    if (!d->cfg.has_phys) return;
    send_phys_info(d);
}

static void handle_phys_ack(apex_device_t *d,
                            const uint8_t *body,
                            size_t body_len)
{
    /* A device that does not implement the PHYS exchange treats the whole PHYS
     * message family — including a host's best-effort PHYS_ACK(ACK_REJECT_PHYS)
     * courtesy — as an unknown msg_id and ignores it (§3.2.11, §3.6). Its 5 s
     * watchdog then drives it back to discovery, where it is denied honestly
     * with ACK_REJECT_POLICY. */
    if (!d->cfg.has_phys) return;
    if (body_len < 1) return;
    uint8_t ack      = body[0];
    uint8_t assigned = (body_len > 1) ? body[1] : 0;

    if (d->link == APEX_DEVICE_STATE_CONNECTED) {
        /* Post-latch PHYS_ACK (NO host-imposed post-latch terminals). It
         * is a receipt — for a pending in-flight update it settles delivery —
         * and ACK_REJECT_PHYS is additionally the advisory envelope signal.
         * Never a session terminal: the link stays CONNECTED. */
        bool was_pending = d->phys_update_pending;
        d->phys_update_pending = false;
        if (ack == APEX_ACK_REJECT_PHYS && d->cfg.on_phys_advisory) {
            d->cfg.on_phys_advisory(d->cfg.on_phys_advisory_user);
        }
        if (was_pending && d->cfg.on_phys_update_result) {
            d->cfg.on_phys_update_result(d->cfg.on_phys_update_result_user,
                                         true);
        }
        return;
    }

    /* Pre-latch (provisional phase): terminal gate semantics (§3.2.11). */
    switch (ack) {
    case APEX_ACK_OK:
        /* Terminal ACK_OK may conclude the provisional phase here (§3.2.11). */
        latch_connected(d, assigned, 0);
        break;
    case APEX_ACK_PROVISIONAL:
        break; /* More queries coming; stay provisional. */
    case APEX_ACK_REJECT_PHYS:
    default:
        /* Provisional reject: an ordinary pre-commitment reject — the device
         * stops retrying (§3.2.11). */
        go_rejected(d, APEX_ACK_REJECT_PHYS);
        break;
    }
}

static void handle_baud_ack(apex_device_t *d,
                            const uint8_t *body,
                            size_t body_len)
{
    if (!d->baud_pending || body_len < 1) return;
    uint8_t ack  = body[0];
    uint8_t code = (body_len > 1) ? body[1] : 0;

    if (ack == APEX_ACK_OK) {
        /* Granted (echoes the code we proposed). Switch at the app's UART. */
        if (code != d->baud_request) return; /* Stale ack for another rung. */
        d->baud_current = code;
        d->baud_pending = false;
        if (d->cfg.on_baud_result) {
            d->cfg.on_baud_result(d->cfg.on_baud_result_user, true,
                                  (apex_baud_code_t)code);
        }
        return;
    }

    if (ack == APEX_ACK_REJECT_BAUD) {
        /* `code` is the counter-offer hint: highest code the host supports
         * strictly below our request (0 = nothing above the default). Ladder
         * down: jump to the hint if it is usable and makes downward progress,
         * else give up (§3.4). */
        uint8_t hint = code;
        if (hint != 0 && hint >= d->baud_min && hint < d->baud_request) {
            d->baud_request = hint;
            send_baud_request(d, hint);
        } else {
            d->baud_pending = false;
            if (d->cfg.on_baud_result) {
                d->cfg.on_baud_result(d->cfg.on_baud_result_user, false,
                                      (apex_baud_code_t)d->baud_current);
            }
        }
        return;
    }
    /* Any other ack in a BAUD_CHANGE_ACK: terminal, per the global namespace. */
    d->baud_pending = false;
    if (d->cfg.on_baud_result) {
        d->cfg.on_baud_result(d->cfg.on_baud_result_user, false,
                              (apex_baud_code_t)d->baud_current);
    }
}

static void handle_name_request(apex_device_t *d,
                                const uint8_t *body,
                                size_t body_len)
{
    if (body_len < 1) return;
    if (d->cfg.on_name_request) {
        d->cfg.on_name_request(d->cfg.on_name_request_user, body[0]);
    }
}

static void handle_host_state(apex_device_t *d,
                              const uint8_t *body,
                              size_t body_len)
{
    if (body_len < 1) return;
    if (d->cfg.on_host_state) {
        d->cfg.on_host_state(d->cfg.on_host_state_user,
                             (apex_flight_state_t)body[0]);
    }
}

static void handle_config_frame(apex_device_t *d,
                                const uint8_t *payload,
                                size_t payload_len)
{
    if (payload_len == 0) return;  /* heartbeat */

    uint8_t msg_id = payload[0];
    const uint8_t *body = payload + 1;
    size_t body_len = payload_len - 1;

    switch (msg_id) {
    case APEX_CFG_MSG_CONFIG_REPLY:
        handle_config_reply(d, body, body_len);
        break;
    case APEX_CFG_MSG_PHYS_REQUEST:
        handle_phys_request(d);
        break;
    case APEX_CFG_MSG_PHYS_ACK:
        handle_phys_ack(d, body, body_len);
        break;
    case APEX_CFG_MSG_BAUD_CHANGE_ACK:
        handle_baud_ack(d, body, body_len);
        break;
    case APEX_CFG_MSG_NAME_REQUEST:
        handle_name_request(d, body, body_len);
        break;
    case APEX_CFG_MSG_HOST_STATE:
        handle_host_state(d, body, body_len);
        break;
    case APEX_CFG_MSG_RESET_REQUEST:
        handle_reset_request(d);
        break;
    default:
        /* DEVICE_INFO, NAME_REPLY, CONFIG_ACK, PHYS_INFO, BAUD_CHANGE_REQUEST
         * are device→host; unknown msg_ids are ignored (§3.6). */
        break;
    }
}

/* ---------------------------------------------------------------------------
 * VERSION_BEACON consumption (§3.6.2, mutual-receipt gate)
 * ------------------------------------------------------------------------- */

static void on_beacon(void *user, uint16_t min_version, uint16_t max_version)
{
    apex_device_t *d = (apex_device_t *)user;

    /* Rule 4: link-local, and ignored inside an established / establishing
     * session (mid-session beacons are log-worthy, not actionable). REJECTED
     * is terminal — a beacon does not revive it (that would defeat eviction);
     * only RESET_REQUEST does. Beacons act only in DISCOVERING and
     * INCOMPATIBLE. */
    if (d->link != APEX_DEVICE_STATE_DISCOVERING &&
        d->link != APEX_DEVICE_STATE_INCOMPATIBLE) {
        return;
    }

    /* This build speaks only wire v1 (range [1,1]). Intersection with the
     * beacon's [min,max] is non-empty iff min <= 1 <= max. */
    if (min_version <= APEX_PROTOCOL_VERSION && APEX_PROTOCOL_VERSION <= max_version) {
        /* Version tier closed: record the peer range, compute the mutual
         * version min(own_max, their_max), and open with DEVICE_INFO at once
         * (rather than waiting a retry period). */
        d->peer_beacon_seen = true;
        d->peer_min_version = min_version;
        d->peer_max_version = max_version;
        d->mutual_version   = (max_version < (uint16_t)APEX_PROTOCOL_VERSION)
                                  ? max_version : (uint16_t)APEX_PROTOCOL_VERSION;
        if (d->link == APEX_DEVICE_STATE_INCOMPATIBLE) {
            set_link(d, APEX_DEVICE_STATE_DISCOVERING);
        }
        /* Beacon-precedes-hello (normative per the accepted finding): the
         * host's own mutual-receipt gate needs OUR beacon before it will
         * answer DEVICE_INFO, and its gate state is unknowable from here (it
         * may have just unlinked and discarded our earlier beacons — the
         * hot-swap case). Emit one immediately before the hello unless one
         * already went out at this very instant. */
        if (!d->beacon_sent_once || d->last_beacon_tx_ms != d->now_ms) {
            send_beacon(d);
        }
        send_device_info(d);
    } else {
        /* Disjoint ranges — incompatible. Back off; beaconing continues at
         * <= 0.1 Hz from the tick loop (§3.6.2 rule 2). */
        d->peer_beacon_seen = false;
        d->mutual_version   = 0;
        set_link(d, APEX_DEVICE_STATE_INCOMPATIBLE);
        d->last_beacon_tx_ms = d->now_ms;  /* start the backoff window */
    }
}

/* ---------------------------------------------------------------------------
 * Frame dispatch
 * ------------------------------------------------------------------------- */

static void on_frame(void *user,
                     const apex_hdr_t *hdr,
                     const uint8_t *payload,
                     size_t payload_len)
{
    apex_device_t *d = (apex_device_t *)user;

    /* Pre-closure input gate: while in discovery without a compatible
     * host beacon held, drop ALL non-beacon input — broadcasts (HOST_STATE)
     * and 0x01-addressed frames included; the host's beacon (delivered on the
     * separate framer beacon path) is the first frame a fresh device consumes.
     * Version agreement is per device pair: a hot-swapped device was never
     * party to the version its predecessor negotiated, so session traffic
     * still flowing on the port is void for the newcomer — not merely stale —
     * and possibly at a version it cannot correctly interpret. Once the tier
     * closes, the normal §3.8 filter below applies again. (A re-enumerating
     * device WAS party to the current pair agreement and keeps its peer range
     * across the session reset, so this gates only fresh power-ups and
     * post-incompatible states.) */
    if (!d->peer_beacon_seen &&
        (d->link == APEX_DEVICE_STATE_DISCOVERING ||
         d->link == APEX_DEVICE_STATE_INCOMPATIBLE)) {
        return;
    }

    /* Unsupported (non-v1) session version: drop silently (receiver
     * discipline — an endpoint never transmits because it received something
     * unintelligible; version resolution is the unconditional periodic beacon
     * exchange, §3.8/§3.6.2). */
    if (hdr->protocol_version != APEX_PROTOCOL_VERSION) return;

    /* §3.8 device-side address filter: act only on frames addressed to our
     * assigned ID, the broadcast value, or the unassigned marker while we are
     * still in our pre-assignment period (discovery + provisional). */
    bool pre_assignment = (d->assigned_device_id == APEX_DEVICE_ID_UNASSIGNED);
    bool addr_match =
        hdr->device_id == d->assigned_device_id ||
        hdr->device_id == APEX_DEVICE_ID_BROADCAST ||
        (pre_assignment && hdr->device_id == APEX_DEVICE_ID_UNASSIGNED);
    if (!addr_match) return;

    /* Host-liveliness watchdog feed (spec ruling): before CONNECTED, only
     * frames addressed to the device — the unassigned marker 0x01, or its
     * just-latched assigned id — refresh the watchdog; broadcasts (0xFF) do
     * NOT. An abandoned provisional device on a bus with ongoing HOST_STATE
     * broadcasts must still time out and re-enter discovery. Broadcast content
     * is still consumed normally below. Post-CONNECTED, any frame counts
     * (§3.5). */
    if (d->link == APEX_DEVICE_STATE_CONNECTED ||
        hdr->device_id != APEX_DEVICE_ID_BROADCAST) {
        d->last_rx_ms = d->now_ms;
    }

    if (hdr->traffic_type == APEX_TRAFFIC_CONFIG) {
        handle_config_frame(d, payload, payload_len);
        return;
    }

    if (d->link != APEX_DEVICE_STATE_CONNECTED) return;
    if (hdr->traffic_type != d->cfg.device_class) return;

    if (d->cfg.on_class_rx) {
        d->cfg.on_class_rx(d->cfg.on_class_rx_user, payload, payload_len);
    }
}

/* ---------------------------------------------------------------------------
 * Public lifecycle
 * ------------------------------------------------------------------------- */

void apex_device_init(apex_device_t *d, const apex_device_cfg_t *cfg)
{
    if (!d) return;
    memset(d, 0, sizeof(*d));
    if (cfg) d->cfg = *cfg;
    if (d->cfg.discovery_retry_ms == 0) {
        d->cfg.discovery_retry_ms = APEX_DISCOVERY_RETRY_MS;
    }
    if (d->cfg.beacon_period_ms == 0) {
        d->cfg.beacon_period_ms = 1000u;  /* 1 Hz default (bounds 10..1000) */
    }
    /* A contiguous class-version range MUST be declared (§3.6). Default to the
     * sole v1 class version 1..1 if the caller left it unset. */
    if (d->cfg.class_version_min == 0 && d->cfg.class_version_max == 0) {
        d->cfg.class_version_min = 1;
        d->cfg.class_version_max = 1;
    }
    apex_framer_rx_init(&d->framer);
    apex_framer_set_beacon_cb(&d->framer, on_beacon, d);
    d->link = APEX_DEVICE_STATE_DISCOVERING;
    d->assigned_device_id = APEX_DEVICE_ID_UNASSIGNED;
    d->baud_current = APEX_BAUD_CODE_115200;
    /* Seed the current physical state: inertia from cfg.phys, mass from
     * cfg.mass_grams (cfg.phys.mass_grams is ignored — at config time
     * PHYS_INFO's mass duplicates DEVICE_INFO's). */
    d->phys = d->cfg.phys;
    d->phys.mass_grams = d->cfg.mass_grams;
}

void apex_device_feed_rx(apex_device_t *d,
                         const uint8_t *bytes,
                         size_t n,
                         uint32_t now_ms)
{
    if (!d) return;
    d->now_ms = now_ms;
    apex_framer_feed(&d->framer, bytes, n, on_frame, d);
}

void apex_device_tick(apex_device_t *d, uint32_t now_ms)
{
    if (!d) return;
    d->now_ms = now_ms;

    /* A deferred RESET_REQUEST is re-checked every tick and honored as soon as
     * the safety hook permits (bounded deferral — msg 13). */
    if (d->reset_pending &&
        (!d->cfg.reenum_permitted ||
         d->cfg.reenum_permitted(d->cfg.reenum_permitted_user))) {
        reset_to_discovery(d);
    }

    switch (d->link) {
    case APEX_DEVICE_STATE_DISCOVERING: {
        /* Beacon periodically while unlinked — from power-up, continuing
         * even after the host's beacon is received (tolerates lost beacons),
         * stopping only once the handshake progresses past discovery. */
        if (!d->beacon_sent_once ||
            (uint32_t)(now_ms - d->last_beacon_tx_ms) >= d->cfg.beacon_period_ms) {
            send_beacon(d);
        }
        /* Mutual-receipt gate: no DEVICE_INFO until the host's beacon
         * closed the version tier. Then retransmit at the discovery cadence
         * (§3.3). No watchdog applies until the first CONFIG_REPLY. */
        if (d->peer_beacon_seen) {
            bool first_try = !d->ever_sent;
            bool due = (uint32_t)(now_ms - d->last_discovery_tx_ms) >=
                       d->cfg.discovery_retry_ms;
            if (first_try || due) send_device_info(d);
        }
        break;
    }
    case APEX_DEVICE_STATE_INCOMPATIBLE:
        /* Wire-version incompatible: beaconing continues at <= 0.1 Hz
         * (§3.6.2 rule 2). Only a compatible beacon re-opens discovery. */
        if ((uint32_t)(now_ms - d->last_beacon_tx_ms) >= 10000u) {
            send_beacon(d);
        }
        break;
    case APEX_DEVICE_STATE_PROVISIONAL:
    case APEX_DEVICE_STATE_CONNECTED:
        /* In-flight physical update retransmit: 3 x 500 ms, then report
         * delivery failure once via on_phys_update_result. */
        if (d->phys_update_pending &&
            d->link == APEX_DEVICE_STATE_CONNECTED &&
            (int32_t)(now_ms - d->phys_update_next_ms) >= 0) {
            if (d->phys_update_attempts < 3) {
                send_phys_info(d);
                d->phys_update_attempts++;
                d->phys_update_next_ms = now_ms + 500u;
            } else {
                d->phys_update_pending = false;
                if (d->cfg.on_phys_update_result) {
                    d->cfg.on_phys_update_result(
                        d->cfg.on_phys_update_result_user, false);
                }
            }
        }
        /* 1 Hz transmit floor (§3.5) — applies through the provisional phase so
         * the host's PROVISIONAL watchdog stays fed. */
        if ((uint32_t)(now_ms - d->last_tx_ms) >= APEX_HEARTBEAT_TX_PERIOD_MS) {
            send_heartbeat(d);
        }
        /* 5 s watchdog (§3.5): the host has gone silent — restart discovery.
         * Applies from the first CONFIG_REPLY, i.e. in both these states. */
        if ((uint32_t)(now_ms - d->last_rx_ms) >= APEX_HEARTBEAT_WATCHDOG_MS) {
            reset_to_discovery(d);
        }
        break;
    case APEX_DEVICE_STATE_REJECTED:
    default:
        /* Terminal: the device stopped retrying. No traffic. */
        break;
    }
}

/* ---------------------------------------------------------------------------
 * Outbound
 * ------------------------------------------------------------------------- */

apex_status_t apex_device_send(apex_device_t *d,
                               const uint8_t *payload,
                               size_t payload_len)
{
    if (!d) return APEX_ERR_INVALID_ARGS;
    if (d->link != APEX_DEVICE_STATE_CONNECTED) return APEX_ERR_BAD_STATE;
    if (payload_len > APEX_MAX_PAYLOAD_LENGTH) return APEX_ERR_INVALID_ARGS;
    return emit_frame(d, d->cfg.device_class, payload, (uint8_t)payload_len);
}

apex_status_t apex_device_send_name_reply(apex_device_t *d,
                                          const char *name,
                                          size_t name_len)
{
    if (!d) return APEX_ERR_INVALID_ARGS;
    if (name_len > APEX_MAX_PAYLOAD_LENGTH - 1) return APEX_ERR_INVALID_ARGS;
    uint8_t payload[APEX_MAX_PAYLOAD_LENGTH];
    payload[0] = APEX_CFG_MSG_NAME_REPLY;
    if (name_len > 0) memcpy(payload + 1, name, name_len);
    return emit_frame(d, APEX_TRAFFIC_CONFIG, payload, (uint8_t)(1 + name_len));
}

apex_status_t apex_device_request_baud(apex_device_t *d,
                                       apex_baud_code_t preferred,
                                       apex_baud_code_t min)
{
    if (!d) return APEX_ERR_INVALID_ARGS;
    if (d->link != APEX_DEVICE_STATE_CONNECTED) return APEX_ERR_BAD_STATE;
    if (d->baud_pending) return APEX_ERR_BAD_STATE;
    if ((uint8_t)preferred < (uint8_t)min) return APEX_ERR_INVALID_ARGS;
    d->baud_pending = true;
    d->baud_request = (uint8_t)preferred;
    d->baud_min     = (uint8_t)min;
    send_baud_request(d, (uint8_t)preferred);
    return APEX_OK;
}

apex_status_t apex_device_request_reenumeration(apex_device_t *d)
{
    if (!d) return APEX_ERR_INVALID_ARGS;
    if (d->link != APEX_DEVICE_STATE_PROVISIONAL &&
        d->link != APEX_DEVICE_STATE_CONNECTED) {
        return APEX_ERR_BAD_STATE;
    }
    /* Announce under the current id (assigned, or 0x01 while provisional),
     * then drop the session — announce-then-drop (msg 13). The rate revert, if
     * any, is signalled inside reset_to_discovery via on_baud_result; note the
     * announcement itself goes out at the still-current rate. */
    uint8_t payload[1] = { APEX_CFG_MSG_RESET_REQUEST };
    emit_frame(d, APEX_TRAFFIC_CONFIG, payload, sizeof(payload));
    reset_to_discovery(d);
    return APEX_OK;
}

apex_status_t apex_device_send_phys_update(apex_device_t *d,
                                           const apex_phys_t *phys)
{
    if (!d || !phys) return APEX_ERR_INVALID_ARGS;
    if (!d->cfg.has_phys) return APEX_ERR_UNSUPPORTED;
    if (d->link != APEX_DEVICE_STATE_CONNECTED) return APEX_ERR_BAD_STATE;
    /* One update in flight at a time, and <= 2 Hz sustained. A violating
     * call is rejected, not queued — the caller coalesces and re-calls with
     * the latest state. */
    if (d->phys_update_pending) return APEX_ERR_BAD_STATE;
    if (d->phys_update_ever &&
        (uint32_t)(d->now_ms - d->phys_update_last_req_ms) < 500u) {
        return APEX_ERR_BAD_STATE;
    }

    /* The stored current state updates immediately (it is reality): a later
     * re-enumeration re-declares these values regardless of delivery. */
    d->phys = *phys;
    d->phys_update_ever        = true;
    d->phys_update_last_req_ms = d->now_ms;
    d->phys_update_pending     = true;
    d->phys_update_attempts    = 1;
    d->phys_update_next_ms     = d->now_ms + 500u;
    send_phys_info(d);
    return APEX_OK;
}
