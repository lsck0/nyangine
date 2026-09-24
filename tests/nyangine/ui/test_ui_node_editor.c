/**
 * The node editor, driven headless through the recorder: a canvas, two nodes at graph positions, and the links
 * between their ports. Nothing under test knows a presenter is installed — it reads the command stream a pass
 * declared, the way the rest of the widget set is checked. The pointer drives the interactions: a node is dragged
 * by its title, a link by pulling from an output stub to an input one, the canvas panned by its empty space and
 * zoomed by the wheel, and every one of those is read back as geometry or as an event the editor reported.
 **/

#include "nyangine/nyangine.c"
#include "nyangine/nyangine.h"

#include "SDL3/SDL_init.h"

static NYA_Window window = { .handle = { .index = 1, .generation = 1 }, .screen_width = 800, .screen_height = 600 };

/* Driving the pointer and the clock, the same way the shadcn widget test does. */

static void pointer_move(f32x2 point) {
    f32x2     from  = nya_input_mouse_position();
    NYA_Event event = { .type = NYA_EVENT_MOUSE_MOVED, .as_mouse_moved_event = { .x = point.x, .y = point.y, .delta_x = point.x - from.x, .delta_y = point.y - from.y } };
    nya_system_input_handle_event(&event);
}

static void pointer_button(b8 down) {
    f32x2     at    = nya_input_mouse_position();
    NYA_Event event = { .type = down ? NYA_EVENT_MOUSE_BUTTON_DOWN : NYA_EVENT_MOUSE_BUTTON_UP, .as_mouse_button_event = { .is_down = down, .button = NYA_MOUSE_BUTTON_LEFT, .x = at.x, .y = at.y } };
    nya_system_input_handle_event(&event);
}

static void wheel(f32 amount) {
    f32x2     at    = nya_input_mouse_position();
    NYA_Event event = { .type = NYA_EVENT_MOUSE_WHEEL_MOVED, .as_mouse_wheel_event = { .amount_y = amount, .mouse_x = at.x, .mouse_y = at.y } };
    nya_system_input_handle_event(&event);
}

static void tick(void) {
    nya_event_dispatch((NYA_Event){ .type = NYA_EVENT_UPDATING_ENDED });
    nya_world()->sim_system.tick++;
}

/* The graph under test. A source with one output and a sink with one input, and any links the case set. */

static NYA_UINodeEditor editor;
static f32x2            pos_a;
static f32x2            pos_b;

static const NYA_ConstCString OUT_A[] = { "out" };
static const NYA_ConstCString IN_B[]  = { "in" };

static NYA_UINodeLink links[4];
static u32            link_count;

static void graph(NYA_UIPass pass) {
    NYA_UI* ui = nya_ui_begin(&window, pass);

    if (nya_ui_node_editor_begin(ui, "canvas", &editor)) {
        (void)nya_ui_node(ui, (NYA_UINode){ .key = 1, .title = "alpha", .position = &pos_a, .outputs = 1, .output_labels = OUT_A }, &editor);
        (void)nya_ui_node(ui, (NYA_UINode){ .key = 2, .title = "beta", .position = &pos_b, .inputs = 1, .input_labels = IN_B }, &editor);

        for (u32 i = 0; i < link_count; i++) nya_ui_node_link(ui, &editor, links[i]);

        nya_ui_node_editor_end(ui, &editor);
    }

    nya_ui_end(ui);
    if (pass == NYA_UI_PASS_INPUT) tick();
}

/** Two draw passes, so the pass that is read was laid out from measurements rather than a first blank one. */
static void draw_twice(NYA_UIRecorder* recorder) {
    graph(NYA_UI_PASS_DRAW);
    nya_ui_recorder_reset(recorder);
    graph(NYA_UI_PASS_DRAW);
}

/* The same transform the widget uses, so the test computes where a node must land rather than guessing. */

static f32 combined(void) {
    return 1.0F * editor.zoom; // the pass scale is 1 headless.
}

