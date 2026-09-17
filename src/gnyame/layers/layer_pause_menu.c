/**
 * @file layer_pause_menu.c
 *
 * The pause menu over a stopped 2D world, with the volume rows as its options screen.
 * */
#include "gnyame/gnyame.h"

NYA_INTERNAL const GNY_MenuItem _GNY_PAUSE_MENU_ITEMS[] = {
    { .label = nya_string_menu_resume,        .screen = GNY_SCREEN_RESUME                                       },
    { .label = nya_string_menu_restart,       .screen = GNY_SCREEN_RESTART                                      },
    { .label = nya_string_menu_master_volume, .kind = GNY_MENU_ITEM_KIND_VOLUME, .channel = NYA_VOLUME_CHANNEL_MASTER },
    { .label = nya_string_menu_music_volume,  .kind = GNY_MENU_ITEM_KIND_VOLUME, .channel = NYA_VOLUME_CHANNEL_MUSIC  },
    { .label = nya_string_menu_main_menu,     .screen = GNY_SCREEN_MAIN_MENU                                    },
    { .label = nya_string_menu_quit,          .screen = GNY_SCREEN_QUIT                                         },
};

NYA_INTERNAL GNY_Menu _gny_pause_menu = {
    .title      = "paused",
    .items      = _GNY_PAUSE_MENU_ITEMS,
    .item_count = (u32)nya_carray_length(_GNY_PAUSE_MENU_ITEMS),
    .on_cancel  = GNY_SCREEN_RESUME,
};

void gny_layer_pause_menu_on_create(NYA_Window* window) {
    nya_unused(window);

    // on "resume", so escape then enter is the quickest way back.
    _gny_pause_menu.selected = 0;

    // the start or confirm press that opened this menu is still held.
    _gny_pause_menu.pad_held = U32_MAX;
}

void gny_layer_pause_menu_on_destroy(NYA_Window* window) {
    // the solver is restarted by the screen change that popped this layer, which knows whether it should be.
    nya_unused(window);
}

void gny_layer_pause_menu_on_event(NYA_Window* window, NYA_Event* event) {
    if (gny_menu_handle_event(window, &_gny_pause_menu, event)) event->was_handled = true;
}

void gny_layer_pause_menu_on_update(NYA_Window* window, f32 delta_time_s) {
    nya_unused(window, delta_time_s);

    gny_menu_update(&_gny_pause_menu);
}

void gny_layer_pause_menu_on_render(NYA_Window* window) {
    gny_menu_draw(window, &_gny_pause_menu);
}
