/**
 * @file math_spring.h
 *
 * Damped springs: the interruptible half of animation, next to the easing curves that are not.
 *
 * ```c
 * static NYA_SpringF32 zoom = { .value = 1.0F, .frequency = 4.0F, .damping = 1.0F };
 *
 * // Every frame. The target may change at any moment; the spring absorbs it.
 * nya_spring_f32(&zoom, target_zoom, delta_time_s);
 * camera.zoom = zoom.value;
 * ```
 *
 * **Why this exists next to nya_ease.** An ease is a function of `t` over a fixed duration: retarget it
 * halfway and the value jumps, because nothing carries the velocity it had. A spring carries velocity,
 * so a new target bends the motion instead of restarting it. Anything a player can interrupt — a camera
 * chasing, a menu item under a moving cursor, a value that tracks input — wants a spring; a scripted
 * beat with a known duration wants an ease.
 *
 * ⚠ `nya_ease_spring` in math_tween.h is *not* this. It is a closed-form damped cosine evaluated from
 * `t` alone, with no state, so it is a shaped curve and cannot survive interruption. This is the
 * integrator.
 *
 * **Integrated implicitly, which is unconditionally stable.** Explicit and semi-implicit Euler both have
 * a step limit near `omega * dt < 2`, and ordinary parameters cross it at sixty hertz — a damping ratio
 * of 3 at 4 Hz, or 20 Hz at any damping, give NaN within a few frames. Solving the step implicitly has
 * no such limit; a huge step lands on the target rather than diverging. The step is still clamped, but
 * for what a stall *means* rather than for stability.
 *
 * `damping` is a ratio: 1 is critically damped and settles as fast as it can without overshoot, below 1
 * overshoots and rings, above 1 crawls in. 1 is almost always what you want; 0.5 is a bouncy UI.
 * */
#pragma once

#include "nyangine/base/base_types.h"
#include "nyangine/math/math_vector.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * CONSTANTS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/**
 * The largest step a spring will integrate at once, in seconds.
 *
 * A frame that took longer than this is a stall, not motion, and integrating it whole throws the spring
 * across the screen. Clamping is the same choice a fixed-timestep loop makes for the same reason.
 * */
#define NYA_SPRING_MAX_STEP 0.1F

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * TYPES
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

typedef struct NYA_SpringF32   NYA_SpringF32;
typedef struct NYA_SpringF32x2 NYA_SpringF32x2;
typedef struct NYA_SpringF32x3 NYA_SpringF32x3;

/**
 * A scalar spring. Zero-initialise it and set `frequency`; the rest have usable defaults.
 *
 * `frequency` is in oscillations per second and is the one number that matters: it is how fast the
 * thing wants to arrive. 1 is a slow drift, 4 is a responsive UI, 20 snaps.
 * */
struct NYA_SpringF32 {
    f32 value;
    f32 velocity;

    /** Oscillations per second. Zero is treated as NYA_SPRING_DEFAULT_FREQUENCY. */
    f32 frequency;

    /** Damping ratio. Zero is treated as 1, critically damped. */
    f32 damping;
};

struct NYA_SpringF32x2 {
    f32x2 value;
    f32x2 velocity;
    f32   frequency;
    f32   damping;
};

struct NYA_SpringF32x3 {
    f32x3 value;
    f32x3 velocity;
    f32   frequency;
    f32   damping;
};

/** What a zeroed `frequency` means. Responsive without being twitchy. */
#define NYA_SPRING_DEFAULT_FREQUENCY 4.0F

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * FUNCTIONS AND MACROS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/** Steps the spring toward `target` and returns its new value. */
NYA_API f32   nya_spring_f32(NYA_SpringF32* spring, f32 target, f32 delta_time_s);
NYA_API f32x2 nya_spring_f32x2(NYA_SpringF32x2* spring, f32x2 target, f32 delta_time_s);
NYA_API f32x3 nya_spring_f32x3(NYA_SpringF32x3* spring, f32x3 target, f32 delta_time_s);

/** Puts the spring at `value` with no velocity. For a teleport, where easing in would be wrong. */
NYA_API void nya_spring_f32_reset(NYA_SpringF32* spring, f32 value);
NYA_API void nya_spring_f32x2_reset(NYA_SpringF32x2* spring, f32x2 value);
NYA_API void nya_spring_f32x3_reset(NYA_SpringF32x3* spring, f32x3 value);

/** Whether the spring has effectively arrived: close to `target` and barely moving. */
NYA_API b8 nya_spring_f32_settled(const NYA_SpringF32* spring, f32 target, f32 epsilon) __attr_no_discard;
