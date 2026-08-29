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
 *
 * Eight matches a couch and what Steam Remote Play Together actually reaches; no shared-screen game
 * wants more. Each claimed slot costs a NYA_InputState, allocated only once a device is assigned.
 * */
#define NYA_INPUT_MAX_PLAYERS 8

/** How many distinct devices the roster remembers. Past this, further devices route to the merged view only. */
#define NYA_INPUT_MAX_SOURCES 16

/**
 * Bytes of committed text and of IME composition kept per frame, including the terminator.
 *
 * A frame's worth, not a field's worth — a text field accumulates into its own storage. Bounded
 * rather than grown because it is filled from events a remote nobody controls.
 * */
#define NYA_INPUT_TEXT_MAX 256

/**
 * Every device at once, which is what the plain nya_input_* queries read.
 *
 * Outside the valid player range on purpose, so passing it to a per-player query behaves like the
 * single-player one — makes "does anyone want to pause" expressible without iterating slots.
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
 *
 * One for the merged view and one per claimed player slot, sharing this struct so a per-player query
 * and the merged one cannot drift apart.
 * */
struct NYA_InputState {
    NYA_HMapᐸNYA_Keycodeˏb8ᐳ* keys_just_pressed;
    NYA_HMapᐸNYA_Keycodeˏb8ᐳ* keys_pressed;
    NYA_HMapᐸNYA_Keycodeˏb8ᐳ* keys_just_released;

    /**
     * Modifiers held right now.
     *
     * Tracked continuously rather than read off the triggering key event: an action query asks
     * whether a chord is held *at this moment*, and the last key event may be frames old — or the
     * release of the very modifier in question.
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
 * The engine names only the handful every game and menu needs; a game's own actions continue the
 * numbering from NYA_INPUT_ACTION_USER:
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
 * The gap is deliberate: adding an engine action later must not renumber a game's, since bindings
 * are written to disk.
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

/**
 * What kind of physical input a binding names.
 *
 * A tagged union rather than a parallel gamepad table, so there is one binding list, one query path and
 * one thing for a settings file and a rebinding screen to understand. A second table would have to be
 * kept in step with this one forever, and every `nya_input_action_pressed` would have to consult both.
 * */
typedef enum NYA_InputBindingKind {
    /** A zeroed binding. The slot is unbound. */
    NYA_INPUT_BINDING_NONE = 0,

    NYA_INPUT_BINDING_KEY,
    NYA_INPUT_BINDING_GAMEPAD_BUTTON,

    /**
     * An axis past a threshold, treated as a button.
     *
     * What lets a trigger or a stick direction bind to an ordinary action: "right stick left" is an
     * axis and a sign, and without this every such binding is a special case in game code.
     * */
    NYA_INPUT_BINDING_GAMEPAD_AXIS,

    NYA_INPUT_BINDING_KIND_COUNT,
} NYA_InputBindingKind;

/**
 * One way to trigger an action. A zeroed binding is unbound.
 *
 * `key` and `modifiers` stay at the top and keep their names, so the keyboard half of this reads
 * exactly as it did before gamepads existed and a `{ .key = ... }` initialiser still compiles.
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
     *
     * Signed, because the sign *is* the direction: -0.5 is "left stick pushed left past halfway". Zero
     * is treated as NYA_GAMEPAD_TRIGGER_THRESHOLD in the positive direction.
     * */
    f32 axis_threshold;
} NYA_InputBinding;

struct NYA_InputSystem {
    NYA_Arena* allocator;

    /**
     * What each action is called, for a settings file and for a rebinding screen.
     *
     * Copied into `allocator` rather than pointed at the caller's literal: a game's action names are
     * string literals in the game's shared library, and a hot reload unmaps that library, so borrowed
     * pointers would dangle and settings could be written from freed memory.
     *
     * Null until something registers a name. See nya_input_action_name.
     * */
    NYA_CString action_names[NYA_INPUT_ACTION_MAX];

