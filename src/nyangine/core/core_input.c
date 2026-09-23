#include "nyangine/nyangine.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API DECLARATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/*
 * Not NYA_INTERNAL: nya_callback resolves it by name with dlsym after a hot reload, and a hidden symbol is not
 * exported by -rdynamic.
 */
// NOLINTNEXTLINE(misc-use-internal-linkage)
void _nya_system_event_on_update_ended_hook(NYA_Event* event);

/**
 * Whether the modifiers a binding asks for are exactly the ones in `current`.
 * */
NYA_INTERNAL b8 _nya_input_modifiers_match_against(NYA_KeyModFlag required, NYA_KeyModFlag current) __attr_no_discard;

/** The binding table slot for an action, asserting the action is one that can be bound. */
NYA_INTERNAL NYA_InputBinding* _nya_input_bindings_for(NYA_InputAction action) __attr_no_discard;

/**
 * The state a query should read, or null when the slot is unclaimed.
 * */
NYA_INTERNAL NYA_InputState* _nya_input_state_for(u32 player) __attr_no_discard;

/** The roster entry for `source`, or null when it has never produced an event. */
NYA_INTERNAL NYA_InputSourceBinding* _nya_input_source_find(NYA_InputSource source) __attr_no_discard;

/**
 * The roster entry for `source`, adding it as unclaimed if this is the first time it is seen.
 * */
NYA_INTERNAL NYA_InputSourceBinding* _nya_input_source_intern(NYA_InputSource source) __attr_no_discard;

/** Allocates a player's tables the first time a device is routed to that slot. */
NYA_INTERNAL NYA_InputState* _nya_input_player_claim(u32 player) __attr_no_discard;

/** Allocates the three key tables of a state into `allocator`. */
NYA_INTERNAL void _nya_input_state_init(NYA_InputState* state, NYA_Arena* allocator);

/** Frees the three key tables. The state struct itself belongs to the arena. */
NYA_INTERNAL void _nya_input_state_deinit(NYA_InputState* state);

/** Folds one event into one state. Called once for the merged view and once for the routed player. */
NYA_INTERNAL void _nya_input_state_handle_event(NYA_InputState* state, const NYA_Event* event);

/** Drops the per frame edges: just pressed, just released, and the two deltas. */
NYA_INTERNAL void _nya_input_state_end_frame(NYA_InputState* state);
NYA_INTERNAL void _nya_input_text_handle_event(NYA_InputSystem* system, const NYA_Event* event);

/** The source an event carries, or NYA_INPUT_SOURCE_NONE for an event that carries none. */
NYA_INTERNAL NYA_InputSource _nya_input_event_source(const NYA_Event* event) __attr_no_discard;

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

    _nya_input_state_init(&app->input_system.merged, app->input_system.allocator);

    app->input_system.last_source = NYA_INPUT_SOURCE_NONE;

    // guarded, so restarting the app in one process does not register duplicates.
    static b8 ceiling_registered = false;
    if (!ceiling_registered) {
        nya_ceiling_register("input_sources", NYA_INPUT_MAX_SOURCES, &app->input_system.source_count);
        ceiling_registered = true;
    }

    // slots start unclaimed; nya_input_source_assign allocates them, so a single-player game pays for none.
    for (u32 player = 0; player < NYA_INPUT_MAX_PLAYERS; player++) app->input_system.players[player] = (NYA_InputState){ 0 };

    // named so a settings file can store a rebound Confirm. a game's actions must name themselves too, or they are
    // not persisted.
    nya_input_action_name_set(NYA_INPUT_ACTION_CONFIRM, "confirm");
    nya_input_action_name_set(NYA_INPUT_ACTION_CANCEL, "cancel");
    nya_input_action_name_set(NYA_INPUT_ACTION_PAUSE, "pause");
    nya_input_action_name_set(NYA_INPUT_ACTION_UP, "up");
    nya_input_action_name_set(NYA_INPUT_ACTION_DOWN, "down");
    nya_input_action_name_set(NYA_INPUT_ACTION_LEFT, "left");
    nya_input_action_name_set(NYA_INPUT_ACTION_RIGHT, "right");

    nya_event_hook_register((NYA_EventHook){
        .event_type = NYA_EVENT_UPDATING_ENDED,
        .hook_type  = NYA_EVENT_HOOK_TYPE_IMMEDIATE,
        .fn         = nya_callback(_nya_system_event_on_update_ended_hook),
    });

    nya_log_info("Input system initialized.");
}

void nya_system_input_deinit(void) {
    NYA_App* app = nya_app_get();

    _nya_input_state_deinit(&app->input_system.merged);

    for (u32 player = 0; player < NYA_INPUT_MAX_PLAYERS; player++) {
        if (app->input_system.players[player].keys_pressed == nullptr) continue;

        _nya_input_state_deinit(&app->input_system.players[player]);
    }

    // the key tables came from this arena.
    nya_arena_destroy(app->input_system.allocator);

    nya_log_info("Input system deinitialized.");
}

