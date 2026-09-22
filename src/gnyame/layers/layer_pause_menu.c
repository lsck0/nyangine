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

/**
 * The two tool windows' state, which is the caller's by design: the close button in a title bar writes false here
 * and nothing in the UI can ever write true, so the menu below is what puts them back. See NYA_UIWindowState.
 * */
NYA_INTERNAL NYA_UIWindowState _GNY_LOOK_WINDOW    = { .open = true };
NYA_INTERNAL NYA_UIWindowState _GNY_WIDGETS_WINDOW = { .open = true };

/** What both windows' hamburger offers, in the order NYA_UIWindowState.menu_picked indexes. */
enum {
    _GNY_WINDOW_MENU_RESET = 0,
    _GNY_WINDOW_MENU_CLOSE,
    _GNY_WINDOW_MENU_COUNT,
};

NYA_INTERNAL void _gny_pause_menu(NYA_Window* window, NYA_UIPass pass);

/** A window editing the style in NYA_CONFIG that gny_ui_begin hands the platform window. */
NYA_INTERNAL void _gny_look_panel(NYA_UI* ui);

/** The player's graphics settings, saved on quit and laid over both scenes' renderer options every frame. */
NYA_INTERNAL void _gny_graphics_panel(NYA_UI* ui);

/**
 * A window over the widgets the menus above do not use: tabs, a table, a folding section, a dropdown, radio
 * buttons, a chart and an opacity group. It shows real counters, so it is a debug readout as well as the thing
 * that exercises them, and with the look window beside it, two windows that can be dragged onto each other.
 * */
NYA_INTERNAL void _gny_widgets_panel(NYA_Window* window, NYA_UI* ui);

/** A label and a row of choices sharing the rest, with `*selected` the index of the chosen one. */
NYA_INTERNAL void _gny_choice_row(NYA_UI* ui, NYA_ConstCString label, const NYA_ConstCString* choices, u32 count, u32* selected);

/** A volume row that writes back to the settings when it moves. */
NYA_INTERNAL void _gny_volume_slider(NYA_UI* ui, NYA_ConstCString label, NYA_VolumeChannel channel);

// The nya_lambda bodies written below, hoisted out to here by src/build/pp/lambda.c. After the
// declarations above and after _GNY_LOCALES, which is what a body of this file may name.
#include "genyarated/lambdas/gnyame_layers_layer_pause_menu_c.h"

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

                /*
                 * At the barrier, so no label already laid out this pass goes stale under it. `i` is
                 * copied by nya_sim_defer rather than captured: the body is a file scope function and
                 * could not name a local of this one even if it tried.
                 */
                if (nya_ui_selectable(ui, _GNY_LOCALES[i].name, current) && !current) {
                    nya_sim_defer(nya_lambda(gny_locale_apply, void, (void* data), {
                        u32 index = *(u32*)data;
                        nya_assert(index < nya_carray_length(_GNY_LOCALES));

                        NYA_Error loaded = nya_i18n_load(_GNY_LOCALES[index].locale, NYA_STRING_KEYS, NYA_STRING_COUNT);
                        if (!loaded.ok) nya_log_warn("Could not load '%s': %s", _GNY_LOCALES[index].locale, (NYA_ConstCString)loaded.message);
                    }), &i, sizeof(i));
                }
            }

            nya_ui_panel_end(ui);
        }

        // greyed rather than hidden when no friends service is up, so the menu does not change shape
        // when a player starts Discord mid-session.
        b8 social = nya_social_available();

        if (!social) nya_ui_disabled_begin(ui);

        if (nya_ui_button(ui, nya_string_menu_invite())) {
            NYA_Error opened = nya_social_invite_open();

            // the Steam overlay is the only provider with a picker; on Discord the invite is sent from
            // the chat window and the presence card is what makes it possible, so this says so once.
            if (!opened.ok) nya_log_info("No invite dialog here: %s", (NYA_ConstCString)opened.message);
        }

        if (!social) nya_ui_disabled_end(ui);

        // who an invite goes out as, so a player on the wrong account finds out before a friend does.
        NYA_ConstCString signed_in = nya_social_user_name();
        if (social && signed_in[0] != '\0') nya_ui_label(ui, signed_in, nya_ui_style_get(window).text_dim);

        /*
         * What this player may do in the session, out of the same table the web interface reads. The
         * rank is a label rather than a setting: it is the resolver's answer, and the only way to
         * change it is for somebody who outranks them to say so.
         */
        if (gny_guild() != nullptr) {
            u64 local = gny_guild_local();

            char rank[64] = { 0 };
            (void)snprintf(rank, sizeof(rank), "%s", gny_guild_rank_name(local));

            if (rank[0] != '\0') nya_ui_label(ui, rank, nya_ui_style_get(window).text_dim);

            // and the one moderation action there is. Greyed out rather than hidden when they may not:
            // a player who cannot kick should see that the session has moderation, not that it has none.
            b8 may_kick = gny_guild_may(local, GNY_PERMISSION_KICK);

            if (!may_kick) nya_ui_disabled_begin(ui);

            if (nya_ui_button(ui, "kick the last player to join")) {
                u64 target = (u64)nya_net_server_peer_count();

                NYA_Error kicked = gny_guild_kick(local, target);
                if (!kicked.ok) nya_log_info("Not kicked: %s", (NYA_ConstCString)kicked.message);
            }

            if (!may_kick) nya_ui_disabled_end(ui);
        }

        // only while they are gone, since a window that is showing has its own close button and needs no second
        // switch. This is the whole of what "the caller owns the flag" buys: the UI cannot show a window again.
        if (!_GNY_LOOK_WINDOW.open || !_GNY_WIDGETS_WINDOW.open) {
            if (nya_ui_panel_begin(ui, nullptr, (NYA_UIPanel){ .direction = NYA_UI_DIRECTION_ROW, .children = nya_ui_grow(1), .frameless = true })) {
                if (!_GNY_LOOK_WINDOW.open && nya_ui_button(ui, nya_string_menu_look())) _GNY_LOOK_WINDOW = (NYA_UIWindowState){ .open = true };
                if (!_GNY_WIDGETS_WINDOW.open && nya_ui_button(ui, nya_string_menu_widgets())) _GNY_WIDGETS_WINDOW = (NYA_UIWindowState){ .open = true };

                nya_ui_panel_end(ui);
            }
        }

        if (nya_ui_button(ui, nya_string_menu_main_menu())) gny_screen_request(GNY_SCREEN_MAIN_MENU);
        if (nya_ui_button(ui, nya_string_menu_quit())) gny_screen_request(GNY_SCREEN_QUIT);

        if (nya_ui_cancelled(ui)) gny_screen_request(GNY_SCREEN_RESUME);

        nya_ui_panel_end(ui);
    }

    _gny_look_panel(ui);
    _gny_graphics_panel(ui);
    _gny_widgets_panel(window, ui);

    nya_ui_style_pop(ui);
    gny_ui_end(window, ui);
}

