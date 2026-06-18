/**
 * @file apex_repeater.c
 * @brief Repeater device class — both Host and Device sides.
 *
 * Copyright (c) 2026 Neros Technologies. MIT License — see LICENSE.
 */
#include <string.h>

#include "apex/apex_repeater.h"

/* ---------------------------------------------------------------------------
 * Shared helpers
 * ------------------------------------------------------------------------- */

/* Count set bits in the link-block region of capability_flags (bits 0–4).
 * These are the bits that each produce one 5-byte link block in TELEMETRY. */
static uint8_t count_link_blocks(uint8_t cap_flags)
{
    uint8_t v = cap_flags & APEX_RPT_CAP_LINK_MASK;
    uint8_t c = 0;
    while (v) { c += (v & 1u); v >>= 1; }
    return c;
}

/* ===========================================================================
 * Device-side
 * ========================================================================= */

static apex_status_t dev_send(apex_repeater_device_t *d,
                              const uint8_t *payload, size_t len)
{
    return apex_device_send(d->core, payload, len);
}

/* Serialize the current telemetry into buf. Returns frame length, 0 on overflow. */
static size_t dev_build_telemetry(const apex_repeater_device_t *d,
                                  uint8_t *buf, size_t buf_size)
{
    uint8_t cap = d->telemetry.capability_flags;
    uint8_t nlinks = count_link_blocks(cap);
    uint8_t nant = (cap & APEX_RPT_CAP_ANTENNAS) ? d->telemetry.antenna_count : 0u;
    if (nant > APEX_REPEATER_MAX_ANTENNAS) return 0;
    size_t needed = 3u + (size_t)nlinks * 5u
                    + ((cap & APEX_RPT_CAP_ANTENNAS) ? (1u + (size_t)nant * 7u) : 0u);
    if (needed > buf_size) return 0;

    size_t i = 0;
    buf[i++] = APEX_RPT_MSG_TELEMETRY;
    buf[i++] = d->telemetry.device_state;
    buf[i++] = cap;

    for (int b = 0; b < 5; b++) {
        if (!(cap & (uint8_t)(1u << b))) continue;
        const apex_repeater_link_block_t *lb = &d->telemetry.link_blocks[b];
        buf[i++] = (uint8_t)lb->rssi_dbm;
        buf[i++] = lb->lq_percent;
        buf[i++] = (uint8_t)lb->snr_db;
        buf[i++] = (uint8_t)lb->tx_power_dbm;
        buf[i++] = lb->flags;
    }

    if (cap & APEX_RPT_CAP_ANTENNAS) {
        buf[i++] = nant;
        for (uint8_t a = 0; a < nant; a++) {
            const apex_repeater_directionality_t *dir = &d->telemetry.antenna_dir[a];
            buf[i++] = (uint8_t)(dir->antenna_bearing_deg & 0xFFu);
            buf[i++] = (uint8_t)(dir->antenna_bearing_deg >> 8);
            buf[i++] = (uint8_t)(dir->distal_bearing_deg & 0xFFu);
            buf[i++] = (uint8_t)(dir->distal_bearing_deg >> 8);
            buf[i++] = (uint8_t)(dir->distal_distance_m & 0xFFu);
            buf[i++] = (uint8_t)(dir->distal_distance_m >> 8);
            buf[i++] = dir->confidence;
        }
    }
    return i;
}

/* Serialize current config into buf as a CONFIG_REPORT frame.
 * Returns frame length, 0 on overflow. */
