/**
 * The menus and screen changes, headless: keys, the pointer and the pause action drive the real menu layers,
 * and a request rearranges the layer stack only at the barrier.
 **/

#include "nyangine/nyangine.c"
#include "gnyame/gnyame.c"

#include "SDL3/SDL_init.h"

#define WINDOW_WIDTH  1280
#define WINDOW_HEIGHT 720

static NYA_Event key(NYA_Keycode keycode, b8 down) {
    return (NYA_Event){ .type = down ? NYA_EVENT_KEY_DOWN : NYA_EVENT_KEY_UP, .as_key_event = { .is_down = down, .key = keycode } };
}

/** A key going down and up before the next tick, as the input system sees a quick press. */
static void tap(NYA_Keycode keycode) {
    NYA_Event down = key(keycode, true);
    NYA_Event up   = key(keycode, false);
    nya_system_input_handle_event(&down);
    nya_system_input_handle_event(&up);
}

static void pointer_move(f32x2 point) {
    f32x2     from  = nya_input_mouse_position();
    NYA_Event event = { .type = NYA_EVENT_MOUSE_MOVED, .as_mouse_moved_event = { .x = point.x, .y = point.y, .delta_x = point.x - from.x, .delta_y = point.y - from.y } };
    nya_system_input_handle_event(&event);
}

static void pointer_button(NYA_MouseButton button, b8 down) {
    f32x2     at    = nya_input_mouse_position();
    NYA_Event event = { .type = down ? NYA_EVENT_MOUSE_BUTTON_DOWN : NYA_EVENT_MOUSE_BUTTON_UP, .as_mouse_button_event = { .is_down = down, .button = button, .x = at.x, .y = at.y } };
    nya_system_input_handle_event(&event);
}

/** The rest of a tick after the layers updated: the barrier, then the edges roll. */
static void barrier(void) {
    nya_system_sim_apply_commands();
    nya_event_dispatch((NYA_Event){ .type = NYA_EVENT_UPDATING_ENDED });
    nya_world()->sim_system.tick++;
}

static void main_menu(void) {
    gny_layer_main_menu_on_update(nya_window_get(GNY_WINDOW_MAIN), 0.0F);
}

static void pause_menu(void) {
    gny_layer_pause_menu_on_update(nya_window_get(GNY_WINDOW_MAIN), 0.0F);
}

