/**
 * @file layer_pause_menu.c
 *
 * The pause menu, pushed on top of a running game and popped to resume.
 *
 * Pushed above the HUD rather than in place of it, so the counters stay readable while the world is
 * stopped — which is most of what pausing a demo like this is for. The solver is stopped by the
 * screen change in layers.c, not here, so "paused" cannot come to mean two different things.
 *
 * Escape both opens and closes it: the HUD layer turns escape into gny_screen_pause while playing,
 * and this layer sits above the HUD once it is up, so it sees the next escape first.
 * */
#include "gnyame/gnyame.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * ITEMS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

NYA_INTERNAL GNY_MenuItem _gny_pause_menu_items[] = {
    { .action = GNY_MENU_ACTION_RESUME    },
    { .action = GNY_MENU_ACTION_RESTART   },

    /*
     * The options screen, such as it is.
     *
     * Two rows rather than a submenu, because two settings do not justify one — and because a volume
     * a player cannot hear while they set it is a volume they set twice. Edited with left and right;
     * nya_settings_volume_set writes straight through, and gny_actions_deinit persists the result.
     */
    { .kind = GNY_MENU_ITEM_KIND_VOLUME, .channel = NYA_VOLUME_CHANNEL_MASTER },
    { .kind = GNY_MENU_ITEM_KIND_VOLUME, .channel = NYA_VOLUME_CHANNEL_MUSIC  },

    { .action = GNY_MENU_ACTION_MAIN_MENU },
    { .action = GNY_MENU_ACTION_QUIT      },
};

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * ON CREATE
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

void gny_layer_pause_menu_on_create(NYA_Window* window) {
    nya_unused(window);

    GNY_World* world = gny_world();

    // Rebuilt on every push, for the reason spelled out in layer_main_menu.c: the item array lives
    // in this library and a hot reload replaces it, while GNY_World outlives the reload.
    // Translated, and re-read on every push for the same reason the main menu's are. See its on_create.
    _gny_pause_menu_items[0].label = nya_string_menu_resume();
    _gny_pause_menu_items[1].label = nya_string_menu_restart();
    _gny_pause_menu_items[2].label = nya_string_menu_master_volume();
    _gny_pause_menu_items[3].label = nya_string_menu_music_volume();
    _gny_pause_menu_items[4].label = nya_string_menu_main_menu();
    _gny_pause_menu_items[5].label = nya_string_menu_quit();

    world->pause_menu = (GNY_Menu){
        .title      = "paused",
        .items      = _gny_pause_menu_items,
        .item_count = (u32)(sizeof(_gny_pause_menu_items) / sizeof(_gny_pause_menu_items[0])),

        // On "resume", so escape-then-enter is the fastest way back into the game.
        .selected = 0,
    };
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * ON DESTROY
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

void gny_layer_pause_menu_on_destroy(NYA_Window* window) {
    nya_unused(window);

    // The solver is restarted by whichever screen change popped this layer, not here. A pop that
    // meant "go to the main menu" wants it left alone, and on_destroy cannot tell the two apart.
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * ON EVENT
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

void gny_layer_pause_menu_on_event(NYA_Window* window, NYA_Event* event) {
    GNY_World* world = gny_world();

    // Cancel closes what pause opened. Taken before the shared handler, which leaves cancel alone
    // precisely so each menu can answer it for itself.
    if (event->type == NYA_EVENT_KEY_DOWN && !event->as_key_event.is_repeat
        && nya_input_action_matches(NYA_INPUT_ACTION_CANCEL, event->as_key_event.key, event->as_key_event.modifier_flags)) {
        gny_screen_resume();
        event->was_handled = true;
        return;
    }

    GNY_MenuAction action;
    if (gny_menu_handle_event(window, &world->pause_menu, event, &action)) event->was_handled = true;

    switch (action) {
        case GNY_MENU_ACTION_RESUME: {
            gny_screen_resume();
            event->was_handled = true;
        } break;

        case GNY_MENU_ACTION_RESTART: {
            gny_screen_restart();
            event->was_handled = true;
        } break;

        case GNY_MENU_ACTION_MAIN_MENU: {
            gny_screen_main_menu();
            event->was_handled = true;
        } break;

        case GNY_MENU_ACTION_QUIT: {
            gny_screen_quit();
            event->was_handled = true;
        } break;

        default: break;
    }
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * ON UPDATE
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

void gny_layer_pause_menu_on_update(NYA_Window* window, f32 delta_time_s) {
    nya_unused(window, delta_time_s);
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * ON RENDER
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

void gny_layer_pause_menu_on_render(NYA_Window* window) {
    gny_menu_draw(window, &gny_world()->pause_menu);
}
