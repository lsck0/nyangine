/**
 * @file ui_node.c
 *
 * A node graph editor built from the primitives already in the module: a canvas is a frameless panel that clips to
 * itself and reads the pan and zoom off the caller's state; a node is the theme's panel with its accent for the port
 * stubs; a link is three of the flat fills a rule is drawn from, bent into an elbow. Nothing here names a drawing
 * primitive or a colour of the backend's — every rectangle goes out as a NYA_UIWidgetDraw the presenters already
 * answer, so the whole thing draws on a GPU, in a terminal and through the recorder that tests it. See ui.h.
 *
 * Why it is shaped this way
 *
 * - The canvas keeps nothing. A node's position, the pan, the zoom and the drag in flight all live in the caller's
 *   NYA_UINodeEditor and the caller's own f32x2 positions, for the reason a window keeps its state outside the
 *   widget: an immediate mode call that held its own graph could never be redrawn, because the call that would
 *   redraw it is the one being made. The widget reports the connect and disconnect gestures and the caller folds
 *   them into whatever it keeps the graph in.
 * - Nodes take no room in the layout. A canvas positions its own children, so each node is drawn at an explicit
 *   rectangle rather than through nya_ui_place, the way a window's chrome buttons are. Hit testing is the same
 *   manual test the panel drag uses, gated by the same `covered`, `claimed` and clip checks a widget answers to, so
 *   a node under another panel is not grabbed and a node dragged off the canvas is cut by it.
 * - A graph point maps to a window point by `canvas origin + pan + point * scale * zoom`, and back the other way for
 *   hit testing, so panning and zooming move the nodes and the places the pointer is read against by exactly the
 *   same transform. The zoom scales the boxes and the port positions but not the text: a font rasterises an atlas
 *   per point size, so zooming the type would mint one per notch, the same reason the display scale is snapped.
 * - Links are drawn a layer below the nodes, so a wire crossing a node runs under it rather than over. The canvas
 *   panel draws its background at its own layer, bumps one layer for the nodes, and a link drops back to the base to
 *   draw. A link is declared after the nodes because it looks up where this pass placed each of its ends, recorded
 *   in the editor's per-pass port table as the nodes go by.
 * */
#include "nyangine/nyangine.h"

#include "nyangine/ui/ui_internal.h"


// TYPES

/** Everything a node's rectangles are worked out from once, so its input pass and its draw pass agree to the pixel. */
typedef struct {
    NYA_Rectf box;
    NYA_Rectf title;

    f32 title_h;
    f32 row;
    f32 marker;
    f32 pad;
} _NYA_UINodeMetrics;


// PRIVATE API DECLARATION

/** The window point a graph point sits at, and the graph point a window point is over, through the pan and zoom. */
NYA_INTERNAL f32x2 _nya_ui_node_to_screen(const NYA_UINodeEditor* editor, f32 scale, f32x2 graph) __attr_no_discard;
NYA_INTERNAL f32x2 _nya_ui_node_to_graph(const NYA_UINodeEditor* editor, f32 scale, f32x2 point) __attr_no_discard;

/** The combined scale a node is drawn at: the pass's own scale times the canvas zoom. */
NYA_INTERNAL f32 _nya_ui_node_scale(const NYA_UI* ui, const NYA_UINodeEditor* editor) __attr_no_discard;

/** A node's rectangles at the current pan and zoom, from the caller's NYA_UINode. */
NYA_INTERNAL _NYA_UINodeMetrics _nya_ui_node_metrics(const NYA_UI* ui, const NYA_UINodeEditor* editor, const NYA_UINode* node) __attr_no_discard;

/** One port stub's square, `index` down `side`'s edge, and the point on the edge a link meets it at. */
NYA_INTERNAL NYA_Rectf _nya_ui_node_port_rect(const _NYA_UINodeMetrics* metrics, b8 output, u32 index) __attr_no_discard;
NYA_INTERNAL f32x2     _nya_ui_node_port_anchor(const _NYA_UINodeMetrics* metrics, b8 output, u32 index) __attr_no_discard;

/** Remembers where a port ended up this pass, so a link declared later can route to it. Dropped when the table fills. */
NYA_INTERNAL void _nya_ui_node_port_record(NYA_UINodeEditor* editor, u64 node, u32 port, b8 output, f32x2 at);

