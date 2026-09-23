/**
 * @file layer_cube3d_features.c
 *
 * The 3D demo's render feature switchboard: one row per NYA_RenderFeature, cycling the tri-state switch the
 * renderer reads. It writes nya_config_engine()->renderer.features, which gny_config_renderer_apply already
 * hands to nya_render_features_set every frame, so there is no second copy of the state anywhere.
 *
 * Deliberately not a second graphics settings menu. The pause menu's panel owns NYA_SettingsGraphics, the
 * player's settings, and escape reaches it from this scene; these are the developer switches above them, which
 * until now only a config file edit could reach.
 * */
#include "gnyame/gnyame.h"

// What nya_watch() below expands to, written by src/build/pp/watch.c from the @watch annotation.
#include "genyarated/watches/gnyame_layers_layer_cube3d_features_c.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API DECLARATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/** What one switch reads as on its button. */
NYA_INTERNAL NYA_ConstCString _gny_cube3d_toggle_label(NYA_RenderToggle toggle);

/** One row: the feature's name, and the button that cycles its switch. */
NYA_INTERNAL void _gny_cube3d_feature_row(NYA_UI* ui, NYA_Window* window, NYA_RenderToggle* switches, NYA_RenderFeature feature);

/** The rows from `first` up to but not including `end`, as one column. */
NYA_INTERNAL void _gny_cube3d_feature_column(NYA_UI* ui, NYA_Window* window, NYA_ConstCString id, u32 first, u32 end);

/**
 * Every cell of the decal sheet, drawn at icon size with the index that names it.
 *
 * A legend rather than a control: `mark->cell` picks one of these when a cube lands, and which number
 * is which splat is otherwise only answerable by opening the PNG. It is also the only thing in the game
 * that calls nya_ui_icon, which had been written, tested and never drawn by a caller.
 * */
NYA_INTERNAL void _gny_cube3d_decal_legend(NYA_UI* ui);

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PUBLIC API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

