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
    /*
     * The one publisher gnyame trusts, pinned in the program: the bundled plugins/hello is signed with
     * this key (its plugin.sig), so a default build — which refuses unsigned plugins — loads it and
     * nothing else. Only the public half is here; its secret was drawn on a developer's machine by
     * `./build plugin keygen` and does not ship. Editing hello invalidates its signature on purpose;
     * re-signing means a fresh key, a new plugin.sig, and this literal changed to match. See
     * core_plugin_signature.h, and `./build plugin` for the tool that produced the pair.
     */
    static const NYA_CryptoSignPublicKey nyangine_publisher = {
        .bytes = { 0xa0, 0x6f, 0xed, 0x39, 0xd7, 0x4f, 0x69, 0x08, 0xa9, 0x35, 0x5c, 0x7b, 0x3a, 0xef, 0xfe, 0xc3,
                   0x57, 0xac, 0xd4, 0x14, 0xbd, 0x11, 0x87, 0x1b, 0x8e, 0xd6, 0xe7, 0x9a, 0x18, 0x2e, 0x86, 0x85 },
    };
    NYA_TRY(nya_plugin_trust_key("nyangine", &nyangine_publisher));

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

/*
 * ─────────────────────────────────────────────────────────
 * THE COMMAND LINE
 * ─────────────────────────────────────────────────────────
 *
 * A program here is not "a game" or "a server": it is a CLI whose commands start different sets of
 * the parts above. `gnyame` plays, `gnyame serve` runs headless and serves, `gnyame export` writes the
 * world it would have generated and exits. Every one of them is the same engine with a different list
 * registered, which is what "programs compose" means in practice.
 *
 * The flags are declared here and *interpreted* by net_config.h, one pair at a time through
 * nya_net_config_apply. So the help text below and the meaning of `--tickrate` cannot drift apart:
 * this file says what the flags are called and `net` says what they do.
 */

/** Which command a run is. Play is the zero, because `gnyame` with no argument is the game. */
typedef enum {
    _GNY_COMMAND_PLAY = 0,
    _GNY_COMMAND_SERVE,
    _GNY_COMMAND_EXPORT,
} _GnyCommand;

NYA_INTERNAL _GnyCommand _GNY_COMMAND = _GNY_COMMAND_PLAY;

/** Where `gnyame export` writes, from its positional argument. */
NYA_INTERNAL char _GNY_EXPORT_PATH[256] = { 0 };

/* The launch flags, shared by the commands that can take them. */

/*
 * Every one of these carries a default, which is what makes it optional: a parameter with none is one
 * the parser insists on. The default is never read — `was_matched` decides whether a flag reaches
 * net_config.h at all, so an untouched flag leaves that vocabulary's own default alone.
 */
#define _GNY_FLAG_STRING(variable, flag, text)                                                                                                       \
    NYA_INTERNAL NYA_ArgParameter variable = { .kind          = NYA_ARG_PARAMETER_KIND_FLAG,                                                          \
                                               .value.type    = NYA_TYPE_STRING,                                                                     \
                                               .name          = (flag),                                                                              \
                                               .description   = (text),                                                                                \
                                               .default_value = { .type = NYA_TYPE_STRING, .as_string = (char*)"" } }

#define _GNY_FLAG_NUMBER(variable, flag, text)                                                                                                       \
    NYA_INTERNAL NYA_ArgParameter variable = { .kind          = NYA_ARG_PARAMETER_KIND_FLAG,                                                          \
                                               .value.type    = NYA_TYPE_S64,                                                                        \
                                               .name          = (flag),                                                                              \
                                               .description   = (text),                                                                                \
                                               .default_value = { .type = NYA_TYPE_S64, .as_s64 = 0 } }