/** The anchor of the port `(node, port)` on `output`'s side this pass, or false when it was not on screen. */
NYA_INTERNAL b8 _nya_ui_node_port_find(const NYA_UINodeEditor* editor, u64 node, u32 port, b8 output, f32x2* out) __attr_no_discard;

/** Draws the elbow from `from` to `to` in `color`, three flat fills thick by `scale`, in the layer `layer`. */
NYA_INTERNAL void _nya_ui_node_wire(NYA_UI* ui, s32 layer, s32 restore, f32x2 from, f32x2 to, NYA_Color color, f32 scale);

/** One flat fill, in the colour a rule draws, the segment of a wire between two points on one axis. */
NYA_INTERNAL void _nya_ui_node_segment(NYA_UI* ui, NYA_Rectf rect, NYA_Color color);


// THE CANVAS

b8 nya_ui_node_editor_begin(NYA_UI* ui, NYA_ConstCString id, NYA_UINodeEditor* editor) {
    nya_assert(ui != nullptr && ui == _nya_ui.open);
    nya_assert(id != nullptr, "a canvas is named, so its size and its place survive the list it is in being reordered");
    nya_assert(editor != nullptr, "a canvas reports into the caller's state; see NYA_UINodeEditor");

    // a zeroed editor is unzoomed, and the zoom is kept in range whatever the caller last left it at.
    if (editor->zoom <= 0.0F) editor->zoom = 1.0F;
    editor->zoom = nya_clamp(editor->zoom, NYA_UI_NODE_ZOOM_MIN, NYA_UI_NODE_ZOOM_MAX);

    // the events are cleared here rather than at the end, so a caller that never opens the canvas reads no stale link.
    editor->connected    = false;
    editor->disconnected = false;

    // the canvas fills its container: a frameless panel whose children take no room, since a node is placed by the transform rather than stacked.
    if (!nya_ui_panel_begin(ui, id, (NYA_UIPanel){ .width = nya_ui_grow(1), .height = nya_ui_grow(1), .frameless = true })) return false;

    _NYA_UILayout*    layout = &_nya_ui.layouts[_nya_ui.depth - 1];
    const NYA_UILook* look   = _nya_ui_look();
    NYA_Rectf         canvas = layout->bounds;

    // cut everything drawn inside to the canvas, and make the end put the scissor back, since nothing here scrolls.
    layout->clip     = nya_rect_intersection(layout->clip, canvas);
    layout->clipping = true;

    editor->_canvas     = canvas;
    editor->_hit        = false;
    editor->_port_count = 0;

    if (ui->pass == NYA_UI_PASS_INPUT) {
        b8 over = !layout->covered && !_nya_ui_claimed(_nya_ui.pointer) && nya_rect_contains(canvas, _nya_ui.pointer);

        // the wheel zooms about the pointer: the graph point under it is held while the zoom changes, so the canvas grows toward the cursor rather than the origin; the wheel is spent here so nothing behind also scrolls.
        if (over && _nya_ui.wheel != 0.0F) {
            f32   before = _nya_ui_node_scale(ui, editor);
            f32x2 fixed  = _nya_ui_node_to_graph(editor, before, _nya_ui.pointer);

            editor->zoom = nya_clamp(editor->zoom * (1.0F + (_nya_ui.wheel * NYA_UI_NODE_ZOOM_STEP)), NYA_UI_NODE_ZOOM_MIN, NYA_UI_NODE_ZOOM_MAX);

            f32 after     = _nya_ui_node_scale(ui, editor);
            editor->pan.x = _nya_ui.pointer.x - canvas.x - (fixed.x * after);
            editor->pan.y = _nya_ui.pointer.y - canvas.y - (fixed.y * after);

            _nya_ui.wheel   = 0.0F;
            _nya_ui.wheel_x = 0.0F;
        }

        // a pan already under way follows the pointer by where it was grabbed, and lets go when the button does.
        if (editor->_panning) {
            if (_nya_ui.pointer_down) {
                editor->pan = (f32x2){ _nya_ui.pointer.x - editor->_pan_grip.x, _nya_ui.pointer.y - editor->_pan_grip.y };
            } else {
                editor->_panning = false;
            }
        }
    }

    // the backdrop, in the theme's track colour, at the canvas's own layer and under everything declared after it.
    if (_nya_ui_drawn(canvas)) {
        NYA_UIWidgetDraw backdrop = { .kind = NYA_UI_WIDGET_STRIPE, .rect = canvas, .color = look->style.track };

        _nya_ui_draw(ui, &backdrop);
    }

    // the nodes draw a layer above the wires; the end and the link routine move between the two.
    editor->_layer  = layout->layer;
    layout->layer  += 1;
    _nya_ui_layer_set(ui, layout->layer);

    if (ui->pass == NYA_UI_PASS_DRAW) _nya_ui_scissor(ui, layout->clip);

    return true;
}

