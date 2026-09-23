/**
 * @file ui_layout.c
 *
 * Containers and placement: where a child goes, how much room it gets, and what a container remembers between
 * passes. See ui.h.
 * */
#include "nyangine/nyangine.h"

#include "nyangine/ui/ui_internal.h"


/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * LAYOUT
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

b8 nya_ui_panel_begin(NYA_UI* ui, NYA_ConstCString id, NYA_UIPanel panel) {
    return _nya_ui_panel_open(ui, id, panel, nullptr);
}

b8 _nya_ui_panel_open(NYA_UI* ui, NYA_ConstCString id, NYA_UIPanel panel, const NYA_Rectf* at) {
    nya_assert(ui != nullptr && ui == _nya_ui.open);
    nya_assert(panel.anchor < NYA_UI_ANCHOR_COUNT && panel.direction < NYA_UI_DIRECTION_COUNT && panel.align < NYA_UI_ALIGN_COUNT);
    nya_assert(panel.overflow < NYA_UI_OVERFLOW_COUNT && panel.text < NYA_UI_TEXT_COUNT);
    nya_assert(panel.width.kind < NYA_UI_SIZE_COUNT && panel.height.kind < NYA_UI_SIZE_COUNT && panel.children.kind < NYA_UI_SIZE_COUNT);
    nya_assert(panel.gap >= 0.0F && panel.padding >= 0.0F);
    nya_assert(at == nullptr || id != nullptr, "a float takes no place in its container, so it is named");

    const _NYA_UILayout* parent = &_nya_ui.layouts[_nya_ui.depth - 1];
    const NYA_UILook*    look   = _nya_ui_look();
    const NYA_UISkin*    skin   = &look->style.panel_skin;

    u64 key   = id != nullptr ? _nya_ui_id(parent->scope, id) : _nya_ui_id_at(parent->key, parent->count);
    u32 index = _nya_ui_panel_claim(key);

    if (index == U32_MAX) {
        _nya_ui.next_set = false;
        return false;
    }

    _NYA_UIPanelState* state = &_nya_ui.panels[index];
    state->pass              = _nya_ui.pass_serial;

    b8 floating  = at != nullptr;
    b8 top_level = !floating && _nya_ui.depth == 1;
    b8 covered   = !floating && parent->covered;

    // written every pass, so a container moved inside another leaves the stack the pass it moves.
    state->top_level = top_level;

    if (top_level) {
        // declaring it is its first raise, so a stack nobody has clicked is the order the calls came in.
        if (state->order == 0) {
            _nya_ui.raise_serial += 1;
            state->order          = _nya_ui.raise_serial;
        }

        state->z = panel.z;
        covered  = _nya_ui_panel_covered(ui, index);
    }

    NYA_UIText text = panel.text != NYA_UI_TEXT_INHERIT ? panel.text : parent->text;

    // frameless is only what it asks for; a frame pads by its skin's insets or the style's padding, inside the outline.
    f32x2 before = { _nya_ui_px(panel.padding), _nya_ui_px(panel.padding) };
    f32x2 after  = before;

    if (!panel.frameless && panel.padding <= 0.0F && skin->texture[0] != '\0') {
        before = (f32x2){ _nya_ui_px(skin->left), _nya_ui_px(skin->top) };
        after  = (f32x2){ _nya_ui_px(skin->right), _nya_ui_px(skin->bottom) };
    } else if (!panel.frameless) {
        f32 pad = (panel.padding > 0.0F ? before.x : look->padding) + look->outline;
        before  = (f32x2){ pad, pad };
        after   = before;
    }

    f32 header      = 0.0F;
    f32 title_width = 0.0F;

    if (panel.title != nullptr) {
        header      = look->title_bar + look->spacing;
        title_width = _nya_ui_text_width(NYA_UI_TEXT_TITLE, panel.title);
    }

    f32x2            chrome   = { before.x + after.x, before.y + after.y + header };
    const NYA_UISize sizes[2] = { panel.width, panel.height };

    NYA_Rectf bounds;
    f32x2     room;

    if (floating) {
        const NYA_Rectf* screen = &_nya_ui.layouts[0].clip;

        f32x2 size = {
            at->width > 0.0F ? at->width : _nya_ui_extent(panel.width, state->size.x, screen->width),
            at->height > 0.0F ? at->height : _nya_ui_extent(panel.height, state->size.y, screen->height),
        };

        // pushed back inside the window rather than hanging off it, so a list opened on the last row is still whole.
        bounds = (NYA_Rectf){
            .x      = nya_clamp(at->x, screen->x, nya_max(screen->x + screen->width - size.x, screen->x)),
            .y      = nya_clamp(at->y, screen->y, nya_max(screen->y + screen->height - size.y, screen->y)),
            .width  = size.x,
            .height = size.y,
        };

        room = size;
    } else if (_nya_ui.depth == 1) {
        const NYA_Rectf* safe = &_nya_ui.safe;

        f32x2 size = {
            _nya_ui_extent(panel.width, state->size.x, safe->width),
            _nya_ui_extent(panel.height, state->size.y, safe->height),
        };

        // 0, 0.5 or 1 across and down, so one expression anchors to either edge or the middle.
        u32   column = panel.anchor % 3;
        u32   row    = panel.anchor / 3;
        f32   across = (f32)column * 0.5F;
        f32   down   = (f32)row * 0.5F;
        f32x2 offset = { _nya_ui_px(panel.offset.x), _nya_ui_px(panel.offset.y) };

        bounds = (NYA_Rectf){
            .x      = roundf(safe->x + ((safe->width - size.x) * across) + (offset.x * (1.0F - (across * 2.0F)))),
            .y      = roundf(safe->y + ((safe->height - size.y) * down) + (offset.y * (1.0F - (down * 2.0F)))),
            .width  = size.x,
            .height = size.y,
        };

        // slides up into place when it shows after being gone, on the wall clock so a paused world still animates.
        if (look->style.appear_s > 0.0F) {
            f64 now = nya_app_uptime_s();
            if (now - state->seen_s > NYA_UI_APPEAR_GAP_S) state->shown_s = now;
            state->seen_s = now;

            f32 t     = nya_ease(look->style.easing, nya_clamp((f32)(now - state->shown_s) / look->style.appear_s, 0.0F, 1.0F));
            bounds.y += roundf((1.0F - t) * _nya_ui_px(NYA_UI_APPEAR_OFFSET));
        }

        // moved by the pointer, then clamped so a panel dragged at the edge, or a shrinking window, cannot strand it.
        if (panel.draggable) {
            /*
             * Where it is on screen, which is where it was anchored plus everything it has been dragged
             * by so far. The grab has to be taken from this and not from the anchor: a window moved once
             * would otherwise keep its handle where it used to be, and the second grab would do nothing.
             */
            f32 shown_x = nya_clamp(bounds.x + state->drag.x, safe->x, nya_max(safe->x + safe->width - bounds.width, safe->x));
            f32 shown_y = nya_clamp(bounds.y + state->drag.y, safe->y, nya_max(safe->y + safe->height - bounds.height, safe->y));

            /*
             * From the top edge down to the bottom of the bar that is drawn, rather than the strip the
             * layout reserved. The two are not the same rectangle: the bar sits a padding below the top
             * edge and the reservation adds the spacing under it, so the reserved strip stops short of
             * the bar's bottom — which is exactly where a person aims when the title is tall. Taking the
             * frame's top edge with it keeps the whole visible header a handle.
             */
            f32       strip = header > 0.0F ? before.y + look->title_bar : look->line_heights[NYA_UI_TEXT_BODY];
            NYA_Rectf grip  = { shown_x, shown_y, bounds.width, strip };

            _nya_ui_panel_drag(ui, key, state, grip, covered);

            f32 x = nya_clamp(bounds.x + state->drag.x, safe->x, nya_max(safe->x + safe->width - bounds.width, safe->x));
            f32 y = nya_clamp(bounds.y + state->drag.y, safe->y, nya_max(safe->y + safe->height - bounds.height, safe->y));

            state->drag = (f32x2){ x - bounds.x, y - bounds.y };
            bounds.x    = x;
            bounds.y    = y;
        }

        // what the safe area leaves past the offset, as a margin on both sides unless the anchor centres that axis.
        room = (f32x2){ safe->width - (across == 0.5F ? 0.0F : offset.x * 2.0F), safe->height - (down == 0.5F ? 0.0F : offset.y * 2.0F) };

        for (u32 axis = 0; axis < 2; axis++) {
            if (sizes[axis].kind == NYA_UI_SIZE_FIXED || sizes[axis].kind == NYA_UI_SIZE_GROW) room[axis] = nya_min(room[axis], size[axis]);
            if (sizes[axis].max > 0.0F) room[axis] = nya_min(room[axis], _nya_ui_px(sizes[axis].max));
        }

        // A titled or draggable top level panel swallows a pointer inside it, so a scene under a non-modal
        // UI (nya_ui_pointer_over) can skip a world click that landed on the panel. The frameless HUD is a
        // read-only overlay and takes no clicks, so it is left out.
        if (ui->pass == NYA_UI_PASS_INPUT && (panel.title != nullptr || panel.draggable) && nya_rect_contains(bounds, _nya_ui.pointer)) {
            _nya_ui.pointer_over_panel = true;
        }
    } else {
        u32 main  = parent->main;
        u32 cross = 1 - main;

        f32x2 natural = state->size;
        b8    fill    = sizes[cross].kind == NYA_UI_SIZE_AUTO;

        // across, a fixed or growing size is decided here; along, the container decides with the rest of its children.
        f32 parent_across = parent->extent[cross] > 0.0F ? parent->extent[cross] : parent->room[cross];
        if (sizes[cross].kind == NYA_UI_SIZE_FIXED || sizes[cross].kind == NYA_UI_SIZE_GROW) natural[cross] = _nya_ui_extent(sizes[cross], natural[cross], parent_across);

        NYA_UISize along = _nya_ui_next_size(parent, sizes[main]);
        bounds           = _nya_ui_place(sizes[main], natural, fill);

        f32x2 placed = { bounds.width, bounds.height };

        room[cross] = sizes[cross].kind == NYA_UI_SIZE_FIT || placed[cross] <= 0.0F ? parent_across : placed[cross];
        room[main]  = along.kind == NYA_UI_SIZE_FIXED || along.kind == NYA_UI_SIZE_GROW ? placed[main] : parent->room[main];

        for (u32 axis = 0; axis < 2; axis++) {
            if (sizes[axis].max > 0.0F) room[axis] = nya_min(room[axis], _nya_ui_px(sizes[axis].max));
        }
    }

    s32 layer = parent->layer;

    if (top_level) {
        state->bounds = bounds;

        // a press anywhere in the topmost panel under the pointer raises it, chrome and widgets alike, which is
        // what makes a dragged panel behave: it comes forward the moment it is touched and stays there.
        if (ui->pass == NYA_UI_PASS_INPUT && _nya_ui.pointer_pressed && !covered && !_nya_ui_claimed(_nya_ui.pointer) &&
            nya_rect_contains(bounds, _nya_ui.pointer)) {
            // a press that brought this panel out from under another is spent on that, and not on a
            // widget inside it: somebody aiming at a window they cannot fully see is aiming at the window.
            if (_nya_ui_panel_raise(ui, index)) _nya_ui.raise_swallowed = state->id;
        }

        // the renderer paints layers low to high whatever order the calls came in, so a raised panel declared
        // first still draws over the ones after it. See nya_render2d_layer_set.
        layer = _nya_ui.layer_base + 1 + (s32)_nya_ui_panel_rank(ui, index);
    }

    // over every panel whatever it was raised to: a list belongs over the thing that opened it, and that thing is
    // already the panel in front when the list is open at all.
    if (floating) layer = _nya_ui.layer_base + 1 + (s32)NYA_UI_PANELS_MAX;

    // only when it moves: an ordinary nested container draws in its parent's layer, and the backends treat a set as
    // a reason to end the batch they were filling.
    if (layer != parent->layer) _nya_ui_layer_set(ui, layer);

    f32x2 extent = { nya_max(bounds.width - chrome.x, 0.0F), nya_max(bounds.height - chrome.y, 0.0F) };
    u32   main   = panel.direction == NYA_UI_DIRECTION_ROW ? 0 : 1;
    f32   gap    = panel.gap > 0.0F ? _nya_ui_px(panel.gap) : look->spacing;

    // an axis scrolls when what the content measured last pass is longer than the room it has now.
    b8 scrolls[2] = {
        state->measured && extent.x > 0.0F && state->content.x > extent.x + 0.5F,
        state->measured && extent.y > 0.0F && state->content.y > extent.y + 0.5F,
    };

    f32x2 scroll = { scrolls[0] ? state->scroll.x : 0.0F, scrolls[1] ? state->scroll.y : 0.0F };
    f32   top    = bounds.y + before.y + header;

    // a float is cut by the window, not by the container that opened it, which is half of what floating means.
    NYA_Rectf clip = floating ? _nya_ui.layouts[0].clip : parent->clip;

    if (scrolls[0] || scrolls[1]) {
        // inside the outline, and a little above the content so a focused widget's edge is not cut.
        f32       inset     = panel.frameless ? 0.0F : look->outline;
        f32       above     = panel.frameless ? top : top - roundf(before.y * 0.5F);
        NYA_Rectf view_clip = { bounds.x + inset, above, bounds.width - (inset * 2.0F), bounds.y + bounds.height - inset - above };

        clip = nya_rect_intersection(clip, view_clip);
    }

    f32 gaps = state->count > 1 ? gap * (f32)(state->count - 1) : 0.0F;

    _NYA_UILayout* layout = _nya_ui_layout_push();
    *layout               = (_NYA_UILayout){
        .origin      = { bounds.x + before.x - scroll.x, top - scroll.y },
        .extent      = extent,
        .room        = { nya_max(room.x - chrome.x, 0.0F), nya_max(room.y - chrome.y, 0.0F) },
        .main        = main,
        .gap         = gap,
        .children    = panel.children,
        .align       = panel.align,
        .overflow    = panel.overflow != NYA_UI_OVERFLOW_INHERIT ? panel.overflow : parent->overflow,
        .text        = text,
        .grow_space  = extent[main] > 0.0F ? nya_max(extent[main] - state->fixed - gaps, 0.0F) : 0.0F,
        .grow_total  = state->grow,
        .key         = key,
        .scope       = id != nullptr ? key : parent->scope,
        .group       = main == 1 ? U32_MAX : (parent->main == 0 ? parent->group : _nya_ui.widget_count),
        .panel       = index,
        .options     = panel,
        .bounds      = bounds,
        .root_panel  = top_level ? index : parent->root_panel,
        .layer       = layer,
        .floating    = floating,
        .before      = before,
        .after       = after,
        .header      = header,
        .title_width = title_width,
        .scroll      = scroll,
        .clip        = clip,
        .covered     = covered,
        .scrolls     = { scrolls[0], scrolls[1] },
        .hidden      = parent->hidden || !state->measured || look->line_heights[text] <= 0.0F,
    };

    // a float over a scrolling panel is not the scrolling panel's, so the scissor that panel set comes off first.
    if (floating && ui->pass == NYA_UI_PASS_DRAW) _nya_ui_scissor(ui, clip);

    if (!_nya_ui_drawing()) return true;

    // the whole frame in one call, the title with it: what a panel looks like is not the layout's business.
    NYA_UIWidgetDraw frame = {
        .kind     = NYA_UI_WIDGET_PANEL,
        .rect     = bounds,
        .label    = panel.title,
        .as_panel = { .options       = &layout->options,
                      .title_width   = title_width,
                      .inset         = before,
                      .bar           = header > 0.0F ? look->title_bar : 0.0F,
                      .title_room    = panel.title_room },
    };

    _nya_ui_draw(ui, &frame);

    if (scrolls[0] || scrolls[1]) {
        _nya_ui_scissor(ui, clip);
        layout->clipping = true;
    }

    return true;
}

