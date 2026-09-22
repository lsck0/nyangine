/**
 * The window state queries, against the setters they pair with.
 *
 * The whole `nya_window_is_*` family plus the display queries were defined and called by nothing in the
 * tree — thirteen of the audit's dead functions, and the largest cluster after Steam. They are thin
 * wrappers over SDL, which is exactly why nobody thought to check them: a thin wrapper that asks the
 * wrong question, or asks about the wrong window, looks like working code and reads like working code.
 *
 * What is under test is the pairing, not SDL. A compositor may refuse any of these — that is what the
 * header says about mouse grab and it is true of the rest — so a request that does not take is not a
 * failure here. What would be a failure is a getter that answers about the wrong window, contradicts a
 * flag the window was created with, or crashes on a handle that is not a window at all.
 *
 * Runs on the offscreen video driver, so there is no display and no compositor to refuse anything. The
 * cases that need a real one say so rather than pretending.
 **/

#include "nyangine/nyangine.c"
#include "nyangine/nyangine.h"

#include "SDL3/SDL_init.h"
#include "SDL3/SDL_video.h"

s32 main(void) {
  // No display on CI, and nothing here is ever presented.
  SDL_SetHintWithPriority(SDL_HINT_VIDEO_DRIVER, "offscreen", SDL_HINT_OVERRIDE);

  _NYA_APP_INSTANCE = (NYA_App){ .initialized = true };

  b8 sdl_ok = SDL_Init(0);
  nya_assert(sdl_ok, "SDL_Init failed: %s", SDL_GetError());

  nya_system_callback_init();
  NYA_EXPECT(nya_system_events_init());
  nya_system_window_init();

  defer nya_system_window_deinit();
  defer nya_system_events_deinit();
  defer nya_system_callback_deinit();

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: a handle that is not a window answers rather than crashing
  // ─────────────────────────────────────────────────────────────────────────────
  {
    /*
     * The case a caller reaches by holding a handle across a close, which is the ordinary way to get
     * one of these wrong. Every one of them has to answer.
     */
    const NYA_WindowHandle nowhere = { 0 };

    nya_check(!nya_window_is_valid(nowhere), "a zeroed handle is not a window");

    nya_check(!nya_window_is_fullscreen(nowhere), "fullscreen answers false");
    nya_check(!nya_window_is_maximized(nowhere), "maximized answers false");
    nya_check(!nya_window_is_minimized(nowhere), "minimized answers false");
    nya_check(!nya_window_is_visible(nowhere), "visible answers false");
    nya_check(!nya_window_is_occluded(nowhere), "occluded answers false");
    nya_check(!nya_window_is_resizable(nowhere), "resizable answers false");
    nya_check(!nya_window_is_borderless(nowhere), "borderless answers false");
    nya_check(!nya_window_is_always_on_top(nowhere), "always on top answers false");
    nya_check(!nya_window_has_focus(nowhere), "focus answers false");

    printf("  PASSED\n");
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: the count follows what is open
  // ─────────────────────────────────────────────────────────────────────────────
  const u32 before = nya_window_count();

  NYA_WindowHandle window = nya_window_create("state", 320, 240, NYA_WINDOW_RESIZABLE);
  nya_assert(nya_window_is_valid(window), "the window was created");

  {
    nya_check(nya_window_count() == before + 1, "opening one is counted, got " FMTu32 " against " FMTu32, nya_window_count(), before + 1);

    printf("  PASSED\n");
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: a getter agrees with the flag the window was created with
  // ─────────────────────────────────────────────────────────────────────────────
  {
    /*
     * The one claim that does not depend on a compositor honouring anything: the window asked to be
     * resizable at creation, so the getter has to say so. A getter reading the wrong SDL flag, or
     * reading a different window's flags, fails right here.
     */
    nya_check(nya_window_is_resizable(window), "a window created resizable reads as resizable");
    nya_check(!nya_window_is_borderless(window), "and not as borderless, which it did not ask for");

    printf("  PASSED\n");
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: each getter answers about its own window and not about another
  // ─────────────────────────────────────────────────────────────────────────────
  {
    NYA_WindowHandle other = nya_window_create("other", 160, 120, NYA_WINDOW_NONE);
    nya_assert(nya_window_is_valid(other), "the second window was created");

    // Created without it, so the two have to disagree. A getter that ignores its argument cannot.
    nya_check(nya_window_is_resizable(window), "the resizable one still reads resizable");
    nya_check(!nya_window_is_resizable(other), "and the other one does not");

    nya_window_set_title(other, "renamed");
    nya_check(strcmp(nya_window_title(other), "renamed") == 0, "a title reaches its own window, got '%s'", nya_window_title(other));
    nya_check(strcmp(nya_window_title(window), "state") == 0, "and leaves the other alone, got '%s'", nya_window_title(window));

    nya_window_destroy(other);
    nya_check(!nya_window_is_valid(other), "a destroyed window stops being valid");
    nya_check(nya_window_is_valid(window), "and takes no other window with it");

    printf("  PASSED\n");
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: the setters run and the getters keep answering
  // ─────────────────────────────────────────────────────────────────────────────
  {
    /*
     * Not "the request took". Every one of these is a request a window system may refuse, and the
     * offscreen driver refuses most of them; asserting that fullscreen sticks would be asserting
     * something about the driver rather than about this code. What is asserted is that the pair runs
     * and still answers, which is what catches a wrapper that reads a freed handle.
     */
    nya_window_set_fullscreen(window, true);
    (void)nya_window_is_fullscreen(window);
    nya_window_set_fullscreen(window, false);

    nya_window_set_borderless(window, true);
    (void)nya_window_is_borderless(window);
    nya_window_set_borderless(window, false);

    nya_window_set_always_on_top(window, true);
    (void)nya_window_is_always_on_top(window);
    nya_window_set_always_on_top(window, false);

    /*
     * Not asserted either way. SDL_SetWindowResizable(false) is ignored by the offscreen driver —
     * probed directly against SDL to be sure whose behaviour that is, and the flag stays set — so an
     * assertion here would be about the driver and would fail on a runner with a real display.
     */
    nya_window_set_resizable(window, false);
    (void)nya_window_is_resizable(window);
    nya_window_set_resizable(window, true);

    nya_window_show(window);
    nya_window_maximize(window);
    nya_window_restore(window);
    nya_window_minimize(window);
    nya_window_restore(window);
    nya_window_raise(window);

    nya_check(nya_window_is_valid(window), "the window survives being told to do all of that");

    printf("  PASSED\n");
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: the display queries answer, and their bounds are the right way round
  // ─────────────────────────────────────────────────────────────────────────────
  {
    const NYA_Rect bounds = nya_window_display_bounds(window);
    const NYA_Rect usable = nya_window_display_usable_bounds(window);

    // The offscreen driver has a display; a headless build with none returns a zeroed rect, which is
    // an answer rather than a crash and is the only thing worth asserting for both cases.
    nya_check(bounds.width >= 0 && bounds.height >= 0, "the display bounds are not negative, got %dx%d", bounds.width, bounds.height);
    nya_check(usable.width >= 0 && usable.height >= 0, "and neither is the usable area, got %dx%d", usable.width, usable.height);

    // The usable area is the display minus whatever the desktop keeps, so it cannot be the larger.
    if (bounds.width > 0 && usable.width > 0) {
      nya_check(usable.width <= bounds.width, "the usable area is not wider than the display, %d against %d", usable.width, bounds.width);
      nya_check(usable.height <= bounds.height, "nor taller, %d against %d", usable.height, bounds.height);
    }

    nya_check(nya_window_display_name(window) != nullptr, "the display has a name, even if it is a placeholder");
    nya_check(nya_window_display_scale(window) > 0.0F, "and a scale above zero, got %f", (f64)nya_window_display_scale(window));

    printf("  PASSED\n");
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: the cursor, which is four more calls nothing made
  // ─────────────────────────────────────────────────────────────────────────────
  {
    /*
     * nya_cursor_set, nya_cursor, nya_cursor_visible_set and nya_cursor_visible had no caller. The
     * shape is engine state rather than a request the window system can refuse — the header says it
     * is cheap to call every frame with the same value, which only holds if the engine remembers what
     * is set — so unlike the window flags above, this one does assert that it took.
     */
    const NYA_CursorShape original  = nya_cursor();
    const b8              was_shown = nya_cursor_visible();

    /*
     * Whether a shape sticks is the platform's to decide: the offscreen driver answers
     * "CreateSystemCursor is not currently supported" for every one of them, and the engine's
     * documented answer to that is to warn and keep whatever it had. So the claim here is the one
     * that holds either way — a shape it could not create leaves the previous one intact rather than
     * recording a shape that was never set.
     */
    for (u32 shape = 0; shape < NYA_CURSOR_COUNT; shape++) {
      nya_cursor_set((NYA_CursorShape)shape);

      const NYA_CursorShape now = nya_cursor();
      nya_check(now == (NYA_CursorShape)shape || now == original, "cursor shape " FMTu32 " either took or was left alone, got %d", shape,
                (int)now);
    }

    // An out of range shape is refused outright, whatever the platform can do.
    nya_cursor_set(NYA_CURSOR_COUNT);
    nya_check(nya_cursor() < NYA_CURSOR_COUNT, "an out of range shape is refused, got %d", (int)nya_cursor());

    // Setting the same shape twice is the every-frame case the header invites, and must not drift.
    const NYA_CursorShape twice_before = nya_cursor();
    nya_cursor_set(NYA_CURSOR_WAIT);
    nya_cursor_set(NYA_CURSOR_WAIT);
    nya_check(nya_cursor() == NYA_CURSOR_WAIT || nya_cursor() == twice_before, "setting the same shape twice does not drift");

    nya_cursor_visible_set(false);
    nya_check(!nya_cursor_visible(), "the pointer can be hidden");

    nya_cursor_visible_set(true);
    nya_check(nya_cursor_visible(), "and shown again");

    nya_cursor_set(original);
    nya_cursor_visible_set(was_shown);

    printf("  PASSED\n");
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // CLEANUP
  // ─────────────────────────────────────────────────────────────────────────────
  nya_window_destroy(window);
  nya_check(nya_window_count() == before, "closing them puts the count back, got " FMTu32, nya_window_count());

  nya_log_info("PASSED: test_window_state");

  return nya_check_failures() == 0 ? 0 : 1;
}
