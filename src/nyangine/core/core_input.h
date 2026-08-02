#pragma once

#include "nyangine/base/base.h"
#include "nyangine/base/base_hmap.h"
#include "nyangine/core/core_event.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * TYPES
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

typedef struct NYA_InputSystem NYA_InputSystem;
nya_derive_hmap(NYA_Keycode, b8);

struct NYA_InputSystem {
    NYA_Arena* allocator;

    NYA_HMapᐸNYA_Keycodeˏb8ᐳ* keys_just_pressed;
    NYA_HMapᐸNYA_Keycodeˏb8ᐳ* keys_pressed;
    NYA_HMapᐸNYA_Keycodeˏb8ᐳ* keys_just_released;

    f32x2 mouse_position;
    f32x2 mouse_position_delta;
    f32x2 mouse_wheel_delta;

    b8 mouse_buttons_just_pressed[NYA_MOUSE_BUTTON_COUNT];
    b8 mouse_buttons_pressed[NYA_MOUSE_BUTTON_COUNT];
    b8 mouse_buttons_just_released[NYA_MOUSE_BUTTON_COUNT];
};

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * FUNCTIONS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/*
 * ─────────────────────────────────────────────────────────
 * SYSTEM FUNCTIONS
 * ─────────────────────────────────────────────────────────
 */

NYA_API void nya_system_input_init(void);
NYA_API void nya_system_input_deinit(void);
NYA_API void nya_system_input_handle_event(NYA_Event* event);

/*
 * ─────────────────────────────────────────────────────────
 * INPUT FUNCTIONS
 * ─────────────────────────────────────────────────────────
 */

NYA_API b8 nya_input_key_just_pressed(NYA_Keycode key);
NYA_API b8 nya_input_key_pressed(NYA_Keycode key);
NYA_API b8 nya_input_key_just_released(NYA_Keycode key);

NYA_API f32x2 nya_input_mouse_position(void);
NYA_API f32x2 nya_input_mouse_position_delta(void);
NYA_API f32x2 nya_input_mouse_wheel_scroll(void);
NYA_API b8    nya_input_mouse_button_just_pressed(NYA_MouseButton button);
NYA_API b8    nya_input_mouse_button_pressed(NYA_MouseButton button);
NYA_API b8    nya_input_mouse_button_just_released(NYA_MouseButton button);