void nya_ui_panel_end(NYA_UI* ui) {
    nya_assert(ui != nullptr && ui == _nya_ui.open);
    nya_assert(_nya_ui.depth > 1, "no container is open");

    const _NYA_UILayout* layout = &_nya_ui.layouts[_nya_ui.depth - 1];
    _nya_ui.depth              -= 1;

    const _NYA_UILayout* parent  = &_nya_ui.layouts[_nya_ui.depth - 1];
    const NYA_UIPanel*   options = &layout->options;
    const NYA_UILook*    look    = _nya_ui_look();

    f32x2 content = layout->main == 1 ? (f32x2){ layout->across, layout->used } : (f32x2){ layout->used, layout->across };
    f32x2 natural = {
        nya_max(content.x, layout->title_width) + layout->before.x + layout->after.x,
        content.y + layout->header + layout->before.y + layout->after.y,
    };

    const NYA_UISize sizes[2] = { options->width, options->height };

    for (u32 axis = 0; axis < 2; axis++) {
        natural[axis] = nya_max(natural[axis], _nya_ui_px(sizes[axis].min));
        if (sizes[axis].max > 0.0F) natural[axis] = nya_min(natural[axis], _nya_ui_px(sizes[axis].max));
    }

    // a face still loading measures zero, and that size must not stick.
    _NYA_UIPanelState* state = &_nya_ui.panels[layout->panel];
    state->size              = (f32x2){ ceilf(natural.x), ceilf(natural.y) };
    state->measured          = look->line_heights[layout->text] > 0.0F;
    state->content           = content;
    state->fixed             = layout->fixed;
    state->grow              = layout->grow;
    state->count             = layout->count;

    f32x2 reach = {
        layout->extent.x > 0.0F ? nya_max(content.x - layout->extent.x, 0.0F) : 0.0F,
        layout->extent.y > 0.0F ? nya_max(content.y - layout->extent.y, 0.0F) : 0.0F,
    };

    // the innermost scrolling container under the pointer takes the wheel, since it ends first.
    if (!layout->covered && !_nya_ui_claimed(_nya_ui.pointer) && (reach.x > 0.0F || reach.y > 0.0F) &&
        nya_rect_contains(nya_rect_intersection(layout->bounds, layout->floating ? layout->clip : parent->clip), _nya_ui.pointer)) {
        f32 step = _nya_ui_px(NYA_UI_SCROLL_STEP);

        // shift turns the wheel sideways, and so does a container that only has somewhere to go across.
        b8  sideways = (nya_input_modifiers() & NYA_KEYMOD_SHIFT) != 0 || reach.y <= 0.0F;
        u32 axis     = sideways ? 0 : 1;

        if (reach[axis] > 0.0F && _nya_ui.wheel != 0.0F) {
            state->scroll[axis] -= _nya_ui.wheel * step;
            _nya_ui.wheel        = 0.0F;
        }

        // a trackpad's own sideways scroll, which arrives on its own axis whatever the modifiers say.
        if (reach.x > 0.0F && _nya_ui.wheel_x != 0.0F) {
            state->scroll.x -= _nya_ui.wheel_x * step;
            _nya_ui.wheel_x  = 0.0F;
        }
    }

    state->scroll.x = nya_clamp(state->scroll.x, 0.0F, reach.x);
    state->scroll.y = nya_clamp(state->scroll.y, 0.0F, reach.y);

    if (layout->clipping) {
        _nya_ui_scissor(ui, layout->floating ? layout->clip : parent->clip);

        for (u32 axis = 0; axis < 2; axis++) {
            if (layout->scrolls[axis]) _nya_ui_scrollbar_draw(ui, layout, axis);
        }
    }

    // after the scrollbars, which belong to the panel, and back to where the container was opened from, so anything
    // the caller draws between two panels lands in the layer it asked for rather than in the last panel's.
    if (layout->layer != parent->layer) _nya_ui_layer_set(ui, parent->layer);
}

