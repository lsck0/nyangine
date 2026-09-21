/**
 * @file layer_main_menu.c
 *
 * The title screen: one panel of buttons over the background. The same function reads input on update and draws
 * on render, see ui.h.
 * */
#include "gnyame/gnyame.h"

NYA_INTERNAL void _gny_main_menu(NYA_Window* window, NYA_UIPass pass);
NYA_INTERNAL void _gny_main_menu_build_stamp(NYA_UI* ui, NYA_Window* window);

void gny_layer_main_menu_on_create(NYA_Window* window) {
    nya_ui_focus_reset(window);
}

void gny_layer_main_menu_on_destroy(NYA_Window* window) {
    nya_unused(window);
}

void gny_layer_main_menu_on_event(NYA_Window* window, NYA_Event* event) {
    nya_unused(window);

    (void)nya_ui_modal_event(event);
}

void gny_layer_main_menu_on_update(NYA_Window* window, f32 delta_time_s) {
    nya_unused(delta_time_s);

    _gny_main_menu(window, NYA_UI_PASS_INPUT);
}

void gny_layer_main_menu_on_render(NYA_Window* window) {
    _gny_main_menu(window, NYA_UI_PASS_DRAW);
}

void _gny_main_menu(NYA_Window* window, NYA_UIPass pass) {
    NYA_UI* ui = gny_ui_begin(window, pass);
    nya_ui_scrim(ui);

    NYA_UIPanel panel = { .anchor = NYA_UI_ANCHOR_CENTER, .width = nya_ui_fixed(GNY_MENU_WIDTH), .align = NYA_UI_ALIGN_CENTER, .title = "nyangine" };

    // cancel does nothing here: there is nothing to go back to, and quitting on the dismiss key would surprise people.
    if (nya_ui_panel_begin(ui, "main_menu", panel)) {
        nya_ui_label(ui, nya_string_menu_subtitle(), nya_ui_style_get(window).text_dim);

        if (nya_ui_button(ui, nya_string_menu_2d_scene())) gny_screen_request(GNY_SCREEN_START_GAME);
        if (nya_ui_button(ui, nya_string_menu_3d_scene())) gny_screen_request(GNY_SCREEN_CUBE3D);
        if (nya_ui_button(ui, nya_string_menu_quit())) gny_screen_request(GNY_SCREEN_QUIT);

        nya_ui_panel_end(ui);
    }

    _gny_main_menu_build_stamp(ui, window);

    nya_ui_end(ui);
}

/**
 * The build, in the bottom left corner.
 *
 * Here rather than only in the crash report because the first question about any bug is which build
 * it was, and a player reading it off the title screen answers that without reproducing anything.
 * Frameless and dimmed, so it reads as a watermark rather than as a control.
 */
void _gny_main_menu_build_stamp(NYA_UI* ui, NYA_Window* window) {
    u8 line[NYA_BUILD_LINE_MAX] = { 0 };
    (void)nya_build_line(line, (u32)sizeof(line));

    NYA_UIPanel stamp = { .anchor = NYA_UI_ANCHOR_BOTTOM_LEFT, .frameless = true };

    if (nya_ui_panel_begin(ui, "build_stamp", stamp)) {
        nya_ui_label(ui, (NYA_ConstCString)line, nya_ui_style_get(window).text_dim);
        nya_ui_panel_end(ui);
    }
}
