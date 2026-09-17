/**
 * The menu widget and screen changes, headless: keys and the mouse drive a menu, and a request rearranges
 * the layer stack only at the barrier.
 **/

#include "nyangine/nyangine.c"
#include "gnyame/gnyame.c"

#include "SDL3/SDL_init.h"

#define WINDOW_WIDTH  1280
#define WINDOW_HEIGHT 720

static NYA_Event key(NYA_Keycode keycode) {
    return (NYA_Event){ .type = NYA_EVENT_KEY_DOWN, .as_key_event = { .is_down = true, .key = keycode } };
}

static NYA_Event mouse_moved(f32x2 point) {
    return (NYA_Event){ .type = NYA_EVENT_MOUSE_MOVED, .as_mouse_moved_event = { .x = point.x, .y = point.y } };
}

static NYA_Event mouse_down(NYA_MouseButton button, f32x2 point) {
    return (NYA_Event){ .type = NYA_EVENT_MOUSE_BUTTON_DOWN, .as_mouse_button_event = { .is_down = true, .button = button, .x = point.x, .y = point.y } };
}

static b8 menu_event(const NYA_Window* window, GNY_Menu* menu, NYA_Event event) {
    return gny_menu_handle_event(window, menu, &event);
}

static f32x2 row_center(const NYA_Window* window, const GNY_Menu* menu, u32 index) {
    NYA_Rectf bounds = gny_menu_item_bounds(window, menu, index);
    return (f32x2){ bounds.x + (bounds.width * 0.5F), bounds.y + (bounds.height * 0.5F) };
}

/** The layer ids from the bottom of the main window's stack, joined by spaces. */
static NYA_ConstCString stack(void) {
    static char joined[256];
    joined[0] = '\0';

    NYA_Window* window = nya_window_get(GNY_WINDOW_MAIN);
    for (u64 i = 0; i < window->layer_stack->length; i++) {
        if (i > 0) (void)strncat(joined, " ", sizeof(joined) - strlen(joined) - 1);
        (void)strncat(joined, window->layer_stack->items[i].id, sizeof(joined) - strlen(joined) - 1);
    }

    return joined;
}

static void stack_reset(void) {
    NYA_Window* window = nya_window_get(GNY_WINDOW_MAIN);
    while (window->layer_stack->length > 0) (void)nya_layer_pop(GNY_WINDOW_MAIN);

    nya_layer_push(GNY_WINDOW_MAIN, GNY_LAYER_BACKGROUND);
    nya_layer_push(GNY_WINDOW_MAIN, GNY_LAYER_MAIN_MENU);
}

/** A layer with an id and no hooks, standing in for the scenes, which build physics and GPU state. */
static NYA_Layer layer_stub(NYA_ConstCString id) {
    NYA_Layer layer = { .enabled = true };
    (void)snprintf(layer.id, sizeof(layer.id), "%s", id);
    return layer;
}

static const GNY_MenuItem items[] = {
    { .label = nya_string_menu_resume,       .screen = GNY_SCREEN_RESUME                                      },
    { .label = nya_string_menu_music_volume, .kind = GNY_MENU_ITEM_KIND_VOLUME, .channel = NYA_VOLUME_CHANNEL_MUSIC },
    { .label = nya_string_menu_quit,         .screen = GNY_SCREEN_QUIT                                        },
};

