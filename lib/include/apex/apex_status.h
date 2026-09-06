/**
 * @file apex_status.h
 * @brief Return-code enum used across the APEX library, plus the host-managed
 *        device lifecycle status (APEX_Core.md §4).
 *
 * Copyright (c) 2026 Neros Technologies. MIT License — see LICENSE.
 */
#ifndef APEX_STATUS_H
#define APEX_STATUS_H

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    APEX_OK                  = 0,
    APEX_ERR_INVALID_ARGS    = -1,
    APEX_ERR_BUFFER_TOO_SMALL = -2,
    APEX_ERR_MALFORMED       = -3,
    APEX_ERR_BAD_CRC         = -4,
    APEX_ERR_UNSUPPORTED     = -5,
    APEX_ERR_BAD_STATE       = -6,
    APEX_ERR_NOT_FOUND       = -7,
    APEX_ERR_FULL            = -8,
} apex_status_t;

/* ---------------------------------------------------------------------------
 * Host-managed device lifecycle — §4 (ApexDeviceStatus_t).
 *
 * Device-level states the host tracks as devices come and go on the bus,
 * distinct from any class-specific state machine.
 * ------------------------------------------------------------------------- */

typedef enum {
    APEX_DEV_STATUS_UNKNOWN     = 0x00, /* Empty / recycled slot. */
    APEX_DEV_STATUS_PROVISIONAL = 0x01, /* Slot allocated, first CONFIG_REPLY
                                         * issued, ID not yet latched — covers
                                         * the whole provisional configuration
                                         * phase up to CONFIG_ACK (§3.3). */
    APEX_DEV_STATUS_CONNECTED   = 0x02, /* Device confirmed its ID; class
                                         * traffic flows. */
    APEX_DEV_STATUS_EXPENDED    = 0x03, /* Device signalled it is expended. */
    APEX_DEV_STATUS_FAULT       = 0xFF, /* Missed heartbeats or other error. */
} apex_device_status_t;

#ifdef __cplusplus
}
#endif

#endif /* APEX_STATUS_H */
