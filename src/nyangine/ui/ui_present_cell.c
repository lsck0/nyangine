/**
 * @file ui_present_cell.c
 *
 * The presenter that draws a TUI: every widget as the characters a terminal has drawn it with since before there
 * were pixels. Nothing here decides what a widget *is*; it is handed one NYA_UIWidgetDraw at a time, as the shape
 * presenter beside it is, and decides what it looks like. See ui_present_cell.h.
 *
 * It writes into the caller's grid and nowhere else, which is the whole reason a TUI is checkable: the screen is a
 * value, and nya_ui_cells_present is the only call that puts it on a terminal.
 * */
#include "nyangine/nyangine.h"

#include "nyangine/ui/ui_internal.h"


/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * CONSTANTS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/** Bytes of one string this file will walk. A label longer than a screen is a caller mistake, not a measurement. */
#define _NYA_UI_CELL_TEXT_MAX 1024

/** Eighths of a cell the block elements divide a row into, which is what gives a chart more resolution than a row. */
#define _NYA_UI_CELL_EIGHTHS 8

/** The share of a picker's hue bar one row stands for, in degrees. */
#define _NYA_UI_CELL_HUE_SPAN 360.0F


/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * TYPES
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/** A rectangle in cells: where a widget actually lands, once the pixels it was laid out in have been divided. */
typedef struct {
    s32 column;
    s32 row;
    s32 columns;
    s32 rows;
} _NYA_UICellRect;

/**
 * Every character this file can draw, and its ASCII stand-in. One table, so "is there a fallback" is a question
 * with a table to look at rather than a promise: a glyph added without its ASCII column asserts the first time it
 * is drawn.
 * */
typedef enum {
    _NYA_UI_CELL_GLYPH_FRAME_TOP_LEFT = 0,
    _NYA_UI_CELL_GLYPH_FRAME_TOP_RIGHT,
    _NYA_UI_CELL_GLYPH_FRAME_BOTTOM_LEFT,
    _NYA_UI_CELL_GLYPH_FRAME_BOTTOM_RIGHT,
    _NYA_UI_CELL_GLYPH_FRAME_HORIZONTAL,
    _NYA_UI_CELL_GLYPH_FRAME_VERTICAL,

    /** The delimiters a widget is written between, and what they become when it has focus. */
    _NYA_UI_CELL_GLYPH_OPEN,
    _NYA_UI_CELL_GLYPH_CLOSE,
    _NYA_UI_CELL_GLYPH_FOCUS_OPEN,
    _NYA_UI_CELL_GLYPH_FOCUS_CLOSE,
    _NYA_UI_CELL_GLYPH_ROUND_OPEN,
    _NYA_UI_CELL_GLYPH_ROUND_CLOSE,

    _NYA_UI_CELL_GLYPH_ON,
    _NYA_UI_CELL_GLYPH_OFF,
    _NYA_UI_CELL_GLYPH_DOT,
    _NYA_UI_CELL_GLYPH_CHOSEN,

    _NYA_UI_CELL_GLYPH_TRACK,
    _NYA_UI_CELL_GLYPH_FILL,

    _NYA_UI_CELL_GLYPH_DOWN,
    _NYA_UI_CELL_GLYPH_UP,
    _NYA_UI_CELL_GLYPH_RIGHT,
    _NYA_UI_CELL_GLYPH_CROSS,
    _NYA_UI_CELL_GLYPH_MENU,
    _NYA_UI_CELL_GLYPH_GRIP,

    _NYA_UI_CELL_GLYPH_UNDERLINE,
    _NYA_UI_CELL_GLYPH_ELLIPSIS,
    _NYA_UI_CELL_GLYPH_ICON,
    _NYA_UI_CELL_GLYPH_CURSOR,

    /** The eighths, one to eight, which have to stay contiguous: a bar picks one by how full it is. */
    _NYA_UI_CELL_GLYPH_BLOCK_1,
    _NYA_UI_CELL_GLYPH_BLOCK_2,
    _NYA_UI_CELL_GLYPH_BLOCK_3,
    _NYA_UI_CELL_GLYPH_BLOCK_4,
    _NYA_UI_CELL_GLYPH_BLOCK_5,
    _NYA_UI_CELL_GLYPH_BLOCK_6,
    _NYA_UI_CELL_GLYPH_BLOCK_7,
    _NYA_UI_CELL_GLYPH_BLOCK_8,

    _NYA_UI_CELL_GLYPH_COUNT,
} _NYA_UICellGlyph;


/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * STATE
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/** The two columns of the table. Read through _nya_ui_cell_pick, which picks the one the grid was opened with. */
NYA_INTERNAL const u32 _NYA_UI_CELL_GLYPHS[_NYA_UI_CELL_GLYPH_COUNT][2] = {
    [_NYA_UI_CELL_GLYPH_FRAME_TOP_LEFT]     = { 0x250CU, '+' },
    [_NYA_UI_CELL_GLYPH_FRAME_TOP_RIGHT]    = { 0x2510U, '+' },
    [_NYA_UI_CELL_GLYPH_FRAME_BOTTOM_LEFT]  = { 0x2514U, '+' },
    [_NYA_UI_CELL_GLYPH_FRAME_BOTTOM_RIGHT] = { 0x2518U, '+' },
    [_NYA_UI_CELL_GLYPH_FRAME_HORIZONTAL]   = { 0x2500U, '-' },
    [_NYA_UI_CELL_GLYPH_FRAME_VERTICAL]     = { 0x2502U, '|' },

    [_NYA_UI_CELL_GLYPH_OPEN]        = { '[', '[' },
    [_NYA_UI_CELL_GLYPH_CLOSE]       = { ']', ']' },
    [_NYA_UI_CELL_GLYPH_FOCUS_OPEN]  = { 0x2039U, '<' },
    [_NYA_UI_CELL_GLYPH_FOCUS_CLOSE] = { 0x203AU, '>' },
    [_NYA_UI_CELL_GLYPH_ROUND_OPEN]  = { '(', '(' },
    [_NYA_UI_CELL_GLYPH_ROUND_CLOSE] = { ')', ')' },

    [_NYA_UI_CELL_GLYPH_ON]     = { 'x', 'x' },
    [_NYA_UI_CELL_GLYPH_OFF]    = { ' ', ' ' },
    [_NYA_UI_CELL_GLYPH_DOT]    = { 0x2022U, '*' },
    [_NYA_UI_CELL_GLYPH_CHOSEN] = { 0x2023U, '*' },

    [_NYA_UI_CELL_GLYPH_TRACK] = { 0x2591U, '-' },
    [_NYA_UI_CELL_GLYPH_FILL]  = { 0x2588U, '#' },

    [_NYA_UI_CELL_GLYPH_DOWN]  = { 0x25BEU, 'v' },
    [_NYA_UI_CELL_GLYPH_UP]    = { 0x25B4U, '^' },
    [_NYA_UI_CELL_GLYPH_RIGHT] = { 0x25B8U, '>' },
    [_NYA_UI_CELL_GLYPH_CROSS] = { 0x2715U, 'x' },
    [_NYA_UI_CELL_GLYPH_MENU]  = { 0x2261U, '=' },
    [_NYA_UI_CELL_GLYPH_GRIP]  = { 0x25E2U, '/' },

    [_NYA_UI_CELL_GLYPH_UNDERLINE] = { 0x2501U, '=' },
    [_NYA_UI_CELL_GLYPH_ELLIPSIS]  = { 0x2026U, '~' },
    [_NYA_UI_CELL_GLYPH_ICON]      = { 0x25AAU, '*' },
    [_NYA_UI_CELL_GLYPH_CURSOR]    = { '+', '+' },

    [_NYA_UI_CELL_GLYPH_BLOCK_1] = { 0x2581U, '_' },
    [_NYA_UI_CELL_GLYPH_BLOCK_2] = { 0x2582U, '_' },
    [_NYA_UI_CELL_GLYPH_BLOCK_3] = { 0x2583U, '-' },
    [_NYA_UI_CELL_GLYPH_BLOCK_4] = { 0x2584U, '-' },
    [_NYA_UI_CELL_GLYPH_BLOCK_5] = { 0x2585U, '=' },
    [_NYA_UI_CELL_GLYPH_BLOCK_6] = { 0x2586U, '=' },
    [_NYA_UI_CELL_GLYPH_BLOCK_7] = { 0x2587U, '#' },
    [_NYA_UI_CELL_GLYPH_BLOCK_8] = { 0x2588U, '#' },
};


