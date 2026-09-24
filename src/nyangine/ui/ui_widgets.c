/**
 * @file ui_widgets.c
 *
 * The widgets a menu is made of: text, buttons and the ones that carry a value. See ui.h.
 * */
#include "nyangine/nyangine.h"

#include "nyangine/ui/ui_internal.h"


/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * WIDGETS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

void nya_ui_label(NYA_UI* ui, NYA_ConstCString text) __attr_overloaded {
    nya_assert(ui != nullptr);

    nya_ui_label(ui, text, _nya_ui_look()->style.text.normal);
}

void nya_ui_label(NYA_UI* ui, NYA_ConstCString text, NYA_Color color) __attr_overloaded {
    nya_assert(ui != nullptr && ui == _nya_ui.open);
    nya_assert(text != nullptr);

    const _NYA_UILayout* layout = &_nya_ui.layouts[_nya_ui.depth - 1];
    const NYA_UILook*    look   = _nya_ui_look();

    if (_nya_ui.disabled > 0) color = look->style.text.disabled;

    // the presenter folds or shrinks it, so what it measures here is exactly what it draws below.
    f32   room = _nya_ui_text_room(layout);
    f32x2 size = _nya_ui_measure(layout->text, text, room, layout->overflow);

    NYA_Rectf rect = _nya_ui_place((NYA_UISize){ 0 }, (f32x2){ ceilf(size.x), ceilf(size.y) }, false);
    if (!_nya_ui_drawn(rect)) return;

    NYA_UIWidgetDraw widget = {
        .kind     = NYA_UI_WIDGET_LABEL,
        .rect     = rect,
        .label    = text,
        .color    = color,
        .as_label = { .room = room, .overflow = layout->overflow, .align = layout->align },
    };

    _nya_ui_draw(ui, &widget);
}

b8 nya_ui_button(NYA_UI* ui, NYA_ConstCString label) {
    nya_assert(ui != nullptr && ui == _nya_ui.open);
    nya_assert(label != nullptr);

    const _NYA_UILayout* layout = &_nya_ui.layouts[_nya_ui.depth - 1];
    const NYA_UILook*    look   = _nya_ui_look();

    f32       text_width = _nya_ui_text_width(layout->text, label);
    NYA_Rectf rect       = _nya_ui_place((NYA_UISize){ 0 }, (f32x2){ ceilf(text_width) + (look->padding * 2.0F), _nya_ui_item_height(layout) }, true);

    _NYA_UIWidget widget = _nya_ui_widget(ui, label, rect, false);
    if (widget.refused) return false;

    if (_nya_ui_drawn(rect)) {
        NYA_UIWidgetDraw draw = { .kind = NYA_UI_WIDGET_BUTTON, .rect = rect, .state = _nya_ui_state(ui, widget), .label = label };

        _nya_ui_draw(ui, &draw);
    }

    return widget.activated;
}

b8 nya_ui_selectable(NYA_UI* ui, NYA_ConstCString label, b8 selected) {
    nya_assert(ui != nullptr && ui == _nya_ui.open);
    nya_assert(label != nullptr);

    const _NYA_UILayout* layout = &_nya_ui.layouts[_nya_ui.depth - 1];
    const NYA_UILook*    look   = _nya_ui_look();

    f32 height = _nya_ui_item_height(layout);
    f32 dot    = roundf(height * 0.14F);

    f32       text_width = _nya_ui_text_width(layout->text, label);
    NYA_Rectf rect       = _nya_ui_place((NYA_UISize){ 0 }, (f32x2){ ceilf(text_width) + (look->padding * 3.0F) + (dot * 2.0F), height }, true);

    _NYA_UIWidget widget = _nya_ui_widget(ui, label, rect, false);
    if (widget.refused) return false;

    if (_nya_ui_drawn(rect)) {
        NYA_UIWidgetDraw draw = {
            .kind      = NYA_UI_WIDGET_SELECTABLE,
            .rect      = rect,
            .state     = _nya_ui_state(ui, widget),
            .label     = label,
            .as_choice = { .on = selected },
        };

        _nya_ui_draw(ui, &draw);
    }

    return widget.activated;
}

