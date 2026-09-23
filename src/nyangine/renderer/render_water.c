#include "nyangine/nyangine.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API DECLARATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/** The fractional part in [0, 1), the phase of a scroll within its wrap. Kept beside the wind's own helpers. */
NYA_INTERNAL f32 _nya_water_fract(f32 value) __attr_no_discard;

/*
 * The along-flow octave weights and the cross-chop weight, summing to NYA_WATER_WAVE_PEAK. Two octaves along
 * the current plus one across it: enough that the surface does not read as a single travelling sine, few
 * enough to stay cheap in a vertex stage. The multipliers on frequency and speed are deliberately not whole
 * ratios, so the sum does not visibly repeat. The water vertex shader mirrors these exactly.
 */

/** The two along-flow octaves. Sum to one before the cross chop is added. */
NYA_INTERNAL const f32 _NYA_WATER_OCTAVE[2] = { 0.60F, 0.40F };

/** The cross-flow chop, perpendicular to the current, at a shorter wavelength and a faster travel. */
#define _NYA_WATER_CHOP 0.35F

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

    // a triangular weight: one exactly when the first layer wraps (phase 0 or 1) and zero mid-cycle, so a
    // layer's discontinuity at the wrap is always fully masked by the other layer.
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

    // the cross chop, shorter still, so the surface is not a set of parallel ridges.
    f32 phase_2 = (side * frequency * 0.8F) + (time * speed * 1.9F);
    height += _NYA_WATER_CHOP * sinf(phase_2);

    // the octave and chop weights sum to NYA_WATER_WAVE_PEAK, so this is bounded by amplitude times it.
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
