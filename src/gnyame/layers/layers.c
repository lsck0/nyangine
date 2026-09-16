#include "gnyame/gnyame.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API DECLARATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/** The screen changes, as they run at the simulation barrier. See gny_screen_* in layers.h. */
typedef enum GNY_ScreenChange GNY_ScreenChange;

enum GNY_ScreenChange {
    GNY_SCREEN_START_GAME,
    GNY_SCREEN_PAUSE,
    GNY_SCREEN_RESUME,
    GNY_SCREEN_MAIN_MENU,
    GNY_SCREEN_CUBE3D,
    GNY_SCREEN_RESTART,
    GNY_SCREEN_QUIT,
};

NYA_INTERNAL void _gny_screen_request(GNY_ScreenChange change);
NYA_INTERNAL void _gny_screen_apply(void* data);

/** Pops the top layer only if it is the one named, so a change cannot eat something else's layer. */
NYA_INTERNAL b8 _gny_layer_pop_if(void* layer_id);

/** The ground: a filled band under the polyline, plus the brighter line along its surface. */
NYA_INTERNAL void _gny_terrain_draw(NYA_Window* window);

/*
 * One axis of nya_render2d_text_measure_with_font.
 */
NYA_INTERNAL f32 _gny_text_width_with_font(NYA_ConstCString font, f32 size, NYA_ConstCString text);
NYA_INTERNAL f32 _gny_text_height_with_font(NYA_ConstCString font, f32 size, NYA_ConstCString text);

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * LAYER REGISTRATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