b8 _nya_ui_float_begin(NYA_UI* ui, NYA_ConstCString id, NYA_UIPanel panel, NYA_Rectf at) {
    nya_assert(ui != nullptr && ui == _nya_ui.open && id != nullptr);
    nya_assert(_nya_ui.claim_count < NYA_UI_CLAIMS_MAX, "more than NYA_UI_CLAIMS_MAX floats in one pass");

    // whatever the row before it asked for is the row's, not the list's.
    _nya_ui.next_set = false;

    return _nya_ui_panel_open(ui, id, panel, &at);
}

void _nya_ui_float_end(NYA_UI* ui) {
    nya_assert(ui != nullptr && ui == _nya_ui.open);
    nya_assert(_nya_ui.layouts[_nya_ui.depth - 1].floating, "_nya_ui_float_end without a _nya_ui_float_begin");

    NYA_Rectf bounds = _nya_ui.layouts[_nya_ui.depth - 1].bounds;

    nya_ui_panel_end(ui);

    // claimed as it closes, so its own widgets were reached and everything declared after it is not. See ui.h.
    nya_assert(_nya_ui.claim_count < NYA_UI_CLAIMS_MAX);
    _nya_ui.claims[_nya_ui.claim_count++] = bounds;

    // the scissor the container under it had set is back on, since that container is still open.
    if (ui->pass == NYA_UI_PASS_DRAW) _nya_ui_scissor(ui, _nya_ui.layouts[_nya_ui.depth - 1].clip);
}

