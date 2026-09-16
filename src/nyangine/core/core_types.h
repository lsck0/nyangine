/**
 * @file core_types.h
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
 * Identifies a window for as long as it exists.
 * */
struct NYA_WindowHandle {
    u32 index;
    u32 generation;
};

#define NYA_WINDOW_HANDLE_NONE ((NYA_WindowHandle){ .index = 0, .generation = 0 })

/**
 * Identifies an entity for as long as it lives. Same shape and reasoning as NYA_WindowHandle, and it
 * matters more here: entities refer to each other constantly, and deferred simulation commands hold
 * references across a barrier, so bumping `generation` on despawn is what keeps a stale reference from
 * silently addressing whichever entity next occupied the slot.
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
    NYA_INPUT_DEVICE_KIND_GAMEPAD,

    NYA_INPUT_DEVICE_KIND_COUNT,
};

/**
 * Which physical (or virtual) device an input event came from — the engine's answer to "several
 * people are playing on one machine". Steam Remote Play Together is the motivating case: the host
 * streams the game out and every remote player's keyboard and mouse arrive back as ordinary input
 * events on the host, so without a source on the event, four players pressing W are indistinguishable
 * from one player pressing W four times.
 * */
struct NYA_InputSource {
    NYA_InputDeviceKind kind;
    u32                 id;
};

#define NYA_INPUT_SOURCE_NONE ((NYA_InputSource){ .kind = NYA_INPUT_DEVICE_KIND_NONE, .id = 0 })
