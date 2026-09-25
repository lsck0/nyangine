/**
 * @file ui.c
 *
 * The module's lifetime: one pass over a window's UI from begin to end, the per window state behind it, and the
 * flags a layer reads back out of a pass. The widgets, layout, look, input and drawing live in the ui_* files
 * beside this one. See ui.h.
 * */
#include "nyangine-core/nyangine.h"

#include "nyangine-ui/ui_internal.h"


// STATE

/* Zeroed, so the tables cost the binary nothing. Every ui_*.c file included after this one reaches it. */
NYA_INTERNAL _NYA_UISystem _nya_ui = { 0 };


// PASSES

NYA_UI* nya_ui_begin(NYA_Window* window, NYA_UIPass pass) {
    nya_assert(window != nullptr);
    nya_assert(pass < NYA_UI_PASS_COUNT);
    nya_assert(_nya_ui.open == nullptr, "a UI pass is already open; end it before beginning another");

    if (!_nya_ui.registered) {
        nya_ceiling_register("ui_widgets", NYA_UI_WIDGETS_MAX, &_nya_ui.widget_count_worst);
        nya_ceiling_register("ui_panels", NYA_UI_PANELS_MAX, &_nya_ui.panel_count);
        _nya_ui.registered = true;
    }

    NYA_UI* ui = _nya_ui_context(window);
    ui->window = window;
    ui->pass   = pass;
    ui->scale  = _nya_ui_scale_derive(window, &ui->style);

    _nya_ui.open            = ui;
    _nya_ui.trace           = nya_trace_begin(NYA_TRACE_UI);
    _nya_ui.pass_serial    += 1;
    _nya_ui.widget_count    = 0;
    _nya_ui.focus_found     = U32_MAX;
    _nya_ui.depth           = 0;
    _nya_ui.editing_seen    = false;
    _nya_ui.typing_at_begin = ui->editing != 0;
    _nya_ui.look_depth      = 0;
    _nya_ui.next_set        = false;
    _nya_ui.disabled        = 0;
    _nya_ui.claim_count     = 0;
    _nya_ui.drag_started    = false;
    _nya_ui.raise_swallowed = 0;
    _nya_ui.opacities[0]    = 1.0F;
    _nya_ui.opacity_depth   = 0;

    // before anything is measured, since every size the pass adds up comes back out of the presenter.
    _nya_ui_look_build(ui, 0);

    _nya_ui.layer_base = _nya_ui_layer_get(ui);

    // a panel opened in either of this window's last two passes is still standing; one older is a slot the table hasn't reused, and a stale rectangle must not occlude anything.
    ui->pass_previous = ui->pass_current;
    ui->pass_current  = _nya_ui.pass_serial;

    NYA_Rectf screen = { 0.0F, 0.0F, (f32)window->screen_width, (f32)window->screen_height };
    NYA_Rectf safe   = screen;

    // no platform window in a headless test, and then the whole target is safe.
    if (window->sdl_window != nullptr) {
        NYA_Rect area    = nya_window_safe_area(window->handle);
        f32      density = nya_window_pixel_density(window->handle);

        if (area.width > 0 && area.height > 0) safe = (NYA_Rectf){ (f32)area.x * density, (f32)area.y * density, (f32)area.width * density, (f32)area.height * density };
    }

    f32 margin   = _nya_ui.looks[0].margin;
    _nya_ui.safe = (NYA_Rectf){ safe.x + margin, safe.y + margin, nya_max(safe.width - (margin * 2.0F), 0.0F), nya_max(safe.height - (margin * 2.0F), 0.0F) };

    _nya_ui_input_read(ui);

    _NYA_UILayout* root = _nya_ui_layout_push();
    *root               = (_NYA_UILayout){
        .extent     = { screen.width, screen.height },
        .room       = { screen.width, screen.height },
        .main       = 1,
        .gap        = _nya_ui.looks[0].spacing,
        .overflow   = ui->style.overflow,
        .text       = NYA_UI_TEXT_BODY,
        .key        = (u64)window->handle.index + 1,
        .scope      = (u64)window->handle.index + 1,
        .group      = U32_MAX,
        .panel      = U32_MAX,
        .root_panel = U32_MAX,
        .layer      = _nya_ui.layer_base,
        .clip       = screen,
        .hidden     = _nya_ui.looks[0].line_heights[NYA_UI_TEXT_BODY] <= 0.0F,
    };

    return ui;
}