b8 nya_ui_toggle(NYA_UI* ui, NYA_ConstCString label, b8* value) {
    nya_assert(ui != nullptr && ui == _nya_ui.open);
    nya_assert(label != nullptr && value != nullptr);

    const _NYA_UILayout* layout = &_nya_ui.layouts[_nya_ui.depth - 1];
    const NYA_UILook*    look   = _nya_ui_look();

    f32 height      = _nya_ui_item_height(layout);
    f32 pill_height = roundf(height * 0.5F);
    f32 pill_width  = roundf(pill_height * 1.8F);

    f32       text_width = _nya_ui_text_width(layout->text, label);
    NYA_Rectf rect       = _nya_ui_place((NYA_UISize){ 0 }, (f32x2){ ceilf(text_width) + (look->padding * 3.0F) + pill_width, height }, true);

    _NYA_UIWidget widget = _nya_ui_widget(ui, label, rect, true);
    if (widget.refused) return false;

    b8 before = *value;

    if (widget.activated) *value = !*value;
    if (widget.focused && _nya_ui.presses[_NYA_UI_LEFT]) *value = false;
    if (widget.focused && _nya_ui.presses[_NYA_UI_RIGHT]) *value = true;

    if (_nya_ui_drawn(rect)) {
        NYA_UIWidgetDraw draw = {
            .kind      = NYA_UI_WIDGET_TOGGLE,
            .rect      = rect,
            .state     = _nya_ui_state(ui, widget),
            .label     = label,
            .as_choice = { .on = *value },
        };

        _nya_ui_draw(ui, &draw);
    }

    return *value != before;
}

b8 nya_ui_slider(NYA_UI* ui, NYA_ConstCString label, f32* value, f32 min, f32 max, f32 step) {
    nya_assert(ui != nullptr && ui == _nya_ui.open);
    nya_assert(label != nullptr && value != nullptr);
    nya_assert(max > min && step >= 0.0F, "a slider needs min below max and a step of zero or more");

    const _NYA_UILayout* layout = &_nya_ui.layouts[_nya_ui.depth - 1];
    const NYA_UILook*    look   = _nya_ui_look();

    f32 height = _nya_ui_item_height(layout);
    f32 radius = roundf(height * 0.2F);

    f32       text_width = _nya_ui_text_width(layout->text, label);
    NYA_Rectf rect       = _nya_ui_place((NYA_UISize){ 0 }, (f32x2){ ceilf(text_width) + (look->padding * 3.0F) + (height * 2.0F), height }, true);

    // the right part of the row, past the label, inset by the knob so it can reach both ends.
    f32 track_x     = roundf(rect.x + nya_max(rect.width * 0.5F, text_width + (look->padding * 2.0F)) + radius);
    f32 track_width = nya_max((rect.x + rect.width - look->padding - radius) - track_x, 1.0F);

    _NYA_UIWidget widget = _nya_ui_widget(ui, label, rect, true);
    if (widget.refused) return false;

    f32 before = *value;

    if (ui->pass == NYA_UI_PASS_INPUT && !widget.disabled) {
        if (widget.focused && _nya_ui.presses[_NYA_UI_LEFT]) *value -= step;
        if (widget.focused && _nya_ui.presses[_NYA_UI_RIGHT]) *value += step;

        // a press past the label grabs the knob; one on the label only focuses the row.
        if (_nya_ui.pointer_pressed && ui->active == widget.id && _nya_ui.pointer.x >= track_x - (radius * 2.0F)) ui->dragging = true;

        if (ui->active == widget.id && ui->dragging && _nya_ui.pointer_down) {
            f32 t  = nya_clamp((_nya_ui.pointer.x - track_x) / track_width, 0.0F, 1.0F);
            *value = min + (t * (max - min));

            if (step > 0.0F) *value = min + (roundf((*value - min) / step) * step);
        }

        *value = nya_clamp(*value, min, max);
    }

    if (_nya_ui_drawn(rect)) {
        NYA_UIWidgetDraw draw = {
            .kind      = NYA_UI_WIDGET_SLIDER,
            .rect      = rect,
            .state     = _nya_ui_state(ui, widget),
            .label     = label,
            .as_slider = {
                .track = { track_x, rect.y, track_width, rect.height },
                .t     = nya_clamp((*value - min) / (max - min), 0.0F, 1.0F),
            },
        };

        _nya_ui_draw(ui, &draw);
    }

    return *value != before;
}

