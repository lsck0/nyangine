/**
 * @file steam.h
 *
 * THIS IS NOT YET USED.
 * To properly use the Steamworks SDK, a few flags have to be added in the build system as well as the corresponding
 * steam_api dll has to be shipped too.
 *
 * - Linux Flags:
 * "-L./vendor/steam/redistributable_bin/linux64/", "-Wl,-rpath,$ORIGIN/vendor/steam/redistributable_bin/linux64",
 * "-lsteam_api"
 *
 * - Windows Flags:
 * "-L./vendor/steam/redistributable_bin/win64/", "-Wl,-rpath,$ORIGIN/vendor/steam/redistributable_bin/win64",
 * "-lsteam_api64"
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

NYA_API NYA_SteamInitResult nya_system_steam_init(OUT NYA_CString err_msg) __attr_no_discard;
NYA_API void                nya_system_steam_deinit(void);

/*
 * ─────────────────────────────────────────────────────────
 * STEAM FUNCTIONS
 * ─────────────────────────────────────────────────────────
 */
