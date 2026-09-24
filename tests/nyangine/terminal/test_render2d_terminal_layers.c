/**
 * The terminal backend's sorted cell buffer: whether nya_render2d_layer_set means anything here.
 *
 * A terminal writes cells where they are drawn and has no batch to sort afterwards, so before this the
 * layer was recorded, read back, and changed nothing: two overlapping top level panels came out in call
 * order, and a panel the UI had raised was covered by whatever was declared after it.
 *
 * This is also the first test of any kind against the terminal backend. It compiles against it rather
 * than the GPU one because it lives under tests/nyangine/terminal/; see _test_shares_engine.
 **/

#include "nyangine/nyangine.c"
#include "nyangine/nyangine.h"

/** A colour that is unmistakably itself, so a cell says which draw won. */
#define RED   ((NYA_Color){ 1.0F, 0.0F, 0.0F, 1.0F })
#define GREEN ((NYA_Color){ 0.0F, 1.0F, 0.0F, 1.0F })
#define BLUE  ((NYA_Color){ 0.0F, 0.0F, 1.0F, 1.0F })

/** The cell the tests all aim at, well inside any grid a terminal reports. */
#define AT_COLUMN 3
#define AT_ROW    2

/** That cell in pixels, since render2d speaks pixels and the backend divides them into cells. */
#define AT_X ((f32)(AT_COLUMN * NYA_TERMINAL_CELL_WIDTH_PX))
#define AT_Y ((f32)(AT_ROW * NYA_TERMINAL_CELL_HEIGHT_PX))

static u32 cell_background(void) { return nya_terminal_cell_get(AT_COLUMN, AT_ROW).background; }

/** One cell filled at `layer`. */
static void fill_at(NYA_Window* window, s32 layer, NYA_Color color) {
  nya_render2d_layer_set(window, layer);
  nya_render2d_rect(window, AT_X, AT_Y, (f32)NYA_TERMINAL_CELL_WIDTH_PX, (f32)NYA_TERMINAL_CELL_HEIGHT_PX, color);
}

s32 main(void) {
  _NYA_APP_INSTANCE = (NYA_App){ .initialized = true };

  nya_system_callback_init();
  defer nya_system_callback_deinit();

  /*
   * Opened without a tty: the tests never present, and the backend writes into its own grid whether or
   * not anything is watching. A terminal that refuses to open leaves nothing to test, so that is a skip
   * rather than a failure.
   */
  if (!nya_terminal_open((NYA_TerminalOptions){ .detached = true }).ok) {
    nya_log_warn("No terminal here, skipping the layer tests.");

    return 0;
  }

  defer nya_terminal_close();

  NYA_Window* window = nya_render2d_terminal_window();
  nya_assert(window != nullptr, "the terminal backend has a window");

  // TEST: a higher layer covers a lower one, whichever order they were drawn in
  {
    nya_render2d_terminal_frame_begin(window, NYA_COLOR_BLACK);

    fill_at(window, 0, RED);
    fill_at(window, 1, GREEN);

    const u32 raised_last = cell_background();

    nya_render2d_terminal_frame_begin(window, NYA_COLOR_BLACK);

    // The same two, declared the other way about. This is the case that used to come out wrong.
    fill_at(window, 1, GREEN);
    fill_at(window, 0, RED);

    const u32 raised_first = cell_background();

    nya_check(raised_last == raised_first, "the layer decides, not the call order: got 0x%06X then 0x%06X", raised_last, raised_first);
    nya_check(raised_first == nya_terminal_ink(0.0F, 1.0F, 0.0F), "and the higher layer is what is left, got 0x%06X", raised_first);

    printf("  PASSED\n");
  }

  // TEST: equal layers still paint over each other, or a panel could not draw
  //       its own text onto its own fill
  {
    nya_render2d_terminal_frame_begin(window, NYA_COLOR_BLACK);

    fill_at(window, 2, RED);
    fill_at(window, 2, BLUE);

    nya_check(cell_background() == nya_terminal_ink(0.0F, 0.0F, 1.0F), "the later of two equal layers wins, got 0x%06X", cell_background());

    printf("  PASSED\n");
  }

  // TEST: text obeys the same gate as a fill
  {
    nya_render2d_terminal_frame_begin(window, NYA_COLOR_BLACK);

    fill_at(window, 3, GREEN);

    // Under the fill, so it must not land: a label on a panel below a raised one stays hidden.
    nya_render2d_layer_set(window, 1);
    nya_render2d_text(window, "X", AT_X, AT_Y, RED);

    const NYA_TerminalCell covered = nya_terminal_cell_get(AT_COLUMN, AT_ROW);
    nya_check(covered.codepoint != 'X', "text below the covering layer does not land, got U+%04X", covered.codepoint);

    // And above it, where it must.
    nya_render2d_layer_set(window, 4);
    nya_render2d_text(window, "X", AT_X, AT_Y, RED);

    const NYA_TerminalCell shown = nya_terminal_cell_get(AT_COLUMN, AT_ROW);
    nya_check(shown.codepoint == 'X', "text above it does, got U+%04X", shown.codepoint);

    printf("  PASSED\n");
  }

  // TEST: the layers reset with the clear, so last frame's stack does not defend
  //       a cell nobody has written this frame
  {
    nya_render2d_terminal_frame_begin(window, NYA_COLOR_BLACK);
    fill_at(window, 9, GREEN);

    nya_render2d_terminal_frame_begin(window, NYA_COLOR_BLACK);
    fill_at(window, 0, RED);

    nya_check(cell_background() == nya_terminal_ink(1.0F, 0.0F, 0.0F), "a new frame starts at layer zero, got 0x%06X", cell_background());

    printf("  PASSED\n");
  }

  nya_log_info("PASSED: test_render2d_terminal_layers");

  return nya_check_failures() == 0 ? 0 : 1;
}
