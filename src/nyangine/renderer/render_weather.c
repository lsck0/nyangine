#include "nyangine/nyangine.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * CONSTANTS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 *
 * The two looks side by side: rain fast, near-vertical and short-lived; snow slow, wind-drifted and lingering. Emission is a per-second max the intensity scales.
 */

#define WEATHER_BOX_DEFAULT     ((f32x3){ 40.0F, 20.0F, 40.0F })
#define WEATHER_CEILING_DEFAULT 15.0F

/** Rain: a slight +x lean off vertical, hard fast drops, a short life, only lightly caught by the wind. */
#define WEATHER_RAIN_DIRECTION      ((f32x3){ 0.08F, -1.0F, 0.0F })
#define WEATHER_RAIN_SPEED          ((f32x2){ 28.0F, 40.0F })
#define WEATHER_RAIN_LIFETIME       ((f32x2){ 0.8F, 1.4F })
#define WEATHER_RAIN_SIZE           ((f32x2){ 0.05F, 0.09F })
#define WEATHER_RAIN_SIZE_END       ((f32x2){ 0.04F, 0.07F })
#define WEATHER_RAIN_COLOR_START    ((NYA_Color){ 0.62F, 0.70F, 0.85F, 0.55F })
#define WEATHER_RAIN_COLOR_END      ((NYA_Color){ 0.62F, 0.70F, 0.85F, 0.0F })
#define WEATHER_RAIN_GRAVITY        ((f32x3){ 0.0F, -35.0F, 0.0F })
#define WEATHER_RAIN_WIND_INFLUENCE 0.4F
#define WEATHER_RAIN_EMIT_MAX       2500.0F

/** Snow: straight down but slow, big soft flakes, a long life, and a strong pull toward the wind. */
#define WEATHER_SNOW_DIRECTION      ((f32x3){ 0.0F, -1.0F, 0.0F })
#define WEATHER_SNOW_SPEED          ((f32x2){ 1.5F, 3.5F })
#define WEATHER_SNOW_LIFETIME       ((f32x2){ 5.0F, 10.0F })
#define WEATHER_SNOW_SIZE           ((f32x2){ 0.06F, 0.13F })
#define WEATHER_SNOW_SIZE_END       ((f32x2){ 0.05F, 0.11F })
#define WEATHER_SNOW_COLOR_START    ((NYA_Color){ 1.0F, 1.0F, 1.0F, 0.9F })
#define WEATHER_SNOW_COLOR_END      ((NYA_Color){ 1.0F, 1.0F, 1.0F, 0.0F })
#define WEATHER_SNOW_GRAVITY        ((f32x3){ 0.0F, -2.5F, 0.0F })
#define WEATHER_SNOW_WIND_INFLUENCE 2.5F
#define WEATHER_SNOW_EMIT_MAX       900.0F

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API DECLARATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/** Re-binds the pool's wind to the current field at the current mode's influence. */
NYA_INTERNAL void _nya_weather_apply_wind(NYA_Weather* weather);

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PURE MATH
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

NYA_WeatherParams nya_weather_params(NYA_WeatherMode mode, f32 intensity) {
    // clamped, so a caller passing a raw slider or a stray value still gets a sane rate.
    intensity = nya_clamp(intensity, 0.0F, 1.0F);

    switch (mode) {
        case NYA_WEATHER_RAIN: {
            return (NYA_WeatherParams){
                .direction      = WEATHER_RAIN_DIRECTION,
                .speed          = WEATHER_RAIN_SPEED,
                .lifetime_s     = WEATHER_RAIN_LIFETIME,
                .size           = WEATHER_RAIN_SIZE,
                .size_end       = WEATHER_RAIN_SIZE_END,
                .color_start    = WEATHER_RAIN_COLOR_START,
                .color_end      = WEATHER_RAIN_COLOR_END,
                .gravity        = WEATHER_RAIN_GRAVITY,
                .wind_influence = WEATHER_RAIN_WIND_INFLUENCE,
                .emit_per_second = WEATHER_RAIN_EMIT_MAX * intensity,
            };
        }

        case NYA_WEATHER_SNOW: {
            return (NYA_WeatherParams){
                .direction      = WEATHER_SNOW_DIRECTION,
                .speed          = WEATHER_SNOW_SPEED,
                .lifetime_s     = WEATHER_SNOW_LIFETIME,
                .size           = WEATHER_SNOW_SIZE,
                .size_end       = WEATHER_SNOW_SIZE_END,
                .color_start    = WEATHER_SNOW_COLOR_START,
                .color_end      = WEATHER_SNOW_COLOR_END,
                .gravity        = WEATHER_SNOW_GRAVITY,
                .wind_influence = WEATHER_SNOW_WIND_INFLUENCE,
                .emit_per_second = WEATHER_SNOW_EMIT_MAX * intensity,
            };
        }

        case NYA_WEATHER_CLEAR:
        case NYA_WEATHER_COUNT:
        default: {
            // nothing falls: a zero rate emits nothing, and the rest is left zeroed so a stray read is harmless.
            return (NYA_WeatherParams){ 0 };
        }
    }
}