/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API DECLARATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

NYA_INTERNAL void  _nya_ui_cell_look_build(void* state, u32 depth, const NYA_UIStyle* style, f32 scale, NYA_UILook* out);
NYA_INTERNAL void  _nya_ui_cell_look_use(void* state, u32 depth);
NYA_INTERNAL f32x2 _nya_ui_cell_measure(void* state, NYA_UIText role, NYA_ConstCString text, f32 room, NYA_UIOverflow overflow);
NYA_INTERNAL f32   _nya_ui_cell_measure_bytes(void* state, NYA_UIText role, NYA_ConstCString text, u32 bytes);
NYA_INTERNAL void  _nya_ui_cell_clip_set(void* state, NYA_Window* window, NYA_Rectf clip, b8 whole);
NYA_INTERNAL s32   _nya_ui_cell_layer_get(void* state, NYA_Window* window);
NYA_INTERNAL void  _nya_ui_cell_layer_set(void* state, NYA_Window* window, s32 layer);
NYA_INTERNAL void  _nya_ui_cell_draw(void* state, NYA_Window* window, const NYA_UIWidgetDraw* widget);

/** Code points in the first `bytes` of `text`, which in a monospace grid is its width in cells. */
NYA_INTERNAL u32 _nya_ui_cell_length(NYA_ConstCString text, u64 bytes) __attr_no_discard;

/** `value` pixels as whole cells: floored for a position, rounded but never to nothing for an extent. */
NYA_INTERNAL s32 _nya_ui_cell_floor(f32 value, f32 size) __attr_no_discard;
NYA_INTERNAL s32 _nya_ui_cell_span(f32 value, f32 size) __attr_no_discard;

/** `value` rounded to a whole number of `unit`s, which is how a pixel size becomes one the grid can hold exactly. */
NYA_INTERNAL f32 _nya_ui_cell_quantise(f32 value, f32 unit) __attr_no_discard;

/** A rectangle in window pixels as the cells it covers. */
NYA_INTERNAL _NYA_UICellRect _nya_ui_cell_rect(const NYA_UICells* cells, NYA_Rectf rect) __attr_no_discard;

/** The look at the depth the UI last selected, and the padding of that look in whole columns. */
NYA_INTERNAL const NYA_UILook* _nya_ui_cell_look(const NYA_UICells* cells) __attr_no_discard;
NYA_INTERNAL s32               _nya_ui_cell_padding(const NYA_UICells* cells) __attr_no_discard;

/** One glyph from the table, in the column the grid was opened with. */
NYA_INTERNAL u32 _nya_ui_cell_pick(const NYA_UICells* cells, _NYA_UICellGlyph glyph) __attr_no_discard;

/** `color` laid over the packed `base` by its own alpha. What a translucent fill does to the cell under it. */
NYA_INTERNAL u32 _nya_ui_cell_blend(u32 base, NYA_Color color) __attr_no_discard;

/** Which of a widget's four colours its state asks for, eased the way the shape presenter eases them. */
NYA_INTERNAL NYA_Color _nya_ui_cell_color(const NYA_UIStateColors* colors, const NYA_UIWidgetState* state) __attr_no_discard;

/**
 * The cell at `column`/`row` when a write at the current layer may have it, and null when it may not: outside the
 * grid, outside the clip, or already written by something on top. Claims the layer and grows the used extent.
 * */
NYA_INTERNAL NYA_TerminalCell* _nya_ui_cell_claim(NYA_UICells* cells, s32 column, s32 row) __attr_no_discard;

/** Paper across a rectangle, with `ink` for whatever is written on it later. A translucent paper keeps the glyphs under it. */
NYA_INTERNAL void _nya_ui_cell_fill(NYA_UICells* cells, _NYA_UICellRect rect, NYA_Color paper, NYA_Color ink, u8 attributes);

/** Adds `attributes` to the cells of `rect` that something has already written, leaving what they hold. */
NYA_INTERNAL void _nya_ui_cell_mark(NYA_UICells* cells, _NYA_UICellRect rect, u8 attributes);

/** One code point into one cell. */
NYA_INTERNAL void _nya_ui_cell_glyph(NYA_UICells* cells, s32 column, s32 row, u32 codepoint, NYA_Color color, u8 attributes);

/** The same code point across `count` cells, rightwards. */
NYA_INTERNAL void _nya_ui_cell_run(NYA_UICells* cells, s32 column, s32 row, s32 count, u32 codepoint, NYA_Color color, u8 attributes);

/** `text` from one cell rightwards, at most `room` cells and cut with an ellipsis when it does not fit. Cells written. */
NYA_INTERNAL s32 _nya_ui_cell_text(NYA_UICells* cells, s32 column, s32 row, NYA_ConstCString text, s32 room, NYA_Color color, u8 attributes);

/** `text` broken at `room` cells and placed by `align`, which is what a wrapped label is here. */
NYA_INTERNAL void _nya_ui_cell_wrap(NYA_UICells* cells, _NYA_UICellRect rect, NYA_ConstCString text, NYA_UIAlign align, NYA_Color color, u8 attributes);

/** The delimiters around a widget: square, or the angles that say it has focus. */
NYA_INTERNAL void _nya_ui_cell_delimit(NYA_UICells* cells, _NYA_UICellRect rect, b8 focused, b8 round, NYA_Color color, u8 attributes);

/** `codepoint` encoded as UTF-8 into `out`, which holds at least four bytes. How many it took. */
NYA_INTERNAL u32 _nya_ui_cell_encode(u32 codepoint, OUT u8* out) __attr_no_discard;

/* One per NYA_UIWidgetKind that needs more than a line, in the enum's order. */

NYA_INTERNAL void _nya_ui_cell_panel(NYA_UICells* cells, const NYA_UIWidgetDraw* widget, _NYA_UICellRect rect, u8 attributes);
NYA_INTERNAL void _nya_ui_cell_label(NYA_UICells* cells, const NYA_UIWidgetDraw* widget, _NYA_UICellRect rect, u8 attributes);
NYA_INTERNAL void _nya_ui_cell_choice(NYA_UICells* cells, const NYA_UIWidgetDraw* widget, _NYA_UICellRect rect, NYA_Color ink, u8 attributes);
NYA_INTERNAL void _nya_ui_cell_slider(NYA_UICells* cells, const NYA_UIWidgetDraw* widget, _NYA_UICellRect rect, NYA_Color ink, u8 attributes);
NYA_INTERNAL void _nya_ui_cell_dropdown(NYA_UICells* cells, const NYA_UIWidgetDraw* widget, _NYA_UICellRect rect, NYA_Color ink, u8 attributes);
NYA_INTERNAL void _nya_ui_cell_field(NYA_UICells* cells, const NYA_UIFieldDraw* field, NYA_Rectf box, NYA_Color ink, b8 focused, u8 attributes);
NYA_INTERNAL void _nya_ui_cell_picker(NYA_UICells* cells, const NYA_UIWidgetDraw* widget, _NYA_UICellRect rect, NYA_Color ink, u8 attributes);
NYA_INTERNAL void _nya_ui_cell_chart(NYA_UICells* cells, const NYA_UIWidgetDraw* widget, _NYA_UICellRect rect, u8 attributes);
NYA_INTERNAL void _nya_ui_cell_mark_glyph(NYA_UICells* cells, NYA_UIMark mark, s32 column, s32 row, NYA_Color ink, u8 attributes);


/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * LIFETIME
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

void nya_ui_cells_init(NYA_UICells* cells, NYA_UICellOptions options) {
    nya_assert(cells != nullptr);
    nya_assert(options.cell.x >= 0.0F && options.cell.y >= 0.0F, "a cell has no negative side");

    if (options.cell.x <= 0.0F || options.cell.y <= 0.0F) options.cell = NYA_UI_CELL_SIZE;

    // padding is one number the layout uses on both axes, so it is only ever a whole cell in both when a cell is a
    // whole number of its own widths tall. Anything else would leave a widget half a column wide somewhere.
    nya_assert(fmodf(options.cell.y, options.cell.x) == 0.0F, "a cell %f wide and %f tall cannot hold a padding that is whole in both axes",
               (f64)options.cell.x, (f64)options.cell.y);

    // cleared in place rather than assigned from a literal: the grid is most of a megabyte, and a compound literal
    // of it is that megabyte on the stack before the copy.
    nya_memset(cells, 0, sizeof(*cells));

    cells->options   = options;
    cells->presenter = (NYA_UIPresenter){
        .name          = "cell",
        .state         = cells,
        .look_build    = _nya_ui_cell_look_build,
        .look_use      = _nya_ui_cell_look_use,
        .measure       = _nya_ui_cell_measure,
        .measure_bytes = _nya_ui_cell_measure_bytes,
        .clip_set      = _nya_ui_cell_clip_set,
        .layer_get     = _nya_ui_cell_layer_get,
        .layer_set     = _nya_ui_cell_layer_set,
        .draw          = _nya_ui_cell_draw,
    };

    nya_ui_cells_reset(cells);
}