static NYA_Rectf box_of(f32x2 pos) {
    f32 s = combined();

    return (NYA_Rectf){ editor._canvas.x + editor.pan.x + roundf(pos.x * s), editor._canvas.y + editor.pan.y + roundf(pos.y * s), roundf(NYA_UI_NODE_WIDTH * s), 0.0F };
}

static f32 title_h(void) {
    return nya_max(roundf((16.0F + 8.0F) * editor.zoom), 1.0F); // one recorder cell plus the padding, zoomed.
}

static f32 row_h(void) {
    return nya_max(roundf(16.0F * editor.zoom), 1.0F);
}

/** The point a wire meets a port on `output`'s side, index `index`, of the node whose box is `box`. */
static f32x2 anchor(NYA_Rectf box, b8 output, u32 index) {
    f32 x = output ? box.x + box.width : box.x;
    f32 y = box.y + title_h() + ((f32)index * row_h()) + (row_h() * 0.5F);

    return (f32x2){ roundf(x), roundf(y) };
}

static const NYA_UIWidgetDraw* panel_at(const NYA_UIRecorder* recorder, f32 x, f32 y) {
    for (u32 i = 0; i < nya_ui_recorder_count(recorder); i++) {
        const NYA_UIWidgetDraw* widget = nya_ui_recorder_at(recorder, i);

        if (widget->kind == NYA_UI_WIDGET_PANEL && (s32)widget->rect.x == (s32)x && (s32)widget->rect.y == (s32)y) return widget;
    }

    return nullptr;
}

static u32 count_of(const NYA_UIRecorder* recorder, NYA_UIWidgetKind kind) {
    u32 count = 0;

    for (u32 i = 0; i < nya_ui_recorder_count(recorder); i++) {
        if (nya_ui_recorder_at(recorder, i)->kind == kind) count += 1;
    }

    return count;
}

