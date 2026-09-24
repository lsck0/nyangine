/**
 * @file ui_present_cell.h
 *
 * The presenter that draws the UI as a terminal draws things: a button is `[ save ]`, a toggle is `[x] label`, a
 * panel is a box drawing frame, a slider is a bar of blocks. The shape presenter already reaches cells — render2d's
 * terminal backend rasterises its rectangles into them — but what comes out is a drawing of a GUI made of
 * characters. This one is a TUI.
 *
 * Functions
 *
 *   nya_ui_cells_init, nya_ui_cells_deinit   the caller owned grid the presenter writes into
 *   nya_ui_cells_reset                       throws the last pass's cells away
 *   nya_ui_cells_presenter                   a presenter drawing into that grid
 *   nya_ui_cells_at                          one cell, for a test or a caller that reads its own screen
 *   nya_ui_cells_present                     the cells this pass wrote, onto the terminal
 *   nya_ui_cells_write                       the same grid as rows of text
 *
 * ```c
 * // a TUI, from a program built with -DNYA_TERMINAL.
 * static NYA_UICells cells;
 * nya_ui_cells_init(&cells, (NYA_UICellOptions){ 0 });
 * nya_ui_presenter_set(window, nya_ui_cells_presenter(&cells));
 *
 * nya_render2d_terminal_frame_begin(window, ground);
 * nya_ui_cells_reset(&cells);
 * menu(window, NYA_UI_PASS_DRAW);
 * nya_ui_cells_present(&cells);
 * nya_render2d_terminal_frame_end(window);
 * ```
 *
 * ## Cells, not pixels
 *
 * `measure` and `measure_bytes` answer in whole cells, and `look_build` rounds every size the layout adds up to a
 * whole cell before the layout ever sees it: margins, padding and spacing to a whole row, a row's height to exactly
 * one row, every line height to one row. A widget one and a half cells wide cannot be laid out because no
 * measurement it is built from is a half. Roundness, shadows and the focus pop are zeroed rather than rounded:
 * there is no half cell for a corner to be round in, nothing under a cell to drop a shadow onto, and a widget that
 * grew by a cell and settled back would make the whole row jump.
 *
 * A caller's own sizes still have to be whole cells — `nya_ui_fixed(300)` is 37 and a half columns and rounds — so
 * `NYA_UICellOptions.cell` is public and a layout written in multiples of it lands exactly.
 *
 * ## How focus reads with no colour and no pointer
 *
 * Two marks, because either one alone is lost somewhere a terminal is used. The whole widget is drawn in reverse
 * video, which is the one emphasis every terminal has: `NYA_TERMINAL_COLOR_NONE` still carries attributes, so a
 * `TERM=dumb` session shows focus when it shows nothing else. And the pair of delimiters a widget is written
 * between — `[ ]` on a button, a field and a slider, `( )` on a radio — becomes `‹ ›` when it has focus, so focus
 * is still readable in a screenshot, in a pipe, and on a terminal that drops SGR entirely. Neither mark takes a
 * column the layout did not already give the widget, which is why there is no focus bar: a bar down the leading
 * edge is one whole column of a row that is only as wide as its text.
 *
 * Held is underline, disabled is dim. Both are attributes for the same reason.
 *
 * ## Unicode, and the terminal that cannot draw it
 *
 * Box drawing, block elements and the small arrows are multi-byte, and every one of them is one code point that
 * occupies one cell. There is no probe for whether a terminal's font has them — a terminal never answers that — so
 * it is the caller's switch: `NYA_UICellOptions.ascii` swaps one table, `_NYA_UI_CELL_GLYPHS`, for its ASCII
 * column, and every glyph this file can draw has an entry in both. A frame becomes `+-+`, a bar becomes `####----`,
 * a chevron becomes `v`. Nothing else changes, so the layout is identical either way: the ASCII fallback is a
 * different table, not a different drawing.
 *
 * Two things to know before leaving it on. Box drawing and the arrows are East Asian *Ambiguous* width: a terminal
 * configured to render ambiguous characters double wide draws a frame two columns per corner and shears the whole
 * screen. And measurement here counts code points, so a label of wide characters measures narrow and draws over
 * its neighbour; that is render2d's terminal backend's behaviour too, and fixing it is one width table for both.
 *
 * ## What is on the grid, and when it reaches the screen
 *
 * The presenter writes into the caller's `NYA_UICells` and nothing else, which is what makes a TUI checkable with
 * no terminal at all: a test reads the cells back, or the whole screen as text. `nya_ui_cells_present` is the one
 * call that touches the terminal, and it writes only the cells this pass wrote — so a program that drew its own
 * bars into a `nya_ui_space` rectangle keeps them, because the UI never wrote there.
 *
 * Two things follow from the grid being the output rather than a step on the way to one. A program's own drawing
 * goes on *after* the present, not during the pass, or the panel a widget sits in covers it. And a window with no
 * terminal under it — a GPU one — shows nothing: this presenter has a screen of characters to hand anybody who
 * asks for it and no way to draw one, which is the shape presenter's job.
 * */