static size_t dev_build_config_report(const apex_repeater_device_t *d,
                                      uint8_t *buf, size_t buf_size)
{
    uint8_t cap = d->config.capability_flags;
    bool has_antennas = (cap & APEX_RPT_CAP_ANTENNAS) != 0;
    size_t i = 0;

    if (i + 2 > buf_size) return 0;
    buf[i++] = APEX_RPT_MSG_CONFIG_REPORT;
    buf[i++] = cap;

    /* C2 link config sections (bits 0–3): c2_protocol(1) + version(1) + len(1)
     * + blob + (antenna_id(1) when an antenna list is present). */
    for (int b = 0; b < 4; b++) {
        if (!(cap & (uint8_t)(1u << b))) continue;
        const apex_repeater_c2_config_t *c2 = &d->config.c2[b];
        uint8_t blen = c2->c2_config_len;
        if (blen > APEX_REPEATER_C2_CONFIG_MAX) blen = APEX_REPEATER_C2_CONFIG_MAX;
        if (i + 3u + blen + (has_antennas ? 1u : 0u) > buf_size) return 0;
        buf[i++] = c2->c2_protocol;
        buf[i++] = c2->c2_config_version;
        buf[i++] = blen;
        if (blen > 0) { memcpy(buf + i, c2->c2_config_blob, blen); i += blen; }
        if (has_antennas) buf[i++] = c2->antenna_id;
    }

    /* Video section (bit 4): center + bandwidth per direction, + antenna_id(1)
     * when an antenna list is present. */
    if (cap & APEX_RPT_CAP_VIDEO) {
        if (i + 11u + (has_antennas ? 1u : 0u) > buf_size) return 0;
        const apex_repeater_video_config_t *v = &d->config.video;
        buf[i++] = (uint8_t)(v->rx_freq_mhz & 0xFFu);
        buf[i++] = (uint8_t)(v->rx_freq_mhz >> 8);
        buf[i++] = (uint8_t)(v->rx_bw_mhz & 0xFFu);
        buf[i++] = (uint8_t)(v->rx_bw_mhz >> 8);
        buf[i++] = v->rx_format;
        buf[i++] = (uint8_t)(v->tx_freq_mhz & 0xFFu);
        buf[i++] = (uint8_t)(v->tx_freq_mhz >> 8);
        buf[i++] = (uint8_t)(v->tx_bw_mhz & 0xFFu);
        buf[i++] = (uint8_t)(v->tx_bw_mhz >> 8);
        buf[i++] = (uint8_t)v->tx_power_dbm;
        buf[i++] = v->tx_format;
        if (has_antennas) buf[i++] = v->antenna_id;
    }

    /* Antenna list section (bit 5): antenna_count(1) + per antenna
     * antenna_type(1) + antenna_bearing_ref(1). */
    if (has_antennas) {
        uint8_t nant = d->config.antenna_count;
        if (nant > APEX_REPEATER_MAX_ANTENNAS) return 0;
        if (i + 1u + (size_t)nant * 2u > buf_size) return 0;
        buf[i++] = nant;
        for (uint8_t a = 0; a < nant; a++) {
            buf[i++] = d->config.antennas[a].antenna_type;
            buf[i++] = d->config.antennas[a].antenna_bearing_ref;
        }
    }

    /* Global section (always present). */
    if (i + 2 > buf_size) return 0;
    buf[i++] = d->config.global.encryption_state;
    buf[i++] = d->config.global.distal_tlm_rate_hz;

    return i;
}

static void dev_send_ack(apex_repeater_device_t *d,
                         uint8_t acked_msg,
                         apex_repeater_ack_result_t result,
                         uint8_t failed_section)
{
    uint8_t buf[4] = {
        APEX_RPT_MSG_ACK,
        acked_msg,
        (uint8_t)result,
        failed_section,
    };
    dev_send(d, buf, sizeof(buf));
}

static void dev_send_telemetry(apex_repeater_device_t *d)
{
    /* Max telemetry: 3 header + 5 links × 5 bytes + 1 antenna_count
     * + MAX_ANTENNAS × 7 directionality bytes. */
    uint8_t buf[3 + 5 * 5 + 1 + APEX_REPEATER_MAX_ANTENNAS * 7];
    size_t len = dev_build_telemetry(d, buf, sizeof(buf));
    if (len == 0) return;
    if (dev_send(d, buf, len) == APEX_OK) {
        d->last_telemetry_tx_ms = d->now_ms;
        d->telemetry_dirty = false;
        d->first_telemetry_sent = true;
    }
}

static void dev_send_config_report(apex_repeater_device_t *d)
{
    uint8_t buf[APEX_V0_MAX_PAYLOAD_LENGTH];
    size_t len = dev_build_config_report(d, buf, sizeof(buf));
    if (len > 0) dev_send(d, buf, len);
}

/* ---------------------------------------------------------------------------
 * Device-side command handlers
 * ------------------------------------------------------------------------- */

static void dev_handle_get_config(apex_repeater_device_t *d)
{
    dev_send_config_report(d);
}

