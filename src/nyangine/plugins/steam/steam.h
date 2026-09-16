/**
 * @file steam.h
 */
#pragma once

#include "nyangine/base/base_attributes.h"
#include "nyangine/base/base_string.h"
#include "nyangine/base/base_types.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * TYPES
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

typedef enum {
    NYA_SYSTEM_STEAM_INIT_OK               = 0,
    NYA_SYSTEM_STEAM_INIT_FAILED_GENERIC   = 1,
    NYA_SYSTEM_STEAM_INIT_NO_STEAM_CLIENT  = 2,
    NYA_SYSTEM_STEAM_INIT_VERSION_MISMATCH = 3,
    NYA_SYSTEM_STEAM_INIT_COUNT,
} NYA_SteamInitResult;

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * FUNCTIONS AND MACROS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/*
 * ─────────────────────────────────────────────────────────
 * SYSTEM FUNCTIONS
 * ─────────────────────────────────────────────────────────
 */

/**
 * Initialises the Steam API. Writes a diagnostic into `err_msg` on failure.
 * */
NYA_API NYA_SteamInitResult nya_system_steam_init(OUT NYA_CString err_msg, u64 err_msg_capacity) __attr_no_discard;
NYA_API void                nya_system_steam_deinit(void);

/*
 * ─────────────────────────────────────────────────────────
 * STEAM FUNCTIONS
 * ─────────────────────────────────────────────────────────
 */
