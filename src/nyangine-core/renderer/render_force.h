/**
 * @file render_force.h
 *
 * One analytic force field, sampled by position, velocity and time. No grid and no compute pass: a
 * closed-form acceleration a particle system or a fluid emitter reads without any shared buffer, the
 * same shape [[render_wind]] takes. Where wind is one rich thing — a steady push plus a layered-sine
 * gust — this is the general primitive: a small set of composable kinds (uniform, point, vortex, drag,
 * turbulence) that sum, so a scene builds the behaviour it wants out of orthogonal pieces rather than
 * out of a bespoke field per effect.
 *
 * ```c
 * // a gravity well plus a stirring turbulence, held together in one set the caller owns.
 * NYA_ForceSet forces = { 0 };
 * forces.fields[forces.count++] = nya_force_field((NYA_ForceOptions){
 *     .kind = NYA_FORCE_POINT, .center = { 0, 0, 0 }, .strength = -40.0F, .radius = 8.0F, .falloff = NYA_FORCE_FALLOFF_INVERSE_SQUARE });
 * forces.fields[forces.count++] = nya_force_field((NYA_ForceOptions){
 *     .kind = NYA_FORCE_TURBULENCE, .strength = 6.0F, .scale = 0.2F, .seed = 3.0F });
 *
 * nya_forces_advance(&forces, delta_time_s);                                 // once a frame
 * f32x3 acceleration = nya_forces_sample(&forces, particle.position, particle.velocity);
 * particle.velocity += acceleration * delta_time_s;
 * ```
 *
 * Relation to wind: a NYA_FORCE_UNIFORM field is exactly the steady directional push wind leans
 * foliage along (direction times strength), so the plain directional case is subsumed here. Wind is
 * left intact — it keeps its gust and its swirl, which this primitive deliberately does not carry —
 * and a scene may read both, summing a wind sample and a force sample into the same velocity.
 * */
#pragma once

#include "nyangine-std/base/base_attributes.h"
#include "nyangine-std/base/base_types.h"
#include "nyangine-std/math/math_noise.h"
#include "nyangine-std/math/math_vector.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * CONSTANTS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/**
 * Fields one NYA_ForceSet holds at once. A scene composes a handful of forces — a well, a breeze, a
 * stir — not a table of them, and eight leaves room before the fixed array is the thing telling you to
 * merge two of them. No allocation rides on it: a set is a plain value the caller owns.
 * */
#ifndef NYA_FORCE_SET_MAX
#define NYA_FORCE_SET_MAX 8
#endif

/** Turbulence spatial frequency, per world unit, when NYA_ForceOptions.scale is zero. */
#define NYA_FORCE_TURBULENCE_SCALE 0.1F

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * TYPES
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

typedef enum NYA_ForceKind    NYA_ForceKind;
typedef enum NYA_ForceFalloff NYA_ForceFalloff;
typedef struct NYA_ForceOptions NYA_ForceOptions;
typedef struct NYA_ForceField   NYA_ForceField;
typedef struct NYA_ForceSet     NYA_ForceSet;

/** Which shape of force a field produces. Each reads a subset of the options; see NYA_ForceOptions. */
enum NYA_ForceKind {
    /**
     * A constant directional push, `direction * strength`, everywhere and always. The plain case wind
     * carries as its steady part; use this when you want that push without wind's gust.
     * */
    NYA_FORCE_UNIFORM = 0,

    /**
     * Attract to or repel from `center`, along the line to it, with a `falloff` over `radius`. The sign
     * of `strength` chooses: positive pushes away (an explosion impulse), negative pulls in (a gravity
     * well). Zero force beyond `radius` for the bounded falloffs.
     * */
    NYA_FORCE_POINT,

    /**
     * Swirl about the axis `direction` through `center`: a tangential push, perpendicular to both the
     * axis and the spoke out to the point, with a `falloff` over `radius`. A whirlwind or a drain. The
     * sign of `strength` chooses the spin's handedness.
     * */
    NYA_FORCE_VORTEX,

    /**
     * A force opposing the sampled velocity, `-strength * velocity`, so a field can damp motion. This is
     * the kind that gives nya_force_at a velocity to read; the others ignore it.
     * */
    NYA_FORCE_DRAG,

    /**
     * Curl noise: the curl of a Perlin vector potential, which is divergence-free by construction, so it
     * stirs and folds without a net inflow or outflow that would pump particles apart. `scale` sets the
     * spatial frequency and `strength` the magnitude; the field drifts slowly with time. The noise is
     * baked from `seed` into the field, so sampling stays a pure function of the field's own bytes.
     * */
    NYA_FORCE_TURBULENCE,

    NYA_FORCE_KIND_COUNT,
};

/** How a point or vortex force fades with distance from its centre or axis. */
enum NYA_ForceFalloff {
    /** No falloff: constant `strength` at every distance, `radius` ignored. The default, and what a plain pull wants. */
    NYA_FORCE_FALLOFF_NONE = 0,

