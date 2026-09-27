/**
 * The quicksave, headless and through its keys: a load brings every crate back with its body, its size and its
 * callbacks, gives the main camera its view again and rebuilds the level, and a missing slot or a crate the file
 * sized wrong costs nothing but a log line. test_scene.c covers the document; this covers the game's re-attach.
 **/

#include "nyangine-core/nyangine.c"
#include "gnyame/gnyame.c"

#include "SDL3/SDL_init.h"

#define CRATE_COUNT 6

static void press(NYA_Keycode keycode) {
    NYA_Event event = { .type = NYA_EVENT_KEY_DOWN, .as_key_event = { .is_down = true, .key = keycode } };
    gny_layer_game_on_event(nya_window_get(GNY_WINDOW_MAIN), &event);
    nya_assert(event.was_handled, "the key reached no action");
}

/** Crates whose body, size and callbacks all agree with what gny_entity_box_create makes. */
static u32 whole_crates(void) {
    u32 count = 0;

    nya_entity_foreach_kind (GNY_ENTITY_BOX, crate) {
        b8 bound = nya_callback_get(crate->on_update) == (void*)gny_entity_box_on_update && nya_callback_get(crate->on_click) == (void*)gny_entity_box_on_click;
        if (bound && nya_physics2d_body_attached(crate) && crate->physics2d.size.x == crate->scale.x) count++;
    }

    return count;
}

s32 main(void) {
    SDL_SetHintWithPriority(SDL_HINT_VIDEO_DRIVER, "offscreen", SDL_HINT_OVERRIDE);

    // the slot is written under the save root, so a scratch one keeps the player's out of this.
    NYA_Arena*  scratch   = nya_arena_create(.name = "test_quicksave_scratch");
    NYA_String* temp_root = nullptr;
    NYA_EXPECT(nya_filesystem_temp_directory(scratch, &temp_root));

    NYA_CString data_home = nya_string_to_cstring(scratch, nya_path_join(scratch, nya_string_to_cstring(scratch, temp_root), "gnyame-test-quicksave"));
    (void)nya_filesystem_delete_recursive(data_home);

    nya_assert(nya_host_environment_add("XDG_DATA_HOME", data_home));
    nya_assert(nya_host_environment_add("APPDATA", data_home));

    _NYA_APP_INSTANCE = (NYA_App){ .initialized = true, .options = { _NYA_APP_DEFAULT_OPTIONS } };

    b8 sdl_ok = SDL_Init(0);
    nya_assert(sdl_ok, "SDL_Init failed: %s", SDL_GetError());

    NYA_EXPECT(nya_system_save_init());
    nya_system_settings_init();
    nya_system_callback_init();
    NYA_EXPECT(nya_system_events_init());
    nya_system_input_init();
    nya_system_asset_init();
    nya_system_window_init();

    NYA_World* engine_world = nya_world_create();
    (void)nya_world_set(engine_world);

    defer nya_arena_destroy(scratch);
    defer nya_system_save_deinit();
    defer nya_system_settings_deinit();
    defer nya_system_callback_deinit();
    defer nya_system_events_deinit();
    defer nya_system_input_deinit();
    defer nya_system_asset_deinit();
    defer nya_system_window_deinit();
    defer nya_world_destroy(engine_world);

    GNY_World* world = nya_arena_alloc(engine_world->allocator, sizeof(GNY_World));
    *world           = (GNY_World){ .allocator = engine_world->allocator, .window_main = NYA_WINDOW_HANDLE_NONE, .terrain = NYA_ENTITY_HANDLE_NONE, .terrain_seed = 1 };
    nya_world_user_data_set(world);

    world->window_main = nya_window_create("quicksave", 640, 360, NYA_WINDOW_NONE);
    nya_assert(nya_window_is_valid(world->window_main));

    gny_actions_init();

    // the parts of the game layer's on_create a save holds, without the map and its GPU state.
    gny_entity_camera_create((f32x2){ 120.0F, 30.0F }, 1.5F);
    gny_terrain_generate(world->terrain_seed);
    _gny_level_fixtures_spawn();
    for (u32 i = 0; i < CRATE_COUNT; i++) (void)gny_entity_box_create((f32x2){ (f32)i * 60.0F, -200.0F }, GNY_ENTITY_BOX_DEFAULT_FLAGS);

    u32 entities = nya_entity_count();

    // No slot yet: the load is refused before the world is touched.
    {
        press(NYA_KEY_F9);
        nya_check(nya_entity_count() == entities && whole_crates() == CRATE_COUNT, "a missing slot keeps the world, %u entities", nya_entity_count());
    }

    // A save, a crate the file sizes wrong, then the load: everything else comes back whole.
    {
        press(NYA_KEY_F5);
        nya_check(nya_save_exists(GNY_QUICKSAVE_FILE), "the quicksave slot is written");

        gny_entity_box_destroy_all();
        nya_system_sim_apply_commands();
        nya_check(whole_crates() == 0, "the crates are gone before the load");

        press(NYA_KEY_F9);

        nya_check(whole_crates() == CRATE_COUNT, "every crate is back with its body and callbacks, %u of %u", whole_crates(), CRATE_COUNT);
        nya_check(nya_entity_count() == entities, "and the level is rebuilt around them, %u entities for %u", nya_entity_count(), entities);

        NYA_Entity* camera = nya_entity_get(world->camera);
        nya_check(camera != nullptr && gny_entity_camera_view(camera) != nullptr && camera->scale.x == 1.5F, "the main camera has its view and zoom back");
        nya_check(nya_physics2d_body_attached(nya_entity_get(world->terrain)), "the terrain has its chain again");
    }

    // A crate the file says is too large is refused on its own.
    {
        // the last one: a break leaves only the inner loop of the foreach.
        NYA_Entity* oversized = nullptr;
        nya_entity_foreach_kind (GNY_ENTITY_BOX, crate) oversized = crate;
        oversized->scale = (f32x3){ GNY_BOX_MAX_SIZE * 10.0F, GNY_BOX_MAX_SIZE * 10.0F, 1.0F };

        press(NYA_KEY_F5);
        press(NYA_KEY_F9);
        nya_check(whole_crates() == CRATE_COUNT - 1 && gny_entity_box_count(nullptr) == CRATE_COUNT - 1, "the oversized crate is dropped, %u left",
                  gny_entity_box_count(nullptr));
    }

    return nya_check_failures() == 0 ? 0 : 1;
}
