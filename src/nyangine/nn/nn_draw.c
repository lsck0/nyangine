#include "nyangine/nyangine.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * CONSTANTS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

#define NYA_NN_DRAW_DEFAULT_WIDTH  400.0F
#define NYA_NN_DRAW_DEFAULT_HEIGHT 300.0F
#define NYA_NN_DRAW_DEFAULT_RADIUS 14.0F

/** Clear space between adjacent circles, as a fraction of a diameter. Matches nn_neat_draw. */
#define NYA_NN_DRAW_NODE_GAP 0.3F

#define NYA_NN_DRAW_MIN_RADIUS       3.0F
#define NYA_NN_DRAW_MIN_VALUE_RADIUS 11.0F

#define NYA_NN_DRAW_LINE_MIN_THICKNESS   1.0F
#define NYA_NN_DRAW_LINE_THICKNESS_RANGE 2.5F
#define NYA_NN_DRAW_LINE_MIN_ALPHA       0.25F
#define NYA_NN_DRAW_LINE_ALPHA_RANGE     0.6F

/** Space between a circle and the label beside it. */
#define NYA_NN_DRAW_LABEL_GAP 6.0F

#define NYA_NN_DRAW_CENTERED 0.5F

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * TYPES
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/** One drawn column: the activations behind it and how they map onto the units actually shown. */
typedef struct _NYA_NNDrawColumn {
    /** The activation row this column shows. Borrowed from the graph, alive for the whole draw. */
    const f32* values;

    /** Units the layer really has, and how many of them are drawn. */
    u32 total;
    u32 shown;

    /** Left edge x of the column's circles. */
    f32 x;
} _NYA_NNDrawColumn;

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API DECLARATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

NYA_INTERNAL void _nya_nn_draw_apply_style_defaults(NYA_NNDrawStyle* style);

/**
 * Which real unit the `slot`th drawn circle stands for.
 *
 * Evenly spaced across the layer rather than the first N, so a sampled column is representative of
 * the whole layer instead of one end of it.
 * */
NYA_INTERNAL u32 _nya_nn_draw_unit_for_slot(u32 slot, u32 shown, u32 total) __attr_no_discard;

/** Vertical centre of the `slot`th circle in a column. */
NYA_INTERNAL f32 _nya_nn_draw_slot_y(u32 slot, u32 shown, f32 origin_y, f32 usable_height) __attr_no_discard;

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PUBLIC API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

