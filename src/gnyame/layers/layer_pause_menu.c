/**
 * @file layer_pause_menu.c
 *
 * The pause menu over a stopped 2D world, with the options inline: volumes, the stats overlay, the player's name and
 * the language. Beside it a look panel edits the UI's own style live, so every widget and both the flat and the
 * skinned look can be tried in one place.
 * */
#include "gnyame/gnyame.h"

/** The locales in assets/i18n, each named in its own language so it can be found from any other. */
NYA_INTERNAL const struct {
    NYA_ConstCString locale;
    NYA_ConstCString name;
} _GNY_LOCALES[] = {
    { "en", "English" },
    { "de", "Deutsch" },
};

NYA_INTERNAL void _gny_pause_menu(NYA_Window* window, NYA_UIPass pass);

/** Every widget the UI has, editing the style in NYA_CONFIG that gny_ui_begin hands the window. */
NYA_INTERNAL void _gny_look_panel(NYA_UI* ui);

/** A volume row that writes back to the settings when it moves. */
NYA_INTERNAL void _gny_volume_slider(NYA_UI* ui, NYA_ConstCString label, NYA_VolumeChannel channel);

/** Loads the locale whose index into _GNY_LOCALES is copied in, at the barrier, so no label in the pass goes stale. */
NYA_INTERNAL void _gny_locale_apply(void* data);

void gny_layer_pause_menu_on_create(NYA_Window* window) {
    // on "resume", so escape then enter is the quickest way back.
    nya_ui_focus_reset(window);
}

void gny_layer_pause_menu_on_destroy(NYA_Window* window) {
    // the solver is restarted by the screen change that popped this layer, which knows whether it should be. the
    // reset stops a name field still typing.
    nya_ui_focus_reset(window);
}

void gny_layer_pause_menu_on_event(NYA_Window* window, NYA_Event* event) {
    nya_unused(window);

    // so a click on the panel cannot drop a crate behind it.
    (void)nya_ui_modal_event(event);
}

void gny_layer_pause_menu_on_update(NYA_Window* window, f32 delta_time_s) {
    nya_unused(delta_time_s);

    _gny_pause_menu(window, NYA_UI_PASS_INPUT);

    // pause closes the menu it opened, from the start button as from the key, unless the key only left the name field.
    if (nya_input_action_just_pressed(NYA_INPUT_ACTION_PAUSE) && !nya_ui_typing(window)) gny_screen_request(GNY_SCREEN_RESUME);
}

void gny_layer_pause_menu_on_render(NYA_Window* window) {
    _gny_pause_menu(window, NYA_UI_PASS_DRAW);
}

void _gny_pause_menu(NYA_Window* window, NYA_UIPass pass) {
    NYA_UI* ui = gny_ui_begin(window, pass);
    nya_ui_scrim(ui);

    // the window's look with its skins cut from the menu's sheet, which leaves them flat when the config names none.
    NYA_UIStyle skinned = nya_ui_style_get(window);
    (void)snprintf(skinned.panel_skin.texture, sizeof(skinned.panel_skin.texture), "%s", NYA_CONFIG.game.menu_sheet);
    nya_ui_style_push(ui, skinned);

    // a top level panel scrolls on its own when the window is too short for it.
    NYA_UIPanel panel = { .anchor = NYA_UI_ANCHOR_CENTER, .width = nya_ui_fixed(GNY_MENU_WIDTH), .align = NYA_UI_ALIGN_CENTER, .title = nya_string_menu_paused() };

    if (nya_ui_panel_begin(ui, "pause_menu", panel)) {
        if (nya_ui_button(ui, nya_string_menu_resume())) gny_screen_request(GNY_SCREEN_RESUME);
        if (nya_ui_button(ui, nya_string_menu_restart())) gny_screen_request(GNY_SCREEN_RESTART);

        _gny_volume_slider(ui, nya_string_menu_master_volume(), NYA_VOLUME_CHANNEL_MASTER);
        _gny_volume_slider(ui, nya_string_menu_music_volume(), NYA_VOLUME_CHANNEL_MUSIC);

        b8 stats = gny_world()->overlay_enabled;
        if (nya_ui_toggle(ui, nya_string_menu_stats(), &stats)) gny_overlay_toggle();

        // saved with the settings on quit, and the name the next session plays under.
        char name[NYA_SETTINGS_NAME_MAX];
        (void)snprintf(name, sizeof(name), "%s", nya_settings_player_name());
        if (nya_ui_text_input(ui, nya_string_menu_name(), name, sizeof(name))) nya_settings_player_name_set(name);

        // the label as wide as its text and the choices sharing the rest; left and right move between them.
        if (nya_ui_panel_begin(ui, nullptr, (NYA_UIPanel){ .direction = NYA_UI_DIRECTION_ROW, .align = NYA_UI_ALIGN_CENTER, .frameless = true })) {
            nya_ui_label(ui, nya_string_menu_language(), nya_ui_style_get(window).text_dim);

            for (u32 i = 0; i < nya_carray_length(_GNY_LOCALES); i++) {
                b8 current = nya_string_equals(nya_i18n_locale(), _GNY_LOCALES[i].locale);

                nya_ui_size(ui, nya_ui_grow(1));
                if (nya_ui_selectable(ui, _GNY_LOCALES[i].name, current) && !current) nya_sim_defer(_gny_locale_apply, &i, sizeof(i));
            }

            nya_ui_panel_end(ui);
        }

        if (nya_ui_button(ui, nya_string_menu_main_menu())) gny_screen_request(GNY_SCREEN_MAIN_MENU);
        if (nya_ui_button(ui, nya_string_menu_quit())) gny_screen_request(GNY_SCREEN_QUIT);

        if (nya_ui_cancelled(ui)) gny_screen_request(GNY_SCREEN_RESUME);

        nya_ui_panel_end(ui);
    }

    _gny_look_panel(ui);

    nya_ui_style_pop(ui);
    nya_ui_end(ui);
}