b8 nya_ui_color_picker(NYA_UI* ui, NYA_ConstCString label, NYA_Color* color) {
    nya_assert(ui != nullptr && ui == _nya_ui.open);
    nya_assert(label != nullptr && color != nullptr);

    const _NYA_UILayout* layout = &_nya_ui.layouts[_nya_ui.depth - 1];
    const NYA_UILook*    look   = _nya_ui_look();

    f32 height  = _nya_ui_item_height(layout);
    f32 gap     = look->spacing;
    f32 padding = look->padding;
    f32 field   = _nya_ui_px(NYA_UI_PICKER_FIELD);
    f32 bar     = _nya_ui_px(NYA_UI_PICKER_BAR);

    f32       text_width = _nya_ui_text_width(layout->text, label);
    NYA_Rectf rect       = _nya_ui_place((NYA_UISize){ 0 }, (f32x2){ ceilf(text_width) + (padding * 3.0F) + (height * 4.0F), height + field + bar + (gap * 2.0F) + padding }, true);

    // the top row is the label, then a swatch and the hex field; under it the field and hue bar, then the alpha bar.
    f32       inset  = roundf(padding * 0.5F);
    f32       row_x  = roundf(rect.x + nya_max(rect.width * 0.4F, text_width + (padding * 2.0F)));
    NYA_Rectf swatch = { row_x, rect.y + inset, height - (inset * 2.0F), height - (inset * 2.0F) };
    NYA_Rectf hex    = { swatch.x + swatch.width + gap, swatch.y, nya_max(rect.x + rect.width - padding - swatch.x - swatch.width - gap, 1.0F), swatch.height };
    NYA_Rectf plane  = { rect.x + padding, rect.y + height + gap, nya_max(rect.width - (padding * 2.0F) - gap - bar, 1.0F), field };
    NYA_Rectf hue    = { plane.x + plane.width + gap, plane.y, bar, field };
    NYA_Rectf alpha  = { plane.x, plane.y + field + gap, rect.width - (padding * 2.0F), bar };

    _NYA_UIWidget widget = _nya_ui_widget(ui, label, rect, true);
    if (widget.refused) return false;

    NYA_ColorHSV hsv    = nya_color_to_hsv(*color);
    NYA_Color    before = *color;

    // a grey has no hue of its own, so the one it was dragged from is kept while this picker holds focus.
    if (ui->hue_id == widget.id && (hsv.s <= 0.0F || hsv.v <= 0.0F)) hsv.h = ui->hue;

    if (ui->pass == NYA_UI_PASS_INPUT && !widget.disabled) {
        b8    moved = false;
        f32x2 at    = _nya_ui.pointer;

        if (widget.focused && (_nya_ui.presses[_NYA_UI_LEFT] || _nya_ui.presses[_NYA_UI_RIGHT])) {
            hsv.h = fmodf(hsv.h + (_nya_ui.presses[_NYA_UI_RIGHT] ? 10.0F : 350.0F), 360.0F);
            moved = true;
        }

        // the part a press lands on keeps the pointer until release, however far it is dragged.
        if (_nya_ui.pointer_pressed && ui->active == widget.id) {
            if (nya_rect_contains(plane, at)) ui->grab = 1;
            if (nya_rect_contains(hue, at)) ui->grab = 2;
            if (nya_rect_contains(alpha, at)) ui->grab = 3;
        }

        if (ui->active == widget.id && _nya_ui.pointer_down && ui->grab != 0) {
            if (ui->grab == 1) hsv.s = nya_clamp((at.x - plane.x) / plane.width, 0.0F, 1.0F);
            if (ui->grab == 1) hsv.v = 1.0F - nya_clamp((at.y - plane.y) / plane.height, 0.0F, 1.0F);
            if (ui->grab == 2) hsv.h = nya_clamp((at.y - hue.y) / hue.height, 0.0F, 0.999F) * 360.0F;
            if (ui->grab == 3) hsv.a = nya_clamp((at.x - alpha.x) / alpha.width, 0.0F, 1.0F);
            moved = true;
        }

        // converted only when something moved, so an untouched colour does not drift through the round trip.
        if (moved) *color = nya_color_from_hsv(hsv);
    }

    if (widget.focused) {
        ui->hue_id = widget.id;
        ui->hue    = hsv.h;
    }

    // typed into a copy that outlives the pass, since the colour only changes once the digits are whole.
    char shown[sizeof(ui->hex)];
    u32  packed = nya_color_to_u32(*color);

    if ((packed & 0xFFU) == 0xFFU) (void)snprintf(shown, sizeof(shown), "#%06X", packed >> 8);
    if ((packed & 0xFFU) != 0xFFU) (void)snprintf(shown, sizeof(shown), "#%08X", packed);

    b8 start = ui->pass == NYA_UI_PASS_INPUT && ui->editing != widget.id &&
               ((widget.focused && _nya_ui.confirm) || (ui->active == widget.id && _nya_ui.pointer_released && nya_rect_contains(hex, _nya_ui.pointer)));

    if (start) nya_memcpy(ui->hex, shown, sizeof(shown));

    char* text = ui->editing == widget.id || start ? ui->hex : shown;

    // the hex field reads the pointer against the rectangle the layout put it at; where the body sinks to is the
    // presenter's business, and it moves the box with it.
    NYA_UIFieldDraw box   = { 0 };
    b8              typed = _nya_ui_field(ui, widget, start, hex, hex, text, sizeof(ui->hex), &box);

    // six or eight digits make a colour; anything shorter waits for the rest.
    NYA_ConstCString digits = text[0] == '#' ? text + 1 : text;
    u64              count  = strlen(digits);
    b8               valid  = count == 6 || count == 8;

    for (u64 i = 0; valid && i < count; i++) valid = isxdigit((unsigned char)digits[i]) != 0;

    if (typed && valid) *color = nya_color_from_hex(digits);

    if (_nya_ui_drawn(rect)) {
        NYA_UIWidgetDraw draw = {
            .kind      = NYA_UI_WIDGET_COLOR_PICKER,
            .rect      = rect,
            .state     = _nya_ui_state(ui, widget),
            .label     = label,
            .as_picker = { .plane = plane, .hue = hue, .alpha = alpha, .swatch = swatch, .hsv = hsv, .value = *color, .field = box },
        };

        _nya_ui_draw(ui, &draw);
    }

    return color->r != before.r || color->g != before.g || color->b != before.b || color->a != before.a;
}

