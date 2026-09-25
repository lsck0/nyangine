/**
 * @file render_water.h
 *
 * The pure math a flowing water surface rests on, kept out of the GPU path so a headless test can reach
 * it and both builds compile it. Two pieces, each a pure function of its arguments:
 *
 *  - `nya_water_flow` — the flow-map time weights. A scrolling ripple wraps every `cycle` seconds, and a
 *    single scroll would visibly jump at the wrap. So two copies are scrolled half a cycle out of step and
 *    blended on a triangular weight that is one exactly when a layer wraps and zero when it is mid-cycle,
 *    so the wrap of one layer is always hidden behind the other. The water fragment shader reads the same
 *    three numbers; keeping the CPU mirror here is what the determinism test pins.
 *
 *  - `nya_water_wave_height` — the summed-sine surface height at a point and time, a faithful mirror of the
 *    displacement the water vertex shader applies. The renderer uses it to pad a water surface's cull radius
 *    by how far the waves lift it, and the test uses it for determinism and the amplitude bound.
 *
 * ```c
 * NYA_WaterFlow flow = nya_water_flow(elapsed_s, 6.0F);   // phase_a, phase_b, blend
 * f32           lift = nya_water_wave_height((f32x2){ x, z }, elapsed_s, (f32x2){ 1, 0 }, 0.2F, 0.6F, 1.0F);
 * ```
 * */
#pragma once

#include "nyangine-std/base/base_attributes.h"
#include "nyangine-std/base/base_types.h"
#include "nyangine-std/math/math_vector.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * CONSTANTS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/**
 * The most a wave lifts the surface, as a multiple of its amplitude: the octave weights nya_water_wave_height
 * sums (0.55 + 0.35 for the two along-flow octaves, plus 0.28 for the long cross swell and 0.22 for the short
 * cross chop). The renderer pads a water surface's cull radius by this times the amplitude so a lifted crest
 * does not pop out at a screen edge, and the test bounds the height by it.
 * */
#define NYA_WATER_WAVE_PEAK 1.40F

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * TYPES
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

typedef struct NYA_WaterFlow NYA_WaterFlow;

/**
 * The two scroll phases of a flow map and the weight that blends them. See nya_water_flow and the water
 * fragment shader, which computes the identical three numbers to scroll its ripple normals.
 * */
struct NYA_WaterFlow {
    /** The first layer's scroll offset, in cycles: how far along its wrap it is, in [0, 1). */
    f32 phase_a;

    /** The second layer's scroll offset, half a cycle out of step with the first, in [0, 1). */
    f32 phase_b;

    /** How much of the second layer to mix in, in [0, 1]: one where the first layer wraps, zero mid-cycle. */
    f32 blend;
};

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * FUNCTIONS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/**
 * The flow-map scroll phases and blend weight at a time. `cycle_seconds` is how long a layer takes to wrap;
 * zero or less is read as one second. A pure function of its two arguments, and periodic in `cycle_seconds`,
 * so `nya_water_flow(t)` and `nya_water_flow(t + cycle)` are identical — which is what makes the wrap invisible.
 * */
NYA_API NYA_WaterFlow nya_water_flow(f32 time, f32 cycle_seconds) __attr_no_discard;

/**
 * The surface height at a horizontal point and time: the same summed sines the water vertex shader adds to a
 * vertex's y. `flow_direction` is the current's heading on the ground (its horizontal part is used; a zero
 * vector is read as +x). `amplitude` scales the whole sum, `frequency` sets the wavelength and `speed` how
 * fast crests travel. A pure function, bounded by `amplitude * NYA_WATER_WAVE_PEAK`.
 * */
NYA_API f32 nya_water_wave_height(f32x2 xz, f32 time, f32x2 flow_direction, f32 amplitude, f32 frequency, f32 speed)
    __attr_no_discard;
