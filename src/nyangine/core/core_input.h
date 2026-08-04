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

/*
 * ─────────────────────────────────────────────────────────
 * ACTIONS
 * ─────────────────────────────────────────────────────────
 */

/**
 * A named thing the player can do, which a key is bound to rather than hardcoded against.
 *
 * The engine names only the handful every game and every menu ends up needing. Everything specific
 * belongs to the game, which continues the numbering from NYA_INPUT_ACTION_USER:
 *
 * ```c
 * enum {
 *     ACTION_JUMP = NYA_INPUT_ACTION_USER,
 *     ACTION_FIRE,
 *     ACTION_CROUCH,
 * };
 *
 * nya_input_action_bind(ACTION_JUMP, NYA_KEY_SPACE);
 * nya_input_action_bind(ACTION_FIRE, NYA_KEY_S, NYA_KEYMOD_CTRL);
 *
 * if (nya_input_action_just_pressed(ACTION_JUMP)) { ... }
 * ```
 *
 * The gap between the engine's actions and NYA_INPUT_ACTION_USER is deliberate: adding an engine
 * action later must not renumber a game's, because bindings get written to disk.
 * */
typedef enum {
    /** The unbound action. Binding or querying it is a mistake, and asserts. */
    NYA_INPUT_ACTION_NONE = 0,

    NYA_INPUT_ACTION_CONFIRM,
    NYA_INPUT_ACTION_CANCEL,
    NYA_INPUT_ACTION_PAUSE,
    NYA_INPUT_ACTION_UP,
    NYA_INPUT_ACTION_DOWN,
    NYA_INPUT_ACTION_LEFT,
    NYA_INPUT_ACTION_RIGHT,

    /** One past the last engine action. Not a valid action. */
    NYA_INPUT_ACTION_ENGINE_COUNT,

    /** Where a game's own actions start. */
    NYA_INPUT_ACTION_USER = 64,

    /** One past the highest action there is room for. Sizes the binding table. */
    NYA_INPUT_ACTION_MAX = 256,
} NYA_InputAction;

/** How many alternative bindings an action can carry: the usual primary and secondary. */
#define NYA_INPUT_BINDINGS_PER_ACTION 2

/** A key plus the modifiers that must be held with it. A zero key means the slot is unbound. */
typedef struct {
    NYA_Keycode    key;
    NYA_KeyModFlag modifiers;
} NYA_InputBinding;

struct NYA_InputSystem {
    NYA_Arena* allocator;

    NYA_HMapᐸNYA_Keycodeˏb8ᐳ* keys_just_pressed;
    NYA_HMapᐸNYA_Keycodeˏb8ᐳ* keys_pressed;
    NYA_HMapᐸNYA_Keycodeˏb8ᐳ* keys_just_released;

    /**
     * Modifiers held right now.
     *
     * Tracked continuously rather than read off the triggering key event, because an action query
     * asks whether a chord is held *at this moment* and the last key event may be several frames
     * old — or may be the release of the very modifier being asked about.
     * */
    NYA_KeyModFlag modifier_flags;

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

/** Modifiers held right now, as a combination of NYA_KEYMOD_ flags. */
NYA_API NYA_KeyModFlag nya_input_modifiers(void) __attr_no_discard;

/*
 * ─────────────────────────────────────────────────────────
 * ACTION FUNCTIONS
 * ─────────────────────────────────────────────────────────
 */

/**
 * Adds `key` to `action` as an alternative, with no modifiers or with the ones given.
 *
 * Takes the first free slot, so binding twice gives an action a primary and a secondary key.
 * Binding a third time replaces the last rather than silently doing nothing, and binding a key the
 * action already has just updates its modifiers.
 *
 * This *adds*. To replace what is there — which is what a rebinding menu does — use
 * nya_input_action_rebind, or nya_input_action_set to write one slot.
 * */
NYA_API void nya_input_action_bind(NYA_InputAction action, NYA_Keycode key) __attr_overloaded;
NYA_API void nya_input_action_bind(NYA_InputAction action, NYA_Keycode key, NYA_KeyModFlag modifiers) __attr_overloaded;

/**
 * Replaces every binding on `action` with this one.
 *
 * What a rebinding screen wants: "press a key for Jump" should leave the action bound to that key
 * and nothing else. nya_input_action_bind *adds* an alternative, so calling it repeatedly from a
 * settings menu accumulates rather than overwrites.
 * */
NYA_API void nya_input_action_rebind(NYA_InputAction action, NYA_Keycode key) __attr_overloaded;
NYA_API void nya_input_action_rebind(NYA_InputAction action, NYA_Keycode key, NYA_KeyModFlag modifiers) __attr_overloaded;

/**
 * Writes one slot directly, leaving the others alone.
 *
 * For a settings screen that shows an action's primary and secondary side by side and lets either
 * cell be rebound on its own. `slot` is below NYA_INPUT_BINDINGS_PER_ACTION; NYA_KEY_UNKNOWN clears
 * just that slot.
 * */
NYA_API void nya_input_action_set(NYA_InputAction action, u32 slot, NYA_Keycode key, NYA_KeyModFlag modifiers);

/** The binding in `slot`, for drawing it in a menu. Its key is NYA_KEY_UNKNOWN when unbound. */
NYA_API NYA_InputBinding nya_input_action_get(NYA_InputAction action, u32 slot) __attr_no_discard;

/** Drops every binding for `action`. */
NYA_API void nya_input_action_unbind(NYA_InputAction action);

/** Whether `action` has any key bound to it at all. */
NYA_API b8 nya_input_action_bound(NYA_InputAction action) __attr_no_discard;

/**
 * True when any of the action's bindings is satisfied.
 *
 * A binding is satisfied when its key is in the requested state and the modifiers match: every
 * modifier the binding asks for is held, and no *other* modifier is. That second half is what keeps
 * a plain W from firing while Ctrl+W is being typed. Lock keys — caps, num, scroll — are state
 * rather than chording and never count either way.
 *
 * The release query deliberately does not check modifiers: letting go of Ctrl before the key is the
 * normal way to end a chord, and requiring both to still be held would mean the release never fires.
 * */
NYA_API b8 nya_input_action_just_pressed(NYA_InputAction action) __attr_no_discard;
NYA_API b8 nya_input_action_pressed(NYA_InputAction action) __attr_no_discard;
NYA_API b8 nya_input_action_just_released(NYA_InputAction action) __attr_no_discard;

NYA_API f32x2 nya_input_mouse_position(void);
NYA_API f32x2 nya_input_mouse_position_delta(void);
NYA_API f32x2 nya_input_mouse_wheel_scroll(void);
NYA_API b8    nya_input_mouse_button_just_pressed(NYA_MouseButton button);
NYA_API b8    nya_input_mouse_button_pressed(NYA_MouseButton button);
NYA_API b8    nya_input_mouse_button_just_released(NYA_MouseButton button);
