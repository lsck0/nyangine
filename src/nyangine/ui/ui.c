/**
 * @file ui.c
 *
 * See ui.h.
 * */
#include "nyangine/nyangine.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * TYPES
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/** One open panel or row. */
typedef struct {
    /** Top left of the content, in window pixels. */
    f32x2 origin;

    /** The width widgets fill and align in. Zero until a fitting panel has been measured once. */
    f32 width;

    /** What is inside so far: its natural extent, and how many widgets. */
    f32x2 used;
    u32   count;

    /** Zero in a panel. */
    u32 columns;

    NYA_UIAlign align;
    NYA_Font    font;
    f32         line_height;
    u64         scope;

    /** Into _NYA_UISystem.panels, or U32_MAX for a row. */
    u32         panel;
    NYA_UIPanel options;
    NYA_Rectf   bounds;
    f32         frame;
    f32         header;
    f32         title_width;

    /** Lays out without drawing: a panel with no size yet, or a font still loading. Inherited inward. */
    b8 hidden;
} _NYA_UILayout;

typedef struct {
    /** Zero is a free slot. */
    u64   id;
    f32x2 size;
    b8    measured;

    /** The pass that last opened it, so a full table only refuses panels that pass is still using. */
    u64 pass;
} _NYA_UIPanelState;

/** A widget's standing in the current pass. */
typedef struct {
    u64 id;
    b8  refused;
    b8  focused;
    b8  held;
    b8  activated;
} _NYA_UIWidget;

/** What persists per window. */
struct NYA_UI {
    b8               claimed;
    NYA_WindowHandle handle;
    NYA_Window*      window;
    NYA_UIPass       pass;

    /** Resolved when set, so every zero is already its default. */
    NYA_UIStyle style;
    NYA_Font    font;
    NYA_Font    title_font;

    u64 focus;
    u32 focus_index;
    f64 focus_changed_s;

    /** The widget the left button went down on, while it stays down, and whether that press grabbed a slider. */
    u64 active;
    b8  dragging;
};

typedef struct {
    NYA_UI windows[NYA_WINDOW_MAX];

    /** The one pass open, whose scratch is everything below. */
    NYA_UI* open;
    u64     pass_serial;
    b8      registered;

    /* This pass's input. All false in a draw pass except what feedback reads. */

    b8    confirm;
    b8    cancel;
    b8    confirm_down;
    b8    directions[4];
    f32x2 pointer;
    b8    pointer_moved;
    b8    pointer_pressed;
    b8    pointer_down;
    b8    pointer_released;

    /** Directions are worked out once per tick for every window, so a second input pass cannot repeat twice as fast. */
    u64 direction_tick;
    u32 repeat_direction;
    f32 repeat_s;

    u64 widgets[NYA_UI_WIDGETS_MAX];
    u32 widget_count;
    u32 widget_count_worst;
    u32 focus_found;

    _NYA_UILayout layouts[NYA_UI_DEPTH_MAX];
    u32           depth;
    NYA_Rectf     last_rect;

    _NYA_UIPanelState panels[NYA_UI_PANELS_MAX];
    u32               panel_count;
} _NYA_UISystem;

NYA_INTERNAL _NYA_UISystem _nya_ui = { .direction_tick = U64_MAX, .repeat_direction = U32_MAX };

/** In the order of _NYA_UISystem.directions. */
NYA_INTERNAL const NYA_InputAction _NYA_UI_DIRECTIONS[4] = { NYA_INPUT_ACTION_UP, NYA_INPUT_ACTION_DOWN, NYA_INPUT_ACTION_LEFT, NYA_INPUT_ACTION_RIGHT };

enum { _NYA_UI_UP = 0, _NYA_UI_DOWN, _NYA_UI_LEFT, _NYA_UI_RIGHT };

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API DECLARATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/** The window's persistent state, reset when its slot now holds a different window. */
NYA_INTERNAL NYA_UI* _nya_ui_context(const NYA_Window* window);

NYA_INTERNAL NYA_UIStyle _nya_ui_style_resolve(NYA_UIStyle style);

/** Reads confirm, cancel, the pointer and the four directions with their hold repeat. */
NYA_INTERNAL void _nya_ui_input_read(NYA_UIPass pass);

/** `label` hashed into `scope`. Never zero, which means no widget. */
NYA_INTERNAL u64 _nya_ui_id(u64 scope, NYA_ConstCString label);

/** The panel slot for `id`: its own, a free one, or the stalest one not opened this pass. U32_MAX when none is left. */
NYA_INTERNAL u32 _nya_ui_panel_claim(u64 id);

NYA_INTERNAL _NYA_UILayout* _nya_ui_layout_push(void);