/** `keycode` tapped, then a whole tick of `menu`. */
static void press(NYA_Keycode keycode, void (*menu)(void)) {
    tap(keycode);
    menu();
    barrier();
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

/** Where the UI last laid out the top level panel `id` in the main window. A zero rectangle when it has no slot. */
static NYA_Rectf panel_bounds(NYA_ConstCString id) {
    NYA_Window* window = nya_window_get(GNY_WINDOW_MAIN);
    u64         key    = _nya_ui_id((u64)window->handle.index + 1, id);

    for (u32 i = 0; i < NYA_UI_PANELS_MAX; i++) {
        if (_nya_ui.panels[i].id == key) return _nya_ui.panels[i].bounds;
    }

    return (NYA_Rectf){ 0 };
}

/** The id of `label` in the named container `inner`, itself inside the top level panel `outer`. */
static u64 widget_id(NYA_ConstCString outer, NYA_ConstCString inner, NYA_ConstCString label) {
    NYA_Window* window = nya_window_get(GNY_WINDOW_MAIN);
    u64         scope  = _nya_ui_id(_nya_ui_id((u64)window->handle.index + 1, outer), inner);

    return _nya_ui_id(scope, label);
}

/** Which widget the left button is down on, read between the press and the release that follow it. */
static u64 pressed_widget(void) {
    return _nya_ui_context(nya_window_get(GNY_WINDOW_MAIN))->active;
}

/** A layer with an id and no hooks, standing in for the scenes, which build physics and GPU state. */
static NYA_Layer layer_stub(NYA_ConstCString id) {
    NYA_Layer layer = { .enabled = true };
    (void)snprintf(layer.id, sizeof(layer.id), "%s", id);
    return layer;
}

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
    nya_system_i18n_init();
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
    defer nya_system_i18n_deinit();
    defer nya_system_window_deinit();
    defer nya_world_destroy(engine_world);

    // the parts of gny_world_create the screens reach, without the Lua VM and the systems.
    GNY_World* world = nya_arena_alloc(engine_world->allocator, sizeof(GNY_World));
    *world           = (GNY_World){ .window_main = NYA_WINDOW_HANDLE_NONE, .terrain = NYA_ENTITY_HANDLE_NONE, .terrain_seed = 1 };
    nya_world_user_data_set(world);

    gny_actions_init();
    gny_fonts_register();
    gny_layers_init();
    NYA_EXPECT(nya_i18n_load(NYA_I18N_BASE_LOCALE, NYA_STRING_KEYS, NYA_STRING_COUNT));

    // what assets/config/engine.nya sets for the menus, without watching the file.
    NYA_CONFIG.engine.ui = (NYA_UIStyle){ .font = "menu", .title_font = "menu_title", .body_size = GNY_MENU_ITEM_SIZE, .title_size = GNY_MENU_TITLE_SIZE, .item_height = 42.0F };

    GNY_LAYER_GAME   = layer_stub(GNY_LAYER_GAME_ID);
    GNY_LAYER_UI     = layer_stub(GNY_LAYER_UI_ID);
    GNY_LAYER_CUBE3D = layer_stub(GNY_LAYER_CUBE3D_ID);

    world->window_main = nya_window_create("screens", WINDOW_WIDTH, WINDOW_HEIGHT, NYA_WINDOW_NONE);
    nya_assert(nya_window_is_valid(world->window_main));

    NYA_Window* window    = nya_window_get(GNY_WINDOW_MAIN);
    window->screen_width  = WINDOW_WIDTH;
    window->screen_height = WINDOW_HEIGHT;

    NYA_ConstCString title  = "gny_layer_background gny_layer_main_menu";
    NYA_ConstCString game   = "gny_layer_background gny_layer_game gny_layer_ui";
    NYA_ConstCString paused = "gny_layer_background gny_layer_game gny_layer_ui gny_layer_pause_menu";

    // ── Up and down move through the title screen and wrap at both ends, on either binding, and confirm waits for the barrier.
    {
        stack_reset();

        press(NYA_KEY_DOWN, main_menu);
        press(NYA_KEY_S, main_menu);
        press(NYA_KEY_DOWN, main_menu);
        press(NYA_KEY_W, main_menu);
        nya_check(nya_string_equals(stack(), title) && !nya_app_get()->should_quit, "moving requests nothing, stack '%s'", stack());

        tap(NYA_KEY_RETURN);
        main_menu();
        nya_check(!nya_app_get()->should_quit, "the request waits for the barrier");

        barrier();
        nya_check(nya_app_get()->should_quit, "down, down, down wrapping to the top, then up wrapping to the bottom, is quit");
        nya_app_get()->should_quit = false;

        press(NYA_KEY_SPACE, main_menu);
        nya_check(nya_app_get()->should_quit, "the alternative confirm key activates too");
        nya_app_get()->should_quit = false;

        press(NYA_KEY_ESCAPE, main_menu);
        nya_check(!nya_app_get()->should_quit && nya_string_equals(stack(), title), "cancel on the title screen changes nothing, stack '%s'", stack());

        NYA_Event unbound = key(NYA_KEY_X, true);
        gny_layer_main_menu_on_event(window, &unbound);
        nya_check(unbound.was_handled, "an unbound key is swallowed too, the menu is modal");

        NYA_Event moved = { .type = NYA_EVENT_MOUSE_MOVED };
        gny_layer_main_menu_on_event(window, &moved);
        nya_check(!moved.was_handled, "hover is left for layers below");
    }

    // ── The HUD pauses on the pause action, and while paused leaves it to the menu, which resumes.
    {
        gny_screen_request(GNY_SCREEN_START_GAME);
        barrier();

        tap(NYA_KEY_ESCAPE);
        gny_layer_ui_on_update(window, 0.0F);
        barrier();
        nya_check(nya_string_equals(stack(), paused) && !nya_physics2d_enabled(), "escape pauses, stack '%s'", stack());

        tap(NYA_KEY_ESCAPE);
        gny_layer_ui_on_update(window, 0.0F);
        pause_menu();
        barrier();
        nya_check(nya_string_equals(stack(), game) && nya_physics2d_enabled(), "and escape in the pause menu resumes once, stack '%s'", stack());

        gny_screen_request(GNY_SCREEN_PAUSE);
        barrier();

        NYA_Event held = key(NYA_KEY_ESCAPE, true);
        nya_system_input_handle_event(&held);
        pause_menu();
        barrier();
        nya_check(nya_string_equals(stack(), game), "a pressed escape resumes, stack '%s'", stack());

        gny_screen_request(GNY_SCREEN_PAUSE);
        barrier();

        NYA_Event repeat = key(NYA_KEY_ESCAPE, true);
        repeat.as_key_event.is_repeat = true;
        nya_system_input_handle_event(&repeat);
        pause_menu();
        barrier();
        nya_check(nya_string_equals(stack(), paused), "but the same escape held into the reopened menu does not, stack '%s'", stack());

        NYA_Event released = key(NYA_KEY_ESCAPE, false);
        nya_system_input_handle_event(&released);
        barrier();
    }

    // ── The pause menu's options: volumes step and clamp, the stats toggle flips, and the language row reloads the strings.
    {
        gny_screen_request(GNY_SCREEN_RESUME);
        gny_screen_request(GNY_SCREEN_PAUSE);
        barrier();
        nya_check(nya_string_equals(stack(), paused), "a fresh pause menu, stack '%s'", stack());

        nya_settings_volume_set(NYA_VOLUME_CHANNEL_MUSIC, 0.5F);

        for (u32 i = 0; i < 3; i++) press(NYA_KEY_DOWN, pause_menu);

        press(NYA_KEY_RIGHT, pause_menu);
        nya_check(fabsf(nya_settings_volume(NYA_VOLUME_CHANNEL_MUSIC) - (0.5F + GNY_VOLUME_STEP)) < 1e-4F, "right raises the music by a step, got %f",
                  (f64)nya_settings_volume(NYA_VOLUME_CHANNEL_MUSIC));

        press(NYA_KEY_A, pause_menu);
        press(NYA_KEY_LEFT, pause_menu);
        nya_check(fabsf(nya_settings_volume(NYA_VOLUME_CHANNEL_MUSIC) - (0.5F - GNY_VOLUME_STEP)) < 1e-4F, "left lowers it, on either key, got %f",
                  (f64)nya_settings_volume(NYA_VOLUME_CHANNEL_MUSIC));

        for (u32 i = 0; i < 40; i++) press(NYA_KEY_RIGHT, pause_menu);
        nya_check(nya_settings_volume(NYA_VOLUME_CHANNEL_MUSIC) == 1.0F, "and it clamps at full");
        nya_check(nya_settings_volume(NYA_VOLUME_CHANNEL_MASTER) == 1.0F, "without touching master");

        press(NYA_KEY_RETURN, pause_menu);
        nya_check(nya_string_equals(stack(), paused) && !nya_app_get()->should_quit, "confirm on a volume row requests nothing, stack '%s'", stack());

        press(NYA_KEY_DOWN, pause_menu);
        press(NYA_KEY_RETURN, pause_menu);
        nya_check(world->overlay_enabled, "confirm on stats turns the overlay on");
        press(NYA_KEY_LEFT, pause_menu);
        nya_check(!world->overlay_enabled, "and left turns it off");

        // past the name field to the language row, whose choices sit side by side.
        press(NYA_KEY_DOWN, pause_menu);
        press(NYA_KEY_DOWN, pause_menu);
        press(NYA_KEY_RIGHT, pause_menu);
        press(NYA_KEY_RETURN, pause_menu);
        nya_check(nya_string_equals(nya_i18n_locale(), "de") && nya_string_equals(nya_string_menu_resume(), "fortsetzen"), "picking Deutsch loads it, got '%s'",
                  nya_i18n_locale());

        // every other label changed, and focus kept its place, so left is English again.
        press(NYA_KEY_LEFT, pause_menu);
        press(NYA_KEY_RETURN, pause_menu);
        nya_check(nya_string_equals(nya_i18n_locale(), "en"), "and English loads back, got '%s'", nya_i18n_locale());
    }

    /*
     * ── Confirm requests the row's screen at the barrier.
     *
     * Downward from the top of the pause panel, not upward by wrapping. Wrapping walks backwards through
     * every panel appended after this one, so the count had to be re-derived each time a panel grew a
     * line, and it silently pointed at the wrong row the day the widgets panel was added. Counting from
     * the top only depends on the pause panel's own rows, which is what this test is about.
     */
    {
        gny_screen_request(GNY_SCREEN_RESUME);
        gny_screen_request(GNY_SCREEN_PAUSE);
        barrier();

        // resume, restart, master, music, stats, name, the language row, main menu, and then quit. The
        // two locale choices are one stop, not two: cells in a row share a focus index and left and
        // right move between them.
        const u32 rows_above_quit = 8;

        for (u32 i = 0; i < rows_above_quit; i++) press(NYA_KEY_DOWN, pause_menu);

        tap(NYA_KEY_RETURN);
        pause_menu();
        nya_check(!nya_app_get()->should_quit, "the request waits for the barrier");

        barrier();
        nya_check(nya_app_get()->should_quit, "quit applies at the barrier");
        nya_app_get()->should_quit = false;
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

        nya_check(!nya_input_action_pressed(NYA_INPUT_ACTION_CONFIRM), "with no pad connected nothing is held");

        pause_menu();
        barrier();
        nya_check(nya_string_equals(stack(), paused) && !nya_app_get()->should_quit, "and polling changes nothing, stack '%s'", stack());
    }

    // ── The menus lay out through the registered fonts, and the title is a distance field.
    {
        NYA_Font title_font = nya_font_named("menu_title");
        NYA_Font item_font  = nya_font_named("menu");

        nya_check(nya_font_valid(title_font) && title_font.point_size == GNY_MENU_TITLE_SIZE, "menu_title is registered at the title size");
        nya_check(nya_font_valid(item_font) && item_font.point_size == GNY_MENU_ITEM_SIZE, "menu is registered at the row size");
        nya_check(nya_font_sdf(title_font) && !nya_font_sdf(item_font), "the title asks for a distance field and the rows do not");

        for (u32 i = 0; i < 16 && nya_font_metrics(title_font).line_height * nya_font_metrics(item_font).line_height == 0.0F; i++) {
            nya_event_dispatch((NYA_Event){ .type = NYA_EVENT_FRAME_ENDED });
        }

        nya_check(nya_font_metrics(title_font).line_height > nya_font_metrics(item_font).line_height, "both faces load, the title the larger");
    }

    // ── The pointer on the title screen: hover focuses, a left click on a row activates it, anything else does not.
    {
        stack_reset();

        // one pass to measure the centred panel, then where its third button is from the style and the fonts.
        gny_layer_main_menu_on_render(window);

        NYA_UIStyle style   = nya_ui_style_get(window);
        f32         frame   = style.padding + style.outline;
        f32         header  = ceilf(nya_font_metrics(nya_font_named("menu_title")).line_height) + style.spacing;
        f32         line    = ceilf(nya_font_metrics(nya_font_named("menu")).line_height);
        f32         content = line + (3.0F * (style.item_height + style.spacing));
        f32         top     = (WINDOW_HEIGHT - ((frame * 2.0F) + header + content)) * 0.5F;
        f32x2       quit    = { WINDOW_WIDTH * 0.5F, top + frame + header + line + (style.spacing * 3.0F) + (style.item_height * 2.5F) };

        NYA_Event click = { .type = NYA_EVENT_MOUSE_BUTTON_DOWN };
        gny_layer_main_menu_on_event(window, &click);
        nya_check(click.was_handled, "a click is the menu's, so nothing drops behind it");

        pointer_move(quit);
        pointer_button(NYA_MOUSE_BUTTON_RIGHT, true);
        pointer_button(NYA_MOUSE_BUTTON_RIGHT, false);
        main_menu();
        barrier();
        nya_check(!nya_app_get()->should_quit, "a right click activates nothing");

        pointer_move((f32x2){ WINDOW_WIDTH * 0.5F, WINDOW_HEIGHT - 4.0F });
        pointer_button(NYA_MOUSE_BUTTON_LEFT, true);
        pointer_button(NYA_MOUSE_BUTTON_LEFT, false);
        main_menu();
        barrier();
        nya_check(!nya_app_get()->should_quit && nya_string_equals(stack(), title), "nor does a click off the rows, stack '%s'", stack());

        pointer_move(quit);
        pointer_button(NYA_MOUSE_BUTTON_LEFT, true);
        main_menu();
        barrier();

        pointer_button(NYA_MOUSE_BUTTON_LEFT, false);
        main_menu();
        nya_check(!nya_app_get()->should_quit, "a click waits for the barrier too");

        barrier();
        nya_check(nya_app_get()->should_quit, "a left click on the quit row quits");
        nya_app_get()->should_quit = false;

        press(NYA_KEY_UP, main_menu);
        press(NYA_KEY_RETURN, main_menu);
        nya_check(nya_string_equals(stack(), "gny_layer_background gny_layer_cube3d"), "and the keys carry on from the row the pointer focused, stack '%s'", stack());
    }

    // ── Screen requests rearrange the stack at the barrier, and only from the screen they belong to.
    {
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
        nya_check(nya_string_equals(stack(), paused), "pausing twice pushes one pause menu, stack '%s'", stack());
        nya_check(!nya_physics2d_enabled(), "and stops the solver");

        gny_screen_request(GNY_SCREEN_RESUME);
        nya_system_sim_apply_commands();
        nya_check(nya_string_equals(stack(), game), "resume pops the pause menu, stack '%s'", stack());
        nya_check(nya_physics2d_enabled(), "and restarts the solver");

        u64 seed = world->terrain_seed;

        gny_screen_request(GNY_SCREEN_PAUSE);
        gny_screen_request(GNY_SCREEN_RESTART);
        nya_system_sim_apply_commands();
        nya_check(nya_string_equals(stack(), game), "restart closes the pause menu, stack '%s'", stack());
        nya_check(world->terrain_seed == seed + 1, "and moves to the next terrain seed, got " FMTu64, world->terrain_seed);
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

    /*
     * ── The widgets panel dragged over the pause panel keeps every click it is given.
     *
     * The reported bug, in the menu it was reported in: the panel moved over the others and clicks went
     * through it to whatever was underneath. Both halves are here, because they are two mechanisms: a
     * press on the title bar, which is chrome and belongs to no widget at all, and a press on a widget
     * of the panel, which belongs to that one. Each is checked against the same point with nothing over
     * it first, so a pass cannot mean the point simply reaches nothing.
     */
    {
        stack_reset();
        gny_screen_request(GNY_SCREEN_START_GAME);
        barrier();
        gny_screen_request(GNY_SCREEN_PAUSE);
        barrier();

        // every container lays out from what it measured last pass, so the panels need a few before they stand.
        for (u32 i = 0; i < 4; i++) {
            pause_menu();
            barrier();
        }

        NYA_UIStyle style  = nya_ui_style_get(window);
        f32         header = ceilf(nya_font_metrics(nya_font_named("menu_title")).line_height) + style.spacing;
        f32         frame  = style.padding + style.outline;
        f32         pitch  = style.item_height + style.spacing;

        // the music volume row, fourth down the pause panel: a slider, so pressing it changes no screen.
        NYA_Rectf pause_at = panel_bounds("pause_menu");
        f32x2     target   = { WINDOW_WIDTH * 0.5F, pause_at.y + frame + header + (pitch * 3.0F) + (style.item_height * 0.5F) };

        // what that point reaches with nothing but the pause panel there.
        pointer_move(target);
        pointer_button(NYA_MOUSE_BUTTON_LEFT, true);
        pause_menu();

        u64 beneath = pressed_widget();
        nya_check(beneath != 0, "a row of the pause panel sits under the target point, stack '%s', panel %f %f %f %f", stack(), (f64)pause_at.x, (f64)pause_at.y,
                  (f64)pause_at.width, (f64)pause_at.height);

        pointer_button(NYA_MOUSE_BUTTON_LEFT, false);
        pause_menu();
        barrier();

        // whatever that press started, a field typing included, is not what this is about.
        nya_ui_focus_reset(window);
        pause_menu();
        barrier();

        // by the title bar, four pixels in, to the middle of the window, which puts the bar over that row.
        NYA_Rectf parked = panel_bounds("widgets");
        f32x2     grip   = { parked.x + (parked.width * 0.5F), parked.y + 4.0F };

        nya_check(parked.width > 0.0F && !nya_rect_contains(parked, target), "the widgets panel starts in the corner, clear of that row");

        pointer_move(grip);
        pointer_button(NYA_MOUSE_BUTTON_LEFT, true);
        pause_menu();
        barrier();

        pointer_move(target);
        pause_menu();
        barrier();

        pointer_button(NYA_MOUSE_BUTTON_LEFT, false);
        pause_menu();
        barrier();

        NYA_Rectf moved = panel_bounds("widgets");
        nya_check(nya_rect_contains(moved, target), "dragging its title bar moves it over the pause panel, got %f %f %f %f", (f64)moved.x, (f64)moved.y,
                  (f64)moved.width, (f64)moved.height);

        // a press on the title bar: chrome claims the pointer and hands it to nobody.
        pointer_move(target);
        pointer_button(NYA_MOUSE_BUTTON_LEFT, true);
        pause_menu();

        u64 on_chrome = pressed_widget();

        pointer_button(NYA_MOUSE_BUTTON_LEFT, false);
        pause_menu();
        barrier();

        nya_check(on_chrome == 0, "a press on the dragged panel's title bar activates nothing under it, got " FMTu64, on_chrome);

        // and a press on its first row goes to that row, not to the pause panel's underneath it.
        f32x2 tab = { moved.x + (moved.width * 0.25F), moved.y + frame + header + (style.item_height * 0.5F) };

        pointer_move(tab);
        pointer_button(NYA_MOUSE_BUTTON_LEFT, true);
        pause_menu();

        u64 on_tab = pressed_widget();

        pointer_button(NYA_MOUSE_BUTTON_LEFT, false);
        pause_menu();
        barrier();

        nya_check(on_tab == widget_id("widgets", "pages", nya_string_menu_table()), "a press on its first tab is the tab's, got " FMTu64, on_tab);
        nya_check(on_tab != beneath, "and not the pause panel row it covers");
        nya_check(nya_string_equals(stack(), paused) && !nya_app_get()->should_quit, "and no screen was requested through it, stack '%s'", stack());
    }

    return nya_check_failures() == 0 ? 0 : 1;
}
