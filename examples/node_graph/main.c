/**
 * @file examples/node_graph/main.c
 *
 * A little node-graph editor: a pannable, zoomable canvas of connected nodes on the left, a side panel
 * of the shadcn-like widgets on the right, and a graph model in the middle that the two agree on. Drag
 * an output port onto another node's input and the wire you draw becomes an edge in the model; grab a
 * wired input and it comes loose again. Built only out of `nya_ui_*`.
 *
 * ```
 * ./build run example node_graph
 * SDL_VIDEODRIVER=offscreen NYA_NODE_GRAPH_FRAMES=6 ./build run example node_graph   # headless, six frames
 * ```
 *
 * ## What it teaches
 *
 * - nya_ui_node_editor_begin / _end, nya_ui_node and nya_ui_node_link. The widget keeps no graph: the
 *   caller declares its nodes and links every pass and owns their positions, and the editor only reports
 *   the connect and disconnect *gestures* back through NYA_UINodeEditor. So the interesting wiring is not
 *   drawing the graph — it is folding those two gestures back into a model the next pass draws from. That
 *   round trip is the whole point of this example: `graph_connect` and `graph_detach` below are what turns
 *   a dragged wire into an edge and a grabbed port into a cut one.
 * - The shadcn-like widget set that landed alongside the editor: nya_ui_card_begin/_end frames the side
 *   panel with a heading and a subtitle, nya_ui_badge tags each node type, nya_ui_progress shows how much
 *   of the graph is wired, and nya_ui_breadcrumb draws the trail the panel sits under. None carry a colour
 *   of their own — they are the theme's panel, accent and text — so the whole thing follows a restyle.
 * - The two-pass shape every windowed `nya_ui_*` program has: the same frame function runs once from
 *   on_update as the input pass, where a drag becomes a gesture, and once from on_render as the draw pass.
 *   See ui.h's header for why input is read per tick and drawing happens per frame.
 *
 * ## Running headless
 *
 * NYA_NODE_GRAPH_FRAMES bounds the run to a fixed number of frames and then quits, so `./build run example
 * node_graph` under `SDL_VIDEODRIVER=offscreen` draws its frames on a device with no display and exits
 * cleanly — which is how it earns its place in the nightly sanitizer sweep. With the variable unset it is
 * an ordinary interactive window: drag the nodes by their title bars, drag the empty canvas to pan, the
 * wheel to zoom, and wire the ports up by hand.
 * */
#include "genyarated/assets.h"
#include "nyangine/nyangine.h"

#include "nyangine/nyangine.c"

/* CONSTANTS */

#define WINDOW_TITLE  "nyangine — node graph"
#define WINDOW_WIDTH  1280
#define WINDOW_HEIGHT 720

/** The layer's id, compared by content so it survives a code reload. */
#define LAYER_ID "node_graph"

/** The side panel's width, in pixels at scale 1. */
#define PANEL_WIDTH 300.0F

/** How many nodes and edges the model holds. Small and fixed: this is a demonstration graph, not an app. */
#define NODES_MAX 6u
#define EDGES_MAX 16u

/** A muted dark ground, so the canvas and the panel read against it. */
#define COLOR_GROUND ((NYA_Color){ 0.05F, 0.06F, 0.09F, 1.0F })

/* TYPES */

/** What a node does, which is the badge it wears in the side panel. */
typedef enum {
    NODE_SOURCE = 0,
    NODE_FILTER,
    NODE_SINK,

    NODE_TYPE_COUNT,
} NodeType;

/** The label each type shows on its badge, in NodeType order. */
static const NYA_ConstCString NODE_TYPE_LABEL[NODE_TYPE_COUNT] = {
    [NODE_SOURCE] = "source",
    [NODE_FILTER] = "filter",
    [NODE_SINK]   = "sink",
};

/**
 * One node in the model. `position` is the node's own — the editor writes it as the title bar is dragged,
 * so it lives here rather than in a pass-local. The port-label arrays point at string literals below.
 * */