s32 main(void) {
    SDL_SetHintWithPriority(SDL_HINT_VIDEO_DRIVER, "offscreen", SDL_HINT_OVERRIDE);
    SDL_SetHintWithPriority(SDL_HINT_AUDIO_DRIVER, "dummy", SDL_HINT_OVERRIDE);

    // settings load from the data directory, so a scratch one keeps the player's own bindings out of this.
    NYA_Arena*  scratch   = nya_arena_create(.name = "test_screens_scratch");
    NYA_String* temp_root = nullptr;
    NYA_EXPECT(nya_filesystem_temp_directory(scratch, &temp_root));

    NYA_CString data_home = nya_string_to_cstring(scratch, nya_path_join(scratch, nya_string_to_cstring(scratch, temp_root), "gnyame-test-screens"));
    (void)nya_filesystem_delete_recursive(data_home);

#if OS_WINDOWS
    (void)_putenv_s("XDG_DATA_HOME", data_home);
    (void)_putenv_s("APPDATA", data_home);
#else
    setenv("XDG_DATA_HOME", data_home, 1);
    setenv("APPDATA", data_home, 1);
#endif

    _NYA_APP_INSTANCE = (NYA_App){ .initialized = true };

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

    // the parts of gny_world_create the screens reach, without the Lua VM and the systems.
    GNY_World* world = nya_arena_alloc(engine_world->allocator, sizeof(GNY_World));
    *world           = (GNY_World){ .window_main = NYA_WINDOW_HANDLE_NONE, .terrain = NYA_ENTITY_HANDLE_NONE, .terrain_seed = 1 };
    nya_world_user_data_set(world);

    gny_actions_init();
    gny_fonts_register();
    gny_layers_init();

    GNY_LAYER_GAME   = layer_stub(GNY_LAYER_GAME_ID);
    GNY_LAYER_UI     = layer_stub(GNY_LAYER_UI_ID);
    GNY_LAYER_CUBE3D = layer_stub(GNY_LAYER_CUBE3D_ID);

    world->window_main = nya_window_create("screens", WINDOW_WIDTH, WINDOW_HEIGHT, NYA_WINDOW_NONE);
    nya_assert(nya_window_is_valid(world->window_main));

    NYA_Window* window    = nya_window_get(GNY_WINDOW_MAIN);
    window->screen_width  = WINDOW_WIDTH;
    window->screen_height = WINDOW_HEIGHT;

    // ── Up and down move the selection and wrap at both ends, on either binding.
    {
        GNY_Menu menu = { .title = "menu", .items = items, .item_count = nya_carray_length(items) };

        nya_check(menu_event(window, &menu, key(NYA_KEY_DOWN)), "a key belongs to the menu");
        nya_check(menu.selected == 1, "down selects the next row, got " FMTu32, menu.selected);

        (void)menu_event(window, &menu, key(NYA_KEY_S));
        nya_check(menu.selected == 2, "and so does the alternative key, got " FMTu32, menu.selected);

        (void)menu_event(window, &menu, key(NYA_KEY_DOWN));
        nya_check(menu.selected == 0, "down on the last row wraps to the first, got " FMTu32, menu.selected);

        (void)menu_event(window, &menu, key(NYA_KEY_UP));
        nya_check(menu.selected == 2, "up on the first row wraps to the last, got " FMTu32, menu.selected);

        (void)menu_event(window, &menu, key(NYA_KEY_W));
        nya_check(menu.selected == 1, "and up moves back, got " FMTu32, menu.selected);

        nya_check(menu_event(window, &menu, key(NYA_KEY_X)), "an unbound key is swallowed too, the menu is modal");
        nya_check(menu.selected == 1, "without moving anything");

        GNY_Menu empty = { .title = "empty" };
        nya_check(!menu_event(window, &empty, key(NYA_KEY_DOWN)), "a menu with no rows takes nothing");
    }

    // ── Left and right edit a volume row in place and clamp; confirm on it requests nothing.
    {
        GNY_Menu menu = { .title = "menu", .items = items, .item_count = nya_carray_length(items), .selected = 1 };

        nya_settings_volume_set(NYA_VOLUME_CHANNEL_MUSIC, 0.5F);

        (void)menu_event(window, &menu, key(NYA_KEY_RIGHT));
        nya_check(fabsf(nya_settings_volume(NYA_VOLUME_CHANNEL_MUSIC) - (0.5F + GNY_VOLUME_STEP)) < 1e-4F, "right raises by a step, got %f",
                  (f64)nya_settings_volume(NYA_VOLUME_CHANNEL_MUSIC));

        (void)menu_event(window, &menu, key(NYA_KEY_A));
        (void)menu_event(window, &menu, key(NYA_KEY_LEFT));
        nya_check(fabsf(nya_settings_volume(NYA_VOLUME_CHANNEL_MUSIC) - (0.5F - GNY_VOLUME_STEP)) < 1e-4F, "left lowers by a step, got %f",
                  (f64)nya_settings_volume(NYA_VOLUME_CHANNEL_MUSIC));

        for (u32 i = 0; i < 40; i++) (void)menu_event(window, &menu, key(NYA_KEY_RIGHT));
        nya_check(nya_settings_volume(NYA_VOLUME_CHANNEL_MUSIC) == 1.0F, "and it clamps at full");

        (void)menu_event(window, &menu, key(NYA_KEY_RETURN));
        nya_system_sim_apply_commands();
        // a volume row's screen is NONE, which gny_screen_request asserts on, so reaching here is the check.
        nya_check(!nya_app_get()->should_quit, "confirm on a volume row requests no screen");

        (void)menu_event(window, &menu, key(NYA_KEY_RIGHT));
        nya_check(menu.selected == 1, "and editing does not move the selection");
    }

    // ── Confirm requests the row's screen, applied at the barrier and not before.
    {
        GNY_Menu menu = { .title = "menu", .items = items, .item_count = nya_carray_length(items), .selected = 2 };

        (void)menu_event(window, &menu, key(NYA_KEY_RETURN));
        nya_check(!nya_app_get()->should_quit, "the request waits for the barrier");

        nya_system_sim_apply_commands();
        nya_check(nya_app_get()->should_quit, "quit applies at the barrier");

        nya_app_get()->should_quit = false;

        menu.selected = 2;
        (void)menu_event(window, &menu, key(NYA_KEY_SPACE));
        nya_system_sim_apply_commands();
        nya_check(nya_app_get()->should_quit, "the alternative confirm key activates too");

        nya_app_get()->should_quit = false;
    }

    // ── Cancel requests on_cancel, ignores a repeat, and NONE swallows it.
    {
        stack_reset();

        GNY_Menu title = { .title = "title", .items = items, .item_count = nya_carray_length(items), .on_cancel = GNY_SCREEN_NONE };
        nya_check(menu_event(window, &title, key(NYA_KEY_ESCAPE)), "cancel with nothing to go back to is swallowed");
        nya_system_sim_apply_commands();
        nya_check(!nya_app_get()->should_quit && nya_string_equals(stack(), "gny_layer_background gny_layer_main_menu"),
                  "and changes nothing, stack '%s'", stack());

        gny_screen_request(GNY_SCREEN_START_GAME);
        nya_system_sim_apply_commands();
        gny_screen_request(GNY_SCREEN_PAUSE);
        nya_system_sim_apply_commands();

        GNY_Menu menu = { .title = "paused", .items = items, .item_count = nya_carray_length(items), .on_cancel = GNY_SCREEN_RESUME };

        NYA_Event repeat              = key(NYA_KEY_ESCAPE);
        repeat.as_key_event.is_repeat = true;

        nya_check(gny_menu_handle_event(window, &menu, &repeat), "a held cancel is still the menu's");
        nya_system_sim_apply_commands();
        nya_check(nya_layer_get(GNY_WINDOW_MAIN, GNY_LAYER_PAUSE_MENU_ID) != nullptr, "but a repeat does not resume, stack '%s'", stack());

        nya_check(menu_event(window, &menu, key(NYA_KEY_ESCAPE)), "cancel belongs to the menu");
        nya_system_sim_apply_commands();
        nya_check(nya_layer_get(GNY_WINDOW_MAIN, GNY_LAYER_PAUSE_MENU_ID) == nullptr, "cancel resumes, stack '%s'", stack());
    }

    // ── The menus and movement resolve to a gamepad as well as keys, and a menu polls it without a device.
    {
        NYA_InputBinding confirm = nya_input_action_get(NYA_INPUT_ACTION_CONFIRM, 2);
        nya_check(confirm.kind == NYA_INPUT_BINDING_GAMEPAD_BUTTON && confirm.button == NYA_GAMEPAD_BUTTON_SOUTH, "confirm is the south button");
        nya_check(nya_input_action_get(NYA_INPUT_ACTION_PAUSE, 1).button == NYA_GAMEPAD_BUTTON_START, "pause is start");

        NYA_InputBinding up    = nya_input_action_get(NYA_INPUT_ACTION_UP, 2);
        NYA_InputBinding stick = nya_input_action_get(NYA_INPUT_ACTION_UP, 3);
        nya_check(up.kind == NYA_INPUT_BINDING_GAMEPAD_BUTTON && up.button == NYA_GAMEPAD_BUTTON_DPAD_UP, "menu up is the d-pad");
        nya_check(stick.kind == NYA_INPUT_BINDING_GAMEPAD_AXIS && stick.axis == NYA_GAMEPAD_AXIS_LEFT_Y && stick.axis_threshold < 0.0F,
                  "and the left stick pushed up");

        NYA_InputBinding left = nya_input_action_get(GNY_ACTION_MOVE_LEFT, 3);
        nya_check(nya_input_action_get(GNY_ACTION_MOVE_LEFT, 1).key == NYA_KEY_A, "walking keeps both keys");
        nya_check(left.kind == NYA_INPUT_BINDING_GAMEPAD_AXIS && left.axis == NYA_GAMEPAD_AXIS_LEFT_X && left.axis_threshold < 0.0F,
                  "and walks left on the stick");

        nya_check(!gny_action_pad_held(NYA_INPUT_ACTION_CONFIRM), "with no pad connected nothing is held");

        GNY_Menu menu = { .title = "menu", .items = items, .item_count = nya_carray_length(items), .selected = 2, .pad_held = U32_MAX };
        gny_menu_update(&menu);
        nya_system_sim_apply_commands();
        nya_check(menu.pad_held == 0 && menu.selected == 2 && !nya_app_get()->should_quit, "and polling changes nothing");
    }

    // ── The mouse: hover selects without consuming, a left click activates a row, anything else is swallowed.
    {
        GNY_Menu menu = { .title = "menu", .subtitle = "sub", .items = items, .item_count = nya_carray_length(items) };

        NYA_Rectf first  = gny_menu_item_bounds(window, &menu, 0);
        NYA_Rectf second = gny_menu_item_bounds(window, &menu, 1);
        nya_check(fabsf(second.y - (first.y + GNY_MENU_ITEM_HEIGHT)) < 1e-3F, "rows stack one row height apart");
        nya_check(first.x + (first.width * 0.5F) == WINDOW_WIDTH * 0.5F, "and are centred in the window");

        nya_check(!menu_event(window, &menu, mouse_moved(row_center(window, &menu, 2))), "hover is left for layers below");
        nya_check(menu.selected == 2, "and selects the row under the pointer, got " FMTu32, menu.selected);

        (void)menu_event(window, &menu, mouse_moved((f32x2){ 1.0F, 1.0F }));
        nya_check(menu.selected == 2, "off every row the selection stays");

        nya_check(menu_event(window, &menu, mouse_down(NYA_MOUSE_BUTTON_LEFT, row_center(window, &menu, 1))), "a click is the menu's");
        nya_check(menu.selected == 1, "a click selects its row");
        nya_system_sim_apply_commands();
        nya_check(!nya_app_get()->should_quit, "and on a volume row requests nothing");

        nya_check(menu_event(window, &menu, mouse_down(NYA_MOUSE_BUTTON_RIGHT, row_center(window, &menu, 2))), "a right click is swallowed");
        nya_system_sim_apply_commands();
        nya_check(menu.selected == 1 && !nya_app_get()->should_quit, "without selecting or activating");

        nya_check(menu_event(window, &menu, mouse_down(NYA_MOUSE_BUTTON_LEFT, (f32x2){ 1.0F, 1.0F })), "a click off the rows is swallowed");
        nya_system_sim_apply_commands();
        nya_check(!nya_app_get()->should_quit, "and requests nothing");

        (void)menu_event(window, &menu, mouse_down(NYA_MOUSE_BUTTON_LEFT, row_center(window, &menu, 2)));
        nya_system_sim_apply_commands();
        nya_check(nya_app_get()->should_quit, "a left click on a screen row requests it");

        nya_app_get()->should_quit = false;
    }

    // ── Screen requests rearrange the stack at the barrier, and only from the screen they belong to.
    {
        NYA_ConstCString title = "gny_layer_background gny_layer_main_menu";
        NYA_ConstCString game  = "gny_layer_background gny_layer_game gny_layer_ui";
        NYA_ConstCString pause = "gny_layer_background gny_layer_game gny_layer_ui gny_layer_pause_menu";

        stack_reset();
        nya_check(gny_modal_active(), "the title screen is modal");

        gny_screen_request(GNY_SCREEN_START_GAME);
        nya_check(nya_string_equals(stack(), title), "nothing moves before the barrier, stack '%s'", stack());

        nya_system_sim_apply_commands();
        nya_check(nya_string_equals(stack(), game), "starting replaces the title with the game and HUD, stack '%s'", stack());
        nya_check(!gny_modal_active(), "and the game is not modal");

        gny_screen_request(GNY_SCREEN_START_GAME);
        gny_screen_request(GNY_SCREEN_RESUME);
        nya_system_sim_apply_commands();
        nya_check(nya_string_equals(stack(), game), "start and resume away from their screens do nothing, stack '%s'", stack());

        gny_screen_request(GNY_SCREEN_PAUSE);
        gny_screen_request(GNY_SCREEN_PAUSE);
        nya_system_sim_apply_commands();
        nya_check(nya_string_equals(stack(), pause), "pausing twice pushes one pause menu, stack '%s'", stack());
        nya_check(!nya_physics2d_enabled(), "and stops the solver");

        gny_screen_request(GNY_SCREEN_RESUME);
        nya_system_sim_apply_commands();
        nya_check(nya_string_equals(stack(), game), "resume pops the pause menu, stack '%s'", stack());
        nya_check(nya_physics2d_enabled(), "and restarts the solver");

        gny_screen_request(GNY_SCREEN_PAUSE);
        gny_screen_request(GNY_SCREEN_RESTART);
        nya_system_sim_apply_commands();
        nya_check(nya_string_equals(stack(), game), "restart closes the pause menu, stack '%s'", stack());
        nya_check(world->terrain_seed == 2, "and moves to the next terrain seed, got " FMTu64, world->terrain_seed);
        nya_check(nya_physics2d_enabled(), "with the solver running");

        gny_screen_request(GNY_SCREEN_PAUSE);
        gny_screen_request(GNY_SCREEN_MAIN_MENU);
        nya_system_sim_apply_commands();
        nya_check(nya_string_equals(stack(), title), "main menu from pause leaves only the title, stack '%s'", stack());
        nya_check(!nya_entity_is_valid(world->terrain), "and clears the 2D scene");

        gny_screen_request(GNY_SCREEN_CUBE3D);
        nya_system_sim_apply_commands();
        nya_check(nya_string_equals(stack(), "gny_layer_background gny_layer_cube3d"), "the 3D demo replaces the title, stack '%s'", stack());

        gny_screen_request(GNY_SCREEN_MAIN_MENU);
        nya_system_sim_apply_commands();
        nya_check(nya_string_equals(stack(), title), "and main menu comes back from it, stack '%s'", stack());

        gny_screen_request(GNY_SCREEN_MAIN_MENU);
        nya_system_sim_apply_commands();
        nya_check(nya_string_equals(stack(), title), "main menu on the title changes nothing, stack '%s'", stack());
    }

    // ── The menu lays out through the registered fonts, and the title is a distance field.
    {
        GNY_Menu menu = { .title = "nyangine", .subtitle = "physics sandbox", .items = items, .item_count = nya_carray_length(items) };

        NYA_Font title_font = nya_font_named("menu_title");
        NYA_Font item_font  = nya_font_named("menu");

        nya_check(nya_font_valid(title_font) && title_font.point_size == GNY_MENU_TITLE_SIZE, "menu_title is registered at the title size");
        nya_check(nya_font_valid(item_font) && item_font.point_size == GNY_MENU_ITEM_SIZE, "menu is registered at the row size");
        nya_check(nya_font_sdf(title_font) && !nya_font_sdf(item_font), "the title asks for a distance field and the rows do not");

        for (u32 i = 0; i < 16 && nya_font_height(title_font, menu.title) * nya_font_height(item_font, menu.subtitle) == 0.0F; i++) {
            nya_event_dispatch((NYA_Event){ .type = NYA_EVENT_FRAME_ENDED });
        }

        f32 title_height    = nya_font_height(title_font, menu.title);
        f32 subtitle_height = nya_font_height(item_font, menu.subtitle);
        nya_check(title_height > subtitle_height && subtitle_height > 0.0F, "both faces load and measure, got %f and %f", (f64)title_height,
                  (f64)subtitle_height);

        NYA_Rectf first    = gny_menu_item_bounds(window, &menu, 0);
        f32       expected = ((WINDOW_HEIGHT - ((GNY_MENU_PADDING * 2.0F) + title_height + subtitle_height + GNY_MENU_TITLE_GAP + (GNY_MENU_ITEM_HEIGHT * 3.0F))) * 0.5F)
                           + GNY_MENU_PADDING + title_height + subtitle_height + GNY_MENU_TITLE_GAP;
        nya_check(fabsf(first.y - expected) < 1e-3F, "the first row sits under the measured title, %f against %f", (f64)first.y, (f64)expected);
    }

    return nya_check_failures() == 0 ? 0 : 1;
}