void nya_system_input_handle_event(NYA_Event* event) {
    nya_assert(event != nullptr);

    NYA_InputSystem* system = &nya_app_get()->input_system;

    /*
     * Every event goes to the merged view, and also to the routed player's view if there is one. Menus and the
     * single-player API read the merged view, so it must keep seeing assigned devices.
     */
    _nya_input_text_handle_event(system, event);

    _nya_input_state_handle_event(&system->merged, event);

    NYA_InputSource source = _nya_input_event_source(event);
    if (source.kind == NYA_INPUT_DEVICE_KIND_NONE) return;

    system->last_source = source;

    NYA_InputSourceBinding* binding = _nya_input_source_intern(source);
    if (binding == nullptr || binding->player >= NYA_INPUT_MAX_PLAYERS) return;

    NYA_InputState* state = _nya_input_state_for(binding->player);
    if (state == nullptr) return;

    _nya_input_state_handle_event(state, event);
}

/*
 * ─────────────────────────────────────────────────────────
 * INPUT FUNCTIONS
 * ─────────────────────────────────────────────────────────
 */

b8 nya_input_key_just_pressed(NYA_Keycode key) {
    return nya_input_key_just_pressed_by(NYA_INPUT_PLAYER_ANY, key);
}

b8 nya_input_key_pressed(NYA_Keycode key) {
    return nya_input_key_pressed_by(NYA_INPUT_PLAYER_ANY, key);
}

b8 nya_input_key_just_released(NYA_Keycode key) {
    return nya_input_key_just_released_by(NYA_INPUT_PLAYER_ANY, key);
}

NYA_KeyModFlag nya_input_modifiers(void) {
    return nya_input_modifiers_by(NYA_INPUT_PLAYER_ANY);
}

/*
 * The per-player forms. The single-player queries above are the NYA_INPUT_PLAYER_ANY case, so each question
 * has one implementation.
 */

b8 nya_input_key_just_pressed_by(u32 player, NYA_Keycode key) {
    NYA_InputState* state = _nya_input_state_for(player);
    if (state == nullptr) return false;

    b8* just_pressed = nya_hmap_get(state->keys_just_pressed, key);
    return just_pressed != nullptr && *just_pressed;
}

b8 nya_input_key_pressed_by(u32 player, NYA_Keycode key) {
    NYA_InputState* state = _nya_input_state_for(player);
    if (state == nullptr) return false;

    b8* pressed = nya_hmap_get(state->keys_pressed, key);
    return pressed != nullptr && *pressed;
}

b8 nya_input_key_just_released_by(u32 player, NYA_Keycode key) {
    NYA_InputState* state = _nya_input_state_for(player);
    if (state == nullptr) return false;

    b8* just_released = nya_hmap_get(state->keys_just_released, key);
    return just_released != nullptr && *just_released;
}

NYA_KeyModFlag nya_input_modifiers_by(u32 player) {
    NYA_InputState* state = _nya_input_state_for(player);
    if (state == nullptr) return NYA_KEYMOD_NONE;

    return state->modifier_flags;
}

/*
 * ─────────────────────────────────────────────────────────
 * SOURCE AND PLAYER FUNCTIONS
 * ─────────────────────────────────────────────────────────
 */

NYA_InputSource nya_input_source_last(void) {
    return nya_app_get()->input_system.last_source;
}

u32 nya_input_source_count(void) {
    return nya_app_get()->input_system.source_count;
}

NYA_InputSource nya_input_source_at(u32 index) {
    NYA_InputSystem* system = &nya_app_get()->input_system;

    if (index >= system->source_count) return NYA_INPUT_SOURCE_NONE;

    return system->sources[index].source;
}

u32 nya_input_source_player(NYA_InputSource source) {
    NYA_InputSourceBinding* binding = _nya_input_source_find(source);

    // unseen reads the same as unassigned, which a join screen wants.
    if (binding == nullptr) return NYA_INPUT_PLAYER_NONE;

    return binding->player;
}

void nya_input_source_assign(NYA_InputSource source, u32 player) {
    nya_assert(player < NYA_INPUT_MAX_PLAYERS, "Player %u is past NYA_INPUT_MAX_PLAYERS.", player);
    nya_assert(source.kind != NYA_INPUT_DEVICE_KIND_NONE, "NYA_INPUT_SOURCE_NONE is not a device and cannot be assigned.");

    NYA_InputSourceBinding* binding = _nya_input_source_intern(source);

    if (binding == nullptr) {
        // roster full: reported, not asserted, since device count is outside the game's control. the merged view still
        // gets the events.
        nya_log_warn("Cannot assign input source (kind %d, id %u) to player %u: already tracking %d sources.", (int)source.kind, source.id, player,
                 NYA_INPUT_MAX_SOURCES);
        return;
    }

    // before the write, so an unclaimable slot leaves routing alone.
    if (_nya_input_player_claim(player) == nullptr) return;

    binding->player = player;
}