void nya_ui_node_editor_end(NYA_UI* ui, NYA_UINodeEditor* editor) {
    nya_assert(ui != nullptr && ui == _nya_ui.open && editor != nullptr);
    nya_assert(_nya_ui.depth > 1, "nya_ui_node_editor_end without a begin");

    const _NYA_UILayout* layout = &_nya_ui.layouts[_nya_ui.depth - 1];

    if (ui->pass == NYA_UI_PASS_INPUT) {
        b8 over = !layout->covered && !_nya_ui_claimed(_nya_ui.pointer) && nya_rect_contains(editor->_canvas, _nya_ui.pointer);

        // a press that landed on empty canvas — no node and no port took it — starts a pan from the next move on.
        if (!editor->_panning && !editor->_linking && editor->_drag_node == 0 && !editor->_hit && _nya_ui.pointer_pressed && over) {
            editor->_panning  = true;
            editor->_pan_grip = (f32x2){ _nya_ui.pointer.x - editor->pan.x, _nya_ui.pointer.y - editor->pan.y };
        }

        // a link dragged out and dropped on nothing is let go, since no input port claimed it this pass.
        if (editor->_linking && _nya_ui.pointer_released && !editor->connected) editor->_linking = false;
    }

    // the in-flight link, from the output stub it left to wherever the pointer is now, drawn under the nodes.
    if (editor->_linking && _nya_ui_drawing()) {
        f32x2 from = { 0.0F, 0.0F };

        if (_nya_ui_node_port_find(editor, editor->_link_node, editor->_link_port, true, &from)) {
            _nya_ui_node_wire(ui, editor->_layer, editor->_layer, from, _nya_ui.pointer, _nya_ui_look()->style.accent, _nya_ui_node_scale(ui, editor));
        }
    }

    // the panel end sees the bumped layer differ from the parent's and puts the layer, the scissor and the depth back.
    nya_ui_panel_end(ui);
}


// A NODE

