/**
 * @file core_gamepad.h
 *
 * Gamepads: connection, buttons, axes and rumble.
 *
 * ```c
 * // Hotplug is handled for you; this is just "is anyone holding one".
 * if (nya_gamepad_count() > 0) {
 *     NYA_GamepadId pad = nya_gamepad_at(0);
 *
 *     f32x2 move = { nya_gamepad_axis(pad, NYA_GAMEPAD_AXIS_LEFT_X), nya_gamepad_axis(pad, NYA_GAMEPAD_AXIS_LEFT_Y) };
 *     if (nya_gamepad_button_just_pressed(pad, NYA_GAMEPAD_BUTTON_SOUTH)) jump();
 *     if (hit) nya_gamepad_rumble(pad, 0.4F, 0.8F, 120);
 * }
 * ```
 *
 * **Hotplug is not optional.** `SDL_EVENT_GAMEPAD_ADDED` fires for every already-connected pad during
 * `SDL_Init` as well as for later ones, so opening on that event is the single correct path — there is
 * no separate enumeration pass, and writing one produces double-open bugs. It is also a certification
 * requirement on Steam Deck and Xbox, and on Windows a controller may simply not be present at startup.
 *
 * **Buttons are named by position, not by letter.** `NYA_GAMEPAD_BUTTON_SOUTH` is the bottom face
 * button: A on Xbox, B on Nintendo, Cross on PlayStation. Naming it "A" bakes one vendor's layout into
 * every call site and is how a Switch player ends up being told to press the wrong button. What glyph
 * to *draw* is a separate question — see nya_gamepad_kind.
 *
 * **Axes are already deadzoned and normalised** to [-1, 1], with the triggers in [0, 1]. A raw stick
 * never reads exactly zero at rest, so an undeadzoned axis makes a character drift; doing it here means
 * every caller gets it right rather than each one inventing a threshold.
 *
 * Rumble and the other optional features vary by controller *and* by OS, so they are probed at runtime
 * — `nya_gamepad_has_rumble` before assuming, and a rumble on a pad without it is a silent no-op rather
 * than an error.
 * */
#pragma once

#include "nyangine/base/base_types.h"
#include "nyangine/core/core_types.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * CONSTANTS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/**
 * How many gamepads may be connected at once.
 *
 * Eight, which is more than any couch holds and is what the routing in core_input.h can address as
 * separate players anyway.
 * */
#ifndef NYA_GAMEPAD_MAX
#define NYA_GAMEPAD_MAX 8
#endif

/**
 * How far a stick must leave centre before it counts, as a fraction of full deflection.
 *
 * A worn stick rests noticeably off centre, and without this a character drifts on an untouched pad —
 * the single most reported controller bug there is. Applied radially to a stick pair rather than per
 * axis, so the dead zone is a circle and a diagonal is not easier to register than a cardinal.
 * */
#ifndef NYA_GAMEPAD_STICK_DEADZONE
#define NYA_GAMEPAD_STICK_DEADZONE 0.18F
#endif

/** How far a trigger must be pulled before it counts. Lower than a stick's: a trigger rests at rest. */
#ifndef NYA_GAMEPAD_TRIGGER_DEADZONE
#define NYA_GAMEPAD_TRIGGER_DEADZONE 0.08F
#endif

/** What a trigger must reach to read as a button press, for a binding that treats it as one. */
#ifndef NYA_GAMEPAD_TRIGGER_THRESHOLD
#define NYA_GAMEPAD_TRIGGER_THRESHOLD 0.5F
#endif

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * TYPES
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/** A connected gamepad, as the platform's instance id. Stable while the pad stays plugged in. */
typedef u32 NYA_GamepadId;

/** No gamepad. What every query falls back to and what an unconnected slot reports. */
#define NYA_GAMEPAD_NONE ((NYA_GamepadId)0)

/**
 * Face buttons by position rather than by letter, so a call site is not written in one vendor's
 * alphabet. SOUTH is the bottom face button: A on Xbox, B on Nintendo, Cross on PlayStation.
 * */
typedef enum NYA_GamepadButton {
    NYA_GAMEPAD_BUTTON_SOUTH = 0,
    NYA_GAMEPAD_BUTTON_EAST,
    NYA_GAMEPAD_BUTTON_WEST,
    NYA_GAMEPAD_BUTTON_NORTH,

    NYA_GAMEPAD_BUTTON_BACK,
    NYA_GAMEPAD_BUTTON_GUIDE,
    NYA_GAMEPAD_BUTTON_START,

    NYA_GAMEPAD_BUTTON_LEFT_STICK,
    NYA_GAMEPAD_BUTTON_RIGHT_STICK,
    NYA_GAMEPAD_BUTTON_LEFT_SHOULDER,
    NYA_GAMEPAD_BUTTON_RIGHT_SHOULDER,

    NYA_GAMEPAD_BUTTON_DPAD_UP,
    NYA_GAMEPAD_BUTTON_DPAD_DOWN,
    NYA_GAMEPAD_BUTTON_DPAD_LEFT,
    NYA_GAMEPAD_BUTTON_DPAD_RIGHT,

    NYA_GAMEPAD_BUTTON_COUNT,
} NYA_GamepadButton;

