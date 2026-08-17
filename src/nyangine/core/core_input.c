#include "nyangine/nyangine.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API DECLARATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/*
 * Deliberately not NYA_INTERNAL: registered with nya_callback, so update_callback_pointers in main.c
 * re-resolves it by name with dlsym after every hot reload. A hidden or static symbol is not in the
 * dynamic symbol table that -rdynamic populates, so making this internal survives the build and dies
 * on the first reload. See the longer note in core_asset.c.
 */
// NOLINTNEXTLINE(misc-use-internal-linkage)
void _nya_system_event_on_update_ended_hook(NYA_Event* event);

/**
 * Whether the modifiers a binding asks for are exactly the ones in `current`.
 *
 * Takes the held set rather than reading it, for two reasons. nya_input_action_matches is asked about
 * the modifiers an *event* carried, and the live set is not the right answer there: a slow frame can
 * deliver a key event several milliseconds late, by which time the player may have let go of the
 * Ctrl that was part of it. And a per player query has to compare against that player's own
 * modifiers, or player 2 holding shift would satisfy a chord player 1 is halfway through.
 * */
NYA_INTERNAL b8 _nya_input_modifiers_match_against(NYA_KeyModFlag required, NYA_KeyModFlag current) __attr_no_discard;

/** The binding table slot for an action, asserting the action is one that can be bound. */
NYA_INTERNAL NYA_InputBinding* _nya_input_bindings_for(NYA_InputAction action) __attr_no_discard;

/**
 * The state a query should read, or null when the slot is unclaimed.
 *
 * NYA_INPUT_PLAYER_ANY is the merged view. A slot with no device assigned has no state and reads as
 * nothing held, which is what lets a loop over NYA_INPUT_MAX_PLAYERS run without a guard.
 * */
NYA_INTERNAL NYA_InputState* _nya_input_state_for(u32 player) __attr_no_discard;

/** The roster entry for `source`, or null when it has never produced an event. */
NYA_INTERNAL NYA_InputSourceBinding* _nya_input_source_find(NYA_InputSource source) __attr_no_discard;

/**
 * The roster entry for `source`, adding it as unclaimed if this is the first time it is seen.
 *
 * Null once the roster is full, which is not a failure: past NYA_INPUT_MAX_SOURCES a device still
 * feeds the merged view, it just cannot be assigned to a player.
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

/** Drops the per frame edges — just pressed, just released, and the two deltas. */
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

    // Every slot starts unclaimed, which is to say with no key tables. Those are created by
    // nya_input_source_assign, so a single-player game never pays for seven empty copies of them.
    for (u32 player = 0; player < NYA_INPUT_MAX_PLAYERS; player++) app->input_system.players[player] = (NYA_InputState){ 0 };

    // The engine's own actions name themselves, so a settings file can carry a rebound Confirm
    // without the game having to know these exist. A game's actions name themselves, or are not
    // persisted.
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

    nya_info("Input system initialized.");
}

void nya_system_input_deinit(void) {
    NYA_App* app = nya_app_get();

    _nya_input_state_deinit(&app->input_system.merged);

    for (u32 player = 0; player < NYA_INPUT_MAX_PLAYERS; player++) {
        if (app->input_system.players[player].keys_pressed == nullptr) continue;

        _nya_input_state_deinit(&app->input_system.players[player]);
    }

    // The key tables came out of this arena, so whatever the loop above missed goes with it.
    nya_arena_destroy(app->input_system.allocator);

    nya_info("Input system deinitialized.");
}

