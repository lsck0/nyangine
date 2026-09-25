/**
 * @file screens.h
 *
 * Screens are arrangements of the main window's layer stack: the main menu, the 2D game, the pause
 * menu over it, the 3D demo. Changes are requested, then applied at the simulation barrier.
 *
 * ```c
 * if (nya_ui_button(ui, nya_string_menu_resume())) gny_screen_request(GNY_SCREEN_RESUME);
 * ```
 * */
#pragma once

#include "nyangine-core/nyangine.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * SCREENS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/** A rearrangement of the main window's layer stack. See gny_screen_request. */
typedef enum GNY_Screen {
    GNY_SCREEN_NONE = 0,
    GNY_SCREEN_START_GAME,
    GNY_SCREEN_PAUSE,
    GNY_SCREEN_RESUME,
    GNY_SCREEN_RESTART,
    GNY_SCREEN_MAIN_MENU,
    GNY_SCREEN_CUBE3D,

    /** The join request prompt, over whatever is on screen. */
    GNY_SCREEN_SOCIAL_PROMPT,
    GNY_SCREEN_SOCIAL_DISMISS,

    GNY_SCREEN_QUIT,
    GNY_SCREEN_COUNT,
} GNY_Screen;

/** Applies `screen` at the next simulation barrier, since the layer stack may be mid iteration. */
void gny_screen_request(GNY_Screen screen);

/** Whether a menu is on top of the world. */
b8 gny_modal_active(void);