/** Sticks in [-1, 1] with y positive downward, matching the screen; triggers in [0, 1]. */
typedef enum NYA_GamepadAxis {
    NYA_GAMEPAD_AXIS_LEFT_X = 0,
    NYA_GAMEPAD_AXIS_LEFT_Y,
    NYA_GAMEPAD_AXIS_RIGHT_X,
    NYA_GAMEPAD_AXIS_RIGHT_Y,
    NYA_GAMEPAD_AXIS_LEFT_TRIGGER,
    NYA_GAMEPAD_AXIS_RIGHT_TRIGGER,

    NYA_GAMEPAD_AXIS_COUNT,
} NYA_GamepadAxis;

/**
 * What the pad looks like, which is what decides the glyphs to draw.
 *
 * Steam Deck compatibility requires on-screen glyphs to match the device in use, so this is not
 * cosmetic — it is a submission requirement.
 * */
typedef enum NYA_GamepadKind {
    NYA_GAMEPAD_KIND_UNKNOWN = 0,
    NYA_GAMEPAD_KIND_XBOX,
    NYA_GAMEPAD_KIND_PLAYSTATION,
    NYA_GAMEPAD_KIND_NINTENDO,
    NYA_GAMEPAD_KIND_STEAM_DECK,

    NYA_GAMEPAD_KIND_COUNT,
} NYA_GamepadKind;

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * FUNCTIONS AND MACROS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

NYA_API void nya_system_gamepad_init(void);
NYA_API void nya_system_gamepad_deinit(void);

/** Rolls the just-pressed and just-released edges. Called once per frame, before events are handled. */
NYA_API void nya_system_gamepad_frame_begin(void);

/** Consumes an SDL gamepad event. Returns whether it was one. */
NYA_API b8 nya_system_gamepad_handle_sdl_event(const void* sdl_event);

/** How many pads are connected. */
NYA_API u32 nya_gamepad_count(void) __attr_no_discard;

/** The id of the `index`th connected pad, in connection order, or NYA_GAMEPAD_NONE. */
NYA_API NYA_GamepadId nya_gamepad_at(u32 index) __attr_no_discard;

NYA_API b8              nya_gamepad_connected(NYA_GamepadId pad) __attr_no_discard;
NYA_API NYA_ConstCString nya_gamepad_name(NYA_GamepadId pad) __attr_no_discard;
NYA_API NYA_GamepadKind nya_gamepad_kind(NYA_GamepadId pad) __attr_no_discard;

NYA_API b8 nya_gamepad_button_pressed(NYA_GamepadId pad, NYA_GamepadButton button) __attr_no_discard;
NYA_API b8 nya_gamepad_button_just_pressed(NYA_GamepadId pad, NYA_GamepadButton button) __attr_no_discard;
NYA_API b8 nya_gamepad_button_just_released(NYA_GamepadId pad, NYA_GamepadButton button) __attr_no_discard;

/** The axis, deadzoned and normalised. Zero for an unconnected pad, so a caller need not check first. */
NYA_API f32 nya_gamepad_axis(NYA_GamepadId pad, NYA_GamepadAxis axis) __attr_no_discard;

/**
 * A stick as a pair, with the dead zone applied radially.
 *
 * Radially rather than per axis, so the dead zone is a circle: applying it per axis leaves a
 * cross-shaped hole where a diagonal registers at a smaller deflection than a cardinal does.
 * */
NYA_API f32x2 nya_gamepad_stick(NYA_GamepadId pad, b8 right_stick) __attr_no_discard;

/**
 * An axis treated as a button: past `threshold`, whose *sign is its direction*.
 *
 * -0.5 means "pushed past halfway in the negative direction", not "further than -0.5". Zero threshold
 * is NYA_GAMEPAD_TRIGGER_THRESHOLD, positive. The edges come from a snapshot taken each frame, so an
 * axis binding behaves exactly like a button binding.
 * */
NYA_API b8 nya_gamepad_axis_pressed(NYA_GamepadId pad, NYA_GamepadAxis axis, f32 threshold) __attr_no_discard;
NYA_API b8 nya_gamepad_axis_just_pressed(NYA_GamepadId pad, NYA_GamepadAxis axis, f32 threshold) __attr_no_discard;
NYA_API b8 nya_gamepad_axis_just_released(NYA_GamepadId pad, NYA_GamepadAxis axis, f32 threshold) __attr_no_discard;

/** Whether this pad can rumble at all. Varies by controller and by OS, so probe rather than assume. */
NYA_API b8 nya_gamepad_has_rumble(NYA_GamepadId pad) __attr_no_discard;

/**
 * Rumbles for `duration_ms`. Intensities are [0, 1]; low is the heavy motor, high the light one.
 *
 * A no-op on a pad that cannot, rather than an error: a game should not have to branch on it, and a
 * missing rumble is not a failure worth propagating.
 * */
NYA_API void nya_gamepad_rumble(NYA_GamepadId pad, f32 low_frequency, f32 high_frequency, u32 duration_ms);

/** Stops any rumble immediately. What a pause menu wants. */
NYA_API void nya_gamepad_rumble_stop(NYA_GamepadId pad);
