#pragma once

#include "nyangine/base/base.h"
#include "nyangine/base/base_hmap.h"
#include "nyangine/core/core_event.h"
// For NYA_GamepadButton and NYA_GamepadAxis: a binding can name either.
#include "nyangine/core/core_gamepad.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * TYPES
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

typedef struct NYA_InputSystem        NYA_InputSystem;
typedef struct NYA_InputState         NYA_InputState;
typedef struct NYA_InputSourceBinding NYA_InputSourceBinding;
nya_derive_hmap(NYA_Keycode, b8);

/*
 * ─────────────────────────────────────────────────────────
 * PLAYERS AND SOURCES
 * ─────────────────────────────────────────────────────────
 */

/**
 * How many local players' input the engine tracks apart from each other.
 * */
#define NYA_INPUT_MAX_PLAYERS 8

/** How many distinct devices the roster remembers. Past this, further devices route to the merged view only. */
#define NYA_INPUT_MAX_SOURCES 16

/**
 * Bytes of committed text and of IME composition kept per frame, including the terminator.
 * */
#define NYA_INPUT_TEXT_MAX 256

/**
 * Every device at once, which is what the plain nya_input_* queries read.
 * */
#define NYA_INPUT_PLAYER_ANY ((u32)-1)

/** A device nobody has assigned to a slot yet. What nya_input_source_player answers for a stranger. */
#define NYA_INPUT_PLAYER_NONE ((u32)-2)

/** One device, and the player slot it feeds. See NYA_InputSystem.sources. */
struct NYA_InputSourceBinding {
    NYA_InputSource source;

    /** NYA_INPUT_PLAYER_NONE until something assigns it. */
    u32 player;
};

/**
 * One view of the input devices: what is held, where the pointer is, what was pressed this frame.
 * */
struct NYA_InputState {
    NYA_HMapᐸNYA_Keycodeˏb8ᐳ* keys_just_pressed;
    NYA_HMapᐸNYA_Keycodeˏb8ᐳ* keys_pressed;
    NYA_HMapᐸNYA_Keycodeˏb8ᐳ* keys_just_released;

    /**
     * Modifiers held right now.
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
 * ─────────────────────────────────────────────────────────
 * ACTIONS
 * ─────────────────────────────────────────────────────────
 */

/**
 * A named thing the player can do, which a key is bound to rather than hardcoded against.
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

/** How many alternative bindings an action can carry: two keys, a gamepad button and a stick direction. */
#define NYA_INPUT_BINDINGS_PER_ACTION 4

/**
 * What kind of physical input a binding names.
 * */
typedef enum NYA_InputBindingKind {
    /** A zeroed binding. The slot is unbound. */
    NYA_INPUT_BINDING_NONE = 0,

    NYA_INPUT_BINDING_KEY,
    NYA_INPUT_BINDING_GAMEPAD_BUTTON,

    /**
     * An axis past a threshold, treated as a button.
     * */
    NYA_INPUT_BINDING_GAMEPAD_AXIS,

    NYA_INPUT_BINDING_KIND_COUNT,
} NYA_InputBindingKind;

/**
 * One way to trigger an action. A zeroed binding is unbound.
 * */
typedef struct {
    NYA_InputBindingKind kind;

    /** NYA_INPUT_BINDING_KEY. */
    NYA_Keycode    key;
    NYA_KeyModFlag modifiers;

    /** NYA_INPUT_BINDING_GAMEPAD_BUTTON. */
    NYA_GamepadButton button;

    /** NYA_INPUT_BINDING_GAMEPAD_AXIS. */
    NYA_GamepadAxis axis;

    /**
     * How far the axis must travel, and in which direction, for the binding to read as pressed.
     * */
    f32 axis_threshold;
} NYA_InputBinding;

struct NYA_InputSystem {
    NYA_Arena* allocator;

    /**
     * What each action is called, for a settings file and for a rebinding screen.
     * */
    NYA_CString action_names[NYA_INPUT_ACTION_MAX];

