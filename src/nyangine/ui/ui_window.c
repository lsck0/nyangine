/**
 * @file ui_window.c
 *
 * Windows and the chrome they are made of: a title bar with a menu, a collapse chevron and a close X, a corner grip
 * that resizes, and the folding section a window's contents are grouped into. None of it owns a flag; see ui.h.
 * */
#include "nyangine/nyangine.h"

#include "nyangine/ui/ui_internal.h"


/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * WINDOWS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

b8 nya_ui_window_begin(NYA_UI* ui, NYA_ConstCString id, NYA_UIWindow window, NYA_UIWindowState* state) {
    nya_assert(ui != nullptr && ui == _nya_ui.open);
    nya_assert(id != nullptr, "a window is named, so its place and its size survive the list it is in being reordered");
    nya_assert(window.title != nullptr, "a window has a title bar to carry its chrome; a panel is the one without");
    nya_assert(state != nullptr, "a window reports into the caller's state; see NYA_UIWindowState");
    nya_assert(window.menu != nullptr || window.menu_count == 0, "a menu with items needs the labels to draw them from");
    nya_assert(_nya_ui.depth == 1, "a window is a top level panel; nest a panel inside one instead");

    // written every pass, so a caller that only reads it after the begin can never see an older pass's pick.
    state->menu_picked = NYA_UI_MENU_NONE;

    if (!state->open) return false;

    NYA_UIPanel panel = window.panel;
    panel.title       = window.title;
    panel.draggable   = true;

    // what the grip left, until the grip is dragged again. Collapsed, the height is whatever the bar measures.
    if (window.resize && state->size.x > 0.0F) panel.width = nya_ui_fixed(state->size.x);
    if (window.resize && state->size.y > 0.0F && !state->collapsed) panel.height = nya_ui_fixed(state->size.y);
    if (state->collapsed) panel.height = nya_ui_fit();

    if (!nya_ui_panel_begin(ui, id, panel)) return false;

    _NYA_UILayout*     layout = &_nya_ui.layouts[_nya_ui.depth - 1];
    const _NYA_UILook* look   = _nya_ui_look();

    NYA_Rectf bounds = layout->bounds;
    f32       side   = look->line_heights[NYA_UI_TEXT_TITLE];

    /*
     * The bar and the corner grip are chrome, outside the view a scrolling window clips its contents to, so the
     * clip comes off for as long as they are declared and goes back on for the body. Without this a window whose
     * contents are longer than it is has a close button that is neither drawn nor reachable.
     */
    NYA_Rectf inner   = layout->clip;
    b8        clipped = layout->clipping;

    layout->clip = _nya_ui.layouts[0].clip;
    if (clipped && ui->pass == NYA_UI_PASS_DRAW) _nya_ui_scissor(ui, layout->clip);

    // the title strip the panel reserved, which is also the strip the panel drag grips; the buttons sit in it and
    // call that grab off when one of them takes the press. See _nya_ui_widget.
    NYA_Rectf bar = { bounds.x + layout->before.x, bounds.y + layout->before.y, nya_max(bounds.width - layout->before.x - layout->after.x, 0.0F), side };

    // placed right to left and declared left to right, so tab walks the bar the way it reads.
    f32       right    = bar.x + bar.width;
    NYA_Rectf close    = { 0 };
    NYA_Rectf collapse = { 0 };

    if (window.close) {
        right -= side;
        close  = (NYA_Rectf){ right, bar.y, side, side };
    }

    if (window.collapse) {
        right    -= side;
        collapse  = (NYA_Rectf){ right, bar.y, side, side };
    }

    if (window.menu_count > 0) {
        NYA_Rectf button = { bar.x, bar.y, side, side };

        // the module's one open list, so a window's menu and a dropdown cannot both be showing and escape closes
        // whichever is. The id is the button's, which is what nya_ui_dropdown holds too.
        u64 menu = _nya_ui_id(layout->scope, "menu");
        b8  open = ui->open == menu;

        if (_nya_ui_chrome_button(ui, "menu", button, _NYA_UI_MARK_MENU)) ui->open = open ? 0 : menu;

        if (open) {
            u32   picked = NYA_UI_MENU_NONE;
            f32x2 at     = { button.x, button.y + button.height };

            // as wide as the widest label plus the room a selectable puts around one, so it never wraps its own row.
            f32 width = 0.0F;
            for (u32 i = 0; i < window.menu_count; i++) width = nya_max(width, nya_font_width(look->fonts[layout->text], window.menu[i]));

            width += (look->padding * 5.0F) + look->line_heights[layout->text];

            if (_nya_ui_choice_list(ui, "menu", window.menu, window.menu_count, &picked, at, width)) {
                state->menu_picked = picked;
                ui->open           = 0;
            }

            if (_nya_ui.pointer_pressed && !nya_rect_contains(button, _nya_ui.pointer) && !_nya_ui_claimed(_nya_ui.pointer)) ui->open = 0;
        }
    }

    if (window.collapse) {
        _NYA_UIMark mark = state->collapsed ? _NYA_UI_MARK_COLLAPSED : _NYA_UI_MARK_EXPANDED;

        if (_nya_ui_chrome_button(ui, "collapse", collapse, mark)) state->collapsed = !state->collapsed;
    }

    if (window.close && _nya_ui_chrome_button(ui, "close", close, _NYA_UI_MARK_CLOSE)) state->open = false;

    if (window.resize && !state->collapsed) {
        f32       grip_side = _nya_ui_px(NYA_UI_GRIP);
        NYA_Rectf grip      = { bounds.x + bounds.width - grip_side, bounds.y + bounds.height - grip_side, grip_side, grip_side };

        _nya_ui_window_resize(ui, layout->key, state, bounds, grip);

        if (_nya_ui_drawn(grip)) _nya_ui_mark_draw(ui, _NYA_UI_MARK_GRIP, grip, look->style.text_dim);
    }

    layout->clip = inner;
    if (clipped && ui->pass == NYA_UI_PASS_DRAW) _nya_ui_scissor(ui, inner);

    if (state->collapsed) {
        nya_ui_panel_end(ui);
        return false;
    }

    return true;
}

