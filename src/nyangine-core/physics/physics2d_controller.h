/**
 * @file physics2d_controller.h
 *
 * ```c
 * static NYA_CharacterController2D player = { 0 };
 *
 * // Once per fixed tick, after physics has stepped.
 * nya_character2d_update(&player, entity, (NYA_CharacterInput2D){
 *     .move = axis, .jump_held = held, .jump_pressed = pressed,
 * }, delta_time_s);
 * ```
 * */
#pragma once

#include "nyangine-std/base/base_handle.h"
#include "nyangine-std/base/base_types.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * TYPES
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

typedef struct NYA_CharacterController2D NYA_CharacterController2D;
typedef struct NYA_CharacterTuning2D     NYA_CharacterTuning2D;
typedef struct NYA_CharacterInput2D      NYA_CharacterInput2D;

/** What the game says the player is asking for this tick. */
struct NYA_CharacterInput2D {
    /** Horizontal intent in [-1, 1]. An analog stick passes its axis straight through. */
    f32 move;

    /** Whether jump went down *this tick*. Buffered, so it need not coincide with being grounded. */
    b8 jump_pressed;

    /** Whether jump is still held. What variable jump height reads. */
    b8 jump_held;

    /** Whether the player is asking to drop through a one-way platform. */
    b8 drop_through;
};

/** Every field's zero is a usable default, so `(NYA_CharacterTuning2D){ 0 }` is a working character. */
struct NYA_CharacterTuning2D {
    /** Top horizontal speed, in world units per second. Default 220. */
    f32 max_speed;

    /** How fast top speed is reached and lost, in units per second squared. Defaults 2400 and 2600. */
    f32 acceleration;
    f32 deceleration;

    /** Air control as a fraction of ground control. Default 0.65. */
    f32 air_control;

    /** Upward speed a jump starts at. Default 520. */
    f32 jump_speed;

    /**
     * How long after leaving the ground a jump still works, in seconds. Default 0.1.
     * */
    f32 coyote_time_s;

    /** How long before landing a jump press is remembered, in seconds. Default 0.12. */
    f32 jump_buffer_s;

    /** Downward acceleration while rising, and the multiplier applied while falling. Defaults 1400, 1.9. */
    f32 gravity;
    f32 fall_gravity_multiplier;

    /** What a rising velocity is cut to when jump is released early, as a fraction. Default 0.4. */
    f32 jump_cut_multiplier;

    /** Terminal downward speed, so a long fall does not tunnel. Default 1200. */
    f32 max_fall_speed;
};

struct NYA_CharacterController2D {
    NYA_CharacterTuning2D tuning;

    /** Whether the character was on the ground at the end of the last update. */
    b8 grounded;

    /** Whether it became grounded this tick, for a landing sound or a puff of dust. */
    b8 landed;

    /** Whether a jump started this tick. */
    b8 jumped;

    /** Whether it is rising under a jump, as opposed to merely moving upward. */
    b8 jumping;

    /** Which way it last faced. Kept through a stop, so a standing character does not snap to the right. */
    f32 facing;

    /*
     * The two forgiveness timers, counting down.
     */
    f32 coyote_left_s;
    f32 buffer_left_s;
};

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * FUNCTIONS AND MACROS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/** Fills in every unset tuning field. Called by nya_character2d_update, so it is rarely needed directly. */
NYA_API NYA_CharacterTuning2D nya_character2d_tuning_defaults(NYA_CharacterTuning2D tuning) __attr_no_discard;

/**
 * Advances the controller one fixed tick and writes the body's velocity.
 * */
NYA_API void nya_character2d_update(NYA_CharacterController2D* controller, NYA_EntityHandle entity, NYA_CharacterInput2D input,
                                    f32 delta_time_s);

/** Cancels an in-progress jump and clears both forgiveness timers. What a death or a cutscene wants. */
NYA_API void nya_character2d_reset(NYA_CharacterController2D* controller);
