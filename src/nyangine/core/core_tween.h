/**
 * @file core_tween.h
 *
 * ```c
 * // Slide a panel in, then flash it, as one timeline.
 * nya_tween_sequence((NYA_TweenSpec[]){
 *     nya_tween_spec_f32(&panel.x, 0.0F,   0.30F, .ease = NYA_EASE_CUBIC_OUT),
 *     nya_tween_spec_f32(&panel.alpha, 1.0F, 0.15F, .ease = NYA_EASE_LINEAR, .on_complete = nya_callback(panel_ready)),
 * }, 2);
 *
 * // Or one on its own, held so it can be cancelled.
 * NYA_Tween handle = nya_tween_f32(&camera.zoom, 2.0F, 0.5F, .ease = NYA_EASE_BACK_OUT, .delay = 0.1F);
 * ```
 * */
#pragma once

#include "nyangine/base/base_types.h"
#include "nyangine/core/core_callback.h"
#include "nyangine/math/math_tween.h"
#include "nyangine/math/math_vector.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * CONSTANTS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/**
 * How many tweens can run at once.
 * */
#ifndef NYA_TWEEN_MAX
#define NYA_TWEEN_MAX 512
#endif

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * TYPES
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

typedef struct NYA_Tween        NYA_Tween;
typedef struct NYA_TweenOptions NYA_TweenOptions;
typedef struct NYA_TweenSpec    NYA_TweenSpec;

/**
 * Identifies a running tween.
 * */
struct NYA_Tween {
    u32 index;
    u32 generation;
};

#define NYA_TWEEN_NONE ((NYA_Tween){ .index = 0, .generation = 0 })

/** What a tween is animating. Set by the nya_tween_* entry points, not by callers. */
typedef enum NYA_TweenTarget {
    NYA_TWEEN_TARGET_F32 = 0,
    NYA_TWEEN_TARGET_F32X2,
    NYA_TWEEN_TARGET_F32X3,
    NYA_TWEEN_TARGET_F32X4,

    NYA_TWEEN_TARGET_COUNT,
} NYA_TweenTarget;

/** Everything optional about a tween. Every field's zero is its default. */
struct NYA_TweenOptions {
    /** The curve. Zero is NYA_EASE_LINEAR. */
    NYA_EaseType ease;

    /** Seconds to wait before the first sample. The start value is read when the delay ends, not now. */
    f32 delay;

    /**
     * How many times to run. Zero and one both mean once; `NYA_TWEEN_REPEAT_FOREVER` never finishes.
     * */
    u32 repeat;

    /** Whether alternate runs play backwards. Only meaningful with `repeat`. */
    b8 yoyo;

    /** Run when the last repetition finishes. Not run when the tween is cancelled. */
    NYA_CallbackHandle on_complete;
};

/** Repeat count meaning "until cancelled". */
#define NYA_TWEEN_REPEAT_FOREVER 0xFFFFFFFFU

/** One entry in a timeline. Build these with the nya_tween_spec_* macros. */
struct NYA_TweenSpec {
    NYA_TweenTarget  target;
    void*            address;
    f32              to_f32;
    f32x2            to_f32x2;
    f32x3            to_f32x3;
    f32x4            to_f32x4;
    f32              duration;
    NYA_TweenOptions options;
};

/** The signature a completion callback must have. */
typedef void (*NYA_TweenOnCompleteFn)(void* address);

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * FUNCTIONS AND MACROS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

NYA_API void nya_system_tween_init(void);
NYA_API void nya_system_tween_deinit(void);

/** Advances every running tween. Call once per frame from the game's loop. */
NYA_API void nya_system_tween_update(f32 delta_time_s);

NYA_API NYA_Tween nya_tween_f32_with_options(f32* address, f32 to, f32 duration_s, NYA_TweenOptions options);
NYA_API NYA_Tween nya_tween_f32x2_with_options(f32x2* address, f32x2 to, f32 duration_s, NYA_TweenOptions options);
NYA_API NYA_Tween nya_tween_f32x3_with_options(f32x3* address, f32x3 to, f32 duration_s, NYA_TweenOptions options);
NYA_API NYA_Tween nya_tween_f32x4_with_options(f32x4* address, f32x4 to, f32 duration_s, NYA_TweenOptions options);