    /**
     * Every device folded together. What the whole single-player API reads.
     *
     * The state the engine has always kept: one keyboard's key tables and one mouse's position and
     * buttons, fed by every device at once. A game that never assigns a source to a player sees
     * exactly what it saw before per-player input existed.
     * */
    NYA_InputState merged;

    /**
     * One view per player slot, claimed or not.
     *
     * Stored inline rather than as pointers into the arena: the expensive part of a state is its
     * three key tables, created only when a slot is claimed and destroyed when released — what's
     * here is the eighty-odd bytes of struct around them, cheaper than the pointer chase and
     * unleakable.
     *
     * It was an array of arena-allocated pointers, and that leaked: nya_input_players_reset frees a
     * slot's tables and drops it, but an arena has no per-allocation free, so the struct stayed
     * behind and the next claim allocated another. A game returning to its lobby repeatedly grew the
     * input arena forever.
     *
     * A slot is claimed exactly when its key tables exist — see _nya_input_state_for — so there is no
     * separate flag to fall out of step with them.
     * */
    NYA_InputState players[NYA_INPUT_MAX_PLAYERS];

    /**
     * Which player each device feeds, and the roster of devices seen at all.
     *
     * A device appears here the first time it produces an event, with `player` set to
     * NYA_INPUT_PLAYER_NONE — what makes a join screen possible: nya_input_source_last names the
     * device that just pressed something, and the game assigns it a slot.
     *
     * A flat array with a linear scan: bounded at NYA_INPUT_MAX_SOURCES and walked once per input
     * event. A hash map would cost a hash per keystroke to search sixteen entries.
     * */
    NYA_InputSourceBinding sources[NYA_INPUT_MAX_SOURCES];
    u32                    source_count;

    /** The device that produced the most recent key or mouse event. What a "press any button to join" screen reads. */
    NYA_InputSource last_source;

