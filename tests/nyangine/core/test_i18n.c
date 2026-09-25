/**
 * Localization: loading a locale, falling back, and formatting through the generated accessors.
 **/

#include "nyangine-core/nyangine.c"
#include "nyangine-core/nyangine.h"

#include "genyarated/strings.h"

#include "SDL3/SDL_init.h"

s32 main(void) {
  _NYA_APP_INSTANCE = (NYA_App){ .initialized = true };
  b8 sdl_ok         = SDL_Init(0);
  nya_assert(sdl_ok, "SDL_Init failed: %s", SDL_GetError());

  // What the asset system needs, rather than nya_app_init: an arena, and somewhere to register its frame-ended hooks, which is the event system, which needs the callback system.
  nya_system_callback_init();
  NYA_EXPECT(nya_system_events_init());
  nya_system_asset_init();
  nya_system_i18n_init();

  defer nya_system_i18n_deinit();
  defer nya_system_asset_deinit();
  defer nya_system_events_deinit();
  defer nya_system_callback_deinit();

  // TEST: nothing loaded shows a findable placeholder
  {
    NYA_ConstCString missing = nya_i18n_raw(NYA_STRING_MENU_START);

    // Not an empty string. An empty one is indistinguishable from a label meant to be blank, so a missing translation would show as a gap nobody investigates; this shows on screen and says which key to add.
    nya_assert(missing != nullptr && missing[0] == '[', "an unloaded id reads as a placeholder, got '%s'", missing);
    nya_assert(nya_string_equals(nya_i18n_locale(), ""), "and no locale is loaded");

    printf("  PASSED\n");
  }

  // TEST: the base locale loads and the accessors read it
  {
    NYA_EXPECT(nya_i18n_load("en", NYA_STRING_KEYS, NYA_STRING_COUNT));

    nya_assert(nya_string_equals(nya_i18n_locale(), "en"), "the locale is en");

    // A key with no arguments returns the stored string directly.
    nya_assert(nya_string_equals(nya_string_menu_start(), "start"), "got '%s'", nya_string_menu_start());
    nya_assert(nya_string_equals(nya_string_hud_paused(), "PHYSICS PAUSED"), "got '%s'", nya_string_hud_paused());

    /* The typed accessors, which is the whole design. */
    nya_assert(nya_string_equals(nya_string_hud_greeting("Ada"), "Hello, Ada!"), "got '%s'", nya_string_hud_greeting("Ada"));
    nya_assert(nya_string_equals(nya_string_hud_score("Ada", 4200), "Ada scored 4200 points"), "got '%s'", nya_string_hud_score("Ada", 4200));
    nya_assert(nya_string_equals(nya_string_hud_boxes(12, 3), "boxes 12 (3 awake)"), "got '%s'", nya_string_hud_boxes(12, 3));

    printf("  PASSED\n");
  }

  // TEST: switching language changes every string, including the formatted ones
  {
    NYA_EXPECT(nya_i18n_load("de", NYA_STRING_KEYS, NYA_STRING_COUNT));

    nya_assert(nya_string_equals(nya_i18n_locale(), "de"), "the locale is de");
    nya_assert(nya_string_equals(nya_string_menu_start(), "starten"), "got '%s'", nya_string_menu_start());

    // Non-ASCII, which is the other half of this working at all: `hauptmenü` is nine characters and ten bytes, and a renderer that could not draw the tenth would show a gap.
    nya_assert(nya_string_equals(nya_string_menu_main_menu(), "hauptmenü"), "got '%s'", nya_string_menu_main_menu());
    nya_assert(nya_utf8_count(nya_string_menu_main_menu()) == 9, "nine characters, ten bytes");

    // The same call site, the same arguments, a different language. Nothing at the call site changed.
    nya_assert(nya_string_equals(nya_string_hud_score("Ada", 4200), "Ada erzielte 4200 Punkte"), "got '%s'", nya_string_hud_score("Ada", 4200));

    NYA_EXPECT(nya_i18n_load("en", NYA_STRING_KEYS, NYA_STRING_COUNT));
    nya_assert(nya_string_equals(nya_string_menu_start(), "start"), "and switching back works");

    printf("  PASSED\n");
  }

  // TEST: a locale that is not there leaves the loaded one alone
  {
    NYA_Error error = nya_i18n_load("zz", NYA_STRING_KEYS, NYA_STRING_COUNT);

    nya_assert(!error.ok, "a missing locale fails");

    // And changes nothing. A player's saved language preference may name a locale a later build dropped, and the right response is to keep speaking the language already loaded.
    nya_assert(nya_string_equals(nya_i18n_locale(), "en"), "the loaded locale is untouched, got '%s'", nya_i18n_locale());
    nya_assert(nya_string_equals(nya_string_menu_start(), "start"), "and so are its strings");

    printf("  PASSED\n");
  }

  // TEST: the format ring holds several strings at once, then recycles
  {
    NYA_ConstCString first  = nya_string_hud_greeting("one");
    NYA_ConstCString second = nya_string_hud_greeting("two");

    // Two at once, which is what makes the accessors usable inline in one draw call.
    nya_assert(nya_string_equals(first, "Hello, one!"), "got '%s'", first);
    nya_assert(nya_string_equals(second, "Hello, two!"), "got '%s'", second);

    // And exactly NYA_I18N_FORMAT_SLOTS later the first is gone. Documented rather than defended: this is what makes the call free of an arena and a free, and is why nothing should store one.
    for (u32 i = 0; i < NYA_I18N_FORMAT_SLOTS; i++) (void)nya_string_hud_greeting("filler");

    nya_assert(nya_string_equals(first, "Hello, filler!"), "the ring recycled, got '%s'", first);

    printf("  PASSED\n");
  }

  // TEST: a locale from bytes replaces the file's, with no fallback and no watch
  {
    // what a mod or a download hands over: jsonc, and only some of the keys.
    static const char document[] = "{\n"
                                   "  // a translator's note, which the file loader allows too\n"
                                   "  \"menu_start\": \"begin\",\n"
                                   "  \"hud_greeting\": \"Ahoy, %s!\"\n"
                                   "}\n";

    NYA_EXPECT(nya_i18n_load_bytes("xx", (const u8*)document, sizeof(document) - 1, NYA_STRING_KEYS, NYA_STRING_COUNT));

    nya_assert(nya_string_equals(nya_i18n_locale(), "xx"), "the locale is the one named, got '%s'", nya_i18n_locale());
    nya_assert(nya_string_equals(nya_string_menu_start(), "begin"), "got '%s'", nya_string_menu_start());
    nya_assert(nya_string_equals(nya_string_hud_greeting("Ada"), "Ahoy, Ada!"), "got '%s'", nya_string_hud_greeting("Ada"));

    // no fallback: a key the bytes left out is the placeholder, not the English that was loaded before.
    NYA_ConstCString quit = nya_i18n_raw(NYA_STRING_MENU_QUIT);
    nya_assert(quit[0] == '[', "a missing key falls back to nothing, got '%s'", quit);

    // nothing watched: the file the en load registered is let go, so editing it cannot reload over these.
    nya_assert(nya_app_get()->i18n_system.handle == nullptr, "the previous locale file is still watched");
    nya_assert(nya_app_get()->i18n_system.fallback_handle == nullptr, "and so is its fallback");

    // bad bytes change nothing, the same as a missing file.
    static const char broken[] = "{ \"menu_start\": ";
    nya_assert(!nya_i18n_load_bytes("yy", (const u8*)broken, sizeof(broken) - 1, NYA_STRING_KEYS, NYA_STRING_COUNT).ok, "a truncated document fails");
    nya_assert(!nya_i18n_load_bytes("yy", (const u8*)document, sizeof(document) - 1, NYA_STRING_KEYS, 0).ok, "and so does a locale with no keys");

    nya_assert(nya_string_equals(nya_i18n_locale(), "xx"), "a failed load keeps the locale, got '%s'", nya_i18n_locale());
    nya_assert(nya_string_equals(nya_string_menu_start(), "begin"), "and its strings, got '%s'", nya_string_menu_start());

    printf("  PASSED\n");
  }

  printf("PASSED: test_i18n\n");
  return 0;
}
