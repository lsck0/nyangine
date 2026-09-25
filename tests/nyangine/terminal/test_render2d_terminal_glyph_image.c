/**
 * The two calls only the terminal backend has: one character with a terminal's attributes, and a picture
 * through the kitty protocol, which a terminal without it answers by drawing nothing.
 **/

#include "nyangine-core/nyangine.c"
#include "nyangine-core/nyangine.h"

/** The cell the glyphs land in, and its neighbour, which nothing may touch. */
#define AT_COLUMN 4
#define AT_ROW    2
#define AT_X      ((f32)(AT_COLUMN * NYA_TERMINAL_CELL_WIDTH_PX))
#define AT_Y      ((f32)(AT_ROW * NYA_TERMINAL_CELL_HEIGHT_PX))

#define RED ((NYA_Color){ 1.0F, 0.0F, 0.0F, 1.0F })

/** A picture small enough to send, and larger than one cell. */
#define PICTURE_SIDE 4

s32 main(void) {
  _NYA_APP_INSTANCE = (NYA_App){ .initialized = true };

  nya_system_callback_init();
  defer nya_system_callback_deinit();

  if (!nya_terminal_open((NYA_TerminalOptions){ .detached = true }).ok) {
    nya_log_warn("No terminal here, skipping the glyph and image tests.");

    return 0;
  }

  defer nya_terminal_close();

  NYA_Window* window = nya_render2d_terminal_window();
  nya_assert(window != nullptr, "the terminal backend has a window");

  const u32 paper = nya_terminal_ink(0.0F, 0.0F, 0.0F);

  // TEST: a glyph is one cell, with its colour and its attributes, on the paper that was already there
  {
    nya_render2d_terminal_frame_begin(window, NYA_COLOR_BLACK);

    const NYA_TerminalCell neighbour = nya_terminal_cell_get(AT_COLUMN + 1, AT_ROW);

    const u8 attributes = NYA_TERMINAL_ATTRIBUTE_BOLD | NYA_TERMINAL_ATTRIBUTE_UNDERLINE;
    nya_render2d_terminal_glyph(window, AT_X, AT_Y, '!', RED, attributes);

    const NYA_TerminalCell cell = nya_terminal_cell_get(AT_COLUMN, AT_ROW);

    nya_check(cell.codepoint == '!', "the character landed, got U+%04X", cell.codepoint);
    nya_check(cell.foreground == nya_terminal_ink(1.0F, 0.0F, 0.0F), "in its colour");
    nya_check(cell.attributes == attributes, "bold and underlined, got 0x%02X", cell.attributes);
    nya_check(cell.background == paper, "on the paper the frame cleared to");

    const NYA_TerminalCell after = nya_terminal_cell_get(AT_COLUMN + 1, AT_ROW);
    nya_check(after.codepoint == neighbour.codepoint && after.attributes == neighbour.attributes, "and nothing spilled into the next cell");

    printf("  PASSED\n");
  }

  // TEST: every length of UTF-8 comes back as the one code point it encodes
  {
    // two, three and four bytes: an accented letter, a block element, and one past the basic plane.
    const u32 codepoints[] = { 0x00E9U, 0x2588U, 0x1F525U };

    for (u32 i = 0; i < nya_carray_length(codepoints); i++) {
      nya_render2d_terminal_frame_begin(window, NYA_COLOR_BLACK);
      nya_render2d_terminal_glyph(window, AT_X, AT_Y, codepoints[i], RED, NYA_TERMINAL_ATTRIBUTE_NONE);

      const NYA_TerminalCell cell = nya_terminal_cell_get(AT_COLUMN, AT_ROW);
      nya_check(cell.codepoint == codepoints[i], "U+%04X survived the encode, got U+%04X", codepoints[i], cell.codepoint);
      nya_check(cell.attributes == NYA_TERMINAL_ATTRIBUTE_NONE, "with no attributes");
    }

    printf("  PASSED\n");
  }

  // TEST: a terminal without the kitty protocol says so and draws nothing
  {
    static u8 rgba[PICTURE_SIDE * PICTURE_SIDE * 4];
    for (u32 i = 0; i < sizeof(rgba); i++) rgba[i] = 0xFF;

    nya_render2d_terminal_frame_begin(window, NYA_COLOR_BLACK);

    // refused before anything is sent, whatever the terminal can do: there is no cell left of the first.
    nya_check(!nya_render2d_terminal_image(window, -AT_X, AT_Y, rgba, PICTURE_SIDE, PICTURE_SIDE), "a picture off the left edge is refused");

    // the probe goes by the terminal's name, so a suite run inside kitty really has the protocol.
    if (nya_terminal_capabilities().kitty_images) {
      nya_log_warn("This terminal has kitty images, so the refusal without them is not tested here.");
    } else {
      nya_check(!nya_render2d_terminal_image(window, AT_X, AT_Y, rgba, PICTURE_SIDE, PICTURE_SIDE), "without the protocol a picture is refused");
    }

    // a picture never goes into the grid, refused or not.
    const NYA_TerminalCell cell = nya_terminal_cell_get(AT_COLUMN, AT_ROW);
    nya_check(cell.background == paper && (cell.codepoint == 0 || cell.codepoint == ' '), "and the cells under it are as cleared");

    printf("  PASSED\n");
  }

  nya_log_info("PASSED: test_render2d_terminal_glyph_image");

  return nya_check_failures() == 0 ? 0 : 1;
}