void nya_ui_cells_deinit(NYA_UICells* cells) {
    nya_assert(cells != nullptr);

    nya_memset(cells, 0, sizeof(*cells));
}

void nya_ui_cells_reset(NYA_UICells* cells) {
    nya_assert(cells != nullptr);

    nya_memset(cells->cells, 0, sizeof(cells->cells));
    nya_memset(cells->layers, 0, sizeof(cells->layers));

    cells->used_columns = 0;
    cells->used_rows    = 0;

    // the whole grid, so a pass that never sets a clip is not clipped to nothing.
    cells->clip_column  = 0;
    cells->clip_row     = 0;
    cells->clip_columns = NYA_UI_CELL_COLUMNS_MAX;
    cells->clip_rows    = NYA_UI_CELL_ROWS_MAX;
    cells->layer        = 0;
}


/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * THE GRID
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

const NYA_UIPresenter* nya_ui_cells_presenter(NYA_UICells* cells) {
    nya_assert(cells != nullptr);
    nya_assert(cells->presenter.draw == _nya_ui_cell_draw, "a grid is prepared by nya_ui_cells_init before it presents anything");

    return &cells->presenter;
}

NYA_TerminalCell nya_ui_cells_at(const NYA_UICells* cells, u16 column, u16 row) {
    nya_assert(cells != nullptr);

    if (column >= NYA_UI_CELL_COLUMNS_MAX || row >= NYA_UI_CELL_ROWS_MAX) return (NYA_TerminalCell){ 0 };

    return cells->cells[row][column];
}

void nya_ui_cells_present(const NYA_UICells* cells) {
    nya_assert(cells != nullptr);

    u16 columns = nya_min(cells->used_columns, nya_terminal_columns());
    u16 rows    = nya_min(cells->used_rows, nya_terminal_rows());

    for (u16 row = 0; row < rows; row++) {
        for (u16 column = 0; column < columns; column++) {
            // only what this pass wrote: the frame's own drawing keeps every cell the UI did not touch.
            if (cells->layers[row][column] == 0) continue;

            nya_terminal_cell_set(column, row, cells->cells[row][column]);
        }
    }
}

u32 nya_ui_cells_write(const NYA_UICells* cells, char* out, u32 capacity) {
    nya_assert(cells != nullptr && out != nullptr);
    nya_assert(capacity > 0, "a dump needs room for at least the terminator");

    u32 used = 0;
    out[0]   = '\0';

    for (u16 row = 0; row < cells->used_rows; row++) {
        // the blanks at the end of a row say nothing, and cutting them is what makes two dumps diff usefully.
        u16 last = 0;
        for (u16 column = 0; column < cells->used_columns; column++) {
            u32 codepoint = cells->cells[row][column].codepoint;

            if (codepoint != 0 && codepoint != ' ') last = (u16)(column + 1);
        }

        for (u16 column = 0; column < last; column++) {
            u32 codepoint = cells->cells[row][column].codepoint;
            u8  encoded[4];
            u32 size = _nya_ui_cell_encode(codepoint != 0 ? codepoint : ' ', encoded);

            if (used + size + 1 >= capacity) return used;

            nya_memcpy(out + used, encoded, size);

            // terminated as it goes, so a dump that runs out of room mid row is still a string.
            used     += size;
            out[used] = '\0';
        }

        if (used + 2 >= capacity) return used;

        out[used] = '\n';
        used     += 1;
        out[used] = '\0';
    }

    return used;
}


/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * INTERNAL: THE GRID
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

u32 _nya_ui_cell_length(NYA_ConstCString text, u64 bytes) {
    nya_assert(text != nullptr);

    u32 length = 0;
    u64 at     = 0;

    while (at < bytes) {
        u32 codepoint = 0;
        u32 used      = nya_terminal_utf8_decode((const u8*)text + at, bytes - at, &codepoint);

        if (used == 0) break;

        at     += used;
        length += 1;
    }

    return length;
}

s32 _nya_ui_cell_floor(f32 value, f32 size) {
    nya_assert(size > 0.0F);

    return (s32)floorf(value / size);
}

s32 _nya_ui_cell_span(f32 value, f32 size) {
    nya_assert(size > 0.0F);

    // never nothing: a rule half a cell tall is still a rule, and a widget that rounded away would be a hole.
    return (s32)nya_max(roundf(value / size), 1.0F);
}

f32 _nya_ui_cell_quantise(f32 value, f32 unit) {
    nya_assert(unit > 0.0F);

    return roundf(value / unit) * unit;
}

_NYA_UICellRect _nya_ui_cell_rect(const NYA_UICells* cells, NYA_Rectf rect) {
    nya_assert(cells != nullptr);

    f32x2 cell = cells->options.cell;

    return (_NYA_UICellRect){
        .column  = _nya_ui_cell_floor(rect.x, cell.x),
        .row     = _nya_ui_cell_floor(rect.y, cell.y),
        .columns = _nya_ui_cell_span(rect.width, cell.x),
        .rows    = _nya_ui_cell_span(rect.height, cell.y),
    };
}

const NYA_UILook* _nya_ui_cell_look(const NYA_UICells* cells) {
    nya_assert(cells != nullptr && cells->depth <= NYA_UI_STYLE_DEPTH_MAX);

    return &cells->looks[cells->depth];
}

s32 _nya_ui_cell_padding(const NYA_UICells* cells) {
    return (s32)(_nya_ui_cell_look(cells)->padding / cells->options.cell.x);
}

u32 _nya_ui_cell_pick(const NYA_UICells* cells, _NYA_UICellGlyph glyph) {
    nya_assert(cells != nullptr && glyph < _NYA_UI_CELL_GLYPH_COUNT);

    u32 codepoint = _NYA_UI_CELL_GLYPHS[glyph][cells->options.ascii ? 1 : 0];

    nya_assert(codepoint != 0, "glyph %u has no entry in the %s column of the table", (u32)glyph, cells->options.ascii ? "ascii" : "unicode");

    return codepoint;
}

u32 _nya_ui_cell_blend(u32 base, NYA_Color color) {
    f32 alpha = nya_clamp(color.a, 0.0F, 1.0F);

    f32 red   = (f32)((base >> 16U) & 0xFFU) / 255.0F;
    f32 green = (f32)((base >> 8U) & 0xFFU) / 255.0F;
    f32 blue  = (f32)(base & 0xFFU) / 255.0F;

    return nya_terminal_ink(red + ((color.r - red) * alpha), green + ((color.g - green) * alpha), blue + ((color.b - blue) * alpha));
}

NYA_Color _nya_ui_cell_color(const NYA_UIStateColors* colors, const NYA_UIWidgetState* state) {
    nya_assert(colors != nullptr && state != nullptr);

    if (state->disabled) return colors->disabled;

    return nya_color_mix(nya_color_mix(colors->normal, colors->focused, state->focus), colors->pressed, state->press);
}

NYA_TerminalCell* _nya_ui_cell_claim(NYA_UICells* cells, s32 column, s32 row) {
    nya_assert(cells != nullptr);

    if (column < 0 || row < 0 || column >= NYA_UI_CELL_COLUMNS_MAX || row >= NYA_UI_CELL_ROWS_MAX) return nullptr;

    if (column < cells->clip_column || row < cells->clip_row) return nullptr;
    if (column >= cells->clip_column + cells->clip_columns || row >= cells->clip_row + cells->clip_rows) return nullptr;

    // at least, not greater than: a widget lays down its fill and then writes its label on it at the same layer.
    // Biased by one so that zero is a cell this pass has not written at all, which is what present skips.
    u8 wanted = (u8)nya_clamp(cells->layer, 0, U8_MAX - 1) + 1U;

    if (wanted < cells->layers[row][column]) return nullptr;

    cells->layers[row][column] = wanted;

    if (column >= (s32)cells->used_columns) cells->used_columns = (u16)(column + 1);
    if (row >= (s32)cells->used_rows) cells->used_rows = (u16)(row + 1);

    return &cells->cells[row][column];
}