void nya_system_input_handle_event(NYA_Event* event) {
    nya_assert(event != nullptr);

    NYA_InputSystem* system = &nya_app_get()->input_system;

    /*
     * Every event goes to the merged view, and to the routed player's view as well when there is one.
     *
     * Both rather than either: the merged view is what the whole single-player API and every menu
     * reads, so routing an event *away* from it the moment a game assigned a device to a slot would
     * silently break the pause menu of any game that added a second player.
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
 * The per player forms, which the six above are the NYA_INPUT_PLAYER_ANY case of.
 *
 * Written this way round rather than the other so there is exactly one implementation of each
 * question. The single-player spelling stays because it is what almost every call site wants, and
 * making every one of them pass a slot it does not have would be noise.
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

    // A device nobody has seen is as unclaimed as one nobody has assigned, and a join screen wants
    // the same answer for both.
    if (binding == nullptr) return NYA_INPUT_PLAYER_NONE;

    return binding->player;
}

void nya_input_source_assign(NYA_InputSource source, u32 player) {
    nya_assert(player < NYA_INPUT_MAX_PLAYERS, "Player %u is past NYA_INPUT_MAX_PLAYERS.", player);
    nya_assert(source.kind != NYA_INPUT_DEVICE_KIND_NONE, "NYA_INPUT_SOURCE_NONE is not a device and cannot be assigned.");

    NYA_InputSourceBinding* binding = _nya_input_source_intern(source);

    if (binding == nullptr) {
        // The roster is full. Reported rather than asserted: it is a device count, which is outside
        // the game's control, and the device still feeds the merged view.
        nya_warn("Cannot assign input source (kind %d, id %u) to player %u: already tracking %d sources.", (int)source.kind, source.id, player,
                 NYA_INPUT_MAX_SOURCES);
        return;
    }

    // Before the write, so a slot that turns out to be unclaimable leaves the routing alone rather
    // than pointing at a state that was never allocated.
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
     * The slots are torn down rather than merely unrouted.
     *
     * A player who leaves mid-frame leaves keys held in their state, and a slot reused by somebody
     * else would start with those still down — a lobby that assigns player 2 to a new device would
     * find them already walking left. Destroying the tables is the only way to be sure.
     *
     * The roster of *devices* is kept: they are still plugged in, and forgetting them would break
     * nya_input_source_at for a join screen drawn on the very next frame.
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

void nya_input_action_name_set(NYA_InputAction action, NYA_ConstCString name) {
    nya_assert(action != NYA_INPUT_ACTION_NONE, "NYA_INPUT_ACTION_NONE is the unbound action and cannot carry a name.");
    nya_assert(action < NYA_INPUT_ACTION_MAX, "Action %d is past NYA_INPUT_ACTION_MAX.", (int)action);
    nya_assert(name != nullptr && name[0] != '\0', "An action name must be a non-empty string.");

    NYA_InputSystem* system = &nya_app_get()->input_system;

    NYA_InputAction existing = nya_input_action_from_name(name);
    if (existing != NYA_INPUT_ACTION_NONE && existing != action) {
        // Refused rather than allowed to win, because the reverse lookup can only answer one of them
        // and the loser would silently never load its bindings from a settings file.
        nya_log_error("Action %d cannot be called '%s': action %d already is.", (int)action, name, (int)existing);
        return;
    }

    // Copied, not borrowed. See NYA_InputSystem.action_names — the caller's literal may live in a
    // shared library that a hot reload is about to unmap.
    system->action_names[action] = nya_string_to_cstring(system->allocator, nya_string_from(system->allocator, name));
}

NYA_ConstCString nya_input_action_name(NYA_InputAction action) {
    if (action == NYA_INPUT_ACTION_NONE || action >= NYA_INPUT_ACTION_MAX) return nullptr;

    return nya_app_get()->input_system.action_names[action];
}

NYA_InputAction nya_input_action_from_name(NYA_ConstCString name) {
    if (name == nullptr) return NYA_INPUT_ACTION_NONE;

    NYA_InputSystem* system = &nya_app_get()->input_system;

    // A linear scan over 256 slots, on a path that runs once per action per settings load. A map
    // would cost an allocation and a hash to save a few hundred comparisons at startup.
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
        if (bindings[i].key == NYA_KEY_UNKNOWN) continue;
        if (!nya_input_key_just_pressed_by(player, bindings[i].key)) continue;

        // Against this player's own modifier state, not the merged one: player 2 holding shift must
        // not satisfy a chord that player 1 is halfway through.
        if (!_nya_input_modifiers_match_against(bindings[i].modifiers, nya_input_modifiers_by(player))) continue;

        return true;
    }

    return false;
}

b8 nya_input_action_pressed_by(u32 player, NYA_InputAction action) {
    NYA_InputBinding* bindings = _nya_input_bindings_for(action);

    for (u32 i = 0; i < NYA_INPUT_BINDINGS_PER_ACTION; i++) {
        if (bindings[i].key == NYA_KEY_UNKNOWN) continue;
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
        if (bindings[i].key != key) continue;
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

        // No modifier check here on purpose; see the note in core_input.h. Releasing Ctrl before the
        // key it modified is the normal way to end a chord.
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
    // The caller's index, so it is checked here too rather than only where SDL's is.
    if (button >= NYA_MOUSE_BUTTON_COUNT) return false;

    NYA_InputState* state = _nya_input_state_for(player);
    if (state == nullptr) return false;

    return state->mouse_buttons_just_pressed[button];
}

b8 nya_input_mouse_button_pressed_by(u32 player, NYA_MouseButton button) {
    // The caller's index, so it is checked here too rather than only where SDL's is.
    if (button >= NYA_MOUSE_BUTTON_COUNT) return false;

    NYA_InputState* state = _nya_input_state_for(player);
    if (state == nullptr) return false;

    return state->mouse_buttons_pressed[button];
}

b8 nya_input_mouse_button_just_released_by(u32 player, NYA_MouseButton button) {
    // The caller's index, so it is checked here too rather than only where SDL's is.
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

    NYA_InputSystem* system = &nya_app_get()->input_system;

    _nya_input_state_end_frame(&system->merged);

    for (u32 player = 0; player < NYA_INPUT_MAX_PLAYERS; player++) {
        if (system->players[player].keys_pressed == nullptr) continue;

        _nya_input_state_end_frame(&system->players[player]);
    }

    /*
     * Typed text is cleared here and the composition is not.
     *
     * They have different lifetimes and it matters: text is what arrived *this frame* and a reader
     * that missed it has missed it, while a composition persists across frames until the IME either
     * commits or cancels it. Clearing the composition here would make it flicker for exactly one
     * frame each time it changed.
     */
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

    // Covers NYA_INPUT_PLAYER_NONE too, which is (u32)-2 and therefore also past the end. Reading
    // "the player nobody is" as nothing held is what a caller passing the result of
    // nya_input_source_player straight through deserves, rather than an assertion.
    if (player >= NYA_INPUT_MAX_PLAYERS) return nullptr;

    NYA_InputState* state = &system->players[player];

    // No key tables means nobody has claimed this slot. That is the claim flag, rather than a second
    // bool that could disagree with whether the tables are actually there.
    if (state->keys_pressed == nullptr) return nullptr;

    return state;
}

