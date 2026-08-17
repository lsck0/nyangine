/**
 * @file core_types.h
 *
 * Handle types shared across core.
 *
 * Split out because the modules that need them refer to each other: events carry a window handle,
 * and windows are defined in terms of the events they receive. Neither can include the other, so the
 * handles live here, below both. NYA_InputSource is here for the same reason — an event carries one
 * and the input system routes on one, and core_input.h is what includes core_event.h rather than the
 * other way round.
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
typedef enum NYA_InputDeviceKind NYA_InputDeviceKind;
typedef struct NYA_InputSource   NYA_InputSource;

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

enum NYA_InputDeviceKind {
    /** No device, or the platform did not say which. What a zeroed NYA_InputSource is. */
    NYA_INPUT_DEVICE_KIND_NONE = 0,

    NYA_INPUT_DEVICE_KIND_KEYBOARD,
    NYA_INPUT_DEVICE_KIND_MOUSE,

    NYA_INPUT_DEVICE_KIND_COUNT,
};

/**
 * Which physical (or virtual) device an input event came from.
 *
 * The engine's answer to "several people are playing on one machine". Steam Remote Play Together is
 * the case that motivates it: the host streams the game out and every remote player's keyboard and
 * mouse arrive back as ordinary input events on the host. Without a source on the event, four
 * players pressing W are indistinguishable from one player pressing W four times.
 *
 * `id` is the platform's instance id, carried through untouched. It is only unique within a `kind` —
 * keyboard 1 and mouse 1 are different devices — so the pair is the identity and the id alone is not.
 *
 * ## Zero means "the platform did not say", and that is common
 *
 * SDL fills `which` in only when it can. Its own headers say so: a keyboard event's id is "0 if
 * unknown or virtual", and a mouse event's is the instance id "in relative mode ... or 0". So on a
 * desktop that is not in relative mouse mode, every mouse event reports source id zero, and on
 * platforms without per-device keyboard support so does every key.
 *
 * That is why zero is a valid, routable source rather than an error: it is the ordinary
 * single-player case, it routes to whichever player it is assigned to like any other device, and it
 * is what everything falls back to. Nothing here promises the platform *can* tell two keyboards
 * apart — it promises that when the platform can, the engine does not throw the answer away.
 * */
struct NYA_InputSource {
    NYA_InputDeviceKind kind;
    u32                 id;
};

#define NYA_INPUT_SOURCE_NONE ((NYA_InputSource){ .kind = NYA_INPUT_DEVICE_KIND_NONE, .id = 0 })
