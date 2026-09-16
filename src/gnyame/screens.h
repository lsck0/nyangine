/**
 * @file screens.h
 *
 * Screens are arrangements of the main window's layer stack: the main menu, the 2D game, the pause
 * menu over it, the 3D demo. Changes are requested, then applied at the simulation barrier.
 *
 * The menu widget both menus share lives here too, because choosing a row is a screen request. A
 * menu layer only supplies its rows, forwards events and draws.
 *
 * ```c
 * NYA_INTERNAL GNY_MenuItem items[] = {
 *     { .label = nya_string_menu_resume, .screen = GNY_SCREEN_RESUME },
 *     { .label = nya_string_menu_music_volume, .kind = GNY_MENU_ITEM_KIND_VOLUME, .channel = NYA_VOLUME_CHANNEL_MUSIC },
 * };
 *
 * menu = (GNY_Menu){ .title = "paused", .items = items, .item_count = nya_carray_length(items), .on_cancel = GNY_SCREEN_RESUME };
 *
 * // on_event
 * if (gny_menu_handle_event(window, &menu, event)) event->was_handled = true;
 *
 * // on_render
 * gny_menu_draw(window, &menu);
 * ```
 * */
#pragma once

#include "nyangine/nyangine.h"

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
    GNY_SCREEN_QUIT,
    GNY_SCREEN_COUNT,
} GNY_Screen;

/** Applies `screen` at the next simulation barrier, since the layer stack may be mid iteration. */
void gny_screen_request(GNY_Screen screen);

/** Whether a menu is on top of the world. */
b8 gny_modal_active(void);

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * MENUS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

typedef enum GNY_MenuItemKind {
    /** Confirm requests `screen`. */
    GNY_MENU_ITEM_KIND_SCREEN = 0,

    /** Left and right edit the volume of `channel` in place. */
    GNY_MENU_ITEM_KIND_VOLUME,
} GNY_MenuItemKind;

typedef struct GNY_MenuItem {
    /** A generated string accessor, so the label follows the current locale. */
    NYA_ConstCString (*label)(void);

    GNY_MenuItemKind  kind;
    GNY_Screen        screen;
    NYA_VolumeChannel channel;
} GNY_MenuItem;

typedef struct GNY_Menu {
    NYA_ConstCString title;

    /** Drawn under the title in the dim colour. Optional. */
    NYA_ConstCString subtitle;

    const GNY_MenuItem* items;
    u32                 item_count;
    u32                 selected;

    /** What cancel requests. NONE swallows it. */
    GNY_Screen on_cancel;
} GNY_Menu;

/** Where row `index` sits, in window pixels. Drawing and hit testing both use it. */
NYA_Rectf gny_menu_item_bounds(const NYA_Window* window, const GNY_Menu* menu, u32 index);

/** Navigates, edits volumes and requests screens. True when the event belonged to the menu. */
b8 gny_menu_handle_event(const NYA_Window* window, GNY_Menu* menu, const NYA_Event* event);

/** The scrim, the panel, the title and the rows, in screen space. */
void gny_menu_draw(NYA_Window* window, const GNY_Menu* menu);
