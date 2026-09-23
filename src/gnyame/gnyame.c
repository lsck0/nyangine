#include "gnyame/gnyame.h"

#include "gnyame/config.c"
#include "gnyame/actions.c"
#include "gnyame/entities/entities.c"
#include "gnyame/guild.c"
#include "gnyame/net.c"
#include "gnyame/social.c"
#include "gnyame/sim.c"
#include "gnyame/robots.c"
#include "gnyame/web.c"
#include "gnyame/world.c"
#include "gnyame/screens.c"
#include "gnyame/systems/systems.c"
#include "gnyame/layers/layer_background.c"
#include "gnyame/layers/layer_cube3d.c"
#include "gnyame/layers/layer_cube3d_features.c"
#include "gnyame/layers/layer_cube3d_stones.c"
#include "gnyame/layers/layer_game.c"
#include "gnyame/layers/layer_main_menu.c"
#include "gnyame/layers/layer_pause_menu.c"
#include "gnyame/layers/layer_social.c"
#include "gnyame/layers/layer_ui.c"
#include "gnyame/layers/layers.c"
#include "gnyame/windows.c"
// after the layers, whose stack it reads, and the screens it asks for.
#include "gnyame/agent.c"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * GNYAME INIT
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/** False in a DLL that gnyame_init has not run in, which after startup means a code reload. */
NYA_INTERNAL b8 _gnyame_loaded = false;

/**
 * What the command line asked for, read before the engine starts because it decides which parts are
 * registered at all. The world keeps the copy everything else reads, as GNY_LAUNCH.
 * */
NYA_INTERNAL NYA_NetLaunchConfig _GNY_LAUNCH = { 0 };

/*
 * ─────────────────────────────────────────────────────────
 * THE PARTS
 * ─────────────────────────────────────────────────────────
 *
 * Everything gnyame brings up is a system in the engine's registry rather than a call in a hand
 * written startup sequence. That buys three things worth the ceremony: the order is one order, shared
 * with the engine's own subsystems and printed by nya_system_registry_report; teardown is the reverse
 * of it and cannot drift from bring-up; and what a part needs is declared, so a part that wants a
 * window and has none is refused at startup by name.
 *
 * It is also what makes "a dedicated server" stop being a mode. A server is this program with the
 * drawing parts not registered, which is the `if` in _gnyame_parts below and nothing else.
 */

NYA_Error gny_part_actions_init(void) {
    gny_actions_init();

    // the name typed in the pause menu last time, unless the command line gives one. Here rather than
    // beside the parse, because it reads the settings this part just loaded.
    if (!_GNY_LAUNCH.named && nya_settings_player_name()[0] != '\0') {
        (void)snprintf(_GNY_LAUNCH.name, sizeof(_GNY_LAUNCH.name), "%s", nya_settings_player_name());
    }

    nya_net_config_report(&_GNY_LAUNCH);

    return NYA_OK;
}

void gny_part_actions_deinit(void) { gny_actions_deinit(); }

NYA_Error gny_part_locale_init(void) {
    // The base locale, and only the base locale. A missing one is not worth refusing to start over:
    // every string falls back to its key, which is readable enough to fix it from.
    NYA_Error localized = nya_i18n_load(NYA_I18N_BASE_LOCALE, NYA_STRING_KEYS, NYA_STRING_COUNT);

    if (!localized.ok) {
        nya_log_error("Could not load the '%s' locale (%s); strings will show their key names.", NYA_I18N_BASE_LOCALE,
                      (NYA_ConstCString)localized.message);
    }

    return NYA_OK;
}

NYA_Error gny_part_world_init(void) {
    gny_world_create(_GNY_LAUNCH);
    gny_sim_init();

    return NYA_OK;
}

/*
 * No `deinit` pair: the world outlives the registry on purpose. A layer's on_destroy reads it, and
 * layers go down with the window, which is an engine subsystem and therefore torn down after every
 * game part. gnyame_deinit destroys the world once nya_app_deinit has returned.
 */

