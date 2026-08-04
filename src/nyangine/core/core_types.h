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
typedef struct NYA_EntityHandle NYA_EntityHandle;

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

/**
 * Identifies an entity for as long as it lives, and stops identifying it once it does not.
 *
 * Same shape and same reasoning as NYA_WindowHandle, and the reason it matters more here: entities
 * refer to each other constantly, and deferred simulation commands hold references across a barrier.
 * A raw index would silently address whichever entity next occupied the slot. Bumping `generation`
 * on despawn turns that into a lookup that returns null.
 * */
struct NYA_EntityHandle {
    u32 index;
    u32 generation;
};

#define NYA_ENTITY_HANDLE_NONE ((NYA_EntityHandle){ .index = 0, .generation = 0 })
