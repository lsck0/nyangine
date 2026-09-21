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
    const _NYA_UILook*   look   = _nya_ui_look();

    if (_nya_ui.disabled > 0) color = look->style.text.disabled;

    NYA_Font font  = look->fonts[layout->text];
    f32      line  = look->line_heights[layout->text];
    f32      room  = _nya_ui_text_room(layout);
    f32      width = nya_font_width(font, text);
    b8       over  = room > 0.0F && width > room;

    if (over && layout->overflow == NYA_UI_OVERFLOW_WRAP) {
        f32x2     size = nya_font_measure_wrapped(font, text, room);
        NYA_Rectf rect = _nya_ui_place((NYA_UISize){ 0 }, (f32x2){ ceilf(size.x), ceilf(size.y) }, false);

        // NYA_UIAlign and NYA_TextAlign share their order.
        if (_nya_ui_drawn(rect)) nya_font_draw_wrapped(ui->window, font, text, rect.x, rect.y, rect.width, (NYA_TextAlign)layout->align, _nya_ui_fade(color));
        return;
    }

    if (over && layout->overflow == NYA_UI_OVERFLOW_SHRINK) {
        // whole points, so a line that changes a digit a frame does not bake an atlas a frame.
        font.point_size = nya_max(floorf(font.point_size * (room / width)), 1.0F);
        width           = nya_font_width(font, text);

        if (width > room && font.point_size > 1.0F) {
            font.point_size -= 1.0F;
            width            = nya_font_width(font, text);
        }

        // a size not loaded yet measures zero; holding the room keeps the container from collapsing for that frame.
        if (width <= 0.0F) width = room;
    }

    NYA_Rectf rect = _nya_ui_place((NYA_UISize){ 0 }, (f32x2){ ceilf(width), line }, false);
    if (!_nya_ui_drawn(rect)) return;

    // a shrunk line keeps the line height of its size, centred in it.
    f32 y = font.point_size != look->fonts[layout->text].point_size ? roundf(rect.y + ((line - nya_font_metrics(font).line_height) * 0.5F)) : rect.y;

    nya_font_draw(ui->window, font, text, rect.x, y, _nya_ui_fade(color));
}

b8 nya_ui_button(NYA_UI* ui, NYA_ConstCString label) {
    nya_assert(ui != nullptr && ui == _nya_ui.open);
    nya_assert(label != nullptr);

    const _NYA_UILayout* layout = &_nya_ui.layouts[_nya_ui.depth - 1];
    const _NYA_UILook*   look   = _nya_ui_look();

    f32       text_width = nya_font_width(look->fonts[layout->text], label);
    NYA_Rectf rect       = _nya_ui_place((NYA_UISize){ 0 }, (f32x2){ ceilf(text_width) + (look->padding * 2.0F), _nya_ui_item_height(layout) }, true);

    _NYA_UIWidget widget = _nya_ui_widget(ui, label, rect, false);
    if (widget.refused) return false;

    if (_nya_ui_drawn(rect)) {
        NYA_Rectf body = _nya_ui_body_draw(ui, rect, widget);
        _nya_ui_text_draw(ui, layout, label, text_width, body, NYA_UI_ALIGN_CENTER, _nya_ui_color(&look->style.text, widget));
    }

    return widget.activated;
}

b8 nya_ui_selectable(NYA_UI* ui, NYA_ConstCString label, b8 selected) {
    nya_assert(ui != nullptr && ui == _nya_ui.open);
    nya_assert(label != nullptr);

    const _NYA_UILayout* layout = &_nya_ui.layouts[_nya_ui.depth - 1];
    const _NYA_UILook*   look   = _nya_ui_look();

    f32 height = _nya_ui_item_height(layout);
    f32 dot    = roundf(height * 0.14F);

    f32       text_width = nya_font_width(look->fonts[layout->text], label);
    NYA_Rectf rect       = _nya_ui_place((NYA_UISize){ 0 }, (f32x2){ ceilf(text_width) + (look->padding * 3.0F) + (dot * 2.0F), height }, true);

    _NYA_UIWidget widget = _nya_ui_widget(ui, label, rect, false);
    if (widget.refused) return false;

    if (_nya_ui_drawn(rect)) {
        NYA_Rectf body   = _nya_ui_body_draw(ui, rect, widget);
        f32x2     center = { body.x + look->padding + dot, body.y + roundf(body.height * 0.5F) };

        // an empty hole when not chosen, so the list reads as a choice even before anything is picked.
        nya_render2d_circle(ui->window, center, dot, _nya_ui_fade(selected && !widget.disabled ? look->style.accent : look->style.track));

        NYA_Rectf text = { body.x + (dot * 2.0F) + look->padding, body.y, body.width - (dot * 2.0F) - look->padding, body.height };
        _nya_ui_text_draw(ui, layout, label, text_width, text, NYA_UI_ALIGN_CENTER, _nya_ui_color(&look->style.text, widget));
    }

    return widget.activated;
}

