/**
 * @file ui_input.c
 *
 * What a pass reads and who it goes to: this tick's presses and pointer, the focus a widget takes, and the
 * transitions its colours follow. See ui.h.
 * */
#include "nyangine/nyangine.h"

#include "nyangine/ui/ui_internal.h"


// CONSTANTS

/** In _NYA_UIPress's order. The caret keys are raw keys, since a field has to tell the arrows from A and D. */
NYA_INTERNAL const _NYA_UIPress _NYA_UI_PRESSES[_NYA_UI_PRESS_COUNT] = {
    { .action = NYA_INPUT_ACTION_UP },
    { .action = NYA_INPUT_ACTION_DOWN },
    { .action = NYA_INPUT_ACTION_LEFT },
    { .action = NYA_INPUT_ACTION_RIGHT },

    // a raw key and not an action: tab is not something a player rebinds, and a gamepad has no tab.
    { .key = NYA_KEY_TAB },

    { .key = NYA_KEY_LEFT },
    { .key = NYA_KEY_RIGHT },
    { .key = NYA_KEY_BACKSPACE },
    { .key = NYA_KEY_DELETE },
};


// INTERNAL

void _nya_ui_input_read(NYA_UI* ui) {
    _nya_ui.pointer      = nya_input_mouse_position();
    _nya_ui.pointer_down = nya_input_mouse_button_pressed(NYA_MOUSE_BUTTON_LEFT);
    _nya_ui.confirm_down = nya_input_action_pressed(NYA_INPUT_ACTION_CONFIRM);

    b8 input = ui->pass == NYA_UI_PASS_INPUT;

    // Recomputed each input pass by the panels as they lay out; cleared here so a panel that closed stops swallowing world clicks the moment it's gone.
    if (input) _nya_ui.pointer_over_panel = false;

    f32x2 delta              = nya_input_mouse_position_delta();
    _nya_ui.confirm          = input && nya_input_action_just_pressed(NYA_INPUT_ACTION_CONFIRM);
    _nya_ui.cancel           = input && nya_input_action_just_pressed(NYA_INPUT_ACTION_CANCEL);
    _nya_ui.pointer_moved    = input && (delta.x != 0.0F || delta.y != 0.0F);
    _nya_ui.pointer_pressed  = input && nya_input_mouse_button_just_pressed(NYA_MOUSE_BUTTON_LEFT);
    _nya_ui.pointer_released = input && nya_input_mouse_button_just_released(NYA_MOUSE_BUTTON_LEFT);
    _nya_ui.wheel            = input ? nya_input_mouse_wheel_scroll().y : 0.0F;
    _nya_ui.wheel_x          = input ? nya_input_mouse_wheel_scroll().x : 0.0F;

    // escape backs out one step: it closes an open list here, before any widget or layer reads it as a cancel, so the key that leaves a dropdown doesn't also leave the menu the dropdown is in.
    if (_nya_ui.cancel && ui->open != 0) {
        ui->open       = 0;
        _nya_ui.cancel = false;
    }

    nya_memset(_nya_ui.presses, 0, sizeof(_nya_ui.presses));
    if (!input) return;

    // Presses are worked out once per tick so two windows can't repeat twice as fast; a program with no world has no tick, and a TUI is that case — its passes are the only clock, and it opens one input pass per frame.
    u64 tick = nya_world_exists() ? nya_world()->sim_system.tick + 1 : _nya_ui.pass_serial;

    if (tick != _nya_ui.press_tick) {
        _nya_ui.press_tick = tick;
        _nya_ui_presses_read(ui->editing != 0);
    }

    nya_memcpy(_nya_ui.presses, _nya_ui.tick_presses, sizeof(_nya_ui.presses));

    // the menu keys are letters a field types.
    if (ui->editing != 0) nya_memset(_nya_ui.presses, 0, _NYA_UI_CARET_LEFT * sizeof(b8));
}

