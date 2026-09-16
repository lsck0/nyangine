/**
 * Hot reloading a locale: editing a translation while the game runs replaces the strings.
 **/

// Before the engine, so the watch and the fields it reads are compiled in. See the note above.
#ifndef NYA_ASSET_HOT_RELOAD
#define NYA_ASSET_HOT_RELOAD
#endif

#include "nyangine/nyangine.c"
#include "nyangine/nyangine.h"

#include "generated/strings.h"

#include "SDL3/SDL_init.h"
#include "SDL3/SDL_timer.h"

/** A locale code of its own, so no file in the repository is written to. */
#define FIXTURE_LOCALE "zt"
#define FIXTURE_PATH   "./assets/i18n/" FIXTURE_LOCALE ".json"

/** The key the test moves, chosen because it takes no arguments and so cannot disagree on specifiers. */
#define MOVED_KEY "menu_start"

/**
 * Writes a complete locale: every key of the base, with `MOVED_KEY` set to `moved`.
 * */
static void write_fixture(NYA_ConstCString moved) {
  NYA_Arena*  arena = nya_arena_create(.name = "fixture");
  defer       nya_arena_destroy(arena);
  NYA_String* out = nya_string_create(arena);

  nya_string_extend(out, "{\n");

  for (u32 i = 0; i < NYA_STRING_COUNT; i++) {
    NYA_ConstCString value = nya_string_equals(NYA_STRING_KEYS[i], MOVED_KEY) ? moved : nya_i18n_raw(i);

    // JSON escaping, for the two characters that would otherwise end the string early. The locales in
    // this repository contain neither, which is exactly why it is done here rather than assumed.
    NYA_String* escaped = nya_string_create(arena);
    for (const char* c = value; *c != '\0'; c++) {
      if (*c == '"' || *c == '\\') nya_string_push_back(escaped, '\\');
      nya_string_push_back(escaped, (u8)*c);
    }

    nya_string_extend(out, nya_string_to_cstring(arena, nya_string_sprintf(arena, "  \"%s\": \"%s\"%s\n", NYA_STRING_KEYS[i],
                                                                          nya_string_to_cstring(arena, escaped),
                                                                          i + 1 < NYA_STRING_COUNT ? "," : "")));
  }

  nya_string_extend(out, "}\n");

  NYA_EXPECT(nya_file_write(FIXTURE_PATH, out));
}

/**
 * One frame's worth of the end-of-frame work, which is where both reload passes live.
 * */
static void end_frame(void) {
  nya_app_get()->frame_stats.uptime_ns = nya_clock_get_monotonic_ns();

  nya_event_dispatch((NYA_Event){ .type = NYA_EVENT_FRAME_ENDED });
}

