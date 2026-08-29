/**
 * @file debug_overlay.h
 *
 * The on-screen developer readout: frame time, frame rate, and what the frame cost to draw.
 *
 * ```c
 * nya_render2d_font_set(NYA_ASSET_FONTS_ALDRICH_TTF, 24.0F);
 * nya_debug_overlay_draw(window, (NYA_DebugOverlayStyle){ .x = 16, .y = 16 });
 * ```
 *
 * Work time (update + render + present, before the frame limiter's sleep) is what to optimise
 * against; wall time includes the sleep/vsync wait and sits at the cap regardless of work done, so
 * it's for checking deadlines instead. Milliseconds are shown before fps, since fps compresses the
 * part that matters (60→55 fps is 1.5 ms; 20→15 fps is 17 ms). The worst frame in the window is
 * shown beside the average because a run smooth apart from one 40 ms stall averages fine and feels
 * broken.
 *
 * History is sampled by the draw call itself, so a frame that skips drawing the overlay isn't
 * recorded — keeps the subsystem free when off, at the cost of a moment to fill after enabling it
 * mid-run.
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
 * Frames of history kept for the average, the worst case and the graph — about a second at
 * 120 fps: long enough to steady the average, short enough that the worst case is still recent.
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

/**
 * Arenas listed in the memory section, largest first, capped rather than showing all
 * NYA_ARENA_REGISTRY_MAX entries — a HUD listing forty arenas is a wall, not a readout.
 * */
#ifndef NYA_DEBUG_OVERLAY_ARENAS
#define NYA_DEBUG_OVERLAY_ARENAS 6
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
     *
     * Fixed rather than auto-scaled to the worst sample: auto-scaling rescales on every spike and
     * so never looks different — the shape is the information a fixed scale preserves.
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
     * NYA_DEBUG_OVERLAY_ARENAS). Shown by default: it's the only memory view that isn't a process
     * total, and a total can't say which subsystem is growing.
     * */
    b8 hide_memory;

    /**
     * Adds a line naming what forced the most draw calls this frame and how many were dropped. Off
     * by default — it answers a question worth asking while optimising, not worth a line the rest
     * of the time.
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
 *
 * Call it once per frame, last, so the draw call count it reports includes everything before it —
 * everything except its own.
 * */
NYA_API void nya_debug_overlay_draw(NYA_Window* window, NYA_DebugOverlayStyle style);

/**
 * Milliseconds of *work* the average observed frame took — update, render and present, without the
 * frame limiter's sleep. For callers without the overlay (a headless benchmark, a shutdown log
 * line). Zero until the overlay has drawn at least once, since that is what samples.
 * */
NYA_API f32 nya_debug_frame_time_average_ms(void) __attr_no_discard;

/** Milliseconds the worst observed frame took, over the same window. */
NYA_API f32 nya_debug_frame_time_worst_ms(void) __attr_no_discard;
