#include "nyangine/nyangine.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * STATE
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/**
 * Recent frame times in milliseconds. Module global since the engine has no use for it, and per
 * process since windows share a frame loop.
 * */
NYA_INTERNAL f32 _nya_debug_frame_times_ms[NYA_DEBUG_OVERLAY_HISTORY] = { 0 };

/** Where the next sample goes. Wraps. */
NYA_INTERNAL u32 _nya_debug_frame_cursor = 0;

/** Samples taken, saturating at the ring size, so an empty slot differs from a 0 ms frame. */
NYA_INTERNAL u32 _nya_debug_sample_count = 0;

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API DECLARATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/** Fills anything the caller left at zero with a sensible default. */
NYA_INTERNAL void _nya_debug_overlay_apply_style_defaults(NYA_DebugOverlayStyle* style);

/**
 * Bytes as a fixed width string in a readable unit. Returns one of a few static buffers, so several
 * calls work in one format string. Not thread safe.
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
     * Work is frame_end - frame_start, stamped before the limiter sleeps, so it follows the game's cost.
     * Wall time includes the sleep and sits at the cap. delta_time_s is the fixed step and never moves.
     * Work is one frame stale here because the overlay draws before this frame's end is stamped.
     */
    f32 work_ms = 0.0F;
    if (frame->frame_end_time_ns > frame->prev_frame_time_ns) {
        work_ms = (f32)nya_time_ns_to_s(frame->frame_end_time_ns - frame->prev_frame_time_ns) * 1000.0F;
    }

    f32 wall_ms = (f32)nya_time_ns_to_s(frame->elapsed_ns) * 1000.0F;

    // the graph and averages track work, the number a change to the game moves.
    f32 current_ms = work_ms;

    _nya_debug_frame_times_ms[_nya_debug_frame_cursor] = current_ms;
    _nya_debug_frame_cursor                            = (_nya_debug_frame_cursor + 1) % NYA_DEBUG_OVERLAY_HISTORY;

    if (_nya_debug_sample_count < NYA_DEBUG_OVERLAY_HISTORY) _nya_debug_sample_count++;

    // printed figures refresh every NYA_DEBUG_OVERLAY_REFRESH_SECONDS of uptime; the graph samples every
    // frame.
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

    // laid out from text metrics, so the panel fits the caller's font.
    f32 line_height = nya_render2d_font_line_height();
    if (line_height <= 0.0F) line_height = 16.0F;

    f32 padding = 8.0F;

    // the biggest arenas, picked before drawing since the panel is sized first. A partial selection
    // sort, cheaper than sorting the registry to show six.
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

            // unnamed arenas are scratch inside one call.
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

    // byte gauges, such as the GPU's, below the arenas. few enough to show all of them.
    u32 gauge_count = style.hide_memory ? 0 : nya_gauge_count();

    // the registry returns entries fullest first, so the first few rows are the ones to show.
    u32 ceiling_count = 0;

    if (!style.hide_ceilings) ceiling_count = nya_min(nya_ceiling_count(), (u32)NYA_DEBUG_OVERLAY_CEILINGS);

    u32 line_count = 2;
    if (!style.hide_draw_stats) line_count++;
    if (style.show_batch_breakdown) line_count++;
    if (!style.hide_memory) line_count += memory_count + 1 + gauge_count;
    if (ceiling_count > 0) line_count += ceiling_count + 1;

    f32 panel_width  = style.width + (padding * 2.0F);
    f32 panel_height = (line_height * (f32)line_count) + (padding * 2.0F);
    if (!style.hide_graph) panel_height += style.height + padding;

    if (style.background.a > 0.0F) nya_render2d_rect(window, style.x, style.y, panel_width, panel_height, style.background);

    f32 text_x = style.x + padding;
    f32 text_y = style.y + padding;

    // fixed width numbers: the font is proportional, so 9.9 to 10.0 would shift the row every frame.
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

    // worst beside average, since an average hides the hitch.
    nya_render2d_textf_with_font(window, style.font, style.font_size, text_x, text_y, style.text_color, "avg %7.2f     worst %7.2f", (f64)average_ms, (f64)worst_ms);
    text_y += line_height;

    if (!style.hide_draw_stats) {
        nya_render2d_textf_with_font(window, style.font, style.font_size, text_x, text_y, style.text_color, "%5u draws   %7u verts", draw_stats.draw_calls, draw_stats.vertices);
        text_y += line_height;
    }

    if (style.show_batch_breakdown) {
        // only the largest reason. The full breakdown is in nya_render2d_frame_stats.
        NYA_Render2DFlushReason worst_reason = NYA_RENDER2D_FLUSH_FRAME_END;
        u32                 worst_count  = 0;

        for (u32 i = 0; i < NYA_RENDER2D_FLUSH_REASON_COUNT; i++) {
            if (draw_stats.draw_calls_by_reason[i] <= worst_count) continue;

            worst_count  = draw_stats.draw_calls_by_reason[i];
            worst_reason = (NYA_Render2DFlushReason)i;
        }

        // red once anything is dropped: that is a bug, not a cost.
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
            // sizes in a fixed field, so they line up as a column.
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

        // counted by the engine at every create and release, not read from the driver.
        for (u32 i = 0; i < gauge_count; i++) {
            nya_render2d_textf_with_font(
                window,
                style.font,
                style.font_size,
                text_x,
                text_y,
                (NYA_Color){ 0.72F, 0.76F, 0.82F, 1.0F },
                "  %-20s %9s",
                nya_gauge_name_at(i),
                _nya_debug_format_bytes(nya_gauge_bytes_at(i))
            );
            text_y += line_height;
        }
    }

    if (ceiling_count > 0) {
        nya_render2d_textf_with_font(
            window,
            style.font,
            style.font_size,
            text_x,
            text_y,
            style.text_color,
            "ceilings %5u tracked",
            nya_ceiling_count()
        );
        text_y += line_height;

        for (u32 i = 0; i < ceiling_count; i++) {
            u32 capacity = nya_ceiling_capacity_at(i);
            u32 live     = nya_ceiling_live_at(i);

            f32 fullness = capacity > 0 ? (f32)live / (f32)capacity : 0.0F;

            /*
             * The graph's three bands. A full ceiling has already refused something and logged it, so amber
             * means there is still time to raise the number.
             */
            NYA_Color color = (NYA_Color){ 0.72F, 0.76F, 0.82F, 1.0F };

            if (fullness >= 0.9F) color = (NYA_Color){ 0.95F, 0.45F, 0.45F, 1.0F };
            else if (fullness >= 0.75F) color = (NYA_Color){ 0.95F, 0.80F, 0.45F, 1.0F };

            // live and capacity rather than a percentage, so the row says what to raise and by how much.
            nya_render2d_textf_with_font(
                window,
                style.font,
                style.font_size,
                text_x,
                text_y,
                color,
                "  %-20s %5u/%-5u %3.0f%%",
                nya_ceiling_name_at(i),
                live,
                capacity,
                (f64)(fullness * 100.0F)
            );
            text_y += line_height;
        }
    }

    if (style.hide_graph) return;

    f32 graph_x = style.x + padding;
    f32 graph_y = text_y;

    nya_render2d_rect(window, graph_x, graph_y, style.width, style.height, (NYA_Color){ 0.0F, 0.0F, 0.0F, 0.35F });

    // one bar per sample, oldest on the left. Bars, since a line implies values in between.
    f32 column_width = style.width / (f32)NYA_DEBUG_OVERLAY_HISTORY;

    for (u32 i = 0; i < _nya_debug_sample_count; i++) {
        // read from the cursor so the newest sample is on the right.
        u32 index  = (_nya_debug_frame_cursor + NYA_DEBUG_OVERLAY_HISTORY - _nya_debug_sample_count + i) % NYA_DEBUG_OVERLAY_HISTORY;
        f32 sample = _nya_debug_frame_times_ms[index];

        f32 fraction = nya_clamp(sample / style.graph_ceiling_ms, 0.0F, 1.0F);
        f32 bar      = fraction * style.height;

        /*
         * Green up to half the ceiling, amber to three quarters, red past it.
         */
        NYA_Color color = (NYA_Color){ 0.35F, 0.85F, 0.45F, 0.9F };
        if (fraction > 0.75F) color = (NYA_Color){ 0.95F, 0.35F, 0.35F, 0.9F };
        else if (fraction > 0.5F) color = (NYA_Color){ 0.95F, 0.75F, 0.3F, 0.9F };

        // from the bottom up.
        nya_render2d_rect(window, graph_x + ((f32)i * column_width), graph_y + (style.height - bar), nya_max(column_width, 1.0F), bar, color);
    }
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

NYA_ConstCString _nya_debug_format_bytes(u64 bytes) {
    // a ring, so two calls can appear in one format string.
    static char buffers[4][24] = { 0 };
    static u32  next           = 0;

    char* buffer = buffers[next];
    next         = (next + 1) % nya_carray_length(buffers);

    // one decimal from KiB up, since 12.0 vs 12.9 MiB is what a leak looks like.
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

    // 33.3 ms is two frames at 60 Hz, so the top of the graph means a deadline missed twice.
    if (style->graph_ceiling_ms <= 0.0F) style->graph_ceiling_ms = 33.3F;

    if (style->font == nullptr) style->font = nya_render2d_font_get();
    if (style->font_size <= 0.0F) style->font_size = nya_render2d_font_size_get();

    // a zeroed struct gives transparent black, which means unset. A deliberate zero alpha panel is
    // skipped by the alpha check anyway.
    if (style->text_color.a == 0.0F) style->text_color = (NYA_Color){ 0.92F, 0.94F, 0.97F, 1.0F };
    if (style->background.a == 0.0F && style->background.r == 0.0F && style->background.g == 0.0F && style->background.b == 0.0F) {
        style->background = (NYA_Color){ 0.04F, 0.05F, 0.07F, 0.78F };
    }
}
