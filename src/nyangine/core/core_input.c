#include "nyangine/nyangine.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API DECLARATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

void _nya_system_event_on_update_ended_hook(NYA_Event* event);

/** Whether the modifiers a binding asks for are exactly the ones being held. */
NYA_INTERNAL b8 _nya_input_modifiers_match(NYA_KeyModFlag required) __attr_no_discard;

/** The binding table slot for an action, asserting the action is one that can be bound. */
NYA_INTERNAL NYA_InputBinding* _nya_input_bindings_for(NYA_InputAction action) __attr_no_discard;

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
        // Taken from the event rather than derived from which modifier keycodes are down: the
        // platform already tracks lock states and AltGr, which no amount of watching key presses
        // reconstructs correctly.
        app->input_system.modifier_flags = event->as_key_event.modifier_flags;

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

NYA_KeyModFlag nya_input_modifiers(void) {
    return nya_app_get()->input_system.modifier_flags;
}

/*
 * ─────────────────────────────────────────────────────────
 * ACTION FUNCTIONS
 * ─────────────────────────────────────────────────────────
 */

void nya_input_action_bind(NYA_InputAction action, NYA_Keycode key) __attr_overloaded {
    nya_input_action_bind(action, key, NYA_KEYMOD_NONE);
}

void nya_input_action_bind(NYA_InputAction action, NYA_Keycode key, NYA_KeyModFlag modifiers) __attr_overloaded {
    nya_assert(key != NYA_KEY_UNKNOWN, "Cannot bind NYA_KEY_UNKNOWN; it is the unbound marker.");

    NYA_InputBinding* bindings = _nya_input_bindings_for(action);

    // Already bound to this key: update the modifiers rather than spending a second slot on it.
    for (u32 i = 0; i < NYA_INPUT_BINDINGS_PER_ACTION; i++) {
        if (bindings[i].key != key) continue;

        bindings[i].modifiers = modifiers;
        return;
    }

    for (u32 i = 0; i < NYA_INPUT_BINDINGS_PER_ACTION; i++) {
        if (bindings[i].key != NYA_KEY_UNKNOWN) continue;

        bindings[i] = (NYA_InputBinding){ .key = key, .modifiers = modifiers };
        return;
    }

    // Full. Replacing the last is friendlier than dropping the request on the floor, and a rebinding
    // screen that offers more alternatives than there are slots is the caller's bug to notice.
    nya_warn("Action %d already has %d bindings; replacing the last.", (int)action, NYA_INPUT_BINDINGS_PER_ACTION);
    bindings[NYA_INPUT_BINDINGS_PER_ACTION - 1] = (NYA_InputBinding){ .key = key, .modifiers = modifiers };
}

void nya_input_action_rebind(NYA_InputAction action, NYA_Keycode key) __attr_overloaded {
    nya_input_action_rebind(action, key, NYA_KEYMOD_NONE);
}

void nya_input_action_rebind(NYA_InputAction action, NYA_Keycode key, NYA_KeyModFlag modifiers) __attr_overloaded {
    nya_assert(key != NYA_KEY_UNKNOWN, "Cannot bind NYA_KEY_UNKNOWN; use nya_input_action_unbind to clear an action.");

    nya_input_action_unbind(action);
    nya_input_action_bind(action, key, modifiers);
}

void nya_input_action_set(NYA_InputAction action, u32 slot, NYA_Keycode key, NYA_KeyModFlag modifiers) {
    nya_assert(slot < NYA_INPUT_BINDINGS_PER_ACTION, "Binding slot %u is past NYA_INPUT_BINDINGS_PER_ACTION.", slot);

    NYA_InputBinding* bindings = _nya_input_bindings_for(action);

    // A cleared slot must not keep stale modifiers, or rebinding it later would inherit them.
    bindings[slot] = key == NYA_KEY_UNKNOWN ? (NYA_InputBinding){ 0 } : (NYA_InputBinding){ .key = key, .modifiers = modifiers };
}

NYA_InputBinding nya_input_action_get(NYA_InputAction action, u32 slot) {
    nya_assert(slot < NYA_INPUT_BINDINGS_PER_ACTION, "Binding slot %u is past NYA_INPUT_BINDINGS_PER_ACTION.", slot);

    return _nya_input_bindings_for(action)[slot];
}