f32x3 nya_weather_emit_center(f32x3 target, f32x3 box, f32 ceiling) {
    nya_unused(box);

    // centred on the target's ground position, lifted a ceiling overhead so drops start above and fall past.
    return (f32x3){ target.x, target.y + ceiling, target.z };
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PUBLIC API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

NYA_Weather* nya_weather_create(NYA_Arena* arena, u32 capacity) {
    nya_assert(arena != nullptr);
    nya_assert(capacity > 0, "a weather system needs room for at least one drop");

    NYA_Weather* weather = nya_arena_alloc(arena, sizeof(NYA_Weather));

    *weather = (NYA_Weather){
        .particles = nya_particles_create(arena, capacity),
        .mode      = NYA_WEATHER_CLEAR,
        .intensity = 0.0F,
        .box       = WEATHER_BOX_DEFAULT,
        .ceiling   = WEATHER_CEILING_DEFAULT,
    };

    // 3D billboards, drawn through render3d exactly as the showcase's pollen is.
    nya_particles_space_set(weather->particles, NYA_PARTICLE_SPACE_3D);

    return weather;
}

void nya_weather_wind_set(NYA_Weather* weather, const NYA_WindField* field) {
    nya_assert(weather != nullptr);

    weather->wind = field;
    _nya_weather_apply_wind(weather);
}

void nya_weather_set(NYA_Weather* weather, NYA_WeatherMode mode, f32 intensity) {
    nya_assert(weather != nullptr);

    weather->mode      = mode;
    weather->intensity = nya_clamp(intensity, 0.0F, 1.0F);

    // the pull toward the wind is a per-mode thing, so re-apply it whenever the mode changes.
    _nya_weather_apply_wind(weather);
}

void nya_weather_follow(NYA_Weather* weather, f32x3 target) {
    nya_assert(weather != nullptr);

    weather->target = target;
}

void nya_weather_box_set(NYA_Weather* weather, f32x3 box, f32 ceiling) {
    nya_assert(weather != nullptr);

    weather->box     = box;
    weather->ceiling = ceiling;
}

void nya_weather_update(NYA_Weather* weather, f32 delta_time_s) {
    nya_assert(weather != nullptr);

    if (delta_time_s <= 0.0F) return;

    NYA_WeatherParams params = nya_weather_params(weather->mode, weather->intensity);

    // spawn the frame's whole-number share, carrying the fraction on, so the rate is frame-rate independent.
    weather->emit_accumulator += params.emit_per_second * delta_time_s;

    u32 to_emit = (u32)weather->emit_accumulator;
    weather->emit_accumulator -= (f32)to_emit;

    if (to_emit > 0) {
        f32x3 center = nya_weather_emit_center(weather->target, weather->box, weather->ceiling);

        // capped by whatever room the pool has, so the count stays bounded however hard it pours.
        (void)nya_particles_emit(weather->particles, (NYA_ParticleBurst){
                                                          .shape       = NYA_PARTICLE_SHAPE_BOX,
                                                          .position    = center,
                                                          .volume      = weather->box,
                                                          .direction   = params.direction,
                                                          .count       = to_emit,
                                                          .speed       = params.speed,
                                                          .lifetime_s  = params.lifetime_s,
                                                          .size        = params.size,
                                                          .size_end    = params.size_end,
                                                          .color_start = params.color_start,
                                                          .color_end   = params.color_end,
                                                          .gravity     = params.gravity,
                                                      });
    }

    // the live drops keep falling and drifting whatever the mode, so a downpour tapers off when it clears.
    nya_particles_update(weather->particles, delta_time_s);
}

void nya_weather_draw(NYA_Window* window, const NYA_Weather* weather) {
    nya_assert(window != nullptr);

    if (weather == nullptr) return;

    nya_particles_draw(window, weather->particles);
}

u32 nya_weather_count(const NYA_Weather* weather) {
    return weather != nullptr ? nya_particles_count(weather->particles) : 0;
}

NYA_WeatherMode nya_weather_mode(const NYA_Weather* weather) {
    return weather != nullptr ? weather->mode : NYA_WEATHER_CLEAR;
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

void _nya_weather_apply_wind(NYA_Weather* weather) {
    NYA_WeatherParams params = nya_weather_params(weather->mode, weather->intensity);

    // a null field is passed straight through: nya_particles_wind_set reads it as "no wind".
    nya_particles_wind_set(weather->particles, weather->wind, params.wind_influence);
}