b8 _nya_ui_claimed(f32x2 point) {
    for (u32 i = 0; i < _nya_ui.claim_count; i++) {
        if (nya_rect_contains(_nya_ui.claims[i], point)) return true;
    }

    return false;
}

void nya_ui_size(NYA_UI* ui, NYA_UISize size) {
    nya_assert(ui != nullptr && ui == _nya_ui.open);
    nya_assert(size.kind < NYA_UI_SIZE_COUNT && size.value >= 0.0F && size.min >= 0.0F && size.max >= 0.0F);

    _nya_ui.next     = size;
    _nya_ui.next_set = true;
}

NYA_Rectf nya_ui_space(NYA_UI* ui, f32 width, f32 height) {
    nya_assert(ui != nullptr && ui == _nya_ui.open);
    nya_assert(width >= 0.0F && height >= 0.0F);

    const _NYA_UILayout* layout  = &_nya_ui.layouts[_nya_ui.depth - 1];
    f32x2                natural = { _nya_ui_px(width), _nya_ui_px(height) };

    return _nya_ui_place((NYA_UISize){ 0 }, natural, natural[1 - layout->main] == 0.0F);
}


/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * TABLES
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

b8 nya_ui_table_begin(NYA_UI* ui, NYA_ConstCString id, NYA_UITable table) {
    nya_assert(ui != nullptr && ui == _nya_ui.open);
    nya_assert(id != nullptr, "a table is named, so its rows keep their ids when the list is reordered");
    nya_assert(table.widths != nullptr && table.columns > 0 && table.columns <= NYA_UI_TABLE_COLUMNS_MAX,
               "a table has 1 to NYA_UI_TABLE_COLUMNS_MAX columns and a width for each, got %u", table.columns);

    if (!nya_ui_panel_begin(ui, id, (NYA_UIPanel){ .frameless = true })) return false;

    _NYA_UILayout* layout = &_nya_ui.layouts[_nya_ui.depth - 1];

    // the row layouts read these off their parent, so one table declaration sizes every cell under it.
    layout->columns      = table.widths;
    layout->column_count = table.columns;
    layout->striped      = table.striped;

    if (table.headers == nullptr) return true;

    const NYA_UILook* look = _nya_ui_look();

    if (nya_ui_table_row_begin(ui)) {
        for (u32 i = 0; i < table.columns; i++) nya_ui_label(ui, table.headers[i], look->style.text_dim);
        nya_ui_table_row_end(ui);
    }

    // a rule under the headers, which is what makes the first row read as a header rather than as data.
    NYA_Rectf rule = nya_ui_space(ui, 0.0F, 1.0F);

    if (_nya_ui_drawn(rule)) {
        NYA_UIWidgetDraw draw = { .kind = NYA_UI_WIDGET_RULE, .rect = rule, .color = look->style.text_dim };

        _nya_ui_draw(ui, &draw);
    }

    return true;
}

