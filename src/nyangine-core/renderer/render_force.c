#include "nyangine-core/nyangine.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API DECLARATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/** The falloff weight in [0, 1] for a point or vortex at `distance` from its centre or axis. */
NYA_INTERNAL f32 _nya_force_falloff(NYA_ForceFalloff falloff, f32 distance, f32 radius) __attr_no_discard;

/**
 * The three-component Perlin vector potential the turbulence force is the curl of. The components read
 * the same table at widely separated, irrational-looking offsets so they decorrelate rather than move
 * in lockstep, which is what keeps the curl from collapsing onto one axis.
 * */
NYA_INTERNAL f32x3 _nya_force_potential(const NYA_Noise* noise, f32x3 q) __attr_no_discard;

/** Below this a spoke has no defined direction, so a point or vortex sample at its own centre is zero rather than NaN. */
#define _NYA_FORCE_EPSILON 1.0e-6F

/** The half-step the turbulence curl is taken over, in noise-input units. Small enough to read the local gradient. */
#define _NYA_FORCE_CURL_STEP 1.0e-3F

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PUBLIC API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

NYA_ForceField nya_force_field(NYA_ForceOptions options) {
    f32x3 direction = options.direction;

    // zero means unset: a push along +x, but a vortex swings about the vertical, so its axis defaults to +y.
    if (direction.x == 0.0F && direction.y == 0.0F && direction.z == 0.0F) {
        direction = options.kind == NYA_FORCE_VORTEX ? (f32x3){ 0.0F, 1.0F, 0.0F } : (f32x3){ 1.0F, 0.0F, 0.0F };
    }

    // The turbulence table is baked once from the seed, so a copy of the field samples identically; the seed's bits become the RNG's hex string.
    NYA_Noise noise;
    {
        u32 bits;
        nya_memcpy(&bits, &options.seed, sizeof(bits));

        char seed_text[9];
        (void)snprintf(seed_text, sizeof(seed_text), "%08X", bits);

        NYA_RNG rng = nya_rng_create(.seed = seed_text);
        noise       = nya_noise_create(&rng);
    }

    return (NYA_ForceField){
        .kind      = options.kind,
        .direction = nya_vector_normalize(direction),
        .center    = options.center,
        .strength  = options.strength != 0.0F ? options.strength : 1.0F,
        .radius    = options.radius > 0.0F ? options.radius : 1.0F,
        .falloff   = options.falloff,
        .scale     = options.scale > 0.0F ? options.scale : NYA_FORCE_TURBULENCE_SCALE,
        .seed      = options.seed,
        .noise     = noise,
        .time      = 0.0F,
    };
}

void nya_force_advance(NYA_ForceField* field, f32 delta_time_s) {
    nya_assert(field != nullptr);

    if (delta_time_s <= 0.0F) return;

    field->time += delta_time_s;
}