b8 nya_ui_node(NYA_UI* ui, NYA_UINode node, NYA_UINodeEditor* editor) {
    nya_assert(ui != nullptr && ui == _nya_ui.open && editor != nullptr);
    nya_assert(_nya_ui.depth > 1, "a node is declared inside a node editor");
    nya_assert(node.key != 0, "a node's key names it in a reported link and is never zero");
    nya_assert(node.position != nullptr, "a node is placed at a caller-owned position it can be dragged to write");

    const _NYA_UILayout* layout  = &_nya_ui.layouts[_nya_ui.depth - 1];
    const NYA_UILook*    look    = _nya_ui_look();
    const NYA_UIStyle*   style   = &look->style;
    f32                  scale   = _nya_ui_node_scale(ui, editor);
    _NYA_UINodeMetrics   metrics = _nya_ui_node_metrics(ui, editor, &node);

    b8 moved = false;

    if (ui->pass == NYA_UI_PASS_INPUT) {
        f32x2 at     = _nya_ui.pointer;
        b8    usable = !layout->covered && !_nya_ui_claimed(at) && nya_rect_contains(layout->clip, at);

        // the drag this node owns follows the pointer by the graph point it was grabbed at, so the box does not jump.
        if (editor->_drag_node == node.key) {
            if (_nya_ui.pointer_down) {
                f32x2 graph = _nya_ui_node_to_graph(editor, scale, at);
                f32x2 next  = { graph.x - editor->_drag_offset.x, graph.y - editor->_drag_offset.y };

                moved            = next.x != node.position->x || next.y != node.position->y;
                *node.position   = next;
            } else {
                editor->_drag_node = 0;
            }
        }

        // a fresh press on this node claims it and clears what an earlier, lower node put here this pass, so the topmost node under the pointer acts (a prior press is already resolved by now); the reachable area is the box grown by half a stub, since a port straddles the edge it sits on.
        NYA_Rectf reach = nya_rect_expand(metrics.box, roundf(metrics.marker * 0.5F));

        if (usable && _nya_ui.pointer_pressed && nya_rect_contains(reach, at)) {
            editor->_hit       = true;
            editor->_drag_node = 0;
            editor->_linking   = false;

            b8 on_port = false;

            // an output stub starts a link the pointer drags out; the ports come before the title, so a stub on the bar's row is a port rather than a handle.
            for (u32 i = 0; i < node.outputs; i++) {
                if (!nya_rect_contains(_nya_ui_node_port_rect(&metrics, true, i), at)) continue;

                editor->_linking   = true;
                editor->_link_node = node.key;
                editor->_link_port = i;
                on_port            = true;
            }

            // an input stub asks to detach whatever ran into it; the caller owns the links, so it drops the one there.
            for (u32 i = 0; i < node.inputs; i++) {
                if (!nya_rect_contains(_nya_ui_node_port_rect(&metrics, false, i), at)) continue;

                editor->disconnected = true;
                editor->detach_node  = node.key;
                editor->detach_port  = i;
                on_port              = true;
            }

            // the title bar is the handle, the way a window's is; the body under it is left to the ports.
            if (!on_port && nya_rect_contains(metrics.title, at)) {
                f32x2 graph          = _nya_ui_node_to_graph(editor, scale, at);
                editor->_drag_node   = node.key;
                editor->_drag_offset = (f32x2){ graph.x - node.position->x, graph.y - node.position->y };
            }
        }

        // a link let go over one of this node's input stubs completes; the topmost such node wins, as with a press.
        if (usable && editor->_linking && _nya_ui.pointer_released) {
            for (u32 i = 0; i < node.inputs; i++) {
                if (!nya_rect_contains(_nya_ui_node_port_rect(&metrics, false, i), at)) continue;

                editor->connected = true;
                editor->link      = (NYA_UINodeLink){ .from_node = editor->_link_node, .from_port = editor->_link_port, .to_node = node.key, .to_port = i };
                editor->_linking  = false;
            }
        }
    }

    // recorded every pass, drawn or not, so a link declared after the nodes can find where each port ended up.
    for (u32 i = 0; i < node.outputs; i++) _nya_ui_node_port_record(editor, node.key, i, true, _nya_ui_node_port_anchor(&metrics, true, i));
    for (u32 i = 0; i < node.inputs; i++) _nya_ui_node_port_record(editor, node.key, i, false, _nya_ui_node_port_anchor(&metrics, false, i));

    if (_nya_ui_drawn(metrics.box)) {
        // the box is the theme's panel with no title of its own (the title is a label so it stays the body size a node wants, not the panel's title size), and a strip along the top marks the drag handle.
        NYA_UIPanel      frame = { 0 };
        NYA_UIWidgetDraw box   = { .kind = NYA_UI_WIDGET_PANEL, .rect = metrics.box, .label = "", .as_panel = { .options = &frame } };
        _nya_ui_draw(ui, &box);

        NYA_UIWidgetDraw bar = { .kind = NYA_UI_WIDGET_RULE, .rect = metrics.title, .color = style->button.normal };
        _nya_ui_draw(ui, &bar);

        if (node.title != nullptr && node.title[0] != '\0') {
            NYA_Rectf text = { metrics.title.x + metrics.pad, metrics.title.y, nya_max(metrics.title.width - (metrics.pad * 2.0F), 0.0F), metrics.title.height };

            NYA_UIWidgetDraw label = {
                .kind     = NYA_UI_WIDGET_LABEL,
                .rect     = text,
                .label    = node.title,
                .color    = style->text.normal,
                .as_label = { .room = text.width, .overflow = NYA_UI_OVERFLOW_VISIBLE, .align = NYA_UI_ALIGN_START },
            };

            _nya_ui_draw(ui, &label);
        }

        // the stubs, in the accent, and each port's label beside it: an input's to the right of its stub, an output's to the left, both in the dim text colour so the port name reads as a caption on the wire.
        f32 line = look->line_heights[layout->text];

        for (u32 i = 0; i < node.inputs; i++) {
            NYA_Rectf        rect  = _nya_ui_node_port_rect(&metrics, false, i);
            NYA_UIWidgetDraw stub  = { .kind = NYA_UI_WIDGET_RULE, .rect = rect, .color = style->accent };
            _nya_ui_draw(ui, &stub);

            if (node.input_labels != nullptr && node.input_labels[i] != nullptr && node.input_labels[i][0] != '\0') {
                f32       cy   = rect.y + (rect.height * 0.5F);
                NYA_Rectf text = { rect.x + rect.width + metrics.pad, roundf(cy - (line * 0.5F)), nya_max(metrics.box.width - rect.width - (metrics.pad * 2.0F), 0.0F), line };

                NYA_UIWidgetDraw label = {
                    .kind = NYA_UI_WIDGET_LABEL, .rect = text, .label = node.input_labels[i], .color = style->text_dim,
                    .as_label = { .room = text.width, .overflow = NYA_UI_OVERFLOW_VISIBLE, .align = NYA_UI_ALIGN_START },
                };

                _nya_ui_draw(ui, &label);
            }
        }

        for (u32 i = 0; i < node.outputs; i++) {
            NYA_Rectf        rect  = _nya_ui_node_port_rect(&metrics, true, i);
            NYA_UIWidgetDraw stub  = { .kind = NYA_UI_WIDGET_RULE, .rect = rect, .color = style->accent };
            _nya_ui_draw(ui, &stub);

            if (node.output_labels != nullptr && node.output_labels[i] != nullptr && node.output_labels[i][0] != '\0') {
                f32       cy    = rect.y + (rect.height * 0.5F);
                f32       right = rect.x;
                f32       left  = metrics.box.x + metrics.pad;
                NYA_Rectf text  = { left, roundf(cy - (line * 0.5F)), nya_max(right - left - metrics.pad, 0.0F), line };

                NYA_UIWidgetDraw label = {
                    .kind = NYA_UI_WIDGET_LABEL, .rect = text, .label = node.output_labels[i], .color = style->text_dim,
                    .as_label = { .room = text.width, .overflow = NYA_UI_OVERFLOW_VISIBLE, .align = NYA_UI_ALIGN_END },
                };

                _nya_ui_draw(ui, &label);
            }
        }
    }

    return moved;
}

