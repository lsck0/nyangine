/**
 * @file steam.h
 *
 * The Steamworks flat API, behind NYA_PLUGIN_STEAM.
 *
 * A plugin rather than a module of the engine, and for the textbook reason: it wraps a vendored
 * third party library that most builds have no use for, and the library is a real cost — a shared
 * object that must be shipped beside the binary, and a running Steam client for any of it to do
 * anything. See plugins.h.
 *
 * It used to live in nyangine/steam and be included unconditionally by nyangine.h, while steam.c was
 * in no translation unit at all. So the declarations below were visible everywhere and defined
 * nowhere: calling one was a link error rather than a runtime failure, and the implementation was
 * never seen by a compiler. Being a plugin fixes both halves — with the flag off there are no
 * declarations to call, and with it on the code is compiled like anything else.
 *
 * ## Turning it on
 *
 * The flag alone is not enough; the redistributable has to be on the link line and next to the
 * executable at runtime. FLAGS_STEAM_LINUX_X86_64 and FLAGS_STEAM_WINDOWS_X86_64 in build/flags.h
 * carry both, and no build rule names either yet, so NYA_EXECUTION_MODE=3 remains unreachable until
 * one does.
 *
 * - Linux: "-L./vendor/steam/redistributable_bin/linux64/", "-Wl,-rpath,$ORIGIN", "-lsteam_api",
 *   and libsteam_api.so copied beside the binary.
 * - Windows: "-L./vendor/steam/redistributable_bin/win64/", "-lsteam_api64", and steam_api64.dll
 *   copied beside the executable. Windows has no rpath; the loader looks beside the .exe.
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
 *
 * `err_msg` may be null; otherwise `err_msg_capacity` is its size in bytes and the result is always
 * null terminated, truncated to fit. Steam's own buffer is 1024 bytes, so that is the most that will
 * ever be written.
 *
 * The capacity parameter is not decoration: this took a bare pointer and copied up to 1024 bytes
 * into it regardless of its size.
 * */
NYA_API NYA_SteamInitResult nya_system_steam_init(OUT NYA_CString err_msg, u64 err_msg_capacity) __attr_no_discard;
NYA_API void                nya_system_steam_deinit(void);

/*
 * ─────────────────────────────────────────────────────────
 * STEAM FUNCTIONS
 * ─────────────────────────────────────────────────────────
 */