void _gny_widgets_panel(NYA_Window* window, NYA_UI* ui) {
    // a ring of the last frames, sampled once per pass so the chart moves while the world is stopped.
    static f32 frame_ms[GNY_WIDGETS_SAMPLES] = { 0 };
    static f32 draws[GNY_WIDGETS_SAMPLES]    = { 0 };
    static u32 samples                       = 0;

    // which tab is showing, what the chart plots and how, how far the whole panel is faded, and whether the plot's
    // settings are folded away.
    static u32 tab      = 0;
    static u32 metric   = 0;
    static u32 kind     = 0;
    static f32 opacity  = 1.0F;
    static b8  settings = true;

    NYA_Render2DFrameStats batch = nya_render2d_frame_stats(window);
    u32                    slot  = samples % GNY_WIDGETS_SAMPLES;

    frame_ms[slot] = nya_app_get()->frame_stats.delta_time_s * 1000.0F;
    draws[slot]    = (f32)batch.draw_calls;
    samples       += 1;

    u32 count = nya_min(samples, (u32)GNY_WIDGETS_SAMPLES);

    // the panel itself fades, frame and all, which is what an opacity group is for.
    nya_ui_opacity_begin(ui, opacity);
    defer nya_ui_opacity_end(ui);

    NYA_ConstCString items[_GNY_WINDOW_MENU_COUNT] = {
        [_GNY_WINDOW_MENU_RESET] = nya_string_menu_reset(),
        [_GNY_WINDOW_MENU_CLOSE] = nya_string_menu_close(),
    };

    NYA_UIWindow widgets = {
        .panel      = { .anchor = NYA_UI_ANCHOR_BOTTOM_RIGHT, .width = nya_ui_fixed(GNY_WIDGETS_WIDTH), .text = NYA_UI_TEXT_SMALL },
        .title      = nya_string_menu_widgets(),
        .close      = true,
        .collapse   = true,
        .resize     = true,
        .menu       = items,
        .menu_count = nya_carray_length(items),
    };

    if (!nya_ui_window_begin(ui, "widgets", widgets, &_GNY_WIDGETS_WINDOW)) return;

    // the hamburger reports an index for the one pass it was picked on, and the caller decides what it means.
    if (_GNY_WIDGETS_WINDOW.menu_picked == _GNY_WINDOW_MENU_RESET) _GNY_WIDGETS_WINDOW = (NYA_UIWindowState){ .open = true };
    if (_GNY_WIDGETS_WINDOW.menu_picked == _GNY_WINDOW_MENU_CLOSE) _GNY_WIDGETS_WINDOW.open = false;

    NYA_ConstCString tabs[] = { nya_string_menu_table(), nya_string_menu_chart() };
    (void)nya_ui_tabs(ui, "pages", tabs, nya_carray_length(tabs), &tab);

    if (tab == 0) {
        // one row per counter, the value column as wide as its digits and the name column taking the rest.
        const f32        widths[]  = { 0.0F, GNY_WIDGETS_VALUE_WIDTH };
        NYA_ConstCString headers[] = { nya_string_menu_metric(), nya_string_menu_value() };

        if (nya_ui_table_begin(ui, "counters", (NYA_UITable){ .widths = widths, .columns = 2, .headers = headers, .striped = true })) {
            struct {
                NYA_ConstCString name;
                f32              value;
            } rows[] = {
                { nya_string_menu_frame_ms(), frame_ms[slot]        },
                { nya_string_menu_draws(),    (f32)batch.draw_calls },
                { nya_string_menu_vertices(), (f32)batch.vertices   },
            };

            for (u32 i = 0; i < nya_carray_length(rows); i++) {
                if (!nya_ui_table_row_begin(ui)) continue;

                char value[GNY_WIDGETS_VALUE_MAX];
                (void)snprintf(value, sizeof(value), "%.2f", (f64)rows[i].value);

                nya_ui_label(ui, rows[i].name);
                nya_ui_label(ui, value);

                nya_ui_table_row_end(ui);
            }

            nya_ui_table_end(ui);
        }
    } else {
        // the dropdown's list and the radio pair land on each other, which is what makes this the place to look at
        // a float: the list hangs over the radios and takes the clicks they would otherwise have had.
        if (nya_ui_section_begin(ui, nya_string_menu_metric(), &settings)) {
            NYA_ConstCString metrics[] = { nya_string_menu_frame_ms(), nya_string_menu_draws() };
            (void)nya_ui_dropdown(ui, nya_string_menu_metric(), metrics, nya_carray_length(metrics), &metric);

            // a radio pair rather than a selectable pair, since the two own one variable between them.
            if (nya_ui_panel_begin(ui, nullptr, (NYA_UIPanel){ .direction = NYA_UI_DIRECTION_ROW, .children = nya_ui_grow(1), .frameless = true })) {
                (void)nya_ui_radio(ui, nya_string_menu_line(), &kind, NYA_UI_CHART_LINE);
                (void)nya_ui_radio(ui, nya_string_menu_bars(), &kind, NYA_UI_CHART_BAR);
                nya_ui_panel_end(ui);
            }

            nya_ui_section_end(ui);
        }

        nya_ui_chart(ui, "plot",
                     (NYA_UIChart){
                         .values = metric == 0 ? frame_ms : draws,
                         .count  = count,
                         .kind   = (NYA_UIChartKind)kind,
                         .height = GNY_WIDGETS_CHART_HEIGHT,
                     });
    }

    (void)nya_ui_slider(ui, nya_string_menu_fade(), &opacity, GNY_WIDGETS_FADE_MIN, 1.0F, GNY_WIDGETS_FADE_STEP);

    nya_ui_window_end(ui);
}