void nya_ui_table_end(NYA_UI* ui) {
    nya_assert(ui != nullptr && ui == _nya_ui.open);
    nya_assert(_nya_ui.layouts[_nya_ui.depth - 1].columns != nullptr, "nya_ui_table_end without a table_begin");

    nya_ui_panel_end(ui);
}

b8 nya_ui_table_row_begin(NYA_UI* ui) {
    nya_assert(ui != nullptr && ui == _nya_ui.open);

    const _NYA_UILayout* table = &_nya_ui.layouts[_nya_ui.depth - 1];
    nya_assert(table->columns != nullptr, "a table row only opens inside a table");

    const f32* widths  = table->columns;
    u32        count   = table->column_count;
    b8         striped = table->striped && (table->count % 2) == 1;

    if (!nya_ui_panel_begin(ui, nullptr, (NYA_UIPanel){ .direction = NYA_UI_DIRECTION_ROW, .align = NYA_UI_ALIGN_CENTER, .frameless = true })) return false;

    _NYA_UILayout* row = &_nya_ui.layouts[_nya_ui.depth - 1];
    row->columns       = widths;
    row->column_count  = count;

    // before the cells, since an immediate pass draws in call order and a stripe belongs under them.
    if (striped && _nya_ui_drawn(row->bounds)) {
        NYA_Color ink = _nya_ui_look()->style.text_dim;

        NYA_UIWidgetDraw draw = {
            .kind  = NYA_UI_WIDGET_STRIPE,
            .rect  = row->bounds,
            .color = { ink.r, ink.g, ink.b, ink.a * NYA_UI_STRIPE_ALPHA },
        };

        _nya_ui_draw(ui, &draw);
    }

    return true;
}

