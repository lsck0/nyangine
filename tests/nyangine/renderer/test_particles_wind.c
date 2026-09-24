/**
 * Particles drifting on a wind field: with a field set, a particle eases its velocity toward what the wind
 * pushes at its position; with none, it integrates exactly as before. The field is steady (gustiness zero), so
 * the target it converges on is a known constant and the check is arithmetic, not eyeballing.
 **/

#include "nyangine/nyangine.h"

#include "nyangine/nyangine.c"

/** Sets one particle to a clean, still initial state so the only thing that can move it is the wind. */
static void seed_one(NYA_ParticleSystem* system) {
    nya_particles_clear(system);
    (void)nya_particles_emit(system, (NYA_ParticleBurst){ .count = 1, .lifetime_s = { 1000.0F, 1000.0F } });
    nya_check(nya_particles_count(system) == 1, "one particle emitted, got %u", nya_particles_count(system));

    system->particles[0].position     = f32x3_zero;
    system->particles[0].velocity     = f32x3_zero;
    system->particles[0].acceleration = f32x3_zero;
    system->particles[0].damping      = 0.0F;
}

int main(void) {
    setvbuf(stdout, nullptr, _IONBF, 0);

    NYA_Arena* arena = nya_arena_create(.name = "test_particles_wind");
    defer      nya_arena_destroy(arena);

    NYA_ParticleSystem* system = nya_particles_create(arena, 16);

    // a steady wind blowing +x at 5, no gust, so nya_wind_at is a constant { 5, 0, 0 } everywhere.
    NYA_WindField wind = nya_wind_field((NYA_WindOptions){ .direction = { 1.0F, 0.0F, 0.0F }, .strength = 5.0F, .gustiness = 0.0F });

    // ─────────────────────────────────────────────────────────────────────────────
    // TEST: no field set — a still particle stays still (old behaviour, unchanged).
    // ─────────────────────────────────────────────────────────────────────────────
    {
        seed_one(system);
        for (u32 i = 0; i < 20; i++) nya_particles_update(system, 0.05F);

        f32x3 v = system->particles[0].velocity;
        nya_check(fabsf(v.x) < 1e-4F && fabsf(v.y) < 1e-4F && fabsf(v.z) < 1e-4F, "with no wind the velocity stays zero, got (%f,%f,%f)", (f64)v.x, (f64)v.y, (f64)v.z);
    }

    // ─────────────────────────────────────────────────────────────────────────────
    // TEST: with the field, the velocity eases toward the wind and settles near it.
    // ─────────────────────────────────────────────────────────────────────────────
    {
        seed_one(system);
        nya_particles_wind_set(system, &wind, 3.0F);

        nya_particles_update(system, 0.05F);
        f32 after_one = system->particles[0].velocity.x;
        nya_check(after_one > 0.0F && after_one < 5.0F, "one step moves toward the wind but not past it, got %f", (f64)after_one);

        for (u32 i = 0; i < 200; i++) nya_particles_update(system, 0.05F);
        f32x3 v = system->particles[0].velocity;
        nya_check(v.x > 4.9F && v.x <= 5.0F, "the velocity settles at the wind, got x=%f", (f64)v.x);
        nya_check(fabsf(v.y) < 1e-3F && fabsf(v.z) < 1e-3F, "and only along the wind, got (y=%f, z=%f)", (f64)v.y, (f64)v.z);
    }

    // ─────────────────────────────────────────────────────────────────────────────
    // TEST: influence zero is the same as no wind — the field is set but pulls nothing.
    // ─────────────────────────────────────────────────────────────────────────────
    {
        seed_one(system);
        nya_particles_wind_set(system, &wind, 0.0F);
        for (u32 i = 0; i < 20; i++) nya_particles_update(system, 0.05F);

        nya_check(fabsf(system->particles[0].velocity.x) < 1e-4F, "influence zero drifts nothing, got %f", (f64)system->particles[0].velocity.x);
    }

    // ─────────────────────────────────────────────────────────────────────────────
    // TEST: clearing the field (null) turns the drift back off.
    // ─────────────────────────────────────────────────────────────────────────────
    {
        seed_one(system);
        nya_particles_wind_set(system, &wind, 3.0F);
        nya_particles_update(system, 0.05F);
        f32 moving = system->particles[0].velocity.x;
        nya_check(moving > 0.0F, "moving under the wind first");

        nya_particles_wind_set(system, nullptr, 3.0F);
        f32 before = system->particles[0].velocity.x;
        for (u32 i = 0; i < 20; i++) nya_particles_update(system, 0.05F);
        nya_check(fabsf(system->particles[0].velocity.x - before) < 1e-4F, "a null field leaves the velocity where it was, %f vs %f", (f64)system->particles[0].velocity.x, (f64)before);
    }

    printf("test_particles_wind: all passed\n");
    return 0;
}
