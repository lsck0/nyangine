/**
 * @file core_types.h
 *
 * Handle types shared across core.
 *
 * Split out because the modules that need them refer to each other: events carry a window handle,
 * and windows are defined in terms of the events they receive. Neither can include the other, so the
 * handles live here, below both.
 * */
#pragma once

#include "nyangine/base/base_types.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * TYPES
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

typedef struct NYA_WindowHandle NYA_WindowHandle;

/**
 * Identifies a window for as long as it exists, and stops identifying it the moment it does not.
 *
 * `generation` is bumped every time a slot is reused, so a handle kept across a window's destruction
 * fails to resolve rather than silently addressing its replacement.
 * */
struct NYA_WindowHandle {
    u32 index;
    u32 generation;
};

#define NYA_WINDOW_HANDLE_NONE ((NYA_WindowHandle){ .index = 0, .generation = 0 })
