/**
 * @file debug_nn.h
 *
 * ```c
 * nya_nn_draw(window, network, graph, state, (NYA_NNDrawStyle){
 *     .x = 24, .y = 24, .width = 480, .height = 320,
 * });
 * ```
 * */
#pragma once

#include "nyangine-std/base/base_types.h"
#include "nyangine-core/nn/nn_layer.h"
#include "nyangine-core/nn/nn_tensor.h"
#include "nyangine-core/renderer/render_color.h"

typedef struct NYA_Window      NYA_Window;
typedef struct NYA_NNDrawStyle NYA_NNDrawStyle;

/**
 * Units drawn per column before the layer is sampled down.
 * */
#ifndef NYA_NN_DRAW_MAX_UNITS
#define NYA_NN_DRAW_MAX_UNITS 12
#endif

struct NYA_NNDrawStyle {
    /** Top left of the region the network is laid out in. */
    f32 x, y;

    /** Size of that region. Zero means 400 by 300. */
    f32 width, height;

    /** Circle radius, before the fit shrinks it. Zero means 14. */
    f32 node_radius;

    /** Null uses whatever nya_render2d_font_set last set. */
    NYA_ConstCString font;
    f32              font_size;

    /** Prints each unit's activation inside its circle. */
    b8 show_values;

    /**
     * Names for the input and output units, drawn left of the inputs and right of the outputs, as
     * nn_neat_draw places them.
     * */
    NYA_ConstCString* input_labels;
    u32               input_label_count;

    NYA_ConstCString* output_labels;
    u32               output_label_count;

    /** Hides the "showing 12 of 64" note. Only appears when a layer was actually sampled down. */
    b8 hide_truncation_note;

    /**
     * Connections weaker than this fraction of the layer's largest are not drawn. Zero means 0.15.
     * */
    f32 weight_threshold;

    /*
     * All-zero colours mean the defaults: inputs green, hidden blue, outputs amber, connections green or
     * red by weight sign, matching nn_neat_draw.
     */
    NYA_Color color_input;
    NYA_Color color_hidden;
    NYA_Color color_output;
    NYA_Color color_positive;
    NYA_Color color_negative;
};

/**
 * Draws `network`, with the activations produced by running it on `input`.
 * */
NYA_API void nya_nn_draw(NYA_Window* window, NYA_NNSequential* network, NYA_NNGraph* graph, const f32* input, NYA_NNDrawStyle style);
