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
 * **Work time is the number to optimise against.** The overlay separates it from wall time: work is
 * update plus render plus present, measured before the frame rate limiter sleeps, so it moves when
 * the game gets faster. Wall time is start of frame to start of the next, including that sleep and
 * the wait for vsync — on a capped run it sits at the cap no matter how little work is done, which
 * makes it the wrong thing to optimise against and the right thing to check deadlines with.
 *
 * **Frame time, not frame rate, is the number to read.** Frames per second compresses exactly the
 * part that matters: the difference between 60 and 55 fps is 1.5 ms, and between 20 and 15 fps is
 * 17 ms, so the same drop in fps means wildly different things depending on where you are. The
 * overlay shows both, with milliseconds first.
 *
 * The **worst** frame in the recent window is shown alongside the average, because an average hides
 * hitches — a run that is smooth apart from one 40 ms stall averages fine and feels broken.
 *
 * ## Sampling
 *
 * History is collected by the draw call itself, so a frame that does not draw the overlay is not in
 * it. That keeps the subsystem free when it is off, at the cost of the graph being a history of
 * *observed* frames rather than of all of them. Turning the overlay on mid-run therefore takes a
 * moment to fill.
 *
 * ## Where this lives
 *
 * `nyangine/debug/` is for things that exist to explain the engine to a developer rather than to
 * run the game: this overlay now, and whatever inspects entities, assets and arenas later. It needs
 * the renderer, so it is excluded from -DNYA_NO_SDL builds along with everything else that draws.
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
 * Frames of history kept for the average, the worst case and the graph.
 *
 * At 120 fps this is about a second — long enough for the average to be steady, short enough that
 * the worst case still refers to something that just happened rather than to a hitch a minute ago.
 * */
/**
 * How often the displayed numbers are refreshed, in seconds.
 *
 * Sampling still happens every frame — the average, the worst case and the graph all see everything.
 * Only the *printed* figures are held, because a number that changes two hundred times a second is
 * not a readout, it is a flicker: the eye cannot read it and cannot even tell whether it is drifting.
 *
 * A fifth of a second is slow enough to read and fast enough to feel live.
 * */
#ifndef NYA_DEBUG_OVERLAY_REFRESH_SECONDS
#define NYA_DEBUG_OVERLAY_REFRESH_SECONDS 0.2F
#endif

#ifndef NYA_DEBUG_OVERLAY_HISTORY
#define NYA_DEBUG_OVERLAY_HISTORY 120
#endif

/**
 * Arenas listed in the memory section, largest first.
 *
 * A cap rather than all of them: the registry holds up to NYA_ARENA_REGISTRY_MAX, most of which are
 * small and unchanging, and a HUD that lists forty arenas is a wall rather than a readout. The
 * biggest few are where growth shows up.
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
     * A fixed ceiling rather than one scaled to the worst sample, because an auto-scaled graph
     * rescales itself the instant anything spikes and so never looks any different — the shape is
     * the information, and a fixed scale is what preserves it.
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
     * Hides the per arena memory lines.
     *
     * One line per named arena, largest first, capped at NYA_DEBUG_OVERLAY_ARENAS. Shown by default
     * because it is the only view of memory the engine has that is not a process total — and a
     * process total cannot tell you *which* subsystem is growing, which is the only question worth
     * asking when it does.
     * */
    b8 hide_memory;

    /**
     * Adds a line naming what forced the most draw calls this frame, and how many draws were dropped.
     *
     * Off by default: it answers "why is the draw call count what it is", which is a question you ask
     * while optimising and not one you want a line of the panel spent on the rest of the time.
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
 * frame rate limiter's sleep.
 *
 * For a caller that wants the number without the overlay — a headless benchmark, or a log line at
 * shutdown. Zero until the overlay has drawn at least once, since that is what samples.
 * */
NYA_API f32 nya_debug_frame_time_average_ms(void) __attr_no_discard;

/** Milliseconds the worst observed frame took, over the same window. */
NYA_API f32 nya_debug_frame_time_worst_ms(void) __attr_no_discard;
