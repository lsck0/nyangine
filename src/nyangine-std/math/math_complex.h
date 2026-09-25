/**
 * @file math_complex.h
 *
 * ```c
 * c64 z = nya_complex(3.0, 4.0);
 * c64 w = z * z + 1.0;
 * f64 r = cabs(w);
 * nya_complex_imag(z) = 0.0;          // the accessors are assignable
 * printf(FMTc64 "\n", FMTc64_ARG(w));
 * ```
 * */
#pragma once

#include "nyangine-std/base/base_attributes.h"
#include "nyangine-std/base/base_types.h"
#include "nyangine-std/math/math_scalar.h"

// TYPES

/* c32, c64 and c128 are declared in base_types.h, next to the f types whose width they carry. */

// CONSTANTS

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

// FORMATTING

/* Rendered as `a+bi`, with the sign of the imaginary part carried by %+. */

#define FMTc32  "%g%+gi"
#define FMTc64  "%g%+gi"
#define FMTc128 "%Lg%+Lgi"

#define FMTc32_ARG(value)  (f64) nya_complex_real(value), (f64)nya_complex_imag(value)
#define FMTc64_ARG(value)  (f64) nya_complex_real(value), (f64)nya_complex_imag(value)
#define FMTc128_ARG(value) (f128) nya_complex_real(value), (f128)nya_complex_imag(value)

// FUNCTIONS AND MACROS

// COMPONENTS

// __real__/__imag__ rather than creal/cimag: these are assignable lvalues and stay exact at every width.

#define nya_complex_real(value) __real__(value)
#define nya_complex_imag(value) __imag__(value)

// CONSTRUCTION

/**
 * Builds a complex number from its components, at the width of the arguments.
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
 * */
NYA_API c32  nya_complex_unit(f32 radians) __attr_overloaded __attr_no_discard;
NYA_API c64  nya_complex_unit(f64 radians) __attr_overloaded __attr_no_discard;
NYA_API c128 nya_complex_unit(f128 radians) __attr_overloaded __attr_no_discard;

// OPERATIONS

// cabs, carg, conj, cexp, clog, cpow, csqrt and the trig functions come from <tgmath.h>; only what it lacks lives below.

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
 * */
NYA_API c32 nya_complex_slerp(c32 a, c32 b, f32 t) __attr_overloaded __attr_no_discard;
NYA_API c64 nya_complex_slerp(c64 a, c64 b, f64 t) __attr_overloaded __attr_no_discard;

/** Compares by distance, so it is not fooled by the sign of a zero component. */
NYA_API b8 nya_complex_approx_equals(c32 a, c32 b, f32 epsilon) __attr_overloaded __attr_no_discard;
NYA_API b8 nya_complex_approx_equals(c64 a, c64 b, f64 epsilon) __attr_overloaded __attr_no_discard;

/** True when both components are finite; false if either is NaN or infinite. */
NYA_API b8 nya_complex_is_finite(c32 value) __attr_overloaded __attr_no_discard;
NYA_API b8 nya_complex_is_finite(c64 value) __attr_overloaded __attr_no_discard;