NYA_Error gny_part_plugins_init(void) {
    // after the world, because a plugin's `on_load` spawns entities into it, and before the layers, so
    // a plugin's systems are registered while the schedule is still being built. Refusals are reported
    // per plugin and never stop the game: `plugins/` is somebody else's code.
    return nya_plugin_load_all();
}

NYA_Error gny_part_net_init(void) {
    gny_net_start();
    return NYA_OK;
}

void gny_part_net_deinit(void) { gny_net_stop(); }

NYA_Error gny_part_web_init(void) {
    gny_web_start();
    return NYA_OK;
}

void gny_part_web_deinit(void) { gny_web_stop(); }

NYA_Error gny_part_layers_init(void) {
    gny_layers_init();
    return NYA_OK;
}

NYA_Error gny_part_window_init(void) {
    gny_window_main_create();
    return NYA_OK;
}

NYA_Error gny_part_social_init(void) {
    gny_social_start();
    return NYA_OK;
}

void gny_part_social_deinit(void) { gny_social_stop(); }

NYA_Error gny_part_screen_init(void) {
    // straight into a scene when asked, so a profile or a smoke run does not have to drive the menu.
    NYA_ConstCString screen = getenv("GNYAME_SCREEN");

    if (screen != nullptr && nya_string_equals(screen, "cube3d")) gny_screen_request(GNY_SCREEN_CUBE3D);
    if (screen != nullptr && nya_string_equals(screen, "game")) gny_screen_request(GNY_SCREEN_START_GAME);

    return NYA_OK;
}

/**
 * Which parts this run is made of. Called by the engine once its own systems are registered and before
 * any of them is brought up; see NYA_AppOptions.parts.
 * */
NYA_INTERNAL void _gnyame_parts(void) {
    NYA_SystemOwner owner = { .kind = NYA_SYSTEM_OWNER_GAME };

    nya_system_register((NYA_SystemEntry){ .name   = "gnyame_actions",
                                           .init   = nya_callback(gny_part_actions_init),
                                           .deinit = nya_callback(gny_part_actions_deinit),
                                           .owner  = owner });

    nya_system_register((NYA_SystemEntry){ .name = "gnyame_locale", .after = "gnyame_actions", .init = nya_callback(gny_part_locale_init), .owner = owner });

    nya_system_register((NYA_SystemEntry){ .name = "gnyame_world", .after = "gnyame_locale", .init = nya_callback(gny_part_world_init), .owner = owner });

    nya_system_register((NYA_SystemEntry){ .name = "gnyame_plugins", .after = "gnyame_world", .init = nya_callback(gny_part_plugins_init), .owner = owner });

    nya_system_register((NYA_SystemEntry){ .name   = "gnyame_net",
                                           .after  = "gnyame_plugins",
                                           .init   = nya_callback(gny_part_net_init),
                                           .deinit = nya_callback(gny_part_net_deinit),
                                           .owner  = owner });

    // after the systems and the world exist, so the first request already describes a running program
    // rather than a half built one. It binds nothing unless GNYAME_WEB_PORT says otherwise, which is
    // why it is registered in both runs and why it claims no listener here.
    nya_system_register((NYA_SystemEntry){ .name   = "gnyame_web",
                                           .after  = "gnyame_net",
                                           .init   = nya_callback(gny_part_web_init),
                                           .deinit = nya_callback(gny_part_web_deinit),
                                           .owner  = owner });

    /*
     * A dedicated server stops here: no layers, no window, nobody to show a join prompt to. Not a mode
     * flag read in five places, just four registrations that do not happen.
     */
    if (_GNY_LAUNCH.dedicated) return;

    // before the window, since a layer's on_create may ask for key bindings and spawn into the world.
    nya_system_register((NYA_SystemEntry){ .name = "gnyame_layers", .after = "gnyame_web", .init = nya_callback(gny_part_layers_init), .owner = owner });

    nya_system_register((NYA_SystemEntry){ .name     = "gnyame_window",
                                           .after    = "gnyame_layers",
                                           .init     = nya_callback(gny_part_window_init),
                                           .needs    = NYA_SYSTEM_FACILITY_GPU,
                                           .owner    = owner });

    // after the window, since a join request pushes a layer onto it. A machine with no display never
    // gets here, and saying so is what stops the prompt being pushed onto nothing.
    nya_system_register((NYA_SystemEntry){ .name   = "gnyame_social",
                                           .after  = "gnyame_window",
                                           .init   = nya_callback(gny_part_social_init),
                                           .deinit = nya_callback(gny_part_social_deinit),
                                           .needs  = NYA_SYSTEM_FACILITY_WINDOW,
                                           .owner  = owner });

    nya_system_register((NYA_SystemEntry){ .name  = "gnyame_screen",
                                           .after = "gnyame_social",
                                           .init  = nya_callback(gny_part_screen_init),
                                           .needs = NYA_SYSTEM_FACILITY_WINDOW,
                                           .owner = owner });
}

