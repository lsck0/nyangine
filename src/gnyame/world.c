/**
 * @file world.c
 *
 * GNY_World and what it owns. See world.h.
 * */
#include "gnyame/gnyame.h"

/** Darkens everything drawn so far, except where an entity carries a light. */
NYA_INTERNAL void _gny_lights_apply(NYA_Window* window);

GNY_World* gny_world(void) {
    return nya_world_user_data();
}

void gny_world_create(NYA_NetLaunchConfig launch) {
    /* From the engine world's arena. */
    NYA_Arena* allocator = nya_world()->allocator;

    GNY_World* world = nya_arena_alloc(allocator, sizeof(GNY_World));

    *world = (GNY_World){
        .allocator           = allocator,
        .launch              = launch,
        .window_main         = NYA_WINDOW_HANDLE_NONE,
        .terrain             = NYA_ENTITY_HANDLE_NONE,
        /* From --seed when given, so a server operator can reproduce a world. */
        .terrain_seed        = launch.world_seed != 0 ? launch.world_seed : 1,

        // allocated once with the world: the pool is fixed and emission never allocates.
        .sparks = nya_particles_create(allocator, GNY_SPARK_POOL),

        // the camera entity is created when the game layer is pushed; until then gny_entity_camera_get returns
        // the identity camera.
        .camera       = NYA_ENTITY_HANDLE_NONE,
        .inset_camera = NYA_ENTITY_HANDLE_NONE,

        .bloom_enabled = true,

        // mid morning, so the first frame is lit. see GNY_SKY_START_PHASE.
        .sky_offset_s = GNY_SKY_START_PHASE * GNY_DAY_LENGTH_S,
    };

    nya_world_user_data_set(world);

    /*
     * The scripting VM, and the fonts.
     */
    NYA_Error lua = nya_lua_create(allocator, (NYA_LuaOptions){ .engine_api = true }, &world->lua);

    // not fatal: a demo without scripts beats one that will not start.
    if (!lua.ok) nya_log_warn("Could not create the Lua VM: %s", (NYA_ConstCString)lua.message);

    /*
     * The script is queued, not run: assets resolve at frame end, and the first tick that finds it loaded runs it
     * (see gny_world_script_tick). It is acquired too, or the unloading sweep drops it before that tick. The
     * reference is held for the world's lifetime.
     */
    (void)nya_asset_load((NYA_AssetLoadParameters){ .type = NYA_ASSET_TYPE_TEXT, .handle = NYA_ASSET_SCRIPTS_STARTUP_LUA });
    (void)nya_asset_acquire(NYA_ASSET_SCRIPTS_STARTUP_LUA);

    gny_fonts_register();

    /* The runtime config, before the systems that may read it. See gnyame/config.h. */
    gny_config_attach();

    // before the game layer's first on_update, and exactly once, unlike a layer's on_create.
    gny_systems_register_all();
}

void gny_fonts_register(void) {
    (void)nya_font_register("ui", GNY_UI_FONT, GNY_UI_FONT_SIZE);
    (void)nya_font_register("title", GNY_UI_FONT, GNY_UI_TITLE_FONT_SIZE);
    (void)nya_font_register("menu", GNY_MENU_FONT, GNY_MENU_ITEM_SIZE);
    (void)nya_font_register("menu_title", GNY_MENU_FONT, GNY_MENU_TITLE_SIZE);

    /* Titles are distance fields; the HUD and menu rows are not. */
    (void)nya_font_sdf_set(nya_font_named("title"), true);
    (void)nya_font_sdf_set(nya_font_named("menu_title"), true);

    nya_font_default_set(nya_font_named("ui"));
}

void gny_world_script_tick(f32 delta_time_s) {
    GNY_World* world = gny_world();
    if (world == nullptr || world->lua == nullptr) return;

    // NOT_FOUND while the script is still queued, the ordinary state for a frame or two.
    if (!world->lua_started) {
        NYA_Error result = nya_lua_run_asset(world->lua, NYA_ASSET_SCRIPTS_STARTUP_LUA);

        if (result.ok) {
            world->lua_started = true;

            /* A value read back out of the script. */
            NYA_Value config = { 0 };

            if (nya_lua_global_get(world->lua, nya_arena_temp, "gnyame", &config).ok && config.type == NYA_TYPE_OBJECT) {
                NYA_Value* greeting = nya_object_get(&config.as_object, "greeting");

                if (greeting != nullptr && greeting->type == NYA_TYPE_STRING) {
                    nya_log_info("startup.lua greets the world as '%s'.", greeting->as_string);
                }
            }
        } else if (result.kind != NYA_ERROR_NOT_FOUND) {
            // a syntax error, said once rather than every tick.
            nya_log_warn("startup.lua: %s", (NYA_ConstCString)result.message);
            world->lua_started = true;
        }

        return;
    }

    world->lua_tick_timer_s += delta_time_s;

    if (world->lua_tick_timer_s < GNY_LUA_TICK_INTERVAL_S) return;

    world->lua_tick_timer_s = 0.0F;

    // optional: NOT_FOUND means the script does not define it. asked, so a real failure still reports.
    if (!nya_lua_has_function(world->lua, "gnyame_tick")) return;

    NYA_Value crates = nya_lua_number((f64)gny_entity_box_count(nullptr));

    NYA_Error called = nya_lua_call(world->lua, nya_arena_temp, "gnyame_tick", &crates, 1, nullptr);

    if (!called.ok) nya_log_warn("gnyame_tick: %s", (NYA_ConstCString)called.message);
}