/**
 * `nya_tween_f32(&x, 10.0F, 0.5F, .ease = NYA_EASE_CUBIC_OUT)` — options are designated initialisers.
 *
 * ⚠ **A vector literal must be parenthesised**, because the preprocessor splits arguments on commas and
 * braces do not protect them — only parentheses do:
 *
 * ```c
 * nya_tween_f32x3(&position, ((f32x3){ 1.0F, 2.0F, 3.0F }), 0.5F);   // right
 * nya_tween_f32x3(&position,  (f32x3){ 1.0F, 2.0F, 3.0F },  0.5F);   // three arguments, not one
 * ```
 * */
#define nya_tween_f32(address, to, duration_s, ...)   nya_tween_f32_with_options(address, to, duration_s, (NYA_TweenOptions){ __VA_ARGS__ })
#define nya_tween_f32x2(address, to, duration_s, ...) nya_tween_f32x2_with_options(address, to, duration_s, (NYA_TweenOptions){ __VA_ARGS__ })
#define nya_tween_f32x3(address, to, duration_s, ...) nya_tween_f32x3_with_options(address, to, duration_s, (NYA_TweenOptions){ __VA_ARGS__ })
#define nya_tween_f32x4(address, to, duration_s, ...) nya_tween_f32x4_with_options(address, to, duration_s, (NYA_TweenOptions){ __VA_ARGS__ })

/**
 * Starts `specs` one after another, each delayed by the total duration of those before it.
 * */
NYA_API NYA_Tween nya_tween_sequence(const NYA_TweenSpec* specs, u32 count);

#define nya_tween_spec_f32(address_, to_, duration_, ...)                                                                                            \
    ((NYA_TweenSpec){ .target = NYA_TWEEN_TARGET_F32,                                                                                                \
                      .address = (address_),                                                                                                         \
                      .to_f32 = (to_),                                                                                                                \
                      .duration = (duration_),                                                                                                        \
                      .options = (NYA_TweenOptions){ __VA_ARGS__ } })

#define nya_tween_spec_f32x2(address_, to_, duration_, ...)                                                                                          \
    ((NYA_TweenSpec){ .target = NYA_TWEEN_TARGET_F32X2,                                                                                              \
                      .address = (address_),                                                                                                         \
                      .to_f32x2 = (to_),                                                                                                              \
                      .duration = (duration_),                                                                                                        \
                      .options = (NYA_TweenOptions){ __VA_ARGS__ } })

#define nya_tween_spec_f32x3(address_, to_, duration_, ...)                                                                                          \
    ((NYA_TweenSpec){ .target = NYA_TWEEN_TARGET_F32X3,                                                                                              \
                      .address = (address_),                                                                                                         \
                      .to_f32x3 = (to_),                                                                                                              \
                      .duration = (duration_),                                                                                                        \
                      .options = (NYA_TweenOptions){ __VA_ARGS__ } })

/** Stops a tween, leaving the value where it is. The completion callback does not run. */
NYA_API void nya_tween_cancel(NYA_Tween tween);

/**
 * Stops every tween writing to `address`, leaving the value where it is.
 * */
NYA_API u32 nya_tween_cancel_target(const void* address);

/** Stops everything. Completion callbacks do not run. */
NYA_API void nya_tween_cancel_all(void);

/** Whether the handle still names a running tween. */
NYA_API b8 nya_tween_active(NYA_Tween tween) __attr_no_discard;

/** How many tweens are running. */
NYA_API u32 nya_tween_count(void) __attr_no_discard;

/**
 * How far along a tween is, from 0 at its start value to 1 at its target.
 * */
NYA_API f32 nya_tween_progress(NYA_Tween tween) __attr_no_discard;
