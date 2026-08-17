/**
 * @file nn_neat_draw.h
 *
 * Drawing a NEAT network: nodes as circles, connections as lines, numbers on both.
 *
 * For looking at what evolution actually built. A fitness score says a genome works; only the
 * picture says *how*, and whether the topology it grew is sensible or a tangle that happens to
 * score well.
 *
 * ```c
 * nya_render2d_font_set(NYA_ASSET_FONTS_ALDRICH_TTF, 24.0F);
 * nya_nn_neat_draw(window, nya_nn_neat_best(neat), (NYA_NeatDrawStyle){
 *     .x = 24, .y = 24, .width = 480, .height = 320,
 * });
 * ```
 *
 * Separate from nn_neat.c because that half has to compile for the build tool, which is built with
 * -DNYA_NO_SDL and has no renderer. Evolution works headless; only the picture needs a window.
 *
 * ## Layout
 *
 * Layered left to right: sensors and the bias on the left edge, outputs on the right, hidden nodes
 * in between at a depth equal to their longest path from an input. That reads the way a network is
 * normally drawn, and it makes a deepening topology visibly deepen.
 *
 * Recurrent connections — which NEAT produces routinely — are the exception to that: an edge that
 * points backwards is still drawn, it just runs right to left. The layering is cycle-safe, so a
 * network that feeds back on itself lays out rather than hanging.
 *
 * Drawing uses the current coordinate space, so it honours a camera. Everything is queued into the
 * ordinary 2D batch, so it costs a handful of draw calls and can be freely mixed with a UI.
 * */
#pragma once

#include "nyangine/base/base_types.h"
#include "nyangine/nn/nn_neat.h"
#include "nyangine/renderer/render_color.h"

typedef struct NYA_Window        NYA_Window;
typedef struct NYA_NeatDrawStyle NYA_NeatDrawStyle;

/**
 * How the network is drawn. Zero initialising is meaningful throughout — see each field.
 *
 * The colours default rather than being required, so `(NYA_NeatDrawStyle){ .width = 400, .height =
 * 300 }` is a complete call.
 * */
struct NYA_NeatDrawStyle {
    /** Top left of the area the network is laid out in. */
    f32 x, y;

    /** Size of that area. Zero means 400 by 300, which fits a small network legibly. */
    f32 width, height;

    /** Circle radius. Zero means 14, which is large enough to hold a two decimal number. */
    f32 node_radius;

    /*
     * Text is opt-out rather than opt-in, because the numbers are usually the point — a picture of
     * the topology without them says what is connected but not what it does.
     *
     * They need a font: nya_render2d_font_set must have been called, or the text silently does not
     * appear while the shapes still do.
     */
    b8 hide_values;
    b8 hide_labels;

    /**
     * The font the numbers and labels are drawn in. Null uses whatever nya_render2d_font_set last set.
     *
     * Worth its own field rather than inheriting the UI font: a value has to fit *inside* a node,
     * and a face sized for a HUD overflows a circle at any radius that keeps a dozen nodes on
     * screen. Load the same face at a smaller point size and name it here.
     * */
    NYA_ConstCString font;

    /**
     * Point size the labels and values are drawn at. Zero uses whatever nya_render2d_font_set last set.
     *
     * A size rather than a second font asset. A face carries no size, so the same file at a smaller
     * size used to mean loading it again under a made up handle; the draw layer derives that now, so
     * this is just a number.
     * */
    f32 font_size;

    /**
     * Print each connection's weight at its midpoint.
     *
     * Opt in, unlike the other two, because it is the first thing that makes the picture unreadable:
     * on anything past a handful of genes the labels of near-parallel edges land on top of each
     * other. Useful when interrogating one specific connection, noise the rest of the time — the
     * line's colour and thickness already say sign and magnitude.
     * */
    b8 show_weights;

    /**
     * Draw disabled connections too, dimmed and thin.
     *
     * Off by default because a long run accumulates many of them and they bury the live topology.
     * Worth turning on when asking why a structure is not doing anything.
     * */
    b8 show_disabled;

    /*
     * Node fill, by kind. All-zero means the defaults, which are chosen to be distinguishable:
     * bias grey, sensors green, hidden blue, outputs amber.
     */
    NYA_Color color_bias;
    NYA_Color color_sensor;
    NYA_Color color_hidden;
    NYA_Color color_output;

    /**
     * Connection colours, by sign of the weight. Excitatory and inhibitory read differently at a
     * glance, which is most of what makes the picture useful.
     * */
    NYA_Color color_positive;
    NYA_Color color_negative;
};

/**
 * Draws `network`. Does nothing when it is null or has no nodes.
 *
 * Safe to call on a network that is still evolving, including the one nya_nn_neat_best returns — it
 * only reads.
 * */
NYA_API void nya_nn_neat_draw(NYA_Window* window, const NYA_NeatNetwork* network, NYA_NeatDrawStyle style);