void _nya_ui_cell_fill(NYA_UICells* cells, _NYA_UICellRect rect, NYA_Color paper, NYA_Color ink, u8 attributes) {
    nya_assert(cells != nullptr);

    if (paper.a <= 0.0F) return;

    for (s32 row = rect.row; row < rect.row + rect.rows; row++) {
        for (s32 column = rect.column; column < rect.column + rect.columns; column++) {
            NYA_TerminalCell* cell = _nya_ui_cell_claim(cells, column, row);
            if (cell == nullptr) continue;

            cell->background = _nya_ui_cell_blend(cell->background, paper);

            // an opaque fill erases what was under it, as a panel drawn over a label has to. A translucent one keeps
            // the character and dims both its ink and its paper, which is what makes a scrim read as a scrim.
            if (paper.a >= 1.0F) {
                cell->codepoint  = ' ';
                cell->foreground = nya_terminal_ink(ink.r, ink.g, ink.b);
                cell->attributes = attributes;
            } else {
                cell->foreground = _nya_ui_cell_blend(cell->foreground, paper);
            }
        }
    }
}

void _nya_ui_cell_mark(NYA_UICells* cells, _NYA_UICellRect rect, u8 attributes) {
    nya_assert(cells != nullptr);

    for (s32 row = rect.row; row < rect.row + rect.rows; row++) {
        for (s32 column = rect.column; column < rect.column + rect.columns; column++) {
            if (column < 0 || row < 0 || column >= NYA_UI_CELL_COLUMNS_MAX || row >= NYA_UI_CELL_ROWS_MAX) continue;
            if (cells->layers[row][column] == 0) continue;

            NYA_TerminalCell* cell = _nya_ui_cell_claim(cells, column, row);
            if (cell == nullptr) continue;

            cell->attributes |= attributes;
        }
    }
}

void _nya_ui_cell_glyph(NYA_UICells* cells, s32 column, s32 row, u32 codepoint, NYA_Color color, u8 attributes) {
    nya_assert(cells != nullptr);

    if (color.a <= 0.0F || codepoint == 0) return;

    NYA_TerminalCell* cell = _nya_ui_cell_claim(cells, column, row);
    if (cell == nullptr) return;

    cell->codepoint  = codepoint;
    cell->foreground = _nya_ui_cell_blend(cell->background, color);
    cell->attributes = attributes;
}

void _nya_ui_cell_run(NYA_UICells* cells, s32 column, s32 row, s32 count, u32 codepoint, NYA_Color color, u8 attributes) {
    for (s32 i = 0; i < count; i++) _nya_ui_cell_glyph(cells, column + i, row, codepoint, color, attributes);
}

s32 _nya_ui_cell_text(NYA_UICells* cells, s32 column, s32 row, NYA_ConstCString text, s32 room, NYA_Color color, u8 attributes) {
    nya_assert(cells != nullptr);

    if (text == nullptr || text[0] == '\0' || room <= 0) return 0;

    u64 size   = strnlen(text, _NYA_UI_CELL_TEXT_MAX);
    b8  cut    = (s32)_nya_ui_cell_length(text, size) > room;
    s32 limit  = cut ? room - 1 : room;
    s32 placed = 0;
    u64 at     = 0;

    while (at < size && placed < limit) {
        u32 codepoint = 0;
        u32 used      = nya_terminal_utf8_decode((const u8*)text + at, size - at, &codepoint);

        if (used == 0) break;

        _nya_ui_cell_glyph(cells, column + placed, row, codepoint, color, attributes);

        at     += used;
        placed += 1;
    }

    // what did not fit is said, rather than simply stopping: a label that ran out of room looks like one.
    if (cut) {
        _nya_ui_cell_glyph(cells, column + placed, row, _nya_ui_cell_pick(cells, _NYA_UI_CELL_GLYPH_ELLIPSIS), color, attributes);
        placed += 1;
    }

    return placed;
}

void _nya_ui_cell_wrap(NYA_UICells* cells, _NYA_UICellRect rect, NYA_ConstCString text, NYA_UIAlign align, NYA_Color color, u8 attributes) {
    nya_assert(cells != nullptr && text != nullptr);

    u64 size = strnlen(text, _NYA_UI_CELL_TEXT_MAX);
    u64 at   = 0;

    /*
     * Broken on the cell, not on the word, which is what render2d's terminal backend does with a wrap width: this
     * walks code points and has no dictionary of where a word ends. A caller that wants word wrapping splits the
     * string itself.
     */
    for (s32 row = rect.row; row < rect.row + rect.rows && at < size; row++) {
        char piece[_NYA_UI_CELL_TEXT_MAX];
        u64  used   = 0;
        s32  placed = 0;

        while (placed < rect.columns && at < size) {
            u32 codepoint = 0;
            u32 taken     = nya_terminal_utf8_decode((const u8*)text + at, size - at, &codepoint);

            if (taken == 0 || used + taken >= sizeof(piece)) break;

            nya_memcpy(&piece[used], text + at, taken);

            used   += taken;
            at     += taken;
            placed += 1;
        }

        piece[used] = '\0';

        // NYA_UIAlign counts start, center, end, so half of it is the share of the leftover room in front.
        s32 shift = (s32)roundf((f32)(rect.columns - placed) * (f32)align * 0.5F);

        (void)_nya_ui_cell_text(cells, rect.column + shift, row, piece, rect.columns - shift, color, attributes);
    }
}

void _nya_ui_cell_delimit(NYA_UICells* cells, _NYA_UICellRect rect, b8 focused, b8 round, NYA_Color color, u8 attributes) {
    nya_assert(cells != nullptr);

    if (rect.columns < 2) return;

    _NYA_UICellGlyph open  = round ? _NYA_UI_CELL_GLYPH_ROUND_OPEN : _NYA_UI_CELL_GLYPH_OPEN;
    _NYA_UICellGlyph close = round ? _NYA_UI_CELL_GLYPH_ROUND_CLOSE : _NYA_UI_CELL_GLYPH_CLOSE;

    // the second of the two focus marks, and the one that survives a terminal with no attributes at all.
    if (focused) {
        open  = _NYA_UI_CELL_GLYPH_FOCUS_OPEN;
        close = _NYA_UI_CELL_GLYPH_FOCUS_CLOSE;
    }

    _nya_ui_cell_glyph(cells, rect.column, rect.row, _nya_ui_cell_pick(cells, open), color, attributes);
    _nya_ui_cell_glyph(cells, rect.column + rect.columns - 1, rect.row, _nya_ui_cell_pick(cells, close), color, attributes);
}

u32 _nya_ui_cell_encode(u32 codepoint, OUT u8* out) {
    nya_assert(out != nullptr);

    if (codepoint < 0x80U) {
        out[0] = (u8)codepoint;
        return 1;
    }

    if (codepoint < 0x800U) {
        out[0] = (u8)(0xC0U | (codepoint >> 6U));
        out[1] = (u8)(0x80U | (codepoint & 0x3FU));
        return 2;
    }

    if (codepoint < 0x10000U) {
        out[0] = (u8)(0xE0U | (codepoint >> 12U));
        out[1] = (u8)(0x80U | ((codepoint >> 6U) & 0x3FU));
        out[2] = (u8)(0x80U | (codepoint & 0x3FU));
        return 3;
    }

    out[0] = (u8)(0xF0U | (codepoint >> 18U));
    out[1] = (u8)(0x80U | ((codepoint >> 12U) & 0x3FU));
    out[2] = (u8)(0x80U | ((codepoint >> 6U) & 0x3FU));
    out[3] = (u8)(0x80U | (codepoint & 0x3FU));

    return 4;
}


/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * INTERNAL: THE SEAM
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