void gnyame_init(s32 argc, NYA_CString* argv) {
    _gnyame_loaded = true;

    /*
     * The command line is read first, because it decides what to bring up.
     */
    _GNY_LAUNCH = nya_net_config_from_args(argc, argv);

    /*
     * The tick rate, from the command line where one was given.
     */
    u64 time_step_ns = nya_time_ms_to_ns(16);

    if (_GNY_LAUNCH.tickrate != 0) {
        u32 tickrate = nya_clamp(_GNY_LAUNCH.tickrate, 10U, 240U);

        if (tickrate != _GNY_LAUNCH.tickrate) nya_log_warn("--tickrate %u is outside 10..240; using %u.", _GNY_LAUNCH.tickrate, tickrate);

        time_step_ns = 1'000'000'000ULL / tickrate;
    }

    // the engine reports instead of panicking, so the game decides: no GPU means stop. NYA_EXPECT routes
    // the message and a backtrace through the crash sink.
    // Alt-tabbing away drops to GNY_UNFOCUSED_FRAME_RATE instead of drawing at full rate in the
    // background.
    // `--server` is headless by definition: it opens no window, so a machine with no GPU backend is a
    // correct place to run one and the renderer is skipped instead of failing the start.
    // Everything gnyame itself brings up is registered by `parts` and brought up in one order with the
    // engine's own subsystems, which is what this call now returns from: a running program.
    NYA_EXPECT(
        nya_app_init(
            .time_step_ns               = time_step_ns,
            .unfocused_frame_rate_limit = GNY_UNFOCUSED_FRAME_RATE,
            .app_id                     = "gnyame",
            .steam_app_id               = GNY_STEAM_APP_ID,
            .headless                   = _GNY_LAUNCH.dedicated,
            .parts                      = _gnyame_parts
        ),
        "while starting the engine"
    );

    if (_GNY_LAUNCH.dedicated) nya_log_info("Running headless; no window will be created.");
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
     * Every part goes down inside nya_app_deinit, in the reverse of the order it came up in, which is
     * the whole reason for registering them: the reasons each one had to come down before the next —
     * the friends list stops offering "join game" before the port closes, the server unhooks from an
     * event system that is still there — are the same reasons it came up after it, and they are now
     * stated once instead of twice.
     */

    // while the job system and the save root are still up: it waits for training, then saves. Not a
    // part, because nothing starts it: a robot is created by the game asking for one.
    gny_robots_destroy();

    nya_app_deinit();

    // last, and outside the registry: a layer's on_destroy reads the world, and layers go down with the
    // window, which is an engine subsystem and therefore torn down after every part above.
    gny_world_destroy();
}

#include "genyarated/reflection.c"
