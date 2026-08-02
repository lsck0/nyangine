#pragma once

#include "SDL3/SDL_mouse.h"

#include "nyangine/base/base_types.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * TYPES
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

typedef u8                           NYA_MouseButton;
typedef enum NYA_MouseButtonFlags    NYA_MouseButtonFlags;
typedef enum NYA_MouseWheelDirection NYA_MouseWheelDirection;

#define NYA_MOUSE_BUTTON_LEFT   1
#define NYA_MOUSE_BUTTON_MIDDLE 2
#define NYA_MOUSE_BUTTON_RIGHT  3
#define NYA_MOUSE_BUTTON_X1     4
#define NYA_MOUSE_BUTTON_X2     5
#define NYA_MOUSE_BUTTON_COUNT  6

enum NYA_MouseButtonFlags {
    NYA_MOUSE_BUTTON_FLAG_NONE   = 0,
    NYA_MOUSE_BUTTON_FLAG_LEFT   = (1 << 0),
    NYA_MOUSE_BUTTON_FLAG_MIDDLE = (1 << 1),
    NYA_MOUSE_BUTTON_FLAG_RIGHT  = (1 << 2),
    NYA_MOUSE_BUTTON_FLAG_X1     = (1 << 3),
    NYA_MOUSE_BUTTON_FLAG_X2     = (1 << 4),
};

enum NYA_MouseWheelDirection {
    NYA_MOUSE_WHEEL_DIRECTION_NORMAL  = SDL_MOUSEWHEEL_NORMAL,
    NYA_MOUSE_WHEEL_DIRECTION_FLIPPED = SDL_MOUSEWHEEL_FLIPPED,
};