static void dev_handle_set_config(apex_repeater_device_t *d,
                                  const uint8_t *body, size_t body_len)
{
    if (body_len < 1) {
        dev_send_ack(d, APEX_RPT_MSG_SET_CONFIG, APEX_RPT_ACK_REJECT_MALFORMED, 0xFF);
        return;
    }

    uint8_t update_mask = body[0];
    /* Bit 7 is reserved and must be zero. */
    if (update_mask & 0x80u) {
        dev_send_ack(d, APEX_RPT_MSG_SET_CONFIG, APEX_RPT_ACK_REJECT_MALFORMED, 0xFF);
        return;
    }

    size_t off = 1;
    apex_repeater_config_t new_cfg = d->config;

    for (int b = 0; b < 7; b++) {
        if (!(update_mask & (uint8_t)(1u << b))) continue;

        if (b < 4) {
            /* C2 link config section: version(1) + len(1) + blob(N).
             * c2_protocol is read-only and NOT carried in SET_CONFIG — the
             * value already in new_cfg (copied from d->config) is preserved. */
            if (off + 2 > body_len) {
                dev_send_ack(d, APEX_RPT_MSG_SET_CONFIG, APEX_RPT_ACK_REJECT_MALFORMED, 0xFF);
                return;
            }
            uint8_t bver = body[off++];
            uint8_t blen = body[off++];
            if (blen > APEX_REPEATER_C2_CONFIG_MAX) {
                dev_send_ack(d, APEX_RPT_MSG_SET_CONFIG,
                             APEX_RPT_ACK_REJECT_INVALID_VALUE, (uint8_t)(1u << b));
                return;
            }
            if (off + blen > body_len) {
                dev_send_ack(d, APEX_RPT_MSG_SET_CONFIG, APEX_RPT_ACK_REJECT_MALFORMED, 0xFF);
                return;
            }
            new_cfg.c2[b].c2_config_version = bver;
            new_cfg.c2[b].c2_config_len = blen;
            if (blen > 0) memcpy(new_cfg.c2[b].c2_config_blob, body + off, blen);
            off += blen;

        } else if (b == 4) {
            /* Video TX section: tx_freq_mhz(2) + tx_bw_mhz(2) + tx_power_dbm(1)
             * + tx_format(1). */
            if (off + 6 > body_len) {
                dev_send_ack(d, APEX_RPT_MSG_SET_CONFIG, APEX_RPT_ACK_REJECT_MALFORMED, 0xFF);
                return;
            }
            new_cfg.video.tx_freq_mhz = (uint16_t)body[off] | ((uint16_t)body[off + 1] << 8);
            off += 2;
            new_cfg.video.tx_bw_mhz = (uint16_t)body[off] | ((uint16_t)body[off + 1] << 8);
            off += 2;
            new_cfg.video.tx_power_dbm = (int8_t)body[off++];
            new_cfg.video.tx_format    = body[off++];

        } else if (b == 5) {
            /* Antenna section (per-antenna): antenna_id(1) + antenna_bearing_ref(1).
             * antenna_type and the link→antenna wiring are HW-fixed. */
            if (off + 2 > body_len) {
                dev_send_ack(d, APEX_RPT_MSG_SET_CONFIG, APEX_RPT_ACK_REJECT_MALFORMED, 0xFF);
                return;
            }
            uint8_t aid = body[off++];
            uint8_t ref = body[off++];
            if (aid >= new_cfg.antenna_count) {
                dev_send_ack(d, APEX_RPT_MSG_SET_CONFIG,
                             APEX_RPT_ACK_REJECT_INVALID_VALUE, (uint8_t)(1u << b));
                return;
            }
            new_cfg.antennas[aid].antenna_bearing_ref = ref;

        } else if (b == 6) {
            /* Global section: distal_tlm_rate_hz(1). encryption_state is read-only. */
            if (off + 1 > body_len) {
                dev_send_ack(d, APEX_RPT_MSG_SET_CONFIG, APEX_RPT_ACK_REJECT_MALFORMED, 0xFF);
                return;
            }
            new_cfg.global.distal_tlm_rate_hz = body[off++];
        }
    }

    /* Let the application validate and apply. */
    if (d->hooks.on_set_config) {
        apex_status_t r = d->hooks.on_set_config(
            d->hooks.on_set_config_user, update_mask, &new_cfg);
        if (r != APEX_OK) {
            dev_send_ack(d, APEX_RPT_MSG_SET_CONFIG,
                         APEX_RPT_ACK_REJECT_INVALID_VALUE, 0xFF);
            return;
        }
    }

    d->config = new_cfg;
    dev_send_ack(d, APEX_RPT_MSG_SET_CONFIG, APEX_RPT_ACK_ACCEPTED, 0xFF);
}

