/**
 * @file layer_ui.c
 *
 * The HUD: what the world costs, and what the keys do.
 *
 * Pushed last, so it draws over everything, and drawn entirely in screen pixels — the game layer
 * resets the camera at the end of its own render precisely so this one does not have to know a
 * camera exists.
 * */
#include "gnyame/gnyame.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API DECLARATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

NYA_INTERNAL void _gny_ui_panel_draw(NYA_Window* window, f32 x, f32 y, f32 width, f32 height);

/** The previous frame's perf spans, indented by nesting depth. Toggled with `t`. */
NYA_INTERNAL void _gny_ui_trace_draw(NYA_Window* window);

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * ON CREATE
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

void gny_layer_ui_on_create(NYA_Window* window) {
    nya_unused(window);

    // Immediate mode state that persists across frames, so it is set once here rather than at the
    // top of every render. The font is rasterized on first use, not now, so this is safe before the
    // asset has finished loading.
    nya_render2d_font_set(GNY_UI_FONT, GNY_UI_FONT_SIZE);
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * ON DESTROY
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

void gny_layer_ui_on_destroy(NYA_Window* window) {
    nya_unused(window);
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * ON EVENT
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

void gny_layer_ui_on_event(NYA_Window* window, NYA_Event* event) {
    nya_unused(window);

    /*
     * Escape opens the pause menu. The only input this layer takes.
     *
     * It belongs here rather than in the game layer because it is about the application rather than
     * the world, and it has to live on a layer that is *below* the pause menu — once that menu is
     * pushed it sits above this one and answers the next escape itself, which is what makes one key
     * both open and close it.
     */
    if (event->type != NYA_EVENT_KEY_DOWN) return;
    if (event->as_key_event.is_repeat) return;

    const NYA_KeyEvent* key = &event->as_key_event;

    if (nya_input_action_matches(GNY_ACTION_TOGGLE_TRACE, key->key, key->modifier_flags)) {
        gny_world()->trace_enabled = !gny_world()->trace_enabled;
        event->was_handled         = true;
        return;
    }

    if (!nya_input_action_matches(NYA_INPUT_ACTION_PAUSE, key->key, key->modifier_flags)) return;

    gny_screen_pause();
    event->was_handled = true;
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * ON UPDATE
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

void gny_layer_ui_on_update(NYA_Window* window, f32 delta_time_s) {
    nya_unused(window, delta_time_s);
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * ON RENDER
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

void gny_layer_ui_on_render(NYA_Window* window) {
    GNY_World*     world = gny_world();
    NYA_FrameStats stats = nya_app_get()->frame_stats;

    f32 line   = nya_render2d_font_line_height();
    f32 origin = GNY_UI_MARGIN + GNY_UI_PADDING;

    /*
     * ── Stats, top left ──
     */

    const u32 stat_lines = 7;

    _gny_ui_panel_draw(window, GNY_UI_MARGIN, GNY_UI_MARGIN, 300.0F, (line * (f32)stat_lines) + (GNY_UI_PADDING * 2.0F));

    f32 y = origin;

    /*
     * Work and period, not one number called "ms/frame".
     *
     * The period is what fps is computed from and is held at 8.3 ms by the 120 fps limiter however
     * little the frame did — which reads as a slow frame and is the opposite. The work is what the
     * frame actually cost, and on this demo it is a fraction of a millisecond. See NYA_FrameStats.
     */
    nya_render2d_textf(window, origin, y, GNY_UI_TEXT, "%.0f fps   work %.2f ms   period %.2f ms", (f64)stats.fps,
                   nya_time_ns_to_s(stats.work_ns) * 1000.0, nya_time_ns_to_s(stats.elapsed_ns) * 1000.0);
    y += line;

    u32 awake = 0;
    u32 boxes = gny_entity_box_count(&awake);

    /*
     * Through the generated accessor rather than a literal.
     *
     * Awake beside the total because that is the number that costs: a settled pile is nearly free and
     * a pile that never sleeps is a bug you cannot see any other way. The reason it is translated is
     * narrower — `hud_boxes` is `"boxes %u (%u awake)"` and its German form reorders nothing, but the
     * build checks that it *could not*, and a HUD that never used a translated format string would
     * leave that check with nothing to check.
     */
    nya_render2d_text(window, nya_string_hud_boxes(boxes, awake), origin, y, GNY_UI_TEXT);
    y += line;

    nya_render2d_textf(window, origin, y, GNY_UI_DIM, "entities %u", nya_entity_count());
    y += line;

    /*
     * What the network is doing, which on this demo is always something.
     *
     * Single player is a server with no remote peers rather than a mode with the networking switched
     * off — see net.h — so there is always a peer count and it is always at least one. Showing it is
     * the cheapest way to make that architecture visible instead of merely true: open a second copy
     * with --connect and this number goes to two.
     */
    nya_render2d_text(window, nya_string_hud_players(nya_net_server_peer_count()), origin, y, GNY_UI_TEXT);
    y += line;

    nya_render2d_text(
        window,
        nya_net_server_is_listening() ? nya_string_hud_hosting(GNY_LAUNCH.listen_port) : nya_string_hud_offline(),
        origin, y, GNY_UI_DIM
    );
    y += line;

    /*
     * What the cursor is on, which is the entity hover feeding back.
     *
     * A highlight would be the prettier demonstration and a worse one: a number that changes as the
     * pointer crosses a crate shows that on_hover fired, while a colour change could equally be a
     * shader doing something clever.
     */
    NYA_Entity* hovered = nya_entity_get(nya_entity_hovered());

    nya_render2d_text(window, nya_string_hud_hovering(hovered != nullptr ? hovered->name : "-"), origin, y, GNY_UI_DIM);
    y += line;

    /*
     * Non-ASCII on the HUD, deliberately.
     *
     * Not decoration: it is the only thing in this build that proves the glyph atlas bakes a
     * codepoint nobody asked for at startup. Before the atlas grew past ASCII these five words drew
     * as a row of gaps, which is what every translated string would have done.
     */
    nya_render2d_text(window, "unicode: Grüße · l'été · años · Ελλάδα · Привет", origin, y, GNY_UI_DIM);
    y += nya_render2d_font_line_height();

    nya_render2d_textf(window, origin, y, GNY_UI_DIM, "bodies %u   spawned %u   lost %u", nya_physics2d_body_count(), world->boxes_spawned,
                   world->boxes_lost);
    y += line;

    nya_render2d_textf(window, origin, y, GNY_UI_DIM, "solver %.2f ms   hits %u", (f64)nya_physics2d_last_step_time_s() * 1000.0, world->hits);
    y += line;

    // Draw calls rather than vertices: a draw call is the number that can be acted on, and this
    // scene is built to stay in single digits — every crate is the same pipeline and no texture.
    NYA_Render2DFrameStats draw_stats = nya_render2d_frame_stats(window);
    nya_render2d_textf(window, origin, y, GNY_UI_DIM, "draw calls %u   verts %u", draw_stats.draw_calls, draw_stats.vertices);
    y += line;

    NYA_Camera2DTopDown camera = gny_entity_camera_get();

    // Says which of the two flags is in charge, since "the keys do nothing" and "the camera is
    // chasing something" look identical otherwise.
    NYA_ConstCString camera_mode = nya_entity_is_valid(gny_entity_camera_target(gny_entity_camera_primary())) ? "following" : "keys";

    nya_render2d_textf(window, origin, y, GNY_UI_DIM, "camera " FMTf32x2 " x%.2f  %s", FMTf32x2_ARG(camera.position), (f64)camera.zoom, camera_mode);
    y += line;

    /*
     * Three states, not two.
     *
     * "paused" and "never started" both read as silence, and telling them apart is the difference
     * between a key that did not register and a track that failed to decode. The track is streamed,
     * so a decode failure surfaces at play time rather than at load.
     */
    NYA_ConstCString music = !world->music_started ? "loading" : (nya_audio_music_playing() ? "playing" : "paused");

    nya_render2d_textf(window, origin, y, GNY_UI_DIM, "music %s   bloom %s", music, world->bloom_enabled ? "on" : "off");

    /*
     * ── Bindings, bottom left ──
     */

    NYA_ConstCString bindings[] = {
        "left click   drop a box",
        "right click  remove a box",
        "middle click watch a box (inset)",
        "space        drop a burst",
        "c / r        clear / regenerate",
        "wasd, wheel  pan / zoom",
        "p / m        pause physics / music",
        "b / t        bloom / frame trace",
        "escape       pause menu",
    };

    const u32 binding_count = (u32)(sizeof(bindings) / sizeof(bindings[0]));

    f32 bindings_height = (line * (f32)binding_count) + (GNY_UI_PADDING * 2.0F);
    f32 bindings_top    = (f32)window->screen_height - GNY_UI_MARGIN - bindings_height;

    _gny_ui_panel_draw(window, GNY_UI_MARGIN, bindings_top, 300.0F, bindings_height);

    y = bindings_top + GNY_UI_PADDING;
    for (u32 i = 0; i < binding_count; i++) {
        nya_render2d_text(window, bindings[i], origin, y, GNY_UI_DIM);
        y += line;
    }

    _gny_ui_trace_draw(window);

    /*
     * ── Paused banner ──
     */

    if (!nya_physics2d_enabled()) {
        NYA_ConstCString text = "PHYSICS PAUSED";

        f32 text_width = nya_render2d_text_width(text);
        f32 x          = ((f32)window->screen_width - text_width) * 0.5F;

        _gny_ui_panel_draw(window, x - GNY_UI_PADDING, GNY_UI_MARGIN, text_width + (GNY_UI_PADDING * 2.0F), line + (GNY_UI_PADDING * 2.0F));
        nya_render2d_text(window, text, x, GNY_UI_MARGIN + GNY_UI_PADDING, GNY_UI_WARNING);
    }
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

void _gny_ui_panel_draw(NYA_Window* window, f32 x, f32 y, f32 width, f32 height) {
    nya_render2d_rect(window, x, y, width, height, GNY_UI_PANEL);
    nya_render2d_rect_outline(window, x, y, width, height, 1.0F, GNY_UI_BORDER);
}

void _gny_ui_trace_draw(NYA_Window* window) {
    if (!gny_world()->trace_enabled) return;

    /*
     * The frame *before* this one, because this one has not finished.
     *
     * A span is only collectable once it has closed, and the outermost "frame" span closes after the
     * render that is drawing this panel. Asking for the previous frame is the difference between a
     * complete breakdown and one missing everything that encloses it.
     */
    u64 frame = nya_perf_frame_current();
    if (frame == 0) return;

    // From the frame allocator: rebuilt every frame and freed with it, which is what that arena is for.
    NYA_ArrayᐸNYA_PerfSpanᐳ* spans = nya_array_create(nya_app_get()->frame_allocator, NYA_PerfSpan);

    u32 count = nya_perf_frame_spans(frame - 1, spans);

    // Zero in a shipping build, where the timers are compiled out entirely. Drawing nothing is right
    // there rather than an empty panel implying the frame did nothing.
    if (count == 0) return;

    f32 line  = nya_render2d_font_line_height();
    u32 drawn = count < GNY_TRACE_MAX_SPANS ? count : GNY_TRACE_MAX_SPANS;

    f32 height = (line * (f32)(drawn + 1)) + (GNY_UI_PADDING * 2.0F);
    f32 x      = (f32)window->screen_width - GNY_UI_MARGIN - GNY_TRACE_WIDTH;
    f32 y      = GNY_UI_MARGIN;

    _gny_ui_panel_draw(window, x, y, GNY_TRACE_WIDTH, height);

    f32 text_x = x + GNY_UI_PADDING;
    f32 text_y = y + GNY_UI_PADDING;

    /*
     * Percentages are against the frame's *work*, not against the outermost span.
     *
     * The "frame" span encloses the limiter's SDL_DelayNS, so at a 120 fps cap it reads 8.4 ms while
     * everything inside it sums to a fifth of a millisecond — dividing by it made every real cost
     * show as one or two percent and hid which of them actually dominated. Work is the number those
     * spans are competing for.
     */
    NYA_FrameStats frame_stats = nya_app_get()->frame_stats;

    u64 total_ns = frame_stats.work_ns;
    if (total_ns == 0) total_ns = 1;

    nya_render2d_textf(window, text_x, text_y, GNY_UI_TEXT, "trace  %u spans   work %.3f ms   slept %.2f ms", count,
                   nya_time_ns_to_s(total_ns) * 1000.0, nya_time_ns_to_s(frame_stats.sleep_ns) * 1000.0);
    text_y += line;

    for (u32 i = 0; i < drawn; i++) {
        const NYA_PerfSpan* span = &spans->items[i];

        f32 fraction = (f32)((f64)span->elapsed_ns / (f64)total_ns);

        // Anything taking a quarter of the frame is where the eye should go first. On an idle demo
        // nothing but the frame span itself qualifies, which is the point.
        NYA_Color color = fraction >= GNY_TRACE_HOT_FRACTION ? GNY_UI_WARNING : GNY_UI_DIM;

        nya_render2d_textf(window, text_x + (GNY_TRACE_INDENT * (f32)span->depth), text_y, color, "%s", span->name);

        // Right aligned, so the numbers form a column that can be scanned rather than read.
        char timing[64];
        (void)snprintf(timing, sizeof(timing), "%.3f ms  %4.1f%%", nya_time_ns_to_s(span->elapsed_ns) * 1000.0, (f64)(fraction * 100.0F));

        nya_render2d_text(window, timing, x + GNY_TRACE_WIDTH - GNY_UI_PADDING - nya_render2d_text_width(timing), text_y, color);

        text_y += line;
    }
}

