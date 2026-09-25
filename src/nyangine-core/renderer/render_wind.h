/**
 * @file render_wind.h
 *
 * One analytic wind field, sampled by position and time. No grid and no compute pass: a steady
 * directional push plus a layered-sine gust, so it is a pure function a vertex shader, a particle
 * system or a fluid emitter can each read without any shared buffer.
 *
 * ```c
 * NYA_WindField wind = nya_wind_field((NYA_WindOptions){ .direction = { 1, 0, 0.3F }, .strength = 2.0F, .gustiness = 0.6F });
 *
 * nya_wind_advance(&wind, delta_time_s);              // once a frame
 * f32x3 push = nya_wind_sample(&wind, plant.position); // world-space displacement/force
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
 * Sine layers the gust is summed from. Three reads as wind rather than a single throb, and the
 * per-octave amplitudes below sum to one so the gust stays in [-1, 1] whatever this is.
 * */
#define NYA_WIND_OCTAVES 3

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * TYPES
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

typedef struct NYA_WindField   NYA_WindField;
typedef struct NYA_WindOptions NYA_WindOptions;

/**
 * How to build a field. Every field has a usable default, so `(NYA_WindOptions){ 0 }` is a light
 * breeze blowing along +x.
 * */
struct NYA_WindOptions {
    /** Which way it blows, in world space. The y is kept, but foliage cares about the horizontal part. Zero is +x. */
    f32x3 direction;

    /** The steady push, in world units. Zero is read as one. */
    f32 strength;

    /** How much the gust swings the push, in [0, 1]. Zero is a dead-steady wind; one doubles and stills it in turn. */
    f32 gustiness;

    /** Phase offset, so two fields over the same ground gust out of step. Any value; zero is fine. */
    f32 seed;
};

/**
 * A wind field. A plain value the caller owns: put it on the stack, in an arena, or in a component.
 * Sampling is a pure function of its fields, so a copy samples identically. See nya_wind_at.
 * */
struct NYA_WindField {
    /** Normalized on the way in. */
    f32x3 direction;

    f32 strength;
    f32 gustiness;
    f32 seed;

    /** Advanced by nya_wind_advance, and what nya_wind_sample reads. Sample an explicit time with nya_wind_at. */
    f32 time;
};

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * FUNCTIONS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/** Builds a field, applying the defaults in NYA_WindOptions. */
NYA_API NYA_WindField nya_wind_field(NYA_WindOptions options) __attr_no_discard;

/** Repoints the wind and resets its strength and gust, leaving the accumulated time alone. */
NYA_API void nya_wind_set(NYA_WindField* field, f32x3 direction, f32 strength, f32 gustiness);

/** Advances the field's own clock. Call once a frame; nya_wind_sample reads what it accumulates. */
NYA_API void nya_wind_advance(NYA_WindField* field, f32 delta_time_s);

/**
 * The wind at a world position and an explicit time, as a displacement/force vector: the steady push
 * plus the gust swinging it, plus a small perpendicular swirl. A pure function, so the same arguments
 * always give the same vector — which is what the determinism test rests on.
 * */
NYA_API f32x3 nya_wind_at(const NYA_WindField* field, f32x3 position, f32 time) __attr_no_discard;

/** nya_wind_at at the field's own accumulated time. The one a frame uses. */
NYA_API f32x3 nya_wind_sample(const NYA_WindField* field, f32x3 position) __attr_no_discard;

/**
 * The steady push alone, direction times strength, with no gust. What a shader leans foliage along
 * before it adds the gust's sway.
 * */
NYA_API f32x3 nya_wind_base(const NYA_WindField* field) __attr_no_discard;