static void dev_handle_antenna_cmd(apex_repeater_device_t *d,
                                   const uint8_t *body, size_t body_len)
{
    /* antenna_id(1) + bearing(2). */
    if (body_len < 3) {
        dev_send_ack(d, APEX_RPT_MSG_ANTENNA_CMD, APEX_RPT_ACK_REJECT_MALFORMED, 0xFF);
        return;
    }
    uint8_t  antenna_id = body[0];
    uint16_t bearing    = (uint16_t)body[1] | ((uint16_t)body[2] << 8);

    /* The device must have an antenna list and the id must name a real antenna. */
    if (!(d->config.capability_flags & APEX_RPT_CAP_ANTENNAS) ||
        antenna_id >= d->config.antenna_count) {
        dev_send_ack(d, APEX_RPT_MSG_ANTENNA_CMD, APEX_RPT_ACK_REJECT_INVALID_VALUE, 0xFF);
        return;
    }
    /* Reject if the targeted antenna isn't aimable or the device is in FAULT. */
    if (d->config.antennas[antenna_id].antenna_type != APEX_RPT_ANTENNA_DIRECTIONAL_AIMABLE ||
        (apex_repeater_state_t)d->telemetry.device_state == APEX_RPT_STATE_FAULT) {
        dev_send_ack(d, APEX_RPT_MSG_ANTENNA_CMD, APEX_RPT_ACK_REJECT_WRONG_STATE, 0xFF);
        return;
    }
    /* bearing must be 0–359 or the special BEARING_AUTO sentinel. */
    if (bearing != APEX_RPT_BEARING_AUTO && bearing > 359u) {
        dev_send_ack(d, APEX_RPT_MSG_ANTENNA_CMD, APEX_RPT_ACK_REJECT_INVALID_VALUE, 0xFF);
        return;
    }
    if (d->hooks.on_antenna_cmd) {
        apex_status_t r = d->hooks.on_antenna_cmd(
            d->hooks.on_antenna_cmd_user, antenna_id, bearing);
        if (r != APEX_OK) {
            dev_send_ack(d, APEX_RPT_MSG_ANTENNA_CMD, APEX_RPT_ACK_REJECT_INVALID_VALUE, 0xFF);
            return;
        }
    }
    dev_send_ack(d, APEX_RPT_MSG_ANTENNA_CMD, APEX_RPT_ACK_ACCEPTED, 0xFF);
}

/* ---------------------------------------------------------------------------
 * Device-side public API
 * ------------------------------------------------------------------------- */

void apex_repeater_device_on_rx(apex_repeater_device_t *d,
                                const uint8_t *payload, size_t payload_len)
{
    if (!d || payload_len < 1) return;
    uint8_t msg_id = payload[0];
    const uint8_t *body = payload + 1;
    size_t body_len = payload_len - 1;

    switch (msg_id) {
    case APEX_RPT_MSG_GET_CONFIG:
        dev_handle_get_config(d);
        break;
    case APEX_RPT_MSG_SET_CONFIG:
        dev_handle_set_config(d, body, body_len);
        break;
    case APEX_RPT_MSG_ANTENNA_CMD:
        dev_handle_antenna_cmd(d, body, body_len);
        break;
    default:
        /* TELEMETRY, DISTAL_TLM, CONFIG_REPORT, ACK are Device→Host; ignore. */
        break;
    }
}

apex_status_t apex_repeater_device_init(apex_repeater_device_t *d,
                                        apex_device_t *core,
                                        const apex_repeater_config_t *initial_config,
                                        const apex_repeater_device_hooks_t *hooks)
{
    if (!d || !core || !initial_config) return APEX_ERR_INVALID_ARGS;
    /* Validate config blob lengths for declared C2 links. */
    for (int b = 0; b < 4; b++) {
        if (!(initial_config->capability_flags & (uint8_t)(1u << b))) continue;
        if (initial_config->c2[b].c2_config_len > APEX_REPEATER_C2_CONFIG_MAX) {
            return APEX_ERR_INVALID_ARGS;
        }
    }
    /* Validate antenna list size when an antenna list is declared. */
    if ((initial_config->capability_flags & APEX_RPT_CAP_ANTENNAS) &&
        initial_config->antenna_count > APEX_REPEATER_MAX_ANTENNAS) {
        return APEX_ERR_INVALID_ARGS;
    }
    memset(d, 0, sizeof(*d));
    d->core   = core;
    d->config = *initial_config;
    if (hooks) d->hooks = *hooks;

    /* Default telemetry: ACTIVE, same capability_flags as config. */
    d->telemetry.device_state    = (uint8_t)APEX_RPT_STATE_ACTIVE;
    d->telemetry.capability_flags = initial_config->capability_flags;
    /* Initialise "no signal" sentinels for all link blocks. */
    for (int b = 0; b < 5; b++) {
        d->telemetry.link_blocks[b].rssi_dbm = APEX_RPT_RSSI_NO_SIGNAL;
        d->telemetry.link_blocks[b].snr_db   = APEX_RPT_RSSI_NO_SIGNAL;
    }
    /* Per-antenna directionality: as many blocks as listed antennas, all unknown. */
    d->telemetry.antenna_count =
        (initial_config->capability_flags & APEX_RPT_CAP_ANTENNAS)
            ? initial_config->antenna_count : 0u;
    for (uint8_t a = 0; a < APEX_REPEATER_MAX_ANTENNAS; a++) {
        d->telemetry.antenna_dir[a].antenna_bearing_deg = 0xFFFFu;
        d->telemetry.antenna_dir[a].distal_bearing_deg  = 0xFFFFu;
        d->telemetry.antenna_dir[a].distal_distance_m   = 0xFFFFu;
    }

    d->last_link = APEX_DEVICE_STATE_DISCOVERING;
    return APEX_OK;
}

