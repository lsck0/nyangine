/**
 * The cell presenter against a real terminal grid: that what it drew into its own cells is what the terminal ends
 * up holding, and that the cells it did not draw into are left to whatever else the frame put there.
 *
 * The presenter itself is tested without a terminal at all, in tests/nyangine/ui/test_ui_present_cell.c — this is
 * the one step that test cannot take, because nya_ui_cells_present is the only call in the file that leaves the
 * grid. It compiles against the terminal backend rather than the GPU one because it lives under
 * tests/nyangine/terminal/; see _test_shares_engine.
 **/

#include "nyangine/nyangine.c"
#include "nyangine/nyangine.h"

#define CELL_W ((f32)NYA_TERMINAL_CELL_WIDTH_PX)
#define CELL_H ((f32)NYA_TERMINAL_CELL_HEIGHT_PX)

/** Where the program's own rectangle goes: a row the widget below is nowhere near. */
#define OWN_COLUMN 2
#define OWN_ROW    6

/** A colour that is unmistakably itself, so a cell says which draw won. */
#define GREEN ((NYA_Color){ 0.0F, 1.0F, 0.0F, 1.0F })

static NYA_UICells cells;

/** One widget through the presenter, with the look built first, exactly as a pass would. */
static void present(NYA_Window* window, const NYA_UIWidgetDraw* widget) {
    const NYA_UIPresenter* presenter = nya_ui_cells_presenter(&cells);

    NYA_UIStyle style = {
        .padding     = CELL_H,
        .item_height = CELL_H,
        .button      = { .normal = { 0.2F, 0.2F, 0.2F, 1.0F } },
        .text        = { .normal = { 0.9F, 0.9F, 0.9F, 1.0F } },
    };

    NYA_UILook built = { 0 };

    nya_ui_cells_reset(&cells);

    presenter->look_build(presenter->state, 0, &style, 1.0F, &built);
    presenter->look_use(presenter->state, 0);
    presenter->draw(presenter->state, window, widget);
}

s32 main(void) {
    _NYA_APP_INSTANCE = (NYA_App){ .initialized = true };

    nya_system_callback_init();
    defer nya_system_callback_deinit();

    /*
     * Opened without a tty: nothing here is presented to a screen, and the backend writes into its own grid whether
     * or not anything is watching. A terminal that refuses to open leaves nothing to test, so that is a skip rather
     * than a failure.
     */
    if (!nya_terminal_open((NYA_TerminalOptions){ .detached = true }).ok) {
        nya_log_warn("No terminal here, skipping the cell presenter tests.");

        return 0;
    }

    defer nya_terminal_close();

    NYA_Window* window = nya_render2d_terminal_window();
    nya_assert(window != nullptr, "the terminal backend has a window");

    nya_ui_cells_init(&cells, (NYA_UICellOptions){ 0 });
    defer nya_ui_cells_deinit(&cells);

    NYA_UIWidgetDraw button = {
        .kind    = NYA_UI_WIDGET_BUTTON,
        .rect    = { 0.0F, 0.0F, 6.0F * CELL_W, CELL_H },
        .label   = "ok",
        .opacity = 1.0F,
        .clip    = { 0.0F, 0.0F, (f32)window->screen_width, (f32)window->screen_height },
        .text    = NYA_UI_TEXT_BODY,
    };

    // TEST: the screen the presenter drew is the screen the terminal holds
    {
        nya_render2d_terminal_frame_begin(window, NYA_COLOR_BLACK);

        present(window, &button);
        nya_ui_cells_present(&cells);

        nya_check(nya_terminal_cell_get(0, 0).codepoint == '[', "the button's delimiter reached the terminal, got U+%04X", nya_terminal_cell_get(0, 0).codepoint);
        nya_check(nya_terminal_cell_get(2, 0).codepoint == 'o' && nya_terminal_cell_get(3, 0).codepoint == 'k', "and so did its label");
        nya_check(nya_terminal_cell_get(5, 0).codepoint == ']', "and the far delimiter landed on the last cell of the row");

        printf("  PASSED\n");
    }

    // TEST: a cell the UI never drew into is left to whatever else the frame drew
    {
        nya_render2d_terminal_frame_begin(window, NYA_COLOR_BLACK);

        nya_render2d_rect(window, (f32)OWN_COLUMN * CELL_W, (f32)OWN_ROW * CELL_H, CELL_W, CELL_H, GREEN);

        present(window, &button);
        nya_ui_cells_present(&cells);

        nya_check(nya_terminal_cell_get(OWN_COLUMN, OWN_ROW).background == nya_terminal_ink(0.0F, 1.0F, 0.0F),
                  "what the program drew for itself survives the UI's cells, got 0x%06X", nya_terminal_cell_get(OWN_COLUMN, OWN_ROW).background);
        nya_check(nya_terminal_cell_get(0, 0).codepoint == '[', "while the UI's own cells are still there");

        printf("  PASSED\n");
    }

    // TEST: and a cell it did draw into is the UI's, whatever was under it
    {
        nya_render2d_terminal_frame_begin(window, NYA_COLOR_BLACK);

        nya_render2d_rect(window, 0.0F, 0.0F, CELL_W, CELL_H, GREEN);

        present(window, &button);
        nya_ui_cells_present(&cells);

        nya_check(nya_terminal_cell_get(0, 0).codepoint == '[', "the UI is drawn over the frame, not under it, got U+%04X",
                  nya_terminal_cell_get(0, 0).codepoint);

        printf("  PASSED\n");
    }

    return nya_check_failures() == 0 ? 0 : 1;
}
