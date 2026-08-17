/**
 * Hot reloading a locale: editing a translation while the game runs replaces the strings.
 *
 * This is the reason i18n moved out of base and onto the asset system. A locale file is registered as
 * a text asset, so nya_asset_get stats it on a throttle and queues it when it changes, and the i18n
 * system's frame hook re-resolves once the timestamp settles.
 *
 * NYA_ASSET_HOT_RELOAD is defined here rather than relied upon, because FLAGS_TEST does not set it —
 * the watch and everything it compares are compiled out of an ordinary test binary, so without this
 * the test would pass by testing nothing. It is defined before the engine is included, which is what
 * makes this the one translation unit in the suite where the reload machinery exists at all.
 *
 * The fixture is generated from NYA_STRING_KEYS and the base locale's own strings rather than written
 * by hand. That is not fastidiousness: the build validates every locale in assets/i18n against the
 * base and fails if one is missing a key or disagrees about a format specifier, so a fixture that
 * outlived a crashing test would otherwise break the next `./build build assets`. Built this way it is
 * a valid locale even if it is left behind.
 **/

// Before the engine, so the watch and the fields it reads are compiled in. See the note above.
#ifndef NYA_ASSET_HOT_RELOAD
#define NYA_ASSET_HOT_RELOAD
#endif

#include "nyangine/nyangine.c"
#include "nyangine/nyangine.h"

#include "assets/strings.h"

#include "SDL3/SDL_init.h"
#include "SDL3/SDL_timer.h"

/** A locale code of its own, so no file in the repository is written to. */
#define FIXTURE_LOCALE "zt"
#define FIXTURE_PATH   "./assets/i18n/" FIXTURE_LOCALE ".json"

/** The key the test moves, chosen because it takes no arguments and so cannot disagree on specifiers. */
#define MOVED_KEY "menu_start"

/**
 * Writes a complete locale: every key of the base, with `MOVED_KEY` set to `moved`.
 *
 * Every *other* value is copied from whatever is loaded right now, which the caller has arranged to be
 * the base locale — so the specifiers match the base by construction rather than by being retyped.
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
 *
 * The clock has to be advanced as well as the event dispatched. nya_asset_get throttles its `stat` on
 * `frame_stats.uptime_ns`, which only the real frame loop writes — leave it at zero and the first
 * lookup sets a deadline a hundred milliseconds into a future that never arrives, so exactly one stat
 * ever happens and every later edit goes unnoticed. A test that dispatched the event alone would show
 * the first reload working and quietly prove nothing about any after it.
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
     *
     * nya_asset_get stats at most once per _NYA_ASSET_STAT_INTERVAL_NS, so the delay is what lets a
     * stat happen at all; _NYA_ASSET_RELOAD_GRACE_FRAMES then postpones the reload, and the reload
     * pass postpones again until it sees the same timestamp twice — which is how it avoids reading a
     * file an editor is still writing. Several frames with a delay between them is the shortest thing
     * that exercises the real path instead of a shortcut through it.
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
     *
     * The parse fails, and the point of the test is what happens next: the strings already loaded stay
     * loaded. A system that committed as it parsed would leave the game with half a language here.
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
     *
     * That instant used to be terminal. A load that cannot open its file leaves the asset FAILED, and
     * the asset system neither stats nor reloads a FAILED asset — so one unlucky save meant no further
     * reload for the rest of the session. _nya_i18n_rearm is what this case exists to hold in place.
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