void apex_repeater_device_tick(apex_repeater_device_t *d, uint32_t now_ms)
{
    if (!d) return;
    d->now_ms = now_ms;

    apex_device_link_state_t link = apex_device_link_state(d->core);
    if (link != d->last_link) {
        if (link != APEX_DEVICE_STATE_CONNECTED) {
            /* Link went away — reset timing so we burst on reconnect. */
            d->first_telemetry_sent = false;
            d->last_telemetry_tx_ms = 0;
            d->telemetry_dirty = false;
        }
        d->last_link = link;
    }

    if (link != APEX_DEVICE_STATE_CONNECTED) return;

    bool first_due = !d->first_telemetry_sent &&
                     (d->last_telemetry_tx_ms == 0 ||
                      (uint32_t)(now_ms - d->last_telemetry_tx_ms) >=
                          APEX_REPEATER_TELEMETRY_FIRST_MS);
    bool periodic_due = d->first_telemetry_sent &&
                        (uint32_t)(now_ms - d->last_telemetry_tx_ms) >=
                            APEX_REPEATER_TELEMETRY_PERIOD_MS;

    if (first_due || periodic_due || d->telemetry_dirty) {
        dev_send_telemetry(d);
    }
}

void apex_repeater_device_update_telemetry(apex_repeater_device_t *d,
                                           const apex_repeater_telemetry_t *tlm)
{
    if (!d || !tlm) return;
    d->telemetry = *tlm;
    d->telemetry_dirty = true;
}

apex_status_t apex_repeater_device_send_distal_tlm(apex_repeater_device_t *d,
                                                    uint8_t link_index,
                                                    const uint8_t *bytes, size_t len)
{
    if (!d || (!bytes && len > 0)) return APEX_ERR_INVALID_ARGS;
    if (link_index >= 4) return APEX_ERR_INVALID_ARGS;
    if (len > APEX_REPEATER_DISTAL_TLM_MAX) return APEX_ERR_BUFFER_TOO_SMALL;
    uint8_t buf[3 + APEX_REPEATER_DISTAL_TLM_MAX];
    buf[0] = APEX_RPT_MSG_DISTAL_TLM;
    buf[1] = link_index;
    buf[2] = d->config.c2[link_index].c2_protocol;  /* tag with the link's protocol */
    if (len > 0) memcpy(buf + 3, bytes, len);
    return dev_send(d, buf, 3 + len);
}

void apex_repeater_device_set_fault(apex_repeater_device_t *d)
{
    if (!d) return;
    if ((apex_repeater_state_t)d->telemetry.device_state == APEX_RPT_STATE_FAULT) return;
    d->telemetry.device_state = (uint8_t)APEX_RPT_STATE_FAULT;
    d->telemetry_dirty = true;
}

/* ===========================================================================
 * Host-side
 * ========================================================================= */

static apex_status_t host_send(apex_repeater_host_t *h, uint8_t device_id,
                               const uint8_t *payload, size_t len)
{
    return apex_host_send(h->core, device_id, APEX_TRAFFIC_REPEATER, payload, len);
}