void _nya_ui_cell_look_build(void* state, u32 depth, const NYA_UIStyle* style, f32 scale, NYA_UILook* out) {
    NYA_UICells* cells = state;

    nya_assert(cells != nullptr && style != nullptr && out != nullptr);
    nya_assert(depth <= NYA_UI_STYLE_DEPTH_MAX, "a look is built at depth %u", depth);

    nya_ui_look_scale(style, scale, out);

    f32x2 cell = cells->options.cell;

    /*
     * Every size the layout adds up, rounded to a whole cell before the layout ever sees one of them. A row is one
     * cell tall and the gaps between rows are whole rows, so a column of widgets adds up to whole rows however many
     * there are; anything else accumulates a half cell per widget and the bottom of a panel lands between two rows.
     */
    out->margin  = _nya_ui_cell_quantise(out->margin, cell.y);
    out->padding = _nya_ui_cell_quantise(out->padding, cell.y);
    out->spacing = _nya_ui_cell_quantise(out->spacing, cell.y);

    // there is no half cell for a corner to be round in, nothing under a cell to drop a shadow onto, and a widget
    // that grew a cell on focus and settled back would make its whole row jump. All three are off, not rounded.
    out->radius  = 0.0F;
    out->outline = 0.0F;
    out->depth   = 0.0F;
    out->pop     = 0.0F;

    // kept at one column so the layout's arithmetic is whole, though nothing here draws a bar: focus is reverse
    // video and the angle delimiters, neither of which takes a column the widget did not already have.
    out->focus_bar = cell.x;

    out->item_height = nya_max(_nya_ui_cell_quantise(out->item_height, cell.y), cell.y);
    out->title_bar   = cell.y;

    // one cell, whatever the role: a grid has one face, and the type scale is a colour and a weight in it, not a size.
    for (u32 i = 0; i < NYA_UI_TEXT_COUNT; i++) out->line_heights[i] = cell.y;

    cells->looks[depth] = *out;
}

void _nya_ui_cell_look_use(void* state, u32 depth) {
    NYA_UICells* cells = state;

    nya_assert(cells != nullptr);
    nya_assert(depth <= NYA_UI_STYLE_DEPTH_MAX, "a look at depth %u was selected", depth);

    cells->depth = depth;
}

f32x2 _nya_ui_cell_measure(void* state, NYA_UIText role, NYA_ConstCString text, f32 room, NYA_UIOverflow overflow) {
    NYA_UICells* cells = state;

    nya_assert(cells != nullptr && text != nullptr);
    nya_assert(role < NYA_UI_TEXT_COUNT);

    f32x2 cell  = cells->options.cell;
    f32   width = (f32)_nya_ui_cell_length(text, strnlen(text, _NYA_UI_CELL_TEXT_MAX)) * cell.x;

    // a terminal has one size, so there is nothing to shrink to: SHRINK measures what VISIBLE does and the drawing
    // cuts the line with an ellipsis instead. That is the one place this presenter cannot do what the style asked.
    if (overflow != NYA_UI_OVERFLOW_WRAP || room <= 0.0F || width <= room) return (f32x2){ width, cell.y };

    // whole cells across, so a wrapped line lands on the grid rather than between two columns of it.
    f32 columns = nya_max(floorf(room / cell.x), 1.0F);
    f32 lines   = ceilf(width / (columns * cell.x));

    return (f32x2){ columns * cell.x, lines * cell.y };
}

f32 _nya_ui_cell_measure_bytes(void* state, NYA_UIText role, NYA_ConstCString text, u32 bytes) {
    NYA_UICells* cells = state;

    nya_assert(cells != nullptr && text != nullptr);
    nya_assert(role < NYA_UI_TEXT_COUNT);

    return (f32)_nya_ui_cell_length(text, nya_min((u64)bytes, strnlen(text, _NYA_UI_CELL_TEXT_MAX))) * cells->options.cell.x;
}

void _nya_ui_cell_clip_set(void* state, NYA_Window* window, NYA_Rectf clip, b8 whole) {
    NYA_UICells* cells = state;

    nya_unused(window);
    nya_assert(cells != nullptr);

    if (whole) {
        cells->clip_column  = 0;
        cells->clip_row     = 0;
        cells->clip_columns = NYA_UI_CELL_COLUMNS_MAX;
        cells->clip_rows    = NYA_UI_CELL_ROWS_MAX;

        return;
    }

    _NYA_UICellRect rect = _nya_ui_cell_rect(cells, clip);

    cells->clip_column  = rect.column;
    cells->clip_row     = rect.row;
    cells->clip_columns = rect.columns;
    cells->clip_rows    = rect.rows;
}

s32 _nya_ui_cell_layer_get(void* state, NYA_Window* window) {
    NYA_UICells* cells = state;

    nya_unused(window);
    nya_assert(cells != nullptr);

    return cells->layer;
}

void _nya_ui_cell_layer_set(void* state, NYA_Window* window, s32 layer) {
    NYA_UICells* cells = state;

    nya_unused(window);
    nya_assert(cells != nullptr);

    cells->layer = layer;
}


/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * INTERNAL: THE WIDGETS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