void nya_ui_table_row_end(NYA_UI* ui) {
    nya_assert(ui != nullptr && ui == _nya_ui.open);
    nya_assert(_nya_ui.layouts[_nya_ui.depth - 1].main == 0, "nya_ui_table_row_end without a row_begin");

    nya_ui_panel_end(ui);
}


/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * INTERNAL
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

u64 _nya_ui_id(u64 scope, NYA_ConstCString label) {
    u64 parts[2] = { scope, nya_hash_fnv1a(label) };
    u64 id       = nya_hash_fnv1a(parts, sizeof(parts));

    return id != 0 ? id : 1;
}

u64 _nya_ui_id_at(u64 key, u32 index) {
    u64 parts[2] = { key, (u64)index + 1 };
    u64 id       = nya_hash_fnv1a(parts, sizeof(parts));

    return id != 0 ? id : 1;
}

u32 _nya_ui_panel_claim(u64 id) {
    u32 free  = U32_MAX;
    u32 stale = U32_MAX;

    for (u32 i = 0; i < NYA_UI_PANELS_MAX; i++) {
        _NYA_UIPanelState* state = &_nya_ui.panels[i];

        if (state->id == id) return i;
        if (state->id == 0 && free == U32_MAX) free = i;
        if (state->id != 0 && state->pass != _nya_ui.pass_serial && (stale == U32_MAX || state->pass < _nya_ui.panels[stale].pass)) stale = i;
    }

    if (free != U32_MAX) {
        _nya_ui.panel_count += 1;
    } else if (stale != U32_MAX) {
        free = stale;
    } else {
        return U32_MAX;
    }

    _nya_ui.panels[free] = (_NYA_UIPanelState){ .id = id };
    return free;
}

b8 _nya_ui_panel_standing(const NYA_UI* ui, const _NYA_UIPanelState* state) {
    nya_assert(ui != nullptr && state != nullptr);

    if (state->id == 0 || !state->top_level) return false;

    // pass serials are handed out one per pass over one window, so either of this window's last two identifies it.
    return state->pass == ui->pass_current || state->pass == ui->pass_previous;
}

b8 _nya_ui_panel_over(const _NYA_UIPanelState* panel, const _NYA_UIPanelState* under) {
    nya_assert(panel != nullptr && under != nullptr);
    nya_assert(panel->order != under->order || panel == under, "two panels share a raise serial");

    if (panel->z != under->z) return panel->z > under->z;

    return panel->order > under->order;
}