static void host_handle_telemetry(apex_repeater_host_t *h, uint8_t device_id,
                                  const uint8_t *body, size_t body_len)
{
    if (body_len < 2) return;

    apex_repeater_telemetry_t tlm;
    memset(&tlm, 0, sizeof(tlm));
    size_t off = 0;

    tlm.device_state     = body[off++];
    tlm.capability_flags = body[off++];
    uint8_t cap = tlm.capability_flags;

    /* Initialise link blocks to "no signal" before filling in what's present. */
    for (int b = 0; b < 5; b++) {
        tlm.link_blocks[b].rssi_dbm = APEX_RPT_RSSI_NO_SIGNAL;
        tlm.link_blocks[b].snr_db   = APEX_RPT_RSSI_NO_SIGNAL;
    }

    /* Parse one link block per set bit in bits 0–4, in bit-index order. */
    for (int b = 0; b < 5; b++) {
        if (!(cap & (uint8_t)(1u << b))) continue;
        if (off + 5 > body_len) return;
        tlm.link_blocks[b].rssi_dbm     = (int8_t)body[off++];
        tlm.link_blocks[b].lq_percent   = body[off++];
        tlm.link_blocks[b].snr_db       = (int8_t)body[off++];
        tlm.link_blocks[b].tx_power_dbm = (int8_t)body[off++];
        tlm.link_blocks[b].flags        = body[off++];
    }

    /* Default per-antenna directionality to "all unknown". */
    for (unsigned a = 0; a < APEX_REPEATER_MAX_ANTENNAS; a++) {
        tlm.antenna_dir[a].antenna_bearing_deg = 0xFFFFu;
        tlm.antenna_dir[a].distal_bearing_deg  = 0xFFFFu;
        tlm.antenna_dir[a].distal_distance_m   = 0xFFFFu;
    }

    if (cap & APEX_RPT_CAP_ANTENNAS) {
        if (off + 1 > body_len) return;            /* missing antenna_count */
        uint8_t nant = body[off++];
        /* This build can only store MAX_ANTENNAS; a larger list is unparseable. */
        if (nant > APEX_REPEATER_MAX_ANTENNAS) return;
        if (off + (size_t)nant * 7u > body_len) return;  /* truncated blocks */
        tlm.antenna_count = nant;
        for (uint8_t a = 0; a < nant; a++) {
            tlm.antenna_dir[a].antenna_bearing_deg =
                (uint16_t)body[off] | ((uint16_t)body[off + 1] << 8); off += 2;
            tlm.antenna_dir[a].distal_bearing_deg =
                (uint16_t)body[off] | ((uint16_t)body[off + 1] << 8); off += 2;
            tlm.antenna_dir[a].distal_distance_m =
                (uint16_t)body[off] | ((uint16_t)body[off + 1] << 8); off += 2;
            tlm.antenna_dir[a].confidence = body[off++];
        }
    }

    if (h->hooks.on_telemetry) {
        h->hooks.on_telemetry(h->hooks.on_telemetry_user, device_id, &tlm);
    }
}

static void host_handle_distal_tlm(apex_repeater_host_t *h, uint8_t device_id,
                                   const uint8_t *body, size_t body_len)
{
    if (body_len < 2) return;  /* link_index + c2_protocol */
    uint8_t link_index  = body[0];
    uint8_t c2_protocol = body[1];
    const uint8_t *bytes = body + 2;
    size_t len = body_len - 2;
    if (h->hooks.on_distal_tlm) {
        h->hooks.on_distal_tlm(h->hooks.on_distal_tlm_user,
                               device_id, link_index, c2_protocol, bytes, len);
    }
}

