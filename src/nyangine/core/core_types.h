/**
 * @file core_types.h
 * */
#pragma once

// NYA_EntityHandle is declared there rather than here: physics answers queries with one and core owns
// the table it indexes, and base is the lowest module both of them can see. See base_handle.h.
#include "nyangine/base/base_handle.h"
#include "nyangine/base/base_types.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * TYPES
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

typedef struct NYA_WindowHandle NYA_WindowHandle;
typedef enum NYA_InputDeviceKind NYA_InputDeviceKind;
typedef struct NYA_InputSource   NYA_InputSource;
typedef enum NYA_SocialProvider  NYA_SocialProvider;

/**
 * Which friends service an invite, a join or a presence card went through.
 *
 * Here rather than in core_social.h because the event that carries it (core_event.h) is declared before
 * the module that produces it, and a game switching on it should not have to include the module.
 * */
enum NYA_SocialProvider {
    NYA_SOCIAL_PROVIDER_NONE = 0,

    NYA_SOCIAL_PROVIDER_DISCORD,
    NYA_SOCIAL_PROVIDER_STEAM,

    NYA_SOCIAL_PROVIDER_COUNT,
};

/**
 * Identifies a window for as long as it exists. Same shape and reasoning as NYA_EntityHandle, and it
 * stays here because core is the only module that opens a window or names one.
 * */
struct NYA_WindowHandle {
    u32 index;
    u32 generation;
};

#define NYA_WINDOW_HANDLE_NONE ((NYA_WindowHandle){ .index = 0, .generation = 0 })

enum NYA_InputDeviceKind {
    /** No device, or the platform did not say which. What a zeroed NYA_InputSource is. */
    NYA_INPUT_DEVICE_KIND_NONE = 0,

    NYA_INPUT_DEVICE_KIND_KEYBOARD,
    NYA_INPUT_DEVICE_KIND_MOUSE,
    NYA_INPUT_DEVICE_KIND_GAMEPAD,

    NYA_INPUT_DEVICE_KIND_COUNT,
};

/**
 * Which physical or virtual device an input event came from, so several people can play on one
 * machine. With Steam Remote Play Together every remote player's input arrives as ordinary events on
 * the host, and without a source four players pressing W look like one.
 * */
struct NYA_InputSource {
    NYA_InputDeviceKind kind;
    u32                 id;
};

#define NYA_INPUT_SOURCE_NONE ((NYA_InputSource){ .kind = NYA_INPUT_DEVICE_KIND_NONE, .id = 0 })