b8 _nya_ui_panel_covered(const NYA_UI* ui, u32 index) {
    nya_assert(ui != nullptr && index < NYA_UI_PANELS_MAX);

    const _NYA_UIPanelState* own = &_nya_ui.panels[index];

    for (u32 i = 0; i < NYA_UI_PANELS_MAX; i++) {
        const _NYA_UIPanelState* other = &_nya_ui.panels[i];

        if (i == index || !_nya_ui_panel_standing(ui, other)) continue;
        if (!_nya_ui_panel_over(other, own)) continue;

        // where it was last laid out: the one that will cover this pass has not been declared yet.
        if (nya_rect_contains(other->bounds, _nya_ui.pointer)) return true;
    }

    return false;
}

b8 _nya_ui_panel_raise(const NYA_UI* ui, u32 index) {
    nya_assert(ui != nullptr && index < NYA_UI_PANELS_MAX);
    nya_assert(_nya_ui.panels[index].order != 0, "a top level panel takes its order when it is first declared");

    _NYA_UIPanelState* own = &_nya_ui.panels[index];

    b8 moved      = false;
    b8 overlapped = false;

    for (u32 i = 0; i < NYA_UI_PANELS_MAX; i++) {
        const _NYA_UIPanelState* other = &_nya_ui.panels[i];

        if (i == index || !_nya_ui_panel_standing(ui, other)) continue;

        // its own z band only: raising never lifts a panel over one the caller deliberately put above it.
        if (other->z != own->z || other->order <= own->order) continue;

        // whether being under it was visible at all: a panel nothing overlaps looks the same in front as
        // behind, so bringing it forward is not something a person can have meant by clicking it.
        overlapped |= nya_rect_overlaps(other->bounds, own->bounds);
        moved       = true;
    }

    if (moved) {
        _nya_ui.raise_serial += 1;
        own->order            = _nya_ui.raise_serial;
    }

    return moved && overlapped;
}

u32 _nya_ui_panel_rank(const NYA_UI* ui, u32 index) {
    nya_assert(ui != nullptr && index < NYA_UI_PANELS_MAX);

    const _NYA_UIPanelState* own  = &_nya_ui.panels[index];
    u32                      rank = 0;

    for (u32 i = 0; i < NYA_UI_PANELS_MAX; i++) {
        const _NYA_UIPanelState* other = &_nya_ui.panels[i];

        if (i == index || !_nya_ui_panel_standing(ui, other)) continue;
        if (_nya_ui_panel_over(own, other)) rank += 1;
    }

    nya_assert(rank < NYA_UI_PANELS_MAX);

    return rank;
}

_NYA_UILayout* _nya_ui_layout_push(void) {
    nya_assert(_nya_ui.depth < NYA_UI_DEPTH_MAX, "containers nest deeper than NYA_UI_DEPTH_MAX");

    return &_nya_ui.layouts[_nya_ui.depth++];
}

f32 _nya_ui_extent(NYA_UISize size, f32 measured, f32 available) {
    f32 extent = measured;

    if (size.kind == NYA_UI_SIZE_FIXED) extent = _nya_ui_px(size.value);
    if (size.kind == NYA_UI_SIZE_GROW) extent = roundf(available * size.value);

    extent = nya_max(extent, _nya_ui_px(size.min));
    if (size.max > 0.0F) extent = nya_min(extent, _nya_ui_px(size.max));

    return nya_min(extent, available);
}

NYA_UISize _nya_ui_next_size(const _NYA_UILayout* layout, NYA_UISize own) {
    if (_nya_ui.next_set) return _nya_ui.next;
    if (own.kind != NYA_UI_SIZE_AUTO) return own;

    /*
     * A table row's cells are sized by their column, which is what lines one row up with the next. Inside a row
     * only: the table itself keeps the widths so every row it opens can read them, and its own children are the
     * rows, not cells. Without the direction test the first row took column 0 as its height and the rule under the
     * headers took column 1, which drew a 70 pixel bar across the top of gnyame's counters table.
     */
    if (layout->columns != nullptr && layout->main == 0 && layout->count < layout->column_count) {
        f32 width = layout->columns[layout->count];

        return width > 0.0F ? nya_ui_fixed(width) : nya_ui_grow(1);
    }

    return layout->children;
}

f32 _nya_ui_share(const _NYA_UILayout* layout, f32 placed, f32 weight) {
    if (layout->grow_total <= 0.0F) return -1.0F;

    // cumulative, so rounding each share cannot leave the row a pixel short or long.
    f32 start = roundf(layout->grow_space * nya_min(placed / layout->grow_total, 1.0F));
    f32 end   = roundf(layout->grow_space * nya_min((placed + weight) / layout->grow_total, 1.0F));

    return end - start;
}