void nya_input_source_release(NYA_InputSource source) {
    NYA_InputSourceBinding* binding = _nya_input_source_find(source);
    if (binding == nullptr) return;

    binding->player = NYA_INPUT_PLAYER_NONE;
}

void nya_input_players_reset(void) {
    NYA_InputSystem* system = &nya_app_get()->input_system;

    for (u32 i = 0; i < system->source_count; i++) system->sources[i].player = NYA_INPUT_PLAYER_NONE;

    /*
     * Slots are torn down, not just unrouted: a held key would carry over to whoever reuses the slot. The roster
     * is kept, since the devices are still plugged in.
     */
    for (u32 player = 0; player < NYA_INPUT_MAX_PLAYERS; player++) {
        if (system->players[player].keys_pressed == nullptr) continue;

        _nya_input_state_deinit(&system->players[player]);
    }
}

/*
 * ─────────────────────────────────────────────────────────
 * ACTION FUNCTIONS
 * ─────────────────────────────────────────────────────────
 */

/** Which edge a query is asking about, so one gamepad walk serves all three. */
typedef enum {
    _NYA_INPUT_EDGE_PRESSED = 0,
    _NYA_INPUT_EDGE_JUST_PRESSED,
    _NYA_INPUT_EDGE_JUST_RELEASED,
} _NYA_InputEdge;

/**
 * Whether any connected pad satisfies `binding` at `edge`.
 */
NYA_INTERNAL b8 _nya_input_binding_gamepad_edge(NYA_InputBinding binding, _NYA_InputEdge edge) {
    for (u32 i = 0; i < nya_gamepad_count(); i++) {
        NYA_GamepadId pad = nya_gamepad_at(i);

        b8 hit = false;

        switch (binding.kind) {
            case NYA_INPUT_BINDING_GAMEPAD_BUTTON: {
                switch (edge) {
                    case _NYA_INPUT_EDGE_PRESSED: hit = nya_gamepad_button_pressed(pad, binding.button); break;
                    case _NYA_INPUT_EDGE_JUST_PRESSED: hit = nya_gamepad_button_just_pressed(pad, binding.button); break;
                    case _NYA_INPUT_EDGE_JUST_RELEASED: hit = nya_gamepad_button_just_released(pad, binding.button); break;
                    default: break;
                }
            } break;

            case NYA_INPUT_BINDING_GAMEPAD_AXIS: {
                switch (edge) {
                    case _NYA_INPUT_EDGE_PRESSED: hit = nya_gamepad_axis_pressed(pad, binding.axis, binding.axis_threshold); break;
                    case _NYA_INPUT_EDGE_JUST_PRESSED: hit = nya_gamepad_axis_just_pressed(pad, binding.axis, binding.axis_threshold); break;
                    case _NYA_INPUT_EDGE_JUST_RELEASED: hit = nya_gamepad_axis_just_released(pad, binding.axis, binding.axis_threshold); break;
                    default: break;
                }
            } break;

            default: break;
        }

        if (hit) return true;
    }

    return false;
}

b8 nya_input_binding_gamepad_pressed(NYA_InputBinding binding) {
    return _nya_input_binding_gamepad_edge(binding, _NYA_INPUT_EDGE_PRESSED);
}

/** Takes the next free slot for a non-keyboard binding, replacing the last when full. */
NYA_INTERNAL void _nya_input_action_bind_slot(NYA_InputAction action, NYA_InputBinding binding) {
    NYA_InputBinding* bindings = _nya_input_bindings_for(action);

    for (u32 i = 0; i < NYA_INPUT_BINDINGS_PER_ACTION; i++) {
        if (bindings[i].kind != NYA_INPUT_BINDING_NONE) continue;

        bindings[i] = binding;
        return;
    }

    nya_log_warn("Action %d already has %d bindings; replacing the last.", (int)action, NYA_INPUT_BINDINGS_PER_ACTION);
    bindings[NYA_INPUT_BINDINGS_PER_ACTION - 1] = binding;
}

void nya_input_action_bind_button(NYA_InputAction action, NYA_GamepadButton button) {
    _nya_input_action_bind_slot(action, (NYA_InputBinding){ .kind = NYA_INPUT_BINDING_GAMEPAD_BUTTON, .button = button });
}

void nya_input_action_bind_axis(NYA_InputAction action, NYA_GamepadAxis axis, f32 threshold) {
    _nya_input_action_bind_slot(action,
                                (NYA_InputBinding){ .kind = NYA_INPUT_BINDING_GAMEPAD_AXIS, .axis = axis, .axis_threshold = threshold });
}

void nya_input_action_bind(NYA_InputAction action, NYA_Keycode key) __attr_overloaded {
    nya_input_action_bind(action, key, NYA_KEYMOD_NONE);
}

