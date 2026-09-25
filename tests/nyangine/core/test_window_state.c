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

#include "nyangine-core/nyangine.c"
#include "nyangine-core/nyangine.h"

#include "SDL3/SDL_init.h"
#include "SDL3/SDL_video.h"

/** A borderless widget's answer: every pixel drags the window. */
static NYA_WindowRegion region_draggable(NYA_WindowHandle window, s32 x, s32 y, void* user_data) {
  nya_unused(window, x, y, user_data);
  return NYA_WINDOW_REGION_DRAGGABLE;
}

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

  // TEST: a handle that is not a window answers rather than crashing
  {
    /* The case a caller reaches by holding a handle across a close, which is the ordinary way to get one of these wrong. Every one of them has to answer. */
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

  // TEST: the count follows what is open
  const u32 before = nya_window_count();

  NYA_WindowHandle window = nya_window_create("state", 320, 240, NYA_WINDOW_RESIZABLE);
  nya_assert(nya_window_is_valid(window), "the window was created");

  {
    nya_check(nya_window_count() == before + 1, "opening one is counted, got " FMTu32 " against " FMTu32, nya_window_count(), before + 1);

    printf("  PASSED\n");
  }

  // TEST: a getter agrees with the flag the window was created with
  {
    /* The one claim that does not depend on a compositor honouring anything: the window asked to be resizable at creation, so the getter has to say so. A getter reading the wrong SDL flag, or reading a different window's flags, fails right here. */
    nya_check(nya_window_is_resizable(window), "a window created resizable reads as resizable");
    nya_check(!nya_window_is_borderless(window), "and not as borderless, which it did not ask for");

    printf("  PASSED\n");
  }

  // TEST: each getter answers about its own window and not about another
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

  // TEST: the setters run and the getters keep answering
  {
    /* Not "the request took". Every one of these is a request a window system may refuse, and the offscreen driver refuses most of them; asserting that fullscreen sticks would be asserting something about the driver rather than about this code. What is asserted is that the pair runs and still answers, which is what catches a wrapper that reads a freed handle. */
    nya_window_set_fullscreen(window, true);
    (void)nya_window_is_fullscreen(window);
    nya_window_set_fullscreen(window, false);

    nya_window_set_borderless(window, true);
    (void)nya_window_is_borderless(window);
    nya_window_set_borderless(window, false);

    nya_window_set_always_on_top(window, true);
    (void)nya_window_is_always_on_top(window);
    nya_window_set_always_on_top(window, false);

    /* Not asserted either way. SDL_SetWindowResizable(false) is ignored by the offscreen driver — probed directly against SDL to be sure whose behaviour that is, and the flag stays set — so an assertion here would be about the driver and would fail on a runner with a real display. */
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

  // TEST: the display queries answer, and their bounds are the right way round
  {
    const NYA_Rect bounds = nya_window_display_bounds(window);
    const NYA_Rect usable = nya_window_display_usable_bounds(window);

    // The offscreen driver has a display; a headless build with none returns a zeroed rect, which is an answer rather than a crash and is the only thing worth asserting for both cases.
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

  // TEST: the cursor, which is four more calls nothing made
  {
    /* nya_cursor_set, nya_cursor, nya_cursor_visible_set and nya_cursor_visible had no caller. The shape is engine state rather than a request the window system can refuse — the header says it is cheap to call every frame with the same value, which only holds if the engine remembers what is set — so unlike the window flags above, this one does assert that it took. */
    const NYA_CursorShape original  = nya_cursor();
    const b8              was_shown = nya_cursor_visible();

    /* Whether a shape sticks is the platform's to decide: the offscreen driver answers "CreateSystemCursor is not currently supported" for every one of them, and the engine's documented answer to that is to warn and keep whatever it had. So the claim here is the one that holds either way — a shape it could not create leaves the previous one intact rather than recording a shape that was never set. */
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

    /* nya_cursor_set_image: a cursor from an image with a hotspot. The argument checks answer whatever the platform can do — a null image, an empty one, or a hotspot outside it are refused up front — so they hold on the offscreen driver and on a real display alike. */
    u8 image[4 * 4 * 4] = { 0 };
    nya_check(!nya_cursor_set_image(nullptr, 4, 4, 0, 0), "a null image is refused");
    nya_check(!nya_cursor_set_image(image, 0, 4, 0, 0), "an image with no width is refused");
    nya_check(!nya_cursor_set_image(image, 4, 4, 4, 0), "a hotspot outside the image is refused");

    /* A valid image either takes or is cleanly refused where the platform cannot build one; both leave the shape answering. What is asserted is that a system shape set after an image is applied rather than skipped as unchanged, which is the bug the image's active flag exists to avoid. */
    (void)nya_cursor_set_image(image, 4, 4, 1, 1);
    nya_cursor_set(NYA_CURSOR_DEFAULT);
    nya_check(nya_cursor() < NYA_CURSOR_COUNT, "a shape set after a custom image still answers, got %d", (int)nya_cursor());

    nya_cursor_set(original);
    nya_cursor_visible_set(was_shown);

    printf("  PASSED\n");
  }

  // TEST: geometry reads back, and requests answer for their own window only
  {
    /* The second cluster the caller rule found: geometry, opacity, grab, flash and sync had no caller. As above, a request the window system may refuse is not asserted to have taken. What is asserted is what holds on any driver: the reads describe a window of the size it was created at, a request answers what nya_window_geometry_is_client_controlled says, and nothing answers for a non-window. */
    u32 width = 0, height = 0, pixel_width = 0, pixel_height = 0;
    nya_window_size(window, &width, &height);
    nya_window_size_in_pixels(window, &pixel_width, &pixel_height);

    nya_check(width == 320 && height == 240, "the logical size is the created one, got " FMTu32 "x" FMTu32, width, height);
    nya_check(pixel_width >= width && pixel_height >= height, "and the pixel size is at least that, got " FMTu32 "x" FMTu32, pixel_width, pixel_height);

    s32 x = 0, y = 0;
    nya_window_position(window, &x, &y);

    const b8 client = nya_window_geometry_is_client_controlled();
    nya_check(nya_window_request_size(window, 300, 200) == client, "a size request answers whether the client controls geometry");
    nya_check(nya_window_request_position(window, x, y) == client, "and so does a position request");
    nya_check(nya_window_request_minimum_size(window, 100, 80) == client, "and a floor");
    nya_check(nya_window_request_maximum_size(window, 2000, 1500) == client, "and a ceiling");
    nya_check(nya_window_request_aspect_ratio(window, 0.0F, 0.0F) == client, "and removing the aspect lock");
    nya_window_sync(window);

    // a handle that is not a window answers false, and writes nothing through its out parameters.
    const NYA_WindowHandle nowhere = { 0 };
    u32                    untouched = 77;
    nya_window_size(nowhere, &untouched, nullptr);
    nya_check(untouched == 77, "a size read from a non-window leaves the out parameter alone");
    nya_check(!nya_window_request_size(nowhere, 1, 1), "and a request to one answers false");

    nya_window_set_opacity(window, 0.5F);
    const f32 opacity = nya_window_opacity(window);
    nya_check(opacity >= 0.0F && opacity <= 1.0F, "opacity reads inside its range, got %f", (f64)opacity);
    nya_window_set_opacity(window, 3.0F);
    nya_check(nya_window_opacity(window) <= 1.0F, "and a value past it is clamped rather than handed to SDL, got %f", (f64)nya_window_opacity(window));
    nya_check(nya_window_opacity(nowhere) == 1.0F, "a non-window is opaque");

    nya_window_set_focusable(window, false);
    nya_window_set_focusable(window, true);

    nya_window_set_mouse_grabbed(window, true);
    (void)nya_window_is_mouse_grabbed(window);
    nya_window_set_mouse_grabbed(window, false);
    nya_check(!nya_window_is_mouse_grabbed(window), "letting go of the pointer reads as let go");

    nya_window_set_relative_mouse(window, true);
    (void)nya_window_is_relative_mouse(window);
    nya_window_set_relative_mouse(window, false);
    nya_check(!nya_window_is_relative_mouse(window), "leaving relative mode reads as left");
    nya_check(!nya_window_is_mouse_grabbed(nowhere) && !nya_window_is_relative_mouse(nowhere), "a non-window grabs nothing");

    nya_window_flash(window, NYA_FLASH_BRIEFLY);
    nya_window_flash(window, NYA_FLASH_CANCEL);

    nya_window_hide(window);
    nya_window_show(window);

    nya_check(nya_window_is_valid(window), "the window survives all of it");

    printf("  PASSED\n");
  }

  // TEST: the display mode and the driver name answer
  {
    const NYA_DisplayMode mode = nya_window_display_mode(window);
    nya_check(mode.width >= 0 && mode.height >= 0 && mode.refresh_rate >= 0.0F, "the display mode is not negative, got %dx%d", mode.width, mode.height);

    const NYA_DisplayMode nothing = nya_window_display_mode((NYA_WindowHandle){ 0 });
    nya_check(nothing.width == 0 && nothing.height == 0, "and a non-window has no display to report");

    nya_check(strcmp(nya_video_driver(), "offscreen") == 0, "the driver is the one this test asked for, got '%s'", nya_video_driver());

    printf("  PASSED\n");
  }

  // TEST: the region callback is what the platform's hit test asks
  {
    /* The platform calls the hit test while the pointer moves, which no test can make it do, so it is called here the way SDL would. Removing the callback must hand the question back to the platform. */
    NYA_Window* target = nya_window_get(window);
    SDL_Point   inside = { 10, 10 };

    nya_window_region_set(window, nya_callback(region_draggable), nullptr);
    nya_check(_nya_window_hit_test(target->sdl_window, &inside, target) == SDL_HITTEST_DRAGGABLE, "the installed callback answers the hit test");

    nya_window_region_set(window, 0, nullptr);
    nya_check(_nya_window_hit_test(target->sdl_window, &inside, target) == SDL_HITTEST_NORMAL, "and with it removed the answer is normal");

    printf("  PASSED\n");
  }

  // TEST: a layer is switched off and on by id, and only that layer
  {
    nya_layer_push(window, _nya_layer_with_id((NYA_Layer){ .enabled = true }, "first"));
    nya_layer_push(window, _nya_layer_with_id((NYA_Layer){ .enabled = true }, "second"));

    nya_layer_disable(window, "first");
    nya_check(!nya_layer_get(window, "first")->enabled, "disabling a layer switches it off");
    nya_check(nya_layer_get(window, "second")->enabled, "and leaves its neighbour on");

    nya_layer_enable(window, "first");
    nya_check(nya_layer_get(window, "first")->enabled, "enabling it switches it back on");

    // an id nothing pushed is not a layer, and asking about it changes nothing.
    nya_layer_disable(window, "absent");
    nya_check(nya_layer_get(window, "absent") == nullptr, "an unknown id stays unknown");

    printf("  PASSED\n");
  }

  // CLEANUP
  nya_window_destroy(window);
  nya_check(nya_window_count() == before, "closing them puts the count back, got " FMTu32, nya_window_count());

  nya_log_info("PASSED: test_window_state");

  return nya_check_failures() == 0 ? 0 : 1;
}
