/**
 * @file layer_main_menu.c
 *
 * The title screen. A whole layer made of the shared menu widget: rows, event forwarding, drawing.
 * */
#include "gnyame/gnyame.h"

NYA_INTERNAL const GNY_MenuItem _GNY_MAIN_MENU_ITEMS[] = {
    { .label = nya_string_menu_2d_scene, .screen = GNY_SCREEN_START_GAME },
    { .label = nya_string_menu_3d_scene, .screen = GNY_SCREEN_CUBE3D     },
    { .label = nya_string_menu_quit,     .screen = GNY_SCREEN_QUIT       },
};

// a DLL static, not world state: selection starts on the first row every time the menu opens anyway.
NYA_INTERNAL GNY_Menu _gny_main_menu = {
    .title      = "nyangine",
    .subtitle   = "physics sandbox",
    .items      = _GNY_MAIN_MENU_ITEMS,
    .item_count = (u32)nya_carray_length(_GNY_MAIN_MENU_ITEMS),

    // nothing to go back to, and quitting on the dismiss key would surprise people.
    .on_cancel = GNY_SCREEN_NONE,
};

void gny_layer_main_menu_on_create(NYA_Window* window) {
    nya_unused(window);

    _gny_main_menu.selected = 0;

    // the start or confirm press that opened this menu is still held.
    _gny_main_menu.pad_held = U32_MAX;
}

void gny_layer_main_menu_on_destroy(NYA_Window* window) {
    nya_unused(window);
}

void gny_layer_main_menu_on_event(NYA_Window* window, NYA_Event* event) {
    if (gny_menu_handle_event(window, &_gny_main_menu, event)) event->was_handled = true;
}

void gny_layer_main_menu_on_update(NYA_Window* window, f32 delta_time_s) {
    nya_unused(window, delta_time_s);

    gny_menu_update(&_gny_main_menu);
}

void gny_layer_main_menu_on_render(NYA_Window* window) {
    gny_menu_draw(window, &_gny_main_menu);
}
