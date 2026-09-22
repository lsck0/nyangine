/**
 * @file math_scalar.h
 * */
#pragma once

#include "nyangine/base/base_assert.h"
#include "nyangine/base/base_basic.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * CONSTANTS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/**
 * Default tolerance for f32 comparisons.
 * */
#define NYA_EPSILON 1.0e-6F

/** π and friends come from <math.h>, which base_basic.h already includes: M_PI, M_PI_2, M_SQRT2. */

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * FUNCTIONS AND MACROS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/*
 * Every argument is bound to a temporary before use. Written the obvious way these macros expand
 * their operands two or more times, so nya_min(i++, j) increments twice and nya_clamp(read(), lo,
 * hi) calls read() three times. The temporaries also give nya_assert_type_match something stable to
 * inspect.
 */

#define nya_min(a, b)                                                                                                                                \
    ({                                                                                                                                               \
        __auto_type _nya_min_a = (a);                                                                                                                \
        __auto_type _nya_min_b = (b);                                                                                                                \
        nya_assert_type_match(_nya_min_a, _nya_min_b);                                                                                               \
        _nya_min_a < _nya_min_b ? _nya_min_a : _nya_min_b;                                                                                           \
    })

#define nya_max(a, b)                                                                                                                                \
    ({                                                                                                                                               \
        __auto_type _nya_max_a = (a);                                                                                                                \
        __auto_type _nya_max_b = (b);                                                                                                                \
        nya_assert_type_match(_nya_max_a, _nya_max_b);                                                                                               \
        _nya_max_a > _nya_max_b ? _nya_max_a : _nya_max_b;                                                                                           \
    })

/**
 * Clamps `value` into [`min`, `max`]. An inverted range is a bug, not a silently empty interval.
 *
 * NaN lands on `min`: it compares false both ways, so without the first test it walked through as NaN. Text never
 * parses to one, but a 0/0 upstream or a peer's raw bytes do, and "clamped" has to mean in range. For integers the
 * first test is always false.
 * */
#define nya_clamp(value, min, max)                                                                                                                   \
    ({                                                                                                                                               \
        __auto_type _nya_clamp_value = (value);                                                                                                      \
        __auto_type _nya_clamp_min   = (min);                                                                                                        \
        __auto_type _nya_clamp_max   = (max);                                                                                                        \
        nya_assert_type_match(_nya_clamp_value, _nya_clamp_min);                                                                                     \
        nya_assert_type_match(_nya_clamp_value, _nya_clamp_max);                                                                                     \
        nya_assert(_nya_clamp_min <= _nya_clamp_max, "nya_clamp called with an inverted range.");                                                    \
        _nya_clamp_value != _nya_clamp_value ? _nya_clamp_min                                                                                        \
        : _nya_clamp_value < _nya_clamp_min  ? _nya_clamp_min                                                                                        \
        : _nya_clamp_value > _nya_clamp_max  ? _nya_clamp_max                                                                                        \
                                             : _nya_clamp_value;                                                                                      \
    })

/**
 * Linear interpolation from `a` to `b`.
 * */
#define nya_lerp(a, b, t)                                                                                                                            \
    ({                                                                                                                                               \
        __auto_type _nya_lerp_a = (a);                                                                                                               \
        __auto_type _nya_lerp_b = (b);                                                                                                               \
        __auto_type _nya_lerp_t = (t);                                                                                                               \
        nya_assert_type_match(_nya_lerp_a, _nya_lerp_b);                                                                                             \
        _nya_lerp_a + (_nya_lerp_b - _nya_lerp_a) * _nya_lerp_t;                                                                                     \
    })
