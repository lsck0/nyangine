#include "nyangine/nyangine.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PUBLIC API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/*
 * ─────────────────────────────────────────────────────────
 * CONSTRUCTION
 * ─────────────────────────────────────────────────────────
 */

/*
 * Built by assigning the components rather than as `real + imaginary * I`, because I is a
 * float _Complex: the arithmetic form would compute a c32 and widen it, quietly costing precision
 * in a c64 or c128. It also keeps an infinite component from meeting a zero one and producing a NaN
 * that was never in the input.
 */

c32 nya_complex_f32(f32 real, f32 imaginary) {
    c32 result;
    nya_complex_real(result) = real;
    nya_complex_imag(result) = imaginary;

    return result;
}

c64 nya_complex_f64(f64 real, f64 imaginary) {
    c64 result;
    nya_complex_real(result) = real;
    nya_complex_imag(result) = imaginary;

    return result;
}

c128 nya_complex_f128(f128 real, f128 imaginary) {
    c128 result;
    nya_complex_real(result) = real;
    nya_complex_imag(result) = imaginary;

    return result;
}

c32 nya_complex_from_polar(f32 magnitude, f32 radians) __attr_overloaded {
    return nya_complex_f32(magnitude * cosf(radians), magnitude * sinf(radians));
}

c64 nya_complex_from_polar(f64 magnitude, f64 radians) __attr_overloaded {
    return nya_complex_f64(magnitude * cos(radians), magnitude * sin(radians));
}

c128 nya_complex_from_polar(f128 magnitude, f128 radians) __attr_overloaded {
    return nya_complex_f128(magnitude * cosl(radians), magnitude * sinl(radians));
}

c32 nya_complex_unit(f32 radians) __attr_overloaded {
    return nya_complex_f32(cosf(radians), sinf(radians));
}

c64 nya_complex_unit(f64 radians) __attr_overloaded {
    return nya_complex_f64(cos(radians), sin(radians));
}

c128 nya_complex_unit(f128 radians) __attr_overloaded {
    return nya_complex_f128(cosl(radians), sinl(radians));
}

/*
 * ─────────────────────────────────────────────────────────
 * OPERATIONS
 * ─────────────────────────────────────────────────────────
 */

f32 nya_complex_magnitude_squared(c32 value) __attr_overloaded {
    f32 real = nya_complex_real(value);
    f32 imag = nya_complex_imag(value);

    return real * real + imag * imag;
}

f64 nya_complex_magnitude_squared(c64 value) __attr_overloaded {
    f64 real = nya_complex_real(value);
    f64 imag = nya_complex_imag(value);

    return real * real + imag * imag;
}

f128 nya_complex_magnitude_squared(c128 value) __attr_overloaded {
    f128 real = nya_complex_real(value);
    f128 imag = nya_complex_imag(value);

    return real * real + imag * imag;
}

/*
 * Zero has no direction, so there is nothing correct to normalize it to. Returning it unchanged
 * keeps a NaN out of whatever the caller does next; callers that care can test the magnitude first.
 */

c32 nya_complex_normalize(c32 value) __attr_overloaded {
    f32 magnitude = cabsf(value);
    if (magnitude == 0.0F) return value;

    return nya_complex_f32(nya_complex_real(value) / magnitude, nya_complex_imag(value) / magnitude);
}

c64 nya_complex_normalize(c64 value) __attr_overloaded {
    f64 magnitude = cabs(value);
    if (magnitude == 0.0) return value;

    return nya_complex_f64(nya_complex_real(value) / magnitude, nya_complex_imag(value) / magnitude);
}

c128 nya_complex_normalize(c128 value) __attr_overloaded {
    f128 magnitude = cabsl(value);
    if (magnitude == 0.0L) return value;

    return nya_complex_f128(nya_complex_real(value) / magnitude, nya_complex_imag(value) / magnitude);
}

c32 nya_complex_lerp(c32 a, c32 b, f32 t) __attr_overloaded {
    return nya_complex_f32(
        nya_complex_real(a) + (nya_complex_real(b) - nya_complex_real(a)) * t,
        nya_complex_imag(a) + (nya_complex_imag(b) - nya_complex_imag(a)) * t
    );
}

c64 nya_complex_lerp(c64 a, c64 b, f64 t) __attr_overloaded {
    return nya_complex_f64(
        nya_complex_real(a) + (nya_complex_real(b) - nya_complex_real(a)) * t,
        nya_complex_imag(a) + (nya_complex_imag(b) - nya_complex_imag(a)) * t
    );
}

c32 nya_complex_slerp(c32 a, c32 b, f32 t) __attr_overloaded {
    f32 magnitude_a = cabsf(a);
    f32 magnitude_b = cabsf(b);

    // A zero endpoint has no angle to interpolate, so there is no arc to follow.
    if (magnitude_a == 0.0F || magnitude_b == 0.0F) return nya_complex_lerp(a, b, t);

    f32 angle_a = cargf(a);
    f32 angle_b = cargf(b);

    // carg returns (-π, π], so two angles either side of the cut differ by nearly a full turn.
    // Wrapping the difference is what keeps the interpolation on the short arc.
    f32 delta = angle_b - angle_a;
    while (delta > (f32)M_PI) delta -= 2.0F * (f32)M_PI;
    while (delta < -(f32)M_PI) delta += 2.0F * (f32)M_PI;

    return nya_complex_from_polar(magnitude_a + (magnitude_b - magnitude_a) * t, angle_a + delta * t);
}

c64 nya_complex_slerp(c64 a, c64 b, f64 t) __attr_overloaded {
    f64 magnitude_a = cabs(a);
    f64 magnitude_b = cabs(b);

    if (magnitude_a == 0.0 || magnitude_b == 0.0) return nya_complex_lerp(a, b, t);

    f64 angle_a = carg(a);
    f64 angle_b = carg(b);

    f64 delta = angle_b - angle_a;
    while (delta > M_PI) delta -= 2.0 * M_PI;
    while (delta < -M_PI) delta += 2.0 * M_PI;

    return nya_complex_from_polar(magnitude_a + (magnitude_b - magnitude_a) * t, angle_a + delta * t);
}

b8 nya_complex_approx_equals(c32 a, c32 b, f32 epsilon) __attr_overloaded {
    return cabsf(a - b) <= epsilon;
}

b8 nya_complex_approx_equals(c64 a, c64 b, f64 epsilon) __attr_overloaded {
    return cabs(a - b) <= epsilon;
}

b8 nya_complex_is_finite(c32 value) __attr_overloaded {
    return isfinite(nya_complex_real(value)) && isfinite(nya_complex_imag(value));
}

b8 nya_complex_is_finite(c64 value) __attr_overloaded {
    return isfinite(nya_complex_real(value)) && isfinite(nya_complex_imag(value));
}
