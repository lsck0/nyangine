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

    // what the chrome about to be placed takes at each end, so the title is centred in what is left.
    const NYA_UILook* measured = _nya_ui_look();
    f32               chrome   = measured->title_bar;

    panel.title_room = (f32x2){ window.menu_count > 0 ? chrome : 0.0F, (window.close ? chrome : 0.0F) + (window.collapse ? chrome : 0.0F) };

    if (!nya_ui_panel_begin(ui, id, panel)) return false;

    _NYA_UILayout*    layout = &_nya_ui.layouts[_nya_ui.depth - 1];
    const NYA_UILook* look   = _nya_ui_look();

    NYA_Rectf bounds = layout->bounds;

    /*
     * A fold, measured. Both heights are known here and nowhere earlier: the one the last pass laid out
     * and the one this pass just did. The origin has already moved by the anchor's share of the
     * difference, and this puts it back, by the same rule the corner grip corrects a resize with. A
     * person folding a window is looking at its title bar, so the title bar is what holds still.
     *
     * Keyed off the flag rather than off the chevron, so a program that folds a window itself gets the
     * same window back.
     */
    _NYA_UIPanelState* folding = &_nya_ui.panels[layout->root_panel];

    if (folding->folded != state->collapsed) {
        folding->folded    = state->collapsed;
        folding->fold_from = folding->fold_height;
    }

    if (folding->fold_from > 0.0F && fabsf(bounds.height - folding->fold_from) > 0.5F) {
        // the anchor's row, the same 0 / 0.5 / 1 share of a size change the grip corrects by.
        u32 row  = (u32)layout->options.anchor / 3;
        f32 down = (f32)row * 0.5F;

        folding->drag.y   += (bounds.height - folding->fold_from) * down;
        folding->fold_from = 0.0F;
    }

    folding->fold_height = bounds.height;

    // square to the bar, so a chrome button is a button in it rather than a tile filling it.
    f32 side = look->title_bar;

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

        if (_nya_ui_chrome_button(ui, "menu", button, NYA_UI_MARK_MENU)) ui->open = open ? 0 : menu;

        if (open) {
            u32   picked = NYA_UI_MENU_NONE;
            f32x2 at     = { button.x, button.y + button.height };

            // as wide as the widest label plus the room a selectable puts around one, so it never wraps its own row.
            f32 width = 0.0F;
            for (u32 i = 0; i < window.menu_count; i++) width = nya_max(width, _nya_ui_text_width(layout->text, window.menu[i]));

            width += (look->padding * 5.0F) + look->line_heights[layout->text];

            if (_nya_ui_choice_list(ui, "menu", window.menu, window.menu_count, &picked, at, width)) {
                state->menu_picked = picked;
                ui->open           = 0;
            }

            if (_nya_ui.pointer_pressed && !nya_rect_contains(button, _nya_ui.pointer) && !_nya_ui_claimed(_nya_ui.pointer)) ui->open = 0;
        }
    }

    if (window.collapse) {
        NYA_UIMark mark = state->collapsed ? NYA_UI_MARK_COLLAPSED : NYA_UI_MARK_EXPANDED;

        if (_nya_ui_chrome_button(ui, "collapse", collapse, mark)) state->collapsed = !state->collapsed;
    }

    if (window.close && _nya_ui_chrome_button(ui, "close", close, NYA_UI_MARK_CLOSE)) state->open = false;

    if (window.resize && !state->collapsed) {
        f32       grip_side = _nya_ui_px(NYA_UI_GRIP);
        NYA_Rectf grip      = { bounds.x + bounds.width - grip_side, bounds.y + bounds.height - grip_side, grip_side, grip_side };

        _nya_ui_window_resize(ui, layout->key, state, bounds, grip);

        if (_nya_ui_drawn(grip)) {
            NYA_UIWidgetDraw draw = { .kind = NYA_UI_WIDGET_GRIP, .rect = grip, .as_mark = { .mark = NYA_UI_MARK_GRIP } };

            _nya_ui_draw(ui, &draw);
        }
    }

    layout->clip = inner;
    if (clipped && ui->pass == NYA_UI_PASS_DRAW) _nya_ui_scissor(ui, inner);

    // the pass that closed or folded it is already past the title bar, so the body is what stops here rather than
    // running once more and leaving a window on screen for a pass after the button said otherwise.
    if (!state->open || state->collapsed) {
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
    const NYA_UILook*    look   = _nya_ui_look();

    f32 height = _nya_ui_item_height(layout);
    f32 mark   = look->line_heights[layout->text];

    f32       text_width = _nya_ui_text_width(layout->text, label);
    NYA_Rectf rect       = _nya_ui_place((NYA_UISize){ 0 }, (f32x2){ ceilf(text_width) + (look->padding * 3.0F) + mark, height }, true);

    _NYA_UIWidget widget = _nya_ui_widget(ui, label, rect, false);
    if (widget.refused) return false;

    if (widget.activated) *open = !*open;

    if (_nya_ui_drawn(rect)) {
        NYA_UIWidgetDraw draw = {
            .kind    = NYA_UI_WIDGET_SECTION,
            .rect    = rect,
            .state   = _nya_ui_state(ui, widget),
            .label   = label,
            .as_mark = { .mark = *open ? NYA_UI_MARK_EXPANDED : NYA_UI_MARK_COLLAPSED },
        };

        _nya_ui_draw(ui, &draw);
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

b8 nya_ui_card_begin(NYA_UI* ui, NYA_ConstCString id, NYA_ConstCString title, NYA_ConstCString subtitle) {
    nya_assert(ui != nullptr && ui == _nya_ui.open);
    nya_assert(id != nullptr, "a card is named, so its contents keep their ids when the list around it is reordered");
    nya_assert(title != nullptr, "a card is a titled container; a plain panel is the one without");

    // a framed panel, which is the card's body and stays open for the caller's content until nya_ui_card_end.
    if (!nya_ui_panel_begin(ui, id, (NYA_UIPanel){ 0 })) return false;

    const NYA_UILook* look = _nya_ui_look();

    /*
     * The heading, at the title size and left aligned rather than centred the way a window's title bar is:
     * a card reads down the page, not across a bar. The size comes from a nested frameless panel that
     * lifts the text role to TITLE, so the label follows the theme's title face and colour with nothing
     * hardcoded. The subtitle is the same trick at the small size in the dim colour.
     */
    if (title[0] != '\0' && nya_ui_panel_begin(ui, "title", (NYA_UIPanel){ .text = NYA_UI_TEXT_TITLE, .frameless = true })) {
        nya_ui_label(ui, title);
        nya_ui_panel_end(ui);
    }

    if (subtitle != nullptr && subtitle[0] != '\0' && nya_ui_panel_begin(ui, "subtitle", (NYA_UIPanel){ .text = NYA_UI_TEXT_SMALL, .frameless = true })) {
        nya_ui_label(ui, subtitle, look->style.text_dim);
        nya_ui_panel_end(ui);
    }

    // a rule under the header, which is what sets the heading apart from the body the caller is about to add.
    NYA_Rectf rule = nya_ui_space(ui, 0.0F, 1.0F);

    if (_nya_ui_drawn(rule)) {
        NYA_UIWidgetDraw draw = { .kind = NYA_UI_WIDGET_RULE, .rect = rule, .color = look->style.text_dim };

        _nya_ui_draw(ui, &draw);
    }

    return true;
}

void nya_ui_card_end(NYA_UI* ui) {
    nya_assert(ui != nullptr && ui == _nya_ui.open);
    nya_assert(_nya_ui.depth > 1, "nya_ui_card_end without a card_begin");

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
    f32x2 moved     = { _nya_ui.pointer.x - ui->resize_grip.x, _nya_ui.pointer.y - ui->resize_grip.y };
    f32x2 delta     = { moved.x / ui->scale, moved.y / ui->scale };
    f32x2 least     = NYA_UI_WINDOW_MIN;
    ui->resize_grip = _nya_ui.pointer;

    f32x2 before = state->size;

    state->size = (f32x2){ nya_max(state->size.x + delta.x, least.x), nya_max(state->size.y + delta.y, least.y) };

    /*
     * A window grows away from the corner that is being pulled, and stops at the screen rather than at
     * its anchor.
     *
     * An anchor is a fraction of the room left over — 0 at the left edge, 0.5 centred, 1 at the right —
     * so the origin moves by that fraction of every change in size. Left alone, a window anchored
     * bottom right grows up and left while the pointer pulls down and right: the corner being dragged
     * runs away from the hand dragging it. So the drag offset is corrected by exactly what the anchor
     * moved, and the top left stays where it is. A window already against the edge it is anchored to
     * has nowhere to put the growth and does grow inwards — there is no room to do anything else — but
     * one with any space keeps its grabbed corner under the pointer.
     */
    f32x2 grew   = { (state->size.x - before.x) * ui->scale, (state->size.y - before.y) * ui->scale };
    // the anchor's column and row, the same 0 / 0.5 / 1 the layout places it by.
    u32 column = (u32)layout->options.anchor % 3;
    u32 row    = (u32)layout->options.anchor / 3;

    f32 across = (f32)column * 0.5F;
    f32 down   = (f32)row * 0.5F;

    // the drag offset is the panel's, which is where "where it is on screen" lives; see _nya_ui_panel_drag.
    _NYA_UIPanelState* panel = &_nya_ui.panels[layout->root_panel];

    panel->drag.x += grew.x * across;
    panel->drag.y += grew.y * down;
}

b8 _nya_ui_chrome_button(NYA_UI* ui, NYA_ConstCString label, NYA_Rectf rect, NYA_UIMark mark) {
    nya_assert(ui != nullptr && label != nullptr);
    nya_assert(mark < NYA_UI_MARK_COUNT);

    // no _nya_ui_place: the bar is chrome the panel already reserved, so its buttons take no room in the content.
    _NYA_UIWidget widget = _nya_ui_widget(ui, label, rect, false);
    if (widget.refused) return false;

    if (_nya_ui_drawn(rect)) {
        // the fill only once it is worth seeing, so a quiet title bar is a title and three marks rather than a row
        // of buttons, and the focus mark still lands because that is drawn whatever the fill does.
        NYA_UIWidgetDraw draw = {
            .kind    = NYA_UI_WIDGET_CHROME,
            .rect    = rect,
            .state   = _nya_ui_state(ui, widget),
            .as_mark = { .mark = mark, .body = widget.focused || widget.held },
        };

        _nya_ui_draw(ui, &draw);
    }

    return widget.activated;
}