void _gny_look_panel(NYA_UI* ui) {
    NYA_UIStyle* style = &NYA_CONFIG.engine.ui;

    NYA_ConstCString items[_GNY_WINDOW_MENU_COUNT] = {
        [_GNY_WINDOW_MENU_RESET] = nya_string_menu_reset(),
        [_GNY_WINDOW_MENU_CLOSE] = nya_string_menu_close(),
    };

    NYA_UIWindow look = {
        .panel      = { .anchor = NYA_UI_ANCHOR_RIGHT, .width = nya_ui_fixed(GNY_LOOK_WIDTH), .align = NYA_UI_ALIGN_CENTER },
        .title      = nya_string_menu_look(),
        .close      = true,
        .collapse   = true,
        .resize     = true,
        .menu       = items,
        .menu_count = nya_carray_length(items),
    };

    if (!nya_ui_window_begin(ui, "look", look, &_GNY_LOOK_WINDOW)) return;

    // reset puts the accent back as well as the window, since that is what this window is for.
    if (_GNY_LOOK_WINDOW.menu_picked == _GNY_WINDOW_MENU_RESET) {
        _GNY_LOOK_WINDOW = (NYA_UIWindowState){ .open = true };
        style->accent    = (NYA_Color){ 0 };
    }

    if (_GNY_LOOK_WINDOW.menu_picked == _GNY_WINDOW_MENU_CLOSE) _GNY_LOOK_WINDOW.open = false;

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

    // zero is scale 1, which is what the leftmost step means. the window's size never moves it.
    (void)nya_ui_slider(ui, nya_string_menu_scale(), &style->scale, 0.0F, GNY_UI_SCALE_MAX, NYA_UI_SCALE_STEP);

    NYA_Color accent = style->accent.a > 0.0F ? style->accent : NYA_UI_ACCENT;
    if (nya_ui_color_picker(ui, nya_string_menu_accent(), &accent)) style->accent = accent;

    // nothing to reset while the accent is already the default.
    b8 changed = style->accent.a > 0.0F;

    if (!changed) nya_ui_disabled_begin(ui);
    if (nya_ui_button(ui, nya_string_menu_reset())) style->accent = (NYA_Color){ 0 };
    if (!changed) nya_ui_disabled_end(ui);

    nya_ui_window_end(ui);
}

