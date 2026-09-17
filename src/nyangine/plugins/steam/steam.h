/**
 * @file steam.h
 *
 * The Steamworks client connection. nya_app drives it from `NYA_AppOptions.steam_app_id`: relaunch through Steam when
 * started outside it, connect, pump callbacks once a frame, disconnect. Without a running client the game carries on
 * without Steam.
 *
 * A shipped depot must not contain steam_appid.txt, or a copy started outside Steam never relaunches through it. Put
 * one beside a development build to run it without the client.
 */
#pragma once

#include "nyangine/base/base_attributes.h"
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
 * True when Steam is relaunching the game through the client and this process should exit now. Call before anything
 * else is brought up.
 * */
NYA_API b8 nya_system_steam_restart_if_necessary(u32 app_id) __attr_no_discard;

/** Connects to the Steam client. Logs and returns the reason when it cannot; the game runs on without Steam. */
NYA_API NYA_SteamInitResult nya_system_steam_init(void);

/** Dispatches Steam's callbacks. Once a frame, on the main thread. Does nothing while not connected. */
NYA_API void nya_system_steam_update(void);

NYA_API void nya_system_steam_deinit(void);

/*
 * ─────────────────────────────────────────────────────────
 * STEAM FUNCTIONS
 * ─────────────────────────────────────────────────────────
 */

/** Whether the client connection is up. */
NYA_API b8 nya_steam_is_connected(void) __attr_no_discard;
