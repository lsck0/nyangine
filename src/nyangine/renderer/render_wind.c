#include "nyangine/nyangine.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API DECLARATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/**
 * The gust envelope at a point and time, in [-1, 1]: layered sines whose amplitudes sum to one. `phase`
 * shifts every layer together, so the swirl can read a second, out-of-step copy of the same shape.
 * */
NYA_INTERNAL f32 _nya_wind_gust(f32x3 position, f32 time, f32 seed, f32 phase);

/*
 * The gust's spatial and temporal shape, three layers. The spatial vectors are deliberately not axis
 * aligned, so a row of plants along one axis does not gust in lockstep; the speeds are low and
 * irrational-looking, so the sum does not visibly repeat. The amplitudes sum to one, which is what
 * keeps the gust in [-1, 1] and lets the tests bound it.
 */

/** Per world unit. Small, so the field varies over metres rather than centimetres. */
NYA_INTERNAL const f32x3 _NYA_WIND_FREQUENCY[NYA_WIND_OCTAVES] = {
    { 0.13F, 0.05F, 0.21F },
    { 0.37F, 0.11F, 0.29F },
    { 0.71F, 0.19F, 0.53F },
};

/** Radians per second. */
NYA_INTERNAL const f32 _NYA_WIND_SPEED[NYA_WIND_OCTAVES] = { 1.10F, 2.30F, 3.70F };

/** Sum to one. */
NYA_INTERNAL const f32 _NYA_WIND_AMPLITUDE[NYA_WIND_OCTAVES] = { 0.50F, 0.30F, 0.20F };

/** How much of the push the swirl crosses it by, relative to the gust. Gentler than the along-wind swing. */
#define _NYA_WIND_SWIRL 0.5F

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PUBLIC API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

NYA_WindField nya_wind_field(NYA_WindOptions options) {
    f32x3 direction = options.direction;

    // zero means unset: a light breeze along +x, the same rule NYA_ParticleBurst.direction uses.
    if (direction.x == 0.0F && direction.y == 0.0F && direction.z == 0.0F) direction = (f32x3){ 1.0F, 0.0F, 0.0F };

    return (NYA_WindField){
        .direction = nya_vector_normalize(direction),
        .strength  = options.strength > 0.0F ? options.strength : 1.0F,
        .gustiness = nya_clamp(options.gustiness, 0.0F, 1.0F),
        .seed      = options.seed,
        .time      = 0.0F,
    };
}

void nya_wind_set(NYA_WindField* field, f32x3 direction, f32 strength, f32 gustiness) {
    nya_assert(field != nullptr);

    if (direction.x == 0.0F && direction.y == 0.0F && direction.z == 0.0F) direction = (f32x3){ 1.0F, 0.0F, 0.0F };

    field->direction = nya_vector_normalize(direction);
    field->strength  = strength > 0.0F ? strength : 0.0F;
    field->gustiness = nya_clamp(gustiness, 0.0F, 1.0F);
}

void nya_wind_advance(NYA_WindField* field, f32 delta_time_s) {
    nya_assert(field != nullptr);

    if (delta_time_s <= 0.0F) return;

    field->time += delta_time_s;
}

f32x3 nya_wind_at(const NYA_WindField* field, f32x3 position, f32 time) {
    nya_assert(field != nullptr);

    f32x3 direction = field->direction;

    // the steady push everything leans along.
    f32x3 base = direction * field->strength;

    if (field->gustiness <= 0.0F) return base;

    // the along-wind swing, and a second, out-of-step gust for the cross sway.
    f32 along = _nya_wind_gust(position, time, field->seed, 0.0F);
    f32 cross = _nya_wind_gust(position, time, field->seed, 1.5707963F);

    // a horizontal perpendicular to the wind, so the swirl is a sideways sway rather than a vertical one.
    // world up, unless the wind blows straight up, in which case +x is a safe reference. see nya_render3d_line.
    f32x3 up   = fabsf(direction.y) < 0.99F ? (f32x3){ 0.0F, 1.0F, 0.0F } : (f32x3){ 1.0F, 0.0F, 0.0F };
    f32x3 perp = nya_vector_normalize(nya_vector_cross(direction, up));

    f32 swing = field->strength * field->gustiness;

    return base + (direction * (swing * along)) + (perp * (swing * _NYA_WIND_SWIRL * cross));
}

f32x3 nya_wind_sample(const NYA_WindField* field, f32x3 position) {
    nya_assert(field != nullptr);

    return nya_wind_at(field, position, field->time);
}

f32x3 nya_wind_base(const NYA_WindField* field) {
    nya_assert(field != nullptr);

    return field->direction * field->strength;
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

f32 _nya_wind_gust(f32x3 position, f32 time, f32 seed, f32 phase) {
    f32 sum = 0.0F;

    for (u32 octave = 0; octave < NYA_WIND_OCTAVES; octave++) {
        f32 spatial = nya_vector_dot(position, _NYA_WIND_FREQUENCY[octave]);
        f32 angle   = spatial + (time * _NYA_WIND_SPEED[octave]) + seed + phase;

        sum += _NYA_WIND_AMPLITUDE[octave] * sinf(angle);
    }

    // the amplitudes sum to one, so the sum is already in [-1, 1]; no normalization needed.
    return sum;
}
