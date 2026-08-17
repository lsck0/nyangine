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
 *
 * The engine has nya_render2d_text_width and nya_render2d_text_height for the *current* font, and only the
 * combined measure for a named one. The menu names its font on every call so it never disturbs the
 * font the HUD set, which leaves it doing `.x` and `.y` on a vector at a dozen call sites.
 */
NYA_INTERNAL f32 _gny_text_width_with_font(NYA_ConstCString font, f32 size, NYA_ConstCString text);
NYA_INTERNAL f32 _gny_text_height_with_font(NYA_ConstCString font, f32 size, NYA_ConstCString text);

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * LAYER REGISTRATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

void gny_layers_init(void) {
    GNY_LAYER_MAIN_MENU = (NYA_Layer){
        .id         = GNY_LAYER_MAIN_MENU_ID,
        .enabled    = true,
        .on_create  = nya_callback(gny_layer_main_menu_on_create),
        .on_destroy = nya_callback(gny_layer_main_menu_on_destroy),
        .on_event   = nya_callback(gny_layer_main_menu_on_event),
        .on_update  = nya_callback(gny_layer_main_menu_on_update),
        .on_render  = nya_callback(gny_layer_main_menu_on_render),
    };

    GNY_LAYER_PAUSE_MENU = (NYA_Layer){
        .id         = GNY_LAYER_PAUSE_MENU_ID,
        .enabled    = true,
        .on_create  = nya_callback(gny_layer_pause_menu_on_create),
        .on_destroy = nya_callback(gny_layer_pause_menu_on_destroy),
        .on_event   = nya_callback(gny_layer_pause_menu_on_event),
        .on_update  = nya_callback(gny_layer_pause_menu_on_update),
        .on_render  = nya_callback(gny_layer_pause_menu_on_render),
    };

    GNY_LAYER_CUBE3D = (NYA_Layer){
        .id         = GNY_LAYER_CUBE3D_ID,
        .enabled    = true,
        .on_create  = nya_callback(gny_layer_cube3d_on_create),
        .on_destroy = nya_callback(gny_layer_cube3d_on_destroy),
        .on_event   = nya_callback(gny_layer_cube3d_on_event),
        .on_update  = nya_callback(gny_layer_cube3d_on_update),
        .on_render  = nya_callback(gny_layer_cube3d_on_render),
    };

    GNY_LAYER_BACKGROUND = (NYA_Layer){
        .id         = GNY_LAYER_BACKGROUND_ID,
        .enabled    = true,
        .on_create  = nya_callback(gny_layer_background_on_create),
        .on_destroy = nya_callback(gny_layer_background_on_destroy),
        .on_event   = nya_callback(gny_layer_background_on_event),
        .on_update  = nya_callback(gny_layer_background_on_update),
        .on_render  = nya_callback(gny_layer_background_on_render),
    };

    GNY_LAYER_GAME = (NYA_Layer){
        .id         = GNY_LAYER_GAME_ID,
        .enabled    = true,
        .on_create  = nya_callback(gny_layer_game_on_create),
        .on_destroy = nya_callback(gny_layer_game_on_destroy),
        .on_event   = nya_callback(gny_layer_game_on_event),
        .on_update  = nya_callback(gny_layer_game_on_update),
        .on_render  = nya_callback(gny_layer_game_on_render),
    };

    GNY_LAYER_UI = (NYA_Layer){
        .id         = GNY_LAYER_UI_ID,
        .enabled    = true,
        .on_create  = nya_callback(gny_layer_ui_on_create),
        .on_destroy = nya_callback(gny_layer_ui_on_destroy),
        .on_event   = nya_callback(gny_layer_ui_on_event),
        .on_update  = nya_callback(gny_layer_ui_on_update),
        .on_render  = nya_callback(gny_layer_ui_on_render),
    };
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
    /*
     * From the engine world's arena rather than one of this library's own.
     *
     * It has to outlive both a frame and a hot reload, and the engine world already provides exactly
     * that — it lives in the executable and is freed when the world is. Owning a second arena here
     * meant two lifetimes to unwind in the right order; now there is one.
     */
    NYA_Arena* allocator = nya_world()->allocator;

    GNY_World* world = nya_arena_alloc(allocator, sizeof(GNY_World));

    *world = (GNY_World){
        .allocator           = allocator,
        .terrain             = NYA_ENTITY_HANDLE_NONE,
        .terrain_points      = nya_arena_alloc(allocator, GNY_TERRAIN_POINT_COUNT * sizeof(f32x2)),
        .terrain_point_count = 0,
        /*
         * From --seed where one was given, so a server operator can reproduce a world.
         *
         * One rather than zero as the fallback, because the terrain generator treats a seed as a seed and
         * zero is a perfectly ordinary one — but a default of zero would be indistinguishable from
         * "nobody chose", which is the ambiguity the launch config avoids by using zero for exactly that.
         */
        .terrain_seed        = GNY_LAUNCH.world_seed != 0 ? GNY_LAUNCH.world_seed : 1,

        // Allocated once with the world, not per burst: the pool is fixed and emission is allocation
        // free, which is the whole point of a ceiling rather than a growable list.
        .sparks = nya_particles_create(allocator, GNY_SPARK_POOL),

        // The camera is an entity and does not exist yet — the game layer creates it when it is
        // pushed. Until then gny_entity_camera_get answers with the identity camera.
        .camera       = NYA_ENTITY_HANDLE_NONE,
        .inset_camera = NYA_ENTITY_HANDLE_NONE,

        .bloom_enabled = true,

        // Mid morning, so the first frame of the demo is lit rather than black. See GNY_SKY_START_PHASE.
        .sky_offset_s = GNY_SKY_START_PHASE * GNY_DAY_LENGTH_S,
    };

    nya_world_user_data_set(world);
}

