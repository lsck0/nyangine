/**
 * The per window render state renderer.c owns: what a window clears to, from creation onwards.
 **/

#include "nyangine/nyangine.c"
#include "nyangine/nyangine.h"

/** Far larger than the stack wants, and the render state lives on it. */
static NYA_Window window;

static b8 color_equals(NYA_Color a, NYA_Color b) {
  return a.r == b.r && a.g == b.g && a.b == b.b && a.a == b.a;
}

s32 main(void) {
  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: a window clears to opaque black, not to the zeroed struct's transparent
  // ─────────────────────────────────────────────────────────────────────────────
  {
    nya_system_renderer_for_window_init(&window);

    // zero alpha would show the desktop through every undrawn pixel of a transparent window.
    nya_check(color_equals(nya_render_clear_color(&window), NYA_COLOR_BLACK), "a new window clears to opaque black");

    printf("  PASSED\n");
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: the colour reads back as set, and setting it touches nothing else
  // ─────────────────────────────────────────────────────────────────────────────
  {
    const NYA_Color clear = { 0.0F, 0.0F, 0.0F, 0.0F };

    // static for the same reason the window is.
    static NYA_RenderSystemWindow expected;
    expected             = window.render_system;
    expected.clear_color = clear;

    nya_render_clear_color_set(&window, clear);

    nya_check(color_equals(nya_render_clear_color(&window), clear), "a transparent clear reads back");
    nya_check(memcmp(&window.render_system, &expected, sizeof(expected)) == 0, "and the window's other render state is untouched");

    const NYA_Color room = { 0.07F, 0.08F, 0.13F, 1.0F };
    nya_render_clear_color_set(&window, room);
    nya_check(color_equals(nya_render_clear_color(&window), room), "a second colour replaces the first");

    printf("  PASSED\n");
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: tearing the window's render state down puts the default back
  // ─────────────────────────────────────────────────────────────────────────────
  {
    nya_system_renderer_for_window_deinit(&window);

    nya_check(color_equals(nya_render_clear_color(&window), NYA_COLOR_BLACK), "a torn down window forgets its colour");

    printf("  PASSED\n");
  }

  nya_log_info("PASSED: test_renderer");

  return nya_check_failures() == 0 ? 0 : 1;
}
