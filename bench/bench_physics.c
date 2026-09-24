/**
 * The world step: what a tick pays to advance a scene full of bodies, in both solvers.
 *
 * A pile of boxes resting on a floor, stepped the way the app loop steps it. The bodies are marked
 * never_sleep, so the number is the cost of a scene the solver is actually working rather than one
 * it has quietly put to rest — the worst case a frame budget has to hold, not the settled floor.
 * Headless: no renderer, no window, only the world and its bodies.
 **/

#include "nyangine/nyangine.c"
#include "nyangine/nyangine.h"

#include "SDL3/SDL_init.h"

/** One fixed tick, matching the engine's default time step. */
#define TICK (1.0F / 60.0F)

/** The two pile sizes, small and a busy scene, reported side by side. */
static const u32 counts[] = { 64, 256 };

/**
 * A floor and `count` boxes stacked in a loose grid above it, every box never_sleep so the step
 * keeps solving contacts instead of skipping a sleeping island.
 * */
static void fill_world_2d(u32 count) {
    nya_entity_clear();

    NYA_EntityHandle floor = nya_entity_spawn(.name = "floor", .position = { 0.0F, 420.0F, 0.0F },
                                              .state = NYA_ENTITY_STATE_ACTIVE | NYA_ENTITY_STATE_STATIC);
    nya_assert(nya_physics2d_body_attach(floor, .type = NYA_PHYSICS_BODY_STATIC, .shape = NYA_PHYSICS2D_SHAPE_BOX, .size = { 4000.0F, 40.0F }));

    const u32 per_row = 16;
    for (u32 i = 0; i < count; i++) {
        f32 x = ((f32)(i % per_row) - (f32)per_row / 2.0F) * 34.0F;
        f32 y = 380.0F - (f32)(i / per_row) * 34.0F;

        NYA_EntityHandle box = nya_entity_spawn(.name = "box", .position = { x, y, 0.0F });
        nya_assert(nya_physics2d_body_attach(box, .shape = NYA_PHYSICS2D_SHAPE_BOX, .size = { 32.0F, 32.0F }, .never_sleep = true));
    }

    nya_assert(nya_physics2d_body_count() == count + 1, "the floor and every box are in the world");
}

/** The 3D twin: a static ground box and a loose grid of dynamic boxes above it, none sleeping. */
static void fill_world_3d(u32 count) {
    nya_entity_clear();

    NYA_EntityHandle floor = nya_entity_spawn(.name = "floor", .position = { 0.0F, -0.5F, 0.0F });
    nya_assert(nya_physics3d_body_attach(floor, .type = NYA_PHYSICS_BODY_STATIC, .shape = NYA_PHYSICS3D_SHAPE_BOX, .size = { 100.0F, 1.0F, 100.0F }));

    const u32 per_row = 8;
    for (u32 i = 0; i < count; i++) {
        f32 x = ((f32)(i % per_row) - (f32)per_row / 2.0F) * 1.1F;
        f32 z = ((f32)((i / per_row) % per_row) - (f32)per_row / 2.0F) * 1.1F;
        f32 y = 0.5F + (f32)(i / (per_row * per_row)) * 1.1F;

        NYA_EntityHandle box = nya_entity_spawn(.name = "box", .position = { x, y, z });
        nya_assert(nya_physics3d_body_attach(box, .shape = NYA_PHYSICS3D_SHAPE_BOX, .size = { 1.0F, 1.0F, 1.0F }, .never_sleep = true));
    }

    nya_assert(nya_physics3d_body_count() == count + 1, "the ground and every box are in the world");
}

s32 main(void) {
    _NYA_APP_INSTANCE = (NYA_App){ .initialized = true };
    b8 sdl_ok         = SDL_Init(0);
    nya_assert(sdl_ok, "SDL_Init failed: %s", SDL_GetError());

    nya_system_callback_init();
    NYA_World* world = nya_world_create();
    (void)nya_world_set(world);

    defer nya_world_destroy(world);
    defer nya_system_callback_deinit();

    // 2D: box2d, the solver a top-down or side-on game steps every tick
    for (u32 c = 0; c < nya_carray_length(counts); c++) {
        const u32 count = counts[c];

        fill_world_2d(count);

        // A short settle before timing, so the pile is in contact rather than mid-drop, which is the configuration the solver spends its time on.
        for (u32 i = 0; i < 30; i++) nya_system_physics2d_update(TICK);

        char group[64];
        (void)snprintf(group, sizeof(group), "physics 2D world step, %u bodies", count);
        nya_bench_begin(group);

        nya_bench("step (box2d)", 1, {
            nya_system_physics2d_update(TICK);
            nya_bench_keep(nya_physics2d_body_count());
        });

        if (nya_bench_end() != 0) return 1;
    }

    nya_entity_clear();

    // 3D: box3d (Jolt), the solver a 3D scene steps every tick
    for (u32 c = 0; c < nya_carray_length(counts); c++) {
        const u32 count = counts[c];

        fill_world_3d(count);

        for (u32 i = 0; i < 30; i++) nya_system_physics3d_update(TICK);

        char group[64];
        (void)snprintf(group, sizeof(group), "physics 3D world step, %u bodies", count);
        nya_bench_begin(group);

        nya_bench("step (box3d)", 1, {
            nya_system_physics3d_update(TICK);
            nya_bench_keep(nya_physics3d_body_count());
        });

        if (nya_bench_end() != 0) return 1;
    }

    return 0;
}