NYA_EntityHandle gny_world_inset_camera(void) {
    GNY_World* world = gny_world();
    nya_assert(world != nullptr, "gny_world_inset_camera before the world exists.");

    if (nya_entity_is_valid(world->inset_camera)) return world->inset_camera;

    NYA_Window* window = nya_window_get(GNY_WINDOW_MAIN);
    if (window == nullptr) return NYA_ENTITY_HANDLE_NONE;

    // Bottom right, clear of the stats and bindings panels on the left and the trace panel opposite.
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

    /*
     * Immediate rather than deferred, unlike everywhere else in this file.
     *
     * This runs from the simulation barrier inside a screen change, which is already past the point
     * where anything is walking the entity table — and a deferred despawn queued here would be
     * applied a tick later, with the layer that owned these entities already gone.
     *
     * By slot rather than through nya_entity_foreach, which is what nya_entity_clear does and for
     * the same reason: despawning is the one thing that macro is not safe under. Each despawn takes
     * the entity's physics body with it, which is the point of the body living on the entity.
     */
    for (u32 slot = 0; slot < nya_entity_slot_count(); slot++) {
        NYA_Entity* entity = nya_entity_at_slot(slot);

        // The map's colliders go with the crates. They are spawned by the game layer and would
        // otherwise survive a return to the menu — and then be spawned a second time on the next
        // start, doubling the floor and pushing anything resting on it out hard.
        if (!gny_entity_is(entity, GNY_ENTITY_BOX) && !gny_entity_is(entity, GNY_ENTITY_TILEMAP)) continue;

        nya_entity_despawn(entity->handle);
    }

    if (nya_entity_is_valid(world->terrain)) nya_entity_despawn(world->terrain);
    if (nya_entity_is_valid(world->camera)) nya_entity_despawn(world->camera);
    if (nya_entity_is_valid(world->inset_camera)) nya_entity_despawn(world->inset_camera);

    world->terrain             = NYA_ENTITY_HANDLE_NONE;
    world->camera              = NYA_ENTITY_HANDLE_NONE;
    world->inset_camera        = NYA_ENTITY_HANDLE_NONE;
    world->terrain_point_count = 0;

    // The map itself came out of the world's arena and is not freed here — only forgotten, so the
    // next start loads a fresh one rather than drawing a map whose colliders have just been despawned.
    world->tilemap = nullptr;
}