s32 main(void) {
  // No real audio device; nya_system_asset_init opens one otherwise. Same reason as test_asset.c.
  SDL_SetHintWithPriority(SDL_HINT_AUDIO_DRIVER, "dummy", SDL_HINT_OVERRIDE);

  _NYA_APP_INSTANCE = (NYA_App){ .initialized = true };
  b8 sdl_ok         = SDL_Init(0);
  nya_assert(sdl_ok, "SDL_Init failed: %s", SDL_GetError());

  nya_system_callback_init();
  NYA_EXPECT(nya_system_events_init());
  nya_system_asset_init();
  nya_system_i18n_init();

  defer nya_system_i18n_deinit();
  defer nya_system_asset_deinit();
  defer nya_system_events_deinit();
  defer nya_system_callback_deinit();

  /*
   * The base locale first, so write_fixture has real strings and real specifiers to copy, and one
   * fixture for the whole run rather than one per case — see the third case for why its lifetime
   * matters.
   */
  NYA_EXPECT(nya_i18n_load(NYA_I18N_BASE_LOCALE, NYA_STRING_KEYS, NYA_STRING_COUNT));

  write_fixture("before");
  defer (void)remove(FIXTURE_PATH);

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: a locale edited on disk is picked up without reloading anything else
  // ─────────────────────────────────────────────────────────────────────────────
  {
    NYA_EXPECT(nya_i18n_load(FIXTURE_LOCALE, NYA_STRING_KEYS, NYA_STRING_COUNT));

    nya_assert(nya_string_equals(nya_string_menu_start(), "before"), "the fixture loaded, got '%s'", nya_string_menu_start());

    // The edit a translator would make.
    write_fixture("after");

    /*
     * Driven rather than waited on, and it takes more than one frame by design.
     */
    b8 reloaded = false;

    for (u32 frame = 0; frame < 40 && !reloaded; frame++) {
      SDL_Delay(20);
      end_frame();

      reloaded = nya_string_equals(nya_string_menu_start(), "after");
    }

    nya_assert(reloaded, "the edit should have been picked up, got '%s'", nya_string_menu_start());

    // And only that string moved: the reload re-resolved the whole file rather than patching one key,
    // so a key it did not touch has to still be there.
    nya_assert(nya_string_equals(nya_string_menu_quit(), "quit"), "the untouched keys survived, got '%s'", nya_string_menu_quit());

    // Still the same locale. A reload is not a language change, and reporting one would send any UI
    // listening for a locale switch chasing a change that did not happen.
    nya_assert(nya_string_equals(nya_i18n_locale(), FIXTURE_LOCALE), "the locale is unchanged, got '%s'", nya_i18n_locale());

    printf("  PASSED\n");
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: a locale caught half written changes nothing and is retried
  // ─────────────────────────────────────────────────────────────────────────────
  {
    write_fixture("good");

    NYA_EXPECT(nya_i18n_load(FIXTURE_LOCALE, NYA_STRING_KEYS, NYA_STRING_COUNT));
    nya_assert(nya_string_equals(nya_string_menu_start(), "good"));

    /*
     * Truncated JSON, which is what an editor writing a file looks like for a few milliseconds.
     */
    NYA_Arena*  arena = nya_arena_create(.name = "half");
    defer       nya_arena_destroy(arena);
    NYA_String* half = nya_string_from(arena, "{\n  \"menu_start\": \"trunc");
    NYA_EXPECT(nya_file_write(FIXTURE_PATH, half));

    for (u32 frame = 0; frame < 20; frame++) {
      SDL_Delay(20);
      end_frame();

      nya_assert(nya_string_equals(nya_string_menu_start(), "good"), "a failed reload must change nothing, got '%s'",
                 nya_string_menu_start());
    }

    // And the failure is not sticky: finishing the write is picked up, because a reload that failed
    // does not record the timestamp it failed on.
    write_fixture("recovered");

    b8 recovered = false;

    for (u32 frame = 0; frame < 40 && !recovered; frame++) {
      SDL_Delay(20);
      end_frame();

      recovered = nya_string_equals(nya_string_menu_start(), "recovered");
    }

    nya_assert(recovered, "the completed write should have been picked up, got '%s'", nya_string_menu_start());

    printf("  PASSED\n");
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: a locale deleted and written again is picked up rather than given up on
  // ─────────────────────────────────────────────────────────────────────────────
  {
    write_fixture("present");

    b8 present = false;
    for (u32 frame = 0; frame < 40 && !present; frame++) {
      SDL_Delay(20);
      end_frame();
      present = nya_string_equals(nya_string_menu_start(), "present");
    }
    nya_assert(present, "the starting state, got '%s'", nya_string_menu_start());

    /*
     * Deleting the file is how an editor that saves atomically looks from the outside: it writes a
     * temporary and renames it over the target, so for an instant the path is not there.
     */
    (void)remove(FIXTURE_PATH);

    for (u32 frame = 0; frame < 15; frame++) {
      SDL_Delay(20);
      end_frame();
    }

    // The strings survive the file going away. There is nothing better to show than the last thing
    // that parsed, and blanking the UI because a translator's editor was mid-save would be worse.
    nya_assert(nya_string_equals(nya_string_menu_start(), "present"), "a missing file changes nothing, got '%s'",
               nya_string_menu_start());

    write_fixture("restored");

    b8 restored = false;
    for (u32 frame = 0; frame < 60 && !restored; frame++) {
      SDL_Delay(20);
      end_frame();
      restored = nya_string_equals(nya_string_menu_start(), "restored");
    }

    nya_assert(restored, "the re-created file should have been picked up, got '%s'", nya_string_menu_start());

    printf("  PASSED\n");
  }

  printf("PASSED: test_i18n_reload\n");
  return 0;
}