static void host_handle_config_report(apex_repeater_host_t *h, uint8_t device_id,
                                      const uint8_t *body, size_t body_len)
{
    /* Minimum: capability_flags(1) + global_section(2). */
    if (body_len < 3) return;

    apex_repeater_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    size_t off = 0;

    cfg.capability_flags = body[off++];
    uint8_t cap = cfg.capability_flags;
    bool has_antennas = (cap & APEX_RPT_CAP_ANTENNAS) != 0;

    /* C2 link config sections: c2_protocol(1) + version(1) + len(1) + blob(N)
     * + (antenna_id(1) when an antenna list is present). */
    for (int b = 0; b < 4; b++) {
        if (!(cap & (uint8_t)(1u << b))) continue;
        if (off + 3 > body_len) return;
        cfg.c2[b].c2_protocol       = body[off++];
        cfg.c2[b].c2_config_version = body[off++];
        cfg.c2[b].c2_config_len     = body[off++];
        uint8_t blen = cfg.c2[b].c2_config_len;
        if (blen > APEX_REPEATER_C2_CONFIG_MAX) return;
        if (off + blen + (has_antennas ? 1u : 0u) > body_len) return;
        if (blen > 0) memcpy(cfg.c2[b].c2_config_blob, body + off, blen);
        off += blen;
        if (has_antennas) cfg.c2[b].antenna_id = body[off++];
    }

    /* Video section: center + bandwidth per direction, + antenna_id(1) when an
     * antenna list is present. */
    if (cap & APEX_RPT_CAP_VIDEO) {
        if (off + 11u + (has_antennas ? 1u : 0u) > body_len) return;
        cfg.video.rx_freq_mhz = (uint16_t)body[off] | ((uint16_t)body[off + 1] << 8); off += 2;
        cfg.video.rx_bw_mhz   = (uint16_t)body[off] | ((uint16_t)body[off + 1] << 8); off += 2;
        cfg.video.rx_format   = body[off++];
        cfg.video.tx_freq_mhz = (uint16_t)body[off] | ((uint16_t)body[off + 1] << 8); off += 2;
        cfg.video.tx_bw_mhz   = (uint16_t)body[off] | ((uint16_t)body[off + 1] << 8); off += 2;
        cfg.video.tx_power_dbm = (int8_t)body[off++];
        cfg.video.tx_format   = body[off++];
        if (has_antennas) cfg.video.antenna_id = body[off++];
    }

    /* Antenna list section: antenna_count(1) + per antenna type(1) + bearing_ref(1). */
    if (has_antennas) {
        if (off + 1 > body_len) return;
        uint8_t nant = body[off++];
        if (nant > APEX_REPEATER_MAX_ANTENNAS) return;
        if (off + (size_t)nant * 2u > body_len) return;
        cfg.antenna_count = nant;
        for (uint8_t a = 0; a < nant; a++) {
            cfg.antennas[a].antenna_type        = body[off++];
            cfg.antennas[a].antenna_bearing_ref = body[off++];
        }
    }

    /* Global section. */
    if (off + 2 > body_len) return;
    cfg.global.encryption_state   = body[off++];
    cfg.global.distal_tlm_rate_hz = body[off];

    if (h->hooks.on_config_report) {
        h->hooks.on_config_report(h->hooks.on_config_report_user, device_id, &cfg);
    }
}

static void host_handle_ack(apex_repeater_host_t *h, uint8_t device_id,
                            const uint8_t *body, size_t body_len)
{
    if (body_len < 3) return;
    apex_repeater_ack_t ack;
    ack.acked_msg      = body[0];
    ack.result         = (apex_repeater_ack_result_t)body[1];
    ack.failed_section = body[2];
    if (h->hooks.on_ack) {
        h->hooks.on_ack(h->hooks.on_ack_user, device_id, &ack);
    }
}

static void host_class_rx(void *user, uint8_t device_id,
                          const uint8_t *payload, size_t payload_len)
{
    apex_repeater_host_t *h = (apex_repeater_host_t *)user;
    if (payload_len < 1) return;
    uint8_t msg_id = payload[0];
    const uint8_t *body = payload + 1;
    size_t body_len = payload_len - 1;

    switch (msg_id) {
    case APEX_RPT_MSG_TELEMETRY:
        host_handle_telemetry(h, device_id, body, body_len);
        break;
    case APEX_RPT_MSG_DISTAL_TLM:
        host_handle_distal_tlm(h, device_id, body, body_len);
        break;
    case APEX_RPT_MSG_CONFIG_REPORT:
        host_handle_config_report(h, device_id, body, body_len);
        break;
    case APEX_RPT_MSG_ACK:
        host_handle_ack(h, device_id, body, body_len);
        break;
    default:
        /* GET_CONFIG, SET_CONFIG, ANTENNA_CMD are Host→Device; ignore. */
        break;
    }
}

apex_status_t apex_repeater_host_init(apex_repeater_host_t *h,
                                      apex_host_t *core,
                                      const apex_repeater_host_hooks_t *hooks)
{
    if (!h || !core) return APEX_ERR_INVALID_ARGS;
    memset(h, 0, sizeof(*h));
    h->core = core;
    if (hooks) h->hooks = *hooks;
    return apex_host_register_class(core, APEX_TRAFFIC_REPEATER,
                                    host_class_rx, h);
}

apex_status_t apex_repeater_host_get_config(apex_repeater_host_t *h,
                                            uint8_t device_id)
{
    uint8_t buf[1] = { APEX_RPT_MSG_GET_CONFIG };
    return host_send(h, device_id, buf, sizeof(buf));
}

