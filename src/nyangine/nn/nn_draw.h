/**
 * @file nn_draw.h
 *
 * Drawing an NYA_NNSequential: layers as columns, units as circles, weights as lines.
 *
 * ```c
 * nya_nn_draw(window, network, graph, state, (NYA_NNDrawStyle){
 *     .x = 24, .y = 24, .width = 480, .height = 320,
 * });
 * ```
 *
 * The counterpart to nn_neat_draw.h, and deliberately a different picture — because the two
 * algorithms fail differently and the drawing is for spotting the failure.
 *
 * A NEAT genome's *topology* is the interesting thing: it starts minimal and grows, so the picture
 * shows which connections exist at all. A fixed network's topology never changes, so what is worth
 * seeing is which units are actually *doing* anything: a layer that has gone entirely dark is dead
 * ReLU, and a network whose activations are all saturated is one that has stopped learning. So this
 * draws live activations, fed through the network as it is drawn.
 *
 * ## Truncation
 *
 * A sixty-four unit hidden layer against another is four thousand connections. Drawn honestly that
 * is a solid grey mat with no information in it, so wide layers are sampled down to
 * NYA_NN_DRAW_MAX_UNITS evenly spaced units and the omission is stated on screen rather than
 * silently hidden. Weak connections are dropped below a threshold for the same reason.
 * */
#pragma once

#include "nyangine/base/base_types.h"
#include "nyangine/nn/nn_layer.h"
#include "nyangine/nn/nn_tensor.h"
#include "nyangine/renderer/render_color.h"

typedef struct NYA_Window      NYA_Window;
typedef struct NYA_NNDrawStyle NYA_NNDrawStyle;

/**
 * Units drawn per column before the layer is sampled down.
 *
 * Twelve, because past that the circles are smaller than the numbers in them and the picture stops
 * being readable at exactly the point it stops being useful.
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
     * Names for the input and output units, drawn beside their column — left of the inputs, right of
     * the outputs, the way nn_neat_draw places them.
     *
     * Optional, and worth supplying. A hidden unit has no name and needs none, but an input column of
     * unlabelled circles says nothing about *what* the network is being asked, and an output column
     * says nothing about what choosing one would mean. On a Q-network in particular the outputs are
     * the interesting half: "which action is this" is the whole question the picture should answer.
     *
     * Borrowed, not copied, and only read during the call. Fewer labels than units is fine — the
     * remainder go unlabelled — and a null entry skips just that one.
     * */
    NYA_ConstCString* input_labels;
    u32               input_label_count;

    NYA_ConstCString* output_labels;
    u32               output_label_count;

    /** Hides the "showing 12 of 64" note. Only appears when a layer was actually sampled down. */
    b8 hide_truncation_note;

    /**
     * Connections weaker than this fraction of the layer's largest are not drawn. Zero means 0.15.
     *
     * The single setting that decides whether the picture is readable. At zero every connection is
     * drawn and a wide layer is an opaque block; the strongest sixth carries almost all of what the
     * layer actually computes.
     * */
    f32 weight_threshold;

    /*
     * All-zero colours mean the defaults: inputs green, hidden blue, outputs amber, and connections
     * green or red by the sign of the weight — matching nn_neat_draw so the two read the same way.
     */
    NYA_Color color_input;
    NYA_Color color_hidden;
    NYA_Color color_output;
    NYA_Color color_positive;
    NYA_Color color_negative;
};

/**
 * Draws `network`, with the activations produced by running it on `input`.
 *
 * `input` is a single row, [1, in_features]. The forward pass happens here, under no_grad, and the
 * graph is reset first — so nothing the caller is holding from that graph survives this call.
 *
 * Does nothing when the network is empty or the input does not match its first layer.
 * */
NYA_API void nya_nn_draw(NYA_Window* window, NYA_NNSequential* network, NYA_NNGraph* graph, const f32* input, NYA_NNDrawStyle style);