void gny_world_destroy(void) {
    /*
     * Nothing to free.
     *
     * This struct and everything hanging off it come out of the engine world's arena, so
     * nya_world_destroy releases the lot — and it runs inside nya_app_deinit, after the entity table
     * has been emptied. Freeing the arena here instead would pull the ground out from under the
     * despawn callbacks that have not run yet.
     *
     * Kept as a named no-op rather than deleted so gnyame_deinit still reads as a pair with
     * gny_world_create, and so there is somewhere obvious to put teardown that is genuinely this
     * library's own.
     */
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * WORLD BUILDING
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

void gny_terrain_generate(u64 seed) {
    GNY_World* world = gny_world();
    nya_assert(world != nullptr, "gny_terrain_generate before the world exists.");

    // Immediate rather than deferred: this runs from startup and from a key press, never from
    // inside an entity iteration, and the new terrain has to be spawned before this call returns.
    if (nya_entity_is_valid(world->terrain)) nya_entity_despawn(world->terrain);

    world->terrain_seed = seed;

    // The RNG takes its seed as an uppercase hex string of at most 64 digits, left padded — not an
    // arbitrary label. A descriptive one like "gnyame-terrain-1" is rejected at the first letter.
    char seed_text[17];
    (void)snprintf(seed_text, sizeof(seed_text), "%016llX", (unsigned long long)seed);

    NYA_RNG   rng   = nya_rng_create(.seed = seed_text);
    NYA_Noise noise = nya_noise_create(&rng);

    NYA_NoiseParams params = { .octaves = 4, .lacunarity = 2.0F, .gain = 0.5F };

    /*
     * Sampled left to right at a fixed spacing, which is what makes the result a *height field*
     * rather than an arbitrary polyline: x is strictly increasing, so no segment can double back and
     * trap a crate inside the ground.
     *
     * fbm returns roughly [-1, 1]; the low frequency term is what gives the hills their overall
     * shape and the higher octaves the small bumps a crate visibly tumbles over.
     */
    for (u32 i = 0; i < GNY_TERRAIN_POINT_COUNT; i++) {
        f32 x = -GNY_TERRAIN_HALF_WIDTH + ((f32)i * GNY_TERRAIN_POINT_STEP);

        f32 height = nya_noise_fbm1(&noise, x * 0.0018F, params);

        world->terrain_points[i] = (f32x2){ x, GNY_TERRAIN_BASE_Y + (height * GNY_TERRAIN_AMPLITUDE) };
    }

    world->terrain_point_count = GNY_TERRAIN_POINT_COUNT;

    world->terrain = nya_entity_spawn(
        .name  = "terrain",
        .type  = GNY_ENTITY_TERRAIN,
        .state = NYA_ENTITY_STATE_ACTIVE | NYA_ENTITY_STATE_VISIBLE | NYA_ENTITY_STATE_STATIC,
    );
    nya_assert(nya_entity_is_valid(world->terrain), "Failed to spawn the terrain entity.");

    // The points are already in world units relative to the origin, and the entity sits at the
    // origin, so they are its body frame unchanged. Friction high enough that a crate landing on a
    // slope settles instead of sliding to the bottom of the map.
    b8 attached = nya_physics2d_body_attach(
        world->terrain,
        .type        = NYA_PHYSICS_BODY_STATIC,
        .shape       = NYA_PHYSICS2D_SHAPE_CHAIN,
        .points      = world->terrain_points,
        .point_count = world->terrain_point_count,
        .friction    = 0.8F,
        .restitution = 0.0F,
    );
    nya_assert(attached, "Failed to attach the terrain's chain body.");

    nya_info("Terrain generated: %u points, seed %llu.", world->terrain_point_count, (unsigned long long)seed);
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

            /*
             * The engine's menu actions, not this game's movement ones.
             *
             * Both default to the same keys and they are deliberately separate: a player who rebinds
             * "walk left" has not asked for the menu cursor to move with it. See actions.h.
             *
             * Auto repeat is kept here rather than dropped, unlike in the game layer — holding down
             * to run a cursor along a list or a volume to the end of its range is what a menu is
             * expected to do.
             */
            if (nya_input_action_matches(NYA_INPUT_ACTION_UP, key->key, key->modifier_flags)) {
                // Wrapping, so arrowing up from the top lands on the last item rather than
                // sticking. Adding item_count - 1 rather than subtracting one keeps it in
                // unsigned arithmetic, where 0 - 1 is four billion.
                menu->selected = (menu->selected + menu->item_count - 1) % menu->item_count;
                return true;
            }

            if (nya_input_action_matches(NYA_INPUT_ACTION_DOWN, key->key, key->modifier_flags)) {
                menu->selected = (menu->selected + 1) % menu->item_count;
                return true;
            }

            // Left and right only mean anything on a row that has a value; on an ordinary row they
            // fall through and are swallowed with everything else.
            if (menu->items[menu->selected].kind == GNY_MENU_ITEM_KIND_VOLUME) {
                f32 step = 0.0F;
                if (nya_input_action_matches(NYA_INPUT_ACTION_LEFT, key->key, key->modifier_flags)) step = -GNY_VOLUME_STEP;
                if (nya_input_action_matches(NYA_INPUT_ACTION_RIGHT, key->key, key->modifier_flags)) step = GNY_VOLUME_STEP;

                if (step != 0.0F) {
                    NYA_VolumeChannel channel = menu->items[menu->selected].channel;

                    // nya_settings_volume_set clamps, so the ends of the range need no handling here.
                    nya_settings_volume_set(channel, nya_settings_volume(channel) + step);
                    return true;
                }
            }

            if (nya_input_action_matches(NYA_INPUT_ACTION_CONFIRM, key->key, key->modifier_flags)) {
                // A volume row has nothing to confirm: it is edited in place, so pressing enter on
                // one should do nothing rather than fire whatever GNY_MENU_ACTION_NONE happens to be.
                if (menu->items[menu->selected].kind == GNY_MENU_ITEM_KIND_VOLUME) return true;

                *out_action = menu->items[menu->selected].action;
                return true;
            }

            /*
             * Everything else is swallowed too, and that is the point of a modal layer.
             *
             * The keys underneath are live: the game layer spawns a burst on space and clears the
             * world on `c`, and the HUD quits on escape. Letting an unrecognised key through means
             * typing at the pause menu quietly rearranges the world behind it.
             *
             * The one exception is cancel, which each menu answers for itself — "back" means
             * something different at the root than it does one level down.
             */
            return !nya_input_action_matches(NYA_INPUT_ACTION_CANCEL, key->key, key->modifier_flags);
        }

        case NYA_EVENT_MOUSE_MOVED: {
            const NYA_MouseMovedEvent* mouse = &event->as_mouse_moved_event;

            // Hover moves the same `selected` the keys do, so the two never disagree about what is
            // highlighted — which is what makes a menu jump when a hand brushes the mouse after
            // arrowing down to something.
            for (u32 i = 0; i < menu->item_count; i++) {
                NYA_Rectf bounds = gny_menu_item_bounds(window, menu, i);

                if (!nya_rect_contains(bounds, (f32x2){ mouse->x, mouse->y })) continue;

                menu->selected = i;
                break;
            }

            // Not consumed: moving the mouse over a menu should not stop anything underneath from
            // tracking it, and nothing here is a click.
            return false;
        }

        case NYA_EVENT_MOUSE_BUTTON_DOWN: {
            const NYA_MouseButtonEvent* mouse = &event->as_mouse_button_event;

            if (mouse->button == NYA_MOUSE_BUTTON_LEFT) {
                for (u32 i = 0; i < menu->item_count; i++) {
                    NYA_Rectf bounds = gny_menu_item_bounds(window, menu, i);

                    if (!nya_rect_contains(bounds, (f32x2){ mouse->x, mouse->y })) continue;

                    menu->selected = i;

                    // Clicking a volume row selects it and leaves the value alone. Dragging a slider
                    // would be a real widget; this menu is a list, and the keys are how a value moves.
                    if (menu->items[i].kind != GNY_MENU_ITEM_KIND_VOLUME) *out_action = menu->items[i].action;

                    break;
                }
            }

            // Consumed whether or not it landed on an item. A click on the empty part of a modal
            // panel is still a click on the panel, and letting it through drops a crate behind it.
            return true;
        }

        default: return false;
    }
}