b8 nya_ui_radio(NYA_UI* ui, NYA_ConstCString label, u32* selected, u32 value) {
    nya_assert(ui != nullptr && ui == _nya_ui.open);
    nya_assert(label != nullptr && selected != nullptr);

    const _NYA_UILayout* layout = &_nya_ui.layouts[_nya_ui.depth - 1];
    const NYA_UILook*    look   = _nya_ui_look();

    f32 height = _nya_ui_item_height(layout);
    f32 ring   = roundf(height * 0.22F);

    f32       text_width = _nya_ui_text_width(layout->text, label);
    NYA_Rectf rect       = _nya_ui_place((NYA_UISize){ 0 }, (f32x2){ ceilf(text_width) + (look->padding * 3.0F) + (ring * 2.0F), height }, true);

    _NYA_UIWidget widget = _nya_ui_widget(ui, label, rect, false);
    if (widget.refused) return false;

    b8 chosen  = *selected == value;
    b8 changed = widget.activated && !chosen;

    if (changed) *selected = value;

    if (_nya_ui_drawn(rect)) {
        NYA_UIWidgetDraw draw = {
            .kind      = NYA_UI_WIDGET_RADIO,
            .rect      = rect,
            .state     = _nya_ui_state(ui, widget),
            .label     = label,
            .as_choice = { .on = *selected == value },
        };

        _nya_ui_draw(ui, &draw);
    }

    return changed;
}