void nya_input_action_bind(NYA_InputAction action, NYA_Keycode key, NYA_KeyModFlag modifiers) __attr_overloaded {
    nya_assert(key != NYA_KEY_UNKNOWN, "Cannot bind NYA_KEY_UNKNOWN; it is the unbound marker.");

    NYA_InputBinding* bindings = _nya_input_bindings_for(action);

    // Already bound to this key: update the modifiers rather than spending a second slot on it.
    for (u32 i = 0; i < NYA_INPUT_BINDINGS_PER_ACTION; i++) {
        if (bindings[i].kind != NYA_INPUT_BINDING_KEY || bindings[i].key != key) continue;

        bindings[i].modifiers = modifiers;
        return;
    }

    for (u32 i = 0; i < NYA_INPUT_BINDINGS_PER_ACTION; i++) {
        if (bindings[i].kind != NYA_INPUT_BINDING_NONE) continue;

        bindings[i] = (NYA_InputBinding){ .kind = NYA_INPUT_BINDING_KEY, .key = key, .modifiers = modifiers };
        return;
    }

    // full: the last slot is replaced rather than the request dropped.
    nya_log_warn("Action %d already has %d bindings; replacing the last.", (int)action, NYA_INPUT_BINDINGS_PER_ACTION);
    bindings[NYA_INPUT_BINDINGS_PER_ACTION - 1] = (NYA_InputBinding){ .kind = NYA_INPUT_BINDING_KEY, .key = key, .modifiers = modifiers };
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

    // a cleared slot drops its modifiers too.
    bindings[slot] = key == NYA_KEY_UNKNOWN
                         ? (NYA_InputBinding){ 0 }
                         : (NYA_InputBinding){ .kind = NYA_INPUT_BINDING_KEY, .key = key, .modifiers = modifiers };
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

    // the tag, not the key: a gamepad-only binding has a zero key.
    for (u32 i = 0; i < NYA_INPUT_BINDINGS_PER_ACTION; i++) {
        if (bindings[i].kind != NYA_INPUT_BINDING_NONE) return true;
    }

    return false;
}

void nya_input_action_name_set(NYA_InputAction action, NYA_ConstCString name) {
    nya_assert(action != NYA_INPUT_ACTION_NONE, "NYA_INPUT_ACTION_NONE is the unbound action and cannot carry a name.");
    nya_assert(action < NYA_INPUT_ACTION_MAX, "Action %d is past NYA_INPUT_ACTION_MAX.", (int)action);
    nya_assert(name != nullptr && name[0] != '\0', "An action name must be a non-empty string.");

    NYA_InputSystem* system = &nya_app_get()->input_system;

    NYA_InputAction existing = nya_input_action_from_name(name);
    if (existing != NYA_INPUT_ACTION_NONE && existing != action) {
        // refused: the reverse lookup can only answer one, and the other would never load from settings.
        nya_log_error("Action %d cannot be called '%s': action %d already is.", (int)action, name, (int)existing);
        return;
    }

    // copied: the literal may live in a library a hot reload unmaps. see NYA_InputSystem.action_names.
    system->action_names[action] = nya_string_to_cstring(system->allocator, nya_string_from(system->allocator, name));
}

NYA_ConstCString nya_input_action_name(NYA_InputAction action) {
    if (action == NYA_INPUT_ACTION_NONE || action >= NYA_INPUT_ACTION_MAX) return nullptr;

    return nya_app_get()->input_system.action_names[action];
}

NYA_InputAction nya_input_action_from_name(NYA_ConstCString name) {
    if (name == nullptr) return NYA_INPUT_ACTION_NONE;

    NYA_InputSystem* system = &nya_app_get()->input_system;

    // a linear scan, once per action per settings load; a map would cost an allocation.
    for (u32 action = 1; action < NYA_INPUT_ACTION_MAX; action++) {
        if (system->action_names[action] == nullptr) continue;
        if (nya_string_equals(system->action_names[action], name)) return (NYA_InputAction)action;
    }

    return NYA_INPUT_ACTION_NONE;
}

b8 nya_input_action_just_pressed(NYA_InputAction action) {
    return nya_input_action_just_pressed_by(NYA_INPUT_PLAYER_ANY, action);
}

b8 nya_input_action_pressed(NYA_InputAction action) {
    return nya_input_action_pressed_by(NYA_INPUT_PLAYER_ANY, action);
}

b8 nya_input_action_just_pressed_by(u32 player, NYA_InputAction action) {
    NYA_InputBinding* bindings = _nya_input_bindings_for(action);

    for (u32 i = 0; i < NYA_INPUT_BINDINGS_PER_ACTION; i++) {
        if (bindings[i].kind == NYA_INPUT_BINDING_NONE) continue;

        // gamepad bindings are not routed per player yet; every connected pad counts.
        if (bindings[i].kind != NYA_INPUT_BINDING_KEY) {
            if (_nya_input_binding_gamepad_edge(bindings[i], _NYA_INPUT_EDGE_JUST_PRESSED)) return true;
            continue;
        }

        if (!nya_input_key_just_pressed_by(player, bindings[i].key)) continue;

        // this player's own modifiers, so player 2's shift cannot complete player 1's chord.
        if (!_nya_input_modifiers_match_against(bindings[i].modifiers, nya_input_modifiers_by(player))) continue;

        return true;
    }

    return false;
}

