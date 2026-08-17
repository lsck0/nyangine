/**
 * @file layer_main_menu.c
 *
 * The title screen, and the only thing over the background when the process starts.
 *
 * There is no world behind it. gny_window_main_create pushes the background and this, and "start"
 * pops this and pushes the game and the HUD in its place — so the terrain, the crates and the audio
 * do not exist until they are asked for, and going back to the menu takes them away again.
 *
 * The menu itself is the shared widget in layers.c. This file is the item list and what choosing one
 * means, which is all a menu layer should be.
 * */
#include "gnyame/gnyame.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * ITEMS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/*
 * Three items: the two scenes and the way out.
 *
 * Not const, and the labels are not here, because they are translated — nya_string_menu_2d_scene() is
 * a function call and a static initialiser cannot contain one. They are filled in on_create, which
 * already rebuilds this array on every push for an unrelated reason and is therefore also the point at
 * which a language change is picked up.
 *
 * The volume rows that used to sit between these moved out entirely. They are still on the pause menu,
 * where a player who is actually listening to something can reach them; having them here as well made
 * the title screen a settings screen with two scenes attached.
 */
NYA_INTERNAL GNY_MenuItem _gny_main_menu_items[] = {
    { .action = GNY_MENU_ACTION_START  },
    { .action = GNY_MENU_ACTION_CUBE3D },
    { .action = GNY_MENU_ACTION_QUIT   },
};

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * ON CREATE
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

void gny_layer_main_menu_on_create(NYA_Window* window) {
    nya_unused(window);

    GNY_World* world = gny_world();

    /*
     * Rebuilt on every push rather than once at startup.
     *
     * The item array is a pointer into this shared library, and a hot reload replaces the library —
     * so a pointer stored before the reload aims into an unmapped page afterwards. GNY_World lives
     * in the executable and survives, which is exactly what makes the stale pointer possible.
     * Re-pointing it here costs nothing and removes the whole class of problem.
     */
    /*
     * The labels, every time the menu is pushed.
     *
     * A no-argument accessor answers a pointer into the locale's own table, which is stable until the
     * language changes or the file is hot reloaded — and both of those replace the table, which is what
     * makes re-reading them here rather than caching them the correct thing to do.
     */
    _gny_main_menu_items[0].label = nya_string_menu_2d_scene();
    _gny_main_menu_items[1].label = nya_string_menu_3d_scene();
    _gny_main_menu_items[2].label = nya_string_menu_quit();

    world->main_menu = (GNY_Menu){
        .title      = "nyangine",
        .subtitle   = "physics sandbox",
        .items      = _gny_main_menu_items,
        .item_count = (u32)(sizeof(_gny_main_menu_items) / sizeof(_gny_main_menu_items[0])),

        // Deliberately not carried over from the last time the menu was open. Coming back from a
        // game should land on "start", not on whatever was picked to leave it.
        .selected = 0,
    };
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * ON DESTROY
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

void gny_layer_main_menu_on_destroy(NYA_Window* window) {
    nya_unused(window);
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * ON EVENT
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

void gny_layer_main_menu_on_event(NYA_Window* window, NYA_Event* event) {
    GNY_World* world = gny_world();

    GNY_MenuAction action;
    if (gny_menu_handle_event(window, &world->main_menu, event, &action)) event->was_handled = true;

    switch (action) {
        case GNY_MENU_ACTION_START: {
            gny_screen_start_game();
            event->was_handled = true;
        } break;

        case GNY_MENU_ACTION_CUBE3D: {
            gny_screen_cube3d();
            event->was_handled = true;
        } break;

        case GNY_MENU_ACTION_QUIT: {
            gny_screen_quit();
            event->was_handled = true;
        } break;

        default: break;
    }

    // Escape does nothing here, and is swallowed rather than passed down.
    //
    // At the root of the menu tree there is nothing to go back to, and the obvious alternative —
    // treating it as quit — puts "leave the program" on the key people press to dismiss things.
    // Quitting stays an explicit item.
    if (event->type == NYA_EVENT_KEY_DOWN
        && nya_input_action_matches(NYA_INPUT_ACTION_CANCEL, event->as_key_event.key, event->as_key_event.modifier_flags)) {
        event->was_handled = true;
    }
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * ON UPDATE
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

void gny_layer_main_menu_on_update(NYA_Window* window, f32 delta_time_s) {
    nya_unused(window, delta_time_s);
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * ON RENDER
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

void gny_layer_main_menu_on_render(NYA_Window* window) {
    // Screen space. Nothing below this ever leaves a camera set — the game layer resets its own at
    // the end of its render — so there is nothing to undo here.
    gny_menu_draw(window, &gny_world()->main_menu);
}
