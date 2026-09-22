/**
 * @file ui_present_record.c
 *
 * The presenter that draws nothing and remembers everything: one NYA_UIWidgetDraw per widget, in declaration
 * order, with the text copied. What makes a UI testable without a GPU, and what a program reaches for when it
 * wants to read its own menu rather than show it. See ui_present.h.
 *
 * It measures in cells rather than glyphs, so a recorded layout is exact arithmetic instead of whatever face
 * happened to be loaded, and two recordings of the same widget tree are comparable. That also makes it the closest
 * thing in the tree to how a terminal measures, which is the point: if a widget lays out here, it lays out there.
 * */
#include "nyangine/nyangine.h"

#include "nyangine/ui/ui_internal.h"


/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API DECLARATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

NYA_INTERNAL void  _nya_ui_record_look_build(void* state, u32 depth, const NYA_UIStyle* style, f32 scale, NYA_UILook* out);
NYA_INTERNAL void  _nya_ui_record_look_use(void* state, u32 depth);
NYA_INTERNAL f32x2 _nya_ui_record_measure(void* state, NYA_UIText role, NYA_ConstCString text, f32 room, NYA_UIOverflow overflow);
NYA_INTERNAL f32   _nya_ui_record_measure_bytes(void* state, NYA_UIText role, NYA_ConstCString text, u32 bytes);
NYA_INTERNAL void  _nya_ui_record_clip_set(void* state, NYA_Window* window, NYA_Rectf clip, b8 whole);
NYA_INTERNAL s32   _nya_ui_record_layer_get(void* state, NYA_Window* window);
NYA_INTERNAL void  _nya_ui_record_layer_set(void* state, NYA_Window* window, s32 layer);
NYA_INTERNAL void  _nya_ui_record_draw(void* state, NYA_Window* window, const NYA_UIWidgetDraw* widget);

/** Codepoints in the first `bytes` of `text`. Every byte that is not a UTF-8 continuation starts one. */
NYA_INTERNAL u32 _nya_ui_record_cells(NYA_ConstCString text, u32 bytes) __attr_no_discard;

/** `text` copied into the recorder's own bytes, since a label built on a caller's stack outlives nothing. "" when full. */
NYA_INTERNAL NYA_ConstCString _nya_ui_record_keep(NYA_UIRecorder* recorder, NYA_ConstCString text);

/** The strings inside `field` copied the same way. */
NYA_INTERNAL void _nya_ui_record_keep_field(NYA_UIRecorder* recorder, NYA_UIFieldDraw* field);


/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * RECORDING
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

void nya_ui_recorder_init(NYA_UIRecorder* recorder, f32x2 cell) {
    nya_assert(recorder != nullptr);
    nya_assert(cell.x >= 0.0F && cell.y >= 0.0F, "a cell has no negative side");

    if (cell.x <= 0.0F || cell.y <= 0.0F) cell = NYA_UI_RECORD_CELL;

    *recorder = (NYA_UIRecorder){
        .cell      = cell,
        .presenter = {
            .name          = "record",
            .state         = recorder,
            .look_build    = _nya_ui_record_look_build,
            .look_use      = _nya_ui_record_look_use,
            .measure       = _nya_ui_record_measure,
            .measure_bytes = _nya_ui_record_measure_bytes,
            .clip_set      = _nya_ui_record_clip_set,
            .layer_get     = _nya_ui_record_layer_get,
            .layer_set     = _nya_ui_record_layer_set,
            .draw          = _nya_ui_record_draw,
        },
    };
}

void nya_ui_recorder_deinit(NYA_UIRecorder* recorder) {
    nya_assert(recorder != nullptr);

    *recorder = (NYA_UIRecorder){ 0 };
}

void nya_ui_recorder_reset(NYA_UIRecorder* recorder) {
    nya_assert(recorder != nullptr);

    recorder->count     = 0;
    recorder->wanted    = 0;
    recorder->text_used = 0;
}

const NYA_UIPresenter* nya_ui_recorder_presenter(NYA_UIRecorder* recorder) {
    nya_assert(recorder != nullptr);
    nya_assert(recorder->presenter.draw == _nya_ui_record_draw, "a recorder is prepared by nya_ui_recorder_init before it presents anything");

    return &recorder->presenter;
}

