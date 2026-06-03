/**
 * @file apex_status.h
 * @brief Return-code enum used across the APEX library.
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

#ifdef __cplusplus
}
#endif

#endif /* APEX_STATUS_H */