void _nya_ui_cell_draw(void* state, NYA_Window* window, const NYA_UIWidgetDraw* widget) {
    NYA_UICells* cells = state;

    nya_unused(window);
    nya_assert(cells != nullptr && widget != nullptr);
    nya_assert(widget->kind < NYA_UI_WIDGET_KIND_COUNT);
    nya_assert(widget->label != nullptr, "%s arrived without a label; \"\" is how a widget says it has none", nya_ui_widget_kind_name(widget->kind));

    const NYA_UILook*  look  = _nya_ui_cell_look(cells);
    const NYA_UIStyle* style = &look->style;

    _NYA_UICellRect rect = _nya_ui_cell_rect(cells, widget->rect);
    NYA_Color       ink  = _nya_ui_cell_color(&style->text, &widget->state);

    /*
     * The attributes every write of this widget carries, which is where focus, press and fading are decided once
     * rather than at twenty call sites. Reverse video is the focus mark that needs no colour and no column; the
     * second mark, the angle delimiters, is inside the widgets that have delimiters.
     */
    u8 attributes = 0;

    if (widget->state.focused) attributes |= NYA_TERMINAL_ATTRIBUTE_REVERSE;
    if (widget->state.held) attributes |= NYA_TERMINAL_ATTRIBUTE_UNDERLINE;
    if (widget->state.disabled || widget->opacity < 1.0F) attributes |= NYA_TERMINAL_ATTRIBUTE_DIM;

    // an opacity group cannot fade a character, so it fades the colours it can and says the rest with dim.
    NYA_Color paper = _nya_ui_cell_color(&style->button, &widget->state);
    paper.a        *= widget->opacity;
    ink.a          *= widget->opacity;

    s32 padding = _nya_ui_cell_padding(cells);

    switch (widget->kind) {
        // translucent, so the characters under it stay and only dim: a scrim over a menu is a scrim, not an erase.
        case NYA_UI_WIDGET_SCRIM: {
            _nya_ui_cell_fill(cells, rect, style->scrim, ink, attributes);
            _nya_ui_cell_mark(cells, rect, NYA_TERMINAL_ATTRIBUTE_DIM);
        } break;

        case NYA_UI_WIDGET_PANEL: _nya_ui_cell_panel(cells, widget, rect, attributes); break;
        case NYA_UI_WIDGET_LABEL: _nya_ui_cell_label(cells, widget, rect, attributes); break;

        /*
         * `[ label ]`, delimiters and all, centred in whatever row the layout gave it. Around the label rather
         * than at the two ends of the row: a button a container stretched to its full width would otherwise be a
         * bracket at each edge of the screen with a word floating between them, which reads as a rule and not as
         * something to press. What was stretched is still the row, and the row is still what a click lands on.
         */
        case NYA_UI_WIDGET_BUTTON: {
            _nya_ui_cell_fill(cells, rect, paper, ink, attributes);

            s32 width = (s32)_nya_ui_cell_length(widget->label, strnlen(widget->label, _NYA_UI_CELL_TEXT_MAX));
            s32 taken = nya_min(width + 4, rect.columns);
            s32 shift = nya_max((rect.columns - taken) / 2, 0);

            _NYA_UICellRect around = { .column = rect.column + shift, .row = rect.row, .columns = taken, .rows = 1 };

            _nya_ui_cell_delimit(cells, around, widget->state.focused, false, ink, attributes);
            (void)_nya_ui_cell_text(cells, around.column + 2, rect.row, widget->label, nya_max(taken - 4, 1), ink, attributes);
        } break;

        case NYA_UI_WIDGET_SELECTABLE:
        case NYA_UI_WIDGET_TOGGLE:
        case NYA_UI_WIDGET_RADIO:        _nya_ui_cell_choice(cells, widget, rect, ink, attributes); break;

        case NYA_UI_WIDGET_SLIDER:   _nya_ui_cell_slider(cells, widget, rect, ink, attributes); break;
        case NYA_UI_WIDGET_DROPDOWN: _nya_ui_cell_dropdown(cells, widget, rect, ink, attributes); break;

        case NYA_UI_WIDGET_FIELD: {
            _nya_ui_cell_fill(cells, rect, paper, ink, attributes);
            (void)_nya_ui_cell_text(cells, rect.column + padding, rect.row, widget->label, rect.columns - padding, ink, attributes);

            _nya_ui_cell_field(cells, &widget->as_field.field, widget->as_field.field.box, ink, widget->state.focused, attributes);
        } break;

        case NYA_UI_WIDGET_COLOR_PICKER: _nya_ui_cell_picker(cells, widget, rect, ink, attributes); break;
        case NYA_UI_WIDGET_CHART:        _nya_ui_cell_chart(cells, widget, rect, attributes); break;

        /*
         * A terminal has no sampler and no sheet to cut from, so an icon is one mark that says a picture belongs
         * here. This is the one widget that cannot be drawn in cells at all; a TUI that needs pictures sends them
         * through the kitty protocol, which is render2d_terminal's nya_render2d_terminal_image and not the UI's.
         */
        case NYA_UI_WIDGET_ICON: {
            _nya_ui_cell_glyph(cells, rect.column + (rect.columns / 2), rect.row + (rect.rows / 2), _nya_ui_cell_pick(cells, _NYA_UI_CELL_GLYPH_ICON),
                               style->text_dim, attributes);
        } break;

        case NYA_UI_WIDGET_SECTION: {
            _nya_ui_cell_fill(cells, rect, paper, ink, attributes);
            _nya_ui_cell_mark_glyph(cells, widget->as_mark.mark, rect.column + padding, rect.row, ink, attributes);
            (void)_nya_ui_cell_text(cells, rect.column + padding + 2, rect.row, widget->label, rect.columns - padding - 2, ink, attributes);
        } break;

        // one square of a title bar: the mark, over the fill only when the bar asked for one.
        case NYA_UI_WIDGET_CHROME: {
            if (widget->as_mark.body) _nya_ui_cell_fill(cells, rect, paper, ink, attributes);

            _nya_ui_cell_mark_glyph(cells, widget->as_mark.mark, rect.column + (rect.columns / 2), rect.row + (rect.rows / 2), ink, attributes);
        } break;

        // not a button: nothing presses a grip, so it is the mark alone, in the corner cell the layout gave it.
        case NYA_UI_WIDGET_GRIP: {
            _nya_ui_cell_mark_glyph(cells, widget->as_mark.mark, rect.column + rect.columns - 1, rect.row + rect.rows - 1, style->text_dim, attributes);
        } break;

        case NYA_UI_WIDGET_SCROLLBAR: {
            for (s32 row = rect.row; row < rect.row + rect.rows; row++) {
                _nya_ui_cell_run(cells, rect.column, row, rect.columns, _nya_ui_cell_pick(cells, _NYA_UI_CELL_GLYPH_FILL), style->text_dim, attributes);
            }
        } break;

        // a rule is a line either way up; a stripe is paper under a table row, which is why it is the one that fills.
        case NYA_UI_WIDGET_RULE: {
            b8 across = widget->rect.width >= widget->rect.height;
            u32 glyph = _nya_ui_cell_pick(cells, across ? _NYA_UI_CELL_GLYPH_FRAME_HORIZONTAL : _NYA_UI_CELL_GLYPH_FRAME_VERTICAL);

            for (s32 row = rect.row; row < rect.row + rect.rows; row++) _nya_ui_cell_run(cells, rect.column, row, rect.columns, glyph, widget->color, attributes);
        } break;

        case NYA_UI_WIDGET_STRIPE: _nya_ui_cell_fill(cells, rect, widget->color, ink, attributes); break;

        case NYA_UI_WIDGET_UNDERLINE: {
            _nya_ui_cell_run(cells, rect.column, rect.row, rect.columns, _nya_ui_cell_pick(cells, _NYA_UI_CELL_GLYPH_UNDERLINE), widget->color, attributes);
        } break;

        case NYA_UI_WIDGET_KIND_COUNT:
        default:                       nya_unreachable();
    }
}

void _nya_ui_cell_panel(NYA_UICells* cells, const NYA_UIWidgetDraw* widget, _NYA_UICellRect rect, u8 attributes) {
    const NYA_UILook*  look  = _nya_ui_cell_look(cells);
    const NYA_UIStyle* style = &look->style;
    const NYA_UIPanel* panel = widget->as_panel.options;

    nya_assert(panel != nullptr, "a panel is drawn from the options that opened it");

    NYA_Color fill = panel->fill;
    if (fill.r == 0.0F && fill.g == 0.0F && fill.b == 0.0F && fill.a == 0.0F) fill = style->panel;

    NYA_Color line = style->text.normal;

    if (!panel->frameless) {
        _nya_ui_cell_fill(cells, rect, fill, line, attributes);

        s32 last_column = rect.column + rect.columns - 1;
        s32 last_row    = rect.row + rect.rows - 1;

        // the four edges and the four corners, which is a frame in every terminal anyone has drawn one in.
        _nya_ui_cell_run(cells, rect.column + 1, rect.row, rect.columns - 2, _nya_ui_cell_pick(cells, _NYA_UI_CELL_GLYPH_FRAME_HORIZONTAL), line, attributes);
        _nya_ui_cell_run(cells, rect.column + 1, last_row, rect.columns - 2, _nya_ui_cell_pick(cells, _NYA_UI_CELL_GLYPH_FRAME_HORIZONTAL), line, attributes);

        for (s32 row = rect.row + 1; row < last_row; row++) {
            _nya_ui_cell_glyph(cells, rect.column, row, _nya_ui_cell_pick(cells, _NYA_UI_CELL_GLYPH_FRAME_VERTICAL), line, attributes);
            _nya_ui_cell_glyph(cells, last_column, row, _nya_ui_cell_pick(cells, _NYA_UI_CELL_GLYPH_FRAME_VERTICAL), line, attributes);
        }

        _nya_ui_cell_glyph(cells, rect.column, rect.row, _nya_ui_cell_pick(cells, _NYA_UI_CELL_GLYPH_FRAME_TOP_LEFT), line, attributes);
        _nya_ui_cell_glyph(cells, last_column, rect.row, _nya_ui_cell_pick(cells, _NYA_UI_CELL_GLYPH_FRAME_TOP_RIGHT), line, attributes);
        _nya_ui_cell_glyph(cells, rect.column, last_row, _nya_ui_cell_pick(cells, _NYA_UI_CELL_GLYPH_FRAME_BOTTOM_LEFT), line, attributes);
        _nya_ui_cell_glyph(cells, last_column, last_row, _nya_ui_cell_pick(cells, _NYA_UI_CELL_GLYPH_FRAME_BOTTOM_RIGHT), line, attributes);
    }

    if (widget->label[0] == '\0') return;

    /*
     * In the top edge with a space either side, between whatever chrome the caller put at each end of the bar. A
     * title written across the whole width slides under the close mark as soon as it is long enough, and a frame
     * with a hole in it is the window that looks broken.
     */
    s32 left  = rect.column + 1 + _nya_ui_cell_span(widget->as_panel.title_room.x, cells->options.cell.x) - 1;
    s32 right = rect.column + rect.columns - 1 - _nya_ui_cell_span(widget->as_panel.title_room.y, cells->options.cell.x) + 1;
    s32 room  = nya_max(right - left - 2, 0);

    if (room <= 0) return;

    _nya_ui_cell_glyph(cells, left, rect.row, ' ', line, attributes);
    s32 placed = _nya_ui_cell_text(cells, left + 1, rect.row, widget->label, room, line, attributes);
    _nya_ui_cell_glyph(cells, left + 1 + placed, rect.row, ' ', line, attributes);
}

void _nya_ui_cell_label(NYA_UICells* cells, const NYA_UIWidgetDraw* widget, _NYA_UICellRect rect, u8 attributes) {
    if (widget->as_label.overflow == NYA_UI_OVERFLOW_WRAP && widget->as_label.room > 0.0F) {
        _nya_ui_cell_wrap(cells, rect, widget->label, widget->as_label.align, widget->color, attributes);
        return;
    }

    (void)_nya_ui_cell_text(cells, rect.column, rect.row, widget->label, rect.columns, widget->color, attributes);
}