s32 main(void) {
    _NYA_APP_INSTANCE = (NYA_App){ .initialized = true };

    b8 sdl_ok = SDL_Init(0);
    nya_assert(sdl_ok, "SDL_Init failed: %s", SDL_GetError());

    nya_system_settings_init();
    nya_system_callback_init();
    NYA_EXPECT(nya_system_events_init());
    nya_system_input_init();
    nya_system_asset_init();

    NYA_World* world = nya_world_create();
    (void)nya_world_set(world);

    defer nya_system_settings_deinit();
    defer nya_system_callback_deinit();
    defer nya_system_events_deinit();
    defer nya_system_input_deinit();
    defer nya_system_asset_deinit();
    defer nya_world_destroy(world);

    nya_input_action_rebind(NYA_INPUT_ACTION_CONFIRM, NYA_KEY_RETURN);
    nya_input_action_rebind(NYA_INPUT_ACTION_CANCEL, NYA_KEY_ESCAPE);

    nya_ui_style_set(&window, (NYA_UIStyle){ .padding = 8.0F, .spacing = 6.0F });

    static NYA_UIRecorder recorder;
    nya_ui_recorder_init(&recorder, (f32x2){ 8.0F, 16.0F });
    defer nya_ui_recorder_deinit(&recorder);

    nya_ui_presenter_set(&window, nya_ui_recorder_presenter(&recorder));

    // The nodes build into the structure their positions ask for: a box each, a title label, and a port stub.
    {
        editor     = (NYA_UINodeEditor){ .zoom = 1.0F };
        pos_a      = (f32x2){ 20.0F, 20.0F };
        pos_b      = (f32x2){ 300.0F, 40.0F };
        link_count = 0;

        draw_twice(&recorder);

        // a panel each for the two nodes, over the one the frameless canvas emits for itself.
        nya_check(count_of(&recorder, NYA_UI_WIDGET_PANEL) == 3, "two node boxes over the canvas frame, got %u", count_of(&recorder, NYA_UI_WIDGET_PANEL));

        NYA_Rectf a = box_of(pos_a);
        NYA_Rectf b = box_of(pos_b);

        const NYA_UIWidgetDraw* box_a = panel_at(&recorder, a.x, a.y);
        const NYA_UIWidgetDraw* box_b = panel_at(&recorder, b.x, b.y);

        nya_check(box_a != nullptr && box_b != nullptr, "each node's box sits at its transformed position");
        nya_check(box_a != nullptr && box_a->rect.width == a.width, "and is the node's width across, got %f of %f", (f64)(box_a ? box_a->rect.width : 0.0F), (f64)a.width);

        // the titles and the port names are labels, so a node reads the same on every backend.
        nya_check(nya_ui_recorder_find(&recorder, NYA_UI_WIDGET_LABEL, "alpha") != nullptr, "a node shows its title");
        nya_check(nya_ui_recorder_find(&recorder, NYA_UI_WIDGET_LABEL, "beta") != nullptr, "and so does the other");
        nya_check(nya_ui_recorder_find(&recorder, NYA_UI_WIDGET_LABEL, "out") != nullptr, "an output port shows its name");
        nya_check(nya_ui_recorder_find(&recorder, NYA_UI_WIDGET_LABEL, "in") != nullptr, "and an input port shows its name");

        // the stubs are the accent fills a rule draws, two title strips and two ports, no wire yet.
        nya_check(count_of(&recorder, NYA_UI_WIDGET_RULE) == 4, "two title strips and two port stubs, got %u", count_of(&recorder, NYA_UI_WIDGET_RULE));
    }

    // Dragging a node by its title moves it, in graph units, however the canvas is placed.
    {
        editor     = (NYA_UINodeEditor){ .zoom = 1.0F };
        pos_a      = (f32x2){ 20.0F, 20.0F };
        pos_b      = (f32x2){ 300.0F, 40.0F };
        link_count = 0;

        draw_twice(&recorder);

        NYA_Rectf a       = box_of(pos_a);
        f32x2     handle  = { a.x + (a.width * 0.5F), a.y + (title_h() * 0.5F) };

        pointer_move(handle);
        pointer_button(true);
        graph(NYA_UI_PASS_INPUT); // the press takes the node.

        pointer_move((f32x2){ handle.x + 40.0F, handle.y + 25.0F });
        graph(NYA_UI_PASS_INPUT); // the move carries it.

        pointer_button(false);
        graph(NYA_UI_PASS_INPUT);

        nya_check(pos_a.x == 60.0F && pos_a.y == 45.0F, "a title drag moves the node by the pointer, got %f,%f", (f64)pos_a.x, (f64)pos_a.y);
    }

    // Pulling from an output stub to an input one reports the link, both ends named by the keys the caller gave.
    {
        editor     = (NYA_UINodeEditor){ .zoom = 1.0F };
        pos_a      = (f32x2){ 20.0F, 20.0F };
        pos_b      = (f32x2){ 300.0F, 40.0F };
        link_count = 0;

        draw_twice(&recorder);

        f32x2 from = anchor(box_of(pos_a), true, 0);
        f32x2 to   = anchor(box_of(pos_b), false, 0);

        pointer_move(from);
        pointer_button(true);
        graph(NYA_UI_PASS_INPUT); // the press starts the link at the output.

        pointer_move(to);
        graph(NYA_UI_PASS_INPUT); // the pointer drags it across.

        pointer_button(false);
        graph(NYA_UI_PASS_INPUT); // the release over the input completes it.

        nya_check(editor.connected, "a drag from an output to an input reports a link");
        nya_check(editor.link.from_node == 1 && editor.link.from_port == 0, "leaving alpha's output, got %llu:%u", (unsigned long long)editor.link.from_node, editor.link.from_port);
        nya_check(editor.link.to_node == 2 && editor.link.to_port == 0, "entering beta's input, got %llu:%u", (unsigned long long)editor.link.to_node, editor.link.to_port);

        // and now that link, declared back, draws an elbow of three fills under the nodes.
        u32 without = count_of(&recorder, NYA_UI_WIDGET_RULE);

        links[0]   = editor.link;
        link_count = 1;
        draw_twice(&recorder);

        nya_check(count_of(&recorder, NYA_UI_WIDGET_RULE) == without + 3, "a link is a three segment elbow, got %u past %u", count_of(&recorder, NYA_UI_WIDGET_RULE), without);
    }

    // Grabbing an input stub asks the caller to detach whatever ran into it.
    {
        editor     = (NYA_UINodeEditor){ .zoom = 1.0F };
        pos_a      = (f32x2){ 20.0F, 20.0F };
        pos_b      = (f32x2){ 300.0F, 40.0F };
        link_count = 0;

        draw_twice(&recorder);

        f32x2 in = anchor(box_of(pos_b), false, 0);

        // the detach is reported on the press, the pass a caller reads it the same as the connect, so it is checked there rather than after the release, which the next pass would have cleared.
        pointer_move(in);
        pointer_button(true);
        graph(NYA_UI_PASS_INPUT);

        nya_check(editor.disconnected && editor.detach_node == 2 && editor.detach_port == 0, "grabbing an input asks to detach it, got %d %llu:%u", editor.disconnected, (unsigned long long)editor.detach_node, editor.detach_port);

        pointer_button(false);
        graph(NYA_UI_PASS_INPUT);
    }

    // Dragging empty canvas pans it, and the nodes move with the pan.
    {
        editor     = (NYA_UINodeEditor){ .zoom = 1.0F };
        pos_a      = (f32x2){ 20.0F, 20.0F };
        pos_b      = (f32x2){ 300.0F, 40.0F };
        link_count = 0;

        draw_twice(&recorder);

        f32x2 empty = { editor._canvas.x + 420.0F, editor._canvas.y + 320.0F }; // clear of both boxes.

        pointer_move(empty);
        pointer_button(true);
        graph(NYA_UI_PASS_INPUT); // the press lands on nothing, so the end arms a pan.

        pointer_move((f32x2){ empty.x + 30.0F, empty.y + 20.0F });
        graph(NYA_UI_PASS_INPUT); // the next move pans.

        pointer_button(false);
        graph(NYA_UI_PASS_INPUT);

        nya_check(editor.pan.x == 30.0F && editor.pan.y == 20.0F, "empty canvas pans by the pointer, got %f,%f", (f64)editor.pan.x, (f64)editor.pan.y);

        // and the node's box has moved with it, which is the transform reading the pan.
        NYA_Rectf a = box_of(pos_a);
        draw_twice(&recorder);
        nya_check(panel_at(&recorder, a.x, a.y) != nullptr, "the node follows the pan to (%f,%f)", (f64)a.x, (f64)a.y);
    }

    // The wheel zooms about the pointer, and hit testing follows: the node is grabbable at its zoomed position.
    {
        editor     = (NYA_UINodeEditor){ .zoom = 1.0F };
        pos_a      = (f32x2){ 20.0F, 20.0F };
        pos_b      = (f32x2){ 300.0F, 40.0F };
        link_count = 0;

        draw_twice(&recorder);

        f32x2 over = { editor._canvas.x + 20.0F, editor._canvas.y + 20.0F };
        f32   was  = editor.zoom;

        pointer_move(over);
        wheel(1.0F);
        graph(NYA_UI_PASS_INPUT);

        nya_check(editor.zoom > was, "the wheel zooms in, got %f from %f", (f64)editor.zoom, (f64)was);

        // the box is bigger now, and pressing its zoomed title still grabs it — so the pointer reads through the zoom.
        NYA_Rectf a      = box_of(pos_a);
        f32x2     handle = { a.x + (a.width * 0.5F), a.y + (title_h() * 0.5F) };

        pointer_move(handle);
        pointer_button(true);
        graph(NYA_UI_PASS_INPUT);

        pointer_move((f32x2){ handle.x + 40.0F, handle.y });
        graph(NYA_UI_PASS_INPUT);

        pointer_button(false);
        graph(NYA_UI_PASS_INPUT);

        // 40 window pixels moved is 40 / (scale * zoom) graph pixels, which is fewer than at zoom one.
        f32 expected = 20.0F + (40.0F / editor.zoom);
        nya_check(fabsf(pos_a.x - expected) < 0.01F, "a drag at zoom %f moves by the zoomed distance, got %f want %f", (f64)editor.zoom, (f64)pos_a.x, (f64)expected);
    }

    return nya_check_failures() == 0 ? 0 : 1;
}