f32x3 nya_force_at(const NYA_ForceField* field, f32x3 position, f32x3 velocity, f32 time) {
    nya_assert(field != nullptr);

    switch (field->kind) {
        case NYA_FORCE_UNIFORM: {
            return field->direction * field->strength;
        }

        case NYA_FORCE_POINT: {
            // The spoke out from the centre; positive strength repels, negative attracts.
            f32x3 spoke    = position - field->center;
            f32   distance = nya_vector_length(spoke);

            if (distance <= _NYA_FORCE_EPSILON) return f32x3_zero;

            f32x3 outward   = spoke * (1.0F / distance);
            f32   magnitude = field->strength * _nya_force_falloff(field->falloff, distance, field->radius);

            return outward * magnitude;
        }

        case NYA_FORCE_VORTEX: {
            // The swirl is tangential to the axis and the radial spoke, so it pushes around the axis; radial distance drives the falloff.
            f32x3 axis   = field->direction;
            f32x3 spoke  = position - field->center;
            f32   along  = nya_vector_dot(spoke, axis);
            f32x3 radial = spoke - (axis * along);
            f32   distance = nya_vector_length(radial);

            if (distance <= _NYA_FORCE_EPSILON) return f32x3_zero;

            f32x3 tangent   = nya_vector_cross(axis, radial * (1.0F / distance));
            f32   magnitude = field->strength * _nya_force_falloff(field->falloff, distance, field->radius);

            return tangent * magnitude;
        }

        case NYA_FORCE_DRAG: {
            // opposes whatever the sampled point is doing, which is why this primitive takes a velocity at all.
            return velocity * (-field->strength);
        }

        case NYA_FORCE_TURBULENCE: {
            // Curl noise: the curl of a vector potential is divergence-free, so the field stirs without a source or sink.
            f32x3 q = position * field->scale;

            // A slow drift so the stir animates; low non-repeating speeds so the axes do not beat, and pure in time.
            q += (f32x3){ time * 0.11F, time * 0.07F, time * 0.13F };

            f32 h   = _NYA_FORCE_CURL_STEP;
            f32 inv = 1.0F / (2.0F * h);

            f32x3 dp_dx = (_nya_force_potential(&field->noise, q + (f32x3){ h, 0.0F, 0.0F })
                           - _nya_force_potential(&field->noise, q - (f32x3){ h, 0.0F, 0.0F })) * inv;
            f32x3 dp_dy = (_nya_force_potential(&field->noise, q + (f32x3){ 0.0F, h, 0.0F })
                           - _nya_force_potential(&field->noise, q - (f32x3){ 0.0F, h, 0.0F })) * inv;
            f32x3 dp_dz = (_nya_force_potential(&field->noise, q + (f32x3){ 0.0F, 0.0F, h })
                           - _nya_force_potential(&field->noise, q - (f32x3){ 0.0F, 0.0F, h })) * inv;

            f32x3 curl = {
                dp_dy.z - dp_dz.y,
                dp_dz.x - dp_dx.z,
                dp_dx.y - dp_dy.x,
            };

            return curl * field->strength;
        }

        case NYA_FORCE_KIND_COUNT:
        default: {
            nya_assert(false, "unknown force kind " FMTu32, (u32)field->kind);
            return f32x3_zero;
        }
    }
}

f32x3 nya_force_sample(const NYA_ForceField* field, f32x3 position, f32x3 velocity) {
    nya_assert(field != nullptr);

    return nya_force_at(field, position, velocity, field->time);
}

void nya_forces_advance(NYA_ForceSet* set, f32 delta_time_s) {
    nya_assert(set != nullptr);

    if (delta_time_s <= 0.0F) return;

    set->time += delta_time_s;
}

f32x3 nya_forces_at(const NYA_ForceSet* set, f32x3 position, f32x3 velocity, f32 time) {
    nya_assert(set != nullptr);
    nya_assert(set->count <= NYA_FORCE_SET_MAX, "a force set holds at most NYA_FORCE_SET_MAX fields, has " FMTu32, set->count);

    f32x3 total = f32x3_zero;

    for (u32 i = 0; i < set->count; i++) total += nya_force_at(&set->fields[i], position, velocity, time);

    return total;
}

f32x3 nya_forces_sample(const NYA_ForceSet* set, f32x3 position, f32x3 velocity) {
    nya_assert(set != nullptr);

    return nya_forces_at(set, position, velocity, set->time);
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

f32 _nya_force_falloff(NYA_ForceFalloff falloff, f32 distance, f32 radius) {
    switch (falloff) {
        case NYA_FORCE_FALLOFF_LINEAR: {
            if (distance >= radius) return 0.0F;
            return 1.0F - (distance / radius);
        }

        case NYA_FORCE_FALLOFF_INVERSE_SQUARE: {
            // softened: one at the centre, a half at the rim, and 1/distance^2 far out, with no singularity.
            f32 radius_squared = radius * radius;
            return radius_squared / ((distance * distance) + radius_squared);
        }

        case NYA_FORCE_FALLOFF_NONE:
        case NYA_FORCE_FALLOFF_COUNT:
        default: {
            return 1.0F;
        }
    }
}

f32x3 _nya_force_potential(const NYA_Noise* noise, f32x3 q) {
    // perlin3 only reads the permutation table, so the cast away from const is sound and lets the field stay const.
    NYA_Noise* n = (NYA_Noise*)noise;

    return (f32x3){
        nya_noise_perlin3(n, q.x, q.y, q.z),
        nya_noise_perlin3(n, q.x + 113.7F, q.y + 71.3F, q.z + 19.1F),
        nya_noise_perlin3(n, q.x - 59.2F, q.y - 137.5F, q.z + 83.6F),
    };
}