void _nya_ui_cell_choice(NYA_UICells* cells, const NYA_UIWidgetDraw* widget, _NYA_UICellRect rect, NYA_Color ink, u8 attributes) {
    const NYA_UIStyle* style   = &_nya_ui_cell_look(cells)->style;
    s32                padding = _nya_ui_cell_padding(cells);

    NYA_Color paper = _nya_ui_cell_color(&style->button, &widget->state);
    paper.a        *= widget->opacity;

    _nya_ui_cell_fill(cells, rect, paper, ink, attributes);

    b8  on   = widget->as_choice.on;
    s32 mark = rect.column + padding;

    /*
     * A selectable is a row of a list, so it is marked and not boxed: a box in front of every row of a list is
     * three columns of noise. A toggle and a radio are a state, so they are the box and the ring every terminal
     * program has drawn them as, and the two differ by their delimiters so that neither ever reads as the other.
     */
    if (widget->kind == NYA_UI_WIDGET_SELECTABLE) {
        u32 glyph = _nya_ui_cell_pick(cells, on ? _NYA_UI_CELL_GLYPH_CHOSEN : _NYA_UI_CELL_GLYPH_OFF);

        _nya_ui_cell_glyph(cells, mark, rect.row, glyph, ink, attributes);
        (void)_nya_ui_cell_text(cells, mark + 2, rect.row, widget->label, rect.columns - padding - 2, ink, attributes);

        return;
    }

    b8              radio = widget->kind == NYA_UI_WIDGET_RADIO;
    _NYA_UICellRect box   = { .column = mark, .row = rect.row, .columns = 3, .rows = 1 };

    _nya_ui_cell_delimit(cells, box, widget->state.focused, radio, ink, attributes);
    _nya_ui_cell_glyph(cells, mark + 1, rect.row, _nya_ui_cell_pick(cells, on ? (radio ? _NYA_UI_CELL_GLYPH_DOT : _NYA_UI_CELL_GLYPH_ON) : _NYA_UI_CELL_GLYPH_OFF),
                       ink, attributes);

    (void)_nya_ui_cell_text(cells, mark + 4, rect.row, widget->label, rect.columns - padding - 4, ink, attributes);
}

void _nya_ui_cell_slider(NYA_UICells* cells, const NYA_UIWidgetDraw* widget, _NYA_UICellRect rect, NYA_Color ink, u8 attributes) {
    const NYA_UIStyle* style   = &_nya_ui_cell_look(cells)->style;
    s32                padding = _nya_ui_cell_padding(cells);

    NYA_Color paper = _nya_ui_cell_color(&style->button, &widget->state);
    paper.a        *= widget->opacity;

    _nya_ui_cell_fill(cells, rect, paper, ink, attributes);
    (void)_nya_ui_cell_text(cells, rect.column + padding, rect.row, widget->label, rect.columns - padding, ink, attributes);

    // the track the input pass read, so what a click lands on is what was drawn.
    _NYA_UICellRect track = _nya_ui_cell_rect(cells, widget->as_slider.track);

    track.row  = rect.row;
    track.rows = 1;

    if (track.columns < 3) return;

    _nya_ui_cell_delimit(cells, track, widget->state.focused, false, ink, attributes);

    s32 room   = track.columns - 2;
    s32 filled = (s32)roundf((f32)room * nya_clamp(widget->as_slider.t, 0.0F, 1.0F));

    NYA_Color accent = widget->state.disabled ? style->track : style->accent;
    accent.a        *= widget->opacity;

    _nya_ui_cell_run(cells, track.column + 1, track.row, filled, _nya_ui_cell_pick(cells, _NYA_UI_CELL_GLYPH_FILL), accent, attributes);
    _nya_ui_cell_run(cells, track.column + 1 + filled, track.row, room - filled, _nya_ui_cell_pick(cells, _NYA_UI_CELL_GLYPH_TRACK), style->text_dim, attributes);
}

void _nya_ui_cell_dropdown(NYA_UICells* cells, const NYA_UIWidgetDraw* widget, _NYA_UICellRect rect, NYA_Color ink, u8 attributes) {
    const NYA_UIStyle* style   = &_nya_ui_cell_look(cells)->style;
    s32                padding = _nya_ui_cell_padding(cells);

    NYA_Color paper = _nya_ui_cell_color(&style->button, &widget->state);
    paper.a        *= widget->opacity;

    _nya_ui_cell_fill(cells, rect, paper, ink, attributes);
    (void)_nya_ui_cell_text(cells, rect.column + padding, rect.row, widget->label, rect.columns - padding, ink, attributes);

    // the chosen option against the right edge, with the caret past it: pointing down when closed and up when the
    // list is showing, which is the one thing that says a row is a dropdown and not a label with a value on it.
    s32 arrow = rect.column + rect.columns - padding - 1;
    s32 width = (s32)_nya_ui_cell_length(widget->as_dropdown.shown, strnlen(widget->as_dropdown.shown, _NYA_UI_CELL_TEXT_MAX));
    s32 value = nya_max(arrow - 1 - width, rect.column + padding);

    (void)_nya_ui_cell_text(cells, value, rect.row, widget->as_dropdown.shown, arrow - 1 - value, ink, attributes);
    _nya_ui_cell_glyph(cells, arrow, rect.row, _nya_ui_cell_pick(cells, widget->as_dropdown.open ? _NYA_UI_CELL_GLYPH_UP : _NYA_UI_CELL_GLYPH_DOWN), ink,
                       attributes);
}

void _nya_ui_cell_field(NYA_UICells* cells, const NYA_UIFieldDraw* field, NYA_Rectf box, NYA_Color ink, b8 focused, u8 attributes) {
    nya_assert(cells != nullptr && field != nullptr);
    nya_assert(field->buffer != nullptr && field->composing != nullptr, "a field is drawn from a buffer and a composition, both of which may be empty but not absent");

    const NYA_UIStyle* style = &_nya_ui_cell_look(cells)->style;
    _NYA_UICellRect    rect  = _nya_ui_cell_rect(cells, box);

    rect.rows = 1;

    if (rect.columns < 3) return;

    _nya_ui_cell_fill(cells, rect, style->track, ink, attributes);
    _nya_ui_cell_delimit(cells, rect, focused, false, ink, attributes);

    s32 room  = rect.columns - 2;
    s32 first = rect.column + 1;

    // what the input pass scrolled the line by, so the caret the widget put inside the box is inside it here too.
    s32 shift = (s32)(field->shift / cells->options.cell.x);
    u64 size  = strnlen(field->buffer, _NYA_UI_CELL_TEXT_MAX);
    u64 at    = 0;

    for (s32 column = -shift; at < size && column < room; column++) {
        u32 codepoint = 0;
        u32 used      = nya_terminal_utf8_decode((const u8*)field->buffer + at, size - at, &codepoint);

        if (used == 0) break;
        at += used;

        if (column < 0) continue;

        // the selection is the terminal's own emphasis, flipped against whatever the row already carries: a focused
        // row is reversed, so a selection inside it is the part that is not.
        u32 from  = nya_min(field->caret, field->select);
        u32 to    = nya_max(field->caret, field->select);
        u8  marks = attributes;

        if (field->editing && at > (u64)from && at <= (u64)to) marks ^= NYA_TERMINAL_ATTRIBUTE_REVERSE;

        _nya_ui_cell_glyph(cells, first + column, rect.row, codepoint, ink, marks);
    }

    if (!field->editing) return;

    // the caret is a cell in reverse, which is what a terminal's own cursor is, and it sits after the composition
    // when the IME has one: that text is not in the buffer yet, so it is drawn dim and underlined ahead of it.
    s32 caret = (s32)_nya_ui_cell_length(field->buffer, nya_min((u64)field->caret, size)) - shift;

    if (field->composing[0] != '\0') {
        s32 composing = _nya_ui_cell_text(cells, first + caret, rect.row, field->composing, room - caret, style->text_dim,
                                          attributes | NYA_TERMINAL_ATTRIBUTE_UNDERLINE | NYA_TERMINAL_ATTRIBUTE_DIM);

        caret += composing;
    }

    if (caret < 0 || caret >= room) return;

    _nya_ui_cell_mark(cells, (_NYA_UICellRect){ .column = first + caret, .row = rect.row, .columns = 1, .rows = 1 }, NYA_TERMINAL_ATTRIBUTE_REVERSE);
}