NYA_InputSourceBinding* _nya_input_source_find(NYA_InputSource source) {
    NYA_InputSystem* system = &nya_app_get()->input_system;

    for (u32 i = 0; i < system->source_count; i++) {
        // Both halves. An id is only unique within a kind, so keyboard 1 and mouse 1 are two devices
        // and matching on the id alone would route one player's mouse into another's keyboard.
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

    /*
     * Full is not an error, and nothing is evicted.
     *
     * Evicting would unassign whichever player happened to be least recently seen — mid-game, on
     * nothing more than someone plugging in a sixteenth device. A device past the cap simply cannot
     * be assigned to a slot; it still feeds the merged view, so it still moves the menu.
     */
    if (system->source_count >= NYA_INPUT_MAX_SOURCES) return nullptr;

    NYA_InputSourceBinding* binding = &system->sources[system->source_count++];

    *binding = (NYA_InputSourceBinding){ .source = source, .player = NYA_INPUT_PLAYER_NONE };

    return binding;
}

NYA_InputState* _nya_input_player_claim(u32 player) {
    nya_assert(player < NYA_INPUT_MAX_PLAYERS, "Player %u is past NYA_INPUT_MAX_PLAYERS.", player);

    NYA_InputSystem* system = &nya_app_get()->input_system;

    NYA_InputState* state = &system->players[player];

    // Already claimed. The tables are the claim, so this is idempotent and a second device assigned
    // to the same player joins the state that is there rather than replacing it.
    if (state->keys_pressed != nullptr) return state;

    *state = (NYA_InputState){ 0 };
    _nya_input_state_init(state, system->allocator);

    return state;
}

void _nya_input_state_init(NYA_InputState* state, NYA_Arena* allocator) {
    nya_assert(state != nullptr);
    nya_assert(allocator != nullptr);

    // 300, which is roughly one slot per keycode a keyboard can produce, so the tables never grow
    // during play. Unchanged from when there was one set of them.
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

    /*
     * Zeroed, which is also what marks the slot unclaimed again.
     *
     * The tables are destroyed and the pointers must not survive them: nya_input_players_reset frees
     * a slot while the arena it came from lives on, so a stale pointer here would outlast what it
     * points at — and _nya_input_state_for reads exactly that pointer to decide whether the slot is
     * claimed.
     */
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

        // Everything else — window events, drops, the frame hooks — has no device behind it.
        default: return NYA_INPUT_SOURCE_NONE;
    }
}