void gny_layer_cube3d_features_draw(NYA_UI* ui, NYA_Window* window, b8* show_hitboxes) {
    nya_assert(ui != nullptr && window != nullptr && show_hitboxes != nullptr);

    NYA_UIPanel panel = {
        .anchor    = NYA_UI_ANCHOR_RIGHT,
        .width     = nya_ui_fixed(GNY_CUBE3D_FEATURES_WIDTH),
        .text      = NYA_UI_TEXT_SMALL,
        .title     = nya_string_cube3d_features(),
        .draggable = true,
    };

    if (!nya_ui_panel_begin(ui, "cube3d_features", panel)) return;

    NYA_UIPanel columns = { .direction = NYA_UI_DIRECTION_ROW, .gap = GNY_CUBE3D_FEATURES_GAP, .frameless = true };

    if (nya_ui_panel_begin(ui, "cube3d_feature_columns", columns)) {
        /*
         * Split where NYA_RenderFeature itself splits: everything up to the post chain's master switch is
         * geometry and shading, and POST and what follows are the full-screen passes.
         */
        _gny_cube3d_feature_column(ui, window, "cube3d_features_scene", 0, NYA_RENDER_FEATURE_POST);
        _gny_cube3d_feature_column(ui, window, "cube3d_features_post", NYA_RENDER_FEATURE_POST, NYA_RENDER_FEATURE_COUNT);

        nya_ui_panel_end(ui);
    }

    /*
     * Not one of the rows above, and deliberately below them: the switches are the renderer's, read
     * from the config every frame, and this one is the scene's own. It draws what the solver holds
     * rather than changing how anything is rendered.
     */
    (void)nya_ui_toggle(ui, nya_string_cube3d_hitboxes(), show_hitboxes);

    _gny_cube3d_decal_legend(ui);

    // nothing to put back while every switch is already on default.
    NYA_RenderToggle* switches = (NYA_RenderToggle*)&nya_config_engine()->renderer.features;

    b8 changed = false;
    for (u32 feature = 0; feature < NYA_RENDER_FEATURE_COUNT; feature++) changed = changed || switches[feature] != NYA_RENDER_TOGGLE_DEFAULT;

    if (!changed) nya_ui_disabled_begin(ui);
    if (nya_ui_button(ui, nya_string_menu_reset())) nya_config_engine()->renderer.features = (NYA_RenderFeatures){ 0 };
    if (!changed) nya_ui_disabled_end(ui);

    nya_ui_panel_end(ui);
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

NYA_ConstCString _gny_cube3d_toggle_label(NYA_RenderToggle toggle) {
    switch (toggle) {
        case NYA_RENDER_TOGGLE_DEFAULT: return nya_string_menu_auto();
        case NYA_RENDER_TOGGLE_ON: return nya_string_menu_on();
        case NYA_RENDER_TOGGLE_OFF: return nya_string_menu_off();

        case NYA_RENDER_TOGGLE_COUNT:
        default: nya_unreachable();
    }
}

void _gny_cube3d_feature_row(NYA_UI* ui, NYA_Window* window, NYA_RenderToggle* switches, NYA_RenderFeature feature) {
    NYA_ConstCString name = nya_render_feature_name(feature);

    // named by the feature, so no two rows share an id however the columns are split.
    if (!nya_ui_panel_begin(ui, name, (NYA_UIPanel){ .direction = NYA_UI_DIRECTION_ROW, .align = NYA_UI_ALIGN_CENTER, .frameless = true })) return;

    // dim while the feature decides for itself, so the switches somebody has moved stand out.
    nya_ui_size(ui, nya_ui_grow(1));

    if (switches[feature] == NYA_RENDER_TOGGLE_DEFAULT) {
        nya_ui_label(ui, name, nya_ui_style_get(window).text_dim);
    } else {
        nya_ui_label(ui, name);
    }

    nya_ui_size(ui, nya_ui_fixed(GNY_CUBE3D_FEATURE_STATE_WIDTH));

    if (nya_ui_selectable(ui, _gny_cube3d_toggle_label(switches[feature]), switches[feature] != NYA_RENDER_TOGGLE_DEFAULT)) {
        switches[feature] = (NYA_RenderToggle)(((u32)switches[feature] + 1) % (u32)NYA_RENDER_TOGGLE_COUNT);
    }

    nya_ui_panel_end(ui);
}

// @watch
void _gny_cube3d_feature_column(NYA_UI* ui, NYA_Window* window, NYA_ConstCString id, u32 first, u32 end) {
    nya_assert_lt(first, end);
    nya_assert_le(end, (u32)NYA_RENDER_FEATURE_COUNT);

    if (!nya_ui_panel_begin(ui, id, (NYA_UIPanel){ .width = nya_ui_grow(1), .frameless = true })) return;

    // read as an array, which the static asserts in render_features.h make legal.
    NYA_RenderToggle* switches = (NYA_RenderToggle*)&nya_config_engine()->renderer.features;
    nya_watch(_gny_cube3d_feature_column);

    for (u32 feature = first; feature < end; feature++) _gny_cube3d_feature_row(ui, window, switches, (NYA_RenderFeature)feature);

    nya_ui_panel_end(ui);
}

void _gny_cube3d_decal_legend(NYA_UI* ui) {
    nya_assert(ui != nullptr);

    if (!nya_ui_panel_begin(ui, "cube3d_decal_legend",
                            (NYA_UIPanel){ .direction = NYA_UI_DIRECTION_ROW, .align = NYA_UI_ALIGN_CENTER, .frameless = true })) {
        return;
    }

    nya_ui_size(ui, nya_ui_grow(1));
    nya_ui_label(ui, nya_string_cube3d_decals());

    for (u32 cell = 0; cell < GNY_CUBE3D_DECAL_COLUMNS * GNY_CUBE3D_DECAL_ROWS; cell++) {
        // the same row-major order nya_render3d_decal cuts the sheet in, so the numbers agree.
        const u32 column = cell % GNY_CUBE3D_DECAL_COLUMNS;
        const u32 row    = cell / GNY_CUBE3D_DECAL_COLUMNS;

        nya_ui_icon(ui,
                    (NYA_UIIcon){
                        .texture      = GNY_CUBE3D_DECAL_TEXTURE,
                        .source_x     = (f32)column * GNY_CUBE3D_DECAL_CELL_SIZE,
                        .source_y     = (f32)row * GNY_CUBE3D_DECAL_CELL_SIZE,
                        .source_width = GNY_CUBE3D_DECAL_CELL_SIZE,
                        .source_height = GNY_CUBE3D_DECAL_CELL_SIZE,
                    },
                    GNY_CUBE3D_DECAL_ICON_SIZE);

        u8 number[8] = { 0 };
        (void)snprintf((char*)number, sizeof(number), FMTu32, cell);
        nya_ui_label(ui, (NYA_ConstCString)number);
    }

    nya_ui_panel_end(ui);
}