b8 nya_ui_tabs(NYA_UI* ui, NYA_ConstCString id, const NYA_ConstCString* labels, u32 count, u32* selected) {
    nya_assert(ui != nullptr && ui == _nya_ui.open);
    nya_assert(id != nullptr && labels != nullptr && selected != nullptr);
    nya_assert(count > 0, "a tab strip needs at least one page");

    return _nya_ui_choice_row(ui, id, labels, count, selected, true);
}

b8 nya_ui_dropdown(NYA_UI* ui, NYA_ConstCString label, const NYA_ConstCString* options, u32 count, u32* selected) {
    nya_assert(ui != nullptr && ui == _nya_ui.open);
    nya_assert(label != nullptr && options != nullptr && selected != nullptr);
    nya_assert(count > 0, "a dropdown needs at least one option");

    const _NYA_UILayout* layout = &_nya_ui.layouts[_nya_ui.depth - 1];
    const NYA_UILook*    look   = _nya_ui_look();

    u32              chosen = nya_min(*selected, count - 1);
    NYA_ConstCString shown  = options[chosen];

    f32 height = _nya_ui_item_height(layout);
    f32 arrow  = roundf(height * 0.18F);

    f32       label_width = _nya_ui_text_width(layout->text, label);
    f32       value_width = _nya_ui_text_width(layout->text, shown);
    NYA_Rectf rect        = _nya_ui_place((NYA_UISize){ 0 }, (f32x2){ ceilf(label_width + value_width) + (look->padding * 4.0F) + (arrow * 2.0F), height }, true);

    _NYA_UIWidget widget = _nya_ui_widget(ui, label, rect, false);
    if (widget.refused) return false;

    b8 open = ui->open == widget.id;

    // activating the closed row opens the list, and activating it again closes it without changing anything.
    if (widget.activated) ui->open = open ? 0 : widget.id;

    if (_nya_ui_drawn(rect)) {
        NYA_UIWidgetDraw draw = {
            .kind        = NYA_UI_WIDGET_DROPDOWN,
            .rect        = rect,
            .state       = _nya_ui_state(ui, widget),
            .label       = label,
            .as_dropdown = { .shown = shown, .open = open },
        };

        _nya_ui_draw(ui, &draw);
    }

    if (!open || widget.disabled) return false;

    // the list hangs under the row and over whatever follows it; see ui.h for what that costs and what it does not.
    u32   picked  = chosen;
    f32x2 at      = { rect.x, rect.y + rect.height };
    b8    changed = _nya_ui_choice_list(ui, label, options, count, &picked, at, rect.width);

    // a press anywhere but the row and the list closes it, which is what every other menu does. The list claimed its
    // own rectangle as it closed, so this reads that rather than guessing where it went.
    if (_nya_ui.pointer_pressed && !nya_rect_contains(rect, _nya_ui.pointer) && !_nya_ui_claimed(_nya_ui.pointer)) ui->open = 0;

    if (!changed) return false;

    *selected = picked;
    ui->open  = 0;

    return true;
}

void nya_ui_chart(NYA_UI* ui, NYA_ConstCString label, NYA_UIChart chart) {
    nya_assert(ui != nullptr && ui == _nya_ui.open);
    nya_assert(label != nullptr);
    nya_assert(chart.kind < NYA_UI_CHART_KIND_COUNT);
    nya_assert(chart.values != nullptr || chart.count == 0, "a chart with points needs values to read them from");

    const _NYA_UILayout* layout = &_nya_ui.layouts[_nya_ui.depth - 1];
    const NYA_UILook*    look   = _nya_ui_look();
    const NYA_UIStyle*   style  = &look->style;

    f32 line   = look->line_heights[layout->text];
    f32 height = chart.height > 0.0F ? _nya_ui_px(chart.height) : roundf(line * 4.0F);

    NYA_Rectf rect = _nya_ui_place((NYA_UISize){ 0 }, (f32x2){ _nya_ui_px(NYA_UI_CHART_WIDTH), height }, true);
    if (!_nya_ui_drawn(rect)) return;

    NYA_Color color = chart.color;
    if (color.r == 0.0F && color.g == 0.0F && color.b == 0.0F && color.a == 0.0F) color = style->accent;

    // the values are the caller's and are read during the call, so the plot is the presenter's to lay out.
    NYA_UIWidgetDraw draw = {
        .kind     = NYA_UI_WIDGET_CHART,
        .rect     = rect,
        .label    = label,
        .color    = color,
        .as_chart = { .chart = &chart },
    };

    _nya_ui_draw(ui, &draw);
}