void _gny_graphics_panel(NYA_UI* ui) {
    NYA_UIPanel panel = { .anchor = NYA_UI_ANCHOR_LEFT, .width = nya_ui_fixed(GNY_GRAPHICS_WIDTH), .text = NYA_UI_TEXT_SMALL, .title = nya_string_menu_graphics() };
    if (!nya_ui_panel_begin(ui, "graphics", panel)) return;

    NYA_SettingsGraphics graphics = nya_settings_graphics();

    // one sample is multisampling off.
    NYA_ConstCString samples[] = { nya_string_menu_off(), "2x", "4x", "8x" };
    u32              sample    = (u32)log2((f64)graphics.msaa_samples);

    _gny_choice_row(ui, nya_string_menu_antialiasing(), samples, nya_carray_length(samples), &sample);
    graphics.msaa_samples = 1U << sample;

    NYA_ConstCString qualities[] = { nya_string_menu_off(), nya_string_menu_low(), nya_string_menu_medium(), nya_string_menu_high() };
    u32              quality     = (u32)graphics.shadows;

    _gny_choice_row(ui, nya_string_menu_shadows(), qualities, nya_carray_length(qualities), &quality);
    graphics.shadows = (NYA_GraphicsQuality)quality;

    (void)nya_ui_toggle(ui, nya_string_menu_fxaa(), &graphics.fxaa);
    (void)nya_ui_toggle(ui, nya_string_menu_occlusion(), &graphics.ambient_occlusion);
    (void)nya_ui_toggle(ui, nya_string_menu_bloom(), &graphics.bloom);
    (void)nya_ui_toggle(ui, nya_string_menu_depth_of_field(), &graphics.depth_of_field);
    (void)nya_ui_toggle(ui, nya_string_menu_eye_adaptation(), &graphics.eye_adaptation);
    (void)nya_ui_toggle(ui, nya_string_menu_light_shafts(), &graphics.light_shafts);
    (void)nya_ui_toggle(ui, nya_string_menu_motion_blur(), &graphics.motion_blur);

    (void)nya_ui_slider(ui, nya_string_menu_field_of_view(), &graphics.fov, GNY_GRAPHICS_FOV_MIN, GNY_GRAPHICS_FOV_MAX, GNY_GRAPHICS_FOV_STEP);
    (void)nya_ui_slider(ui, nya_string_menu_render_scale(), &graphics.render_scale, GNY_GRAPHICS_SCALE_MIN, 1.0F, GNY_GRAPHICS_SCALE_STEP);

    nya_settings_graphics_set(graphics);

    nya_ui_panel_end(ui);
}

void _gny_choice_row(NYA_UI* ui, NYA_ConstCString label, const NYA_ConstCString* choices, u32 count, u32* selected) {
    // named by its label, so two rows can both offer "off".
    if (!nya_ui_panel_begin(ui, label, (NYA_UIPanel){ .direction = NYA_UI_DIRECTION_ROW, .align = NYA_UI_ALIGN_CENTER, .frameless = true })) return;

    nya_ui_label(ui, label);

    for (u32 i = 0; i < count; i++) {
        nya_ui_size(ui, nya_ui_grow(1));
        if (nya_ui_selectable(ui, choices[i], *selected == i)) *selected = i;
    }

    nya_ui_panel_end(ui);
}

void _gny_volume_slider(NYA_UI* ui, NYA_ConstCString label, NYA_VolumeChannel channel) {
    f32 volume = nya_settings_volume(channel);

    if (nya_ui_slider(ui, label, &volume, 0.0F, 1.0F, GNY_VOLUME_STEP)) nya_settings_volume_set(channel, volume);
}
