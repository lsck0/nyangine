#include "nyangine-core/nyangine.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API DECLARATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/** The fractional part in [0, 1), the phase of a scroll within its wrap. Kept beside the wind's own helpers. */
NYA_INTERNAL f32 _nya_water_fract(f32 value) __attr_no_discard;

// Four wave bands whose weights sum to NYA_WATER_WAVE_PEAK; the water vertex shader mirrors these exactly.

/** The two along-flow octaves, the second shorter and faster. */
NYA_INTERNAL const f32 _NYA_WATER_OCTAVE[2] = { 0.55F, 0.35F };

/** The long cross swell, perpendicular to the current, at a longer wavelength and a slow travel. */
#define _NYA_WATER_SWELL 0.28F

/** The short cross chop, perpendicular to the current, at a shorter wavelength and a faster travel. */
#define _NYA_WATER_CHOP 0.22F

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PUBLIC API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

NYA_WaterFlow nya_water_flow(f32 time, f32 cycle_seconds) {
    // zero or less is read as one second, so a caller that leaves it unset still gets a wrapping flow.
    f32 cycle = cycle_seconds > 0.0F ? cycle_seconds : 1.0F;

    f32 t = time / cycle;

    // the first layer's phase, and a second half a cycle out of step with it.
    f32 phase_a = _nya_water_fract(t);
    f32 phase_b = _nya_water_fract(t + 0.5F);

    // A triangular weight: one when the first layer wraps, zero mid-cycle, so the wrap discontinuity is masked by the other layer.
    f32 blend = fabsf(1.0F - (2.0F * phase_a));

    return (NYA_WaterFlow){ .phase_a = phase_a, .phase_b = phase_b, .blend = blend };
}

f32 nya_water_wave_height(f32x2 xz, f32 time, f32x2 flow_direction, f32 amplitude, f32 frequency, f32 speed) {
    // zero means unset: a current along +x, the same rule the wind field uses for its direction.
    f32x2 flow = flow_direction;
    if (flow.x == 0.0F && flow.y == 0.0F) flow = (f32x2){ 1.0F, 0.0F };

    flow = nya_vector_normalize(flow);

    // a horizontal perpendicular for the cross chop, so the chop crosses the current rather than following it.
    f32x2 across = { -flow.y, flow.x };

    // how far along the current, and across it, this point sits — the phase the sines travel through.
    f32 along = nya_vector_dot(xz, flow);
    f32 side  = nya_vector_dot(xz, across);

    // two along-flow octaves, the second shorter and faster and offset so the pair does not beat as one wave.
    f32 phase_0 = (along * frequency) + (time * speed);
    f32 phase_1 = (along * frequency * 1.7F) + (time * speed * 1.3F) + 1.3F;

    f32 height = (_NYA_WATER_OCTAVE[0] * sinf(phase_0)) + (_NYA_WATER_OCTAVE[1] * sinf(phase_1));

    // a long rolling swell across the current, so the channel heaves rather than only rippling along the flow.
    f32 phase_2 = (side * frequency * 0.5F) + (time * speed * 0.7F) + 2.1F;
    height += _NYA_WATER_SWELL * sinf(phase_2);

    // the short cross chop, shorter still, so the surface is not a set of parallel ridges.
    f32 phase_3 = (side * frequency * 1.3F) + (time * speed * 1.9F);
    height += _NYA_WATER_CHOP * sinf(phase_3);

    // the octave, swell and chop weights sum to NYA_WATER_WAVE_PEAK, so this is bounded by amplitude times it.
    return amplitude * height;
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

f32 _nya_water_fract(f32 value) {
    return value - floorf(value);
}
