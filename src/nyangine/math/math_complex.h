/**
 * @file math_complex.h
 *
 * Complex numbers, built on C's native `_Complex` rather than a struct: operators do complex
 * arithmetic directly and `<tgmath.h>` dispatches `cabs`/`carg`/`cexp`/`csqrt` on the argument type,
 * so there are no add/mul wrappers here, same reasoning as math_vector.h.
 *
 * ```c
 * c64 z = nya_complex(3.0, 4.0);
 * c64 w = z * z + 1.0;
 * f64 r = cabs(w);
 * nya_complex_imag(z) = 0.0;          // the accessors are assignable
 * printf(FMTc64 "\n", FMTc64_ARG(w));
 * ```
 *
 * Adds what the standard library leaves out: `c32`/`c64`/`c128` naming to match `f32`/`f64`/`f128`,
 * printing, assignable component accessors, polar construction, and epsilon-based comparison and
 * interpolation.
 *
 * `I` from `<complex.h>` is `_Complex_I` (`float _Complex`), so `1.0 + 2.0*I` builds a c32 and widens,
 * losing precision in a c128 expression; nya_complex builds the value at the requested width instead.
 * */
#pragma once

#include "nyangine/base/base_attributes.h"
#include "nyangine/base/base_types.h"
#include "nyangine/math/math_scalar.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * TYPES
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/* c32, c64 and c128 are declared in base_types.h, next to the f types whose width they carry. */

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * CONSTANTS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

#define c32_zero  ((c32)0.0F)
#define c64_zero  ((c64)0.0)
#define c128_zero ((c128)0.0L)

#define c32_one  ((c32)1.0F)
#define c64_one  ((c64)1.0)
#define c128_one ((c128)1.0L)

/** The imaginary unit, at each width. */
#define c32_i  nya_complex_f32(0.0F, 1.0F)
#define c64_i  nya_complex_f64(0.0, 1.0)
#define c128_i nya_complex_f128(0.0L, 1.0L)

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * FORMATTING
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/* Rendered as `a+bi`, with the sign of the imaginary part carried by %+. */

#define FMTc32  "%g%+gi"
#define FMTc64  "%g%+gi"
#define FMTc128 "%Lg%+Lgi"

#define FMTc32_ARG(value)  (f64) nya_complex_real(value), (f64)nya_complex_imag(value)
#define FMTc64_ARG(value)  (f64) nya_complex_real(value), (f64)nya_complex_imag(value)
#define FMTc128_ARG(value) (f128) nya_complex_real(value), (f128)nya_complex_imag(value)

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * FUNCTIONS AND MACROS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/*
 * ─────────────────────────────────────────────────────────
 * COMPONENTS
 * ─────────────────────────────────────────────────────────
 */

/*
 * __real__ and __imag__ rather than creal/cimag because these are lvalues: `nya_complex_imag(z) = 0`
 * compiles, where the standard functions would only let you read. They are also exact at every
 * width, while creal on a c128 would go through a double on a strict reading.
 */

#define nya_complex_real(value) __real__(value)
#define nya_complex_imag(value) __imag__(value)

/*
 * ─────────────────────────────────────────────────────────
 * CONSTRUCTION
 * ─────────────────────────────────────────────────────────
 */

/**
 * Builds a complex number from its components, at the width of the arguments.
 *
 * Dispatches on the argument type, so nya_complex(1.0F, 2.0F) is a c32 and nya_complex(1.0, 2.0) a
 * c64. Use the explicit nya_complex_f32 / _f64 / _f128 forms when the width has to be pinned
 * regardless of what the arguments happen to be.
 * */
#define nya_complex(real, imaginary)                                                                                                                 \
    _Generic((real) + (imaginary), f32: nya_complex_f32, f128: nya_complex_f128, default: nya_complex_f64)((real), (imaginary))

NYA_API c32  nya_complex_f32(f32 real, f32 imaginary) __attr_no_discard;
NYA_API c64  nya_complex_f64(f64 real, f64 imaginary) __attr_no_discard;
NYA_API c128 nya_complex_f128(f128 real, f128 imaginary) __attr_no_discard;

/** Builds from magnitude and angle in radians. The inverse of cabs and carg together. */
NYA_API c32  nya_complex_from_polar(f32 magnitude, f32 radians) __attr_overloaded __attr_no_discard;
NYA_API c64  nya_complex_from_polar(f64 magnitude, f64 radians) __attr_overloaded __attr_no_discard;
NYA_API c128 nya_complex_from_polar(f128 magnitude, f128 radians) __attr_overloaded __attr_no_discard;

/**
 * The unit complex number at `radians`, that is e^(i·θ).
 *
 * Multiplying by one of these rotates in the plane, which is the 2D counterpart of what a unit
 * quaternion does in 3D.
 * */
NYA_API c32  nya_complex_unit(f32 radians) __attr_overloaded __attr_no_discard;
NYA_API c64  nya_complex_unit(f64 radians) __attr_overloaded __attr_no_discard;
NYA_API c128 nya_complex_unit(f128 radians) __attr_overloaded __attr_no_discard;

/*
 * ─────────────────────────────────────────────────────────
 * OPERATIONS
 * ─────────────────────────────────────────────────────────
 */

/* cabs, carg, conj, cexp, clog, cpow, csqrt and the trig functions come from <tgmath.h>, which
 * base_basic.h already includes. Only what it does not provide lives below. */

/** Squared magnitude. Prefer it over cabs for comparisons; it skips the square root. */
NYA_API f32  nya_complex_magnitude_squared(c32 value) __attr_overloaded __attr_no_discard;
NYA_API f64  nya_complex_magnitude_squared(c64 value) __attr_overloaded __attr_no_discard;
NYA_API f128 nya_complex_magnitude_squared(c128 value) __attr_overloaded __attr_no_discard;

/** Scales to unit magnitude. Returns zero unchanged rather than dividing by it. */
NYA_API c32  nya_complex_normalize(c32 value) __attr_overloaded __attr_no_discard;
NYA_API c64  nya_complex_normalize(c64 value) __attr_overloaded __attr_no_discard;
NYA_API c128 nya_complex_normalize(c128 value) __attr_overloaded __attr_no_discard;

/** Straight line interpolation. For rotation prefer nya_complex_slerp, which keeps the magnitude. */
NYA_API c32 nya_complex_lerp(c32 a, c32 b, f32 t) __attr_overloaded __attr_no_discard;
NYA_API c64 nya_complex_lerp(c64 a, c64 b, f64 t) __attr_overloaded __attr_no_discard;

/**
 * Interpolates along the arc: magnitude and angle move independently.
 *
 * Takes the shortest way round, so slerping from just under a half turn to just over it does not
 * sweep the long way. Falls back to a plain lerp when either endpoint is zero and has no angle.
 * */
NYA_API c32 nya_complex_slerp(c32 a, c32 b, f32 t) __attr_overloaded __attr_no_discard;
NYA_API c64 nya_complex_slerp(c64 a, c64 b, f64 t) __attr_overloaded __attr_no_discard;

/** Compares by distance, so it is not fooled by the sign of a zero component. */
NYA_API b8 nya_complex_approx_equals(c32 a, c32 b, f32 epsilon) __attr_overloaded __attr_no_discard;
NYA_API b8 nya_complex_approx_equals(c64 a, c64 b, f64 epsilon) __attr_overloaded __attr_no_discard;

/** True when both components are finite; false if either is NaN or infinite. */
NYA_API b8 nya_complex_is_finite(c32 value) __attr_overloaded __attr_no_discard;
NYA_API b8 nya_complex_is_finite(c64 value) __attr_overloaded __attr_no_discard;