void _nya_ui_presses_read(b8 typing) {
    b8* fired = _nya_ui.tick_presses;
    nya_memset(fired, 0, sizeof(_nya_ui.tick_presses));

    u32 repeat = 0;

    for (u32 i = 0; i < _NYA_UI_PRESS_COUNT; i++) {
        const _NYA_UIPress* press = &_NYA_UI_PRESSES[i];

        b8 pressed = press->action != NYA_INPUT_ACTION_NONE ? nya_input_action_just_pressed(press->action) : nya_input_key_just_pressed(press->key);
        if (!pressed) continue;

        fired[i] = true;

        // an arrow fires a direction and a caret key at once: a menu repeats the direction, a field the caret.
        if (repeat == 0 || typing) repeat = i + 1;
    }

    if (repeat != 0) {
        _nya_ui.repeat_press = repeat;
        _nya_ui.repeat_s     = -NYA_UI_REPEAT_DELAY_S;
    }

    if (_nya_ui.repeat_press == 0) return;

    u32 held = _nya_ui.repeat_press - 1;
    if (fired[held]) return;

    const _NYA_UIPress* press = &_NYA_UI_PRESSES[held];

    if (!(press->action != NYA_INPUT_ACTION_NONE ? nya_input_action_pressed(press->action) : nya_input_key_pressed(press->key))) {
        _nya_ui.repeat_press = 0;
        return;
    }

    _nya_ui.repeat_s += nya_app_get()->frame_stats.delta_time_s;
    if (_nya_ui.repeat_s < NYA_UI_REPEAT_INTERVAL_S) return;

    _nya_ui.repeat_s -= NYA_UI_REPEAT_INTERVAL_S;
    fired[held]       = true;
}

_NYA_UIWidget _nya_ui_widget(NYA_UI* ui, NYA_ConstCString label, NYA_Rectf rect, b8 horizontal) {
    const _NYA_UILayout* layout = &_nya_ui.layouts[_nya_ui.depth - 1];

    u64 id = _nya_ui_id(layout->scope, label);

    // left out of focus entirely, so the keys step over it.
    if (_nya_ui.disabled > 0) return (_NYA_UIWidget){ .id = id, .disabled = true };

    if (_nya_ui.widget_count >= NYA_UI_WIDGETS_MAX) return (_NYA_UIWidget){ .refused = true };

    if (ui->pass == NYA_UI_PASS_INPUT) {
        for (u32 i = 0; i < _nya_ui.widget_count; i++) {
            nya_assert(_nya_ui.widgets[i] != id, "two widgets in one panel share the label '%s'", label);
        }
    }

    // an empty focus goes to the first widget as it is declared, so a menu's first pass can already confirm.
    if (ui->focus == 0) _nya_ui_focus_set(ui, id);

    u32 index                        = _nya_ui.widget_count++;
    _nya_ui.widgets[index]           = id;
    _nya_ui.widget_groups[index]     = layout->group != U32_MAX ? layout->group : index;
    _nya_ui.widget_panels[index]     = layout->root_panel;
    _nya_ui.widget_horizontal[index] = horizontal;
    if (ui->focus == id) _nya_ui.focus_found = index;

    _NYA_UIWidget widget = { .id = id };

    if (ui->pass == NYA_UI_PASS_INPUT) {
        // Hover, the press and the release all read this one answer, and must: a widget that took hover focus under another panel would activate on the next confirm with no pointer involved. `covered` comes from where the panels over this one were last laid out, since the panel that will cover this widget isn't declared yet; recording presses and picking a winner at a barrier in nya_ui_end was the alternative, but a press and its release arrive in the same tick, so a widget learning only at the barrier couldn't report its own activation until a tick later.
        b8 inside = !layout->covered && !_nya_ui_claimed(_nya_ui.pointer) && nya_rect_contains(rect, _nya_ui.pointer) &&
                    nya_rect_contains(layout->clip, _nya_ui.pointer);

        // hover moves the same focus as the keys, so the two never disagree: only on movement, so a resting pointer doesn't take focus back from the keys, and not while typing, where confirm belongs to the field.
        if (inside && (_nya_ui.pointer_pressed || (_nya_ui.pointer_moved && ui->editing == 0))) {
            _nya_ui_focus_set(ui, id);
            _nya_ui.focus_found = index;
        }

        // the press that brought this panel forward was spent on that; see raise_swallowed.
        if (inside && _nya_ui.raise_swallowed != 0 && _nya_ui.panels[layout->root_panel].id == _nya_ui.raise_swallowed) inside = false;

        if (inside && _nya_ui.pointer_pressed) {
            ui->active = id;

            // a panel grabbed by its grip this pass lets go again: a window's chrome buttons sit in its title bar, and pressing one must be a press on the button rather than the start of a drag.
            if (_nya_ui.drag_started) {
                ui->drag_panel       = 0;
                _nya_ui.drag_started = false;
            }
        }

        widget.activated = (ui->focus == id && _nya_ui.confirm) || (ui->active == id && _nya_ui.pointer_released && inside);

        // one bounce at a time: a pointer activates one widget, and confirm goes to the focused one.
        if (widget.activated) {
            ui->bounce_id = id;
            ui->bounce_s  = nya_app_uptime_s();
        }
    }

    widget.focused = ui->focus == id;
    widget.held    = (ui->active == id && _nya_ui.pointer_down) || (widget.focused && _nya_ui.confirm_down);
    widget.focus   = widget.focused ? 1.0F : 0.0F;
    widget.press   = widget.held ? 1.0F : 0.0F;

    if (ui->pass == NYA_UI_PASS_DRAW && _nya_ui_look()->style.transition_s > 0.0F) _nya_ui_animate(&widget);

    if (widget.focused && ui->reveal) {
        _nya_ui_reveal(rect);
        ui->reveal = false;
    }

    return widget;
}