void nya_input_action_unbind(NYA_InputAction action) {
    NYA_InputBinding* bindings = _nya_input_bindings_for(action);

    nya_memset(bindings, 0, sizeof(NYA_InputBinding) * NYA_INPUT_BINDINGS_PER_ACTION);
}

b8 nya_input_action_bound(NYA_InputAction action) {
    NYA_InputBinding* bindings = _nya_input_bindings_for(action);

    for (u32 i = 0; i < NYA_INPUT_BINDINGS_PER_ACTION; i++) {
        if (bindings[i].key != NYA_KEY_UNKNOWN) return true;
    }

    return false;
}

b8 nya_input_action_just_pressed(NYA_InputAction action) {
    NYA_InputBinding* bindings = _nya_input_bindings_for(action);

    for (u32 i = 0; i < NYA_INPUT_BINDINGS_PER_ACTION; i++) {
        if (bindings[i].key == NYA_KEY_UNKNOWN) continue;
        if (!nya_input_key_just_pressed(bindings[i].key)) continue;
        if (!_nya_input_modifiers_match(bindings[i].modifiers)) continue;

        return true;
    }

    return false;
}

b8 nya_input_action_pressed(NYA_InputAction action) {
    NYA_InputBinding* bindings = _nya_input_bindings_for(action);

    for (u32 i = 0; i < NYA_INPUT_BINDINGS_PER_ACTION; i++) {
        if (bindings[i].key == NYA_KEY_UNKNOWN) continue;
        if (!nya_input_key_pressed(bindings[i].key)) continue;
        if (!_nya_input_modifiers_match(bindings[i].modifiers)) continue;

        return true;
    }

    return false;
}

b8 nya_input_action_just_released(NYA_InputAction action) {
    NYA_InputBinding* bindings = _nya_input_bindings_for(action);

    for (u32 i = 0; i < NYA_INPUT_BINDINGS_PER_ACTION; i++) {
        if (bindings[i].key == NYA_KEY_UNKNOWN) continue;

        // No modifier check here on purpose; see the note in core_input.h. Releasing Ctrl before the
        // key it modified is the normal way to end a chord.
        if (nya_input_key_just_released(bindings[i].key)) return true;
    }

    return false;
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

NYA_InputBinding* _nya_input_bindings_for(NYA_InputAction action) {
    nya_assert(action != NYA_INPUT_ACTION_NONE, "NYA_INPUT_ACTION_NONE is the unbound action and cannot carry a binding.");
    nya_assert(action < NYA_INPUT_ACTION_MAX, "Action %d is past NYA_INPUT_ACTION_MAX.", (int)action);

    return nya_settings()->bindings[action];
}

b8 _nya_input_modifiers_match(NYA_KeyModFlag required) {
    /*
     * Compared a group at a time, so a binding can ask for either Ctrl or specifically left Ctrl.
     *
     * A group the binding does not ask for must not be held: that is what stops a bare W firing
     * while Ctrl+W is being pressed. A group it does ask for must be held on at least one of the
     * sides requested, so NYA_KEYMOD_CTRL accepts either and NYA_KEYMOD_LCTRL accepts only the left.
     *
     * Lock keys are excluded entirely. Caps Lock being on is a state of the keyboard, not part of a
     * chord, and requiring it off would make half the bindings in a game stop working.
     */
    const NYA_KeyModFlag groups[] = { NYA_KEYMOD_CTRL, NYA_KEYMOD_SHIFT, NYA_KEYMOD_ALT, NYA_KEYMOD_GUI };

    NYA_KeyModFlag current = nya_app_get()->input_system.modifier_flags;

    for (u32 i = 0; i < sizeof(groups) / sizeof(groups[0]); i++) {
        NYA_KeyModFlag wanted = required & groups[i];
        NYA_KeyModFlag held   = current & groups[i];

        if (wanted == 0) {
            if (held != 0) return false; // a modifier is down that this binding does not want
        } else if ((held & wanted) == 0) {
            return false; // the side it asked for is not down
        }
    }

    return true;
}

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
