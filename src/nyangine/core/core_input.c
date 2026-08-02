#include "nyangine/nyangine.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API DECLARATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

void _nya_system_event_on_update_ended_hook(NYA_Event* event);

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PUBLIC API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/*
 * ─────────────────────────────────────────────────────────
 * SYSTEM FUNCTIONS
 * ─────────────────────────────────────────────────────────
 */

void nya_system_input_init(void) {
    NYA_App* app = nya_app_get();

    app->input_system = (NYA_InputSystem){
        .allocator = nya_arena_create(.name = "input_system_allocator"),
    };

    const u32  capacity                  = 300;
    NYA_Arena* allocator                 = app->input_system.allocator;
    app->input_system.keys_just_pressed  = nya_hmap_create_with_capacity(allocator, NYA_Keycode, b8, capacity);
    app->input_system.keys_pressed       = nya_hmap_create_with_capacity(allocator, NYA_Keycode, b8, capacity);
    app->input_system.keys_just_released = nya_hmap_create_with_capacity(allocator, NYA_Keycode, b8, capacity);

    nya_event_hook_register((NYA_EventHook){
        .event_type = NYA_EVENT_UPDATING_ENDED,
        .hook_type  = NYA_EVENT_HOOK_TYPE_IMMEDIATE,
        .fn         = nya_callback(_nya_system_event_on_update_ended_hook),
    });

    nya_info("Input system initialized.");
}

void nya_system_input_deinit(void) {
    NYA_App* app = nya_app_get();

    nya_hmap_destroy(app->input_system.keys_just_pressed);
    nya_hmap_destroy(app->input_system.keys_pressed);
    nya_hmap_destroy(app->input_system.keys_just_released);

    nya_arena_destroy(app->input_system.allocator);

    nya_info("Input system deinitialized.");
}

void nya_system_input_handle_event(NYA_Event* event) {
    nya_assert(event != nullptr);

    NYA_App* app = nya_app_get();

    if (event->type == NYA_EVENT_KEY_DOWN || event->type == NYA_EVENT_KEY_UP) {
        NYA_Keycode keycode    = event->as_key_event.key;
        b8          is_down    = event->as_key_event.is_down;
        b8*         is_pressed = nya_hmap_get(app->input_system.keys_pressed, keycode);

        if (is_down) {
            if (is_pressed == nullptr || !(*is_pressed)) nya_hmap_set(app->input_system.keys_just_pressed, keycode, true);
            nya_hmap_set(app->input_system.keys_pressed, keycode, true);
        } else {
            nya_hmap_set(app->input_system.keys_pressed, keycode, false);
            nya_hmap_set(app->input_system.keys_just_released, keycode, true);
        }
    }

    if (event->type == NYA_EVENT_MOUSE_BUTTON_DOWN || event->type == NYA_EVENT_MOUSE_BUTTON_UP) {
        NYA_MouseButton button     = event->as_mouse_button_event.button;
        b8              is_down    = event->as_mouse_button_event.is_down;
        b8*             is_pressed = &app->input_system.mouse_buttons_pressed[button];

        if (is_down) {
            if (!(*is_pressed)) app->input_system.mouse_buttons_just_pressed[button] = true;
            *is_pressed = true;
        } else {
            *is_pressed                                           = false;
            app->input_system.mouse_buttons_just_released[button] = true;
        }
    }

    if (event->type == NYA_EVENT_MOUSE_MOVED) {
        app->input_system.mouse_position = (f32x2){
            event->as_mouse_moved_event.x,
            event->as_mouse_moved_event.y,
        };

        app->input_system.mouse_position_delta += (f32x2){
            event->as_mouse_moved_event.delta_x,
            event->as_mouse_moved_event.delta_y,
        };
    }

    if (event->type == NYA_EVENT_MOUSE_WHEEL_MOVED) {
        app->input_system.mouse_wheel_delta += (f32x2){
            event->as_mouse_wheel_event.amount_x,
            event->as_mouse_wheel_event.amount_y,
        };
    }
}

/*
 * ─────────────────────────────────────────────────────────
 * INPUT FUNCTIONS
 * ─────────────────────────────────────────────────────────
 */

b8 nya_input_key_just_pressed(NYA_Keycode key) {
    NYA_App* app = nya_app_get();

    b8* just_pressed = nya_hmap_get(app->input_system.keys_just_pressed, key);
    return just_pressed != nullptr && *just_pressed;
}

b8 nya_input_key_pressed(NYA_Keycode key) {
    NYA_App* app = nya_app_get();

    b8* pressed = nya_hmap_get(app->input_system.keys_pressed, key);
    return pressed != nullptr && *pressed;
}

b8 nya_input_key_just_released(NYA_Keycode key) {
    NYA_App* app = nya_app_get();

    b8* just_released = nya_hmap_get(app->input_system.keys_just_released, key);
    return just_released != nullptr && *just_released;
}

f32x2 nya_input_mouse_position(void) {
    NYA_App* app = nya_app_get();

    return app->input_system.mouse_position;
}

f32x2 nya_input_mouse_position_delta(void) {
    NYA_App* app = nya_app_get();

    return app->input_system.mouse_position_delta;
}

f32x2 nya_input_mouse_wheel_scroll(void) {
    NYA_App* app = nya_app_get();

    return app->input_system.mouse_wheel_delta;
}

b8 nya_input_mouse_button_just_pressed(NYA_MouseButton button) {
    NYA_App* app = nya_app_get();

    return app->input_system.mouse_buttons_just_pressed[button];
}

b8 nya_input_mouse_button_pressed(NYA_MouseButton button) {
    NYA_App* app = nya_app_get();

    return app->input_system.mouse_buttons_pressed[button];
}

b8 nya_input_mouse_button_just_released(NYA_MouseButton button) {
    NYA_App* app = nya_app_get();

    return app->input_system.mouse_buttons_just_released[button];
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

void _nya_system_event_on_update_ended_hook(NYA_Event* event) {
    nya_assert(event != nullptr);
    nya_assert(event->type == NYA_EVENT_UPDATING_ENDED);

    NYA_App* app = nya_app_get();

    nya_hmap_clear(app->input_system.keys_just_pressed);
    nya_hmap_clear(app->input_system.keys_just_released);

    app->input_system.mouse_position_delta = f32x2_zero;
    app->input_system.mouse_wheel_delta    = f32x2_zero;

    nya_memset(app->input_system.mouse_buttons_just_pressed, 0, sizeof(b8) * NYA_MOUSE_BUTTON_COUNT);
    nya_memset(app->input_system.mouse_buttons_just_released, 0, sizeof(b8) * NYA_MOUSE_BUTTON_COUNT);
}