void nya_ui_window_end(NYA_UI* ui) {
    nya_assert(ui != nullptr && ui == _nya_ui.open);
    nya_assert(_nya_ui.depth == 2, "nya_ui_window_end with %u containers still open inside it", _nya_ui.depth - 2);

    nya_ui_panel_end(ui);
}

b8 nya_ui_section_begin(NYA_UI* ui, NYA_ConstCString label, b8* open) {
    nya_assert(ui != nullptr && ui == _nya_ui.open);
    nya_assert(label != nullptr && open != nullptr);

    const _NYA_UILayout* layout = &_nya_ui.layouts[_nya_ui.depth - 1];
    const _NYA_UILook*   look   = _nya_ui_look();

    f32 height = _nya_ui_item_height(layout);
    f32 mark   = look->line_heights[layout->text];

    f32       text_width = nya_font_width(look->fonts[layout->text], label);
    NYA_Rectf rect       = _nya_ui_place((NYA_UISize){ 0 }, (f32x2){ ceilf(text_width) + (look->padding * 3.0F) + mark, height }, true);

    _NYA_UIWidget widget = _nya_ui_widget(ui, label, rect, false);
    if (widget.refused) return false;

    if (widget.activated) *open = !*open;

    if (_nya_ui_drawn(rect)) {
        NYA_Rectf body  = _nya_ui_body_draw(ui, rect, widget);
        NYA_Color color = _nya_ui_color(&look->style.text, widget);

        NYA_Rectf chevron = { body.x + look->padding, roundf(body.y + ((body.height - mark) * 0.5F)), mark, mark };
        _nya_ui_mark_draw(ui, *open ? _NYA_UI_MARK_EXPANDED : _NYA_UI_MARK_COLLAPSED, chevron, color);

        NYA_Rectf text = { body.x + (look->padding * 2.0F) + mark, body.y, body.width, body.height };
        _nya_ui_text_draw(ui, layout, label, text_width, text, NYA_UI_ALIGN_START, color);
    }

    if (!*open) return false;

    // named by its label, so two sections in one panel keep their own children's ids, and indented by the padding
    // so the fold reads as holding what is under it.
    return nya_ui_panel_begin(ui, label, (NYA_UIPanel){ .padding = look->style.padding, .frameless = true });
}

void nya_ui_section_end(NYA_UI* ui) {
    nya_assert(ui != nullptr && ui == _nya_ui.open);
    nya_assert(_nya_ui.depth > 1, "nya_ui_section_end without a section_begin");

    nya_ui_panel_end(ui);
}


/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * INTERNAL
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