void nya_ui_icon(NYA_UI* ui, NYA_UIIcon icon, f32 size) {
    nya_assert(ui != nullptr && ui == _nya_ui.open);
    nya_assert(size >= 0.0F && icon.source_width >= 0.0F && icon.source_height >= 0.0F);

    const _NYA_UILayout* layout = &_nya_ui.layouts[_nya_ui.depth - 1];
    const NYA_UILook*    look   = _nya_ui_look();

    f32       side = size > 0.0F ? _nya_ui_px(size) : look->line_heights[layout->text];
    NYA_Rectf rect = _nya_ui_place((NYA_UISize){ 0 }, (f32x2){ side, side }, false);

    if (!_nya_ui_drawn(rect)) return;

    NYA_UIWidgetDraw draw = {
        .kind    = NYA_UI_WIDGET_ICON,
        .rect    = rect,
        .state   = { .disabled = _nya_ui.disabled > 0 },
        .as_icon = { .icon = &icon },
    };

    _nya_ui_draw(ui, &draw);
}

void nya_ui_badge(NYA_UI* ui, NYA_ConstCString label) {
    nya_assert(ui != nullptr && ui == _nya_ui.open);
    nya_assert(label != nullptr);

    const NYA_UILook* look = _nya_ui_look();

    /*
     * A chip that fits its own text rather than filling the row, so a handful sit in a line. It is a
     * framed panel — the frame carries the theme's panel fill and rounding on every backend — sized to
     * the small text inside it and padded tight, which is all a tag is. Nothing here names a colour: the
     * panel and the label each read the style, so a badge follows the theme like everything else.
     */
    if (nya_ui_panel_begin(ui, nullptr, (NYA_UIPanel){
            .width   = nya_ui_fit(),
            .text    = NYA_UI_TEXT_SMALL,
            .align   = NYA_UI_ALIGN_CENTER,
            .padding = roundf(look->style.padding * 0.5F),
        })) {
        nya_ui_label(ui, label);
        nya_ui_panel_end(ui);
    }
}

void nya_ui_progress(NYA_UI* ui, f32 fraction) {
    nya_assert(ui != nullptr && ui == _nya_ui.open);

    const NYA_UILook* look = _nya_ui_look();

    // a slim bar of its own, filling the row across; nothing focuses it, so the input pass leaves early like a label.
    NYA_Rectf rect = nya_ui_space(ui, 0.0F, NYA_UI_PROGRESS_HEIGHT);
    if (!_nya_ui_drawn(rect)) return;

    f32 t = nya_clamp(fraction, 0.0F, 1.0F);

    // the empty track first and the filled part over it, the two fill kinds every presenter already draws: the track
    // in the style's track colour, the fill in its accent, so the bar is the theme's and not this widget's.
    NYA_UIWidgetDraw track = { .kind = NYA_UI_WIDGET_STRIPE, .rect = rect, .color = look->style.track };
    _nya_ui_draw(ui, &track);

    if (t > 0.0F) {
        NYA_UIWidgetDraw fill = {
            .kind  = NYA_UI_WIDGET_UNDERLINE,
            .rect  = { rect.x, rect.y, roundf(rect.width * t), rect.height },
            .color = look->style.accent,
        };

        _nya_ui_draw(ui, &fill);
    }
}