void nya_nn_draw(NYA_Window* window, NYA_NNSequential* network, NYA_NNGraph* graph, const f32* input, NYA_NNDrawStyle style) {
    nya_assert(window != nullptr);

    // Null is the normal state before anything has been built, so it draws nothing rather than
    // asserting — a debug overlay must not crash the frame it exists to explain.
    if (network == nullptr || graph == nullptr || input == nullptr) return;
    if (network->layer_count == 0) return;

    _nya_nn_draw_apply_style_defaults(&style);

    /*
     * The forward pass happens here, under no_grad.
     *
     * Drawing needs the activations and only the caller knows the input, so taking the input and
     * running it is the arrangement with the fewest ways to be wrong — the alternative is the caller
     * running the network, holding the result across whatever else it does, and hoping nothing reset
     * the graph in between.
     */
    nya_nn_graph_reset(graph);
    nya_nn_graph_grad_begin(graph);

    // The first linear layer's weight is [in, out], so its row count is the input width.
    u32 input_size = 0;
    for (u32 i = 0; i < network->layer_count; i++) {
        if (network->layers[i]->weight == nullptr) continue;

        input_size = network->layers[i]->weight->shape[0];
        break;
    }

    if (input_size == 0) {
        nya_nn_graph_grad_end(graph);
        return;
    }

    NYA_NNTensor* activation = nya_nn_tensor_from(graph, NYA_NN_SHAPE(1, input_size), input);

    /*
     * A column per *linear* layer, plus the input.
     *
     * Activations are not columns of their own: a ReLU has exactly as many units as the layer before
     * it and drawing both would double the picture's width to say the same thing twice. The column
     * shows the value after the activation, which is what the next layer actually receives.
     */
    _NYA_NNDrawColumn columns[NYA_NN_SEQUENTIAL_MAX_LAYERS + 1];
    u32               column_count = 0;

    columns[column_count++] = (_NYA_NNDrawColumn){ .values = activation->data, .total = input_size };

    for (u32 i = 0; i < network->layer_count; i++) {
        NYA_NNLayer* layer = network->layers[i];

        activation = nya_nn_layer_forward(layer, graph, activation);

        if (layer->kind != NYA_NN_LAYER_LINEAR) {
            // Fold the activation function into the column the linear layer already produced.
            columns[column_count - 1].values = activation->data;
            continue;
        }

        columns[column_count++] = (_NYA_NNDrawColumn){ .values = activation->data, .total = activation->shape[1] };
    }

    nya_nn_graph_grad_end(graph);

    b8 truncated = false;
    for (u32 i = 0; i < column_count; i++) {
        columns[i].shown = nya_min(columns[i].total, (u32)NYA_NN_DRAW_MAX_UNITS);
        if (columns[i].shown < columns[i].total) truncated = true;
    }

    /*
     * Radius fitted to whichever axis is tighter, then columns spread edge to edge.
     *
     * The same arrangement nn_neat_draw arrived at, and for the same reason: the ends of the network
     * must not move as the picture changes, or the eye tracks the movement instead of the values.
     */
    u32 busiest = 1;
    for (u32 i = 0; i < column_count; i++) busiest = nya_max(busiest, columns[i].shown);

    /*
     * Labels are text and do not scale with the circles, so their width comes off the region before
     * the radius is fitted rather than being squeezed along with everything else.
     */
    f32 label_margin_left  = 0.0F;
    f32 label_margin_right = 0.0F;

    for (u32 i = 0; i < style.input_label_count; i++) {
        if (style.input_labels[i] == nullptr) continue;

        f32 width         = nya_render2d_text_measure_with_font(style.font, style.font_size, style.input_labels[i])[0] + NYA_NN_DRAW_LABEL_GAP;
        label_margin_left = nya_max(label_margin_left, width);
    }

    for (u32 i = 0; i < style.output_label_count; i++) {
        if (style.output_labels[i] == nullptr) continue;

        f32 width          = nya_render2d_text_measure_with_font(style.font, style.font_size, style.output_labels[i])[0] + NYA_NN_DRAW_LABEL_GAP;
        label_margin_right = nya_max(label_margin_right, width);
    }

    f32 available_width = nya_max(style.width - label_margin_left - label_margin_right, 1.0F);

    f32 radius = style.node_radius;

    if (column_count > 1) {
        f32 fit = available_width / (2.0F * (((1.0F + NYA_NN_DRAW_NODE_GAP) * (f32)(column_count - 1)) + 1.0F));
        radius  = nya_min(radius, fit);
    }

    if (busiest > 1) {
        f32 fit = style.height / (2.0F * (((1.0F + NYA_NN_DRAW_NODE_GAP) * (f32)(busiest - 1)) + 1.0F));
        radius  = nya_min(radius, fit);
    }

    radius = nya_max(radius, NYA_NN_DRAW_MIN_RADIUS);

    f32 scale         = style.node_radius > 0.0F ? radius / style.node_radius : 1.0F;
    f32 origin_x      = style.x + label_margin_left + radius;
    f32 origin_y      = style.y + radius;
    f32 usable_width  = available_width - (radius * 2.0F);
    f32 usable_height = style.height - (radius * 2.0F);

    for (u32 i = 0; i < column_count; i++) {
        f32 t         = column_count > 1 ? (f32)i / (f32)(column_count - 1) : NYA_NN_DRAW_CENTERED;
        columns[i].x  = origin_x + (t * usable_width);
    }

    /*
     * Connections first, so the circles sit on top of them.
     *
     * Only between drawn units, and only the ones carrying real weight. A layer's weights are
     * compared against that layer's own largest rather than a global maximum, because the scale of
     * an early layer and a late one have nothing to do with each other and a global threshold would
     * simply hide whichever layer happens to be smaller.
     */
    u32 linear_index = 0;
    for (u32 i = 0; i < network->layer_count; i++) {
        NYA_NNLayer* layer = network->layers[i];
        if (layer->kind != NYA_NN_LAYER_LINEAR) continue;

        const _NYA_NNDrawColumn* from = &columns[linear_index];
        const _NYA_NNDrawColumn* to   = &columns[linear_index + 1];
        linear_index++;

        NYA_NNTensor* weight = layer->weight;

        f32 heaviest = 0.0F;
        for (u32 w = 0; w < weight->count; w++) heaviest = nya_max(heaviest, fabsf(weight->data[w]));
        if (heaviest <= 0.0F) continue;

        for (u32 from_slot = 0; from_slot < from->shown; from_slot++) {
            u32 from_unit = _nya_nn_draw_unit_for_slot(from_slot, from->shown, from->total);
            f32 from_y    = _nya_nn_draw_slot_y(from_slot, from->shown, origin_y, usable_height);

            for (u32 to_slot = 0; to_slot < to->shown; to_slot++) {
                u32 to_unit = _nya_nn_draw_unit_for_slot(to_slot, to->shown, to->total);

                f32 value    = weight->data[(from_unit * weight->shape[1]) + to_unit];
                f32 strength = fabsf(value) / heaviest;

                if (strength < style.weight_threshold) continue;

                f32 to_y = _nya_nn_draw_slot_y(to_slot, to->shown, origin_y, usable_height);

                NYA_Color color = value >= 0.0F ? style.color_positive : style.color_negative;
                color.a        *= NYA_NN_DRAW_LINE_MIN_ALPHA + (strength * NYA_NN_DRAW_LINE_ALPHA_RANGE);

                f32 thickness = (NYA_NN_DRAW_LINE_MIN_THICKNESS + (strength * NYA_NN_DRAW_LINE_THICKNESS_RANGE)) * nya_max(scale, 0.35F);

                nya_render2d_line(window, (f32x2){ from->x, from_y }, (f32x2){ to->x, to_y }, thickness, color);
            }
        }
    }

    /*
     * Circles, brightness by activation.
     *
     * The point of the picture: a unit at zero is drawn dark, so a dead ReLU layer reads as a column
     * of dark circles at a glance rather than as numbers that have to be looked at one by one.
     */
    for (u32 i = 0; i < column_count; i++) {
        const _NYA_NNDrawColumn* column = &columns[i];

        NYA_Color base = style.color_hidden;
        if (i == 0) base = style.color_input;
        else if (i == column_count - 1) base = style.color_output;

        // Normalised per column, since a hidden activation and a Q-value are on unrelated scales.
        f32 largest = 0.0F;
        for (u32 u = 0; u < column->total; u++) largest = nya_max(largest, fabsf(column->values[u]));
        if (largest <= 0.0F) largest = 1.0F;

        for (u32 slot = 0; slot < column->shown; slot++) {
            u32 unit = _nya_nn_draw_unit_for_slot(slot, column->shown, column->total);
            f32 y    = _nya_nn_draw_slot_y(slot, column->shown, origin_y, usable_height);

            f32 intensity = fabsf(column->values[unit]) / largest;

            NYA_Color fill = base;
            fill.r        *= 0.25F + (0.75F * intensity);
            fill.g        *= 0.25F + (0.75F * intensity);
            fill.b        *= 0.25F + (0.75F * intensity);

            nya_render2d_circle(window, (f32x2){ column->x, y }, radius, fill);
        }
    }

    /*
     * Labels, beside the outermost columns.
     *
     * Right aligned against the input circles and left aligned against the outputs, so the text
     * always reads outwards from the network and never crosses it.
     */
    for (u32 side = 0; side < 2; side++) {
        const _NYA_NNDrawColumn* column = side == 0 ? &columns[0] : &columns[column_count - 1];

        NYA_ConstCString* labels = side == 0 ? style.input_labels : style.output_labels;
        u32               count  = side == 0 ? style.input_label_count : style.output_label_count;

        if (labels == nullptr) continue;

        for (u32 slot = 0; slot < column->shown; slot++) {
            u32 unit = _nya_nn_draw_unit_for_slot(slot, column->shown, column->total);
            if (unit >= count || labels[unit] == nullptr) continue;

            f32   y    = _nya_nn_draw_slot_y(slot, column->shown, origin_y, usable_height);
            f32x2 size = nya_render2d_text_measure_with_font(style.font, style.font_size, labels[unit]);

            f32 label_x = side == 0 ? column->x - radius - NYA_NN_DRAW_LABEL_GAP - size[0] : column->x + radius + NYA_NN_DRAW_LABEL_GAP;

            nya_render2d_text_with_font(
                window,
                style.font,
                style.font_size,
                labels[unit],
                label_x,
                y - (size[1] * NYA_NN_DRAW_CENTERED),
                (NYA_Color){ 0.8F, 0.82F, 0.86F, 1.0F }
            );
        }
    }

    if (style.show_values && radius >= NYA_NN_DRAW_MIN_VALUE_RADIUS) {
        f32 value_font_size = nya_min(style.font_size, radius * 0.72F);

        // A second pass, so every glyph comes out of one atlas in one draw call rather than costing a
        // pipeline switch per circle. See the note in nn_neat_draw.c.
        for (u32 i = 0; i < column_count; i++) {
            const _NYA_NNDrawColumn* column = &columns[i];

            for (u32 slot = 0; slot < column->shown; slot++) {
                u32 unit = _nya_nn_draw_unit_for_slot(slot, column->shown, column->total);
                f32 y    = _nya_nn_draw_slot_y(slot, column->shown, origin_y, usable_height);

                char text[16];
                (void)snprintf(text, sizeof(text), "%.2g", (f64)column->values[unit]);

                f32x2 size = nya_render2d_text_measure_with_font(style.font, value_font_size, text);

                nya_render2d_text_with_font(
                    window,
                    style.font,
                    value_font_size,
                    text,
                    column->x - (size[0] * NYA_NN_DRAW_CENTERED),
                    y - (size[1] * NYA_NN_DRAW_CENTERED),
                    (NYA_Color){ 0.05F, 0.05F, 0.07F, 1.0F }
                );
            }
        }
    }

    // Stated rather than silent: a picture of twelve units where the layer has sixty-four is a lie
    // unless it says so.
    if (truncated && !style.hide_truncation_note) {
        nya_render2d_textf_with_font(
            window,
            style.font,
            style.font_size,
            style.x,
            style.y + style.height + 4.0F,
            (NYA_Color){ 0.7F, 0.72F, 0.78F, 1.0F },
            "showing %d of %u units per layer",
            NYA_NN_DRAW_MAX_UNITS,
            busiest == NYA_NN_DRAW_MAX_UNITS ? columns[1].total : busiest
        );
    }
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

void _nya_nn_draw_apply_style_defaults(NYA_NNDrawStyle* style) {
    if (style->font == nullptr) style->font = nya_render2d_font_get();
    if (style->font_size <= 0.0F) style->font_size = nya_render2d_font_size_get();

    if (style->width <= 0.0F) style->width = NYA_NN_DRAW_DEFAULT_WIDTH;
    if (style->height <= 0.0F) style->height = NYA_NN_DRAW_DEFAULT_HEIGHT;
    if (style->node_radius <= 0.0F) style->node_radius = NYA_NN_DRAW_DEFAULT_RADIUS;

    if (style->weight_threshold <= 0.0F) style->weight_threshold = 0.15F;

    // A fully transparent colour is what a zeroed struct gives and never what a caller means.
    if (style->color_input.a == 0.0F) style->color_input = (NYA_Color){ 0.45F, 0.85F, 0.55F, 1.0F };
    if (style->color_hidden.a == 0.0F) style->color_hidden = (NYA_Color){ 0.45F, 0.65F, 0.95F, 1.0F };
    if (style->color_output.a == 0.0F) style->color_output = (NYA_Color){ 0.95F, 0.75F, 0.35F, 1.0F };

    if (style->color_positive.a == 0.0F) style->color_positive = (NYA_Color){ 0.4F, 0.9F, 0.6F, 1.0F };
    if (style->color_negative.a == 0.0F) style->color_negative = (NYA_Color){ 0.95F, 0.4F, 0.45F, 1.0F };
}

u32 _nya_nn_draw_unit_for_slot(u32 slot, u32 shown, u32 total) {
    if (shown >= total) return slot;
    if (shown <= 1) return 0;

    // Spread across the whole layer, first and last included, so the sample is representative rather
    // than the top of the column repeated.
    return (slot * (total - 1)) / (shown - 1);
}

f32 _nya_nn_draw_slot_y(u32 slot, u32 shown, f32 origin_y, f32 usable_height) {
    f32 t = shown > 1 ? (f32)slot / (f32)(shown - 1) : NYA_NN_DRAW_CENTERED;

    return origin_y + (t * usable_height);
}