void nya_ui_end(NYA_UI* ui) {
    nya_assert(ui != nullptr && ui == _nya_ui.open);
    nya_assert(_nya_ui.depth == 1, "%u containers are still open", _nya_ui.depth - 1);
    nya_assert(_nya_ui.look_depth == 0, "%u pushed styles were not popped", _nya_ui.look_depth);
    nya_assert(_nya_ui.disabled == 0, "%u disabled blocks were not ended", _nya_ui.disabled);
    nya_assert(_nya_ui.opacity_depth == 0, "%u opacity groups were not ended", _nya_ui.opacity_depth);

    u32 count = _nya_ui.widget_count;

    if (count > 0) {
        // a focused widget that vanished, as every label does when the locale changes, hands focus to its position.
        u32 index  = _nya_ui.focus_found != U32_MAX ? _nya_ui.focus_found : nya_min(ui->focus_index, count - 1);
        u32 before = index;

        const u32* groups = _nya_ui.widget_groups;
        const b8*  press  = _nya_ui.presses;

        // a group is one line: a lone widget, or every focusable cell of a row. up and down keep the position in it.
        u32 first    = groups[index];
        u32 position = index - first;

        if (press[_NYA_UI_UP]) {
            // adding count - 1 wraps upward without an unsigned 0 - 1.
            u32 last = (first + count - 1) % count;
            index    = nya_min(groups[last] + position, last);
        } else if (press[_NYA_UI_DOWN]) {
            u32 next = first;
            while (next < count && groups[next] == first) next++;
            if (next == count) next = 0;

            u32 last = next;
            while (last + 1 < count && groups[last + 1] == next) last++;

            index = nya_min(next + position, last);
        } else if (press[_NYA_UI_LEFT] && !_nya_ui.widget_horizontal[index] && index > first) {
            index -= 1;
        } else if (press[_NYA_UI_RIGHT] && !_nya_ui.widget_horizontal[index] && index + 1 < count && groups[index + 1] == first) {
            index += 1;
        } else if (press[_NYA_UI_TAB]) {
            // every widget in declaration order, lines and panels alike: the one move that reaches a whole UI and the only one a terminal has; adding count - 1 wraps backward without an unsigned 0 - 1.
            b8 backward = (nya_input_modifiers() & NYA_KEYMOD_SHIFT) != 0;
            index       = (index + (backward ? count - 1 : 1)) % count;
        }

        _nya_ui_focus_set(ui, _nya_ui.widgets[index]);
        ui->focus_index = index;

        if (index != before) {
            ui->reveal = true;

            // the panel focus moved into comes forward, so the keyboard and the pointer never disagree about which of two overlapping windows is in front.
            u32 panel = _nya_ui.widget_panels[index];
            if (ui->pass == NYA_UI_PASS_INPUT && panel != U32_MAX) _nya_ui_panel_raise(ui, panel);
        }
    }

    if (ui->pass == NYA_UI_PASS_INPUT) {
        if (!_nya_ui.pointer_down) {
            ui->active   = 0;
            ui->dragging = false;
            ui->grab     = 0;
        }

        // the field left the UI while typing, as when its menu closes.
        if (ui->editing != 0 && !_nya_ui.editing_seen) _nya_ui_typing_stop(ui);

        ui->typing = _nya_ui.typing_at_begin || ui->editing != 0;
    }

    // whatever the caller draws next belongs where it asked for it, not in the layer of the last panel declared.
    _nya_ui_layer_set(ui, _nya_ui.layer_base);

    _nya_ui.widget_count_worst = nya_max(_nya_ui.widget_count_worst, count);
    _nya_ui.depth              = 0;
    _nya_ui.open               = nullptr;

    nya_trace_end(&_nya_ui.trace);
}


// STATE QUERIES

void nya_ui_disabled_begin(NYA_UI* ui) {
    nya_assert(ui != nullptr && ui == _nya_ui.open);

    _nya_ui.disabled += 1;
}

void nya_ui_disabled_end(NYA_UI* ui) {
    nya_assert(ui != nullptr && ui == _nya_ui.open);
    nya_assert(_nya_ui.disabled > 0, "nya_ui_disabled_end without a begin");

    _nya_ui.disabled -= 1;
}

b8 nya_ui_cancelled(const NYA_UI* ui) {
    nya_assert(ui != nullptr && ui == _nya_ui.open);

    return _nya_ui.cancel;
}

b8 nya_ui_typing(const NYA_Window* window) {
    nya_assert(window != nullptr);

    return _nya_ui_context(window)->typing;
}

b8 nya_ui_pointer_over(const NYA_Window* window) {
    nya_assert(window != nullptr);

    // One pointer, one _nya_ui: the window is taken for the API's sake and the day this becomes per window.
    nya_unused(window);

    return _nya_ui.pointer_over_panel;
}

void nya_ui_focus_reset(NYA_Window* window) {
    nya_assert(window != nullptr);

    NYA_UI* ui = _nya_ui_context(window);
    if (ui->editing != 0) _nya_ui_typing_stop(ui);

    ui->focus        = 0;
    ui->focus_index  = 0;
    ui->active       = 0;
    ui->dragging     = false;
    ui->typing       = false;
    ui->reveal       = true;
    ui->open         = 0;
    ui->drag_panel   = 0;
    ui->resize_panel = 0;
}

b8 nya_ui_modal_event(NYA_Event* event) {
    nya_assert(event != nullptr);

    switch (event->type) {
        case NYA_EVENT_KEY_DOWN:
        case NYA_EVENT_KEY_UP:
        case NYA_EVENT_TEXT_INPUT:
        case NYA_EVENT_TEXT_EDITING:
        case NYA_EVENT_MOUSE_BUTTON_DOWN:
        case NYA_EVENT_MOUSE_BUTTON_UP:
        case NYA_EVENT_MOUSE_WHEEL_MOVED: {
            event->was_handled = true;
            return true;
        }

        default: return false;
    }
}


// INTERNAL

NYA_UI* _nya_ui_context(const NYA_Window* window) {
    nya_assert(window->handle.index < NYA_WINDOW_MAX);

    NYA_UI* ui = &_nya_ui.windows[window->handle.index];

    b8 same = ui->claimed && ui->handle.generation == window->handle.generation;
    if (!same) {
        *ui = (NYA_UI){
            .claimed = true,
            .handle  = window->handle,
            .present = nya_ui_presenter_shape(),
            .style   = _nya_ui_style_resolve((NYA_UIStyle){ 0 }),
        };
    }

    return ui;
}