apex_status_t apex_repeater_host_set_config(apex_repeater_host_t *h,
                                            uint8_t device_id,
                                            uint8_t update_mask,
                                            const apex_repeater_config_t *config)
{
    if (!h || !config) return APEX_ERR_INVALID_ARGS;
    /* Bit 7 is reserved. The antenna section (bit 5) is per-antenna and must go
     * through apex_repeater_host_set_antenna_ref() instead of this generic call. */
    if (update_mask & 0x80u) return APEX_ERR_INVALID_ARGS;
    if (update_mask & APEX_RPT_UPDATE_ANTENNA) return APEX_ERR_INVALID_ARGS;

    uint8_t buf[APEX_V0_MAX_PAYLOAD_LENGTH];
    size_t i = 0;
    buf[i++] = APEX_RPT_MSG_SET_CONFIG;
    buf[i++] = update_mask;

    /* C2 link config sections: version(1) + len(1) + blob(N).
     * c2_protocol is read-only and is not included in SET_CONFIG. */
    for (int b = 0; b < 4; b++) {
        if (!(update_mask & (uint8_t)(1u << b))) continue;
        const apex_repeater_c2_config_t *c2 = &config->c2[b];
        uint8_t blen = c2->c2_config_len;
        if (blen > APEX_REPEATER_C2_CONFIG_MAX) return APEX_ERR_INVALID_ARGS;
        if (i + 2u + blen > sizeof(buf)) return APEX_ERR_BUFFER_TOO_SMALL;
        buf[i++] = c2->c2_config_version;
        buf[i++] = blen;
        if (blen > 0) { memcpy(buf + i, c2->c2_config_blob, blen); i += blen; }
    }

    /* Video TX section: tx_freq_mhz(2) + tx_bw_mhz(2) + tx_power_dbm(1) + tx_format(1). */
    if (update_mask & APEX_RPT_UPDATE_VIDEO_TX) {
        if (i + 6 > sizeof(buf)) return APEX_ERR_BUFFER_TOO_SMALL;
        buf[i++] = (uint8_t)(config->video.tx_freq_mhz & 0xFFu);
        buf[i++] = (uint8_t)(config->video.tx_freq_mhz >> 8);
        buf[i++] = (uint8_t)(config->video.tx_bw_mhz & 0xFFu);
        buf[i++] = (uint8_t)(config->video.tx_bw_mhz >> 8);
        buf[i++] = (uint8_t)config->video.tx_power_dbm;
        buf[i++] = config->video.tx_format;
    }

    /* Antenna section (bit 5) is per-antenna and needs an antenna_id selector
     * that apex_repeater_config_t cannot express, so it is NOT built here — use
     * apex_repeater_host_set_antenna_ref(). Guarded against in the bit-7/mask
     * check below; here we simply never emit it. */

    /* Global section: distal_tlm_rate_hz(1). */
    if (update_mask & APEX_RPT_UPDATE_GLOBAL) {
        if (i + 1 > sizeof(buf)) return APEX_ERR_BUFFER_TOO_SMALL;
        buf[i++] = config->global.distal_tlm_rate_hz;
    }

    return host_send(h, device_id, buf, i);
}

apex_status_t apex_repeater_host_antenna_cmd(apex_repeater_host_t *h,
                                             uint8_t device_id,
                                             uint8_t antenna_id,
                                             uint16_t bearing_deg)
{
    if (!h) return APEX_ERR_INVALID_ARGS;
    /* Validate: 0–359 or BEARING_AUTO. */
    if (bearing_deg != APEX_RPT_BEARING_AUTO && bearing_deg > 359u) {
        return APEX_ERR_INVALID_ARGS;
    }
    uint8_t buf[4] = {
        APEX_RPT_MSG_ANTENNA_CMD,
        antenna_id,
        (uint8_t)(bearing_deg & 0xFFu),
        (uint8_t)(bearing_deg >> 8),
    };
    return host_send(h, device_id, buf, sizeof(buf));
}

apex_status_t apex_repeater_host_set_antenna_ref(apex_repeater_host_t *h,
                                                 uint8_t device_id,
                                                 uint8_t antenna_id,
                                                 uint8_t bearing_ref)
{
    if (!h) return APEX_ERR_INVALID_ARGS;
    uint8_t buf[4] = {
        APEX_RPT_MSG_SET_CONFIG,
        APEX_RPT_UPDATE_ANTENNA,
        antenna_id,
        bearing_ref,
    };
    return host_send(h, device_id, buf, sizeof(buf));
}
