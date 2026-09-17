/**
 * The game's menus and movement driven by a virtual gamepad: SDL events in, a menu selection and a walking
 * direction out, with no device attached to the machine.
 **/

#include "nyangine/nyangine.c"
#include "gnyame/gnyame.c"

#include "SDL3/SDL_events.h"
#include "SDL3/SDL_init.h"
#include "SDL3/SDL_joystick.h"

static const GNY_MenuItem items[] = {
    { .label = nya_string_menu_resume,       .screen = GNY_SCREEN_RESUME                                          },
    { .label = nya_string_menu_music_volume, .kind = GNY_MENU_ITEM_KIND_VOLUME, .channel = NYA_VOLUME_CHANNEL_MUSIC },
    { .label = nya_string_menu_quit,         .screen = GNY_SCREEN_QUIT                                            },
};

/** Hands every queued SDL event to the gamepad system, as a frame's event drain does. */
static void pump(void) {
    SDL_PumpEvents();

    SDL_Event event;
    while (SDL_PollEvent(&event)) (void)nya_system_gamepad_handle_sdl_event(&event);
}

static void button(SDL_Joystick* pad, SDL_GamepadButton which, b8 down) {
    nya_assert(SDL_SetJoystickVirtualButton(pad, which, down), "%s", SDL_GetError());
    pump();
}

static void axis(SDL_Joystick* pad, SDL_GamepadAxis which, s16 value) {
    nya_assert(SDL_SetJoystickVirtualAxis(pad, which, value), "%s", SDL_GetError());
    pump();
}

s32 main(void) {
    SDL_SetHintWithPriority(SDL_HINT_AUDIO_DRIVER, "dummy", SDL_HINT_OVERRIDE);

    // the game loads the player's settings over its bindings, so a scratch data directory keeps real ones out.
    NYA_Arena*  scratch   = nya_arena_create(.name = "test_gamepad_menu_scratch");
    NYA_String* temp_root = nullptr;
    NYA_EXPECT(nya_filesystem_temp_directory(scratch, &temp_root));

    NYA_CString data_home = nya_string_to_cstring(scratch, nya_path_join(scratch, nya_string_to_cstring(scratch, temp_root), "gnyame-test-gamepad"));
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
    nya_system_gamepad_init();

    NYA_World* engine_world = nya_world_create();
    (void)nya_world_set(engine_world);

    defer nya_arena_destroy(scratch);
    defer nya_system_save_deinit();
    defer nya_system_settings_deinit();
    defer nya_system_callback_deinit();
    defer nya_system_events_deinit();
    defer nya_system_input_deinit();
    defer nya_system_gamepad_deinit();
    defer nya_world_destroy(engine_world);

    gny_actions_init();

    // the subsystem starts on the second frame.
    nya_system_gamepad_frame_begin();
    nya_system_gamepad_frame_begin();

    SDL_VirtualJoystickDesc description;
    SDL_INIT_INTERFACE(&description);
    description.type     = SDL_JOYSTICK_TYPE_GAMEPAD;
    description.naxes    = SDL_GAMEPAD_AXIS_COUNT;
    description.nbuttons = SDL_GAMEPAD_BUTTON_COUNT;
    description.name     = "virtual pad";

    SDL_JoystickID id = SDL_AttachVirtualJoystick(&description);
    nya_assert(id != 0, "SDL_AttachVirtualJoystick failed: %s", SDL_GetError());

    SDL_Joystick* pad = SDL_OpenJoystick(id);
    nya_assert(pad != nullptr, "SDL_OpenJoystick failed: %s", SDL_GetError());

    defer SDL_DetachVirtualJoystick(id);
    defer SDL_CloseJoystick(pad);

    pump();
    nya_check(nya_gamepad_count() == 1, "the virtual pad connects, got %u", nya_gamepad_count());

    // ── The d-pad and the stick move the selection once per press, and the press that opened the menu is ignored.
    {
        GNY_Menu menu = { .title = "menu", .items = items, .item_count = nya_carray_length(items), .pad_held = U32_MAX };

        button(pad, SDL_GAMEPAD_BUTTON_DPAD_DOWN, true);
        gny_menu_update(&menu);
        nya_check(menu.selected == 0, "a button already down when the menu opened does nothing, got " FMTu32, menu.selected);

        button(pad, SDL_GAMEPAD_BUTTON_DPAD_DOWN, false);
        gny_menu_update(&menu);
        button(pad, SDL_GAMEPAD_BUTTON_DPAD_DOWN, true);
        gny_menu_update(&menu);
        gny_menu_update(&menu);
        nya_check(menu.selected == 1, "a fresh press moves down once however long it is held, got " FMTu32, menu.selected);

        button(pad, SDL_GAMEPAD_BUTTON_DPAD_DOWN, false);
        gny_menu_update(&menu);

        axis(pad, SDL_GAMEPAD_AXIS_LEFTY, 30000);
        gny_menu_update(&menu);
        nya_check(menu.selected == 2, "pushing the stick down moves too, got " FMTu32, menu.selected);

        axis(pad, SDL_GAMEPAD_AXIS_LEFTY, 0);
        gny_menu_update(&menu);

        button(pad, SDL_GAMEPAD_BUTTON_SOUTH, true);
        gny_menu_update(&menu);
        nya_system_sim_apply_commands();
        nya_check(nya_app_get()->should_quit, "south confirms the quit row");

        nya_app_get()->should_quit = false;
        button(pad, SDL_GAMEPAD_BUTTON_SOUTH, false);
    }

    // ── Walking reads the stick and the d-pad through the ordinary action query.
    {
        nya_check(!nya_input_action_pressed(GNY_ACTION_MOVE_LEFT), "nothing held, nothing walks");

        axis(pad, SDL_GAMEPAD_AXIS_LEFTX, -30000);
        nya_check(nya_input_action_pressed(GNY_ACTION_MOVE_LEFT), "the stick walks left");
        nya_check(!nya_input_action_pressed(GNY_ACTION_MOVE_RIGHT), "and not right");

        axis(pad, SDL_GAMEPAD_AXIS_LEFTX, 0);
        button(pad, SDL_GAMEPAD_BUTTON_DPAD_RIGHT, true);
        nya_check(nya_input_action_pressed(GNY_ACTION_MOVE_RIGHT), "the d-pad walks right");
        button(pad, SDL_GAMEPAD_BUTTON_DPAD_RIGHT, false);
    }

    return nya_check_failures() == 0 ? 0 : 1;
}