    /** `1 - distance / radius`, clamped to zero at the rim. A soft, finite-reach well or swirl. */
    NYA_FORCE_FALLOFF_LINEAR,

    /**
     * A softened inverse square, `radius^2 / (distance^2 + radius^2)`: one at the centre, a half at the
     * rim, and the `1/distance^2` of real gravity far out, without the singularity at the centre.
     * */
    NYA_FORCE_FALLOFF_INVERSE_SQUARE,

    NYA_FORCE_FALLOFF_COUNT,
};

/**
 * How to build a field. Every field has a usable default, so `(NYA_ForceOptions){ 0 }` is a unit push
 * along +x, and each kind reads only what it needs.
 * */
struct NYA_ForceOptions {
    NYA_ForceKind kind;

    /** UNIFORM: which way it pushes. VORTEX: the swirl axis. World space; zero is +x for uniform, +y for a vortex. */
    f32x3 direction;

    /** POINT and VORTEX: the centre the force is measured from. */
    f32x3 center;

    /**
     * The magnitude. UNIFORM/VORTEX/TURBULENCE: how hard. POINT: how hard, with the sign choosing
     * repel (+) or attract (−). DRAG: the coefficient `k` in `-k * velocity`. Zero is read as one.
     * */
    f32 strength;

    /** POINT and VORTEX: the falloff reach, in world units. Zero is read as one. */
    f32 radius;

    /** POINT and VORTEX: how the force fades over `radius`. */
    NYA_ForceFalloff falloff;

    /** TURBULENCE: spatial frequency per world unit — small stirs over metres, large over centimetres. Zero is the default. */
    f32 scale;

    /** TURBULENCE: which noise field to bake. VORTEX phase is unaffected. Any value; zero is fine. */
    f32 seed;
};

/**
 * A force field. A plain value the caller owns: put it on the stack, in an arena, or in a component.
 * Sampling is a pure function of its fields — the turbulence noise table included — so a copy samples
 * identically, which is what the determinism test rests on. See nya_force_at.
 * */
struct NYA_ForceField {
    NYA_ForceKind kind;

    /** UNIFORM push direction / VORTEX swirl axis, normalized on the way in. */
    f32x3 direction;
    f32x3 center;

    f32              strength;
    f32              radius;
    NYA_ForceFalloff falloff;
    f32              scale;
    f32              seed;

    /** TURBULENCE only: the Perlin permutation, baked from `seed` at build time. Copied with the field, so pure. */
    NYA_Noise noise;

    /** Advanced by nya_force_advance, and what nya_force_sample reads. Sample an explicit time with nya_force_at. */
    f32 time;
};

/**
 * A set of fields that sum. The composition seam: a consumer holds one of these and reads the total
 * force at a point, and the caller builds it in place — `set.fields[set.count++] = nya_force_field(...)`
 * — so there is no allocation and no bespoke builder. A single field is a set of one.
 * */
struct NYA_ForceSet {
    NYA_ForceField fields[NYA_FORCE_SET_MAX];
    u32            count;

    /** The set's own clock, advanced by nya_forces_advance, that nya_forces_sample reads. */
    f32 time;
};

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * FUNCTIONS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/** Builds a field, applying the defaults in NYA_ForceOptions. Bakes the turbulence noise from `seed`. */
NYA_API NYA_ForceField nya_force_field(NYA_ForceOptions options) __attr_no_discard;

/** Advances the field's own clock. Call once a frame; nya_force_sample reads what it accumulates. */
NYA_API void nya_force_advance(NYA_ForceField* field, f32 delta_time_s);

/**
 * The force at a world position, given the sampled point's velocity and an explicit time, as an
 * acceleration vector the caller scales by its own timestep. Only DRAG and TURBULENCE read `velocity`
 * and `time` respectively; the rest are pure functions of position. A pure function overall, so the
 * same arguments always give the same vector.
 * */
NYA_API f32x3 nya_force_at(const NYA_ForceField* field, f32x3 position, f32x3 velocity, f32 time) __attr_no_discard;

/** nya_force_at at the field's own accumulated time. The one a frame uses for a lone field. */
NYA_API f32x3 nya_force_sample(const NYA_ForceField* field, f32x3 position, f32x3 velocity) __attr_no_discard;

/** Advances a set's shared clock, which every field in it then samples against. Call once a frame. */
NYA_API void nya_forces_advance(NYA_ForceSet* set, f32 delta_time_s);

/** The sum of every field's nya_force_at at an explicit time. The composition. */
NYA_API f32x3 nya_forces_at(const NYA_ForceSet* set, f32x3 position, f32x3 velocity, f32 time) __attr_no_discard;

/** nya_forces_at at the set's own accumulated time. The one a frame uses. */
NYA_API f32x3 nya_forces_sample(const NYA_ForceSet* set, f32x3 position, f32x3 velocity) __attr_no_discard;
