#include "gnyame/gnyame.h"

#include "gnyame/config.c"
#include "gnyame/actions.c"
#include "gnyame/entities/entities.c"
#include "gnyame/net.c"
#include "gnyame/sim.c"
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

void gnyame_init(s32 argc, NYA_CString* argv) {
    /*
     * The command line is read first, because it decides what to bring up.
     */
    GNY_LAUNCH = nya_net_config_from_args(argc, argv);

    nya_net_config_report(&GNY_LAUNCH);

    /*
     * The tick rate, from the command line where one was given.
     */
    u64 time_step_ns = nya_time_ms_to_ns(16);

    if (GNY_LAUNCH.tickrate != 0) {
        u32 tickrate = nya_clamp(GNY_LAUNCH.tickrate, 10U, 240U);

        if (tickrate != GNY_LAUNCH.tickrate) nya_log_warn("--tickrate %u is outside 10..240; using %u.", GNY_LAUNCH.tickrate, tickrate);

        time_step_ns = 1'000'000'000ULL / tickrate;
    }

    // The engine reports rather than panics now, so this is the game deciding what a failed startup
    // means. For a game it means stop: there is no sensible fallback for having no GPU. NYA_EXPECT
    // routes the message and a backtrace through the crash sink on the way out.
    NYA_EXPECT(nya_app_init(.time_step_ns = time_step_ns), "while starting the engine");

    // Before the window, because a layer's on_create is entitled to ask what a key is bound to — and
    // before anything reads a volume, since this is where the player's settings are loaded.
    gny_actions_init();

    /*
     * The base locale, and only the base locale.
     */
    NYA_Error localized = nya_i18n_load(NYA_I18N_BASE_LOCALE, NYA_STRING_KEYS, NYA_STRING_COUNT);

    if (!localized.ok) {
        nya_log_error("Could not load the '%s' locale (%s); strings will show their key names.", NYA_I18N_BASE_LOCALE,
                      (NYA_ConstCString)localized.message);
    }

    // Before the window, because the layer stack's on_create reads the world the moment it is
    // pushed — the terrain is generated from there, and it needs somewhere to put its points.
    gny_world_create();

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
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * GNYAME RUN
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

void gnyame_run(void) {
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
    // Before the engine goes down, because writing the settings file needs the save system that
    // nya_app_deinit tears down — and because a crash during teardown should not be the thing that
    // loses a rebound key.
    gny_actions_deinit();

    // Before the engine, because stopping the server despawns player entities and that needs the
    // world — and because a client should say goodbye rather than let the server time it out.
    gny_net_stop();

    nya_app_deinit();

    gny_world_destroy();
}

#include "generated/reflection.c"