void _nya_input_state_handle_event(NYA_InputState* state, const NYA_Event* event) {
    nya_assert(state != nullptr);
    nya_assert(event != nullptr);

    if (event->type == NYA_EVENT_KEY_DOWN || event->type == NYA_EVENT_KEY_UP) {
        // Taken from the event rather than derived from which modifier keycodes are down: the
        // platform already tracks lock states and AltGr, which no amount of watching key presses
        // reconstructs correctly.
        state->modifier_flags = event->as_key_event.modifier_flags;

        NYA_Keycode keycode    = event->as_key_event.key;
        b8          is_down    = event->as_key_event.is_down;
        b8*         is_pressed = nya_hmap_get(state->keys_pressed, keycode);

        if (is_down) {
            if (is_pressed == nullptr || !(*is_pressed)) nya_hmap_set(state->keys_just_pressed, keycode, true);
            nya_hmap_set(state->keys_pressed, keycode, true);
        } else {
            nya_hmap_set(state->keys_pressed, keycode, false);
            nya_hmap_set(state->keys_just_released, keycode, true);
        }
    }

    if (event->type == NYA_EVENT_MOUSE_BUTTON_DOWN || event->type == NYA_EVENT_MOUSE_BUTTON_UP) {
        NYA_MouseButton button  = event->as_mouse_button_event.button;
        b8              is_down = event->as_mouse_button_event.is_down;

        /*
         * Bounded, because this index comes from the device.
         *
         * SDL reports the platform's button number in a Uint8, and the five named here are only the
         * ones every mouse has — anything with side buttons, a tilt wheel or a thumb cluster reports
         * six and upward. The three tables are NYA_MOUSE_BUTTON_COUNT wide and sit next to each
         * other inside NYA_InputState, so an unbounded write walked straight from one into the next
         * on nothing more exotic than a gaming mouse.
         *
         * Ignored rather than clamped: folding button nine onto button five would report a press the
         * user did not make, which is worse than not seeing it at all.
         */
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

/** Accumulates committed text and tracks the IME composition. See NYA_InputSystem's text members. */
void _nya_input_text_handle_event(NYA_InputSystem* system, const NYA_Event* event) {
    if (event->type == NYA_EVENT_TEXT_INPUT) {
        NYA_ConstCString text = event->as_text_input_event.text;

        if (text == nullptr) return;

        /*
         * Appended, not replaced.
         *
         * More than one of these can arrive between two frames — a paste, a fast typist, a key
         * repeat — and taking only the last would silently drop characters in exactly the case a
         * text field is being stress tested.
         */
        u64 length = strlen(text);

        if (system->text_length + length >= sizeof(system->text)) {
            length = sizeof(system->text) - system->text_length - 1;

            // Truncated on a codepoint boundary rather than mid-sequence, so what reaches the field
            // is always well formed UTF-8. See nya_net_chat_sanitize for the same reasoning.
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

    if (!SDL_StartTextInput(target->sdl_window)) {
        nya_warn("Could not start text input: %s", SDL_GetError());
        return;
    }

    system->text_window = window;
}

void nya_input_text_end(void) {
    NYA_InputSystem* system = &nya_app_get()->input_system;

    NYA_Window* target = nya_window_get(system->text_window);

    if (target != nullptr) (void)SDL_StopTextInput(target->sdl_window);

    system->text_window = NYA_WINDOW_HANDLE_NONE;

    // The composition goes with it. An IME cancelled mid-phrase would otherwise leave its last
    // candidate on screen for as long as nothing else was typed.
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

    SDL_Rect area = { .x = (s32)x, .y = (s32)y, .w = (s32)width, .h = (s32)height };

    // The cursor offset is the third argument: zero puts the candidate window at the start of the
    // area, which is what a single line field wants.
    (void)SDL_SetTextInputArea(target->sdl_window, &area, 0);
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * CLIPBOARD
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

NYA_ConstCString nya_clipboard_text(NYA_Arena* arena) {
    nya_assert(arena != nullptr);

    char* owned = SDL_GetClipboardText();

    // SDL returns an allocation even when there is nothing on the clipboard, and it is the caller's
    // to free either way — which is the whole reason this copies rather than handing it back.
    if (owned == nullptr) return "";

    u64 length = strlen(owned);

    char* copy = nya_arena_alloc(arena, length + 1);

    nya_memcpy(copy, owned, length);
    copy[length] = '\0';

    SDL_free(owned);

    return copy;
}

NYA_Error nya_clipboard_text_set(NYA_ConstCString text) {
    if (text == nullptr) return nya_error(NYA_ERROR_INVALID_ARGUMENT, "no text to copy");

    if (!SDL_SetClipboardText(text)) return nya_error(NYA_ERROR_NOT_OK, "could not set the clipboard: %s", SDL_GetError());

    return NYA_OK;
}

b8 nya_clipboard_has_text(void) {
    return SDL_HasClipboardText();
}
