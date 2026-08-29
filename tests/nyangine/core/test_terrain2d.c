/**
 * The 2D height field: defaults, the strictly-increasing-x guarantee, and clamped sampling.
 *
 * The one property worth pinning hardest is that x strictly increases. That is what makes this a
 * height field rather than a polyline, and it is what stops a chain body doubling back and trapping
 * something inside the ground.
 **/

#include "nyangine/nyangine.c"
#include "nyangine/nyangine.h"

#include "SDL3/SDL_init.h"

enum { KIND_TERRAIN = 11 };

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

    NYA_Arena* arena = nya_arena_create(.name = "test_terrain2d");
    defer      nya_arena_destroy(arena);

    // ── Defaults fill in.
    {
        NYA_Terrain2D* terrain = nullptr;
        NYA_EXPECT(nya_terrain2d_create(arena, (NYA_Terrain2DOptions){ .entity_type = KIND_TERRAIN }, &terrain));

        nya_check(terrain->options.half_width > 0.0F, "half width should default");
        nya_check(terrain->options.point_step > 0.0F, "point step should default");
        nya_check(terrain->point_count > 2, "the profile should have points, got %u", terrain->point_count);
        nya_check(terrain->points != nullptr, "create should allocate the profile");
        nya_check(!nya_entity_is_valid(terrain->entity), "no body before generation");
    }

    // ── Bad arguments are refused rather than asserted on.
    {
        NYA_Terrain2D* terrain = nullptr;
        nya_check(!nya_terrain2d_create(nullptr, (NYA_Terrain2DOptions){ 0 }, &terrain).ok, "a null arena is an error");
        nya_check(!nya_terrain2d_create(arena, (NYA_Terrain2DOptions){ 0 }, nullptr).ok, "a null out pointer is an error");
    }

    // ── Generation: x strictly increases, every y is finite, and a body is spawned.
    {
        NYA_Terrain2D* terrain = nullptr;
        NYA_EXPECT(nya_terrain2d_create(arena, (NYA_Terrain2DOptions){ .entity_type = KIND_TERRAIN }, &terrain));

        nya_terrain2d_generate(terrain, 4242);

        nya_check(terrain->seed == 4242, "the seed should be recorded");
        nya_check(nya_entity_is_valid(terrain->entity), "generation should spawn the ground body");

        u32 not_increasing = 0;
        u32 not_finite     = 0;
        for (u32 i = 0; i < terrain->point_count; i++) {
            if (!isfinite((f64)terrain->points[i].y)) not_finite++;
            if (i > 0 && !(terrain->points[i].x > terrain->points[i - 1].x)) not_increasing++;
        }
        nya_check(not_increasing == 0, "x must strictly increase; %u samples did not", not_increasing);
        nya_check(not_finite == 0, "%u samples were not finite", not_finite);

        nya_check(terrain->max_height > terrain->min_height, "a generated profile should not be flat");

        // ── Sampling on a sample point returns it, and past the ends clamps rather than extrapolating.
        f32 first = nya_terrain2d_height_at(terrain, terrain->points[0].x);
        nya_check(fabsf(first - terrain->points[0].y) < 0.01F, "the first sample should read itself");

        f32 far_left  = nya_terrain2d_height_at(terrain, -terrain->options.half_width * 10.0F);
        f32 far_right = nya_terrain2d_height_at(terrain, terrain->options.half_width * 10.0F);
        nya_check(fabsf(far_left - terrain->points[0].y) < 0.01F, "far left should clamp to the first sample");
        nya_check(fabsf(far_right - terrain->points[terrain->point_count - 1].y) < 0.01F,
                  "far right should clamp to the last sample");

        // ── A midpoint lies between its neighbours.
        f32 mid_x = (terrain->points[3].x + terrain->points[4].x) * 0.5F;
        f32 mid   = nya_terrain2d_height_at(terrain, mid_x);
        f32 lo    = nya_min(terrain->points[3].y, terrain->points[4].y);
        f32 hi    = nya_max(terrain->points[3].y, terrain->points[4].y);
        nya_check(mid >= lo - 0.01F && mid <= hi + 0.01F, "a midpoint should lie between its neighbours");

        nya_terrain2d_release(terrain);
        nya_check(!nya_entity_is_valid(terrain->entity), "release should despawn the body");
    }

    // ── Determinism, and regeneration in place.
    {
        NYA_Terrain2D* a = nullptr;
        NYA_Terrain2D* b = nullptr;
        NYA_EXPECT(nya_terrain2d_create(arena, (NYA_Terrain2DOptions){ .entity_type = KIND_TERRAIN }, &a));
        NYA_EXPECT(nya_terrain2d_create(arena, (NYA_Terrain2DOptions){ .entity_type = KIND_TERRAIN }, &b));

        nya_terrain2d_generate(a, 55);
        nya_terrain2d_generate(b, 55);

        u32 same = 0;
        for (u32 i = 0; i < a->point_count; i++) {
            if (a->points[i].y == b->points[i].y) same++;
        }
        nya_check(same == a->point_count, "the same seed should reproduce the profile exactly (%u/%u)", same, a->point_count);

        f32x2* before = a->points;
        nya_terrain2d_generate(a, 56);
        nya_check(a->points == before, "regeneration must rewrite in place, not reallocate");

        nya_terrain2d_release(a);
        nya_terrain2d_release(b);
    }

    // ── A null terrain is tolerated everywhere a caller might have one before creation.
    {
        nya_check(nya_terrain2d_height_at(nullptr, 0.0F) == 0.0F, "sampling a null terrain is zero");
        nya_terrain2d_release(nullptr);
        nya_terrain2d_draw(nullptr, nullptr);
    }

    return nya_check_failures() == 0 ? 0 : 1;
}