NYA_EntityHandle gny_world_inset_camera(void) {
    GNY_World* world = gny_world();
    nya_assert(world != nullptr, "gny_world_inset_camera before the world exists.");

    if (nya_entity_is_valid(world->inset_camera)) return world->inset_camera;

    NYA_Window* window = nya_window_get(GNY_WINDOW_MAIN);
    if (window == nullptr) return NYA_ENTITY_HANDLE_NONE;

    // bottom right, clear of the stats and bindings panels and the trace panel.
    NYA_Rectf viewport = {
        .x      = (f32)window->screen_width - GNY_CAMERA_VIEW_WIDTH - GNY_CAMERA_VIEW_MARGIN,
        .y      = (f32)window->screen_height - GNY_CAMERA_VIEW_HEIGHT - GNY_CAMERA_VIEW_MARGIN,
        .width  = GNY_CAMERA_VIEW_WIDTH,
        .height = GNY_CAMERA_VIEW_HEIGHT,
    };

    world->inset_camera = gny_entity_camera_create_view(gny_entity_camera_get().position, GNY_CAMERA_VIEW_ZOOM, viewport);

    return world->inset_camera;
}

void gny_world_clear(void) {
    GNY_World* world = gny_world();
    if (world == nullptr) return;

    // first, so the brain is saved and its drones go before the scene they fly in.
    gny_robots_destroy();

    /* Immediate rather than deferred, unlike the rest of this file. */
    for (u32 slot = 0; slot < nya_entity_slot_count(); slot++) {
        NYA_Entity* entity = nya_entity_at_slot(slot);

        // the map's colliders and ledges go with the crates, or the next start spawns a second floor. by slot, since
        // despawning a ledge removes its marker; despawning a stale handle is a no-op.
        if (!gny_entity_is(entity, GNY_ENTITY_BOX) && !gny_entity_is(entity, GNY_ENTITY_TILEMAP)
            && !gny_entity_is(entity, GNY_ENTITY_LEDGE)) {
            continue;
        }

        nya_entity_despawn(entity->handle);
    }

    if (nya_entity_is_valid(world->terrain)) nya_entity_despawn(world->terrain);
    if (nya_entity_is_valid(world->camera)) nya_entity_despawn(world->camera);
    if (nya_entity_is_valid(world->inset_camera)) nya_entity_despawn(world->inset_camera);

    world->terrain             = NYA_ENTITY_HANDLE_NONE;
    world->camera              = NYA_ENTITY_HANDLE_NONE;
    world->inset_camera        = NYA_ENTITY_HANDLE_NONE;

    // the map came from the world's arena and is only forgotten, so the next start loads a fresh one.
    world->tilemap = nullptr;

    /*
     * The VM and the script's reference stay. gny_world_create runs once per process and this runs on every return
     * to the menu, so releasing here would unbalance the single acquire and unload a script the next start needs.
     */
    world->lua_started      = false;
    world->lua_tick_timer_s = 0.0F;
}

void gny_world_destroy(void) {
    /* Nothing to free. */
}

void gny_terrain_generate(u64 seed) {
    GNY_World* world = gny_world();
    nya_assert(world != nullptr, "gny_terrain_generate before the world exists.");

    // created on first use. the shape constants are the game's; sampling, body and drawing are nya_terrain2d_*.
    if (world->terrain2d == nullptr) {
        NYA_EXPECT(
            nya_terrain2d_create(
                nya_world()->allocator,
                (NYA_Terrain2DOptions){
                    .half_width  = GNY_TERRAIN_HALF_WIDTH,
                    .point_step  = GNY_TERRAIN_POINT_STEP,
                    .base_y      = GNY_TERRAIN_BASE_Y,
                    .amplitude   = GNY_TERRAIN_AMPLITUDE,
                    .fill        = GNY_TERRAIN_FILL,
                    .surface     = GNY_TERRAIN_SURFACE,
                    .entity_type = GNY_ENTITY_TERRAIN,
                },
                &world->terrain2d
            ),
            "while creating the 2D terrain"
        );
    }

    nya_terrain2d_generate(world->terrain2d, seed);

    world->terrain_seed = seed;
    world->terrain      = world->terrain2d->entity;
}

