/**
 * Weather is a thin skin over the particle system: a mode picks the particle parameters and how hard the
 * wind pulls, and drops are emitted inside a box that follows a target. Both of those are pure functions,
 * so the checks here are arithmetic, not eyeballing — rain is faster and heavier than snow, snow is pulled
 * harder by the wind, the rate scales with intensity, CLEAR emits nothing, and the emission box tracks its
 * target exactly. A final integration pass steps a live system to prove it stays bounded and drains to CLEAR.
 **/

#include "nyangine-core/nyangine.h"

#include "nyangine-core/nyangine.c"

int main(void) {
    setvbuf(stdout, nullptr, _IONBF, 0);

    // TEST: rain vs snow — the two looks differ in the ways the design promises.
    {
        NYA_WeatherParams rain = nya_weather_params(NYA_WEATHER_RAIN, 1.0F);
        NYA_WeatherParams snow = nya_weather_params(NYA_WEATHER_SNOW, 1.0F);

        // rain falls faster than snow drifts.
        nya_check(rain.speed.x > snow.speed.y, "rain is faster than snow, rain=[%f,%f] snow=[%f,%f]",
                  (f64)rain.speed.x, (f64)rain.speed.y, (f64)snow.speed.x, (f64)snow.speed.y);

        // rain is pulled down harder (more negative gravity y).
        nya_check(rain.gravity.y < snow.gravity.y, "rain gravity is stronger than snow, rain=%f snow=%f",
                  (f64)rain.gravity.y, (f64)snow.gravity.y);

        // snow is caught by the wind far more than rain, which only leans.
        nya_check(snow.wind_influence > rain.wind_influence, "snow rides the wind harder, snow=%f rain=%f",
                  (f64)snow.wind_influence, (f64)rain.wind_influence);

        // rain is short-lived, snow lingers.
        nya_check(snow.lifetime_s.x > rain.lifetime_s.y, "snow lives longer than rain, snow.min=%f rain.max=%f",
                  (f64)snow.lifetime_s.x, (f64)rain.lifetime_s.y);

        // rain leans off vertical; snow falls straight.
        nya_check(fabsf(rain.direction.x) > fabsf(snow.direction.x), "rain leans, snow does not, rain.x=%f snow.x=%f",
                  (f64)rain.direction.x, (f64)snow.direction.x);

        // both actually emit at full intensity.
        nya_check(rain.emit_per_second > 0.0F && snow.emit_per_second > 0.0F, "both pour at full intensity");
    }

    // TEST: intensity scales the emission rate, and zero / CLEAR emit nothing.
    {
        f32 half = nya_weather_params(NYA_WEATHER_RAIN, 0.5F).emit_per_second;
        f32 full = nya_weather_params(NYA_WEATHER_RAIN, 1.0F).emit_per_second;
        f32 none = nya_weather_params(NYA_WEATHER_RAIN, 0.0F).emit_per_second;

        nya_check(full > half && half > none, "the rate rises with intensity, none=%f half=%f full=%f",
                  (f64)none, (f64)half, (f64)full);
        nya_check(none == 0.0F, "intensity zero emits nothing, got %f", (f64)none);

        // half is exactly half of full — the scaling is linear.
        nya_check(fabsf((half * 2.0F) - full) < 1e-3F, "half intensity is half the rate, %f vs %f", (f64)half, (f64)full);

        // intensity is clamped, so an over-range slider does not run the rate away past full.
        f32 over = nya_weather_params(NYA_WEATHER_RAIN, 3.0F).emit_per_second;
        nya_check(fabsf(over - full) < 1e-3F, "intensity clamps at one, over=%f full=%f", (f64)over, (f64)full);

        // CLEAR never emits, at any intensity.
        nya_check(nya_weather_params(NYA_WEATHER_CLEAR, 1.0F).emit_per_second == 0.0F, "CLEAR emits nothing");
    }

    // TEST: the emission box follows its target — centred on its xz, a ceiling above it.
    {
        f32x3 box     = { 40.0F, 20.0F, 40.0F };
        f32   ceiling = 15.0F;

        f32x3 a = nya_weather_emit_center((f32x3){ 3.0F, 1.0F, -7.0F }, box, ceiling);
        nya_check(a.x == 3.0F && a.z == -7.0F, "the box centres on the target's xz, got (%f,%f)", (f64)a.x, (f64)a.z);
        nya_check(fabsf(a.y - (1.0F + ceiling)) < 1e-5F, "the box floats a ceiling above the target, got y=%f", (f64)a.y);

        // move the target; the box moves by exactly the same amount.
        f32x3 b     = nya_weather_emit_center((f32x3){ 13.0F, 1.0F, -7.0F }, box, ceiling);
        f32   moved = b.x - a.x;
        nya_check(fabsf(moved - 10.0F) < 1e-5F, "the box tracks the target one-for-one, moved %f", (f64)moved);
    }

    // TEST: a live system stays bounded under a downpour, then drains once it clears.
    {
        NYA_Arena* arena = nya_arena_create(.name = "test_weather");
        defer      nya_arena_destroy(arena);

        const u32   capacity = 512;
        NYA_Weather* weather = nya_weather_create(arena, capacity);

        NYA_WindField wind = nya_wind_field((NYA_WindOptions){ .direction = { 1, 0, 0 }, .strength = 3.0F });
        nya_weather_wind_set(weather, &wind);

        nya_check(nya_weather_mode(weather) == NYA_WEATHER_CLEAR, "a fresh system starts CLEAR");
        nya_check(nya_weather_count(weather) == 0, "and empty");

        // pour rain and step it for a while; the pool must never exceed its capacity.
        nya_weather_set(weather, NYA_WEATHER_RAIN, 1.0F);
        nya_weather_follow(weather, (f32x3){ 0.0F, 0.0F, 0.0F });

        for (u32 i = 0; i < 300; i++) {
            nya_weather_follow(weather, (f32x3){ (f32)i * 0.1F, 0.0F, 0.0F });
            nya_weather_update(weather, 1.0F / 60.0F);
            nya_wind_advance(&wind, 1.0F / 60.0F);
            nya_check(nya_weather_count(weather) <= capacity, "the pool stays bounded, %u > %u at step %u",
                      nya_weather_count(weather), capacity, i);
        }

        nya_check(nya_weather_count(weather) > 0, "rain fills the pool, got %u", nya_weather_count(weather));

        // clear the sky: nothing new spawns, and the live drops age out to empty.
        nya_weather_set(weather, NYA_WEATHER_CLEAR, 0.0F);
        for (u32 i = 0; i < 200; i++) nya_weather_update(weather, 1.0F / 30.0F);

        nya_check(nya_weather_count(weather) == 0, "CLEAR drains the pool, got %u", nya_weather_count(weather));
    }

    printf("test_weather: all passed\n");
    return 0;
}