b8 nya_ui_toggle(NYA_UI* ui, NYA_ConstCString label, b8* value) {
    nya_assert(ui != nullptr && ui == _nya_ui.open);
    nya_assert(label != nullptr && value != nullptr);

    const _NYA_UILayout* layout = &_nya_ui.layouts[_nya_ui.depth - 1];
    const _NYA_UILook*   look   = _nya_ui_look();

    f32 height      = _nya_ui_item_height(layout);
    f32 pill_height = roundf(height * 0.5F);
    f32 pill_width  = roundf(pill_height * 1.8F);

    f32       text_width = nya_font_width(look->fonts[layout->text], label);
    NYA_Rectf rect       = _nya_ui_place((NYA_UISize){ 0 }, (f32x2){ ceilf(text_width) + (look->padding * 3.0F) + pill_width, height }, true);

    _NYA_UIWidget widget = _nya_ui_widget(ui, label, rect, true);
    if (widget.refused) return false;

    b8 before = *value;

    if (widget.activated) *value = !*value;
    if (widget.focused && _nya_ui.presses[_NYA_UI_LEFT]) *value = false;
    if (widget.focused && _nya_ui.presses[_NYA_UI_RIGHT]) *value = true;

    if (_nya_ui_drawn(rect)) {
        const NYA_UIStyle* style = &look->style;
        NYA_Rectf          body  = _nya_ui_body_draw(ui, rect, widget);
        NYA_Color          color = _nya_ui_color(&style->text, widget);

        NYA_Rectf pill = { body.x + body.width - look->padding - pill_width, roundf(body.y + ((body.height - pill_height) * 0.5F)), pill_width, pill_height };
        _nya_ui_track_draw(ui, pill, pill_height * 0.5F, *value && !widget.disabled ? style->accent : style->track);

        f32   radius = (pill_height * 0.5F) - nya_max(look->outline, _nya_ui_px(2.0F));
        f32x2 center = { *value ? pill.x + pill_width - (pill_height * 0.5F) : pill.x + (pill_height * 0.5F), pill.y + (pill_height * 0.5F) };
        NYA_Rectf knob = { center.x - radius, center.y - radius, radius * 2.0F, radius * 2.0F };

        if (!_nya_ui_skin_draw(ui, &style->knob_skin, knob, color)) nya_render2d_circle(ui->window, center, radius, _nya_ui_fade(color));

        // text after every shape, so the shapes of the whole menu can share a draw call.
        _nya_ui_text_draw(ui, layout, label, text_width, (NYA_Rectf){ body.x + look->padding, body.y, body.width, body.height }, NYA_UI_ALIGN_START, color);
    }

    return *value != before;
}