b8 nya_input_action_pressed_by(u32 player, NYA_InputAction action) {
    NYA_InputBinding* bindings = _nya_input_bindings_for(action);

    for (u32 i = 0; i < NYA_INPUT_BINDINGS_PER_ACTION; i++) {
        if (bindings[i].kind == NYA_INPUT_BINDING_NONE) continue;

        if (bindings[i].kind != NYA_INPUT_BINDING_KEY) {
            if (_nya_input_binding_gamepad_edge(bindings[i], _NYA_INPUT_EDGE_PRESSED)) return true;
            continue;
        }

        if (!nya_input_key_pressed_by(player, bindings[i].key)) continue;
        if (!_nya_input_modifiers_match_against(bindings[i].modifiers, nya_input_modifiers_by(player))) continue;

        return true;
    }

    return false;
}

b8 nya_input_action_matches(NYA_InputAction action, NYA_Keycode key, NYA_KeyModFlag modifiers) {
    if (key == NYA_KEY_UNKNOWN) return false;

    NYA_InputBinding* bindings = _nya_input_bindings_for(action);

    for (u32 i = 0; i < NYA_INPUT_BINDINGS_PER_ACTION; i++) {
        if (bindings[i].key == NYA_KEY_UNKNOWN) continue;
        if (bindings[i].kind != NYA_INPUT_BINDING_KEY || bindings[i].key != key) continue;
        if (!_nya_input_modifiers_match_against(bindings[i].modifiers, modifiers)) continue;

        return true;
    }

    return false;
}

b8 nya_input_action_just_released(NYA_InputAction action) {
    return nya_input_action_just_released_by(NYA_INPUT_PLAYER_ANY, action);
}

b8 nya_input_action_just_released_by(u32 player, NYA_InputAction action) {
    NYA_InputBinding* bindings = _nya_input_bindings_for(action);

    for (u32 i = 0; i < NYA_INPUT_BINDINGS_PER_ACTION; i++) {
        if (bindings[i].key == NYA_KEY_UNKNOWN) continue;

        // no modifier check (see core_input.h): releasing Ctrl before the key is the normal end of a chord.
        if (nya_input_key_just_released_by(player, bindings[i].key)) return true;
    }

    return false;
}

f32x2 nya_input_mouse_position(void) {
    return nya_input_mouse_position_by(NYA_INPUT_PLAYER_ANY);
}

f32x2 nya_input_mouse_position_delta(void) {
    return nya_input_mouse_position_delta_by(NYA_INPUT_PLAYER_ANY);
}

f32x2 nya_input_mouse_wheel_scroll(void) {
    return nya_input_mouse_wheel_scroll_by(NYA_INPUT_PLAYER_ANY);
}

b8 nya_input_mouse_button_just_pressed(NYA_MouseButton button) {
    return nya_input_mouse_button_just_pressed_by(NYA_INPUT_PLAYER_ANY, button);
}

b8 nya_input_mouse_button_pressed(NYA_MouseButton button) {
    return nya_input_mouse_button_pressed_by(NYA_INPUT_PLAYER_ANY, button);
}

b8 nya_input_mouse_button_just_released(NYA_MouseButton button) {
    return nya_input_mouse_button_just_released_by(NYA_INPUT_PLAYER_ANY, button);
}

f32x2 nya_input_mouse_position_by(u32 player) {
    NYA_InputState* state = _nya_input_state_for(player);
    if (state == nullptr) return f32x2_zero;

    return state->mouse_position;
}

f32x2 nya_input_mouse_position_delta_by(u32 player) {
    NYA_InputState* state = _nya_input_state_for(player);
    if (state == nullptr) return f32x2_zero;

    return state->mouse_position_delta;
}

f32x2 nya_input_mouse_wheel_scroll_by(u32 player) {
    NYA_InputState* state = _nya_input_state_for(player);
    if (state == nullptr) return f32x2_zero;

    return state->mouse_wheel_delta;
}

b8 nya_input_mouse_button_just_pressed_by(u32 player, NYA_MouseButton button) {
    // the caller's index, checked here as well.
    if (button >= NYA_MOUSE_BUTTON_COUNT) return false;

    NYA_InputState* state = _nya_input_state_for(player);
    if (state == nullptr) return false;

    return state->mouse_buttons_just_pressed[button];
}

b8 nya_input_mouse_button_pressed_by(u32 player, NYA_MouseButton button) {
    // the caller's index, checked here as well.
    if (button >= NYA_MOUSE_BUTTON_COUNT) return false;

    NYA_InputState* state = _nya_input_state_for(player);
    if (state == nullptr) return false;

    return state->mouse_buttons_pressed[button];
}