#pragma once

#include "nyangine/base/base_attributes.h"
#include "nyangine/base/base_types.h"
#include "nyangine/platform/terminal/terminal.h"
#include "nyangine/ui/ui_present.h"


// CONSTANTS

/** Columns the grid holds. The terminal's own bound, since that is the widest screen this can ever be shown on. */
#ifndef NYA_UI_CELL_COLUMNS_MAX
#define NYA_UI_CELL_COLUMNS_MAX NYA_TERMINAL_COLUMNS_MAX
#endif

/** Rows the grid holds, by the same reasoning. */
#ifndef NYA_UI_CELL_ROWS_MAX
#define NYA_UI_CELL_ROWS_MAX NYA_TERMINAL_ROWS_MAX
#endif

/** What one cell is in pixels when a caller names none: the terminal's, so a TUI needs no numbers of its own. */
#define NYA_UI_CELL_SIZE ((f32x2){ (f32)NYA_TERMINAL_CELL_WIDTH_PX, (f32)NYA_TERMINAL_CELL_HEIGHT_PX })


// TYPES

typedef struct NYA_UICells       NYA_UICells;
typedef struct NYA_UICellOptions NYA_UICellOptions;

/** What a caller decides about the grid. Zeroed asks for terminal cells and the Unicode glyphs. */
struct NYA_UICellOptions {
    /**
     * One cell in pixels. Zero takes NYA_UI_CELL_SIZE. A cell whose height is not a whole number of its own widths
     * is refused: padding is one number used on both axes, and it can only be whole cells in both when it is.
     * */
    f32x2 cell;

    /** Draw every glyph from the ASCII column of the table instead. What a terminal whose font has no box drawing needs. */
    b8 ascii;
};

/**
 * The grid the cell presenter draws into, which the caller owns and which outlives a pass. Fixed, like everything
 * else in the module: a drawn pass allocates nothing.
 * */
struct NYA_UICells {
    /** Filled by nya_ui_cells_init; passed to nya_ui_presenter_set as nya_ui_cells_presenter(cells). */
    NYA_UIPresenter presenter;

    NYA_UICellOptions options;

    /** The cells themselves, and how far into them this pass reached, so a dump is the screen and not the buffer. */
    NYA_TerminalCell cells[NYA_UI_CELL_ROWS_MAX][NYA_UI_CELL_COLUMNS_MAX];
    u16              used_columns;
    u16              used_rows;

    /**
     * Each cell's layer, biased by one so that zero is a cell this pass has not written.
     *
     * A terminal has no batch to sort after the fact, so the sorting is here: a write wins when its layer is at
     * least the one already in the cell, which is how a raised panel covers what was declared before it and how a
     * widget's own text lands on the fill it just laid down.
     * */
    u8 layers[NYA_UI_CELL_ROWS_MAX][NYA_UI_CELL_COLUMNS_MAX];

    /** The style stack, mirrored, and which depth is current. */
    NYA_UILook looks[NYA_UI_STYLE_DEPTH_MAX + 1];
    u32        depth;

    /** What clip_set and layer_set were last told: the clip in cells, and the layer every write is claimed at. */
    s32 clip_column;
    s32 clip_row;
    s32 clip_columns;
    s32 clip_rows;
    s32 layer;
};


// FUNCTIONS

/**
 * Prepares `cells` and the presenter inside it. The grid is cleared, so a pass drawn before the first reset still
 * reads as an empty screen rather than as whatever the caller's memory held.
 * */
NYA_API void nya_ui_cells_init(NYA_UICells* cells, NYA_UICellOptions options);

/**
 * Throws everything away, the presenter inside it included, so a window still pointing at a grid whose storage is
 * gone asserts at the next pass rather than writing into it.
 * */
NYA_API void nya_ui_cells_deinit(NYA_UICells* cells);

/** Throws the last pass away. Called before each pass; a grid that is never reset keeps what the last one drew. */
NYA_API void nya_ui_cells_reset(NYA_UICells* cells);

NYA_API const NYA_UIPresenter* nya_ui_cells_presenter(NYA_UICells* cells) __attr_no_discard;

/** One cell. A zeroed cell when it is outside the grid or nothing wrote it this pass. */
NYA_API NYA_TerminalCell nya_ui_cells_at(const NYA_UICells* cells, u16 column, u16 row) __attr_no_discard;

/**
 * Writes every cell this pass drew onto the terminal, and nothing else: what the UI did not draw on is left to
 * whatever else the frame put there. A no-op where no terminal is open, so a headless pass costs one walk.
 * */
NYA_API void nya_ui_cells_present(const NYA_UICells* cells);

/**
 * The grid as text, one line per row with the trailing blanks cut, terminated, at most `capacity` bytes including
 * the terminator. How many bytes it wrote. What makes a TUI something a test can read and a person can diff.
 * */
NYA_API u32 nya_ui_cells_write(const NYA_UICells* cells, char* out, u32 capacity);