b8 nya_ui_slider(NYA_UI* ui, NYA_ConstCString label, f32* value, f32 min, f32 max, f32 step) {
    nya_assert(ui != nullptr && ui == _nya_ui.open);
    nya_assert(label != nullptr && value != nullptr);
    nya_assert(max > min && step >= 0.0F, "a slider needs min below max and a step of zero or more");

    const _NYA_UILayout* layout = &_nya_ui.layouts[_nya_ui.depth - 1];
    const _NYA_UILook*   look   = _nya_ui_look();

    f32 height = _nya_ui_item_height(layout);
    f32 radius = roundf(height * 0.2F);

    f32       text_width = nya_font_width(look->fonts[layout->text], label);
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
        const NYA_UIStyle* style = &look->style;
        NYA_Rectf          body  = _nya_ui_body_draw(ui, rect, widget);
        NYA_Color          color = _nya_ui_color(&style->text, widget);

        f32       t     = nya_clamp((*value - min) / (max - min), 0.0F, 1.0F);
        NYA_Rectf track = { track_x + (body.x - rect.x), roundf(body.y + ((body.height - radius) * 0.5F)), track_width, radius };

        _nya_ui_track_draw(ui, track, radius * 0.5F, style->track);
        _nya_ui_track_draw(ui, (NYA_Rectf){ track.x, track.y, roundf(track_width * t), radius }, radius * 0.5F, widget.disabled ? style->track : style->accent);

        f32x2     center = { track.x + roundf(track_width * t), track.y + (radius * 0.5F) };
        NYA_Rectf knob   = { center.x - radius, center.y - radius, radius * 2.0F, radius * 2.0F };

        if (!_nya_ui_skin_draw(ui, &style->knob_skin, knob, color)) {
            if (look->outline > 0.0F) nya_render2d_circle(ui->window, center, radius + look->outline, _nya_ui_fade(style->ink));
            nya_render2d_circle(ui->window, center, radius, _nya_ui_fade(color));
        }

        _nya_ui_text_draw(ui, layout, label, text_width, (NYA_Rectf){ body.x + look->padding, body.y, body.width, body.height }, NYA_UI_ALIGN_START, color);
    }

    return *value != before;
}

b8 nya_ui_color_picker(NYA_UI* ui, NYA_ConstCString label, NYA_Color* color) {
    nya_assert(ui != nullptr && ui == _nya_ui.open);
    nya_assert(label != nullptr && color != nullptr);

    const _NYA_UILayout* layout = &_nya_ui.layouts[_nya_ui.depth - 1];
    const _NYA_UILook*   look   = _nya_ui_look();
    const NYA_UIStyle*   style  = &look->style;

    f32 height  = _nya_ui_item_height(layout);
    f32 gap     = look->spacing;
    f32 padding = look->padding;
    f32 field   = _nya_ui_px(NYA_UI_PICKER_FIELD);
    f32 bar     = _nya_ui_px(NYA_UI_PICKER_BAR);

    f32       text_width = nya_font_width(look->fonts[layout->text], label);
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

    char*     text = ui->editing == widget.id || start ? ui->hex : shown;
    NYA_Rectf body = _nya_ui_drawn(rect) ? _nya_ui_body_draw(ui, rect, widget) : rect;
    f32x2     sink = { body.x - rect.x, body.y - rect.y };

    if (_nya_ui_drawn(rect)) {
        NYA_Rectf p = nya_rect_translate(plane, sink);
        NYA_Rectf h = nya_rect_translate(hue, sink);
        NYA_Rectf a = nya_rect_translate(alpha, sink);
        NYA_Rectf w = nya_rect_translate(swatch, sink);

        // faded up front, since every one of these goes straight into a gradient rather than through a draw helper.
        NYA_Color pure  = _nya_ui_fade(nya_color_from_hsv((NYA_ColorHSV){ hsv.h, 1.0F, 1.0F, 1.0F }));
        NYA_Color white = _nya_ui_fade((NYA_Color){ 1.0F, 1.0F, 1.0F, 1.0F });
        NYA_Color black = _nya_ui_fade((NYA_Color){ 0.0F, 0.0F, 0.0F, 1.0F });
        NYA_Color clear = { 0.0F, 0.0F, 0.0F, 0.0F };
        NYA_Color solid = _nya_ui_fade((NYA_Color){ color->r, color->g, color->b, 1.0F });
        NYA_Color faded = { color->r, color->g, color->b, 0.0F };

        nya_render2d_rect_gradient(ui->window, p.x, p.y, p.width, p.height, (NYA_Color[4]){ white, pure, pure, white });
        nya_render2d_rect_gradient(ui->window, p.x, p.y, p.width, p.height, (NYA_Color[4]){ clear, clear, black, black });

        // six bands of hue, red at both ends.
        for (u32 i = 0; i < 6; i++) {
            NYA_Color top    = nya_color_from_hsv((NYA_ColorHSV){ (f32)i * 60.0F, 1.0F, 1.0F, 1.0F });
            NYA_Color bottom = nya_color_from_hsv((NYA_ColorHSV){ (f32)((i + 1) % 6) * 60.0F, 1.0F, 1.0F, 1.0F });
            f32       y0     = roundf(h.y + (h.height * (f32)i / 6.0F));
            f32       y1     = roundf(h.y + (h.height * (f32)(i + 1) / 6.0F));

            nya_render2d_rect_gradient(ui->window, h.x, y0, h.width, y1 - y0, (NYA_Color[4]){ top, top, bottom, bottom });
        }

        _nya_ui_track_draw(ui, a, 0.0F, style->track);
        nya_render2d_rect_gradient(ui->window, a.x, a.y, a.width, a.height, (NYA_Color[4]){ faded, solid, solid, faded });

        _nya_ui_track_draw(ui, w, look->radius, style->track);
        nya_render2d_rect(ui->window, w.x, w.y, w.width, w.height, _nya_ui_fade(*color));

        // where each part stands: a ring on the field, and a notch across each bar.
        f32   mark   = _nya_ui_px(2.0F);
        f32x2 center = { roundf(p.x + (p.width * hsv.s)), roundf(p.y + (p.height * (1.0F - hsv.v))) };

        nya_render2d_circle(ui->window, center, mark * 3.0F, _nya_ui_fade(style->text.normal));
        nya_render2d_circle(ui->window, center, mark * 2.0F, _nya_ui_fade(solid));
        nya_render2d_rect(ui->window, h.x - mark, roundf(h.y + (h.height * hsv.h / 360.0F)) - mark, h.width + (mark * 2.0F), mark * 2.0F, _nya_ui_fade(style->text.normal));
        nya_render2d_rect(ui->window, roundf(a.x + (a.width * hsv.a)) - mark, a.y - mark, mark * 2.0F, a.height + (mark * 2.0F), _nya_ui_fade(style->text.normal));
    }

    b8 typed = _nya_ui_field(ui, widget, start, hex, nya_rect_translate(hex, sink), text, sizeof(ui->hex));

    // six or eight digits make a colour; anything shorter waits for the rest.
    NYA_ConstCString digits = text[0] == '#' ? text + 1 : text;
    u64              count  = strlen(digits);
    b8               valid  = count == 6 || count == 8;

    for (u64 i = 0; valid && i < count; i++) valid = isxdigit((unsigned char)digits[i]) != 0;

    if (typed && valid) *color = nya_color_from_hex(digits);

    if (_nya_ui_drawn(rect)) {
        _nya_ui_text_draw(ui, layout, label, text_width, (NYA_Rectf){ body.x + padding, body.y, body.width, height }, NYA_UI_ALIGN_START, _nya_ui_color(&style->text, widget));
    }

    return color->r != before.r || color->g != before.g || color->b != before.b || color->a != before.a;
}