f32 _nya_ui_text_room(const _NYA_UILayout* layout) {
    // the room, not the width: a column that fits its content would otherwise wrap to what it measured last pass.
    if (layout->main == 1) return layout->room.x;

    NYA_UISize size = _nya_ui_next_size(layout, (NYA_UISize){ 0 });

    if (size.kind == NYA_UI_SIZE_FIXED) return _nya_ui_px(size.value);

    if (size.kind == NYA_UI_SIZE_GROW) {
        f32 share = _nya_ui_share(layout, layout->grow_placed, size.value);
        if (share >= 0.0F) return share;
    }

    // what is left of the row.
    f32 gap = layout->count > 0 ? layout->gap : 0.0F;
    return nya_max(layout->room.x - layout->used - gap, 0.0F);
}

NYA_Rectf _nya_ui_place(NYA_UISize own, f32x2 natural, b8 fill) {
    _NYA_UILayout* layout = &_nya_ui.layouts[_nya_ui.depth - 1];

    u32        main  = layout->main;
    u32        cross = 1 - main;
    NYA_UISize size  = _nya_ui_next_size(layout, own);

    _nya_ui.next_set = false;

    f32 along = natural[main];

    if (size.kind == NYA_UI_SIZE_FIXED) along = _nya_ui_px(size.value);

    if (size.kind == NYA_UI_SIZE_GROW) {
        // a container seen for the first time has no weights yet, and fits until it does.
        f32 share = _nya_ui_share(layout, layout->grow_placed, size.value);
        if (share >= 0.0F) along = share;

        layout->grow_placed += size.value;
        layout->grow        += size.value;
    }

    along = nya_max(along, _nya_ui_px(size.min));
    if (size.max > 0.0F) along = nya_min(along, _nya_ui_px(size.max));
    along = roundf(along);

    if (size.kind != NYA_UI_SIZE_GROW) layout->fixed += along;

    f32 extent = layout->extent[cross];
    f32 across = fill && extent > 0.0F ? extent : roundf(natural[cross]);
    f32 gap    = layout->count > 0 ? layout->gap : 0.0F;

    f32x2 position  = { 0.0F, 0.0F };
    position[main]  = layout->origin[main] + layout->used + gap;
    position[cross] = layout->origin[cross] + (extent > 0.0F && !fill ? roundf((extent - across) * (f32)layout->align * 0.5F) : 0.0F);

    f32x2 taken  = { 0.0F, 0.0F };
    taken[main]  = along;
    taken[cross] = across;

    layout->used   += gap + along;
    layout->across  = nya_max(layout->across, roundf(natural[cross]));
    layout->count  += 1;

    return (NYA_Rectf){ position.x, position.y, taken.x, taken.y };
}

void _nya_ui_reveal(NYA_Rectf rect) {
    // not `near` and `far`: windows.h still defines both as empty macros, and the Windows build fails
    // here with "expected identifier" rather than with anything that names the collision.
    f32x2 top_left     = { rect.x, rect.y };
    f32x2 bottom_right = { rect.x + rect.width, rect.y + rect.height + _nya_ui_look()->depth };

    for (u32 depth = _nya_ui.depth; depth > 1; depth--) {
        const _NYA_UILayout* layout = &_nya_ui.layouts[depth - 1];
        if (!layout->scrolls[0] && !layout->scrolls[1]) continue;

        _NYA_UIPanelState* state = &_nya_ui.panels[layout->panel];

        for (u32 axis = 0; axis < 2; axis++) {
            if (!layout->scrolls[axis]) continue;

            // the view in content space, with room for the shadow under it; panel_end clamps what this overshoots.
            f32 start = layout->origin[axis] + layout->scroll[axis];
            f32 end   = start + layout->extent[axis];

            if (top_left[axis] < start) state->scroll[axis] -= start - top_left[axis];
            if (bottom_right[axis] > end && top_left[axis] >= start) state->scroll[axis] += bottom_right[axis] - end;
        }

        return;
    }
}

void _nya_ui_panel_drag(NYA_UI* ui, u64 key, _NYA_UIPanelState* state, NYA_Rectf grip, b8 covered) {
    nya_assert(ui != nullptr && state != nullptr && key != 0);

    if (ui->pass != NYA_UI_PASS_INPUT) return;

    // `grip` is the title strip as it is drawn, or the top edge when there is no title. Dragging by the body
    // would swallow every click in it.
    if (_nya_ui.pointer_pressed && !covered && !_nya_ui_claimed(_nya_ui.pointer) && ui->drag_panel == 0 && nya_rect_contains(grip, _nya_ui.pointer)) {
        ui->drag_panel       = key;
        ui->drag_grip        = (f32x2){ _nya_ui.pointer.x - state->drag.x, _nya_ui.pointer.y - state->drag.y };
        _nya_ui.drag_started = true;
    }

    if (ui->drag_panel != key) return;

    if (!_nya_ui.pointer_down) {
        ui->drag_panel = 0;
        return;
    }

    state->drag = (f32x2){ _nya_ui.pointer.x - ui->drag_grip.x, _nya_ui.pointer.y - ui->drag_grip.y };
}
