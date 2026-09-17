/**
 * The entity table: bringing it up, spawning into it, and the per tick walk over live entities.
 **/

#include "nyangine/nyangine.c"
#include "nyangine/nyangine.h"

#include "SDL3/SDL_init.h"

#define ENTITIES 1024

static NYA_EntityHandle handles[ENTITIES];

s32 main(void) {
    _NYA_APP_INSTANCE = (NYA_App){ .initialized = true };
    b8 sdl_ok         = SDL_Init(0);
    nya_assert(sdl_ok, "SDL_Init failed: %s", SDL_GetError());

    nya_system_callback_init();
    NYA_EXPECT(nya_system_events_init());
    nya_system_tween_init();

    defer nya_system_tween_deinit();
    defer nya_system_events_deinit();
    defer nya_system_callback_deinit();

    nya_bench_begin("entity table");

    // what every world pays before its first spawn.
    nya_bench("world create + destroy", 0, {
        NYA_World* scratch = nya_world_create();
        nya_world_destroy(scratch);
    });

    NYA_World* world = nya_world_create();
    (void)nya_world_set(world);
    defer nya_world_destroy(world);

    nya_bench("spawn + despawn 1024", ENTITIES, {
        for (u32 i = 0; i < ENTITIES; i++) handles[i] = nya_entity_spawn(.type = i % 4, .position = { (f32)i, (f32)(i * 7 % 512), 0.0F });
        for (u32 i = 0; i < ENTITIES; i++) nya_entity_despawn(handles[i]);
        nya_bench_keep(nya_entity_count());
    });

    for (u32 i = 0; i < ENTITIES; i++) {
        handles[i] = nya_entity_spawn(.type = i % 4, .position = { (f32)i, (f32)(i * 7 % 512), 0.0F }, .velocity = { 1.0F, 0.5F, 0.0F });
    }

    nya_bench("get x1024", ENTITIES, {
        f32 sum = 0.0F;
        for (u32 i = 0; i < ENTITIES; i++) sum += nya_entity_get(handles[i])->position.x;
        nya_bench_keep(sum);
    });

    nya_bench("foreach_kind over 1024", ENTITIES, {
        u32 seen = 0;
        nya_entity_foreach_kind (2, entity) seen += entity->type;
        nya_bench_keep(seen);
    });

    // integration, grid rebuild and hierarchy, the tick a game with no callbacks pays.
    nya_bench("update 1024 moving", ENTITIES, {
        nya_system_entity_update(1.0F / 60.0F);
        nya_bench_keep(nya_entity_get(handles[0])->position.x);
    });

    // the copy at the top of every tick, which a draw between ticks starts from.
    nya_bench("transforms capture 1024", ENTITIES, {
        nya_system_entity_transforms_capture();
        nya_bench_keep(nya_entity_get(handles[0])->position_previous.x);
    });

    // halfway between ticks, so the draw takes the interpolating path.
    _NYA_APP_INSTANCE.options.time_step_ns       = 16'000'000;
    _NYA_APP_INSTANCE.frame_stats.time_behind_ns = 8'000'000;

    nya_bench("render transform x1024", ENTITIES, {
        f32 sum = 0.0F;
        for (u32 i = 0; i < ENTITIES; i++) {
            const NYA_Entity* entity = nya_entity_get(handles[i]);
            sum += nya_entity_render_position(entity).x + nya_entity_render_rotation(entity).w;
        }
        nya_bench_keep(sum);
    });

    return nya_bench_end();
}