void nya_ui_node_link(NYA_UI* ui, NYA_UINodeEditor* editor, NYA_UINodeLink link) {
    nya_assert(ui != nullptr && ui == _nya_ui.open && editor != nullptr);

    if (ui->pass != NYA_UI_PASS_DRAW) return;

    f32x2 from = { 0.0F, 0.0F };
    f32x2 to   = { 0.0F, 0.0F };

    // a link to a node not on screen this pass has nowhere to meet, so it draws nothing rather than a line to zero.
    if (!_nya_ui_node_port_find(editor, link.from_node, link.from_port, true, &from)) return;
    if (!_nya_ui_node_port_find(editor, link.to_node, link.to_port, false, &to)) return;

    // dropped to the wire layer to draw, then back to the node layer for whatever the caller declares next.
    _nya_ui_node_wire(ui, editor->_layer, editor->_layer + 1, from, to, _nya_ui_look()->style.accent, _nya_ui_node_scale(ui, editor));
}


// INTERNAL

f32 _nya_ui_node_scale(const NYA_UI* ui, const NYA_UINodeEditor* editor) {
    nya_assert(ui != nullptr && editor != nullptr);

    return ui->scale * editor->zoom;
}

f32x2 _nya_ui_node_to_screen(const NYA_UINodeEditor* editor, f32 scale, f32x2 graph) {
    return (f32x2){ editor->_canvas.x + editor->pan.x + roundf(graph.x * scale), editor->_canvas.y + editor->pan.y + roundf(graph.y * scale) };
}

f32x2 _nya_ui_node_to_graph(const NYA_UINodeEditor* editor, f32 scale, f32x2 point) {
    nya_assert(scale > 0.0F, "a canvas scale is the pass scale times a clamped zoom, both above zero");

    return (f32x2){ (point.x - editor->_canvas.x - editor->pan.x) / scale, (point.y - editor->_canvas.y - editor->pan.y) / scale };
}