void gny_menu_draw(NYA_Window* window, const GNY_Menu* menu) {
    f32 width  = (f32)window->screen_width;
    f32 height = (f32)window->screen_height;

    // Over the whole window, so whatever is behind reads as inactive rather than merely covered.
    nya_render2d_rect(window, 0.0F, 0.0F, width, height, GNY_MENU_SCRIM);

    // The panel is derived from the first item's box rather than recomputed, so the frame and the
    // hit targets cannot drift apart.
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

        // The selected item is a filled bar with dark text on it rather than merely a brighter
        // label, so which one is selected survives being glanced at.
        if (highlighted) nya_render2d_rect(window, bounds.x, bounds.y, bounds.width, bounds.height, GNY_MENU_HIGHLIGHT);

        /*
         * A volume row draws its value into the label, and the arrows only while it is selected.
         *
         * Arrows on every row at once reads as four sliders competing for attention; on the selected
         * one it reads as an instruction. The buffer is a frame-local stack array because the string
         * is consumed by the very next call.
         */
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

/**
 * Darkens everything drawn so far, except where a crate is.
 *
 * Between the world and the camera reset, deliberately: the lights are in world coordinates and the
 * pass converts them through whatever camera is currently set, so it has to run while that camera is
 * still in effect — and *before* the HUD, which is drawn in screen space and should not be dimmed by
 * a torch being somewhere else.
 * */
NYA_INTERNAL void _gny_lights_apply(NYA_Window* window) {
    // The visible region, in world units, so a crate whose glow reaches the screen is collected even
    // when the crate itself is not on it.
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

    // Terrain before entities, because there is no depth test: later draws land on top, so draw
    // order is the only thing deciding what is in front.
    _gny_terrain_draw(window);

    // After the terrain and before the entities. There is no depth test in 2D, so this ordering is
    // the only thing putting the map over the ground and the crates over the map.
    nya_tilemap_draw(window, gny_world()->tilemap);

    // Every entity that knows how to draw itself, culled to this camera's view by the spatial grid.
    nya_system_entity_render(window);

    // After the crates, so a spark thrown off an impact lands in front of the crate that threw it.
    // There is no depth test in 2D, so this ordering is the only thing that decides.
    nya_particles_draw(window, gny_world()->sparks);

    _gny_lights_apply(window);

    // Back to screen pixels. The caller may be about to composite, and the HUD layer above certainly
    // is — leaving a camera set would put both somewhere in the world.
    nya_render2d_camera_reset(window);
}

/*
 * Registered here rather than by whichever screen happens to want it first.
 *
 * Both the 2D world and the 3D scene composite through this one pipeline, and asset loads are keyed on
 * the handle — so a second registration is a no-op and the danger is not duplication but drift: two
 * copies of the shader and vertex layout that have to agree, in files that have no reason to be read
 * together. One function, called from both on_create paths.
 */
void gny_bloom_pipeline_ensure(NYA_Window* window) {
    /*
     * The bloom pass: one fragment shader, and a pipeline pairing it with the batch's own vertex
     * stage.
     *
     * Neither count is inferred — SDL is told what the shader binds and validates nothing against the
     * compiled binary. effect_bloom.frag.hlsl declares a sampler at t0/space2 and a cbuffer at
     * b0/space3, so both have to be named here.
     *
     * `num_uniform_buffers` was missing, and the way that failed is worth writing down. On Vulkan it
     * worked perfectly: the descriptor set layout SDL builds is permissive enough that a shader reading
     * a uniform block nobody declared still binds. On D3D12 the declared counts *are* the root
     * signature, so a shader reading a CBV that is not in it is a pipeline the driver refuses to
     * create — "Could not create graphics pipeline state, 0x80004005", with no mention of a uniform
     * buffer anywhere in it.
     *
     * The visible symptom was worse than a missing glow: both scenes composite *through* this pipeline,
     * so the whole world drew into an offscreen target and was never blitted back. The 3D scene simply
     * did not appear on Windows.
     */
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

            // The halo spreads onto pixels the world left transparent, so it has to blend rather
            // than overwrite — see the alpha note at the end of the shader.
            .blend = true,

            // Mandatory, and silent if wrong: the default is the wider NYA_Vertex3D layout, and a
            // mismatch is not an error anywhere. The shader would read the batch's twenty byte
            // vertices at a sixty-four byte stride and draw the quad somewhere off screen.
            .vertex_layout = NYA_VERTEX_LAYOUT_2D,
        },
    }), "while queueing the bloom pipeline");
}

