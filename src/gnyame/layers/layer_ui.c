/**
 * @file layer_ui.c
 *
 * The 2D scene's HUD, in screen space over the world: a few translated status lines, the key hints,
 * and the engine's debug overlay on `t`. Shows the i18n accessors (generated from assets/i18n/en.json),
 * the named font registry, entity hover, network status, UI panels, nya_debug_overlay_draw, and the
 * drones' training panel with nya_nn_neat_draw.
 * */
#include "gnyame/gnyame.h"

/** The drones' training numbers, and the genome they fly under them. */
NYA_INTERNAL void _gny_ui_robots(NYA_Window* window, NYA_UI* ui, const GNY_Robots* robots);

void gny_layer_ui_on_create(NYA_Window* window) {
    nya_unused(window);

    // the debug overlay draws with whatever font is current; "ui" is registered in world.c.
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

    if (gny_overlay_key(key)) event->was_handled = true;
}

void gny_layer_ui_on_update(NYA_Window* window, f32 delta_time_s) {
    nya_unused(window, delta_time_s);

    // the key and the gamepad's start alike. while a menu is up, the menu reads pause itself.
    if (nya_input_action_just_pressed(NYA_INPUT_ACTION_PAUSE) && !gny_modal_active()) gny_screen_request(GNY_SCREEN_PAUSE);
}

void gny_layer_ui_on_render(NYA_Window* window) {
    u32         awake   = 0;
    u32         boxes   = gny_entity_box_count(&awake);
    NYA_Entity* hovered = nya_entity_get(nya_entity_hovered());

    NYA_ConstCString lines[] = {
        nya_string_hud_boxes(boxes, awake),
        nya_string_hud_players(nya_net_server_peer_count()),
        nya_net_server_is_listening() ? nya_string_hud_hosting(nya_net_server_port()) : nya_string_hud_offline(),
        nya_string_hud_hovering(hovered != nullptr ? hovered->name : "-"),
    };

    NYA_UI* ui = gny_ui_begin(window, NYA_UI_PASS_DRAW);

    // a frameless column, so the status, training and brain panels stack without adding up heights by hand.
    if (nya_ui_panel_begin(ui, "hud", (NYA_UIPanel){ .text = NYA_UI_TEXT_SMALL, .frameless = true })) {
        if (nya_ui_panel_begin(ui, "status", (NYA_UIPanel){ .width = nya_ui_fixed(GNY_UI_PANEL_WIDTH) })) {
            for (u32 i = 0; i < nya_carray_length(lines); i++) nya_ui_label(ui, lines[i]);

            // kept as an empty line while running, so the panel does not jump when physics stops.
            nya_ui_label(ui, nya_physics2d_enabled() ? "" : nya_string_hud_paused(), GNY_UI_WARNING);

            nya_ui_panel_end(ui);
        }

        GNY_Robots* robots = gny_world()->robots;
        if (robots != nullptr) _gny_ui_robots(window, ui, robots);

        nya_ui_panel_end(ui);
    }

    // wrapped, since a translation of the hints can be wider than the window.
    NYA_UIPanel keys = { .anchor = NYA_UI_ANCHOR_BOTTOM_LEFT, .overflow = NYA_UI_OVERFLOW_WRAP, .text = NYA_UI_TEXT_SMALL };

    if (nya_ui_panel_begin(ui, "keys", keys)) {
        nya_ui_label(ui, nya_string_hud_keys(), nya_ui_style_get(window).text_dim);
        nya_ui_panel_end(ui);
    }

    gny_ui_end(window, ui);

    // frame graph, draw calls, arena memory and the fullest ceilings, or the trace table, all from the engine.
    gny_overlay_draw(window);
}

void _gny_ui_robots(NYA_Window* window, NYA_UI* ui, const GNY_Robots* robots) {
    // held to its width, so a line whose numbers grow shrinks instead of the panel jumping wider.
    NYA_UIPanel training = { .width = nya_ui_fixed(GNY_ROBOT_PANEL_WIDTH), .overflow = NYA_UI_OVERFLOW_SHRINK };

    if (nya_ui_panel_begin(ui, "robots", training)) {
        nya_ui_label(ui, nya_string_hud_robots_neat(robots->generations_before + robots->generation, robots->species, robots->brain_fitness));
        nya_ui_label(ui, nya_string_hud_robots_dqn((u32)robots->dqn_steps, robots->dqn_score, (f64)robots->dqn_exploration * 100.0), GNY_ROBOT_DQN_TEXT);
        nya_ui_label(ui, nya_string_hud_robots_run(robots->runs + 1, nya_max(robots->record, robots->brain_fitness), robots->job_ms));
        nya_ui_panel_end(ui);
    }

    if (!NYA_CONFIG.game.robots.show_brain || robots->brain == nullptr) return;

    // dark, since the network's labels are drawn light.
    if (!nya_ui_panel_begin(ui, "brain", (NYA_UIPanel){ .width = nya_ui_fixed(GNY_ROBOT_PANEL_WIDTH), .fill = GNY_ROBOT_BRAIN_FILL })) return;

    NYA_Rectf area = nya_ui_space(ui, 0.0F, GNY_ROBOT_BRAIN_HEIGHT);
    nya_ui_panel_end(ui);

    // the HUD's own face at its small size and scale, so the labels share its glyph atlas instead of building another.
    NYA_UIStyle style = nya_ui_style_get(window);
    f32         scale = nya_ui_scale(window);

    nya_nn_neat_draw(window, robots->brain, (NYA_NeatDrawStyle){
        .x           = area.x,
        .y           = area.y,
        .width       = area.width,
        .height      = area.height,
        .node_radius = roundf(GNY_ROBOT_NODE_RADIUS * scale),
        .hide_values = true,
        .font        = nya_font_resolve(nya_font_named(style.font)).path,
        .font_size   = roundf(style.small_size * scale),
    });
}