/** Takes room for something `natural` wide and high, filling the column or cell when `fill`. */
NYA_INTERNAL NYA_Rectf _nya_ui_place(f32x2 natural, b8 fill);

/** Registers a focusable widget and works out focus, pointer and activation for it. */
NYA_INTERNAL _NYA_UIWidget _nya_ui_widget(NYA_UI* ui, NYA_ConstCString label, NYA_Rectf rect);

NYA_INTERNAL void _nya_ui_focus_set(NYA_UI* ui, u64 id);

NYA_INTERNAL b8 _nya_ui_drawing(void);

NYA_INTERNAL f32 _nya_ui_item_height(const NYA_UI* ui, const _NYA_UILayout* layout);

/** The shadow, fill and outline of a widget, popped when newly focused and sunk when held. Returns the body. */
NYA_INTERNAL NYA_Rectf _nya_ui_body_draw(NYA_UI* ui, NYA_Rectf rect, _NYA_UIWidget widget, NYA_Color fill);

/** `text`, measured `width` wide, centred vertically in `rect` and placed across it by `align`. */
NYA_INTERNAL void _nya_ui_text_draw(NYA_UI* ui, const _NYA_UILayout* layout, NYA_ConstCString text, f32 width, NYA_Rectf rect, NYA_UIAlign align, NYA_Color color);

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PASSES
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

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

    // by name every pass, since the registry can change under a hot reload.
    ui->font       = nya_font_resolve(ui->style.font[0] != '\0' ? nya_font_named(ui->style.font) : NYA_FONT_NONE);
    ui->title_font = nya_font_resolve(ui->style.title_font[0] != '\0' ? nya_font_named(ui->style.title_font) : NYA_FONT_NONE);

    _nya_ui.open         = ui;
    _nya_ui.pass_serial += 1;
    _nya_ui.widget_count = 0;
    _nya_ui.focus_found  = U32_MAX;
    _nya_ui.depth        = 0;
    _nya_ui.last_rect    = (NYA_Rectf){ 0 };

    _nya_ui_input_read(pass);

    f32 line_height = nya_font_metrics(ui->font).line_height;

    _NYA_UILayout* root = _nya_ui_layout_push();
    *root               = (_NYA_UILayout){
        .width       = (f32)window->screen_width,
        .font        = ui->font,
        .line_height = line_height,
        .scope       = (u64)window->handle.index + 1,
        .panel       = U32_MAX,
        .hidden      = line_height <= 0.0F,
    };

    return ui;
}

