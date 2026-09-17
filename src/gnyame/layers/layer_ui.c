/**
 * @file layer_ui.c
 *
 * The 2D scene's HUD, in screen space over the world: a few translated status lines, the key hints,
 * and the engine's debug overlay on `t`. Shows the i18n accessors (generated from assets/i18n/en.json),
 * the named font registry, entity hover, network status, nya_debug_overlay_draw, and the drones' training
 * panel with nya_nn_neat_draw.
 * */
#include "gnyame/gnyame.h"

/** Whether the gamepad's pause was held last tick. */
NYA_INTERNAL b8 _gny_ui_pause_held = false;

/** A translucent panel behind `lines` rows of text at the top left of `x`, `y`. Returns its height. */
NYA_INTERNAL f32 _gny_ui_panel_draw(NYA_Window* window, f32 x, f32 y, f32 width, u32 lines);

/** The drones' training numbers from `top` down, and the genome they fly under them. */
NYA_INTERNAL void _gny_ui_robots_draw(NYA_Window* window, const GNY_Robots* robots, f32 top);

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

    // the gamepad's pause, which sends no key event. Tracked while a menu is up too, so a start button released
    // over the pause menu is not still down here.
    b8 pause_held = gny_action_pad_held(NYA_INPUT_ACTION_PAUSE);
    if (pause_held && !_gny_ui_pause_held && !gny_modal_active()) gny_screen_request(GNY_SCREEN_PAUSE);

    _gny_ui_pause_held = pause_held;
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

    f32 status_height = _gny_ui_panel_draw(window, GNY_UI_MARGIN, GNY_UI_MARGIN, GNY_UI_PANEL_WIDTH, nya_carray_length(lines));

    for (u32 i = 0; i < nya_carray_length(lines); i++) {
        nya_render2d_text(window, lines[i], x, y + (line * (f32)i), i + 1 == nya_carray_length(lines) ? GNY_UI_WARNING : GNY_UI_TEXT);
    }

    nya_render2d_text(window, nya_string_hud_keys(), x, (f32)window->screen_height - GNY_UI_MARGIN - line, GNY_UI_DIM);

    GNY_Robots* robots = gny_world()->robots;
    if (robots != nullptr) _gny_ui_robots_draw(window, robots, (GNY_UI_MARGIN * 2.0F) + status_height);

    // frame graph, draw calls, arena memory and the fullest ceilings, all from the engine.
    if (gny_world()->overlay_enabled) {
        nya_debug_overlay_draw(window, (NYA_DebugOverlayStyle){ .x = (f32)window->screen_width - GNY_UI_MARGIN - GNY_UI_OVERLAY_WIDTH, .y = GNY_UI_MARGIN });
    }
}

f32 _gny_ui_panel_draw(NYA_Window* window, f32 x, f32 y, f32 width, u32 lines) {
    f32 height = (nya_render2d_font_line_height() * (f32)lines) + (GNY_UI_PADDING * 2.0F);

    nya_render2d_rect(window, x, y, width, height, GNY_UI_PANEL);
    nya_render2d_rect_outline(window, x, y, width, height, 1.0F, GNY_UI_BORDER);

    return height;
}

void _gny_ui_robots_draw(NYA_Window* window, const GNY_Robots* robots, f32 top) {
    f32 line = nya_render2d_font_line_height();

    NYA_ConstCString lines[] = {
        nya_string_hud_robots_neat(robots->generations_before + robots->generation, robots->species, robots->brain_fitness),
        nya_string_hud_robots_dqn((u32)robots->dqn_steps, robots->dqn_score, (f64)robots->dqn_exploration * 100.0),
        nya_string_hud_robots_run(robots->runs + 1, nya_max(robots->record, robots->brain_fitness), robots->job_ms),
    };

    f32 height = _gny_ui_panel_draw(window, GNY_UI_MARGIN, top, GNY_ROBOT_PANEL_WIDTH, nya_carray_length(lines));

    for (u32 i = 0; i < nya_carray_length(lines); i++) {
        nya_render2d_text(window, lines[i], GNY_UI_MARGIN + GNY_UI_PADDING, top + GNY_UI_PADDING + (line * (f32)i), i == 1 ? GNY_ROBOT_DQN_COLOR : GNY_UI_TEXT);
    }

    if (!NYA_CONFIG.game.robots.show_brain || robots->brain == nullptr) return;

    f32 brain_top = top + height + GNY_UI_MARGIN;

    nya_render2d_rect(window, GNY_UI_MARGIN, brain_top, GNY_ROBOT_PANEL_WIDTH, GNY_ROBOT_BRAIN_HEIGHT, GNY_UI_PANEL);
    nya_render2d_rect_outline(window, GNY_UI_MARGIN, brain_top, GNY_ROBOT_PANEL_WIDTH, GNY_ROBOT_BRAIN_HEIGHT, 1.0F, GNY_UI_BORDER);

    // the HUD's own face and size, so the labels share its glyph atlas instead of building another.
    NYA_Font ui = nya_font_named("ui");

    nya_nn_neat_draw(window, robots->brain, (NYA_NeatDrawStyle){
        .x           = GNY_UI_MARGIN + GNY_UI_PADDING,
        .y           = brain_top + GNY_UI_PADDING,
        .width       = GNY_ROBOT_PANEL_WIDTH - (GNY_UI_PADDING * 2.0F),
        .height      = GNY_ROBOT_BRAIN_HEIGHT - (GNY_UI_PADDING * 2.0F),
        .node_radius = 7.0F,
        .hide_values = true,
        .font        = ui.path,
        .font_size   = ui.point_size,
    });
}
