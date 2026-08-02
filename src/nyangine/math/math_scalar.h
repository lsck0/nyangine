/**
 * @file math_scalar.h
 *
 * Scalar helpers: comparison, clamping and interpolation.
 *
 * Kept free of every math header except base_assert.h on purpose. base_array.h, base_heap.h and
 * base_ring.h need nya_max for their growth policy, so anything this file pulls in becomes a
 * dependency of most of base. Vector and matrix types live in math_vector.h and math_matrix.h,
 * which base does not need to grow an array.
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
 *
 * Not FLT_EPSILON, which is the gap between 1.0F and the next float and so is far too tight for
 * anything that has been through a few operations. This is the "close enough for a game" figure:
 * about four decimal digits of slack, which survives a normalize or a matrix round trip.
 *
 * It is absolute, so it stops being meaningful for values much above ~1000. Comparing large
 * magnitudes wants a relative test instead.
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
 *
 * The names are underscored because the expansion is visible to the caller: a nya_min nested inside
 * a nya_max would otherwise declare _nya_a twice in the same scope.
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

/** Clamps `value` into [`min`, `max`]. An inverted range is a bug, not a silently empty interval. */
#define nya_clamp(value, min, max)                                                                                                                   \
    ({                                                                                                                                               \
        __auto_type _nya_clamp_value = (value);                                                                                                      \
        __auto_type _nya_clamp_min   = (min);                                                                                                        \
        __auto_type _nya_clamp_max   = (max);                                                                                                        \
        nya_assert_type_match(_nya_clamp_value, _nya_clamp_min);                                                                                     \
        nya_assert_type_match(_nya_clamp_value, _nya_clamp_max);                                                                                     \
        nya_assert(_nya_clamp_min <= _nya_clamp_max, "nya_clamp called with an inverted range.");                                                    \
        _nya_clamp_value < _nya_clamp_min ? _nya_clamp_min : (_nya_clamp_value > _nya_clamp_max ? _nya_clamp_max : _nya_clamp_value);                \
    })

/**
 * Linear interpolation from `a` to `b`.
 *
 * `t` outside [0, 1] extrapolates, which is deliberate; overshoot is how a spring or a back ease
 * gets its overshoot. The result is computed in the type of `t` so nya_lerp(0, 10, 0.5F) is 5 rather
 * than the integer 5 an all-integer expression would produce.
 * */
#define nya_lerp(a, b, t)                                                                                                                            \
    ({                                                                                                                                               \
        __auto_type _nya_lerp_a = (a);                                                                                                               \
        __auto_type _nya_lerp_b = (b);                                                                                                               \
        __auto_type _nya_lerp_t = (t);                                                                                                               \
        nya_assert_type_match(_nya_lerp_a, _nya_lerp_b);                                                                                             \
        _nya_lerp_a + (_nya_lerp_b - _nya_lerp_a) * _nya_lerp_t;                                                                                     \
    })
