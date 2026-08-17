#include "nyangine/nyangine.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API DECLARATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/*
 * ─────────────────────────────────────────────────────────
 * LAYOUT AND APPEARANCE CONSTANTS
 * ─────────────────────────────────────────────────────────
 *
 * Named rather than written inline. Every one of these was a bare float buried in an expression,
 * which made the drawing code read as arithmetic instead of as intent — and made a tweak a hunt for
 * which 0.75 was the one that mattered.
 */

/** Connection thickness, in pixels: the floor a near-zero weight still draws at, and the span above it. */
#define NYA_NEAT_DRAW_LINE_MIN_THICKNESS 1.0F
#define NYA_NEAT_DRAW_LINE_THICKNESS_RANGE 3.0F

/** Alpha a weak live connection fades to, and how much of the range strength buys back. */
#define NYA_NEAT_DRAW_LINE_MIN_ALPHA 0.35F
#define NYA_NEAT_DRAW_LINE_ALPHA_RANGE 0.65F

/** A disabled gene is dimmed to this fraction and drawn at the minimum thickness. */
#define NYA_NEAT_DRAW_DISABLED_ALPHA 0.25F

/** Gap between a node's circle and the label beneath it, in pixels. */
#define NYA_NEAT_DRAW_LABEL_GAP 2.0F

/** The region and node size used when the caller leaves a style field at zero. */
#define NYA_NEAT_DRAW_DEFAULT_WIDTH 400.0F
#define NYA_NEAT_DRAW_DEFAULT_HEIGHT 300.0F
#define NYA_NEAT_DRAW_DEFAULT_RADIUS 24.0F

/**
 * Clear space between two adjacent circles, as a fraction of a diameter.
 *
 * The layout spreads a column across the whole height whatever it holds, so spacing falls as the
 * network grows and eventually goes below a diameter — which is what made nodes overlap. The radius
 * is now fitted to the spacing instead, and this is how much room is left between neighbours when
 * it is.
 * */
#define NYA_NEAT_DRAW_NODE_GAP 0.3F

/** The radius fit will not go below this. Past it the picture is dots, and dots are still readable. */
#define NYA_NEAT_DRAW_MIN_RADIUS 3.0F

/** Values are dropped below this radius: four glyphs in a circle this small is a smudge, not a number. */
#define NYA_NEAT_DRAW_MIN_VALUE_RADIUS 11.0F

/** Where a lone column sits, and the midpoint used when a column holds a single node. */
#define NYA_NEAT_DRAW_CENTERED 0.5F

/** Where one node ended up, and which column it belongs to. */
typedef struct {
    f32x2 position;
    u32   layer;
} _NYA_NeatNodeLayout;

/** Fills anything the caller left at zero with a sensible default. */
NYA_INTERNAL void _nya_nn_neat_draw_apply_style_defaults(NYA_NeatDrawStyle* style);

/**
 * Assigns every node a column.
 *
 * Sensors and the bias are column zero, outputs are the last column, and a hidden node sits one past
 * the deepest thing feeding it.
 * */
NYA_INTERNAL u32 _nya_nn_neat_draw_layer_nodes(const NYA_NeatNetwork* network, u32* out_layers);

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PUBLIC API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

