/**
 * @file layer_ui.c
 *
 * The 2D scene's HUD, in screen space over the world: a few translated status lines, the key hints,
 * and the engine's debug overlay on `t`. Shows the i18n accessors (generated from assets/i18n/en.json),
 * the named font registry, entity hover, network status and nya_debug_overlay_draw.
 * */
#include "gnyame/gnyame.h"

/** A translucent panel behind `lines` rows of text at the top left of `x`, `y`. */
NYA_INTERNAL void _gny_ui_panel_draw(NYA_Window* window, f32 x, f32 y, f32 width, u32 lines);

void gny_layer_ui_on_create(NYA_Window* window) {
    nya_unused(window);

    // the immediate-mode text calls below draw with whatever font is current; "ui" is registered in world.c.
    NYA_Font ui = nya_font_named("ui");
    nya_render2d_font_set(ui.path, ui.point_size);
}

void gny_layer_ui_on_destroy(NYA_Window* window) {
    nya_unused(window);
}

void gny_layer_ui_on_event(NYA_Window* window, NYA_Event* event) {
    nya_unused(window);

    if (event->type != NYA_EVENT_KEY_DOWN || event->as_key_event.is_repeat) return;
    const NYA_KeyEvent* key = &event->as_key_event;

    if (nya_input_action_matches(GNY_ACTION_TOGGLE_OVERLAY, key->key, key->modifier_flags)) {
        gny_overlay_toggle();
        event->was_handled = true;
    } else if (nya_input_action_matches(NYA_INPUT_ACTION_PAUSE, key->key, key->modifier_flags)) {
        gny_screen_request(GNY_SCREEN_PAUSE);
        event->was_handled = true;
    }
}

void gny_layer_ui_on_update(NYA_Window* window, f32 delta_time_s) {
    nya_unused(window, delta_time_s);
}

void gny_layer_ui_on_render(NYA_Window* window) {
    f32 line = nya_render2d_font_line_height();

    // zero while the font is still loading, so draw nothing rather than stacking every line at one height.
    if (line <= 0.0F) return;

    u32         awake   = 0;
    u32         boxes   = gny_entity_box_count(&awake);
    NYA_Entity* hovered = nya_entity_get(nya_entity_hovered());

    NYA_ConstCString lines[] = {
        nya_string_hud_boxes(boxes, awake),
        nya_string_hud_players(nya_net_server_peer_count()),
        nya_net_server_is_listening() ? nya_string_hud_hosting(GNY_LAUNCH.listen_port) : nya_string_hud_offline(),
        nya_string_hud_hovering(hovered != nullptr ? hovered->name : "-"),
        nya_physics2d_enabled() ? "" : nya_string_hud_paused(),
    };

    f32 x = GNY_UI_MARGIN + GNY_UI_PADDING;
    f32 y = GNY_UI_MARGIN + GNY_UI_PADDING;

    _gny_ui_panel_draw(window, GNY_UI_MARGIN, GNY_UI_MARGIN, GNY_UI_PANEL_WIDTH, nya_carray_length(lines));

    for (u32 i = 0; i < nya_carray_length(lines); i++) {
        nya_render2d_text(window, lines[i], x, y + (line * (f32)i), i + 1 == nya_carray_length(lines) ? GNY_UI_WARNING : GNY_UI_TEXT);
    }

    nya_render2d_text(window, nya_string_hud_keys(), x, (f32)window->screen_height - GNY_UI_MARGIN - line, GNY_UI_DIM);

    // frame graph, draw calls, arena memory and the fullest ceilings, all from the engine.
    if (gny_world()->overlay_enabled) {
        nya_debug_overlay_draw(window, (NYA_DebugOverlayStyle){ .x = (f32)window->screen_width - GNY_UI_MARGIN - GNY_UI_OVERLAY_WIDTH, .y = GNY_UI_MARGIN });
    }
}

void _gny_ui_panel_draw(NYA_Window* window, f32 x, f32 y, f32 width, u32 lines) {
    f32 height = (nya_render2d_font_line_height() * (f32)lines) + (GNY_UI_PADDING * 2.0F);

    nya_render2d_rect(window, x, y, width, height, GNY_UI_PANEL);
    nya_render2d_rect_outline(window, x, y, width, height, 1.0F, GNY_UI_BORDER);
}