void gny_scene_target_ensure(NYA_Window* window) {
    GNY_World* world = gny_world();

    u32 width  = window->screen_width;
    u32 height = window->screen_height;

    // A zero sized window is minimised or mid resize. Creating a target of no size is not a thing
    // the GPU will do, and the caller falls back to drawing straight to the window.
    if (width == 0 || height == 0) return;

    if (world->scene.texture != nullptr && world->scene.width == width && world->scene.height == height) return;

    // Recreated rather than resized, because a GPU texture has no resize. Freeing first matters: a
    // window dragged to a new size produces a run of these, and leaking one per frame of a drag adds
    // up to hundreds of megabytes by the time the mouse comes up.
    if (world->scene.texture != nullptr) nya_render_texture_destroy(&world->scene);

    world->scene = nya_render_texture_create(window, width, height);
}

void _gny_terrain_draw(NYA_Window* window) {
    GNY_World* world = gny_world();
    if (world->terrain_point_count < 2) return;

    /*
     * Filled as one quad per sample interval, from the surface down to a flat bottom well below the
     * view.
     *
     * Not a triangle fan from a single centre: the terrain is a wide, shallow height field, so a fan
     * would produce long thin slivers whose shared vertex is off screen, and any concavity in the
     * profile would fold the fan back over itself. A quad per interval has neither problem and is
     * the same two triangles the batch is built for.
     */
    f32 bottom = GNY_TERRAIN_BASE_Y + GNY_TERRAIN_AMPLITUDE + 2000.0F;

    for (u32 i = 0; i + 1 < world->terrain_point_count; i++) {
        f32x2 left  = world->terrain_points[i];
        f32x2 right = world->terrain_points[i + 1];

        nya_render2d_triangle(window, left, right, (f32x2){ right.x, bottom }, GNY_TERRAIN_FILL);
        nya_render2d_triangle(window, left, (f32x2){ right.x, bottom }, (f32x2){ left.x, bottom }, GNY_TERRAIN_FILL);
    }

    // The surface line last, so it sits on top of the fill rather than being half covered by it.
    nya_render2d_polyline(window, world->terrain_points, world->terrain_point_count, 3.0F, GNY_TERRAIN_SURFACE);
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
    // Through the simulation barrier rather than applied here. See gny_screen_* in layers.h: the
    // layer stack is being iterated by whoever called this, and pushing can reallocate it.
    nya_sim_defer(_gny_screen_apply, &change, sizeof(change));
}

