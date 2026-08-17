#include "nyangine/nyangine.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * STATE
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/**
 * A ring of recent frame times, in milliseconds.
 *
 * A module global rather than something on NYA_App, because the overlay is a developer tool and its
 * history has no meaning to the engine. It is also per process rather than per window: two windows
 * share a frame loop, so two histories would be two views of the same thing.
 * */
NYA_INTERNAL f32 _nya_debug_frame_times_ms[NYA_DEBUG_OVERLAY_HISTORY] = { 0 };

/** Where the next sample goes. Wraps; the ring is full once `_nya_debug_sample_count` says so. */
NYA_INTERNAL u32 _nya_debug_frame_cursor = 0;

/** Samples taken, saturating at the ring size. Distinguishes "empty slot" from "a 0 ms frame". */
NYA_INTERNAL u32 _nya_debug_sample_count = 0;

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API DECLARATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/** Fills anything the caller left at zero with a sensible default. */
NYA_INTERNAL void _nya_debug_overlay_apply_style_defaults(NYA_DebugOverlayStyle* style);

/**
 * Bytes as a fixed width string, in whichever unit keeps it readable.
 *
 * Returns a pointer into a small ring of static buffers, so several calls can be alive in one
 * snprintf without clobbering each other. Not thread safe, which is correct for a HUD.
 * */
NYA_INTERNAL NYA_ConstCString _nya_debug_format_bytes(u64 bytes) __attr_no_discard;

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PUBLIC API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

f32 nya_debug_frame_time_average_ms(void) {
    if (_nya_debug_sample_count == 0) return 0.0F;

    f32 total = 0.0F;
    for (u32 i = 0; i < _nya_debug_sample_count; i++) total += _nya_debug_frame_times_ms[i];

    return total / (f32)_nya_debug_sample_count;
}

f32 nya_debug_frame_time_worst_ms(void) {
    f32 worst = 0.0F;
    for (u32 i = 0; i < _nya_debug_sample_count; i++) worst = nya_max(worst, _nya_debug_frame_times_ms[i]);

    return worst;
}