b8 nya_ui_radio(NYA_UI* ui, NYA_ConstCString label, u32* selected, u32 value) {
    nya_assert(ui != nullptr && ui == _nya_ui.open);
    nya_assert(label != nullptr && selected != nullptr);

    const _NYA_UILayout* layout = &_nya_ui.layouts[_nya_ui.depth - 1];
    const _NYA_UILook*   look   = _nya_ui_look();

    f32 height = _nya_ui_item_height(layout);
    f32 ring   = roundf(height * 0.22F);

    f32       text_width = nya_font_width(look->fonts[layout->text], label);
    NYA_Rectf rect       = _nya_ui_place((NYA_UISize){ 0 }, (f32x2){ ceilf(text_width) + (look->padding * 3.0F) + (ring * 2.0F), height }, true);

    _NYA_UIWidget widget = _nya_ui_widget(ui, label, rect, false);
    if (widget.refused) return false;

    b8 chosen  = *selected == value;
    b8 changed = widget.activated && !chosen;

    if (changed) *selected = value;

    if (_nya_ui_drawn(rect)) {
        const NYA_UIStyle* style  = &look->style;
        NYA_Rectf          body   = _nya_ui_body_draw(ui, rect, widget);
        NYA_Color          color  = _nya_ui_color(&style->text, widget);
        f32x2              center = { body.x + look->padding + ring, body.y + roundf(body.height * 0.5F) };

        // a ring with a dot in it, rather than the filled dot a selectable draws, so the two never read as the same.
        nya_render2d_circle(ui->window, center, ring, _nya_ui_fade(color));
        nya_render2d_circle(ui->window, center, ring - nya_max(_nya_ui_px(2.0F), 1.0F), _nya_ui_fade(style->track));

        if (*selected == value) nya_render2d_circle(ui->window, center, roundf(ring * 0.5F), _nya_ui_fade(widget.disabled ? style->text.disabled : style->accent));

        NYA_Rectf text = { body.x + (ring * 2.0F) + (look->padding * 2.0F), body.y, body.width, body.height };
        _nya_ui_text_draw(ui, layout, label, text_width, text, NYA_UI_ALIGN_START, color);
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
    const _NYA_UILook*   look   = _nya_ui_look();

    u32              chosen = nya_min(*selected, count - 1);
    NYA_ConstCString shown  = options[chosen];

    f32 height = _nya_ui_item_height(layout);
    f32 arrow  = roundf(height * 0.18F);

    f32       label_width = nya_font_width(look->fonts[layout->text], label);
    f32       value_width = nya_font_width(look->fonts[layout->text], shown);
    NYA_Rectf rect        = _nya_ui_place((NYA_UISize){ 0 }, (f32x2){ ceilf(label_width + value_width) + (look->padding * 4.0F) + (arrow * 2.0F), height }, true);

    _NYA_UIWidget widget = _nya_ui_widget(ui, label, rect, false);
    if (widget.refused) return false;

    b8 open = ui->open == widget.id;

    // activating the closed row opens the list, and activating it again closes it without changing anything.
    if (widget.activated) ui->open = open ? 0 : widget.id;

    if (_nya_ui_drawn(rect)) {
        const NYA_UIStyle* style = &look->style;
        NYA_Rectf          body  = _nya_ui_body_draw(ui, rect, widget);
        NYA_Color          color = _nya_ui_color(&style->text, widget);

        _nya_ui_text_draw(ui, layout, label, label_width, (NYA_Rectf){ body.x + look->padding, body.y, body.width, body.height }, NYA_UI_ALIGN_START, color);

        NYA_Rectf value = { body.x, body.y, body.width - (look->padding * 2.0F) - (arrow * 2.0F), body.height };
        _nya_ui_text_draw(ui, layout, shown, value_width, value, NYA_UI_ALIGN_END, color);

        // a caret, pointing down when closed and up when the list is showing.
        f32   x    = body.x + body.width - look->padding - arrow;
        f32   y    = body.y + roundf(body.height * 0.5F);
        f32   tip  = open ? -arrow : arrow;
        f32x2 left = { x - arrow, y - (tip * 0.5F) };

        nya_render2d_triangle(ui->window, left, (f32x2){ x + arrow, y - (tip * 0.5F) }, (f32x2){ x, y + (tip * 0.5F) }, _nya_ui_fade(color));
    }

    if (!open || widget.disabled) return false;

    // the list takes room under the row rather than floating over what follows; see ui.h for why.
    u32 picked  = chosen;
    b8  changed = _nya_ui_choice_row(ui, label, options, count, &picked, false);

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
    const _NYA_UILook*   look   = _nya_ui_look();
    const NYA_UIStyle*   style  = &look->style;

    f32 line   = look->line_heights[layout->text];
    f32 height = chart.height > 0.0F ? _nya_ui_px(chart.height) : roundf(line * 4.0F);

    NYA_Rectf rect = _nya_ui_place((NYA_UISize){ 0 }, (f32x2){ _nya_ui_px(NYA_UI_CHART_WIDTH), height }, true);
    if (!_nya_ui_drawn(rect)) return;

    _nya_ui_track_draw(ui, rect, look->radius, style->track);

    // the newest points, when there are more than the plot takes; a chart of a history shows its end.
    u32        count  = nya_min(chart.count, (u32)NYA_UI_CHART_POINTS_MAX);
    const f32* values = chart.values + (chart.count - count);

    if (count == 0) return;

    f32 low  = chart.min;
    f32 high = chart.max;

    if (high <= low) {
        low  = values[0];
        high = values[0];

        for (u32 i = 1; i < count; i++) {
            low  = nya_min(low, values[i]);
            high = nya_max(high, values[i]);
        }
    }

    // a flat series would divide by zero and, worse, draw a line at a meaningless height; it sits on the floor.
    f32 span = high - low;
    if (span <= 0.0F) span = 1.0F;

    NYA_Color color = chart.color;
    if (color.r == 0.0F && color.g == 0.0F && color.b == 0.0F && color.a == 0.0F) color = style->accent;

    NYA_Rectf plot = nya_rect_expand(rect, -_nya_ui_px(2.0F));
    if (plot.width <= 0.0F || plot.height <= 0.0F) return;

    if (chart.kind == NYA_UI_CHART_BAR) {
        f32 slot = plot.width / (f32)count;
        f32 bar  = nya_max(roundf(slot * NYA_UI_CHART_BAR_SHARE), 1.0F);

        for (u32 i = 0; i < count; i++) {
            f32 t = nya_clamp((values[i] - low) / span, 0.0F, 1.0F);
            f32 h = roundf(plot.height * t);

            nya_render2d_rect(ui->window, roundf(plot.x + ((f32)i * slot)), plot.y + plot.height - h, bar, h, _nya_ui_fade(color));
        }

        return;
    }

    // one point is a dot, not a line, and polyline wants at least two.
    f32 step = count > 1 ? plot.width / (f32)(count - 1) : 0.0F;

    f32x2 points[NYA_UI_CHART_POINTS_MAX];

    for (u32 i = 0; i < count; i++) {
        f32 t = nya_clamp((values[i] - low) / span, 0.0F, 1.0F);

        points[i] = (f32x2){ plot.x + ((f32)i * step), plot.y + plot.height - roundf(plot.height * t) };
    }

    if (count == 1) {
        nya_render2d_circle(ui->window, points[0], _nya_ui_px(2.0F), _nya_ui_fade(color));
        return;
    }

    nya_render2d_polyline(ui->window, points, count, nya_max(_nya_ui_px(NYA_UI_CHART_LINE_WIDTH), 1.0F), _nya_ui_fade(color));
}

void nya_ui_icon(NYA_UI* ui, NYA_UIIcon icon, f32 size) {
    nya_assert(ui != nullptr && ui == _nya_ui.open);
    nya_assert(size >= 0.0F && icon.source_width >= 0.0F && icon.source_height >= 0.0F);

    const _NYA_UILayout* layout = &_nya_ui.layouts[_nya_ui.depth - 1];
    const _NYA_UILook*   look   = _nya_ui_look();

    f32       side = size > 0.0F ? _nya_ui_px(size) : look->line_heights[layout->text];
    NYA_Rectf rect = _nya_ui_place((NYA_UISize){ 0 }, (f32x2){ side, side }, false);

    if (!_nya_ui_drawn(rect)) return;

    NYA_Color tint = _nya_ui.disabled > 0 ? look->style.text.disabled : look->style.text.normal;

    (void)_nya_ui_icon_draw(ui, &icon, rect, tint);
}


/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * INTERNAL
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

b8 _nya_ui_choice_row(NYA_UI* ui, NYA_ConstCString id, const NYA_ConstCString* labels, u32 count, u32* selected, b8 underline) {
    nya_assert(ui != nullptr && id != nullptr && labels != nullptr && selected != nullptr);
    nya_assert(count > 0);

    // named by the caller's id, so two rows offering the same words keep their own focus and ids.
    if (!nya_ui_panel_begin(ui, id, (NYA_UIPanel){ .direction = NYA_UI_DIRECTION_ROW, .align = NYA_UI_ALIGN_CENTER, .frameless = true })) return false;

    const _NYA_UILook* look    = _nya_ui_look();
    u32                chosen  = nya_min(*selected, count - 1);
    b8                 changed = false;

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
        f32                  bar  = nya_max(_nya_ui_px(NYA_UI_FOCUS_BAR), 1.0F);
        f32                  slot = row->extent.x > 0.0F ? (row->extent.x - (row->gap * (f32)(count - 1))) / (f32)count : 0.0F;

        if (slot > 0.0F) {
            f32 x = roundf(row->origin.x + ((slot + row->gap) * (f32)chosen));
            f32 y = roundf(row->origin.y + row->extent.y - bar);

            nya_render2d_rect(ui->window, x, y, roundf(slot), bar, _nya_ui_fade(look->style.accent));
        }
    }

    nya_ui_panel_end(ui);

    return changed;
}