u32 nya_ui_recorder_count(const NYA_UIRecorder* recorder) {
    nya_assert(recorder != nullptr);

    return recorder->count;
}

const NYA_UIWidgetDraw* nya_ui_recorder_at(const NYA_UIRecorder* recorder, u32 index) {
    nya_assert(recorder != nullptr);
    nya_assert(index < recorder->count, "widget %u of %u recorded", index, recorder->count);

    return &recorder->widgets[index];
}

const NYA_UIWidgetDraw* nya_ui_recorder_find(const NYA_UIRecorder* recorder, NYA_UIWidgetKind kind, NYA_ConstCString label) {
    nya_assert(recorder != nullptr);
    nya_assert(kind < NYA_UI_WIDGET_KIND_COUNT);

    for (u32 i = 0; i < recorder->count; i++) {
        const NYA_UIWidgetDraw* widget = &recorder->widgets[i];

        if (widget->kind != kind) continue;
        if (label != nullptr && strcmp(widget->label, label) != 0) continue;

        return widget;
    }

    return nullptr;
}

u32 nya_ui_recorder_write(const NYA_UIRecorder* recorder, char* out, u32 capacity) {
    nya_assert(recorder != nullptr && out != nullptr);
    nya_assert(capacity > 0, "a dump needs room for at least the terminator");

    u32 used = 0;
    out[0]   = '\0';

    for (u32 i = 0; i < recorder->count && used + 1 < capacity; i++) {
        const NYA_UIWidgetDraw* widget = &recorder->widgets[i];

        // the state as four letters, so a dump lines up and a diff of two passes shows what actually moved.
        char state[5] = {
            widget->state.disabled ? 'd' : '-',
            widget->state.focused ? 'f' : '-',
            widget->state.held ? 'h' : '-',
            widget->state.activated ? 'a' : '-',
            '\0',
        };

        s32 wrote = snprintf(out + used, capacity - used, "%-12s %4d,%-4d %4dx%-4d %s %s\n", nya_ui_widget_kind_name(widget->kind), (s32)widget->rect.x,
                             (s32)widget->rect.y, (s32)widget->rect.width, (s32)widget->rect.height, state, widget->label);

        if (wrote <= 0) break;

        // snprintf reports what it wanted, not what it wrote, and a truncated line is the last one either way.
        used += nya_min((u32)wrote, capacity - used - 1);
    }

    return used;
}


/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * INTERNAL
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

void _nya_ui_record_look_build(void* state, u32 depth, const NYA_UIStyle* style, f32 scale, NYA_UILook* out) {
    NYA_UIRecorder* recorder = state;

    nya_assert(recorder != nullptr && style != nullptr && out != nullptr);
    nya_assert(depth <= NYA_UI_STYLE_DEPTH_MAX, "a look is built at depth %u", depth);

    nya_ui_look_scale(style, scale, out);

    // one cell, whatever the role: a grid has one face and the type scale is a colour and a weight in it, not a size.
    for (u32 i = 0; i < NYA_UI_TEXT_COUNT; i++) out->line_heights[i] = recorder->cell.y;

    recorder->looks[depth] = *out;
}

void _nya_ui_record_look_use(void* state, u32 depth) {
    NYA_UIRecorder* recorder = state;

    nya_assert(recorder != nullptr);
    nya_assert(depth <= NYA_UI_STYLE_DEPTH_MAX, "a look at depth %u was selected", depth);

    recorder->depth = depth;
}

f32x2 _nya_ui_record_measure(void* state, NYA_UIText role, NYA_ConstCString text, f32 room, NYA_UIOverflow overflow) {
    NYA_UIRecorder* recorder = state;

    nya_assert(recorder != nullptr && text != nullptr);
    nya_assert(role < NYA_UI_TEXT_COUNT);

    f32 width = (f32)_nya_ui_record_cells(text, (u32)strlen(text)) * recorder->cell.x;

    if (overflow != NYA_UI_OVERFLOW_WRAP || room <= 0.0F || width <= room) return (f32x2){ width, recorder->cell.y };

    // whole cells across, so a wrapped line lands on the grid rather than between two columns of it.
    f32 columns = floorf(room / recorder->cell.x);
    if (columns < 1.0F) columns = 1.0F;

    f32 lines = ceilf(width / (columns * recorder->cell.x));

    return (f32x2){ columns * recorder->cell.x, lines * recorder->cell.y };
}