void nya_debug_overlay_draw(NYA_Window* window, NYA_DebugOverlayStyle style) {
    nya_assert(window != nullptr);

    _nya_debug_overlay_apply_style_defaults(&style);

    NYA_FrameStats* frame = &nya_app_get()->frame_stats;

    /*
     * Two different numbers, and the distinction is the point of the overlay.
     *
     * **Work** is frame_end_time_ns minus frame_start_time_ns: update, render, present. The frame
     * loop stamps frame_end *before* the frame rate limiter sleeps, so this excludes the sleep and
     * is the only one of the two that responds to making the game faster.
     *
     * **Wall** is elapsed_ns, start of one frame to the start of the next. It includes the limiter
     * sleep and the wait for vsync, so on a capped run it sits at the cap however little work is
     * being done — which makes it useless as a cost measure and exactly right as a "are frames
     * arriving on time" measure.
     *
     * Neither is delta_time_s, which is the *fixed simulation step* and reads 16ms forever. Sampling
     * that produced a graph where current, average and worst were all exactly 16.00 and never moved.
     *
     * The work figure is one frame stale: the overlay draws during render, so this frame's end has
     * not been stamped yet and frame_end still belongs to the previous one. A frame late is the
     * right trade against measuring only the part of the frame that happens before the overlay.
     */
    f32 work_ms = 0.0F;
    if (frame->frame_end_time_ns > frame->prev_frame_time_ns) {
        work_ms = (f32)nya_time_ns_to_s(frame->frame_end_time_ns - frame->prev_frame_time_ns) * 1000.0F;
    }

    f32 wall_ms = (f32)nya_time_ns_to_s(frame->elapsed_ns) * 1000.0F;

    // The graph and the averages track work, because that is the number a change to the game moves.
    f32 current_ms = work_ms;

    _nya_debug_frame_times_ms[_nya_debug_frame_cursor] = current_ms;
    _nya_debug_frame_cursor                            = (_nya_debug_frame_cursor + 1) % NYA_DEBUG_OVERLAY_HISTORY;

    if (_nya_debug_sample_count < NYA_DEBUG_OVERLAY_HISTORY) _nya_debug_sample_count++;

    /*
     * The printed figures are latched, and only re-read every NYA_DEBUG_OVERLAY_REFRESH_SECONDS.
     *
     * Everything above still runs every frame, so the history and the graph lose nothing. What this
     * fixes is the display: at two hundred frames a second the work figure changed faster than it
     * could be read, and the whole panel shimmered.
     *
     * Latched on the app's uptime rather than by counting frames, so the refresh rate is the same
     * whatever the frame rate is — which is the entire point.
     */
    static f32 latched_work_ms = 0.0F;
    static f32 latched_wall_ms = 0.0F;
    static f32 latched_fps     = 0.0F;
    static f32 latched_average = 0.0F;
    static f32 latched_worst   = 0.0F;
    static f32 next_refresh_s  = 0.0F;

    f32 uptime_s = nya_app_get()->frame_stats.uptime_s;

    if (uptime_s >= next_refresh_s) {
        next_refresh_s = uptime_s + NYA_DEBUG_OVERLAY_REFRESH_SECONDS;

        latched_work_ms = work_ms;
        latched_wall_ms = wall_ms;
        latched_fps     = frame->fps;
        latched_average = nya_debug_frame_time_average_ms();
        latched_worst   = nya_debug_frame_time_worst_ms();
    }
    f32 average_ms = latched_average;
    f32 worst_ms   = latched_worst;

    work_ms = latched_work_ms;
    wall_ms = latched_wall_ms;

    NYA_Render2DFrameStats draw_stats = nya_render2d_frame_stats(window);

    /*
     * Laid out from the text metrics rather than from constants, so the panel fits whatever font the
     * caller set rather than assuming the one this was written against.
     */
    f32 line_height = nya_render2d_font_line_height();
    if (line_height <= 0.0F) line_height = 16.0F;

    f32 padding = 8.0F;

    /*
     * The arenas worth showing, biggest first.
     *
     * Selected here rather than while drawing, because the panel has to be sized before anything is
     * drawn into it and the count is not known until the registry has been walked. A partial
     * selection sort over the registry: NYA_DEBUG_OVERLAY_ARENAS passes over at most
     * NYA_ARENA_REGISTRY_MAX entries, which is cheaper than sorting the whole registry to show six
     * of it.
     */
    NYA_ArenaStats memory[NYA_DEBUG_OVERLAY_ARENAS] = { 0 };

    u32 memory_count = 0;
    u64 memory_total = 0;

    if (!style.hide_memory) {
        u32 registry_count = nya_arena_registry_count();

        for (u32 i = 0; i < registry_count; i++) {
            NYA_Arena* arena = nya_arena_registry_at(i);
            if (arena == nullptr) continue;

            NYA_ArenaStats stats = nya_arena_stats(arena);
            memory_total        += stats.used_bytes;

            // Unnamed arenas are scratch, created and destroyed inside one call. Naming is what
            // marks an arena as something with a lifetime worth watching.
            if (stats.name == nullptr) continue;

            // Insertion into a list kept in descending order, dropping off the end.
            u32 slot = memory_count < NYA_DEBUG_OVERLAY_ARENAS ? memory_count : NYA_DEBUG_OVERLAY_ARENAS - 1;
            if (memory_count >= NYA_DEBUG_OVERLAY_ARENAS && stats.used_bytes <= memory[slot].used_bytes) continue;

            memory[slot] = stats;
            if (memory_count < NYA_DEBUG_OVERLAY_ARENAS) memory_count++;

            for (u32 j = slot; j > 0 && memory[j].used_bytes > memory[j - 1].used_bytes; j--) {
                NYA_ArenaStats swap = memory[j];
                memory[j]           = memory[j - 1];
                memory[j - 1]       = swap;
            }
        }
    }

    u32 line_count = 2;
    if (!style.hide_draw_stats) line_count++;
    if (style.show_batch_breakdown) line_count++;
    if (!style.hide_memory) line_count += memory_count + 1;

    f32 panel_width  = style.width + (padding * 2.0F);
    f32 panel_height = (line_height * (f32)line_count) + (padding * 2.0F);
    if (!style.hide_graph) panel_height += style.height + padding;

    if (style.background.a > 0.0F) nya_render2d_rect(window, style.x, style.y, panel_width, panel_height, style.background);

    f32 text_x = style.x + padding;
    f32 text_y = style.y + padding;

    /*
     * Every number is padded to a fixed width.
     *
     * The font is proportional, so a value going from 9.9 to 10.0 changes the string's pixel width
     * and everything after it slides sideways. At sixty frames a second that is not a readout, it is
     * a fidget — and the eye tracks the movement instead of the value. Padding to the widest form
     * each field can take keeps the columns still.
     */
    nya_render2d_textf_with_font(
        window,
        style.font,
        style.font_size,
        text_x,
        text_y,
        style.text_color,
        "%7.2f ms work  %7.2f wall  %4.0f fps",
        (f64)work_ms,
        (f64)wall_ms,
        (f64)latched_fps
    );
    text_y += line_height;

    // The worst frame in the window, beside the average — an average alone hides exactly the hitch
    // that makes a run feel broken.
    nya_render2d_textf_with_font(window, style.font, style.font_size, text_x, text_y, style.text_color, "avg %7.2f     worst %7.2f", (f64)average_ms, (f64)worst_ms);
    text_y += line_height;

    if (!style.hide_draw_stats) {
        nya_render2d_textf_with_font(window, style.font, style.font_size, text_x, text_y, style.text_color, "%5u draws   %7u verts", draw_stats.draw_calls, draw_stats.vertices);
        text_y += line_height;
    }

    if (style.show_batch_breakdown) {
        /*
         * The single largest reason, not all six.
         *
         * A breakdown of every reason is a table, and a table does not belong in a HUD. The one that
         * dominates is the one worth acting on, and the counts behind it are available from
         * nya_render2d_frame_stats for anyone who wants the rest.
         */
        NYA_Render2DFlushReason worst_reason = NYA_RENDER2D_FLUSH_FRAME_END;
        u32                 worst_count  = 0;

        for (u32 i = 0; i < NYA_RENDER2D_FLUSH_REASON_COUNT; i++) {
            if (draw_stats.draw_calls_by_reason[i] <= worst_count) continue;

            worst_count  = draw_stats.draw_calls_by_reason[i];
            worst_reason = (NYA_Render2DFlushReason)i;
        }

        // Red once anything is being dropped: a draw that produced nothing is the one number here
        // that is a bug rather than a cost.
        NYA_Color color = style.text_color;
        if (draw_stats.dropped_draws > 0) color = (NYA_Color){ 0.95F, 0.45F, 0.45F, 1.0F };

        nya_render2d_textf_with_font(
            window,
            style.font,
            style.font_size,
            text_x,
            text_y,
            color,
            "%5ux %-10s %5u dropped",
            worst_count,
            nya_render2d_flush_reason_name(worst_reason),
            draw_stats.dropped_draws
        );
        text_y += line_height;
    }

    if (!style.hide_memory) {
        nya_render2d_textf_with_font(window, style.font, style.font_size, text_x, text_y, style.text_color, "mem %9s total", _nya_debug_format_bytes(memory_total));
        text_y += line_height;

        for (u32 i = 0; i < memory_count; i++) {
            // Name left aligned and size right aligned in a fixed field, so the sizes form a column
            // that can be compared down rather than a ragged edge that has to be read one by one.
            nya_render2d_textf_with_font(
                window,
                style.font,
                style.font_size,
                text_x,
                text_y,
                (NYA_Color){ 0.72F, 0.76F, 0.82F, 1.0F },
                "  %-20s %9s",
                memory[i].name,
                _nya_debug_format_bytes(memory[i].used_bytes)
            );
            text_y += line_height;
        }
    }

    if (style.hide_graph) return;

    f32 graph_x = style.x + padding;
    f32 graph_y = text_y;

    nya_render2d_rect(window, graph_x, graph_y, style.width, style.height, (NYA_Color){ 0.0F, 0.0F, 0.0F, 0.35F });

    /*
     * One column per sample, oldest on the left.
     *
     * Drawn as bars rather than a line because a line between two samples implies the frame time
     * passed through the values in between, and it did not — each sample is a whole frame.
     */
    f32 column_width = style.width / (f32)NYA_DEBUG_OVERLAY_HISTORY;

    for (u32 i = 0; i < _nya_debug_sample_count; i++) {
        // Read back from the cursor so the newest sample is on the right, whatever the ring's
        // internal rotation happens to be.
        u32 index  = (_nya_debug_frame_cursor + NYA_DEBUG_OVERLAY_HISTORY - _nya_debug_sample_count + i) % NYA_DEBUG_OVERLAY_HISTORY;
        f32 sample = _nya_debug_frame_times_ms[index];

        f32 fraction = nya_clamp(sample / style.graph_ceiling_ms, 0.0F, 1.0F);
        f32 bar      = fraction * style.height;

        /*
         * Green up to half the ceiling, amber to three quarters, red past it.
         *
         * Colour rather than a threshold line, because the question a glance asks is "is this frame
         * fine", and a bar's height alone does not answer it without reading the scale.
         */
        NYA_Color color = (NYA_Color){ 0.35F, 0.85F, 0.45F, 0.9F };
        if (fraction > 0.75F) color = (NYA_Color){ 0.95F, 0.35F, 0.35F, 0.9F };
        else if (fraction > 0.5F) color = (NYA_Color){ 0.95F, 0.75F, 0.3F, 0.9F };

        // From the bottom up, which is how a bar chart of "how long did this take" reads.
        nya_render2d_rect(window, graph_x + ((f32)i * column_width), graph_y + (style.height - bar), nya_max(column_width, 1.0F), bar, color);
    }
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

NYA_ConstCString _nya_debug_format_bytes(u64 bytes) {
    // A ring, because a single buffer breaks the moment two of these appear in one format string.
    static char buffers[4][24] = { 0 };
    static u32  next           = 0;

    char* buffer = buffers[next];
    next         = (next + 1) % nya_carray_length(buffers);

    // One decimal from KiB up: whole bytes are exact and a fraction of one is meaningless, while
    // "12 MiB" hides the difference between 12.0 and 12.9 which is exactly what a leak looks like.
    if (bytes >= nya_gibyte_to_byte(1ULL)) {
        (void)snprintf(buffer, sizeof(buffers[0]), "%.1f GiB", (f64)bytes / (f64)nya_gibyte_to_byte(1ULL));
    } else if (bytes >= nya_mebyte_to_byte(1ULL)) {
        (void)snprintf(buffer, sizeof(buffers[0]), "%.1f MiB", (f64)bytes / (f64)nya_mebyte_to_byte(1ULL));
    } else if (bytes >= nya_kibyte_to_byte(1ULL)) {
        (void)snprintf(buffer, sizeof(buffers[0]), "%.1f KiB", (f64)bytes / (f64)nya_kibyte_to_byte(1ULL));
    } else {
        (void)snprintf(buffer, sizeof(buffers[0]), "%llu B", (unsigned long long)bytes);
    }

    return buffer;
}

void _nya_debug_overlay_apply_style_defaults(NYA_DebugOverlayStyle* style) {
    if (style->width <= 0.0F) style->width = 300.0F;
    if (style->height <= 0.0F) style->height = 48.0F;

    // 33.3ms is two frames at 60Hz. A frame reaching the top of the graph has missed its deadline
    // twice over, which is the right thing for "the top" to mean.
    if (style->graph_ceiling_ms <= 0.0F) style->graph_ceiling_ms = 33.3F;

    if (style->font == nullptr) style->font = nya_render2d_font_get();
    if (style->font_size <= 0.0F) style->font_size = nya_render2d_font_size_get();

    // A fully transparent colour is what a zeroed struct gives and never what a caller means, so it
    // reads as unspecified. A caller genuinely wanting no panel sets a colour with zero alpha
    // explicitly — which lands here too, so the panel is simply skipped by the alpha check above.
    if (style->text_color.a == 0.0F) style->text_color = (NYA_Color){ 0.92F, 0.94F, 0.97F, 1.0F };
    if (style->background.a == 0.0F && style->background.r == 0.0F && style->background.g == 0.0F && style->background.b == 0.0F) {
        style->background = (NYA_Color){ 0.04F, 0.05F, 0.07F, 0.78F };
    }
}