void _gny_look_panel(NYA_UI* ui) {
    NYA_UIStyle* style = &NYA_CONFIG.engine.ui;

    NYA_UIPanel panel = { .anchor = NYA_UI_ANCHOR_RIGHT, .width = nya_ui_fixed(GNY_LOOK_WIDTH), .align = NYA_UI_ALIGN_CENTER, .title = nya_string_menu_look() };
    if (!nya_ui_panel_begin(ui, "look", panel)) return;

    char* sheet  = NYA_CONFIG.game.menu_sheet;
    b8    skinned = sheet[0] != '\0';

    if (nya_ui_panel_begin(ui, nullptr, (NYA_UIPanel){ .direction = NYA_UI_DIRECTION_ROW, .children = nya_ui_grow(1), .frameless = true })) {
        if (nya_ui_selectable(ui, nya_string_menu_flat(), !skinned)) sheet[0] = '\0';
        if (nya_ui_selectable(ui, nya_string_menu_sheet(), skinned)) (void)snprintf(sheet, sizeof(NYA_CONFIG.game.menu_sheet), "%s", GNY_MENU_SHEET);
        nya_ui_panel_end(ui);
    }

    b8 animated = style->transition_s > 0.0F;

    if (nya_ui_toggle(ui, nya_string_menu_animate(), &animated)) {
        style->transition_s = animated ? GNY_UI_TRANSITION_S : 0.0F;
        style->appear_s     = animated ? GNY_UI_APPEAR_S : 0.0F;
    }

    // zero derives the scale from the window, which is what the leftmost step means.
    (void)nya_ui_slider(ui, nya_string_menu_scale(), &style->scale, 0.0F, GNY_UI_SCALE_MAX, NYA_UI_SCALE_STEP);

    NYA_Color accent = style->accent.a > 0.0F ? style->accent : NYA_UI_ACCENT;
    if (nya_ui_color_picker(ui, nya_string_menu_accent(), &accent)) style->accent = accent;

    // nothing to reset while the accent is already the default.
    b8 changed = style->accent.a > 0.0F;

    if (!changed) nya_ui_disabled_begin(ui);
    if (nya_ui_button(ui, nya_string_menu_reset())) style->accent = (NYA_Color){ 0 };
    if (!changed) nya_ui_disabled_end(ui);

    nya_ui_panel_end(ui);
}

void _gny_volume_slider(NYA_UI* ui, NYA_ConstCString label, NYA_VolumeChannel channel) {
    f32 volume = nya_settings_volume(channel);

    if (nya_ui_slider(ui, label, &volume, 0.0F, 1.0F, GNY_VOLUME_STEP)) nya_settings_volume_set(channel, volume);
}

void _gny_locale_apply(void* data) {
    u32 index = *(u32*)data;
    nya_assert(index < nya_carray_length(_GNY_LOCALES));

    NYA_Error loaded = nya_i18n_load(_GNY_LOCALES[index].locale, NYA_STRING_KEYS, NYA_STRING_COUNT);
    if (!loaded.ok) nya_log_warn("Could not load the '%s' locale: %s", _GNY_LOCALES[index].locale, (NYA_ConstCString)loaded.message);
}