b8 _gny_layer_pop_if(void* layer_id) {
    NYA_Window* window = nya_window_get(GNY_WINDOW_MAIN);
    if (window == nullptr) return false;
    if (window->layer_stack->length == 0) return false;

    // nya_layer_pop takes the top of the stack unconditionally, so a change that fires when the
    // stack is not in the shape it expected would quietly remove somebody else's layer. Checking
    // first turns that into a no-op.
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

            // The game's on_create runs from inside the push, which is what generates the terrain
            // and queues the audio. Pushing the HUD after it means the HUD draws over it.
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

            // Crates first: regenerating under a settled pile leaves anything resting on the old
            // surface embedded in the new one, and the solver pushes it out hard.
            gny_entity_box_destroy_all();
            gny_terrain_generate(world->terrain_seed + 1);
        } break;

        case GNY_SCREEN_MAIN_MENU: {
            // Coming back from the 3D demo, which is a single layer with no pause menu over it. Its
            // on_destroy despawns the cube and the ground, so nothing else has to be unwound here.
            if (_gny_layer_pop_if(GNY_LAYER_CUBE3D_ID)) {
                nya_layer_push(GNY_WINDOW_MAIN, GNY_LAYER_MAIN_MENU);
                return;
            }

            if (!_gny_layer_pop_if(GNY_LAYER_PAUSE_MENU_ID)) return;

            // Before the layers go, while the entity system is certainly still up. Doing this from
            // the game layer's on_destroy instead would also run it during nya_app_deinit, which
            // tears the entity table down before it destroys the windows.
            gny_world_clear();

            // Unwound in the order they were pushed, since nya_layer_pop only takes the top.
            (void)_gny_layer_pop_if(GNY_LAYER_UI_ID);
            (void)_gny_layer_pop_if(GNY_LAYER_GAME_ID);

            // The solver runs again with nothing in the world, which costs nothing and means the
            // menu does not have to remember to switch it back on when the game is started again.
            nya_physics2d_enabled_set(true);

            nya_layer_push(GNY_WINDOW_MAIN, GNY_LAYER_MAIN_MENU);
        } break;

        case GNY_SCREEN_CUBE3D: {
            if (!_gny_layer_pop_if(GNY_LAYER_MAIN_MENU_ID)) return;

            // No HUD layer over it: the demo draws its own text through render2d after
            // nya_render3d_end, which is the interop it exists to show.
            nya_layer_push(GNY_WINDOW_MAIN, GNY_LAYER_CUBE3D);
        } break;

        case GNY_SCREEN_QUIT: {
            nya_app_get()->should_quit = true;
        } break;

        default: break;
    }
}

f32x2 gny_screen_to_world(const NYA_Window* window, f32x2 screen) {
    // The camera's position is the world point at the *centre* of the target, so the offset from
    // the centre is what scales. Rotation is not handled because this demo's camera never turns;
    // adding it would be the same inverse nya_render2d_screen_to_world writes out.
    NYA_Camera2DTopDown camera = gny_entity_camera_get();

    f32x2 center = { (f32)window->screen_width * 0.5F, (f32)window->screen_height * 0.5F };

    return ((screen - center) / camera.zoom) + camera.position;
}
