/**
 * @file nn_neat_draw.h
 *
 * ```c
 * nya_render2d_font_set(NYA_ASSET_FONTS_ALDRICH_TTF, 24.0F);
 * nya_nn_neat_draw(window, nya_nn_neat_best(neat), (NYA_NeatDrawStyle){
 *     .x = 24, .y = 24, .width = 480, .height = 320,
 * });
 * ```
 * */
#pragma once

#include "nyangine/base/base_types.h"
#include "nyangine/nn/nn_neat.h"
#include "nyangine/renderer/render_color.h"

typedef struct NYA_Window        NYA_Window;
typedef struct NYA_NeatDrawStyle NYA_NeatDrawStyle;

/**
 * How the network is drawn. Zero initialising is meaningful throughout — see each field.
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
     */
    b8 hide_values;
    b8 hide_labels;

    /**
     * The font the numbers and labels are drawn in. Null uses whatever nya_render2d_font_set last set.
     * */
    NYA_ConstCString font;

    /**
     * Point size the labels and values are drawn at. Zero uses whatever nya_render2d_font_set last set.
     * */
    f32 font_size;

    /**
     * Print each connection's weight at its midpoint.
     * */
    b8 show_weights;

    /**
     * Draw disabled connections too, dimmed and thin.
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
 * */
NYA_API void nya_nn_neat_draw(NYA_Window* window, const NYA_NeatNetwork* network, NYA_NeatDrawStyle style);