b8 nya_ui_breadcrumb(NYA_UI* ui, NYA_ConstCString id, const NYA_ConstCString* items, u32 count, u32* current) {
    nya_assert(ui != nullptr && ui == _nya_ui.open);
    nya_assert(id != nullptr && items != nullptr && current != nullptr);
    nya_assert(count > 0, "a breadcrumb trail has at least one crumb");

    // named by the caller's id, so two trails offering the same words keep their own focus and ids.
    if (!nya_ui_panel_begin(ui, id, (NYA_UIPanel){ .direction = NYA_UI_DIRECTION_ROW, .align = NYA_UI_ALIGN_CENTER, .frameless = true })) return false;

    const NYA_UILook* look    = _nya_ui_look();
    u32               here    = nya_min(*current, count - 1);
    b8                changed = false;

    for (u32 i = 0; i < count; i++) {
        // a separator between crumbs, a plain dim mark that takes no focus and no click.
        if (i > 0) nya_ui_label(ui, "/", look->style.text_dim);

        /*
         * The crumb the trail is at is the page in view, so it is a label rather than a link: nothing to
         * click, nothing to focus. Every crumb before or after it is a button, which is what carries the
         * focus, the keyboard and the click for free. Activating one navigates there.
         */
        if (i == here) {
            nya_ui_label(ui, items[i]);
        } else if (nya_ui_button(ui, items[i])) {
            *current = i;
            changed  = true;
        }
    }

    nya_ui_panel_end(ui);

    return changed;
}


/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * INTERNAL
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

b8 _nya_ui_choice_list(NYA_UI* ui, NYA_ConstCString id, const NYA_ConstCString* labels, u32 count, u32* selected, f32x2 at, f32 width) {
    nya_assert(ui != nullptr && id != nullptr && labels != nullptr && selected != nullptr);
    nya_assert(count > 0 && width > 0.0F);

    // framed, unlike the row, because a list hanging over other widgets has to hide them to be read at all.
    if (!_nya_ui_float_begin(ui, id, (NYA_UIPanel){ 0 }, (NYA_Rectf){ at.x, at.y, width, 0.0F })) return false;

    u32 chosen  = nya_min(*selected, count - 1);
    b8  changed = false;

    for (u32 i = 0; i < count; i++) {
        if (nya_ui_selectable(ui, labels[i], i == chosen) && i != chosen) {
            *selected = i;
            changed   = true;
        }
    }

    _nya_ui_float_end(ui);

    return changed;
}

b8 _nya_ui_choice_row(NYA_UI* ui, NYA_ConstCString id, const NYA_ConstCString* labels, u32 count, u32* selected, b8 underline) {
    nya_assert(ui != nullptr && id != nullptr && labels != nullptr && selected != nullptr);
    nya_assert(count > 0);

    // named by the caller's id, so two rows offering the same words keep their own focus and ids.
    if (!nya_ui_panel_begin(ui, id, (NYA_UIPanel){ .direction = NYA_UI_DIRECTION_ROW, .align = NYA_UI_ALIGN_CENTER, .frameless = true })) return false;

    const NYA_UILook* look    = _nya_ui_look();
    u32               chosen  = nya_min(*selected, count - 1);
    b8                changed = false;

    for (u32 i = 0; i < count; i++) {
        nya_ui_size(ui, nya_ui_grow(1));

        if (nya_ui_selectable(ui, labels[i], i == chosen) && i != chosen) {
            *selected = i;
            changed   = true;
        }
    }

    // the accent under the chosen cell, which is what makes a row of choices read as a strip of tabs.
    if (underline && _nya_ui_drawing()) {
        const _NYA_UILayout* row  = &_nya_ui.layouts[_nya_ui.depth - 1];
        f32                  bar  = look->focus_bar;
        f32                  slot = row->extent.x > 0.0F ? (row->extent.x - (row->gap * (f32)(count - 1))) / (f32)count : 0.0F;

        if (slot > 0.0F) {
            f32 x = roundf(row->origin.x + ((slot + row->gap) * (f32)chosen));
            f32 y = roundf(row->origin.y + row->extent.y - bar);

            NYA_UIWidgetDraw draw = { .kind = NYA_UI_WIDGET_UNDERLINE, .rect = { x, y, roundf(slot), bar }, .color = look->style.accent };

            _nya_ui_draw(ui, &draw);
        }
    }

    nya_ui_panel_end(ui);

    return changed;
}