    /**
     * Every device folded together. What the whole single-player API reads.
     * */
    NYA_InputState merged;

    /**
     * One view per player slot, claimed or not.
     * */
    NYA_InputState players[NYA_INPUT_MAX_PLAYERS];

    /**
     * Which player each device feeds, and the roster of devices seen at all.
     * */
    NYA_InputSourceBinding sources[NYA_INPUT_MAX_SOURCES];
    u32                    source_count;

    /** The device that produced the most recent key or mouse event. What a "press any button to join" screen reads. */
    NYA_InputSource last_source;

    /* Text input, on the system rather than per player: an IME composes for the one focused field. */

    /** UTF-8 committed this frame, accumulated across however many events delivered it. */
    char text[NYA_INPUT_TEXT_MAX];
    u32  text_length;

    /** The IME's in-progress composition, which is not yet text and must be drawn differently. */
    char composition[NYA_INPUT_TEXT_MAX];
    s32  composition_start;
    s32  composition_length;

    /** Which window text input was started for, or NYA_WINDOW_HANDLE_NONE when it is off. */
    NYA_WindowHandle text_window;
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
 * SOURCES AND PLAYERS
 * ─────────────────────────────────────────────────────────
 */

/*
 * Every key and mouse event carries its device (NYA_InputSource), and the input system can keep a separate view
 * per player. Assign devices to slots, and the per-player queries answer for that slot alone:
 *
 * ```c
 * // A join screen: whoever presses a key claims the next slot.
 * if (nya_input_key_just_pressed(NYA_KEY_RETURN)) {
 *     NYA_InputSource joiner = nya_input_source_last();
 *     if (nya_input_source_player(joiner) == NYA_INPUT_PLAYER_NONE) nya_input_source_assign(joiner, next_slot++);
 * }
 *
 * // In the game, per player.
 * for (u32 player = 0; player < player_count; player++) {
 *     if (nya_input_action_pressed_by(player, ACTION_LEFT)) move(player, -1.0F);
 * }
 * ```
 */

/**
 * The device that produced the most recent key or mouse event.
 * */
NYA_API NYA_InputSource nya_input_source_last(void) __attr_no_discard;

/** How many distinct devices have produced an event so far, capped at NYA_INPUT_MAX_SOURCES. */
NYA_API u32 nya_input_source_count(void) __attr_no_discard;

/** The device at `index` in the roster, in the order they were first seen. NYA_INPUT_SOURCE_NONE past the end. */
NYA_API NYA_InputSource nya_input_source_at(u32 index) __attr_no_discard;

/**
 * Which player slot `source` feeds, or NYA_INPUT_PLAYER_NONE for a device nobody has claimed.
 * */
NYA_API u32 nya_input_source_player(NYA_InputSource source) __attr_no_discard;

/**
 * Routes `source` to `player`, allocating that slot's state on first use.
 * */
NYA_API void nya_input_source_assign(NYA_InputSource source, u32 player);

/** Unclaims a device. It keeps feeding the merged view, like any device nobody has assigned. */
NYA_API void nya_input_source_release(NYA_InputSource source);

/**
 * Unclaims every device and forgets every player's state, without touching bindings or the merged view.
 * */
NYA_API void nya_input_players_reset(void);

/*
 * Per-player queries: the plain query with a slot in front. NYA_INPUT_PLAYER_ANY reads the merged view, and an
 * unassigned slot reads as nothing held, so a loop over NYA_INPUT_MAX_PLAYERS needs no guard.
 */

NYA_API b8 nya_input_key_just_pressed_by(u32 player, NYA_Keycode key) __attr_no_discard;
NYA_API b8 nya_input_key_pressed_by(u32 player, NYA_Keycode key) __attr_no_discard;
NYA_API b8 nya_input_key_just_released_by(u32 player, NYA_Keycode key) __attr_no_discard;
NYA_API NYA_KeyModFlag nya_input_modifiers_by(u32 player) __attr_no_discard;

NYA_API f32x2 nya_input_mouse_position_by(u32 player) __attr_no_discard;
NYA_API f32x2 nya_input_mouse_position_delta_by(u32 player) __attr_no_discard;
NYA_API f32x2 nya_input_mouse_wheel_scroll_by(u32 player) __attr_no_discard;
NYA_API b8    nya_input_mouse_button_just_pressed_by(u32 player, NYA_MouseButton button) __attr_no_discard;
NYA_API b8    nya_input_mouse_button_pressed_by(u32 player, NYA_MouseButton button) __attr_no_discard;
NYA_API b8    nya_input_mouse_button_just_released_by(u32 player, NYA_MouseButton button) __attr_no_discard;

/*
 * ─────────────────────────────────────────────────────────
 * ACTION FUNCTIONS
 * ─────────────────────────────────────────────────────────
 */

/**
 * Adds `key` to `action` as an alternative, with no modifiers or with the ones given.
 * */
NYA_API void nya_input_action_bind(NYA_InputAction action, NYA_Keycode key) __attr_overloaded;
NYA_API void nya_input_action_bind(NYA_InputAction action, NYA_Keycode key, NYA_KeyModFlag modifiers) __attr_overloaded;

/**
 * Replaces every binding on `action` with this one.
 * */
NYA_API void nya_input_action_rebind(NYA_InputAction action, NYA_Keycode key) __attr_overloaded;
NYA_API void nya_input_action_rebind(NYA_InputAction action, NYA_Keycode key, NYA_KeyModFlag modifiers) __attr_overloaded;

/**
 * Writes one slot directly, leaving the others alone.
 * */
NYA_API void nya_input_action_set(NYA_InputAction action, u32 slot, NYA_Keycode key, NYA_KeyModFlag modifiers);

/** The binding in `slot`, for drawing it in a menu. Its key is NYA_KEY_UNKNOWN when unbound. */
NYA_API NYA_InputBinding nya_input_action_get(NYA_InputAction action, u32 slot) __attr_no_discard;

/** Drops every binding for `action`. */
/**
 * Binds a gamepad button, or an axis past a threshold, into the next free slot.
 * */
NYA_API void nya_input_action_bind_button(NYA_InputAction action, NYA_GamepadButton button);
NYA_API void nya_input_action_bind_axis(NYA_InputAction action, NYA_GamepadAxis axis, f32 threshold);

/** Whether `binding` is currently satisfied by any connected pad, ignoring the keyboard half. */
NYA_API b8 nya_input_binding_gamepad_pressed(NYA_InputBinding binding) __attr_no_discard;

/**
 * Drops every binding `action` has.
 *
 * A plugin needs the keybinding permission for this and not merely the input one: taking a key away
 * from the game or from another plugin is a different thing from reading whether it is down.
 *
 * @lua(KEYBINDING)
 * */
NYA_API void nya_input_action_unbind(NYA_InputAction action);

/**
 * Whether `action` has any key bound to it at all.
 *
 * @lua(INPUT)
 * */
NYA_API b8 nya_input_action_bound(NYA_InputAction action) __attr_no_discard;

/*
 * Naming actions. An integer in a settings file is unreadable and breaks when the enum changes.
 *
 * ```c
 * nya_input_action_name_set(ACTION_JUMP, "jump");
 * nya_input_action_bind(ACTION_JUMP, NYA_KEY_SPACE);
 * ```
 *
 * Engine actions name themselves. An unnamed game action is not persisted (see nya_settings_to_object).
 */

/**
 * Gives `action` a name. Copied, so a string literal from a hot reloaded library is safe.
 * */
NYA_API void nya_input_action_name_set(NYA_InputAction action, NYA_ConstCString name);

/**
 * What `action` is called, or null when nothing has named it.
 *
 * @lua(INPUT)
 * */
NYA_API NYA_ConstCString nya_input_action_name(NYA_InputAction action) __attr_no_discard;

/**
 * The action called `name`, or NYA_INPUT_ACTION_NONE. What a settings file's keys resolve through.
 *
 * The call a plugin wants first: an action number is the game's, and a plugin that hard codes one
 * breaks when the game adds an action in the middle.
 *
 * @lua(INPUT)
 * */
NYA_API NYA_InputAction nya_input_action_from_name(NYA_ConstCString name) __attr_no_discard;

/**
 * True on the frame any of the action's bindings became satisfied.
 *
 * @lua(INPUT)
 * */
NYA_API b8 nya_input_action_just_pressed(NYA_InputAction action) __attr_no_discard;

/**
 * True while any of the action's bindings is satisfied.
 *
 * @lua(INPUT)
 * */
NYA_API b8 nya_input_action_pressed(NYA_InputAction action) __attr_no_discard;

/**
 * True on the frame the last of the action's bindings stopped being satisfied.
 *
 * @lua(INPUT)
 * */
NYA_API b8 nya_input_action_just_released(NYA_InputAction action) __attr_no_discard;

/**
 * The same three, restricted to one player's devices. See the note on sources and players above.
 * */
NYA_API b8 nya_input_action_just_pressed_by(u32 player, NYA_InputAction action) __attr_no_discard;
NYA_API b8 nya_input_action_pressed_by(u32 player, NYA_InputAction action) __attr_no_discard;
NYA_API b8 nya_input_action_just_released_by(u32 player, NYA_InputAction action) __attr_no_discard;

/**
 * Whether a key and modifier combination satisfies any of `action`'s bindings.
 *
 * ```c
 * case NYA_EVENT_KEY_DOWN: {
 *     const NYA_KeyEvent* key = &event->as_key_event;
 *     if (key->is_repeat) break;
 *
 *     if (nya_input_action_matches(ACTION_FIRE, key->key, key->modifier_flags)) { ... }
 * } break;
 * ```
 * */
NYA_API b8 nya_input_action_matches(NYA_InputAction action, NYA_Keycode key, NYA_KeyModFlag modifiers) __attr_no_discard;

NYA_API f32x2 nya_input_mouse_position(void);
NYA_API f32x2 nya_input_mouse_position_delta(void);
NYA_API f32x2 nya_input_mouse_wheel_scroll(void);
NYA_API b8    nya_input_mouse_button_just_pressed(NYA_MouseButton button);
NYA_API b8    nya_input_mouse_button_pressed(NYA_MouseButton button);
NYA_API b8    nya_input_mouse_button_just_released(NYA_MouseButton button);

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * TEXT INPUT
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/**
 * Starts delivering typed text, and shows the on-screen keyboard where there is one.
 * */
NYA_API void nya_input_text_begin(NYA_WindowHandle window);

/** Stops delivering typed text. Safe when it was never started. */
NYA_API void nya_input_text_end(void);

/** Whether text is being delivered. */
NYA_API b8 nya_input_text_active(void) __attr_no_discard;

/**
 * The UTF-8 committed this frame, or an empty string.
 * */
NYA_API NYA_ConstCString nya_input_text(void) __attr_no_discard;

/**
 * The IME's in-progress composition, or an empty string.
 * */
NYA_API NYA_ConstCString nya_input_text_composition(void) __attr_no_discard;

/** The selected range within the composition, in bytes. Both are zero when there is none. */
NYA_API void nya_input_text_composition_range(OUT s32* out_start, OUT s32* out_length);

/**
 * Where the caret is, so the IME candidate window can appear beside it.
 * */
NYA_API void nya_input_text_area_set(NYA_WindowHandle window, f32 x, f32 y, f32 width, f32 height);

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * CLIPBOARD
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/**
 * The system clipboard's text, copied into `arena`, or an empty string.
 * */
NYA_API NYA_ConstCString nya_clipboard_text(NYA_Arena* arena) __attr_no_discard;

/** Puts `text` on the system clipboard. */
NYA_API NYA_Error nya_clipboard_text_set(NYA_ConstCString text) __attr_no_discard;

/** Whether the clipboard holds any text. Cheaper than fetching it to find out. */
NYA_API b8 nya_clipboard_has_text(void) __attr_no_discard;