typedef struct {
    u64              key;
    NYA_ConstCString title;
    NodeType         type;
    f32x2            position;
    u32              inputs;
    u32              outputs;
    const NYA_ConstCString* input_labels;
    const NYA_ConstCString* output_labels;
} GraphNode;

/**
 * The graph: the nodes and the edges between their ports. This is the model the editor is a view of; the
 * connect and disconnect gestures the editor reports are folded into `edges` and nothing else keeps them.
 * */
typedef struct {
    GraphNode      nodes[NODES_MAX];
    u32            node_count;

    NYA_UINodeLink edges[EDGES_MAX];
    u32            edge_count;
} Graph;

/** The whole example's state, hung off the world so it survives a hot reload. */
typedef struct {
    NYA_WindowHandle window;

    Graph            graph;

    /** The canvas: where it is panned to, how far it is zoomed, and the drag in flight. Owned here, outlives a pass. */
    NYA_UINodeEditor editor;

    /** The breadcrumb the side panel sits under, and where along it the trail is. */
    u32              crumb;

    /** When non-zero (from NYA_NODE_GRAPH_FRAMES), the window quits after this many frames, for a headless run. */
    u32              max_frames;
    u32              frame_count;
} NodeGraph;

NYA_INTERNAL NodeGraph* node_graph(void) {
    return nya_world_user_data();
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * THE STARTING GRAPH
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 *
 * A tiny image compositor: a source feeds a blur and a colour grade in parallel, and a mix node folds the
 * two back into one output. The port labels are literals with static storage, so the arrays a node points
 * at outlive every pass.
 */

static const NYA_ConstCString SOURCE_OUT[] = { "rgb", "alpha" };
static const NYA_ConstCString BLUR_IN[]    = { "in" };
static const NYA_ConstCString BLUR_OUT[]   = { "out" };
static const NYA_ConstCString GRADE_IN[]   = { "in" };
static const NYA_ConstCString GRADE_OUT[]  = { "out" };
static const NYA_ConstCString MIX_IN[]     = { "a", "b" };
static const NYA_ConstCString MIX_OUT[]    = { "out" };
static const NYA_ConstCString OUT_IN[]     = { "image" };

/** Node keys. Nonzero and unique, since a link names its ends by them. */
enum {
    KEY_SOURCE = 1,
    KEY_BLUR,
    KEY_GRADE,
    KEY_MIX,
    KEY_OUTPUT,
};

/** Fills `graph` with the starting compositor, replacing whatever was there. Also what the "reset" button calls. */
NYA_INTERNAL void graph_reset(Graph* graph) {
    nya_assert(graph != nullptr);

    *graph = (Graph){ 0 };

    graph->nodes[0] = (GraphNode){ .key = KEY_SOURCE, .title = "source", .type = NODE_SOURCE, .position = { 40.0F, 60.0F },
                                   .outputs = 2, .output_labels = SOURCE_OUT };
    graph->nodes[1] = (GraphNode){ .key = KEY_BLUR, .title = "blur", .type = NODE_FILTER, .position = { 280.0F, 30.0F },
                                   .inputs = 1, .outputs = 1, .input_labels = BLUR_IN, .output_labels = BLUR_OUT };
    graph->nodes[2] = (GraphNode){ .key = KEY_GRADE, .title = "grade", .type = NODE_FILTER, .position = { 280.0F, 190.0F },
                                   .inputs = 1, .outputs = 1, .input_labels = GRADE_IN, .output_labels = GRADE_OUT };
    graph->nodes[3] = (GraphNode){ .key = KEY_MIX, .title = "mix", .type = NODE_FILTER, .position = { 520.0F, 90.0F },
                                   .inputs = 2, .outputs = 1, .input_labels = MIX_IN, .output_labels = MIX_OUT };
    graph->nodes[4] = (GraphNode){ .key = KEY_OUTPUT, .title = "output", .type = NODE_SINK, .position = { 760.0F, 110.0F },
                                   .inputs = 1, .input_labels = OUT_IN };
    graph->node_count = 5;

    // the wires the graph opens with; each one fills one input port.
    graph->edges[0] = (NYA_UINodeLink){ .from_node = KEY_SOURCE, .from_port = 0, .to_node = KEY_BLUR, .to_port = 0 };
    graph->edges[1] = (NYA_UINodeLink){ .from_node = KEY_SOURCE, .from_port = 1, .to_node = KEY_GRADE, .to_port = 0 };
    graph->edges[2] = (NYA_UINodeLink){ .from_node = KEY_BLUR, .from_port = 0, .to_node = KEY_MIX, .to_port = 0 };
    graph->edges[3] = (NYA_UINodeLink){ .from_node = KEY_GRADE, .from_port = 0, .to_node = KEY_MIX, .to_port = 1 };
    graph->edges[4] = (NYA_UINodeLink){ .from_node = KEY_MIX, .from_port = 0, .to_node = KEY_OUTPUT, .to_port = 0 };
    graph->edge_count = 5;
}

/* THE MODEL EDITS The two operations the editor's gestures drive. An input port takes one wire, so connecting to one that is already wired replaces its edge rather than doubling it — the same rule the node widget assumes when it lets a wired input be grabbed loose. */

/** Drops any edge landing on `to_node`'s input `to_port`. Returns whether one was removed. */
NYA_INTERNAL b8 graph_detach(Graph* graph, u64 to_node, u32 to_port) {
    nya_assert(graph != nullptr);

    for (u32 i = 0; i < graph->edge_count; i++) {
        if (graph->edges[i].to_node == to_node && graph->edges[i].to_port == to_port) {
            // order does not matter to the model, so fill the hole with the last edge and shrink.
            graph->edges[i] = graph->edges[graph->edge_count - 1];
            graph->edge_count -= 1;
            return true;
        }
    }

    return false;
}

/** Adds `link`, first freeing the input it lands on so a port never holds two wires. */
NYA_INTERNAL void graph_connect(Graph* graph, NYA_UINodeLink link) {
    nya_assert(graph != nullptr);

    // a self link is the one gesture the editor cannot rule out, since a node's own output and input are both on it; the model does, so a stray drag onto the same node changes nothing.
    if (link.from_node == link.to_node) return;

    (void)graph_detach(graph, link.to_node, link.to_port);

    if (graph->edge_count >= EDGES_MAX) {
        nya_log_warn("node_graph: the edge table is full; the wire was dropped.");
        return;
    }

    graph->edges[graph->edge_count] = link;
    graph->edge_count += 1;
}

/** How many input ports the whole graph has, the denominator of the "wired" progress bar. */
NYA_INTERNAL u32 graph_input_ports(const Graph* graph) {
    u32 total = 0;
    for (u32 i = 0; i < graph->node_count; i++) total += graph->nodes[i].inputs;
    return total;
}

/** How many nodes of `type` the graph holds, for the count on a type's badge. */
NYA_INTERNAL u32 graph_type_count(const Graph* graph, NodeType type) {
    u32 count = 0;
    for (u32 i = 0; i < graph->node_count; i++) {
        if (graph->nodes[i].type == type) count += 1;
    }
    return count;
}

/* THE FRAME */

/** The canvas panel: the node editor, its nodes and its wires, and the two gestures folded back into the model. */
NYA_INTERNAL void canvas_panel(NYA_UI* ui, NodeGraph* state) {
    Graph* graph = &state->graph;

    // a definite size, because the editor fills its container and a container with none is a canvas with none.
    if (!nya_ui_panel_begin(ui, "canvas", (NYA_UIPanel){ .width = nya_ui_grow(1), .height = nya_ui_grow(1) })) return;

    if (nya_ui_node_editor_begin(ui, "editor", &state->editor)) {
        // the nodes, each at its own position in graph space. Declaration order is draw order, back to front.
        for (u32 i = 0; i < graph->node_count; i++) {
            GraphNode* node = &graph->nodes[i];

            (void)nya_ui_node(ui, (NYA_UINode){
                                      .key           = node->key,
                                      .title         = node->title,
                                      .position      = &node->position,
                                      .inputs        = node->inputs,
                                      .outputs       = node->outputs,
                                      .input_labels  = node->input_labels,
                                      .output_labels = node->output_labels,
                                  },
                              &state->editor);
        }

        // the wires under the nodes, looked up against where this pass placed each end.
        for (u32 i = 0; i < graph->edge_count; i++) nya_ui_node_link(ui, &state->editor, graph->edges[i]);

        nya_ui_node_editor_end(ui, &state->editor);
    }

    // the gestures the pass reported, read right after the end. On the draw pass both are false — no input is read there — so the model is edited once, on the input pass, exactly as a button is.
    if (state->editor.connected)    graph_connect(graph, state->editor.link);
    if (state->editor.disconnected) (void)graph_detach(graph, state->editor.detach_node, state->editor.detach_port);

    nya_ui_panel_end(ui);
}

/** The side panel: a card of the shadcn-like widgets describing the graph the canvas is showing. */
NYA_INTERNAL void side_panel(NYA_UI* ui, NodeGraph* state) {
    Graph* graph = &state->graph;

    if (!nya_ui_panel_begin(ui, "side", (NYA_UIPanel){ .width = nya_ui_fixed(PANEL_WIDTH), .height = nya_ui_grow(1) })) return;

    // the trail the panel sits under. Purely a bit of chrome here, but it owns its index like any widget.
    static const NYA_ConstCString crumbs[] = { "projects", "compositor", "graph" };
    (void)nya_ui_breadcrumb(ui, "trail", crumbs, nya_carray_length(crumbs), &state->crumb);

    if (nya_ui_card_begin(ui, "about", "Compositor", "a small image pipeline")) {
        // one badge per node type the graph uses, with its count — a fitted tag, so they sit in a line.
        if (nya_ui_panel_begin(ui, "types", (NYA_UIPanel){ .direction = NYA_UI_DIRECTION_ROW, .frameless = true })) {
            for (u32 t = 0; t < NODE_TYPE_COUNT; t++) {
                u32 count = graph_type_count(graph, (NodeType)t);
                if (count == 0) continue;

                char label[24];
                (void)snprintf(label, sizeof(label), "%s %u", NODE_TYPE_LABEL[t], count);
                nya_ui_badge(ui, label);
            }

            nya_ui_panel_end(ui);
        }

        // how much of the graph is wired: filled input ports over all of them. Climbs and falls as ports are wired and cut, so the bar is a live read of the model the canvas edits.
        u32 inputs = graph_input_ports(graph);
        f32 wired  = inputs > 0 ? (f32)graph->edge_count / (f32)inputs : 0.0F;

        char summary[48];
        (void)snprintf(summary, sizeof(summary), "%u of %u inputs wired", graph->edge_count, inputs);
        nya_ui_label(ui, summary);
        nya_ui_progress(ui, wired);

        nya_ui_card_end(ui);
    }

    nya_ui_label(ui, "drag an output onto an input to wire", NYA_COLOR_LIGHT_GRAY);
    nya_ui_label(ui, "grab a wired input to cut it", NYA_COLOR_LIGHT_GRAY);

    // pushed to the bottom, so the way to start over sits under the panel however tall the window is.
    nya_ui_size(ui, nya_ui_grow(1));
    (void)nya_ui_space(ui, 0.0F, 0.0F);

    if (nya_ui_button(ui, "reset graph")) graph_reset(graph);

    nya_ui_panel_end(ui);
}

/**
 * The whole frame, run twice: once from on_update as the input pass, where a drag becomes a gesture and the
 * model changes, and once from on_render as the draw pass, which draws it. See ui.h's header.
 * */
NYA_INTERNAL void frame_pass(NYA_Window* window, NYA_UIPass pass, NodeGraph* state) {
    NYA_UI* ui = nya_ui_begin(window, pass);

    NYA_UIPanel root = { .direction = NYA_UI_DIRECTION_ROW, .width = nya_ui_grow(1), .height = nya_ui_grow(1), .frameless = true };

    if (nya_ui_panel_begin(ui, "root", root)) {
        canvas_panel(ui, state);
        side_panel(ui, state);
        nya_ui_panel_end(ui);
    }

    if (nya_ui_cancelled(ui)) nya_app_get()->should_quit = true;

    nya_ui_end(ui);
}

/* LAYER HOOKS */

void node_graph_layer_on_create(NYA_Window* window) {
    nya_assert(window != nullptr);

    NodeGraph* state = node_graph();

    nya_render_clear_color_set(window, COLOR_GROUND);

    // confirm and cancel are all the input configuration a mouse-driven editor needs; escape backs out and quits.
    nya_input_action_rebind(NYA_INPUT_ACTION_CONFIRM, NYA_KEY_RETURN);
    nya_input_action_rebind(NYA_INPUT_ACTION_CANCEL, NYA_KEY_ESCAPE);

    graph_reset(&state->graph);
    state->editor = (NYA_UINodeEditor){ .zoom = 1.0F };
}

void node_graph_layer_on_destroy(NYA_Window* window) {
    nya_unused(window);
    // the model and the editor live on the world's arena and go with it; nothing of ours to free.
}

void node_graph_layer_on_event(NYA_Window* window, NYA_Event* event) {
    nya_unused(window);
    nya_assert(event != nullptr);

    // let a modal widget claim keys and clicks first; the editor is plain, but a program keeps the habit.
    (void)nya_ui_modal_event(event);
}

void node_graph_layer_on_update(NYA_Window* window, f32 delta_time_s) {
    nya_unused(delta_time_s);

    NodeGraph* state = node_graph();

    // the input pass: a drag becomes a pan, a moved node or a wire, and the model is edited here.
    frame_pass(window, NYA_UI_PASS_INPUT, state);

    // a headless or CI run draws its frames and then quits itself, so the sanitizer sweep terminates.
    state->frame_count += 1;
    if (state->max_frames > 0 && state->frame_count >= state->max_frames) nya_app_get()->should_quit = true;
}

void node_graph_layer_on_render(NYA_Window* window) {
    NodeGraph* state = node_graph();

    // the draw pass: the same layout, drawn. Every widget returns false, so nothing here edits the model.
    frame_pass(window, NYA_UI_PASS_DRAW, state);
}

/* MAIN */

s32 main(s32 argc, NYA_CString* argv) {
    nya_unused(argc, argv);
    nya_backtrace_init();

    NYA_EXPECT(nya_app_init(.app_id = "node_graph"), "while starting the engine");

    // the state lives on the world, so it shares the world's arena and lifetime.
    NodeGraph* state = nya_arena_alloc(nya_world()->allocator, sizeof(NodeGraph));
    *state           = (NodeGraph){ .window = NYA_WINDOW_HANDLE_NONE };

    // an optional frame budget, so a headless or CI run draws a fixed number of frames and then quits.
    NYA_ConstCString frames = getenv("NYA_NODE_GRAPH_FRAMES");
    if (frames != nullptr) state->max_frames = (u32)strtoul(frames, nullptr, 10);

    nya_world_user_data_set(state);

    state->window = nya_window_create(WINDOW_TITLE, WINDOW_WIDTH, WINDOW_HEIGHT, NYA_WINDOW_RESIZABLE);
    nya_assert(nya_window_is_valid(state->window), "the window could not be created");

    // pushing the layer runs its on_create, which builds the starting graph.
    nya_layer_push(state->window, nya_layer_of(node_graph_layer, LAYER_ID));

    nya_app_run();

    nya_app_deinit();

    nya_backtrace_deinit();
    return EXIT_SUCCESS;
}