_GNY_FLAG_STRING(_gny_flag_connect, "connect", "Join the server at this address instead of playing alone.");
_GNY_FLAG_NUMBER(_gny_flag_port, "port", "The port to reach a server on, or to serve from.");
_GNY_FLAG_NUMBER(_gny_flag_listen, "listen", "Also listen on this port, so friends can join this game.");
_GNY_FLAG_STRING(_gny_flag_name, "name", "What other players see. Remembered from the pause menu otherwise.");
_GNY_FLAG_NUMBER(_gny_flag_max_players, "max-players", "How many may be connected at once.");
_GNY_FLAG_NUMBER(_gny_flag_tickrate, "tickrate", "Fixed updates a second, 10 to 240.");
_GNY_FLAG_STRING(_gny_flag_server_key, "server-key", "The only server key this client will talk to, 64 hex digits.");
_GNY_FLAG_NUMBER(_gny_flag_seed, "seed", "The world to generate. Zero draws one.");
_GNY_FLAG_STRING(_gny_flag_transport, "transport", "udp or steam.");
_GNY_FLAG_NUMBER(_gny_flag_latency, "net-latency", "Simulate this many milliseconds of latency.");
_GNY_FLAG_NUMBER(_gny_flag_jitter, "net-jitter", "Simulate this much jitter, in milliseconds.");
_GNY_FLAG_NUMBER(_gny_flag_loss, "net-loss", "Simulate this percentage of packet loss.");
_GNY_FLAG_NUMBER(_gny_flag_duplicate, "net-duplicate", "Simulate this percentage of duplicated packets.");
_GNY_FLAG_NUMBER(_gny_flag_reorder, "net-reorder", "Simulate this percentage of reordered packets.");

NYA_INTERNAL NYA_ArgParameter _gny_flag_help = { .kind          = NYA_ARG_PARAMETER_KIND_FLAG,
                                                 .value.type    = NYA_TYPE_B8,
                                                 .name          = "help",
                                                 .description   = "Show this message.",
                                                 .default_value = { .type = NYA_TYPE_B8, .as_b8 = false } };

NYA_INTERNAL NYA_ArgParameter _gny_argument_output = {
    .kind        = NYA_ARG_PARAMETER_KIND_POSITIONAL,
    .value.type  = NYA_TYPE_STRING,
    .name        = "output",
    .description = "Where to write the world, as a .nya document.",
    .completion  = { .kind = NYA_ARG_COMPLETION_KIND_FILE },
};

/** Every flag a launch understands, in the order the help lists them. */
#define _GNY_LAUNCH_FLAGS                                                                                                                                &_gny_flag_connect, &_gny_flag_port, &_gny_flag_listen, &_gny_flag_name, &_gny_flag_max_players, &_gny_flag_tickrate, &_gny_flag_server_key,              &_gny_flag_seed, &_gny_flag_transport, &_gny_flag_latency, &_gny_flag_jitter, &_gny_flag_loss, &_gny_flag_duplicate, &_gny_flag_reorder

/*
 * One handler per command, and all they do is say which command this is: the work happens after the
 * engine is up, and a handler runs while nothing has been brought up yet.
 */
NYA_INTERNAL void _gny_handle_play(NYA_ArgCommand* command) {
    nya_unused(command);

    _GNY_COMMAND = _GNY_COMMAND_PLAY;
}

NYA_INTERNAL void _gny_handle_serve(NYA_ArgCommand* command) {
    nya_unused(command);

    _GNY_COMMAND = _GNY_COMMAND_SERVE;
}

NYA_INTERNAL void _gny_handle_export(NYA_ArgCommand* command) {
    nya_unused(command);

    _GNY_COMMAND = _GNY_COMMAND_EXPORT;
}

NYA_INTERNAL NYA_ArgCommand _gny_command_serve = {
    .name        = "serve",
    .description = "Run a dedicated server: no window, no local player, everything else the same.",
    .parameters  = { _GNY_LAUNCH_FLAGS, &_gny_flag_help },
    .handler     = _gny_handle_serve,
};

NYA_INTERNAL NYA_ArgCommand _gny_command_export = {
    .name        = "export",
    .description = "Generate a world and write it out, without opening anything.",
    .parameters  = { &_gny_argument_output, &_gny_flag_seed, &_gny_flag_help },
    .handler     = _gny_handle_export,
};

/*
 * The root carries no description of its own — the parser reserves that for the program's, above —
 * so "what running this with no command does" is said in the parser's description instead.
 */
NYA_INTERNAL NYA_ArgCommand _gny_command_root = {
    .is_root    = true,
    .parameters = { _GNY_LAUNCH_FLAGS, &_gny_flag_help },
    .handler    = _gny_handle_play,
    .subcommands = { &_gny_command_serve, &_gny_command_export },
};

