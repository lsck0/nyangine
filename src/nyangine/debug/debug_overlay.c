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

/**
 * The memory section as of the last refresh. Latched with the printed figures, since asking the system which pages
 * are resident is a call per region.
 * */
typedef struct {
    NYA_ArenaStats arenas[NYA_DEBUG_OVERLAY_ARENAS];
    u64            resident[NYA_DEBUG_OVERLAY_ARENAS];
    u32            arena_count;

    /** Every arena's used bytes, named or not. */
    u64 used_total;

    /** The process's resident set, so the rows can be read against it. */
    u64 process_resident;
} _NYA_DebugMemorySample;

NYA_INTERNAL _NYA_DebugMemorySample _nya_debug_memory = { 0 };

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API DECLARATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/** Fills anything the caller left at zero with a sensible default. */
NYA_INTERNAL void _nya_debug_overlay_apply_style_defaults(NYA_DebugOverlayStyle* style);

/** Picks the largest named arenas into _nya_debug_memory, with their resident bytes and the process total. */
NYA_INTERNAL void _nya_debug_memory_sample(void);

/** The frame time graph, its top left corner at `x`, `y`. */
NYA_INTERNAL void _nya_debug_overlay_graph_draw(NYA_Window* window, const NYA_DebugOverlayStyle* style, f32 x, f32 y);

/** The trace page: the table, a total, and the graph. */
NYA_INTERNAL void _nya_debug_overlay_trace_draw(NYA_Window* window, const NYA_DebugOverlayStyle* style, f32 work_ms, b8 refresh);

