/**
 * @file physics2d_controller.h
 *
 * A 2D platformer character controller: the game-feel layer over a dynamic body.
 *
 * ```c
 * static NYA_CharacterController2D player = { 0 };
 *
 * // Once per fixed tick, after physics has stepped.
 * nya_character2d_update(&player, entity, (NYA_CharacterInput2D){
 *     .move = axis, .jump_held = held, .jump_pressed = pressed,
 * }, delta_time_s);
 * ```
 *
 * **Everything here is forgiveness, and forgiveness is what platformers are made of.** None of it
 * changes what is possible; all of it changes whether a player believes the game is fair.
 *
 * - **Coyote time**: a jump still works for a moment after walking off a ledge. Players press jump
 *   *as* they leave, not before, and without this every ledge feels like it grabbed them.
 * - **Jump buffering**: a jump pressed just before landing fires on touchdown instead of being
 *   swallowed. Same input, opposite edge of the same problem.
 * - **Variable height**: releasing early cuts the rise, so a tap is a hop and a hold is a leap.
 * - **Asymmetric gravity**: heavier on the way down. Real projectile motion feels floaty, and almost
 *   every platformer worth playing lies about it.
 *
 * **The controller owns velocity, not position.** It writes through `nya_physics2d_velocity_set` and
 * lets the solver own the rest — writing position every tick fights the solver, which is the mistake
 * `system_movement.c` documents.
 *
 * Grounding comes from `nya_physics2d_grounded`, which is contact-normal based and cached per tick, so
 * a slope counts as ground and a wall does not.
 * */
#pragma once

#include "nyangine/base/base_types.h"
#include "nyangine/core/core_types.h"

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

    /** Air control as a fraction of ground control. Default 0.65 — less than ground, more than none. */
    f32 air_control;

    /** Upward speed a jump starts at. Default 520. */
    f32 jump_speed;

    /**
     * How long after leaving the ground a jump still works, in seconds. Default 0.1.
     *
     * Six frames at sixty. Long enough to catch the press that felt on time, short enough that nobody
     * notices it is there — which is the point.
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
     *
     * Timers rather than frame counters so the feel does not change with the tick rate — the whole
     * point is a window measured in how long it *feels*, and sixty milliseconds is sixty milliseconds
     * whether that is three ticks or six.
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
 *
 * The entity must carry a dynamic 2D body. Does nothing if it does not, rather than asserting — a
 * character whose body has not been attached yet is an ordinary startup ordering, not a bug.
 * */
NYA_API void nya_character2d_update(NYA_CharacterController2D* controller, NYA_EntityHandle entity, NYA_CharacterInput2D input,
                                    f32 delta_time_s);

/** Cancels an in-progress jump and clears both forgiveness timers. What a death or a cutscene wants. */
NYA_API void nya_character2d_reset(NYA_CharacterController2D* controller);