void _nya_ui_window_resize(NYA_UI* ui, u64 key, NYA_UIWindowState* state, NYA_Rectf bounds, NYA_Rectf grip) {
    nya_assert(ui != nullptr && state != nullptr && key != 0);

    if (ui->pass != NYA_UI_PASS_INPUT) return;

    const _NYA_UILayout* layout = &_nya_ui.layouts[_nya_ui.depth - 1];

    if (_nya_ui.pointer_pressed && !layout->covered && !_nya_ui_claimed(_nya_ui.pointer) && ui->resize_panel == 0 &&
        nya_rect_contains(grip, _nya_ui.pointer)) {
        ui->resize_panel = key;
        ui->resize_grip  = _nya_ui.pointer;

        // the size it already had, so the first move changes it by the first move and not by everything it is not.
        if (state->size.x <= 0.0F) state->size.x = bounds.width / ui->scale;
        if (state->size.y <= 0.0F) state->size.y = bounds.height / ui->scale;

        // a window is dragged by its bar and resized by its corner, and one press cannot be both.
        _nya_ui.drag_started = false;
        ui->drag_panel       = 0;
    }

    if (ui->resize_panel != key) return;

    if (!_nya_ui.pointer_down) {
        ui->resize_panel = 0;
        return;
    }

    /*
     * Moved by how far the pointer moved rather than set to where it is. An anchor that pins the right or bottom
     * edge moves the window's origin as it grows, so a size measured from that origin feeds its own change back in
     * and the window runs away to its minimum in a handful of passes. A delta cannot: it does not read the origin.
     */
    f32x2 delta     = { (_nya_ui.pointer.x - ui->resize_grip.x) / ui->scale, (_nya_ui.pointer.y - ui->resize_grip.y) / ui->scale };
    f32x2 least     = NYA_UI_WINDOW_MIN;
    ui->resize_grip = _nya_ui.pointer;

    state->size = (f32x2){ nya_max(state->size.x + delta.x, least.x), nya_max(state->size.y + delta.y, least.y) };
}

b8 _nya_ui_chrome_button(NYA_UI* ui, NYA_ConstCString label, NYA_Rectf rect, _NYA_UIMark mark) {
    nya_assert(ui != nullptr && label != nullptr);
    nya_assert(mark < _NYA_UI_MARK_COUNT);

    // no _nya_ui_place: the bar is chrome the panel already reserved, so its buttons take no room in the content.
    _NYA_UIWidget widget = _nya_ui_widget(ui, label, rect, false);
    if (widget.refused) return false;

    if (_nya_ui_drawn(rect)) {
        const _NYA_UILook* look = _nya_ui_look();

        // the fill only once it is worth seeing, so a quiet title bar is a title and three marks rather than a row
        // of buttons, and the focus mark still lands because that is drawn whatever the fill does.
        NYA_Rectf body = widget.focused || widget.held ? _nya_ui_body_draw(ui, rect, widget) : rect;

        _nya_ui_mark_draw(ui, mark, nya_rect_expand(body, -roundf(body.height * 0.25F)), _nya_ui_color(&look->style.text, widget));
    }

    return widget.activated;
}

void _nya_ui_mark_draw(NYA_UI* ui, _NYA_UIMark mark, NYA_Rectf rect, NYA_Color color) {
    nya_assert(ui != nullptr);
    nya_assert(mark < _NYA_UI_MARK_COUNT);

    if (rect.width <= 0.0F || rect.height <= 0.0F) return;

    NYA_Window* window    = ui->window;
    f32         thickness = nya_max(_nya_ui_px(1.5F), 1.0F);
    NYA_Color   faded     = _nya_ui_fade(color);

    f32x2 top_left     = { rect.x, rect.y };
    f32x2 bottom_right = { rect.x + rect.width, rect.y + rect.height };
    f32x2 center       = { rect.x + (rect.width * 0.5F), rect.y + (rect.height * 0.5F) };

    switch (mark) {
        case _NYA_UI_MARK_CLOSE: {
            nya_render2d_line(window, top_left, bottom_right, thickness, faded);
            nya_render2d_line(window, (f32x2){ bottom_right.x, top_left.y }, (f32x2){ top_left.x, bottom_right.y }, thickness, faded);
        } break;

        // a triangle rather than two lines: a chevron a cell tall in a terminal is one glyph either way, and a
        // filled one survives being rounded to cells where a stroked one falls between them.
        case _NYA_UI_MARK_EXPANDED: {
            nya_render2d_triangle(window, top_left, (f32x2){ bottom_right.x, top_left.y }, (f32x2){ center.x, bottom_right.y }, faded);
        } break;

        case _NYA_UI_MARK_COLLAPSED: {
            nya_render2d_triangle(window, top_left, (f32x2){ bottom_right.x, center.y }, (f32x2){ top_left.x, bottom_right.y }, faded);
        } break;

        case _NYA_UI_MARK_MENU: {
            for (u32 i = 0; i < 3; i++) {
                f32 y = roundf(rect.y + (rect.height * ((f32)i * 0.5F)));

                nya_render2d_rect(window, rect.x, nya_min(y, bottom_right.y - thickness), rect.width, thickness, faded);
            }
        } break;

        // two strokes along the corner, which is what a resize grip is everywhere anyone has seen one.
        case _NYA_UI_MARK_GRIP: {
            for (u32 i = 1; i < 3; i++) {
                f32 inset = rect.width * ((f32)i / 3.0F);

                nya_render2d_line(window, (f32x2){ rect.x + inset, bottom_right.y }, (f32x2){ bottom_right.x, rect.y + inset }, thickness, faded);
            }
        } break;

        case _NYA_UI_MARK_COUNT:
        default:                  nya_unreachable();
    }
}