void _nya_ui_focus_set(NYA_UI* ui, u64 id) {
    if (ui->focus == id) return;

    ui->focus           = id;
    ui->focus_changed_s = nya_app_uptime_s();
}

void _nya_ui_animate(_NYA_UIWidget* widget) {
    const NYA_UIStyle* style = &_nya_ui_look()->style;
    _NYA_UIAnimation*  entry = &_nya_ui.animations[widget->id % NYA_UI_ANIMATIONS_MAX];
    f64                now   = nya_app_uptime_s();

    // a slot another widget held starts where this one already is, which only snaps that widget's transition.
    if (entry->id != widget->id) *entry = (_NYA_UIAnimation){ .id = widget->id, .time_s = now, .focus = widget->focus, .press = widget->press };

    f32 step      = (f32)(now - entry->time_s) / style->transition_s;
    entry->time_s = now;
    entry->focus  = widget->focus > entry->focus ? nya_min(entry->focus + step, widget->focus) : nya_max(entry->focus - step, widget->focus);
    entry->press  = widget->press > entry->press ? nya_min(entry->press + step, widget->press) : nya_max(entry->press - step, widget->press);

    widget->focus = nya_ease(style->easing, entry->focus);
    widget->press = nya_ease(style->easing, entry->press);
}

void _nya_ui_typing_start(NYA_UI* ui, u64 id, u32 caret, NYA_Rectf field) {
    nya_assert(ui != nullptr && id != 0 && ui->editing != id);

    // A click takes the keyboard from whichever field had it; a field declared above the one being typed in reads the click before the other can see it and let go, so the handover happens here. Only a click: typing swallows the keys, so nothing on the keyboard can reach a second field.
    if (ui->editing != 0) {
        nya_assert(_nya_ui.pointer_pressed || _nya_ui.pointer_released, "a field started typing while another had the keyboard, and not by a click");
        _nya_ui_typing_stop(ui);
    }

    ui->editing = id;
    ui->caret   = caret;

    // nothing is selected to begin with, and a stale anchor from the last field would swallow the first backspace.
    ui->select           = caret;
    _nya_ui.editing_seen = true;

    nya_input_text_begin(ui->window->handle);
    nya_input_text_area_set(ui->window->handle, field.x, field.y, field.width, field.height);
}

void _nya_ui_typing_stop(NYA_UI* ui) {
    ui->editing = 0;

    nya_input_text_end();
}