NYA_INTERNAL NYA_ArgParser _gny_parser = {
    .name        = "gnyame",
    // NYA_VERSION rather than VERSION: a check or a test builds this translation unit without the
    // build's own -DVERSION, and "unknown" is a better answer there than a compile error.
    .version     = NYA_VERSION,
    .description = "The demo game, and the reference for how a program is built on nyangine. With no command it plays.",
    .root_command = &_gny_command_root,
};

/**
 * Hands every flag the parser matched to net_config.h, which is where a flag means something.
 *
 * A number is written back out as text because that is the shape the vocabulary takes, and because a
 * flag that is a number here and a string there would be two descriptions of one thing again.
 * */
NYA_INTERNAL void _gnyame_launch_from(const NYA_ArgCommand* command, OUT NYA_NetLaunchConfig* out_launch) {
    *out_launch = nya_net_config_default();

    // A server says so before anything else, because the rest of the resolution depends on it.
    if (_GNY_COMMAND == _GNY_COMMAND_SERVE) (void)nya_net_config_apply(out_launch, "server", nullptr);

    for (u32 index = 0; index < NYA_ARG_MAX_PARAMETERS; index++) {
        const NYA_ArgParameter* parameter = command->parameters[index];
        if (parameter == nullptr) break;

        if (!parameter->was_matched || parameter->kind != NYA_ARG_PARAMETER_KIND_FLAG) continue;
        if (nya_string_equals(parameter->name, "help")) continue;

        char text[NYA_NET_MAX_ADDRESS] = { 0 };

        if (parameter->value.type == NYA_TYPE_STRING) {
            (void)snprintf(text, sizeof(text), "%s", parameter->value.as_string != nullptr ? parameter->value.as_string : "");
        } else {
            (void)snprintf(text, sizeof(text), FMTs64, parameter->value.as_s64);
        }

        (void)nya_net_config_apply(out_launch, parameter->name, text);
    }

    nya_net_config_finish(out_launch);
}