void nya_ui_end(NYA_UI* ui) {
    nya_assert(ui != nullptr && ui == _nya_ui.open);
    nya_assert(_nya_ui.depth == 1, "%u panels or rows are still open", _nya_ui.depth - 1);

    u32 count = _nya_ui.widget_count;

    if (count > 0) {
        // a focused widget that vanished, as every label does when the locale changes, hands focus to its position.
        u32 index = _nya_ui.focus_found != U32_MAX ? _nya_ui.focus_found : nya_min(ui->focus_index, count - 1);

        // adding count - 1 wraps upward without an unsigned 0 - 1.
        if (_nya_ui.directions[_NYA_UI_UP]) index = (index + count - 1) % count;
        if (_nya_ui.directions[_NYA_UI_DOWN]) index = (index + 1) % count;

        _nya_ui_focus_set(ui, _nya_ui.widgets[index]);
        ui->focus_index = index;
    }

    if (ui->pass == NYA_UI_PASS_INPUT && !_nya_ui.pointer_down) {
        ui->active   = 0;
        ui->dragging = false;
    }

    _nya_ui.widget_count_worst = nya_max(_nya_ui.widget_count_worst, count);
    _nya_ui.depth              = 0;
    _nya_ui.open               = nullptr;
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * LAYOUT
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

b8 nya_ui_panel_begin(NYA_UI* ui, NYA_ConstCString id, NYA_UIPanel panel) {
    nya_assert(ui != nullptr && ui == _nya_ui.open);
    nya_assert(id != nullptr);
    nya_assert(panel.anchor < NYA_UI_ANCHOR_COUNT && panel.align < NYA_UI_ALIGN_COUNT);

    const _NYA_UILayout* parent = &_nya_ui.layouts[_nya_ui.depth - 1];

    u64 key   = _nya_ui_id(parent->scope, id);
    u32 index = _nya_ui_panel_claim(key);
    if (index == U32_MAX) return false;

    _NYA_UIPanelState* state = &_nya_ui.panels[index];
    state->pass              = _nya_ui.pass_serial;

    const NYA_UIStyle* style = &ui->style;

    NYA_Font font        = nya_font_valid(panel.font) ? panel.font : parent->font;
    f32      line_height = nya_font_valid(panel.font) ? nya_font_metrics(font).line_height : parent->line_height;
    f32      frame       = panel.frameless ? 0.0F : style->padding + style->outline;

    f32 header      = 0.0F;
    f32 title_width = 0.0F;

    if (panel.title != nullptr) {
        header      = nya_font_metrics(ui->title_font).line_height + style->spacing;
        title_width = nya_font_width(ui->title_font, panel.title);
    }

    f32x2 size = {
        panel.width > 0.0F ? panel.width : state->size.x,
        panel.height > 0.0F ? panel.height : state->size.y,
    };

    NYA_Rectf bounds;

    if (_nya_ui.depth == 1) {
        // 0, 0.5 or 1 across and down, so one expression anchors to either edge or the middle.
        u32 column = panel.anchor % 3;
        u32 row    = panel.anchor / 3;
        f32 across = (f32)column * 0.5F;
        f32 down   = (f32)row * 0.5F;

        bounds = (NYA_Rectf){
            .x      = (((f32)ui->window->screen_width - size.x) * across) + (panel.offset.x * (1.0F - (across * 2.0F))),
            .y      = (((f32)ui->window->screen_height - size.y) * down) + (panel.offset.y * (1.0F - (down * 2.0F))),
            .width  = size.x,
            .height = size.y,
        };
    } else {
        bounds = _nya_ui_place(size, false);
    }

    _nya_ui.last_rect = bounds;

    _NYA_UILayout* layout = _nya_ui_layout_push();
    *layout               = (_NYA_UILayout){
        .origin      = { bounds.x + frame, bounds.y + frame + header },
        .width       = nya_max(size.x - (frame * 2.0F), 0.0F),
        .align       = panel.align,
        .font        = font,
        .line_height = line_height,
        .scope       = key,
        .panel       = index,
        .options     = panel,
        .bounds      = bounds,
        .frame       = frame,
        .header      = header,
        .title_width = title_width,
        .hidden      = parent->hidden || !state->measured || line_height <= 0.0F,
    };

    if (!_nya_ui_drawing()) return true;

    NYA_Window* window = ui->window;

    if (!panel.frameless) {
        nya_render2d_rect_rounded(window, bounds.x, bounds.y + style->depth, bounds.width, bounds.height, style->radius, style->ink);
        nya_render2d_rect_rounded(window, bounds.x, bounds.y, bounds.width, bounds.height, style->radius, style->panel);
        nya_render2d_rect_rounded_outline(window, bounds.x, bounds.y, bounds.width, bounds.height, style->radius, style->outline, style->ink);
    }

    if (panel.title != nullptr) {
        f32 x = bounds.x + ((bounds.width - title_width) * 0.5F);
        f32 y = bounds.y + frame;

        // a hard offset shadow in ink, the cartoon way.
        nya_font_draw(window, ui->title_font, panel.title, x, y + (style->depth * 0.5F), style->ink);
        nya_font_draw(window, ui->title_font, panel.title, x, y, style->accent);
    }

    return true;
}

void nya_ui_panel_end(NYA_UI* ui) {
    nya_assert(ui != nullptr && ui == _nya_ui.open);
    nya_assert(_nya_ui.depth > 1);

    const _NYA_UILayout* layout = &_nya_ui.layouts[_nya_ui.depth - 1];
    nya_assert(layout->panel != U32_MAX, "a row is open; close it with nya_ui_row_end");

    _nya_ui.depth -= 1;

    f32x2 natural = {
        nya_max(layout->used.x, layout->title_width) + (layout->frame * 2.0F),
        layout->header + layout->used.y + (layout->frame * 2.0F),
    };

    f32x2 size = {
        layout->options.width > 0.0F ? layout->options.width : natural.x,
        layout->options.height > 0.0F ? layout->options.height : natural.y,
    };

    // a face still loading measures zero, and that size must not stick.
    _NYA_UIPanelState* state = &_nya_ui.panels[layout->panel];
    state->size              = size;
    state->measured          = layout->line_height > 0.0F;

    _nya_ui.last_rect = (NYA_Rectf){ layout->bounds.x, layout->bounds.y, size.x, size.y };
}

void nya_ui_row_begin(NYA_UI* ui, u32 columns) {
    nya_assert(ui != nullptr && ui == _nya_ui.open);
    nya_assert(columns > 0);

    const _NYA_UILayout* parent = &_nya_ui.layouts[_nya_ui.depth - 1];
    nya_assert(parent->columns == 0, "a row cannot open inside a row");

    f32 gap = parent->count > 0 ? ui->style.spacing : 0.0F;

    _NYA_UILayout* layout = _nya_ui_layout_push();
    *layout               = (_NYA_UILayout){
        .origin      = { parent->origin.x, parent->origin.y + parent->used.y + gap },
        .width       = parent->width,
        .columns     = columns,
        .align       = parent->align,
        .font        = parent->font,
        .line_height = parent->line_height,
        .scope       = parent->scope,
        .panel       = U32_MAX,
        .hidden      = parent->hidden,
    };
}

void nya_ui_row_end(NYA_UI* ui) {
    nya_assert(ui != nullptr && ui == _nya_ui.open);
    nya_assert(_nya_ui.depth > 1 && _nya_ui.layouts[_nya_ui.depth - 1].columns > 0, "no row is open");

    f32x2 used     = _nya_ui.layouts[_nya_ui.depth - 1].used;
    _nya_ui.depth -= 1;

    // filling, so a fixed column hands the row its whole width at its left edge, where the cells already went.
    (void)_nya_ui_place(used, true);
}

NYA_Rectf nya_ui_space(NYA_UI* ui, f32 width, f32 height) {
    nya_assert(ui != nullptr && ui == _nya_ui.open);
    nya_assert(width >= 0.0F && height >= 0.0F);

    return _nya_ui_place((f32x2){ width, height }, width == 0.0F);
}

void nya_ui_scrim(NYA_UI* ui) {
    nya_assert(ui != nullptr && ui == _nya_ui.open);

    if (ui->pass != NYA_UI_PASS_DRAW) return;

    nya_render2d_rect(ui->window, 0.0F, 0.0F, (f32)ui->window->screen_width, (f32)ui->window->screen_height, ui->style.scrim);
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * WIDGETS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

void nya_ui_label(NYA_UI* ui, NYA_ConstCString text) __attr_overloaded {
    nya_assert(ui != nullptr);

    nya_ui_label(ui, text, ui->style.text);
}

void nya_ui_label(NYA_UI* ui, NYA_ConstCString text, NYA_Color color) __attr_overloaded {
    nya_assert(ui != nullptr && ui == _nya_ui.open);
    nya_assert(text != nullptr);

    const _NYA_UILayout* layout = &_nya_ui.layouts[_nya_ui.depth - 1];

    NYA_Rectf rect = _nya_ui_place((f32x2){ nya_font_width(layout->font, text), layout->line_height }, false);

    if (_nya_ui_drawing()) nya_font_draw(ui->window, layout->font, text, rect.x, rect.y, color);
}

b8 nya_ui_button(NYA_UI* ui, NYA_ConstCString label) {
    nya_assert(ui != nullptr && ui == _nya_ui.open);
    nya_assert(label != nullptr);

    const _NYA_UILayout* layout = &_nya_ui.layouts[_nya_ui.depth - 1];
    const NYA_UIStyle*   style  = &ui->style;

    f32       text_width = nya_font_width(layout->font, label);
    NYA_Rectf rect       = _nya_ui_place((f32x2){ text_width + (style->padding * 2.0F), _nya_ui_item_height(ui, layout) }, true);

    _NYA_UIWidget widget = _nya_ui_widget(ui, label, rect);
    if (widget.refused) return false;

    if (_nya_ui_drawing()) {
        NYA_Rectf body = _nya_ui_body_draw(ui, rect, widget, widget.focused ? style->accent : style->button);
        _nya_ui_text_draw(ui, layout, label, text_width, body, NYA_UI_ALIGN_CENTER, widget.focused ? style->accent_text : style->text);
    }

    return widget.activated;
}

b8 nya_ui_selectable(NYA_UI* ui, NYA_ConstCString label, b8 selected) {
    nya_assert(ui != nullptr && ui == _nya_ui.open);
    nya_assert(label != nullptr);

    const _NYA_UILayout* layout = &_nya_ui.layouts[_nya_ui.depth - 1];
    const NYA_UIStyle*   style  = &ui->style;

    f32 height = _nya_ui_item_height(ui, layout);
    f32 dot    = height * 0.14F;

    f32       text_width = nya_font_width(layout->font, label);
    NYA_Rectf rect       = _nya_ui_place((f32x2){ text_width + (style->padding * 3.0F) + (dot * 2.0F), height }, true);

    _NYA_UIWidget widget = _nya_ui_widget(ui, label, rect);
    if (widget.refused) return false;

    if (_nya_ui_drawing()) {
        NYA_Color fill = widget.focused ? style->accent : style->button;
        NYA_Rectf body = _nya_ui_body_draw(ui, rect, widget, fill);

        f32x2 center = { body.x + style->padding + dot, body.y + (body.height * 0.5F) };

        // an empty ring when not chosen, so the list reads as a choice even before anything is picked.
        nya_render2d_circle(ui->window, center, dot, style->ink);
        if (!selected) nya_render2d_circle(ui->window, center, dot - (style->outline * 0.6F), fill);

        NYA_Rectf text = { body.x + (dot * 2.0F) + style->padding, body.y, body.width - (dot * 2.0F) - style->padding, body.height };
        _nya_ui_text_draw(ui, layout, label, text_width, text, NYA_UI_ALIGN_CENTER, widget.focused ? style->accent_text : style->text);
    }

    return widget.activated;
}

b8 nya_ui_toggle(NYA_UI* ui, NYA_ConstCString label, b8* value) {
    nya_assert(ui != nullptr && ui == _nya_ui.open);
    nya_assert(label != nullptr && value != nullptr);

    const _NYA_UILayout* layout = &_nya_ui.layouts[_nya_ui.depth - 1];
    const NYA_UIStyle*   style  = &ui->style;

    f32 height      = _nya_ui_item_height(ui, layout);
    f32 pill_height = height * 0.5F;
    f32 pill_width  = pill_height * 1.8F;

    f32       text_width = nya_font_width(layout->font, label);
    NYA_Rectf rect       = _nya_ui_place((f32x2){ text_width + (style->padding * 3.0F) + pill_width, height }, true);

    _NYA_UIWidget widget = _nya_ui_widget(ui, label, rect);
    if (widget.refused) return false;

    b8 before = *value;

    if (widget.activated) *value = !*value;
    if (widget.focused && _nya_ui.directions[_NYA_UI_LEFT]) *value = false;
    if (widget.focused && _nya_ui.directions[_NYA_UI_RIGHT]) *value = true;

    if (_nya_ui_drawing()) {
        NYA_Color fill = widget.focused ? style->accent : style->button;
        NYA_Rectf body = _nya_ui_body_draw(ui, rect, widget, fill);

        _nya_ui_text_draw(ui, layout, label, text_width, (NYA_Rectf){ body.x + style->padding, body.y, body.width, body.height }, NYA_UI_ALIGN_START,
                          widget.focused ? style->accent_text : style->text);

        f32 x = body.x + body.width - style->padding - pill_width;
        f32 y = body.y + ((body.height - pill_height) * 0.5F);

        // on reads as the accent, or as ink on a row the accent already fills.
        NYA_Color on = widget.focused ? style->ink : style->accent;

        nya_render2d_rect_rounded(ui->window, x, y, pill_width, pill_height, pill_height * 0.5F, *value ? on : style->panel);
        nya_render2d_rect_rounded_outline(ui->window, x, y, pill_width, pill_height, pill_height * 0.5F, style->outline * 0.7F, style->ink);

        f32   knob   = (pill_height * 0.5F) - style->outline;
        f32x2 center = { *value ? x + pill_width - (pill_height * 0.5F) : x + (pill_height * 0.5F), y + (pill_height * 0.5F) };
        nya_render2d_circle(ui->window, center, knob, *value ? style->button : style->ink);
    }

    return *value != before;
}

b8 nya_ui_slider(NYA_UI* ui, NYA_ConstCString label, f32* value, f32 min, f32 max, f32 step) {
    nya_assert(ui != nullptr && ui == _nya_ui.open);
    nya_assert(label != nullptr && value != nullptr);
    nya_assert(max > min && step >= 0.0F, "a slider needs min below max and a step of zero or more");

    const _NYA_UILayout* layout = &_nya_ui.layouts[_nya_ui.depth - 1];
    const NYA_UIStyle*   style  = &ui->style;

    f32 height = _nya_ui_item_height(ui, layout);
    f32 knob   = height * 0.2F;

    f32       text_width = nya_font_width(layout->font, label);
    NYA_Rectf rect       = _nya_ui_place((f32x2){ text_width + (style->padding * 3.0F) + (height * 3.0F), height }, true);

    // the right part of the row, past the label, inset by the knob so it can reach both ends.
    f32 track_x     = rect.x + nya_max(rect.width * 0.5F, text_width + (style->padding * 2.0F)) + knob;
    f32 track_width = nya_max((rect.x + rect.width - style->padding - knob) - track_x, 1.0F);

    _NYA_UIWidget widget = _nya_ui_widget(ui, label, rect);
    if (widget.refused) return false;

    f32 before = *value;

    if (ui->pass == NYA_UI_PASS_INPUT) {
        if (widget.focused && _nya_ui.directions[_NYA_UI_LEFT]) *value -= step;
        if (widget.focused && _nya_ui.directions[_NYA_UI_RIGHT]) *value += step;

        // a press past the label grabs the knob; one on the label only focuses the row.
        if (_nya_ui.pointer_pressed && ui->active == widget.id && _nya_ui.pointer.x >= track_x - (knob * 2.0F)) ui->dragging = true;

        if (ui->active == widget.id && ui->dragging && _nya_ui.pointer_down) {
            f32 t  = nya_clamp((_nya_ui.pointer.x - track_x) / track_width, 0.0F, 1.0F);
            *value = min + (t * (max - min));

            if (step > 0.0F) *value = min + (roundf((*value - min) / step) * step);
        }

        *value = nya_clamp(*value, min, max);
    }

    if (_nya_ui_drawing()) {
        NYA_Color fill = widget.focused ? style->accent : style->button;
        NYA_Rectf body = _nya_ui_body_draw(ui, rect, widget, fill);

        _nya_ui_text_draw(ui, layout, label, text_width, (NYA_Rectf){ body.x + style->padding, body.y, body.width, body.height }, NYA_UI_ALIGN_START,
                          widget.focused ? style->accent_text : style->text);

        f32 track_height = knob;
        f32 x            = track_x + (body.x - rect.x);
        f32 y            = body.y + ((body.height - track_height) * 0.5F);
        f32 t            = nya_clamp((*value - min) / (max - min), 0.0F, 1.0F);

        nya_render2d_rect_rounded(ui->window, x, y, track_width, track_height, track_height * 0.5F, style->panel);
        nya_render2d_rect_rounded(ui->window, x, y, track_width * t, track_height, track_height * 0.5F, widget.focused ? style->ink : style->accent);
        nya_render2d_rect_rounded_outline(ui->window, x, y, track_width, track_height, track_height * 0.5F, style->outline * 0.6F, style->ink);

        f32x2 center = { x + (track_width * t), y + (track_height * 0.5F) };
        nya_render2d_circle(ui->window, center, knob + style->outline, style->ink);
        nya_render2d_circle(ui->window, center, knob, style->button);
    }

    return *value != before;
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * STATE
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

b8 nya_ui_cancelled(const NYA_UI* ui) {
    nya_assert(ui != nullptr && ui == _nya_ui.open);

    return _nya_ui.cancel;
}

NYA_Rectf nya_ui_last_rect(const NYA_UI* ui) {
    nya_assert(ui != nullptr && ui == _nya_ui.open);

    return _nya_ui.last_rect;
}

void nya_ui_focus_reset(NYA_Window* window) {
    nya_assert(window != nullptr);

    NYA_UI* ui      = _nya_ui_context(window);
    ui->focus       = 0;
    ui->focus_index = 0;
    ui->active      = 0;
    ui->dragging    = false;
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

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * STYLE
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

void nya_ui_style_set(NYA_Window* window, NYA_UIStyle style) {
    nya_assert(window != nullptr);

    _nya_ui_context(window)->style = _nya_ui_style_resolve(style);
}

NYA_UIStyle nya_ui_style_get(const NYA_Window* window) {
    nya_assert(window != nullptr);

    return _nya_ui_context(window)->style;
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

NYA_UI* _nya_ui_context(const NYA_Window* window) {
    nya_assert(window->handle.index < NYA_WINDOW_MAX);

    NYA_UI* ui = &_nya_ui.windows[window->handle.index];

    b8 same = ui->claimed && ui->handle.generation == window->handle.generation;
    if (!same) *ui = (NYA_UI){ .claimed = true, .handle = window->handle, .style = _nya_ui_style_resolve((NYA_UIStyle){ 0 }) };

    return ui;
}

NYA_UIStyle _nya_ui_style_resolve(NYA_UIStyle style) {
    style.font[NYA_UI_FONT_NAME_MAX - 1]       = '\0';
    style.title_font[NYA_UI_FONT_NAME_MAX - 1] = '\0';

    if (style.padding <= 0.0F) style.padding = NYA_UI_PADDING;
    if (style.spacing <= 0.0F) style.spacing = NYA_UI_SPACING;
    if (style.radius <= 0.0F) style.radius = NYA_UI_RADIUS;
    if (style.outline <= 0.0F) style.outline = NYA_UI_OUTLINE;
    if (style.depth <= 0.0F) style.depth = NYA_UI_DEPTH;
    if (style.pop <= 0.0F) style.pop = NYA_UI_POP;
    if (style.item_height < 0.0F) style.item_height = 0.0F;

    struct {
        NYA_Color* color;
        NYA_Color  fallback;
    } colors[] = {
        { &style.scrim,       NYA_UI_SCRIM       },
        { &style.panel,       NYA_UI_PANEL       },
        { &style.ink,         NYA_UI_INK         },
        { &style.text,        NYA_UI_TEXT        },
        { &style.text_dim,    NYA_UI_TEXT_DIM    },
        { &style.button,      NYA_UI_BUTTON      },
        { &style.accent,      NYA_UI_ACCENT      },
        { &style.accent_text, NYA_UI_ACCENT_TEXT },
    };

    for (u32 i = 0; i < nya_carray_length(colors); i++) {
        NYA_Color c = *colors[i].color;
        if (c.r == 0.0F && c.g == 0.0F && c.b == 0.0F && c.a == 0.0F) *colors[i].color = colors[i].fallback;
    }

    return style;
}

void _nya_ui_input_read(NYA_UIPass pass) {
    _nya_ui.pointer      = nya_input_mouse_position();
    _nya_ui.pointer_down = nya_input_mouse_button_pressed(NYA_MOUSE_BUTTON_LEFT);
    _nya_ui.confirm_down = nya_input_action_pressed(NYA_INPUT_ACTION_CONFIRM);

    b8 input = pass == NYA_UI_PASS_INPUT;

    f32x2 delta              = nya_input_mouse_position_delta();
    _nya_ui.confirm          = input && nya_input_action_just_pressed(NYA_INPUT_ACTION_CONFIRM);
    _nya_ui.cancel           = input && nya_input_action_just_pressed(NYA_INPUT_ACTION_CANCEL);
    _nya_ui.pointer_moved    = input && (delta.x != 0.0F || delta.y != 0.0F);
    _nya_ui.pointer_pressed  = input && nya_input_mouse_button_just_pressed(NYA_MOUSE_BUTTON_LEFT);
    _nya_ui.pointer_released = input && nya_input_mouse_button_just_released(NYA_MOUSE_BUTTON_LEFT);

    if (!input) {
        nya_memset(_nya_ui.directions, 0, sizeof(_nya_ui.directions));
        return;
    }

    u64 tick = nya_world()->sim_system.tick;
    if (tick == _nya_ui.direction_tick) return;

    _nya_ui.direction_tick = tick;
    nya_memset(_nya_ui.directions, 0, sizeof(_nya_ui.directions));

    for (u32 i = 0; i < nya_carray_length(_NYA_UI_DIRECTIONS); i++) {
        if (!nya_input_action_just_pressed(_NYA_UI_DIRECTIONS[i])) continue;

        _nya_ui.directions[i]    = true;
        _nya_ui.repeat_direction = i;
        _nya_ui.repeat_s         = -NYA_UI_REPEAT_DELAY_S;
    }

    u32 held = _nya_ui.repeat_direction;
    if (held == U32_MAX || _nya_ui.directions[held]) return;

    if (!nya_input_action_pressed(_NYA_UI_DIRECTIONS[held])) {
        _nya_ui.repeat_direction = U32_MAX;
        return;
    }

    _nya_ui.repeat_s += nya_app_get()->frame_stats.delta_time_s;
    if (_nya_ui.repeat_s < NYA_UI_REPEAT_INTERVAL_S) return;

    _nya_ui.repeat_s        -= NYA_UI_REPEAT_INTERVAL_S;
    _nya_ui.directions[held] = true;
}

u64 _nya_ui_id(u64 scope, NYA_ConstCString label) {
    u64 parts[2] = { scope, nya_hash_fnv1a(label) };
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

_NYA_UILayout* _nya_ui_layout_push(void) {
    nya_assert(_nya_ui.depth < NYA_UI_DEPTH_MAX, "panels and rows nest deeper than NYA_UI_DEPTH_MAX");

    return &_nya_ui.layouts[_nya_ui.depth++];
}

NYA_Rectf _nya_ui_place(f32x2 natural, b8 fill) {
    _NYA_UILayout* layout  = &_nya_ui.layouts[_nya_ui.depth - 1];
    f32            spacing = _nya_ui.open->style.spacing;
    f32            gap     = layout->count > 0 ? spacing : 0.0F;
    f32            factor  = (f32)layout->align * 0.5F;

    NYA_Rectf rect;

    if (layout->columns > 0) {
        nya_assert(layout->count < layout->columns, "a row of %u columns got another widget", layout->columns);

        if (layout->width > 0.0F) {
            f32 cell  = (layout->width - (spacing * (f32)(layout->columns - 1))) / (f32)layout->columns;
            f32 width = fill ? cell : nya_min(natural.x, cell);
            f32 x     = layout->origin.x + ((cell + spacing) * (f32)layout->count);

            rect = (NYA_Rectf){ x + ((cell - width) * factor), layout->origin.y, width, natural.y };
        } else {
            rect = (NYA_Rectf){ layout->origin.x + layout->used.x + gap, layout->origin.y, natural.x, natural.y };
        }

        layout->used.x += gap + natural.x;
        layout->used.y  = nya_max(layout->used.y, natural.y);
    } else {
        f32 width = fill && layout->width > 0.0F ? layout->width : natural.x;
        f32 x     = layout->origin.x + (layout->width > 0.0F ? (layout->width - width) * factor : 0.0F);

        rect = (NYA_Rectf){ x, layout->origin.y + layout->used.y + gap, width, natural.y };

        layout->used.x  = nya_max(layout->used.x, natural.x);
        layout->used.y += gap + natural.y;
    }

    layout->count     += 1;
    _nya_ui.last_rect  = rect;

    return rect;
}

_NYA_UIWidget _nya_ui_widget(NYA_UI* ui, NYA_ConstCString label, NYA_Rectf rect) {
    if (_nya_ui.widget_count >= NYA_UI_WIDGETS_MAX) return (_NYA_UIWidget){ .refused = true };

    u64 id = _nya_ui_id(_nya_ui.layouts[_nya_ui.depth - 1].scope, label);

    if (ui->pass == NYA_UI_PASS_INPUT) {
        for (u32 i = 0; i < _nya_ui.widget_count; i++) {
            nya_assert(_nya_ui.widgets[i] != id, "two widgets in one panel share the label '%s'", label);
        }
    }

    // an empty focus goes to the first widget as it is declared, so a menu's first pass can already confirm.
    if (ui->focus == 0) _nya_ui_focus_set(ui, id);

    u32 index                         = _nya_ui.widget_count++;
    _nya_ui.widgets[index]            = id;
    if (ui->focus == id) _nya_ui.focus_found = index;

    _NYA_UIWidget widget = { .id = id };

    if (ui->pass == NYA_UI_PASS_INPUT) {
        b8 inside = nya_rect_contains(rect, _nya_ui.pointer);

        // hover moves the same focus as the keys, so the two never disagree. only on movement, so a resting pointer
        // does not take focus back from the keys.
        if (inside && (_nya_ui.pointer_moved || _nya_ui.pointer_pressed)) {
            _nya_ui_focus_set(ui, id);
            _nya_ui.focus_found = index;
        }

        if (inside && _nya_ui.pointer_pressed) ui->active = id;

        widget.activated = (ui->focus == id && _nya_ui.confirm) || (ui->active == id && _nya_ui.pointer_released && inside);
    }

    widget.focused = ui->focus == id;
    widget.held    = (ui->active == id && _nya_ui.pointer_down) || (widget.focused && _nya_ui.confirm_down);

    return widget;
}

void _nya_ui_focus_set(NYA_UI* ui, u64 id) {
    if (ui->focus == id) return;

    ui->focus           = id;
    ui->focus_changed_s = nya_app_uptime_s();
}

b8 _nya_ui_drawing(void) {
    return _nya_ui.open->pass == NYA_UI_PASS_DRAW && !_nya_ui.layouts[_nya_ui.depth - 1].hidden;
}

f32 _nya_ui_item_height(const NYA_UI* ui, const _NYA_UILayout* layout) {
    return ui->style.item_height > 0.0F ? ui->style.item_height : layout->line_height + (ui->style.padding * 1.5F);
}

NYA_Rectf _nya_ui_body_draw(NYA_UI* ui, NYA_Rectf rect, _NYA_UIWidget widget, NYA_Color fill) {
    const NYA_UIStyle* style = &ui->style;

    if (widget.focused) {
        // grows at once and settles back, ease out.
        f32 t    = nya_clamp((f32)(nya_app_uptime_s() - ui->focus_changed_s) / NYA_UI_POP_S, 0.0F, 1.0F);
        f32 grow = style->pop * (1.0F - t) * (1.0F - t);

        rect = (NYA_Rectf){ rect.x - grow, rect.y - grow, rect.width + (grow * 2.0F), rect.height + (grow * 2.0F) };
    }

    // held, the body sinks onto its shadow.
    if (!widget.held) nya_render2d_rect_rounded(ui->window, rect.x, rect.y + style->depth, rect.width, rect.height, style->radius, style->ink);
    if (widget.held) rect.y += style->depth;

    nya_render2d_rect_rounded(ui->window, rect.x, rect.y, rect.width, rect.height, style->radius, fill);
    nya_render2d_rect_rounded_outline(ui->window, rect.x, rect.y, rect.width, rect.height, style->radius, style->outline, style->ink);

    return rect;
}

void _nya_ui_text_draw(NYA_UI* ui, const _NYA_UILayout* layout, NYA_ConstCString text, f32 width, NYA_Rectf rect, NYA_UIAlign align, NYA_Color color) {
    f32 x = rect.x + ((rect.width - width) * (f32)align * 0.5F);
    f32 y     = rect.y + ((rect.height - layout->line_height) * 0.5F);

    nya_font_draw(ui->window, layout->font, text, x, y, color);
}
