/**
 * The title screen and movement driven by a virtual gamepad: SDL events in, a menu choice and a walking
 * direction out, with no device attached to the machine.
 **/

#include "nyangine-core/nyangine.c"
#include "gnyame/gnyame.c"

#include "SDL3/SDL_events.h"
#include "SDL3/SDL_init.h"
#include "SDL3/SDL_joystick.h"

/** The title screen needs a window only for its size and its UI state. */
static NYA_Window window = { .handle = { .index = 1, .generation = 1 }, .screen_width = 1280, .screen_height = 720 };

/** One update tick of the title screen: its input pass, the barrier, and the edges rolling. */
static void tick(void) {
    gny_layer_main_menu_on_update(&window, 0.0F);
    nya_system_sim_apply_commands();
    nya_event_dispatch((NYA_Event){ .type = NYA_EVENT_UPDATING_ENDED });
    nya_world()->sim_system.tick++;
}

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
    // the game loads the player's settings over its bindings, so a scratch data directory keeps real ones out.
    NYA_Arena*  scratch   = nya_arena_create(.name = "test_gamepad_menu_scratch");
    NYA_String* temp_root = nullptr;
    NYA_EXPECT(nya_filesystem_temp_directory(scratch, &temp_root));

    NYA_CString data_home = nya_string_to_cstring(scratch, nya_path_join(scratch, nya_string_to_cstring(scratch, temp_root), "gnyame-test-gamepad"));
    (void)nya_filesystem_delete_recursive(data_home);

    nya_assert(nya_host_environment_add("XDG_DATA_HOME", data_home));
    nya_assert(nya_host_environment_add("APPDATA", data_home));

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

    // The d-pad and the stick move focus once per press, a press held from before the menu does not, and south confirms.
    {
        button(pad, SDL_GAMEPAD_BUTTON_DPAD_DOWN, true);
        nya_event_dispatch((NYA_Event){ .type = NYA_EVENT_UPDATING_ENDED });

        gny_layer_main_menu_on_create(&window);
        tick();

        button(pad, SDL_GAMEPAD_BUTTON_DPAD_DOWN, false);
        tick();

        // held over two ticks, and a zero tick length, so no repeat.
        button(pad, SDL_GAMEPAD_BUTTON_DPAD_DOWN, true);
        tick();
        tick();

        button(pad, SDL_GAMEPAD_BUTTON_DPAD_DOWN, false);
        tick();

        axis(pad, SDL_GAMEPAD_AXIS_LEFTY, 30000);
        tick();

        axis(pad, SDL_GAMEPAD_AXIS_LEFTY, 0);
        tick();
        nya_check(!nya_app_get()->should_quit, "moving requests nothing");

        // the held press was ignored and the fresh one moved once, so the stick lands on quit rather than past it.
        button(pad, SDL_GAMEPAD_BUTTON_SOUTH, true);
        tick();
        nya_check(nya_app_get()->should_quit, "south confirms the quit row");

        nya_app_get()->should_quit = false;
        button(pad, SDL_GAMEPAD_BUTTON_SOUTH, false);
        tick();
    }

    // Walking reads the stick and the d-pad through the ordinary action query.
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
