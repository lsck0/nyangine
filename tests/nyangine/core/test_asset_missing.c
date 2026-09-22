/**
 * What a draw call does with a handle that has nothing behind it.
 *
 * A failed asset used to draw nothing at all, which on a screen is indistinguishable from a draw that
 * was never made, from a sprite behind the camera, and from a tint of zero alpha. The two halves of the
 * answer are here: nya_asset_is_missing, which separates "not there" from "not there yet", and
 * nya_asset_missing_report, which says so once rather than once a frame.
 *
 * The magenta itself is not tested. Reaching the geometry needs a GPU; see test_debug_physics.c for the
 * same wall and the same reason.
 **/

#include "nyangine/nyangine.c"
#include "nyangine/nyangine.h"

#include "SDL3/SDL_init.h"

/** Counts the warnings this file provokes, so "once" can be held to account. */
static u32 warnings = 0;

static void count_warnings(NYA_LogLevel level, NYA_ConstCString message, u32 length, void* user_data) {
  nya_unused(message, length, user_data);

  if (level == NYA_LOG_LEVEL_WARN) warnings++;
}

s32 main(void) {
  SDL_SetHintWithPriority(SDL_HINT_VIDEO_DRIVER, "offscreen", SDL_HINT_OVERRIDE);

  _NYA_APP_INSTANCE = (NYA_App){ .initialized = true };
  b8 sdl_ok         = SDL_Init(0);
  nya_assert(sdl_ok, "SDL_Init failed: %s", SDL_GetError());

  nya_system_callback_init();
  NYA_EXPECT(nya_system_events_init());
  nya_system_asset_init();

  defer nya_system_asset_deinit();
  defer nya_system_events_deinit();
  defer nya_system_callback_deinit();

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: a handle nothing was ever loaded for is missing
  // ─────────────────────────────────────────────────────────────────────────────
  {
    nya_check(nya_asset_is_missing("./assets/texture/there_is_no_such_file.png"), "a handle with nothing behind it is missing");
    nya_check(nya_asset_status("./assets/texture/there_is_no_such_file.png") == NYA_ASSET_STATUS_UNLOADED, "and reports unloaded");

    printf("  PASSED\n");
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: the warning comes once per handle, however many draws ask
  // ─────────────────────────────────────────────────────────────────────────────
  {
    nya_asset_missing_forget();
    nya_log_sink_add(count_warnings, nullptr);

    warnings = 0;

    for (u32 i = 0; i < 60; i++) nya_asset_missing_report("./assets/texture/missing_a.png");

    nya_check(warnings == 1, "sixty draws of one broken handle warn once, got " FMTu32, warnings);

    // A second handle is a second thing wrong, and says so.
    nya_asset_missing_report("./assets/texture/missing_b.png");
    nya_check(warnings == 2, "a different handle warns too, got " FMTu32, warnings);

    nya_asset_missing_report("./assets/texture/missing_a.png");
    nya_check(warnings == 2, "and the first one still does not warn again, got " FMTu32, warnings);

    // What a reload calls: the asset may have been fixed, so it is allowed to complain again.
    nya_asset_missing_forget();
    nya_asset_missing_report("./assets/texture/missing_a.png");
    nya_check(warnings == 3, "after forgetting, it warns once more, got " FMTu32, warnings);

    (void)nya_log_sink_remove(count_warnings, nullptr);

    printf("  PASSED\n");
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: past the ceiling it keeps warning rather than going quiet about a
  //       handle it never recorded
  // ─────────────────────────────────────────────────────────────────────────────
  {
    nya_asset_missing_forget();
    nya_log_sink_add(count_warnings, nullptr);

    warnings = 0;

    u8 handle[64] = { 0 };
    for (u32 i = 0; i < _NYA_ASSET_MISSING_REPORTED_MAX + 4; i++) {
      (void)snprintf((char*)handle, sizeof(handle), "./assets/texture/missing_%u.png", i);
      nya_asset_missing_report((NYA_ConstCString)handle);
    }

    nya_check(warnings == _NYA_ASSET_MISSING_REPORTED_MAX + 4, "every distinct handle warns, got " FMTu32, warnings);

    // The four past the ceiling were never recorded, so they warn again. Repeating is the right way to
    // be wrong here: going quiet would hide a handle nobody has been told about.
    (void)snprintf((char*)handle, sizeof(handle), "./assets/texture/missing_%u.png", _NYA_ASSET_MISSING_REPORTED_MAX + 3);
    nya_asset_missing_report((NYA_ConstCString)handle);

    nya_check(warnings == _NYA_ASSET_MISSING_REPORTED_MAX + 5, "a handle past the ceiling repeats rather than going quiet, got " FMTu32, warnings);

    (void)nya_log_sink_remove(count_warnings, nullptr);
    nya_asset_missing_forget();

    printf("  PASSED\n");
  }

  nya_log_info("PASSED: test_asset_missing");

  return nya_check_failures() == 0 ? 0 : 1;
}