f32 _nya_ui_record_measure_bytes(void* state, NYA_UIText role, NYA_ConstCString text, u32 bytes) {
    NYA_UIRecorder* recorder = state;

    nya_assert(recorder != nullptr && text != nullptr);
    nya_assert(role < NYA_UI_TEXT_COUNT);

    return (f32)_nya_ui_record_cells(text, bytes) * recorder->cell.x;
}

void _nya_ui_record_clip_set(void* state, NYA_Window* window, NYA_Rectf clip, b8 whole) {
    NYA_UIRecorder* recorder = state;

    nya_unused(window, whole);
    nya_assert(recorder != nullptr);

    recorder->clip = clip;
}

s32 _nya_ui_record_layer_get(void* state, NYA_Window* window) {
    NYA_UIRecorder* recorder = state;

    nya_unused(window);
    nya_assert(recorder != nullptr);

    return recorder->layer;
}

void _nya_ui_record_layer_set(void* state, NYA_Window* window, s32 layer) {
    NYA_UIRecorder* recorder = state;

    nya_unused(window);
    nya_assert(recorder != nullptr);

    recorder->layer = layer;
}

void _nya_ui_record_draw(void* state, NYA_Window* window, const NYA_UIWidgetDraw* widget) {
    NYA_UIRecorder* recorder = state;

    nya_unused(window);
    nya_assert(recorder != nullptr && widget != nullptr);
    nya_assert(widget->kind < NYA_UI_WIDGET_KIND_COUNT);

    recorder->wanted += 1;

    // refused past the table rather than grown, like every other fixed table in the module.
    if (recorder->count >= NYA_UI_RECORD_MAX) return;

    NYA_UIWidgetDraw* kept = &recorder->widgets[recorder->count];

    *kept        = *widget;
    kept->label  = _nya_ui_record_keep(recorder, widget->label);

    switch (widget->kind) {
        case NYA_UI_WIDGET_DROPDOWN: kept->as_dropdown.shown = _nya_ui_record_keep(recorder, widget->as_dropdown.shown); break;
        case NYA_UI_WIDGET_FIELD:    _nya_ui_record_keep_field(recorder, &kept->as_field.field); break;

        case NYA_UI_WIDGET_COLOR_PICKER: _nya_ui_record_keep_field(recorder, &kept->as_picker.field); break;

        // the caller's own structs are borrowed for the length of the call, so what is kept is what outlives it.
        case NYA_UI_WIDGET_PANEL: kept->as_panel.options = nullptr; break;
        case NYA_UI_WIDGET_CHART: kept->as_chart.chart = nullptr; break;
        case NYA_UI_WIDGET_ICON:  kept->as_icon.icon = nullptr; break;

        default: break;
    }

    recorder->count += 1;
}

u32 _nya_ui_record_cells(NYA_ConstCString text, u32 bytes) {
    nya_assert(text != nullptr);

    u32 cells = 0;

    for (u32 i = 0; i < bytes && text[i] != '\0'; i++) {
        if (((u8)text[i] & 0xC0U) != 0x80U) cells += 1;
    }

    return cells;
}

NYA_ConstCString _nya_ui_record_keep(NYA_UIRecorder* recorder, NYA_ConstCString text) {
    nya_assert(recorder != nullptr);

    if (text == nullptr || text[0] == '\0') return "";

    u32 size = (u32)strlen(text) + 1;

    // the tail of a pass that overran is dropped rather than half copied, so nothing reads a cut string.
    if (recorder->text_used + size > NYA_UI_RECORD_TEXT_MAX) return "";

    char* kept = recorder->text + recorder->text_used;

    nya_memcpy(kept, text, size);
    recorder->text_used += size;

    return kept;
}

void _nya_ui_record_keep_field(NYA_UIRecorder* recorder, NYA_UIFieldDraw* field) {
    nya_assert(recorder != nullptr && field != nullptr);

    field->buffer    = _nya_ui_record_keep(recorder, field->buffer);
    field->composing = _nya_ui_record_keep(recorder, field->composing);
}
