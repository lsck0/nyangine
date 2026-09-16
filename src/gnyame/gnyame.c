#include "gnyame/gnyame.h"

#include "gnyame/config.c"
#include "gnyame/actions.c"
#include "gnyame/entities/entities.c"
#include "gnyame/net.c"
#include "gnyame/sim.c"
#include "gnyame/world.c"
#include "gnyame/screens.c"
#include "gnyame/systems/systems.c"
#include "gnyame/layers/layer_background.c"
#include "gnyame/layers/layer_cube3d.c"
#include "gnyame/layers/layer_game.c"
#include "gnyame/layers/layer_main_menu.c"
#include "gnyame/layers/layer_pause_menu.c"
#include "gnyame/layers/layer_ui.c"
#include "gnyame/layers/layers.c"
#include "gnyame/windows.c"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * GNYAME INIT
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/** False in a DLL that gnyame_init has not run in, which after startup means a code reload. */
NYA_INTERNAL b8 _gnyame_loaded = false;

void gnyame_init(s32 argc, NYA_CString* argv) {
    _gnyame_loaded = true;

    /*
     * The command line is read first, because it decides what to bring up.
     */
    NYA_NetLaunchConfig launch = nya_net_config_from_args(argc, argv);

    nya_net_config_report(&launch);

    /*
     * The tick rate, from the command line where one was given.
     */
    u64 time_step_ns = nya_time_ms_to_ns(16);

    if (launch.tickrate != 0) {
        u32 tickrate = nya_clamp(launch.tickrate, 10U, 240U);

        if (tickrate != launch.tickrate) nya_log_warn("--tickrate %u is outside 10..240; using %u.", launch.tickrate, tickrate);

        time_step_ns = 1'000'000'000ULL / tickrate;
    }

    // the engine reports instead of panicking, so the game decides: no GPU means stop. NYA_EXPECT routes
    // the message and a backtrace through the crash sink.
    // Alt-tabbing away drops to GNY_UNFOCUSED_FRAME_RATE instead of drawing at full rate in the
    // background.
    NYA_EXPECT(
        nya_app_init(.time_step_ns = time_step_ns, .unfocused_frame_rate_limit = GNY_UNFOCUSED_FRAME_RATE, .app_id = "gnyame"),
        "while starting the engine"
    );

    // before the window, since a layer's on_create may ask for key bindings, and before anything reads a
    // volume, since this loads the player's settings.
    gny_actions_init();

    /*
     * The base locale, and only the base locale.
     */
    NYA_Error localized = nya_i18n_load(NYA_I18N_BASE_LOCALE, NYA_STRING_KEYS, NYA_STRING_COUNT);

    if (!localized.ok) {
        nya_log_error("Could not load the '%s' locale (%s); strings will show their key names.", NYA_I18N_BASE_LOCALE,
                      (NYA_ConstCString)localized.message);
    }

    // before the window, since the layer stack's on_create generates the terrain into the world.
    gny_world_create(launch);

    gny_sim_init();

    gny_net_start();

    /*
     * A dedicated server stops here: no layers, no window.
     */
    if (nya_net_server_is_dedicated()) {
        nya_log_info("Running headless; no window will be created.");
        return;
    }

    gny_layers_init();
    gny_window_main_create();

    // straight into a scene when asked, so a profile or a smoke run does not have to drive the menu.
    NYA_ConstCString screen = getenv("GNYAME_SCREEN");
    if (screen != nullptr && nya_string_equals(screen, "cube3d")) gny_screen_request(GNY_SCREEN_CUBE3D);
    if (screen != nullptr && nya_string_equals(screen, "game")) gny_screen_request(GNY_SCREEN_START_GAME);
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * GNYAME RUN
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

void gnyame_run(void) {
    // a freshly reloaded DLL starts with zeroed globals: layers and config are rebuilt, the rest lives in the world.
    if (!_gnyame_loaded) {
        gny_layers_init();
        gny_config_attach();
        _gnyame_loaded = true;

        nya_log_debug("Restored the layers and config after a code reload.");
    }

    nya_app_run();
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * GNYAME DEINIT
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

void gnyame_deinit(void) {
    /*
     * The engine first, the world after it.
     */
    // before the engine goes down, since saving settings needs the save system, and a crash in teardown
    // should not lose a rebound key.
    gny_actions_deinit();

    // before the engine, since stopping despawns player entities, and a client should disconnect cleanly
    // rather than time out.
    gny_net_stop();

    nya_app_deinit();

    gny_world_destroy();
}

#include "generated/reflection.c"