b8 gnyame_init(s32 argc, NYA_CString* argv) {
    _gnyame_loaded = true;

    /*
     * The command line is read first, because it decides what to bring up — which parts are
     * registered, and whether there is a frame at all.
     */
    _gny_parser.executable_name = argv[0];

    NYA_ArgCommand* command = nullptr;
    NYA_Error       parsed  = nya_args_parse(&_gny_parser, argc, argv, &command);

    if (!parsed.ok) {
        (void)fprintf(stderr, "Error: %s\n\n", (NYA_ConstCString)parsed.message);
        nya_args_print_usage(&_gny_parser, command);

        return false;
    }

    if (_gny_flag_help.value.as_b8) {
        nya_args_print_usage(&_gny_parser, command);
        return false;
    }

    // The handler only records which command this is; everything it implies happens below, once the
    // parts it asks for have been brought up.
    NYA_Error chosen = nya_args_run_command(command);

    if (!chosen.ok) {
        (void)fprintf(stderr, "Error: %s\n", (NYA_ConstCString)chosen.message);
        return false;
    }

    if (_GNY_COMMAND == _GNY_COMMAND_EXPORT) {
        if (!_gny_argument_output.was_matched || _gny_argument_output.value.as_string == nullptr) {
            (void)fprintf(stderr, "Error: export needs somewhere to write\n\n");
            nya_args_print_usage(&_gny_parser, command);

            return false;
        }

        (void)snprintf(_GNY_EXPORT_PATH, sizeof(_GNY_EXPORT_PATH), "%s", _gny_argument_output.value.as_string);
    }

    _gnyame_launch_from(command, &_GNY_LAUNCH);

    // An export opens nothing and joins nothing: it is the world generator and a file.
    if (_GNY_COMMAND == _GNY_COMMAND_EXPORT) _GNY_LAUNCH.dedicated = true;

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

    return true;
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * GNYAME RUN
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/**
 * `gnyame export`: the world this launch would have generated, written out and nothing else.
 *
 * The one shot command, and the reason a program here is a CLI rather than a game with a server mode:
 * this brings up exactly the parts that make a world, asks them for it, and returns. There is no
 * frame, no window and no socket, and the same code made the world that a game would have played in.
 * */
NYA_INTERNAL NYA_Error _gnyame_export(NYA_ConstCString path) {
    NYA_Arena* arena = nya_arena_create(.name = "gnyame_export");
    defer nya_arena_destroy(arena);

    /*
     * The ground is generated here rather than waited for: a playing run makes it when the game screen
     * is pushed, and this command pushes nothing. The seed is the launch's, so `--seed` means the same
     * thing to an export as it does to a game.
     */
    gny_terrain_generate(gny_world()->terrain_seed);

    const GNY_World* world = gny_world();

    NYA_Object* document = nya_object_create(arena);

    nya_object_add(document, "seed", (NYA_Value){ .type = NYA_TYPE_U64, .as_u64 = world->terrain_seed });
    nya_object_add(document, "half_width", (NYA_Value){ .type = NYA_TYPE_F32, .as_f32 = GNY_TERRAIN_HALF_WIDTH });
    nya_object_add(document, "step", (NYA_Value){ .type = NYA_TYPE_F32, .as_f32 = GNY_TERRAIN_POINT_STEP });

    /*
     * The ground as the numbers it is, sampled at the step the generator used, rather than whatever
     * the physics body ended up holding: this file is the world, and it must read the same on a
     * machine that never ran the physics.
     */
    NYA_ArrayᐸNYA_Valueᐳ* heights = nya_array_create(arena, NYA_Value);

    for (u32 index = 0; index < GNY_TERRAIN_POINT_COUNT; index++) {
        f32 x = -GNY_TERRAIN_HALF_WIDTH + ((f32)index * GNY_TERRAIN_POINT_STEP);

        // A named value rather than a compound literal in the call: the braces would split the macro's
        // arguments at the comma inside them.
        NYA_Value height = { .type = NYA_TYPE_F32, .as_f32 = nya_terrain2d_height_at(world->terrain2d, x) };

        nya_array_add(heights, height);
    }

    nya_object_add(document, "heights", (NYA_Value){ .type = NYA_TYPE_ARRAY, .as_array = *heights });

    NYA_String* text = nya_serialize(arena, document, NYA_SERDE_FORMAT_NYA, NYA_SERDE_PRETTY);
    if (text == nullptr) return nya_error(NYA_ERROR_NOT_OK, "the world could not be written as a document");

    NYA_File file = { 0 };
    NYA_TRY(nya_file_open(path, NYA_FILE_MODE_WRITE | NYA_FILE_MODE_CREATE | NYA_FILE_MODE_TRUNCATE, &file));
    defer nya_file_close(&file);

    NYA_TRY(nya_file_write_bytes(&file, text->items, text->length));

    nya_log_info("Wrote the world of seed " FMTu64 " to %s, %u points.", world->terrain_seed, path, GNY_TERRAIN_POINT_COUNT);

    return NYA_OK;
}

void gnyame_run(void) {
    // a freshly reloaded DLL starts with zeroed globals: layers and config are rebuilt, the rest lives in the world.
    if (!_gnyame_loaded) {
        gny_layers_init();
        gny_config_attach();
        _gnyame_loaded = true;

        nya_log_debug("Restored the layers and config after a code reload.");
    }

    // The one shot command does its work here rather than in init, because it needs the world that
    // the parts brought up — and then there is no frame to run.
    if (_GNY_COMMAND == _GNY_COMMAND_EXPORT) {
        NYA_Error written = _gnyame_export(_GNY_EXPORT_PATH);

        if (!written.ok) nya_log_error("Could not export: %s", (NYA_ConstCString)written.message);

        // A hot reloading build calls this until the app says it is done, and a one shot command is
        // done after one.
        nya_app_get()->should_quit = true;

        return;
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

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * THE APP ENTRY CONTRACT
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/*
 * The fixed trio the generic hot-reload host resolves in every app DLL (core_app_entry.h). gnyame is
 * the default app; these are thin aliases over its own gnyame_init/run/deinit, so the host loads it by
 * the same symbol names it uses for gnyame-cli or any other app, and nothing in gnyame's internals had
 * to be renamed. A release build links gnyame in and main.c calls gnyame_init directly instead.
 */
b8   nya_app_entry_init(s32 argc, NYA_CString* argv) { return gnyame_init(argc, argv); }
void nya_app_entry_run(void) { gnyame_run(); }
void nya_app_entry_deinit(void) { gnyame_deinit(); }

#include "genyarated/reflection.c"