void gny_layers_init(void) {
    GNY_LAYER_MAIN_MENU = nya_layer_of(gny_layer_main_menu, GNY_LAYER_MAIN_MENU_ID);
    GNY_LAYER_PAUSE_MENU = nya_layer_of(gny_layer_pause_menu, GNY_LAYER_PAUSE_MENU_ID);
    GNY_LAYER_CUBE3D = nya_layer_of(gny_layer_cube3d, GNY_LAYER_CUBE3D_ID);
    GNY_LAYER_BACKGROUND = nya_layer_of(gny_layer_background, GNY_LAYER_BACKGROUND_ID);
    GNY_LAYER_GAME = nya_layer_of(gny_layer_game, GNY_LAYER_GAME_ID);
    GNY_LAYER_UI = nya_layer_of(gny_layer_ui, GNY_LAYER_UI_ID);
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * WORLD
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

GNY_World* gny_world(void) {
    return nya_world_user_data();
}

void gny_world_create(void) {
    /* From the engine world's arena. */
    NYA_Arena* allocator = nya_world()->allocator;

    GNY_World* world = nya_arena_alloc(allocator, sizeof(GNY_World));

    *world = (GNY_World){
        .allocator           = allocator,
        .terrain             = NYA_ENTITY_HANDLE_NONE,
        /* From --seed when given, so a server operator can reproduce a world. */
        .terrain_seed        = GNY_LAUNCH.world_seed != 0 ? GNY_LAUNCH.world_seed : 1,

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

    /*
     * The named fonts.
     */
    (void)nya_font_register("ui", GNY_UI_FONT, GNY_UI_FONT_SIZE);
    (void)nya_font_register("title", GNY_UI_FONT, GNY_UI_TITLE_FONT_SIZE);

    /* The title face is a distance field; the HUD face is not. */
    (void)nya_font_sdf_set(nya_font_named("title"), true);

    nya_font_default_set(nya_font_named("ui"));

    /*
     * The runtime config, loaded once and kept in sync with its file under NYA_ASSET_HOT_RELOAD. See
     * gnyame/config.h for what NYA_CONFIG holds.
     */
    NYA_Error config_loaded = nya_config_watch(GNY_CONFIG_FILE, nya_reflect_of(GNY_Config), &NYA_CONFIG);

    // not fatal, like a missing settings file: NYA_CONFIG keeps its zeroed defaults.
    if (!config_loaded.ok) nya_log_warn("Could not load %s: %s", GNY_CONFIG_FILE, (NYA_ConstCString)config_loaded.message);

    // before the game layer's first on_update, and exactly once, unlike a layer's on_create.
    gny_systems_register_all();
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

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * WORLD BUILDING
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

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

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * SCREENS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

void gny_screen_start_game(void) {
    _gny_screen_request(GNY_SCREEN_START_GAME);
}

void gny_screen_pause(void) {
    _gny_screen_request(GNY_SCREEN_PAUSE);
}

void gny_screen_resume(void) {
    _gny_screen_request(GNY_SCREEN_RESUME);
}

void gny_screen_main_menu(void) {
    _gny_screen_request(GNY_SCREEN_MAIN_MENU);
}

void gny_screen_cube3d(void) {
    _gny_screen_request(GNY_SCREEN_CUBE3D);
}

void gny_screen_restart(void) {
    _gny_screen_request(GNY_SCREEN_RESTART);
}

void gny_screen_quit(void) {
    _gny_screen_request(GNY_SCREEN_QUIT);
}

b8 gny_modal_active(void) {
    return nya_layer_get(GNY_WINDOW_MAIN, GNY_LAYER_MAIN_MENU_ID) != nullptr
        || nya_layer_get(GNY_WINDOW_MAIN, GNY_LAYER_PAUSE_MENU_ID) != nullptr;
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * MENUS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

NYA_Rectf gny_menu_item_bounds(const NYA_Window* window, const GNY_Menu* menu, u32 index) {
    f32 width  = (f32)window->screen_width;
    f32 height = (f32)window->screen_height;

    f32 title_height = _gny_text_height_with_font(GNY_MENU_FONT, GNY_MENU_TITLE_SIZE, menu->title);
    if (menu->subtitle != nullptr) title_height += _gny_text_height_with_font(GNY_MENU_FONT, GNY_MENU_ITEM_SIZE, menu->subtitle);

    f32 items_height = GNY_MENU_ITEM_HEIGHT * (f32)menu->item_count;
    f32 panel_height = (GNY_MENU_PADDING * 2.0F) + title_height + GNY_MENU_TITLE_GAP + items_height;

    f32 panel_x = (width - GNY_MENU_WIDTH) * 0.5F;
    f32 panel_y = (height - panel_height) * 0.5F;

    f32 first_item_y = panel_y + GNY_MENU_PADDING + title_height + GNY_MENU_TITLE_GAP;

    return (NYA_Rectf){
        .x      = panel_x + GNY_MENU_PADDING,
        .y      = first_item_y + (GNY_MENU_ITEM_HEIGHT * (f32)index),
        .width  = GNY_MENU_WIDTH - (GNY_MENU_PADDING * 2.0F),
        .height = GNY_MENU_ITEM_HEIGHT,
    };
}

b8 gny_menu_handle_event(const NYA_Window* window, GNY_Menu* menu, const NYA_Event* event, OUT GNY_MenuAction* out_action) {
    nya_assert(out_action != nullptr);

    *out_action = GNY_MENU_ACTION_NONE;

    if (menu->item_count == 0) return false;

    switch (event->type) {
        case NYA_EVENT_KEY_DOWN: {
            const NYA_KeyEvent* key = &event->as_key_event;

            /* The engine's menu actions, not the game's movement. */
            if (nya_input_action_matches(NYA_INPUT_ACTION_UP, key->key, key->modifier_flags)) {
                // wraps from the top to the last item. adding item_count - 1 avoids unsigned 0 - 1.
                menu->selected = (menu->selected + menu->item_count - 1) % menu->item_count;
                return true;
            }

            if (nya_input_action_matches(NYA_INPUT_ACTION_DOWN, key->key, key->modifier_flags)) {
                menu->selected = (menu->selected + 1) % menu->item_count;
                return true;
            }

            // left and right only act on a row with a value; otherwise they are swallowed.
            if (menu->items[menu->selected].kind == GNY_MENU_ITEM_KIND_VOLUME) {
                f32 step = 0.0F;
                if (nya_input_action_matches(NYA_INPUT_ACTION_LEFT, key->key, key->modifier_flags)) step = -GNY_VOLUME_STEP;
                if (nya_input_action_matches(NYA_INPUT_ACTION_RIGHT, key->key, key->modifier_flags)) step = GNY_VOLUME_STEP;

                if (step != 0.0F) {
                    NYA_VolumeChannel channel = menu->items[menu->selected].channel;

                    // nya_settings_volume_set clamps.
                    nya_settings_volume_set(channel, nya_settings_volume(channel) + step);
                    return true;
                }
            }

            if (nya_input_action_matches(NYA_INPUT_ACTION_CONFIRM, key->key, key->modifier_flags)) {
                // enter on a volume row does nothing; the value is edited in place.
                if (menu->items[menu->selected].kind == GNY_MENU_ITEM_KIND_VOLUME) return true;

                *out_action = menu->items[menu->selected].action;
                return true;
            }

            /* Everything else is swallowed too: the layer is modal. */
            return !nya_input_action_matches(NYA_INPUT_ACTION_CANCEL, key->key, key->modifier_flags);
        }

        case NYA_EVENT_MOUSE_MOVED: {
            const NYA_MouseMovedEvent* mouse = &event->as_mouse_moved_event;

            // hover moves the same `selected` as the keys, so they never disagree.
            for (u32 i = 0; i < menu->item_count; i++) {
                NYA_Rectf bounds = gny_menu_item_bounds(window, menu, i);

                if (!nya_rect_contains(bounds, (f32x2){ mouse->x, mouse->y })) continue;

                menu->selected = i;
                break;
            }

            // not consumed: nothing here is a click, and layers below may track the mouse.
            return false;
        }

        case NYA_EVENT_MOUSE_BUTTON_DOWN: {
            const NYA_MouseButtonEvent* mouse = &event->as_mouse_button_event;

            if (mouse->button == NYA_MOUSE_BUTTON_LEFT) {
                for (u32 i = 0; i < menu->item_count; i++) {
                    NYA_Rectf bounds = gny_menu_item_bounds(window, menu, i);

                    if (!nya_rect_contains(bounds, (f32x2){ mouse->x, mouse->y })) continue;

                    menu->selected = i;

                    // clicking a volume row selects it; the keys move the value.
                    if (menu->items[i].kind != GNY_MENU_ITEM_KIND_VOLUME) *out_action = menu->items[i].action;

                    break;
                }
            }

            // consumed even off an item: a click on the panel must not drop a crate behind it.
            return true;
        }

        default: return false;
    }
}

void gny_menu_draw(NYA_Window* window, const GNY_Menu* menu) {
    f32 width  = (f32)window->screen_width;
    f32 height = (f32)window->screen_height;

    // over the whole window, so what is behind reads as inactive.
    nya_render2d_rect(window, 0.0F, 0.0F, width, height, GNY_MENU_SCRIM);

    // derived from the first item's box, so the frame and the hit targets agree.
    NYA_Rectf first = gny_menu_item_bounds(window, menu, 0);
    NYA_Rectf last  = gny_menu_item_bounds(window, menu, menu->item_count - 1);

    f32 panel_x      = first.x - GNY_MENU_PADDING;
    f32 panel_width  = first.width + (GNY_MENU_PADDING * 2.0F);
    f32 items_bottom = last.y + last.height;

    f32 title_height = _gny_text_height_with_font(GNY_MENU_FONT, GNY_MENU_TITLE_SIZE, menu->title);
    if (menu->subtitle != nullptr) title_height += _gny_text_height_with_font(GNY_MENU_FONT, GNY_MENU_ITEM_SIZE, menu->subtitle);

    f32 panel_y      = first.y - GNY_MENU_TITLE_GAP - title_height - GNY_MENU_PADDING;
    f32 panel_height = (items_bottom + GNY_MENU_PADDING) - panel_y;

    nya_render2d_rect(window, panel_x, panel_y, panel_width, panel_height, GNY_MENU_PANEL);
    nya_render2d_rect_outline(window, panel_x, panel_y, panel_width, panel_height, 1.0F, GNY_MENU_BORDER);

    f32 text_y = panel_y + GNY_MENU_PADDING;

    f32 title_width = _gny_text_width_with_font(GNY_MENU_FONT, GNY_MENU_TITLE_SIZE, menu->title);
    nya_render2d_text_with_font(window, GNY_MENU_FONT, GNY_MENU_TITLE_SIZE, menu->title, panel_x + ((panel_width - title_width) * 0.5F), text_y,
                            GNY_MENU_TITLE);
    text_y += _gny_text_height_with_font(GNY_MENU_FONT, GNY_MENU_TITLE_SIZE, menu->title);

    if (menu->subtitle != nullptr) {
        f32 subtitle_width = _gny_text_width_with_font(GNY_MENU_FONT, GNY_MENU_ITEM_SIZE, menu->subtitle);
        nya_render2d_text_with_font(window, GNY_MENU_FONT, GNY_MENU_ITEM_SIZE, menu->subtitle, panel_x + ((panel_width - subtitle_width) * 0.5F), text_y,
                                GNY_MENU_SUBTITLE);
    }

    for (u32 i = 0; i < menu->item_count; i++) {
        NYA_Rectf bounds = gny_menu_item_bounds(window, menu, i);

        b8 highlighted = i == menu->selected;

        // the selected item is a filled bar with dark text, easy to spot at a glance.
        if (highlighted) nya_render2d_rect(window, bounds.x, bounds.y, bounds.width, bounds.height, GNY_MENU_HIGHLIGHT);

        /* A volume row draws its value into the label, and arrows while selected. */
        char             row[64];
        NYA_ConstCString label = menu->items[i].label;

        if (menu->items[i].kind == GNY_MENU_ITEM_KIND_VOLUME) {
            f32 volume = nya_settings_volume(menu->items[i].channel);

            (void)snprintf(row, sizeof(row), highlighted ? "< %s  %3.0f%% >" : "%s  %3.0f%%", label, (f64)(volume * 100.0F));
            label = row;
        }

        f32 label_width  = _gny_text_width_with_font(GNY_MENU_FONT, GNY_MENU_ITEM_SIZE, label);
        f32 label_height = _gny_text_height_with_font(GNY_MENU_FONT, GNY_MENU_ITEM_SIZE, label);

        nya_render2d_text_with_font(window, GNY_MENU_FONT, GNY_MENU_ITEM_SIZE, label, bounds.x + ((bounds.width - label_width) * 0.5F),
                                bounds.y + ((bounds.height - label_height) * 0.5F), highlighted ? GNY_MENU_ITEM_ON : GNY_MENU_ITEM);
    }
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * WORLD DRAWING
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/** Darkens everything drawn so far, except where a crate glows. */
NYA_INTERNAL void _gny_lights_apply(NYA_Window* window) {
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
    _gny_terrain_draw(window);

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

/* Registered here rather than by whichever screen wants it first. */
void gny_bloom_pipeline_ensure(NYA_Window* window) {
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
}

void _gny_terrain_draw(NYA_Window* window) {
    GNY_World* world = gny_world();
    if (world->terrain2d == nullptr) return;

    nya_terrain2d_draw(world->terrain2d, window);
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

f32 _gny_text_width_with_font(NYA_ConstCString font, f32 size, NYA_ConstCString text) {
    return nya_render2d_text_measure_with_font(font, size, text).x;
}

f32 _gny_text_height_with_font(NYA_ConstCString font, f32 size, NYA_ConstCString text) {
    return nya_render2d_text_measure_with_font(font, size, text).y;
}

void _gny_screen_request(GNY_ScreenChange change) {
    // through the simulation barrier: the layer stack is being iterated, and pushing can reallocate it.
    nya_sim_defer(_gny_screen_apply, &change, sizeof(change));
}

b8 _gny_layer_pop_if(void* layer_id) {
    NYA_Window* window = nya_window_get(GNY_WINDOW_MAIN);
    if (window == nullptr) return false;
    if (window->layer_stack->length == 0) return false;

    // nya_layer_pop takes the top unconditionally, so check first rather than remove someone else's layer.
    if (window->layer_stack->items[window->layer_stack->length - 1].id != layer_id) return false;

    (void)nya_layer_pop(GNY_WINDOW_MAIN);
    return true;
}

void _gny_screen_apply(void* data) {
    GNY_ScreenChange change = *(GNY_ScreenChange*)data;

    GNY_World* world = gny_world();

    switch (change) {
        case GNY_SCREEN_START_GAME: {
            if (!_gny_layer_pop_if(GNY_LAYER_MAIN_MENU_ID)) return;

            // the game's on_create runs inside the push (terrain, audio); the HUD pushed after draws over it.
            nya_layer_push(GNY_WINDOW_MAIN, GNY_LAYER_GAME);
            nya_layer_push(GNY_WINDOW_MAIN, GNY_LAYER_UI);
        } break;

        case GNY_SCREEN_PAUSE: {
            if (gny_modal_active()) return;

            nya_layer_push(GNY_WINDOW_MAIN, GNY_LAYER_PAUSE_MENU);
            nya_physics2d_enabled_set(false);
        } break;

        case GNY_SCREEN_RESUME: {
            if (!_gny_layer_pop_if(GNY_LAYER_PAUSE_MENU_ID)) return;

            nya_physics2d_enabled_set(true);
        } break;

        case GNY_SCREEN_RESTART: {
            if (!_gny_layer_pop_if(GNY_LAYER_PAUSE_MENU_ID)) return;

            nya_physics2d_enabled_set(true);

            // crates first, or a pile resting on the old surface ends up embedded in the new one.
            gny_entity_box_destroy_all();
            gny_terrain_generate(world->terrain_seed + 1);
        } break;

        case GNY_SCREEN_MAIN_MENU: {
            // back from the 3D demo, a single layer whose on_destroy despawns its own entities.
            if (_gny_layer_pop_if(GNY_LAYER_CUBE3D_ID)) {
                nya_layer_push(GNY_WINDOW_MAIN, GNY_LAYER_MAIN_MENU);
                return;
            }

            if (!_gny_layer_pop_if(GNY_LAYER_PAUSE_MENU_ID)) return;

            // before the layers go, while entities are still up. from on_destroy it would also run during
            // nya_app_deinit, after the entity table is gone.
            gny_world_clear();

            // unwound in push order, since nya_layer_pop only takes the top.
            (void)_gny_layer_pop_if(GNY_LAYER_UI_ID);
            (void)_gny_layer_pop_if(GNY_LAYER_GAME_ID);

            // the solver keeps running on an empty world, so starting the game again needs no switch.
            nya_physics2d_enabled_set(true);

            nya_layer_push(GNY_WINDOW_MAIN, GNY_LAYER_MAIN_MENU);
        } break;

        case GNY_SCREEN_CUBE3D: {
            if (!_gny_layer_pop_if(GNY_LAYER_MAIN_MENU_ID)) return;

            // no HUD layer: the demo draws its text through render2d after nya_render3d_end.
            nya_layer_push(GNY_WINDOW_MAIN, GNY_LAYER_CUBE3D);
        } break;

        case GNY_SCREEN_QUIT: {
            nya_app_get()->should_quit = true;
        } break;

        default: break;
    }
}

f32x2 gny_screen_to_world(const NYA_Window* window, f32x2 screen) {
    // the camera position is the world point at the target's centre, so the offset from the centre scales. this
    // camera never rotates.
    NYA_Camera2DTopDown camera = gny_entity_camera_get();

    f32x2 center = { (f32)window->screen_width * 0.5F, (f32)window->screen_height * 0.5F };

    return ((screen - center) / camera.zoom) + camera.position;
}