b8 nya_input_mouse_button_just_released_by(u32 player, NYA_MouseButton button) {
    // the caller's index, checked here as well.
    if (button >= NYA_MOUSE_BUTTON_COUNT) return false;

    NYA_InputState* state = _nya_input_state_for(player);
    if (state == nullptr) return false;

    return state->mouse_buttons_just_released[button];
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

b8 _nya_input_modifiers_match_against(NYA_KeyModFlag required, NYA_KeyModFlag current) {
    /*
     * Compared per modifier group, so a binding can ask for either Ctrl or the left one only. A group not asked for
     * must not be held (a bare W must not fire during Ctrl+W); a requested group must be held on a requested side.
     * Lock keys are ignored.
     */
    const NYA_KeyModFlag groups[] = { NYA_KEYMOD_CTRL, NYA_KEYMOD_SHIFT, NYA_KEYMOD_ALT, NYA_KEYMOD_GUI };

    for (u32 i = 0; i < sizeof(groups) / sizeof(groups[0]); i++) {
        NYA_KeyModFlag wanted = required & groups[i];
        NYA_KeyModFlag held   = current & groups[i];

        if (wanted == 0) {
            if (held != 0) return false; // a modifier is down that this binding does not want
        } else if ((held & wanted) == 0) {
            return false; // the requested side is not down
        }
    }

    return true;
}

void _nya_system_event_on_update_ended_hook(NYA_Event* event) {
    nya_assert(event != nullptr);
    nya_assert(event->type == NYA_EVENT_UPDATING_ENDED);

    NYA_InputSystem* system = &nya_app_get()->input_system;

    _nya_input_state_end_frame(&system->merged);
    nya_system_gamepad_tick_end();

    for (u32 player = 0; player < NYA_INPUT_MAX_PLAYERS; player++) {
        if (system->players[player].keys_pressed == nullptr) continue;

        _nya_input_state_end_frame(&system->players[player]);
    }

    /* Typed text is cleared every frame; the composition persists until the IME commits or cancels. */
    system->text[0]   = '\0';
    system->text_length = 0;
}

/*
 * ─────────────────────────────────────────────────────────
 * STATE, SOURCES AND PLAYERS
 * ─────────────────────────────────────────────────────────
 */

NYA_InputState* _nya_input_state_for(u32 player) {
    NYA_InputSystem* system = &nya_app_get()->input_system;

    if (player == NYA_INPUT_PLAYER_ANY) return &system->merged;

    // also covers NYA_INPUT_PLAYER_NONE, (u32)-2: the player nobody is holds nothing.
    if (player >= NYA_INPUT_MAX_PLAYERS) return nullptr;

    NYA_InputState* state = &system->players[player];

    // no key tables means unclaimed; the tables are the claim flag.
    if (state->keys_pressed == nullptr) return nullptr;

    return state;
}

NYA_InputSourceBinding* _nya_input_source_find(NYA_InputSource source) {
    NYA_InputSystem* system = &nya_app_get()->input_system;

    for (u32 i = 0; i < system->source_count; i++) {
        // kind and id: ids are only unique within a kind.
        if (system->sources[i].source.kind != source.kind) continue;
        if (system->sources[i].source.id != source.id) continue;

        return &system->sources[i];
    }

    return nullptr;
}

NYA_InputSourceBinding* _nya_input_source_intern(NYA_InputSource source) {
    NYA_InputSourceBinding* existing = _nya_input_source_find(source);
    if (existing != nullptr) return existing;

    NYA_InputSystem* system = &nya_app_get()->input_system;

    /* Full is not an error, and nothing is evicted. */
    if (system->source_count >= NYA_INPUT_MAX_SOURCES) return nullptr;

    NYA_InputSourceBinding* binding = &system->sources[system->source_count++];

    *binding = (NYA_InputSourceBinding){ .source = source, .player = NYA_INPUT_PLAYER_NONE };

    return binding;
}

NYA_InputState* _nya_input_player_claim(u32 player) {
    nya_assert(player < NYA_INPUT_MAX_PLAYERS, "Player %u is past NYA_INPUT_MAX_PLAYERS.", player);

    NYA_InputSystem* system = &nya_app_get()->input_system;

    NYA_InputState* state = &system->players[player];

    // already claimed: idempotent, and a second device joins the existing state.
    if (state->keys_pressed != nullptr) return state;

    *state = (NYA_InputState){ 0 };
    _nya_input_state_init(state, system->allocator);

    return state;
}

void _nya_input_state_init(NYA_InputState* state, NYA_Arena* allocator) {
    nya_assert(state != nullptr);
    nya_assert(allocator != nullptr);

    // 300, about one slot per keycode, so the tables never grow during play.
    const u32 capacity = 300;

    state->keys_just_pressed  = nya_hmap_create_with_capacity(allocator, NYA_Keycode, b8, capacity);
    state->keys_pressed       = nya_hmap_create_with_capacity(allocator, NYA_Keycode, b8, capacity);
    state->keys_just_released = nya_hmap_create_with_capacity(allocator, NYA_Keycode, b8, capacity);
}

void _nya_input_state_deinit(NYA_InputState* state) {
    nya_assert(state != nullptr);

    nya_hmap_destroy(state->keys_just_pressed);
    nya_hmap_destroy(state->keys_pressed);
    nya_hmap_destroy(state->keys_just_released);

    /* Zeroed, which marks the slot unclaimed. */
    *state = (NYA_InputState){ 0 };
}

NYA_InputSource _nya_input_event_source(const NYA_Event* event) {
    nya_assert(event != nullptr);

    switch (event->type) {
        case NYA_EVENT_KEY_DOWN:
        case NYA_EVENT_KEY_UP:            return event->as_key_event.source;
        case NYA_EVENT_MOUSE_BUTTON_DOWN:
        case NYA_EVENT_MOUSE_BUTTON_UP:   return event->as_mouse_button_event.source;
        case NYA_EVENT_MOUSE_MOVED:       return event->as_mouse_moved_event.source;
        case NYA_EVENT_MOUSE_WHEEL_MOVED: return event->as_mouse_wheel_event.source;

        // window events, drops and frame hooks have no device.
        default: return NYA_INPUT_SOURCE_NONE;
    }
}

void _nya_input_state_handle_event(NYA_InputState* state, const NYA_Event* event) {
    nya_assert(state != nullptr);
    nya_assert(event != nullptr);

    if (event->type == NYA_EVENT_KEY_DOWN || event->type == NYA_EVENT_KEY_UP) {
        // from the event: the platform tracks lock states and AltGr, which key presses cannot reconstruct.
        state->modifier_flags = event->as_key_event.modifier_flags;

        NYA_Keycode keycode    = event->as_key_event.key;
        b8          is_down    = event->as_key_event.is_down;
        b8*         is_pressed = nya_hmap_get(state->keys_pressed, keycode);

        if (is_down) {
            if (is_pressed == nullptr || !(*is_pressed)) nya_hmap_add(state->keys_just_pressed, keycode, true);
            nya_hmap_add(state->keys_pressed, keycode, true);
        } else {
            nya_hmap_add(state->keys_pressed, keycode, false);
            nya_hmap_add(state->keys_just_released, keycode, true);
        }
    }

    if (event->type == NYA_EVENT_MOUSE_BUTTON_DOWN || event->type == NYA_EVENT_MOUSE_BUTTON_UP) {
        NYA_MouseButton button  = event->as_mouse_button_event.button;
        b8              is_down = event->as_mouse_button_event.is_down;

        /* Bounded, since the index comes from the device. */
        if (button >= NYA_MOUSE_BUTTON_COUNT) return;

        b8* is_pressed = &state->mouse_buttons_pressed[button];

        if (is_down) {
            if (!(*is_pressed)) state->mouse_buttons_just_pressed[button] = true;
            *is_pressed = true;
        } else {
            *is_pressed                                 = false;
            state->mouse_buttons_just_released[button] = true;
        }
    }

    if (event->type == NYA_EVENT_MOUSE_MOVED) {
        state->mouse_position = (f32x2){
            event->as_mouse_moved_event.x,
            event->as_mouse_moved_event.y,
        };

        state->mouse_position_delta += (f32x2){
            event->as_mouse_moved_event.delta_x,
            event->as_mouse_moved_event.delta_y,
        };
    }

    if (event->type == NYA_EVENT_MOUSE_WHEEL_MOVED) {
        state->mouse_wheel_delta += (f32x2){
            event->as_mouse_wheel_event.amount_x,
            event->as_mouse_wheel_event.amount_y,
        };
    }
}

void _nya_input_state_end_frame(NYA_InputState* state) {
    nya_assert(state != nullptr);

    nya_hmap_clear(state->keys_just_pressed);
    nya_hmap_clear(state->keys_just_released);

    state->mouse_position_delta = f32x2_zero;
    state->mouse_wheel_delta    = f32x2_zero;

    nya_memset(state->mouse_buttons_just_pressed, 0, sizeof(b8) * NYA_MOUSE_BUTTON_COUNT);
    nya_memset(state->mouse_buttons_just_released, 0, sizeof(b8) * NYA_MOUSE_BUTTON_COUNT);
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * TEXT INPUT
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/** Accumulates committed text and tracks the IME composition. */
void _nya_input_text_handle_event(NYA_InputSystem* system, const NYA_Event* event) {
    if (event->type == NYA_EVENT_TEXT_INPUT) {
        NYA_ConstCString text = event->as_text_input_event.text;

        if (text == nullptr) return;

        /* Appended, not replaced. */
        u64 length = strlen(text);

        if (system->text_length + length >= sizeof(system->text)) {
            length = sizeof(system->text) - system->text_length - 1;

            // truncated on a codepoint boundary, so the result is valid UTF-8. see nya_net_chat_sanitize.
            while (length > 0 && ((u8)text[length] & 0xC0) == 0x80) length--;
        }

        if (length == 0) return;

        nya_memcpy(system->text + system->text_length, text, length);

        system->text_length              += (u32)length;
        system->text[system->text_length]  = '\0';

        return;
    }

    if (event->type == NYA_EVENT_TEXT_EDITING) {
        NYA_ConstCString text = event->as_text_editing_event.text;

        system->composition_start  = event->as_text_editing_event.start;
        system->composition_length = event->as_text_editing_event.length;

        if (text == nullptr) {
            system->composition[0] = '\0';
            return;
        }

        (void)snprintf(system->composition, sizeof(system->composition), "%s", text);
    }
}

void nya_input_text_begin(NYA_WindowHandle window) {
    NYA_Window* target = nya_window_get(window);
    if (target == nullptr) return;

    NYA_InputSystem* system = &nya_app_get()->input_system;

#if OS_WASM
    // A wasm build has no SDL and no OS text-input service: the browser owns the IME and the DOM's own
    // <input> gathers text natively. So this only records which window is taking text; there is no SDL
    // session to open. See the header's note on what a browser adds back.
    nya_unused(target);
#else
    if (!SDL_StartTextInput(target->sdl_window)) {
        nya_log_warn("Could not start text input: %s", SDL_GetError());
        return;
    }
#endif

    system->text_window = window;
}

void nya_input_text_end(void) {
    NYA_InputSystem* system = &nya_app_get()->input_system;

    NYA_Window* target = nya_window_get(system->text_window);

#if OS_WASM
    // No SDL text-input session was opened (see nya_input_text_begin); nothing to close.
    nya_unused(target);
#else
    if (target != nullptr) (void)SDL_StopTextInput(target->sdl_window);
#endif

    system->text_window = NYA_WINDOW_HANDLE_NONE;

    // the composition goes too, or a cancelled IME leaves its candidate on screen.
    system->composition[0]     = '\0';
    system->composition_start  = 0;
    system->composition_length = 0;
}

b8 nya_input_text_active(void) {
    return nya_window_get(nya_app_get()->input_system.text_window) != nullptr;
}

NYA_ConstCString nya_input_text(void) {
    return nya_app_get()->input_system.text;
}

NYA_ConstCString nya_input_text_composition(void) {
    return nya_app_get()->input_system.composition;
}

void nya_input_text_composition_range(OUT s32* out_start, OUT s32* out_length) {
    const NYA_InputSystem* system = &nya_app_get()->input_system;

    if (out_start != nullptr) *out_start = system->composition_start;
    if (out_length != nullptr) *out_length = system->composition_length;
}

void nya_input_text_area_set(NYA_WindowHandle window, f32 x, f32 y, f32 width, f32 height) {
    NYA_Window* target = nya_window_get(window);
    if (target == nullptr) return;

#if OS_WASM
    // No SDL, so no candidate-window rectangle to place; the browser positions its own IME. A no-op.
    nya_unused(target), nya_unused(x), nya_unused(y), nya_unused(width), nya_unused(height);
#else
    SDL_Rect area = { .x = (s32)x, .y = (s32)y, .w = (s32)width, .h = (s32)height };

    // cursor offset zero puts the candidate window at the start, right for a single-line field.
    (void)SDL_SetTextInputArea(target->sdl_window, &area, 0);
#endif
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * CLIPBOARD
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

NYA_ConstCString nya_clipboard_text(NYA_Arena* arena) {
    nya_assert(arena != nullptr);

#if OS_WASM
    // Reading the clipboard in a browser is an async, permission-gated API a synchronous C call cannot
    // reach; a paste arrives as a DOM event instead. So there is nothing to hand back here.
    nya_unused(arena);
    return "";
#else
    char* owned = SDL_GetClipboardText();

    // SDL returns an allocation even when the clipboard is empty, and the caller frees it either way.
    if (owned == nullptr) return "";

    u64 length = strlen(owned);

    char* copy = nya_arena_alloc(arena, length + 1);

    nya_memcpy(copy, owned, length);
    copy[length] = '\0';

    SDL_free(owned);

    return copy;
#endif
}

NYA_Error nya_clipboard_text_set(NYA_ConstCString text) {
    if (text == nullptr) return nya_error(NYA_ERROR_INVALID_ARGUMENT, "no text to copy");

#if OS_WASM
    // Writing the clipboard is likewise an async browser API out of a synchronous call's reach. Reported
    // as OK rather than an error so a copy shortcut is a silent no-op, not a failure a caller must handle.
    return NYA_OK;
#else
    if (!SDL_SetClipboardText(text)) return nya_error(NYA_ERROR_NOT_OK, "could not set the clipboard: %s", SDL_GetError());

    return NYA_OK;
#endif
}

b8 nya_clipboard_has_text(void) {
#if OS_WASM
    return false;
#else
    return SDL_HasClipboardText();
#endif
}
