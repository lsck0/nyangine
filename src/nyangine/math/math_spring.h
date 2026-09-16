/**
 * @file math_spring.h
 *
 * ```c
 * static NYA_SpringF32 zoom = { .value = 1.0F, .frequency = 4.0F, .damping = 1.0F };
 *
 * // every frame. The target may change at any moment; the spring absorbs it.
 * nya_spring_f32(&zoom, target_zoom, delta_time_s);
 * camera.zoom = zoom.value;
 * ```
 *
 * Not `nya_ease_spring` from math_tween.h, which is a stateless damped cosine of `t` and cannot survive
 * interruption. This is the integrator.
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