/** The systems page: one row per registered system, in run order. */
NYA_INTERNAL void _nya_debug_overlay_systems_draw(NYA_Window* window, const NYA_DebugOverlayStyle* style);

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

    b8 refresh = uptime_s >= next_refresh_s;

    if (refresh) {
        next_refresh_s = uptime_s + NYA_DEBUG_OVERLAY_REFRESH_SECONDS;

        latched_work_ms = work_ms;
        latched_wall_ms = wall_ms;
        latched_fps     = frame->fps;
        latched_average = nya_debug_frame_time_average_ms();
        latched_worst   = nya_debug_frame_time_worst_ms();

        if (!style.hide_memory && style.page == NYA_DEBUG_OVERLAY_PAGE_STATS) _nya_debug_memory_sample();
    }
    f32 average_ms = latched_average;
    f32 worst_ms   = latched_worst;

    work_ms = latched_work_ms;
    wall_ms = latched_wall_ms;

    if (style.page == NYA_DEBUG_OVERLAY_PAGE_TRACE) {
        _nya_debug_overlay_trace_draw(window, &style, work_ms, refresh);
        return;
    }

    // Per system timing runs while the page that shows it is up, the way tracing runs while the trace
    // page is up: two clock reads per system per phase is not a bill to pay for numbers nobody reads.
    b8 wants_accounting = style.page == NYA_DEBUG_OVERLAY_PAGE_SYSTEMS;

    if (nya_system_accounting_is_enabled() != wants_accounting) {
        if (wants_accounting) nya_system_accounting_enable();
        else nya_system_accounting_disable();
    }

    if (style.page == NYA_DEBUG_OVERLAY_PAGE_SYSTEMS) {
        _nya_debug_overlay_systems_draw(window, &style);
        return;
    }

    NYA_Render2DFrameStats draw_stats = nya_render2d_frame_stats(window);

    // laid out from text metrics, so the panel fits the caller's font.
    f32 line_height = nya_render2d_font_line_height();
    if (line_height <= 0.0F) line_height = 16.0F;

    f32 padding = 8.0F;

    const _NYA_DebugMemorySample* memory       = &_nya_debug_memory;
    u32                           memory_count = memory->arena_count;

    // byte gauges, such as the GPU's, below the arenas. few enough to show all of them.
    u32 gauge_count = style.hide_memory ? 0 : nya_gauge_count();

    // the registry returns entries fullest first, so the first few rows are the ones to show.
    u32 ceiling_count = 0;

    if (!style.hide_ceilings) ceiling_count = nya_min(nya_ceiling_count(), (u32)NYA_DEBUG_OVERLAY_CEILINGS);

    char net_line[NYA_NET_STATS_LINE_MAX];
    b8   has_net = nya_net_stats_line(net_line, sizeof(net_line));

    u32 line_count = has_net ? 3 : 2;
    if (!style.hide_draw_stats) line_count += 2;
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

    if (has_net) {
        nya_render2d_textf_with_font(window, style.font, style.font_size, text_x, text_y, style.text_color, "%s", net_line);
        text_y += line_height;
    }

    if (!style.hide_draw_stats) {
        nya_render2d_textf_with_font(window, style.font, style.font_size, text_x, text_y, style.text_color, "%5u draws   %7u verts", draw_stats.draw_calls, draw_stats.vertices);
        text_y += line_height;

        // the whole renderer's last finished frame, 3D and post passes included, so a batching regression shows here.
        NYA_RenderFrameStats frame_stats = nya_render_frame_stats(window);

        nya_render2d_textf_with_font(window, style.font, style.font_size, text_x, text_y, style.text_color, "%5u gpu draws %3u passes %9s up",
                                     frame_stats.draw_calls, frame_stats.passes, _nya_debug_format_bytes(frame_stats.upload_bytes));
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
        nya_render2d_textf_with_font(
            window,
            style.font,
            style.font_size,
            text_x,
            text_y,
            style.text_color,
            "mem %9s used %9s rss",
            _nya_debug_format_bytes(memory->used_total),
            _nya_debug_format_bytes(memory->process_resident)
        );
        text_y += line_height;

        for (u32 i = 0; i < memory_count; i++) {
            // sizes in a fixed field, so they line up as a column. used, then how much of it is resident.
            nya_render2d_textf_with_font(
                window,
                style.font,
                style.font_size,
                text_x,
                text_y,
                (NYA_Color){ 0.72F, 0.76F, 0.82F, 1.0F },
                "  %-16s %9s %9s",
                memory->arenas[i].name,
                _nya_debug_format_bytes(memory->arenas[i].used_bytes),
                _nya_debug_format_bytes(memory->resident[i])
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

    _nya_debug_overlay_graph_draw(window, &style, style.x + padding, text_y);
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

void _nya_debug_memory_sample(void) {
    _NYA_DebugMemorySample sample = { .process_resident = nya_memory_process_resident_bytes() };

    // the biggest arenas by used bytes. A partial selection sort, cheaper than sorting the registry to show six.
    u32 registry_count = nya_arena_registry_count();

    for (u32 i = 0; i < registry_count; i++) {
        NYA_Arena* arena = nya_arena_registry_at(i);
        if (arena == nullptr) continue;

        NYA_ArenaStats stats  = nya_arena_stats(arena);
        sample.used_total    += stats.used_bytes;

        // unnamed arenas are scratch inside one call.
        if (stats.name == nullptr) continue;

        // Insertion into a list kept in descending order, dropping off the end.
        u32 slot = sample.arena_count < NYA_DEBUG_OVERLAY_ARENAS ? sample.arena_count : NYA_DEBUG_OVERLAY_ARENAS - 1;
        if (sample.arena_count >= NYA_DEBUG_OVERLAY_ARENAS && stats.used_bytes <= sample.arenas[slot].used_bytes) continue;

        sample.arenas[slot]   = stats;
        sample.resident[slot] = nya_arena_resident_bytes(arena);
        if (sample.arena_count < NYA_DEBUG_OVERLAY_ARENAS) sample.arena_count++;

        for (u32 j = slot; j > 0 && sample.arenas[j].used_bytes > sample.arenas[j - 1].used_bytes; j--) {
            NYA_ArenaStats stats_swap    = sample.arenas[j];
            u64            resident_swap = sample.resident[j];

            sample.arenas[j]       = sample.arenas[j - 1];
            sample.resident[j]     = sample.resident[j - 1];
            sample.arenas[j - 1]   = stats_swap;
            sample.resident[j - 1] = resident_swap;
        }
    }

    _nya_debug_memory = sample;
}

void _nya_debug_overlay_graph_draw(NYA_Window* window, const NYA_DebugOverlayStyle* style, f32 x, f32 y) {
    nya_render2d_rect(window, x, y, style->width, style->height, (NYA_Color){ 0.0F, 0.0F, 0.0F, 0.35F });

    // one bar per sample, oldest on the left. Bars, since a line implies values in between.
    f32 column_width = style->width / (f32)NYA_DEBUG_OVERLAY_HISTORY;

    for (u32 i = 0; i < _nya_debug_sample_count; i++) {
        // read from the cursor so the newest sample is on the right.
        u32 index  = (_nya_debug_frame_cursor + NYA_DEBUG_OVERLAY_HISTORY - _nya_debug_sample_count + i) % NYA_DEBUG_OVERLAY_HISTORY;
        f32 sample = _nya_debug_frame_times_ms[index];

        f32 fraction = nya_clamp(sample / style->graph_ceiling_ms, 0.0F, 1.0F);
        f32 bar      = fraction * style->height;

        /*
         * Green up to half the ceiling, amber to three quarters, red past it.
         */
        NYA_Color color = (NYA_Color){ 0.35F, 0.85F, 0.45F, 0.9F };
        if (fraction > 0.75F) color = (NYA_Color){ 0.95F, 0.35F, 0.35F, 0.9F };
        else if (fraction > 0.5F) color = (NYA_Color){ 0.95F, 0.75F, 0.3F, 0.9F };

        // from the bottom up.
        nya_render2d_rect(window, x + ((f32)i * column_width), y + (style->height - bar), nya_max(column_width, 1.0F), bar, color);
    }
}

void _nya_debug_overlay_trace_draw(NYA_Window* window, const NYA_DebugOverlayStyle* style, f32 work_ms, b8 refresh) {
    // held with the printed figures, so the table reads instead of flickering.
    static NYA_TraceStats rows[NYA_TRACE_FEATURE_MAX];
    static u32            row_count = 0;
    static NYA_TraceStats total     = { 0 };

    // every frame the page is up, or tracing stops.
    nya_trace_request();

    if (refresh) {
        row_count = nya_trace_stats(rows, NYA_TRACE_FEATURE_MAX, style->sort);
        total     = (NYA_TraceStats){ .name = "total" };

        for (u32 i = 0; i < row_count; i++) {
            total.cpu_ms     += rows[i].cpu_ms;
            total.gpu_ms     += rows[i].gpu_ms;
            total.has_gpu     = total.has_gpu || rows[i].has_gpu;
            total.vram_bytes += rows[i].vram_bytes;
            total.ram_bytes  += rows[i].ram_bytes;
            total.draws      += rows[i].draws;
        }
    }

    f32 line_height = nya_render2d_font_line_height();
    if (line_height <= 0.0F) line_height = 16.0F;

    f32 padding = 8.0F;
    u32 shown   = nya_min(row_count, (u32)NYA_DEBUG_OVERLAY_TRACE_ROWS);

    // a heading, the column names, the rows and the total.
    f32 panel_height = (line_height * (f32)(shown + 3)) + (padding * 2.0F);
    if (!style->hide_graph) panel_height += style->height + padding;

    if (style->background.a > 0.0F) nya_render2d_rect(window, style->x, style->y, style->width + (padding * 2.0F), panel_height, style->background);

    static const NYA_ConstCString sort_names[NYA_TRACE_SORT_COUNT] = { "cpu", "gpu", "vram", "ram", "name" };

    f32 text_x = style->x + padding;
    f32 text_y = style->y + padding;

    if (NYA_TRACE_ENABLED) {
        nya_render2d_textf_with_font(window, style->font, style->font_size, text_x, text_y, style->text_color, "%7.2f ms work   by %s", (f64)work_ms,
                                     sort_names[style->sort]);
    } else {
        nya_render2d_textf_with_font(window, style->font, style->font_size, text_x, text_y, style->text_color, "tracing is compiled out of this build");
    }
    text_y += line_height;

    // columns at fixed fractions of the width: the font is proportional, so padded printf fields would not line up.
    const f32 columns[] = { 0.0F, 0.34F, 0.49F, 0.64F, 0.80F, 0.93F };
    const NYA_Color dim = { 0.72F, 0.76F, 0.82F, 1.0F };

    NYA_ConstCString headings[] = { "feature", "cpu ms", "gpu ms", "vram", "ram", "draws" };

    for (u32 column = 0; column < nya_carray_length(headings); column++) {
        nya_render2d_textf_with_font(window, style->font, style->font_size, text_x + (columns[column] * style->width), text_y, dim, "%s", headings[column]);
    }
    text_y += line_height;

    for (u32 i = 0; i <= shown; i++) {
        const NYA_TraceStats* row = i < shown ? &rows[i] : &total;

        char cells[6][24];
        (void)snprintf(cells[0], sizeof(cells[0]), "%s", row->name);
        (void)snprintf(cells[1], sizeof(cells[1]), "%.2f", row->cpu_ms);
        // n/a rather than zero: the feature shares a GPU group with another, or GPU time is not measured.
        if (row->has_gpu) (void)snprintf(cells[2], sizeof(cells[2]), "%.2f", row->gpu_ms);
        else (void)snprintf(cells[2], sizeof(cells[2]), "n/a");
        (void)snprintf(cells[3], sizeof(cells[3]), "%s", row->vram_bytes > 0 ? _nya_debug_format_bytes(row->vram_bytes) : "-");
        (void)snprintf(cells[4], sizeof(cells[4]), "%s", row->ram_bytes > 0 ? _nya_debug_format_bytes(row->ram_bytes) : "-");
        (void)snprintf(cells[5], sizeof(cells[5]), "%.0f", (f64)row->draws);

        NYA_Color color = i < shown ? style->text_color : dim;

        for (u32 column = 0; column < nya_carray_length(cells); column++) {
            nya_render2d_textf_with_font(window, style->font, style->font_size, text_x + (columns[column] * style->width), text_y, color, "%s", cells[column]);
        }
        text_y += line_height;
    }

    if (!style->hide_graph) _nya_debug_overlay_graph_draw(window, style, text_x, text_y);
}

void _nya_debug_overlay_systems_draw(NYA_Window* window, const NYA_DebugOverlayStyle* style) {
    u32 count       = nya_system_registry_count();
    u32 owner_count = nya_system_owner_count();

    f32 line_height = nya_render2d_font_line_height();
    if (line_height <= 0.0F) line_height = 16.0F;

    f32 padding = 8.0F;

    u32 shown = nya_min(count, (u32)NYA_DEBUG_OVERLAY_SYSTEM_ROWS);

    // scrolled to keep the marked row on screen, and clamped so the last page is full rather than short.
    u32 first = 0;
    if (style->selected_system < count && style->selected_system >= shown) first = style->selected_system - shown + 1;

    // a header, the rows, a blank, an owner header and one line per owner.
    f32 panel_height = (line_height * (f32)(shown + owner_count + 3)) + (padding * 2.0F);

    if (style->background.a > 0.0F) nya_render2d_rect(window, style->x, style->y, style->width + (padding * 2.0F), panel_height, style->background);

    f32 text_x = style->x + padding;
    f32 text_y = style->y + padding;

    nya_render2d_textf_with_font(window, style->font, style->font_size, text_x, text_y, style->text_color, "  %-20s %-8s %-6s %-4s %-4s %s", "system",
                                 "owner", "phases", "up", "on", "ms");
    text_y += line_height;

    NYA_Color dim      = (NYA_Color){ 0.72F, 0.76F, 0.82F, 1.0F };
    NYA_Color disabled = (NYA_Color){ 0.95F, 0.80F, 0.45F, 1.0F };

    for (u32 row = 0; row < shown; row++) {
        u32 index = first + row;
        if (index >= count) break;

        const NYA_SystemEntry* entry   = nya_system_registry_at(index);
        b8                     enabled = nya_system_registry_enabled_at(index);

        // one column per phase, the letter where the system has work and a dot where it has none, so the
        // shape of the frame is readable straight down the column.
        char phases[] = { entry->frame != nullptr ? 'f' : '.', entry->tick != nullptr ? 't' : '.', entry->render != nullptr ? 'r' : '.', '\0' };

        // amber for one someone switched off, full brightness for the row the keys are on.
        NYA_Color color = enabled ? dim : disabled;
        if (index == style->selected_system) color = style->text_color;

        nya_render2d_textf_with_font(window, style->font, style->font_size, text_x, text_y, color, "%s %-20s %-8s %-6s %-4s %-4s %5.3f",
                                     index == style->selected_system ? ">" : " ", entry->name, nya_system_owner_name(entry->owner), phases,
                                     nya_system_registry_initialized_at(index) ? "up" : "-", enabled ? "on" : "OFF",
                                     nya_time_ns_to_ms(nya_system_registry_time_ns_at(index)));
        text_y += line_height;
    }

    text_y += line_height;

    nya_render2d_textf_with_font(window, style->font, style->font_size, text_x, text_y, style->text_color, "  %-20s %-8s %-6s %-4s %s", "owner",
                                 "systems", "on", "ms", "memory");
    text_y += line_height;

    // grouped by owner, because "which plugin is costing the frame" is the question a list of forty
    // rows cannot answer.
    for (u32 i = 0; i < owner_count; i++) {
        NYA_SystemOwnerStats owner = nya_system_owner_stats_at(i);

        nya_render2d_textf_with_font(window, style->font, style->font_size, text_x, text_y, dim, "  %-20s %-8u %-6u %-4.3f %s", owner.name,
                                     owner.system_count, owner.enabled_count, nya_time_ns_to_ms(owner.time_ns),
                                     _nya_debug_format_bytes(owner.memory_bytes));
        text_y += line_height;
    }
}

void _nya_debug_overlay_apply_style_defaults(NYA_DebugOverlayStyle* style) {
    if (style->width <= 0.0F) style->width = style->page == NYA_DEBUG_OVERLAY_PAGE_TRACE ? 460.0F : 300.0F;
    if (style->page == NYA_DEBUG_OVERLAY_PAGE_SYSTEMS) style->width = nya_max(style->width, 360.0F);
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
