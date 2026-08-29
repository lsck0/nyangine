/**
 * The heightmap terrain: defaults, sampling, and the guarantees nya_terrain3d_height_at makes.
 *
 * Headless, so no mesh is uploaded and no body is drawn — but the sampling, the rim shaping and the
 * bilinear lookup are all plain arithmetic over the grid and are exactly what is worth pinning.
 **/

#include "nyangine/nyangine.c"
#include "nyangine/nyangine.h"

#include "SDL3/SDL_init.h"

enum { KIND_TERRAIN = 9 };

s32 main(void) {
    SDL_SetHintWithPriority(SDL_HINT_AUDIO_DRIVER, "dummy", SDL_HINT_OVERRIDE);

    _NYA_APP_INSTANCE = (NYA_App){ .initialized = true };
    b8 sdl_ok         = SDL_Init(0);
    nya_assert(sdl_ok, "SDL_Init failed: %s", SDL_GetError());

    nya_system_callback_init();
    NYA_World* world = nya_world_create();
    (void)nya_world_set(world);
    defer nya_world_destroy(world);
    defer nya_system_callback_deinit();

    NYA_Arena* arena = nya_arena_create(.name = "test_terrain3d");
    defer      nya_arena_destroy(arena);

    NYA_Window window = { .screen_width = 320, .screen_height = 200 };

    // ── Zero options fill in, so a caller names only what it cares about.
    {
        NYA_Terrain3D* terrain = nullptr;
        NYA_EXPECT(nya_terrain3d_create(arena, (NYA_Terrain3DOptions){ .entity_type = KIND_TERRAIN }, &terrain));

        nya_check(terrain->resolution == 32, "default resolution should be 32, got %u", terrain->resolution);
        nya_check(terrain->verts == 33, "the sample grid is one larger per side, got %u", terrain->verts);
        nya_check(terrain->options.extent > 0.0F, "extent should default to something usable");
        nya_check(fabsf(terrain->cell - (terrain->options.extent / 32.0F)) < 0.0001F, "cell should be extent/resolution");
        nya_check(terrain->heights != nullptr, "create should allocate the sample grid");
        nya_check(terrain->options.octaves == 4, "default octaves");
    }

    // ── A null arena or out pointer is refused rather than asserted on.
    {
        NYA_Terrain3D* terrain = nullptr;
        nya_check(!nya_terrain3d_create(nullptr, (NYA_Terrain3DOptions){ 0 }, &terrain).ok, "a null arena is an error");
        nya_check(!nya_terrain3d_create(arena, (NYA_Terrain3DOptions){ 0 }, nullptr).ok, "a null out pointer is an error");
    }

    // ── Before generation, sampling is defined and returns zero rather than reading a stale grid.
    {
        NYA_Terrain3D* terrain = nullptr;
        NYA_EXPECT(nya_terrain3d_create(arena, (NYA_Terrain3DOptions){ .entity_type = KIND_TERRAIN }, &terrain));
        nya_check(nya_terrain3d_height_at(terrain, 0.0F, 0.0F) == 0.0F, "an ungenerated terrain samples as zero");
    }

    // ── Generating fills the grid, records the seed, and produces a real height range.
    {
        NYA_Terrain3D* terrain = nullptr;
        NYA_EXPECT(nya_terrain3d_create(arena, (NYA_Terrain3DOptions){ .entity_type = KIND_TERRAIN }, &terrain));

        nya_terrain3d_generate(terrain, &window, 12345);

        nya_check(terrain->seed == 12345, "the seed should be recorded, got %llu", (unsigned long long)terrain->seed);
        nya_check(terrain->max_height > terrain->min_height, "a generated surface should not be flat: %f..%f",
                  (f64)terrain->min_height, (f64)terrain->max_height);

        // Every sample must be finite. A NaN here propagates into the collider and into every drop point.
        u32 samples = terrain->verts * terrain->verts;
        u32 bad     = 0;
        for (u32 i = 0; i < samples; i++) {
            if (!isfinite((f64)terrain->heights[i])) bad++;
        }
        nya_check(bad == 0, "%u of %u samples were not finite", bad, samples);

        // ── Sampling is clamped to the terrain, not extrapolated: far outside must still be in range.
        f32 half = terrain->options.extent * 0.5F;
        f32 far_out = nya_terrain3d_height_at(terrain, half * 100.0F, half * 100.0F);
        nya_check(far_out >= terrain->min_height - 0.001F && far_out <= terrain->max_height + 0.001F,
                  "a sample far outside should clamp into the height range, got %f", (f64)far_out);

        // ── A sample exactly on a grid point should equal that sample.
        f32 corner = nya_terrain3d_height_at(terrain, -half, -half);
        nya_check(fabsf(corner - terrain->heights[0]) < 0.01F, "the corner should read its own sample: %f vs %f",
                  (f64)corner, (f64)terrain->heights[0]);

        // ── The rim stands above the middle. That is the whole point of the radial shaping.
        f32 middle = nya_terrain3d_height_at(terrain, 0.0F, 0.0F);
        f32 edge   = nya_terrain3d_height_at(terrain, half * 0.99F, 0.0F);
        nya_check(edge > middle, "the rim should stand above the centre: edge %f, middle %f", (f64)edge, (f64)middle);
    }

    // ── The same seed produces the same surface; a different one does not.
    {
        NYA_Terrain3D* a = nullptr;
        NYA_Terrain3D* b = nullptr;
        NYA_Terrain3D* c = nullptr;
        NYA_EXPECT(nya_terrain3d_create(arena, (NYA_Terrain3DOptions){ .entity_type = KIND_TERRAIN }, &a));
        NYA_EXPECT(nya_terrain3d_create(arena, (NYA_Terrain3DOptions){ .entity_type = KIND_TERRAIN }, &b));
        NYA_EXPECT(nya_terrain3d_create(arena, (NYA_Terrain3DOptions){ .entity_type = KIND_TERRAIN }, &c));

        nya_terrain3d_generate(a, &window, 777);
        nya_terrain3d_generate(b, &window, 777);
        nya_terrain3d_generate(c, &window, 778);

        u32 samples    = a->verts * a->verts;
        u32 same_as_b  = 0;
        u32 same_as_c  = 0;
        for (u32 i = 0; i < samples; i++) {
            if (a->heights[i] == b->heights[i]) same_as_b++;
            if (a->heights[i] == c->heights[i]) same_as_c++;
        }
        nya_check(same_as_b == samples, "the same seed should reproduce the surface exactly (%u/%u)", same_as_b, samples);
        nya_check(same_as_c < samples, "a different seed should produce a different surface");
    }

    // ── Regenerating reuses the grid rather than allocating a second one.
    {
        NYA_Terrain3D* terrain = nullptr;
        NYA_EXPECT(nya_terrain3d_create(arena, (NYA_Terrain3DOptions){ .entity_type = KIND_TERRAIN }, &terrain));

        nya_terrain3d_generate(terrain, &window, 1);
        f32* first = terrain->heights;

        nya_terrain3d_generate(terrain, &window, 2);
        nya_check(terrain->heights == first, "regeneration must rewrite the grid in place, not reallocate");
        nya_check(terrain->seed == 2, "and take the new seed");
    }

    // ── A custom resolution is honoured all the way through.
    {
        NYA_Terrain3D* terrain = nullptr;
        NYA_EXPECT(nya_terrain3d_create(arena,
                                        (NYA_Terrain3DOptions){ .resolution = 8, .extent = 40.0F, .entity_type = KIND_TERRAIN },
                                        &terrain));
        nya_check(terrain->resolution == 8 && terrain->verts == 9, "resolution should be honoured");
        nya_check(fabsf(terrain->cell - 5.0F) < 0.0001F, "cell should be 40/8, got %f", (f64)terrain->cell);

        nya_terrain3d_generate(terrain, &window, 3);
        nya_check(terrain->max_height > terrain->min_height, "a coarse terrain is still a terrain");
    }

    return nya_check_failures() == 0 ? 0 : 1;
}
