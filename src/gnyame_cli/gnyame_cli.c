/**
 * @file gnyame_cli.c
 *
 * A second app on the same engine, kept deliberately tiny: the point is to prove a project builds more
 * than one hot-reloadable binary, not to be a second game. It brings the engine up headless, ticks a
 * heartbeat system for a while, and exits.
 *
 * It exports the app entry contract (core_app_entry.h) and nothing else, so the generic host in
 * src/main.c loads it exactly as it loads gnyame — a binary named `gnyame-cli.debug` finds
 * `gnyame-cli.debug.so` beside it and reloads it on change, with the reload machinery unchanged from
 * gnyame's. Build and run it with `./build run gnyame-cli`; the default `./build run` is still gnyame.
 * */
#include "nyangine/nyangine.h"

// Bump this, rebuild the DLL while the app is running, and the reloaded image logs the new text: the
// visible proof that a non-gnyame app hot-reloads through the same host. See nya_app_entry_run.
#define GNY_CLI_BANNER "gnyame-cli"

// ~20 seconds at the 10 fps cap below, then it quits on its own: long enough to swap the DLL under a
// running instance, short enough that `./build run gnyame-cli` returns without needing a Ctrl-C.
#define GNY_CLI_MAX_FRAMES 200

// A DLL global, so a code reload zeroes it and the heartbeat count simply restarts — which is the honest
// demonstration that reloaded state lives in the engine world, not here. See gnyame.h on hot reload.
NYA_INTERNAL u32 _gny_cli_frames = 0;

/**
 * The one system this app registers: a heartbeat that also decides when the run is done.
 *
 * Exported, not NYA_INTERNAL: a system holds its callbacks by name and the host re-resolves them with
 * dlsym against the reloaded image (core_system.h, HOT RELOAD), so a static callback would vanish on
 * the first reload — the same reason gnyame declares its callbacks in gnyame.h. gnyame gives its
 * callbacks a header declaration to say so; a one-file app says it here instead, so clang-tidy's
 * "make it static" is answered rather than obeyed.
 * */
void gny_cli_heartbeat(f32 delta_time_s); // NOLINT(misc-use-internal-linkage): resolved by dlsym on reload, must stay external

void gny_cli_heartbeat(f32 delta_time_s) { // NOLINT(misc-use-internal-linkage): see above
    nya_unused(delta_time_s);

    _gny_cli_frames++;

    if (_gny_cli_frames % 10 == 0) nya_log_info("%s: heartbeat %u/%u.", GNY_CLI_BANNER, _gny_cli_frames, (u32)GNY_CLI_MAX_FRAMES);

    if (_gny_cli_frames >= GNY_CLI_MAX_FRAMES) nya_app_get()->should_quit = true;
}

/** Registered by name so the handle survives a code reload, exactly as gnyame's parts are. */
NYA_INTERNAL void gny_cli_parts(void) {
    nya_system_register((NYA_SystemEntry){ .name = "gnyame_cli_heartbeat", .frame = nya_callback(gny_cli_heartbeat) });
}

b8 nya_app_entry_init(s32 argc, NYA_CString* argv) {
    nya_unused(argc, argv);

    nya_log_info("%s up: a second app sharing gnyame's engine and hot-reload host.", GNY_CLI_BANNER);

    // Headless: no window and no GPU, so this runs anywhere. The unfocused cap is the frame limiter for
    // a run that never has a focused window, keeping the heartbeat at a readable rate instead of a spin.
    NYA_EXPECT(
        nya_app_init(
            .headless                   = true,
            .app_id                     = GNY_CLI_BANNER,
            .unfocused_frame_rate_limit = 10,
            .parts                      = gny_cli_parts
        ),
        "while starting %s", GNY_CLI_BANNER
    );

    return true;
}

void nya_app_entry_run(void) { nya_app_run(); }

void nya_app_entry_deinit(void) { nya_app_deinit(); }