void nya_nn_neat_draw(NYA_Window* window, const NYA_NeatNetwork* network, NYA_NeatDrawStyle style) {
    nya_assert(window != nullptr);

    // Null is the normal state before the first generation has been evaluated, so it draws nothing
    // rather than asserting — a debug overlay should not crash the frame it is meant to explain.
    if (network == nullptr) return;
    if (network->nodes == nullptr || network->nodes->length == 0) return;

    _nya_nn_neat_draw_apply_style_defaults(&style);

    u32 node_count = (u32)network->nodes->length;

    NYA_Arena scratch = nya_arena_create_on_stack(.name = "neat_draw");
    defer     nya_arena_destroy_on_stack(&scratch);

    u32*                 layers = nya_arena_alloc(&scratch, node_count * sizeof(u32));
    _NYA_NeatNodeLayout* layout = nya_arena_alloc(&scratch, node_count * sizeof(_NYA_NeatNodeLayout));

    u32 layer_count = _nya_nn_neat_draw_layer_nodes(network, layers);

    /*
     * Positions: x from the column, y spread evenly down it.
     *
     * Counted twice — once to learn how many nodes share each column, once to place them — because
     * a node's vertical position depends on how many siblings it has, which is not known until every
     * node has a column.
     */
    u32* column_totals = nya_arena_alloc(&scratch, layer_count * sizeof(u32));
    u32* column_placed = nya_arena_alloc(&scratch, layer_count * sizeof(u32));

    for (u32 i = 0; i < layer_count; i++) {
        column_totals[i] = 0;
        column_placed[i] = 0;
    }

    for (u32 i = 0; i < node_count; i++) column_totals[layers[i]]++;

    /*
     * Empty columns removed, and the rest renumbered consecutively.
     *
     * A node's column is its longest path from an input, so a network with one deep chain in it
     * produces as many columns as that chain is long while every other node sits at a handful of
     * depths — twenty columns of which four hold anything. Spacing them evenly then wastes most of
     * the width on nothing and squeezes the network into a corner of its own region, which is
     * exactly what it looked like: three inputs on the left, a knot on the right, empty in between.
     *
     * Only occupancy is dropped, never order, so the left to right reading of depth survives.
     */
    u32* column_remap  = nya_arena_alloc(&scratch, layer_count * sizeof(u32));
    u32  occupied_count = 0;

    for (u32 i = 0; i < layer_count; i++) {
        column_remap[i] = occupied_count;
        if (column_totals[i] > 0) occupied_count++;
    }

    if (occupied_count < layer_count) {
        for (u32 i = 0; i < node_count; i++) layers[i] = column_remap[layers[i]];

        for (u32 i = 0; i < layer_count; i++) {
            if (column_totals[i] == 0) continue;

            column_totals[column_remap[i]] = column_totals[i];
        }

        layer_count = occupied_count;
    }

    /*
     * Draw order within a column: everything else first, the bias last.
     *
     * The bias is pushed first because it is node zero, which would put it at the top of the input
     * column above the actual inputs — and it is not one. Sitting at the bottom it reads as what it
     * is: a constant hanging off the side rather than a signal coming in.
     */
    u32* order = nya_arena_alloc(&scratch, node_count * sizeof(u32));

    u32 order_count = 0;
    for (u32 i = 0; i < node_count; i++) {
        if (network->nodes->items[i].kind == NYA_NEAT_NODE_BIAS) continue;
        order[order_count++] = i;
    }
    for (u32 i = 0; i < node_count; i++) {
        if (network->nodes->items[i].kind != NYA_NEAT_NODE_BIAS) continue;
        order[order_count++] = i;
    }

    /*
     * Labels move outside the columns, so the drawing area has to give them room.
     *
     * They used to sit under their circle, which collides with the node below as soon as a column is
     * at all full — and the input and output columns are exactly the ones that are. Beside the
     * outermost columns there is nothing to collide with, and it reads better besides: a name to the
     * left of the input it names, a name to the right of the output.
     */
    f32 label_margin_left  = 0.0F;
    f32 label_margin_right = 0.0F;

    if (!style.hide_labels) {
        for (u32 i = 0; i < node_count; i++) {
            NYA_ConstCString label = network->nodes->items[i].label;
            if (label == nullptr) continue;

            f32 width = nya_render2d_text_measure_with_font(style.font, style.font_size, label)[0] + NYA_NEAT_DRAW_LABEL_GAP;

            if (layers[i] == 0) label_margin_left = nya_max(label_margin_left, width);
            else if (layers[i] == layer_count - 1) label_margin_right = nya_max(label_margin_right, width);
        }
    }

    u32 busiest_column = 1;
    for (u32 i = 0; i < layer_count; i++) busiest_column = nya_max(busiest_column, column_totals[i]);

    /*
     * The radius is fitted to the region; the columns then span it edge to edge.
     *
     * Inputs sit hard against the left of the region and outputs hard against the right, and neither
     * moves when the network grows a layer — only the columns between them redistribute. A layout
     * with a fixed column pitch centred in the region does the opposite: every new hidden layer
     * widens the drawing and shoves both ends outwards, so the whole picture jitters sideways every
     * time evolution deepens the topology, which is exactly when you are watching it.
     *
     * Solved rather than iterated. Along one axis, n nodes with a gap of g diameters between them
     * span 2r·[(1+g)(n-1) + 1], and setting that equal to the available length gives r.
     */
    f32 available_width = nya_max(style.width - label_margin_left - label_margin_right, 1.0F);

    f32 radius = style.node_radius;

    if (layer_count > 1) {
        f32 fit = available_width / (2.0F * (((1.0F + NYA_NEAT_DRAW_NODE_GAP) * (f32)(layer_count - 1)) + 1.0F));
        radius  = nya_min(radius, fit);
    }

    if (busiest_column > 1) {
        f32 fit = style.height / (2.0F * (((1.0F + NYA_NEAT_DRAW_NODE_GAP) * (f32)(busiest_column - 1)) + 1.0F));
        radius  = nya_min(radius, fit);
    }

    radius = nya_max(radius, NYA_NEAT_DRAW_MIN_RADIUS);

    // How far the fit had to shrink things, for everything else that has to shrink with it.
    f32 scale = style.node_radius > 0.0F ? radius / style.node_radius : 1.0F;

    // Inset by the radius so a node on an edge column sits inside the region rather than half out.
    f32 origin_x      = style.x + label_margin_left + radius;
    f32 origin_y      = style.y + radius;
    f32 usable_width  = available_width - (radius * 2.0F);
    f32 usable_height = style.height - (radius * 2.0F);

    for (u32 slot = 0; slot < node_count; slot++) {
        u32 i     = order[slot];
        u32 layer = layers[i];

        // A single column would divide by zero; it is centred instead, which is also what looks
        // right for a network that has not grown any hidden nodes yet.
        f32 column_t = layer_count > 1 ? (f32)layer / (f32)(layer_count - 1) : NYA_NEAT_DRAW_CENTERED;

        /*
         * Vertical position within a column carries no meaning — the nodes in a column are a set,
         * not a sequence — so the column fills the height it has. That makes the ends of the network
         * line up: three inputs and two outputs both spanning the full height read as a network,
         * where two stacks centred on each other at different pitches read as an accident.
         */
        f32 row_t = column_totals[layer] > 1 ? (f32)column_placed[layer] / (f32)(column_totals[layer] - 1) : NYA_NEAT_DRAW_CENTERED;

        layout[i] = (_NYA_NeatNodeLayout){
            .position = {
                origin_x + (column_t * usable_width),
                origin_y + (row_t * usable_height),
            },
            .layer = layer,
        };

        column_placed[layer]++;
    }

    /*
     * Connections first, so the nodes sit on top of them rather than being crossed out by every line
     * that passes through.
     */
    f64 heaviest = 0.0;
    nya_array_foreach (network->connections, connection) heaviest = nya_max(heaviest, fabs(connection->weight));
    if (heaviest <= 0.0) heaviest = 1.0;

    /*
     * Weight labels are collected here and drawn after every line, rather than beside the line they
     * belong to.
     *
     * Same reason the circles and their labels below are drawn as two passes: a line goes through
     * the shape pipeline and text through the textured one, so alternating them forces the batch to
     * flush at every switch. Interleaved, a network with show_weights cost two draw calls per
     * connection — measured on the demo, 32 of its 39 draw calls in a frame were pipeline switches,
     * and almost all of them came from this loop.
     *
     * Only the placement is deferred. The colour is the line's own, computed below, so a label still
     * matches the connection it names.
     */
    typedef struct {
        f32x2     position;
        NYA_Color color;
        char      text[16];
    } _NYA_NeatWeightLabel;

    _NYA_NeatWeightLabel* weight_labels     = nullptr;
    u32                   weight_label_count = 0;

    if (style.show_weights && network->connections != nullptr && network->connections->length > 0) {
        weight_labels = nya_arena_alloc(&scratch, network->connections->length * sizeof(_NYA_NeatWeightLabel));
    }

    nya_array_foreach (network->connections, connection) {
        if (!connection->enabled && !style.show_disabled) continue;
        if (connection->in >= node_count || connection->out >= node_count) continue;

        // Thickness carries magnitude and colour carries sign, so a glance says which connections
        // matter and which way they push. Floored, or a near-zero weight draws nothing at all and
        // the topology looks sparser than it is.
        f32 strength  = (f32)(fabs(connection->weight) / heaviest);
        // Scaled with everything else, or a shrunk network turns into a solid mat of full width
        // lines with the nodes lost inside it.
        f32 thickness = (NYA_NEAT_DRAW_LINE_MIN_THICKNESS + (strength * NYA_NEAT_DRAW_LINE_THICKNESS_RANGE)) * nya_max(scale, 0.35F);

        NYA_Color color = connection->weight >= 0.0 ? style.color_positive : style.color_negative;

        if (!connection->enabled) {
            // Disabled genes are still genes — dimmed rather than recoloured, so they read as
            // "present but off" rather than as a third kind of connection.
            color.a   *= NYA_NEAT_DRAW_DISABLED_ALPHA;
            thickness  = NYA_NEAT_DRAW_LINE_MIN_THICKNESS * nya_max(scale, 0.35F);
        } else {
            // Weak connections fade as well as thin, which separates the structure that is doing the
            // work from the structure that is merely wired up.
            color.a *= NYA_NEAT_DRAW_LINE_MIN_ALPHA + (strength * NYA_NEAT_DRAW_LINE_ALPHA_RANGE);
        }

        nya_render2d_line(window, layout[connection->in].position, layout[connection->out].position, thickness, color);

        if (!style.show_weights || weight_labels == nullptr) continue;

        // At the midpoint, which for a recurrent edge is still between its endpoints and so still
        // unambiguous about which line it belongs to.
        f32x2 midpoint = (layout[connection->in].position + layout[connection->out].position) * NYA_NEAT_DRAW_CENTERED;

        _NYA_NeatWeightLabel* label = &weight_labels[weight_label_count++];
        label->position             = midpoint;
        label->color                = color;
        (void)snprintf(label->text, sizeof(label->text), "%.2f", connection->weight);
    }

    /*
     * The labels, once every line is down.
     *
     * Measured only now because centring needs the rendered width, and nya_render2d_textf cannot do it —
     * it formats and draws in one step, with nothing in between to measure.
     */
    for (u32 i = 0; i < weight_label_count; i++) {
        const _NYA_NeatWeightLabel* label = &weight_labels[i];

        f32x2 size = nya_render2d_text_measure_with_font(style.font, style.font_size, label->text);

        nya_render2d_text_with_font(
            window,
            style.font,
            style.font_size,
            label->text,
            label->position[0] - (size[0] * NYA_NEAT_DRAW_CENTERED),
            label->position[1] - (size[1] * NYA_NEAT_DRAW_CENTERED),
            label->color
        );
    }

    /*
     * Every circle first, then every label — not a circle and its label per node.
     *
     * A circle draws with the shape pipeline and text with the textured one, so interleaving them
     * costs a draw call at each switch: measured on an eight node network, drawing per node cost
     * sixteen draw calls where drawing per kind costs two. The batch can only merge consecutive
     * draws that agree on pipeline, texture and sampler, so grouping by kind is the whole technique.
     */
    for (u32 i = 0; i < node_count; i++) {
        const NYA_NeatNode* node = &network->nodes->items[i];

        NYA_Color fill = style.color_hidden;
        switch (node->kind) {
            case NYA_NEAT_NODE_BIAS:   fill = style.color_bias; break;
            case NYA_NEAT_NODE_SENSOR: fill = style.color_sensor; break;
            case NYA_NEAT_NODE_OUTPUT: fill = style.color_output; break;

            case NYA_NEAT_NODE_HIDDEN:
            case NYA_NEAT_NODE_KIND_COUNT:
            default:                   break;
        }

        nya_render2d_circle(window, layout[i].position, radius, fill);
    }

    // The text pass. Same loop, second time round, so every glyph shares one draw call.
    for (u32 i = 0; i < node_count; i++) {
        const NYA_NeatNode* node = &network->nodes->items[i];

        // Dropped once the fit has shrunk the circles past legibility, rather than drawn as a smear
        // across the node and its neighbours.
        if (!style.hide_values && radius >= NYA_NEAT_DRAW_MIN_VALUE_RADIUS) {
            // One decimal, not two: at any radius that keeps a network of a dozen nodes on screen,
            // four glyphs do not fit inside the circle and spill over its edge.
            char value_text[16];
            (void)snprintf(value_text, sizeof(value_text), "%.1f", node->value);

            f32x2 size = nya_render2d_text_measure_with_font(style.font, style.font_size, value_text);

            // Black on the node, because every default fill is light enough for dark text to read.
            nya_render2d_text_with_font(
                window,
                style.font,
                style.font_size,
                value_text,
                layout[i].position[0] - (size[0] * 0.5F),
                layout[i].position[1] - (size[1] * 0.5F),
                (NYA_Color){ 0.05F, 0.05F, 0.07F, 1.0F }
            );
        }

        // Labels outside the circle rather than inside it: the value is already in there, and a
        // hidden node has no label at all.
        if (style.hide_labels || node->label == nullptr) continue;

        f32x2 label_size = nya_render2d_text_measure_with_font(style.font, style.font_size, node->label);

        /*
         * Beside the column, in the margin reserved for it above — left of the inputs, right of the
         * outputs. A label on any other column has no margin to sit in and falls back to under its
         * circle, which is safe there because the middle columns are the sparse ones.
         */
        f32 label_x = layout[i].position[0] - (label_size[0] * NYA_NEAT_DRAW_CENTERED);
        f32 label_y = layout[i].position[1] + radius + NYA_NEAT_DRAW_LABEL_GAP;

        if (layout[i].layer == 0) {
            label_x = layout[i].position[0] - radius - NYA_NEAT_DRAW_LABEL_GAP - label_size[0];
            label_y = layout[i].position[1] - (label_size[1] * NYA_NEAT_DRAW_CENTERED);
        } else if (layout[i].layer == layer_count - 1) {
            label_x = layout[i].position[0] + radius + NYA_NEAT_DRAW_LABEL_GAP;
            label_y = layout[i].position[1] - (label_size[1] * NYA_NEAT_DRAW_CENTERED);
        }

        nya_render2d_text_with_font(window, style.font, style.font_size, node->label, label_x, label_y, (NYA_Color){ 0.8F, 0.82F, 0.86F, 1.0F });
    }
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

void _nya_nn_neat_draw_apply_style_defaults(NYA_NeatDrawStyle* style) {
    // Null means the current font, which is exactly what nya_render2d_font_get hands back — so the
    // _with_font calls below need no special case for it.
    if (style->font == nullptr) style->font = nya_render2d_font_get();
    if (style->font_size <= 0.0F) style->font_size = nya_render2d_font_size_get();

    if (style->width <= 0.0F) style->width = NYA_NEAT_DRAW_DEFAULT_WIDTH;
    if (style->height <= 0.0F) style->height = NYA_NEAT_DRAW_DEFAULT_HEIGHT;
    // Wide enough for a one decimal value at the default font size, with a little margin.
    if (style->node_radius <= 0.0F) style->node_radius = NYA_NEAT_DRAW_DEFAULT_RADIUS;

    // A fully transparent colour is what a zeroed struct gives, and it is never something a caller
    // asks for on purpose — it would draw nothing. Treated as unspecified.
    if (style->color_bias.a == 0.0F) style->color_bias = (NYA_Color){ 0.72F, 0.74F, 0.78F, 1.0F };
    if (style->color_sensor.a == 0.0F) style->color_sensor = (NYA_Color){ 0.45F, 0.85F, 0.55F, 1.0F };
    if (style->color_hidden.a == 0.0F) style->color_hidden = (NYA_Color){ 0.45F, 0.65F, 0.95F, 1.0F };
    if (style->color_output.a == 0.0F) style->color_output = (NYA_Color){ 0.95F, 0.75F, 0.35F, 1.0F };

    if (style->color_positive.a == 0.0F) style->color_positive = (NYA_Color){ 0.4F, 0.9F, 0.6F, 1.0F };
    if (style->color_negative.a == 0.0F) style->color_negative = (NYA_Color){ 0.95F, 0.4F, 0.45F, 1.0F };
}

u32 _nya_nn_neat_draw_layer_nodes(const NYA_NeatNetwork* network, u32* out_layers) {
    u32 node_count = (u32)network->nodes->length;

    for (u32 i = 0; i < node_count; i++) out_layers[i] = 0;

    /*
     * Relaxation rather than a topological sort, because a NEAT network may be cyclic and a
     * topological sort has nothing to say about a cycle.
     *
     * Each pass pushes a node one column past the deepest thing feeding it. A feed forward network
     * settles in as many passes as it is deep; a cyclic one would never settle, so the pass count is
     * capped at the node count — beyond which any further change is the cycle going round again
     * rather than new depth being discovered.
     */
    for (u32 pass = 0; pass < node_count; pass++) {
        b8 changed = false;

        nya_array_foreach (network->connections, connection) {
            if (!connection->enabled) continue;
            if (connection->in >= node_count || connection->out >= node_count) continue;

            const NYA_NeatNode* out_node = &network->nodes->items[connection->out];

            // Inputs are pinned to column zero: something feeding a sensor is a recurrent edge, and
            // moving the sensor rightwards for it would put the network's inputs in the middle.
            if (out_node->kind == NYA_NEAT_NODE_SENSOR || out_node->kind == NYA_NEAT_NODE_BIAS) continue;

            u32 candidate = out_layers[connection->in] + 1;
            if (candidate <= out_layers[connection->out]) continue;

            out_layers[connection->out] = candidate;
            changed                     = true;
        }

        if (!changed) break;
    }

    /*
     * Outputs get a column past everything else, so they line up on the right edge instead of
     * scattering by how deep the path that happened to reach them was.
     *
     * Measured against the deepest *non-output* node specifically. Seeding it from the deepest node
     * of any kind meant that a hidden node as deep as the network goes left the outputs at that same
     * depth, and they were drawn stacked in the same column as it — hidden nodes sitting directly
     * under the output, with the edge between them running vertically.
     */
    u32 deepest_non_output = 0;
    for (u32 i = 0; i < node_count; i++) {
        if (network->nodes->items[i].kind == NYA_NEAT_NODE_OUTPUT) continue;

        deepest_non_output = nya_max(deepest_non_output, out_layers[i]);
    }

    u32 output_layer = deepest_non_output + 1;

    for (u32 i = 0; i < node_count; i++) {
        if (network->nodes->items[i].kind == NYA_NEAT_NODE_OUTPUT) out_layers[i] = output_layer;
    }

    return output_layer + 1;
}