void _nya_ui_cell_picker(NYA_UICells* cells, const NYA_UIWidgetDraw* widget, _NYA_UICellRect rect, NYA_Color ink, u8 attributes) {
    const NYA_UIStyle* style   = &_nya_ui_cell_look(cells)->style;
    s32                padding = _nya_ui_cell_padding(cells);

    NYA_ColorHSV hsv = widget->as_picker.hsv;

    _NYA_UICellRect plane  = _nya_ui_cell_rect(cells, widget->as_picker.plane);
    _NYA_UICellRect hue    = _nya_ui_cell_rect(cells, widget->as_picker.hue);
    _NYA_UICellRect alpha  = _nya_ui_cell_rect(cells, widget->as_picker.alpha);
    _NYA_UICellRect swatch = _nya_ui_cell_rect(cells, widget->as_picker.swatch);

    (void)_nya_ui_cell_text(cells, rect.column + padding, rect.row, widget->label, rect.columns - padding, ink, attributes);

    /*
     * The plane really is a plane: one cell of paper per step of saturation across and of value down. A terminal
     * that quantises to 256 colours or to 16 shows bands instead of a gradient, and one with no colour at all shows
     * a blank field with the cursor in it — which is why the cursor is a character and not a colour.
     */
    for (s32 row = 0; row < plane.rows; row++) {
        for (s32 column = 0; column < plane.columns; column++) {
            f32 saturation = plane.columns > 1 ? (f32)column / (f32)(plane.columns - 1) : 1.0F;
            f32 value      = plane.rows > 1 ? 1.0F - ((f32)row / (f32)(plane.rows - 1)) : 1.0F;

            NYA_Color at = nya_color_from_hsv((NYA_ColorHSV){ hsv.h, saturation, value, 1.0F });

            _nya_ui_cell_fill(cells, (_NYA_UICellRect){ .column = plane.column + column, .row = plane.row + row, .columns = 1, .rows = 1 }, at, ink, attributes);
        }
    }

    for (s32 row = 0; row < hue.rows; row++) {
        f32       share = hue.rows > 1 ? (f32)row / (f32)(hue.rows - 1) : 0.0F;
        NYA_Color at    = nya_color_from_hsv((NYA_ColorHSV){ share * _NYA_UI_CELL_HUE_SPAN, 1.0F, 1.0F, 1.0F });

        _nya_ui_cell_fill(cells, (_NYA_UICellRect){ .column = hue.column, .row = hue.row + row, .columns = hue.columns, .rows = 1 }, at, ink, attributes);
    }

    for (s32 column = 0; column < alpha.columns; column++) {
        f32       share = alpha.columns > 1 ? (f32)column / (f32)(alpha.columns - 1) : 1.0F;
        NYA_Color at    = { widget->as_picker.value.r, widget->as_picker.value.g, widget->as_picker.value.b, share };

        _nya_ui_cell_fill(cells, (_NYA_UICellRect){ .column = alpha.column + column, .row = alpha.row, .columns = 1, .rows = alpha.rows }, style->track, ink,
                          attributes);
        _nya_ui_cell_fill(cells, (_NYA_UICellRect){ .column = alpha.column + column, .row = alpha.row, .columns = 1, .rows = alpha.rows }, at, ink, attributes);
    }

    _nya_ui_cell_fill(cells, swatch, widget->as_picker.value, ink, attributes);

    // where each part stands, as characters rather than as notches: a notch is a pixel thing.
    s32 across = plane.column + (s32)roundf((f32)nya_max(plane.columns - 1, 0) * hsv.s);
    s32 down   = plane.row + (s32)roundf((f32)nya_max(plane.rows - 1, 0) * (1.0F - hsv.v));

    _nya_ui_cell_glyph(cells, across, down, _nya_ui_cell_pick(cells, _NYA_UI_CELL_GLYPH_CURSOR), style->text.normal, attributes);
    _nya_ui_cell_glyph(cells, hue.column, hue.row + (s32)roundf((f32)nya_max(hue.rows - 1, 0) * (hsv.h / _NYA_UI_CELL_HUE_SPAN)),
                       _nya_ui_cell_pick(cells, _NYA_UI_CELL_GLYPH_RIGHT), style->text.normal, attributes);
    _nya_ui_cell_glyph(cells, alpha.column + (s32)roundf((f32)nya_max(alpha.columns - 1, 0) * hsv.a), alpha.row,
                       _nya_ui_cell_pick(cells, _NYA_UI_CELL_GLYPH_UP), style->text.normal, attributes);

    _nya_ui_cell_field(cells, &widget->as_picker.field, widget->as_picker.field.box, ink, widget->state.focused, attributes);
}

void _nya_ui_cell_chart(NYA_UICells* cells, const NYA_UIWidgetDraw* widget, _NYA_UICellRect rect, u8 attributes) {
    const NYA_UIChart* chart = widget->as_chart.chart;

    nya_assert(chart != nullptr, "a chart is drawn from the values that declared it");

    const NYA_UIStyle* style = &_nya_ui_cell_look(cells)->style;

    _nya_ui_cell_fill(cells, rect, style->track, widget->color, attributes);

    // the newest points, when there are more of them than the plot has columns; a chart of a history shows its end.
    u32        count  = nya_min(chart->count, (u32)NYA_UI_CHART_POINTS_MAX);
    const f32* values = chart->values + (chart->count - count);

    if (count == 0 || rect.columns <= 0 || rect.rows <= 0) return;

    f32 low  = chart->min;
    f32 high = chart->max;

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

    for (s32 column = 0; column < rect.columns; column++) {
        u32 index = (u32)((f32)column / (f32)rect.columns * (f32)count);
        f32 share = nya_clamp((values[nya_min(index, count - 1)] - low) / span, 0.0F, 1.0F);

        /*
         * The eighths, so a plot in cells has eight times the resolution of the rows it is drawn in: a bar is whole
         * blocks up to its last row and the fraction of one at the top. A line is one mark at that height instead,
         * because a line drawn as filled columns is a bar chart with a different name on it.
         */
        s32 eighths = (s32)roundf(share * (f32)(rect.rows * _NYA_UI_CELL_EIGHTHS));
        s32 top     = rect.row + rect.rows - 1 - (eighths / _NYA_UI_CELL_EIGHTHS);

        if (chart->kind == NYA_UI_CHART_LINE) {
            _nya_ui_cell_glyph(cells, rect.column + column, nya_max(top, rect.row), _nya_ui_cell_pick(cells, _NYA_UI_CELL_GLYPH_DOT), widget->color, attributes);
            continue;
        }

        for (s32 row = rect.row + rect.rows - 1; row > top; row--) {
            _nya_ui_cell_glyph(cells, rect.column + column, row, _nya_ui_cell_pick(cells, _NYA_UI_CELL_GLYPH_BLOCK_8), widget->color, attributes);
        }

        s32 part = eighths % _NYA_UI_CELL_EIGHTHS;
        if (part > 0 && top >= rect.row) {
            _nya_ui_cell_glyph(cells, rect.column + column, top, _nya_ui_cell_pick(cells, (_NYA_UICellGlyph)(_NYA_UI_CELL_GLYPH_BLOCK_1 + part - 1)),
                               widget->color, attributes);
        }
    }
}

void _nya_ui_cell_mark_glyph(NYA_UICells* cells, NYA_UIMark mark, s32 column, s32 row, NYA_Color ink, u8 attributes) {
    nya_assert(mark < NYA_UI_MARK_COUNT);

    _NYA_UICellGlyph glyph = _NYA_UI_CELL_GLYPH_CROSS;

    switch (mark) {
        case NYA_UI_MARK_CLOSE:     glyph = _NYA_UI_CELL_GLYPH_CROSS; break;
        case NYA_UI_MARK_COLLAPSED: glyph = _NYA_UI_CELL_GLYPH_RIGHT; break;
        case NYA_UI_MARK_EXPANDED:  glyph = _NYA_UI_CELL_GLYPH_DOWN; break;
        case NYA_UI_MARK_MENU:      glyph = _NYA_UI_CELL_GLYPH_MENU; break;
        case NYA_UI_MARK_GRIP:      glyph = _NYA_UI_CELL_GLYPH_GRIP; break;

        case NYA_UI_MARK_COUNT:
        default:                    nya_unreachable();
    }

    _nya_ui_cell_glyph(cells, column, row, _nya_ui_cell_pick(cells, glyph), ink, attributes);
}