_NYA_UINodeMetrics _nya_ui_node_metrics(const NYA_UI* ui, const NYA_UINodeEditor* editor, const NYA_UINode* node) {
    const _NYA_UILayout* layout = &_nya_ui.layouts[_nya_ui.depth - 1];
    const NYA_UILook*    look   = _nya_ui_look();

    f32 zoom  = editor->zoom;
    f32 scale = _nya_ui_node_scale(ui, editor);
    f32 line  = look->line_heights[layout->text];

    // the box and rows scale with the zoom; the marker uses the full scale, so a stub keeps its shape as the canvas grows, each kept to at least a pixel so a node zoomed right out is still a node rather than nothing.
    f32 pad     = nya_max(roundf(look->padding * zoom), 1.0F);
    f32 row     = nya_max(roundf(line * zoom), 1.0F);
    f32 title_h = nya_max(roundf((line + look->padding) * zoom), 1.0F);
    f32 marker  = nya_max(roundf(NYA_UI_NODE_PORT * scale), 2.0F);
    f32 width   = roundf((node->width > 0.0F ? node->width : NYA_UI_NODE_WIDTH) * scale);

    u32 rows   = nya_max(node->inputs, node->outputs);
    f32 height = title_h + ((f32)rows * row) + pad;

    f32x2 top_left = _nya_ui_node_to_screen(editor, scale, *node->position);

    return (_NYA_UINodeMetrics){
        .box     = { top_left.x, top_left.y, width, height },
        .title   = { top_left.x, top_left.y, width, title_h },
        .title_h = title_h,
        .row     = row,
        .marker  = marker,
        .pad     = pad,
    };
}

NYA_Rectf _nya_ui_node_port_rect(const _NYA_UINodeMetrics* metrics, b8 output, u32 index) {
    f32x2 at   = _nya_ui_node_port_anchor(metrics, output, index);
    f32   half = roundf(metrics->marker * 0.5F);

    return (NYA_Rectf){ at.x - half, at.y - half, metrics->marker, metrics->marker };
}

f32x2 _nya_ui_node_port_anchor(const _NYA_UINodeMetrics* metrics, b8 output, u32 index) {
    // down the edge, one row apart, centred in the row below the title bar; on the left for an input, the right for an output. The point is on the edge itself, where a wire meets the stub.
    f32 x = output ? metrics->box.x + metrics->box.width : metrics->box.x;
    f32 y = metrics->box.y + metrics->title_h + ((f32)index * metrics->row) + (metrics->row * 0.5F);

    return (f32x2){ roundf(x), roundf(y) };
}

void _nya_ui_node_port_record(NYA_UINodeEditor* editor, u64 node, u32 port, b8 output, f32x2 at) {
    if (editor->_port_count >= NYA_UI_NODE_PORTS_MAX) return;

    editor->_ports[editor->_port_count].node   = node;
    editor->_ports[editor->_port_count].port   = port;
    editor->_ports[editor->_port_count].output = output;
    editor->_ports[editor->_port_count].at     = at;
    editor->_port_count                       += 1;
}

b8 _nya_ui_node_port_find(const NYA_UINodeEditor* editor, u64 node, u32 port, b8 output, f32x2* out) {
    nya_assert(out != nullptr);

    for (u32 i = 0; i < editor->_port_count; i++) {
        if (editor->_ports[i].node != node || editor->_ports[i].port != port || editor->_ports[i].output != output) continue;

        *out = editor->_ports[i].at;
        return true;
    }

    return false;
}

void _nya_ui_node_wire(NYA_UI* ui, s32 layer, s32 restore, f32x2 from, f32x2 to, NYA_Color color, f32 scale) {
    f32 thickness = nya_max(roundf(NYA_UI_NODE_LINK * scale), 1.0F);
    f32 half      = roundf(thickness * 0.5F);
    f32 mid       = roundf((from.x + to.x) * 0.5F);

    _nya_ui_layer_set(ui, layer);

    // an elbow: out from the source along x to the midpoint, down y to the target's row, then in to the target. Each segment is a thickness longer than the gap so the corners meet rather than leave a pixel of daylight.
    f32 x0 = nya_min(from.x, mid);
    f32 x1 = nya_max(from.x, mid);
    _nya_ui_node_segment(ui, (NYA_Rectf){ x0, from.y - half, (x1 - x0) + thickness, thickness }, color);

    f32 y0 = nya_min(from.y, to.y);
    f32 y1 = nya_max(from.y, to.y);
    _nya_ui_node_segment(ui, (NYA_Rectf){ mid - half, y0 - half, thickness, (y1 - y0) + thickness }, color);

    f32 x2 = nya_min(mid, to.x);
    f32 x3 = nya_max(mid, to.x);
    _nya_ui_node_segment(ui, (NYA_Rectf){ x2, to.y - half, (x3 - x2) + thickness, thickness }, color);

    _nya_ui_layer_set(ui, restore);
}

void _nya_ui_node_segment(NYA_UI* ui, NYA_Rectf rect, NYA_Color color) {
    NYA_UIWidgetDraw draw = { .kind = NYA_UI_WIDGET_RULE, .rect = rect, .color = color };

    _nya_ui_draw(ui, &draw);
}
