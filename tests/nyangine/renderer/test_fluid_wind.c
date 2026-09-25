/**
 * A fluid volume drifting on a wind field: with a field set, each step pushes the velocity grid toward the wind,
 * so a sample in the interior gains velocity along it; with none, the field stays still. The wind is steady
 * (gustiness zero) so the push is a known constant and the check is arithmetic.
 **/

#include "nyangine-core/nyangine.h"

#include "nyangine-core/nyangine.c"

int main(void) {
    setvbuf(stdout, nullptr, _IONBF, 0);

    NYA_Arena* arena = nya_arena_create(.name = "test_fluid_wind");
    defer      nya_arena_destroy(arena);

    // a still 3D volume: no gravity, no viscosity, nothing emitted, so the only thing that can move it is the wind.
    NYA_Fluid* fluid = nya_fluid_create(arena, (NYA_FluidOptions){
                                                   .space     = NYA_FLUID_SPACE_3D,
                                                   .width     = 16,
                                                   .height    = 16,
                                                   .depth     = 16,
                                                   .cell_size = 1.0F,
                                                   .origin    = { 0.0F, 0.0F, 0.0F },
                                                   .gravity   = { 0.0F, 0.0F, 0.0F },
                                               });

    f32x3 centre = { 8.0F, 8.0F, 8.0F };

    // a steady wind blowing +x at 5, no gust: nya_wind_at is a constant { 5, 0, 0 }.
    NYA_WindField wind = nya_wind_field((NYA_WindOptions){ .direction = { 1.0F, 0.0F, 0.0F }, .strength = 5.0F, .gustiness = 0.0F });

    // TEST: no field — the still volume stays still.
    {
        for (u32 i = 0; i < 10; i++) nya_fluid_step(fluid, 0.05F);
        f32x3 v = nya_fluid_velocity_at(fluid, centre);
        nya_check(fabsf(v.x) < 1e-3F && fabsf(v.y) < 1e-3F && fabsf(v.z) < 1e-3F, "with no wind the volume is still, got (%f,%f,%f)", (f64)v.x, (f64)v.y, (f64)v.z);
    }

    // TEST: with the field, the velocity gains speed along the wind.
    {
        nya_fluid_wind_set(fluid, &wind, 2.0F);
        for (u32 i = 0; i < 20; i++) nya_fluid_step(fluid, 0.05F);

        f32x3 v = nya_fluid_velocity_at(fluid, centre);
        nya_check(v.x > 0.5F, "the wind pushes the volume along +x, got x=%f", (f64)v.x);
        nya_check(fabsf(v.y) < 0.5F && fabsf(v.z) < 0.5F, "and mostly only along the wind, got (y=%f, z=%f)", (f64)v.y, (f64)v.z);
    }

    // TEST: clearing the field (null) stops adding wind — the velocity no longer grows.
    {
        nya_fluid_wind_set(fluid, nullptr, 2.0F);
        f32 before = nya_fluid_velocity_at(fluid, centre).x;
        for (u32 i = 0; i < 20; i++) nya_fluid_step(fluid, 0.05F);
        f32 after = nya_fluid_velocity_at(fluid, centre).x;

        // with no forcing and no viscosity the advected field only diffuses/decays, never grows, so it does not climb past where it was when the wind was removed.
        nya_check(after <= before + 1e-3F, "a null field adds no more wind, before=%f after=%f", (f64)before, (f64)after);
    }

    printf("test_fluid_wind: all passed\n");
    return 0;
}