void _gny_lights_apply(NYA_Window* window) {
    // the visible region in world units, so glow from an off-screen crate is collected.
    u32 width, height;
    nya_render2d_target_size(window, &width, &height);

    f32x2 min = nya_render2d_screen_to_world(window, f32x2_zero);
    f32x2 max = nya_render2d_screen_to_world(window, (f32x2){ (f32)width, (f32)height });

    NYA_Light2D lights[NYA_SHADER_LIGHT2D_MAX];
    f32x2       positions[NYA_SHADER_LIGHT2D_MAX];

    u32 count = nya_system_entity_lights(min, max, lights, positions, nya_carray_length(lights));

    nya_render2d_lights_apply(window, lights, positions, count, GNY_AMBIENT_LIGHT);
}

void gny_world_draw(NYA_Window* window, NYA_Camera2DTopDown camera) {
    nya_render2d_camera_set(window, camera);

    // terrain first: without a depth test, later draws land on top.
    if (gny_world()->terrain2d != nullptr) nya_terrain2d_draw(gny_world()->terrain2d, window);

    // the map after the terrain and before the entities, for the same reason.
    nya_tilemap_draw(window, gny_world()->tilemap);

    // every entity that draws itself, culled to this camera.
    nya_system_entity_render(window);

    // after the crates, so sparks land in front of them.
    nya_particles_draw(window, gny_world()->sparks);

    _gny_lights_apply(window);

    // back to screen pixels before compositing and the HUD.
    nya_render2d_camera_reset(window);
}

void gny_overlay_toggle(void) {
    gny_world()->overlay_enabled = !gny_world()->overlay_enabled;

    // the overlay has room for six arenas; the log takes all of them, for a report or a profile to read.
    if (gny_world()->overlay_enabled) nya_arena_stats_report();
}

void gny_post_pipelines_ensure(NYA_Window* window) {
    /* The bloom pass: one fragment shader, paired with the batch's vertex stage. */
    NYA_EXPECT(nya_asset_load((NYA_AssetLoadParameters){
        .type      = NYA_ASSET_TYPE_SHADER_FRAGMENT,
        .handle    = NYA_ASSET_SHADER_EFFECT_BLOOM_FRAG,
        .as_shader = { .num_samplers = 1, .num_uniform_buffers = 1 },
    }), "while queueing the bloom fragment shader");

    NYA_EXPECT(nya_asset_load((NYA_AssetLoadParameters){
        .type                 = NYA_ASSET_TYPE_GRAPHICS_PIPELINE,
        .handle               = GNY_PIPELINE_BLOOM,
        .as_graphics_pipeline = {
            .window                 = window,
            .vertex_shader_handle   = NYA_ASSET_SHADER_BATCH2D_VERT,
            .fragment_shader_handle = NYA_ASSET_SHADER_EFFECT_BLOOM_FRAG,

            // the halo spreads onto transparent pixels, so it blends.
            .blend = true,

            // required, and silently wrong if missing: the default 3D layout would read twenty-byte vertices at a 36-byte
            // stride.
            .vertex_layout = NYA_VERTEX_LAYOUT_2D,
        },
    }), "while queueing the bloom pipeline");

    NYA_EXPECT(nya_asset_load((NYA_AssetLoadParameters){
        .type      = NYA_ASSET_TYPE_SHADER_FRAGMENT,
        .handle    = NYA_ASSET_SHADER_EFFECT_GRAYSCALE_FRAG,
        .as_shader = { .num_samplers = 1 },
    }), "while queueing the grayscale fragment shader");

    NYA_EXPECT(nya_asset_load((NYA_AssetLoadParameters){
        .type                 = NYA_ASSET_TYPE_GRAPHICS_PIPELINE,
        .handle               = GNY_PIPELINE_GRAYSCALE,
        .as_graphics_pipeline = {
            .window                 = window,
            .vertex_shader_handle   = NYA_ASSET_SHADER_BATCH2D_VERT,
            .fragment_shader_handle = NYA_ASSET_SHADER_EFFECT_GRAYSCALE_FRAG,
            .blend                  = true,
            .vertex_layout          = NYA_VERTEX_LAYOUT_2D,
        },
    }), "while queueing the grayscale pipeline");
}

f32x2 gny_screen_to_world(const NYA_Window* window, f32x2 screen) {
    // the camera position is the world point at the target's centre, so the offset from the centre scales. this
    // camera never rotates.
    NYA_Camera2DTopDown camera = gny_entity_camera_get();

    f32x2 center = { (f32)window->screen_width * 0.5F, (f32)window->screen_height * 0.5F };

    return ((screen - center) / camera.zoom) + camera.position;
}
