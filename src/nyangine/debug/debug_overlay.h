/**
 * @file debug_overlay.h
 *
 * ```c
 * nya_render2d_font_set(NYA_ASSET_FONTS_ALDRICH_TTF, 24.0F);
 * nya_debug_overlay_draw(window, (NYA_DebugOverlayStyle){ .x = 16, .y = 16 });
 * ```
 * */
#pragma once

#include "nyangine/base/base_types.h"
#include "nyangine/renderer/render_color.h"

typedef struct NYA_Window             NYA_Window;
typedef struct NYA_DebugOverlayStyle  NYA_DebugOverlayStyle;

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * CONSTANTS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/**
 * Frames of history for the average, worst case and graph. About a second at 120 fps: enough to
 * steady the average while keeping the worst case recent.
 * */
/**
 * How often the printed numbers refresh, in seconds. Sampling still happens every frame; only the
 * displayed figures are held, since a number changing 200 times a second reads as a flicker, not a
 * readout. A fifth of a second is slow enough to read, fast enough to feel live.
 * */
#ifndef NYA_DEBUG_OVERLAY_REFRESH_SECONDS
#define NYA_DEBUG_OVERLAY_REFRESH_SECONDS 0.2F
#endif

#ifndef NYA_DEBUG_OVERLAY_HISTORY
#define NYA_DEBUG_OVERLAY_HISTORY 120
#endif

/** Arenas listed in the memory section, largest first. Capped because forty rows is unreadable. */
#ifndef NYA_DEBUG_OVERLAY_ARENAS
#define NYA_DEBUG_OVERLAY_ARENAS 6
#endif

/**
 * Ceilings listed in the fullness section, fullest first.
 * */
#ifndef NYA_DEBUG_OVERLAY_CEILINGS
#define NYA_DEBUG_OVERLAY_CEILINGS 4
#endif

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * TYPES
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

struct NYA_DebugOverlayStyle {
    /** Top left corner. */
    f32 x, y;

    /** Graph width. Zero means 220, which is wide enough to see a hitch's shape. */
    f32 width;

    /** Graph height. Zero means 48. */
    f32 height;

    /**
     * The frame time the top of the graph represents, in milliseconds. Zero means 33.3.
     * */
    f32 graph_ceiling_ms;

    /** Font for the readout. Null uses whatever nya_render2d_font_set last set. */
    NYA_ConstCString font;

    /** Point size for the readout. Zero uses whatever nya_render2d_font_set last set. */
    f32 font_size;

    b8 hide_graph;

    /** Hides the draw call and vertex counts, which come from the 2D batch. */
    b8 hide_draw_stats;

    /**
     * Hides the per-arena memory lines (one per named arena, largest first, capped at
     * NYA_DEBUG_OVERLAY_ARENAS, each with its used and resident bytes) and the byte gauges after them, which
     * include GPU memory by kind.
     * Shown by default: it's the only memory view that isn't a process total, and a total can't say
     * which subsystem is growing.
     * */
    b8 hide_memory;

    /**
     * Hides the fixed-capacity fullness lines: the fullest NYA_DEBUG_OVERLAY_CEILINGS ceilings
     * registered with `nya_ceiling_register`, fullest first.
     * */
    b8 hide_ceilings;

    /**
     * Adds a line naming what forced the most draw calls this frame and how many were dropped. Off by
     * default; useful while optimising.
     * */
    b8 show_batch_breakdown;

    /** All-zero means a dark translucent panel; set the alpha to zero for no background at all. */
    NYA_Color background;

    /** All-zero means white. */
    NYA_Color text_color;
};

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * FUNCTIONS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/**
 * Samples this frame and draws the readout.
 * */
NYA_API void nya_debug_overlay_draw(NYA_Window* window, NYA_DebugOverlayStyle style);

/**
 * Average milliseconds of work per frame: update, render and present, without the limiter's sleep.
 * For callers without the overlay, such as a benchmark or a shutdown log. Zero until the overlay has
 * drawn once, since drawing samples.
 * */
NYA_API f32 nya_debug_frame_time_average_ms(void) __attr_no_discard;

/** Milliseconds the worst observed frame took, over the same window. */
NYA_API f32 nya_debug_frame_time_worst_ms(void) __attr_no_discard;
