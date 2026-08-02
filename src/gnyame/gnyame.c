#include "gnyame/gnyame.h"

#include "gnyame/layers/layer_game.c"
#include "gnyame/layers/layer_menu.c"
#include "gnyame/layers/layer_ui.c"
#include "gnyame/layers/layers.c"
#include "gnyame/windows.c"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * GNYAME INIT
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

void gnyame_init(s32 argc, NYA_CString* argv) {
    nya_unused(argc, argv);

    // The engine reports rather than panics now, so this is the game deciding what a failed startup
    // means. For a game it means stop: there is no sensible fallback for having no GPU. NYA_EXPECT
    // routes the message and a backtrace through the crash sink on the way out.
    NYA_EXPECT(nya_app_init(), "while starting the engine");

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
    nya_app_deinit();
}