    /*
     * ── text input ──
     *
     * On the system rather than on a NYA_InputState: text has no per-player meaning, since an IME
     * composes for the one field that has focus. Splitting it per device would ask which keyboard a
     * candidate window belongs to, which is not a question.
     */

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
 * Every key and mouse event carries the device it came from — see NYA_InputSource — and the input
 * system can keep a separate view of the world per player. Assign devices to slots and the per-player
 * queries answer for that slot alone:
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
 *
 * What this does and does not promise: it promises that when the platform distinguishes two devices,
 * the engine does too, all the way from the SDL event to the query. It does not promise that the
 * platform can. SDL fills its `which` field in only where the backend supports it — a mouse id is
 * meaningful in relative mode and zero otherwise, and a keyboard id is "0 if unknown or virtual" — so
 * on a good many desktops every device reports source zero and every player is the same player.
 *
 * That is the honest state of keyboard-and-mouse multiplayer generally, and it is why Steam's own
 * supported path for Remote Play Together is **virtual controllers**, one per remote player, rather
 * than multiplexed keyboards. This machinery is what a gamepad source plugs into when the engine
 * grows one: the routing, the slots and the queries are all device-kind agnostic, so adding
 * NYA_INPUT_DEVICE_KIND_GAMEPAD is a new event source rather than a new input system.
 *
 * Bindings stay global on purpose: two players sharing a screen play with the same control scheme on
 * their own devices, and per-player bindings would mean a settings file and rebinding screen per player.
 */

/**
 * The device that produced the most recent key or mouse event.
 *
 * NYA_INPUT_SOURCE_NONE before anything has happened. Kept across frames rather than cleared, so a
 * screen can poll it instead of hooking every event.
 * */
NYA_API NYA_InputSource nya_input_source_last(void) __attr_no_discard;

/** How many distinct devices have produced an event so far, capped at NYA_INPUT_MAX_SOURCES. */
NYA_API u32 nya_input_source_count(void) __attr_no_discard;

/** The device at `index` in the roster, in the order they were first seen. NYA_INPUT_SOURCE_NONE past the end. */
NYA_API NYA_InputSource nya_input_source_at(u32 index) __attr_no_discard;

/**
 * Which player slot `source` feeds, or NYA_INPUT_PLAYER_NONE for a device nobody has claimed.
 *
 * A device that has never produced an event is also unclaimed — the one call a join screen needs to
 * tell "already playing" from "just walked up".
 * */
NYA_API u32 nya_input_source_player(NYA_InputSource source) __attr_no_discard;

/**
 * Routes `source` to `player`, allocating that slot's state on first use.
 *
 * Reassigning a device moves it; the state it fed before is not rewritten, so a key held across the
 * change stays held for the old slot until released. Assigning a second device to the same player is
 * fine — how a keyboard and a mouse become one player.
 *
 * `player` must be below NYA_INPUT_MAX_PLAYERS. The merged view is fed regardless and cannot be
 * assigned to, so NYA_INPUT_PLAYER_ANY is not a legal argument here.
 * */
NYA_API void nya_input_source_assign(NYA_InputSource source, u32 player);

/** Unclaims a device. It keeps feeding the merged view, like any device nobody has assigned. */
NYA_API void nya_input_source_release(NYA_InputSource source);

/**
 * Unclaims every device and forgets every player's state, without touching bindings or the merged view.
 *
 * What returning to a lobby wants: the next session assigns devices from scratch rather than
 * inheriting whoever happened to be player 3 last time.
 * */
NYA_API void nya_input_players_reset(void);

/*
 * ── The per player queries ──
 *
 * Each is the plain query with a slot in front. NYA_INPUT_PLAYER_ANY reads the merged view, so
 * `nya_input_key_pressed(k)` and `nya_input_key_pressed_by(NYA_INPUT_PLAYER_ANY, k)` are the same
 * call. An unassigned slot reads as nothing held, keeping a loop over NYA_INPUT_MAX_PLAYERS guard-free.
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
 *
 * Takes the first free slot, so binding twice gives an action a primary and a secondary key. Binding
 * a third time replaces the last, and binding a key the action already has just updates its modifiers.
 *
 * This *adds*. To replace what is there — what a rebinding menu wants — use nya_input_action_rebind,
 * or nya_input_action_set to write one slot.
 * */
NYA_API void nya_input_action_bind(NYA_InputAction action, NYA_Keycode key) __attr_overloaded;
NYA_API void nya_input_action_bind(NYA_InputAction action, NYA_Keycode key, NYA_KeyModFlag modifiers) __attr_overloaded;

/**
 * Replaces every binding on `action` with this one.
 *
 * What a rebinding screen wants: "press a key for Jump" should leave the action bound to just that
 * key. nya_input_action_bind *adds* an alternative instead, so calling it repeatedly accumulates.
 * */
NYA_API void nya_input_action_rebind(NYA_InputAction action, NYA_Keycode key) __attr_overloaded;
NYA_API void nya_input_action_rebind(NYA_InputAction action, NYA_Keycode key, NYA_KeyModFlag modifiers) __attr_overloaded;

/**
 * Writes one slot directly, leaving the others alone.
 *
 * For a settings screen showing an action's primary and secondary side by side, each rebindable on
 * its own. `slot` is below NYA_INPUT_BINDINGS_PER_ACTION; NYA_KEY_UNKNOWN clears just that slot.
 * */
NYA_API void nya_input_action_set(NYA_InputAction action, u32 slot, NYA_Keycode key, NYA_KeyModFlag modifiers);

/** The binding in `slot`, for drawing it in a menu. Its key is NYA_KEY_UNKNOWN when unbound. */
NYA_API NYA_InputBinding nya_input_action_get(NYA_InputAction action, u32 slot) __attr_no_discard;

/** Drops every binding for `action`. */
/**
 * Binds a gamepad button, or an axis past a threshold, into the next free slot.
 *
 * The pad is not named: a binding says *what* triggers the action, and *who* is asking is the player
 * routing in nya_input_source_assign. Binding to one specific controller would make a rebinding screen
 * wrong the moment a player swapped pads.
 * */
NYA_API void nya_input_action_bind_button(NYA_InputAction action, NYA_GamepadButton button);
NYA_API void nya_input_action_bind_axis(NYA_InputAction action, NYA_GamepadAxis axis, f32 threshold);

/** Whether `binding` is currently satisfied by any connected pad, ignoring the keyboard half. */
NYA_API b8 nya_input_binding_gamepad_pressed(NYA_InputBinding binding) __attr_no_discard;

NYA_API void nya_input_action_unbind(NYA_InputAction action);

/** Whether `action` has any key bound to it at all. */
NYA_API b8 nya_input_action_bound(NYA_InputAction action) __attr_no_discard;

/*
 * ── Naming actions ──
 *
 * An action is an integer, and an integer is a bad thing to write into a settings file: unreadable to
 * a player, and silently wrong the first time a game inserts an action in the middle of its enum.
 *
 * ```c
 * nya_input_action_name_set(ACTION_JUMP, "jump");
 * nya_input_action_bind(ACTION_JUMP, NYA_KEY_SPACE);
 * ```
 *
 * Engine actions name themselves at startup. A game's actions do not, and an unnamed action is not
 * persisted at all — see nya_settings_to_object, which skips them rather than inventing a key that
 * would stop matching the day the enum changes.
 */

/**
 * Gives `action` a name. Copied, so a string literal from a hot reloaded library is safe.
 *
 * Names are how bindings survive a settings file: nya_settings_to_object writes them and
 * nya_settings_from_object looks them back up. Register them before loading settings, or the file's
 * bindings have nothing to attach to.
 *
 * Naming two actions the same thing is refused with a log — the lookup back from a name could then
 * only answer one of them, and the other would silently never load.
 * */
NYA_API void nya_input_action_name_set(NYA_InputAction action, NYA_ConstCString name);

/** What `action` is called, or null when nothing has named it. */
NYA_API NYA_ConstCString nya_input_action_name(NYA_InputAction action) __attr_no_discard;

/** The action called `name`, or NYA_INPUT_ACTION_NONE. What a settings file's keys resolve through. */
NYA_API NYA_InputAction nya_input_action_from_name(NYA_ConstCString name) __attr_no_discard;

/**
 * True when any of the action's bindings is satisfied.
 *
 * Satisfied when the key is in the requested state and the modifiers match: every modifier the
 * binding asks for is held, and no *other* modifier is — what keeps a plain W from firing while
 * Ctrl+W is typed. Lock keys (caps, num, scroll) are state, not chording, and never count either way.
 *
 * The release query does not check modifiers: letting go of Ctrl before the key is the normal way to
 * end a chord, and requiring both still held would mean the release never fires.
 * */
NYA_API b8 nya_input_action_just_pressed(NYA_InputAction action) __attr_no_discard;
NYA_API b8 nya_input_action_pressed(NYA_InputAction action) __attr_no_discard;
NYA_API b8 nya_input_action_just_released(NYA_InputAction action) __attr_no_discard;

/**
 * The same three, restricted to one player's devices. See the note on sources and players above.
 *
 * Bindings are global, so this asks whether *this player* is holding a key that `action` is bound to.
 * NYA_INPUT_PLAYER_ANY reads the merged view and is identical to the three above.
 * */
NYA_API b8 nya_input_action_just_pressed_by(u32 player, NYA_InputAction action) __attr_no_discard;
NYA_API b8 nya_input_action_pressed_by(u32 player, NYA_InputAction action) __attr_no_discard;
NYA_API b8 nya_input_action_just_released_by(u32 player, NYA_InputAction action) __attr_no_discard;

/**
 * Whether a key and modifier combination satisfies any of `action`'s bindings.
 *
 * The event-driven counterpart to the three queries above, and the one a discrete press wants:
 *
 * ```c
 * case NYA_EVENT_KEY_DOWN: {
 *     const NYA_KeyEvent* key = &event->as_key_event;
 *     if (key->is_repeat) break;
 *
 *     if (nya_input_action_matches(ACTION_FIRE, key->key, key->modifier_flags)) { ... }
 * } break;
 * ```
 *
 * The polls answer "is this held *now*", right for movement and wrong for anything that should
 * happen once per press: on_update runs once per fixed tick, a slow frame runs several, and a poll
 * stays true across all of them — one keypress fires between one and eight times. An event fires
 * exactly once, asked here in terms of actions rather than whichever key happens to be bound today.
 *
 * Same modifier rule as nya_input_action_pressed: no other chording modifier may be held, so a plain
 * W does not fire while Ctrl+W is typed.
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
 *
 * Nothing produces NYA_EVENT_TEXT_INPUT until this is called. That is not an oversight in the
 * platform layer — a key press and a *character* are different things, and which one a keystroke
 * produces depends on the layout, the modifiers and, for CJK, on an input method with its own
 * multi-keystroke state. So the platform only runs that machinery while something is listening.
 *
 * Pair with nya_input_text_end when the field loses focus. Leaving it on costs an IME candidate
 * window that appears over the game while nobody is typing.
 * */
NYA_API void nya_input_text_begin(NYA_WindowHandle window);

/** Stops delivering typed text. Safe when it was never started. */
NYA_API void nya_input_text_end(void);

/** Whether text is being delivered. */
NYA_API b8 nya_input_text_active(void) __attr_no_discard;

/**
 * The UTF-8 committed this frame, or an empty string.
 *
 * Characters rather than keys: one keystroke may produce several bytes, several keystrokes may
 * produce one character, and a paste or an IME commit may produce a whole phrase at once. Read this
 * for content and nya_input_key_just_pressed for control — backspace and the arrows are keys and
 * never appear here.
 *
 * Valid until the end of the frame.
 * */
NYA_API NYA_ConstCString nya_input_text(void) __attr_no_discard;

/**
 * The IME's in-progress composition, or an empty string.
 *
 * What a Japanese or Chinese input method shows while the user is still choosing: it is *not* typed
 * text and must not be inserted into the field, but it does have to be drawn — usually underlined at
 * the caret — or the user cannot see what they are composing. It becomes text, through
 * nya_input_text, only when the method commits it.
 *
 * Ignoring this entirely still gives a working Latin text field. It is what makes the field usable
 * in the languages the atlas already renders; see the note on CJK in render2d.h.
 * */
NYA_API NYA_ConstCString nya_input_text_composition(void) __attr_no_discard;

/** The selected range within the composition, in bytes. Both are zero when there is none. */
NYA_API void nya_input_text_composition_range(OUT s32* out_start, OUT s32* out_length);

/**
 * Where the caret is, so the IME candidate window can appear beside it.
 *
 * In window pixels. Without this the candidate list opens wherever the platform guesses, which is
 * usually a screen corner far from what the user is typing into.
 * */
NYA_API void nya_input_text_area_set(NYA_WindowHandle window, f32 x, f32 y, f32 width, f32 height);

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * CLIPBOARD
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/**
 * The system clipboard's text, copied into `arena`, or an empty string.
 *
 * Copied rather than borrowed because the platform hands back a buffer it expects to be freed, and a
 * caller that had to remember to do that would leak it. Never null, so a paste path has no branch.
 *
 * Here rather than in a module of its own because it exists to serve a text field: the only reason
 * this engine has a clipboard is that Ctrl+V has to do something.
 * */
NYA_API NYA_ConstCString nya_clipboard_text(NYA_Arena* arena) __attr_no_discard;

/** Puts `text` on the system clipboard. */
NYA_API NYA_Error nya_clipboard_text_set(NYA_ConstCString text);

/** Whether the clipboard holds any text. Cheaper than fetching it to find out. */
NYA_API b8 nya_clipboard_has_text(void) __attr_no_discard;
