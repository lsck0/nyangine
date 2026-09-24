/**
 * Localization plurals: CLDR plural-category selection per locale, locale number formatting, and a
 * plural message picking the right variant through the generated accessor.
 **/

#include "nyangine/nyangine.c"
#include "nyangine/nyangine.h"

#include "genyarated/strings.h"

#include "SDL3/SDL_init.h"

// The category a locale picks for a count, named for a compact table of known n -> category vectors.
#define CATEGORY_IS(locale, n, expected)                                                                              \
    nya_assert(                                                                                                       \
        nya_i18n_plural_category((locale), (n)) == (expected), "%s(%lld) is %s, expected %s", (locale), (long long)(n), \
        nya_i18n_plural_category_name(nya_i18n_plural_category((locale), (n))), nya_i18n_plural_category_name(expected) \
    )

s32 main(void) {
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

    // TEST: English — one for exactly one, other for everything else
    {
        CATEGORY_IS("en", 1, NYA_I18N_PLURAL_ONE);
        CATEGORY_IS("en", 0, NYA_I18N_PLURAL_OTHER);
        CATEGORY_IS("en", 2, NYA_I18N_PLURAL_OTHER);
        CATEGORY_IS("en", 5, NYA_I18N_PLURAL_OTHER);
        CATEGORY_IS("en", 11, NYA_I18N_PLURAL_OTHER);
        CATEGORY_IS("en", 100, NYA_I18N_PLURAL_OTHER);
        CATEGORY_IS("en", 21, NYA_I18N_PLURAL_OTHER);

        // The language subtag decides it, so a full tag resolves as its language.
        CATEGORY_IS("en-US", 1, NYA_I18N_PLURAL_ONE);
        CATEGORY_IS("en_GB", 2, NYA_I18N_PLURAL_OTHER);

        // The absolute value is what CLDR selects on, and the most-negative integer must not overflow.
        CATEGORY_IS("en", -1, NYA_I18N_PLURAL_ONE);
        CATEGORY_IS("en", INT64_MIN, NYA_I18N_PLURAL_OTHER);

        // German shares the shape.
        CATEGORY_IS("de", 1, NYA_I18N_PLURAL_ONE);
        CATEGORY_IS("de", 0, NYA_I18N_PLURAL_OTHER);
        CATEGORY_IS("de", 7, NYA_I18N_PLURAL_OTHER);

        printf("  PASSED\n");
    }

    // TEST: French — zero and one are one, a whole million is many
    {
        CATEGORY_IS("fr", 0, NYA_I18N_PLURAL_ONE);
        CATEGORY_IS("fr", 1, NYA_I18N_PLURAL_ONE);
        CATEGORY_IS("fr", 2, NYA_I18N_PLURAL_OTHER);
        CATEGORY_IS("fr", 100, NYA_I18N_PLURAL_OTHER);
        CATEGORY_IS("fr", 1000000, NYA_I18N_PLURAL_MANY);
        CATEGORY_IS("fr", 2000000, NYA_I18N_PLURAL_MANY);
        CATEGORY_IS("fr", 1000001, NYA_I18N_PLURAL_OTHER);

        printf("  PASSED\n");
    }

    // TEST: Russian — last digit, unless the last two are the teens
    {
        CATEGORY_IS("ru", 1, NYA_I18N_PLURAL_ONE);
        CATEGORY_IS("ru", 21, NYA_I18N_PLURAL_ONE);
        CATEGORY_IS("ru", 31, NYA_I18N_PLURAL_ONE);

        CATEGORY_IS("ru", 2, NYA_I18N_PLURAL_FEW);
        CATEGORY_IS("ru", 3, NYA_I18N_PLURAL_FEW);
        CATEGORY_IS("ru", 4, NYA_I18N_PLURAL_FEW);
        CATEGORY_IS("ru", 22, NYA_I18N_PLURAL_FEW);
        CATEGORY_IS("ru", 24, NYA_I18N_PLURAL_FEW);

        CATEGORY_IS("ru", 0, NYA_I18N_PLURAL_MANY);
        CATEGORY_IS("ru", 5, NYA_I18N_PLURAL_MANY);
        CATEGORY_IS("ru", 11, NYA_I18N_PLURAL_MANY);   // teen, not one
        CATEGORY_IS("ru", 12, NYA_I18N_PLURAL_MANY);   // teen, not few
        CATEGORY_IS("ru", 14, NYA_I18N_PLURAL_MANY);
        CATEGORY_IS("ru", 20, NYA_I18N_PLURAL_MANY);
        CATEGORY_IS("ru", 25, NYA_I18N_PLURAL_MANY);
        CATEGORY_IS("ru", 100, NYA_I18N_PLURAL_MANY);
        CATEGORY_IS("ru", 111, NYA_I18N_PLURAL_MANY);  // 11 in the last two digits

        // The absolute value again: -21 is one, like 21.
        CATEGORY_IS("ru_RU", -21, NYA_I18N_PLURAL_ONE);

        printf("  PASSED\n");
    }

    // TEST: Polish — Russian's shape, but one is strictly one
    {
        CATEGORY_IS("pl", 1, NYA_I18N_PLURAL_ONE);
        CATEGORY_IS("pl", 21, NYA_I18N_PLURAL_MANY);   // ends in one, but is not one
        CATEGORY_IS("pl", 2, NYA_I18N_PLURAL_FEW);
        CATEGORY_IS("pl", 4, NYA_I18N_PLURAL_FEW);
        CATEGORY_IS("pl", 22, NYA_I18N_PLURAL_FEW);
        CATEGORY_IS("pl", 12, NYA_I18N_PLURAL_MANY);   // teen
        CATEGORY_IS("pl", 5, NYA_I18N_PLURAL_MANY);
        CATEGORY_IS("pl", 0, NYA_I18N_PLURAL_MANY);

        printf("  PASSED\n");
    }

    // TEST: Czech — one, a few (two to four), otherwise
    {
        CATEGORY_IS("cs", 1, NYA_I18N_PLURAL_ONE);
        CATEGORY_IS("cs", 2, NYA_I18N_PLURAL_FEW);
        CATEGORY_IS("cs", 3, NYA_I18N_PLURAL_FEW);
        CATEGORY_IS("cs", 4, NYA_I18N_PLURAL_FEW);
        CATEGORY_IS("cs", 0, NYA_I18N_PLURAL_OTHER);
        CATEGORY_IS("cs", 5, NYA_I18N_PLURAL_OTHER);
        CATEGORY_IS("cs", 100, NYA_I18N_PLURAL_OTHER);

        printf("  PASSED\n");
    }

    // TEST: Arabic — all six categories
    {
        CATEGORY_IS("ar", 0, NYA_I18N_PLURAL_ZERO);
        CATEGORY_IS("ar", 1, NYA_I18N_PLURAL_ONE);
        CATEGORY_IS("ar", 2, NYA_I18N_PLURAL_TWO);
        CATEGORY_IS("ar", 3, NYA_I18N_PLURAL_FEW);
        CATEGORY_IS("ar", 10, NYA_I18N_PLURAL_FEW);
        CATEGORY_IS("ar", 103, NYA_I18N_PLURAL_FEW);   // 3 in the last two digits
        CATEGORY_IS("ar", 11, NYA_I18N_PLURAL_MANY);
        CATEGORY_IS("ar", 99, NYA_I18N_PLURAL_MANY);
        CATEGORY_IS("ar", 111, NYA_I18N_PLURAL_MANY);  // 11 in the last two digits
        CATEGORY_IS("ar", 100, NYA_I18N_PLURAL_OTHER);
        CATEGORY_IS("ar", 101, NYA_I18N_PLURAL_OTHER);
        CATEGORY_IS("ar", 200, NYA_I18N_PLURAL_OTHER);

        printf("  PASSED\n");
    }

    // TEST: languages with no count distinction, and the fallback for the rest
    {
        // Japanese, Chinese and Korean have one form for every number.
        CATEGORY_IS("ja", 0, NYA_I18N_PLURAL_OTHER);
        CATEGORY_IS("ja", 1, NYA_I18N_PLURAL_OTHER);
        CATEGORY_IS("ja", 2, NYA_I18N_PLURAL_OTHER);
        CATEGORY_IS("zh", 1, NYA_I18N_PLURAL_OTHER);
        CATEGORY_IS("zh-Hans", 5, NYA_I18N_PLURAL_OTHER);
        CATEGORY_IS("ko", 1, NYA_I18N_PLURAL_OTHER);

        // A language whose rules are not spelled out here falls back to other for every count.
        CATEGORY_IS("pt-BR", 1, NYA_I18N_PLURAL_OTHER);
        CATEGORY_IS("xx", 1, NYA_I18N_PLURAL_OTHER);
        CATEGORY_IS("", 1, NYA_I18N_PLURAL_OTHER);

        printf("  PASSED\n");
    }

    // TEST: locale number formatting — separators and grouping
    {
        nya_assert(nya_string_equals(nya_i18n_decimal_separator("en"), "."), "en decimals with a point");
        nya_assert(nya_string_equals(nya_i18n_group_separator("en"), ","), "en groups with a comma");
        nya_assert(nya_string_equals(nya_i18n_decimal_separator("de"), ","), "de decimals with a comma");
        nya_assert(nya_string_equals(nya_i18n_group_separator("de"), "."), "de groups with a point");
        nya_assert(nya_string_equals(nya_i18n_group_separator("fr"), " "), "fr groups with a space");
        nya_assert(nya_string_equals(nya_i18n_decimal_separator("pt-BR"), "."), "an uncovered locale gets the en separators");

        nya_assert(nya_string_equals(nya_i18n_format_integer("en", 1234567), "1,234,567"), "got '%s'", nya_i18n_format_integer("en", 1234567));
        nya_assert(nya_string_equals(nya_i18n_format_integer("en", 0), "0"), "got '%s'", nya_i18n_format_integer("en", 0));
        nya_assert(nya_string_equals(nya_i18n_format_integer("en", 100), "100"), "got '%s'", nya_i18n_format_integer("en", 100));
        nya_assert(nya_string_equals(nya_i18n_format_integer("en", 1000), "1,000"), "got '%s'", nya_i18n_format_integer("en", 1000));
        nya_assert(nya_string_equals(nya_i18n_format_integer("en", -1234), "-1,234"), "got '%s'", nya_i18n_format_integer("en", -1234));
        nya_assert(nya_string_equals(nya_i18n_format_integer("de", 1234567), "1.234.567"), "got '%s'", nya_i18n_format_integer("de", 1234567));
        nya_assert(nya_string_equals(nya_i18n_format_integer("fr", 1234567), "1 234 567"), "got '%s'", nya_i18n_format_integer("fr", 1234567));

        printf("  PASSED\n");
    }

    // TEST: a plural message picks the variant for its count, through the accessor
    {
        NYA_EXPECT(nya_i18n_load("en", NYA_STRING_KEYS, NYA_STRING_COUNT));

        // English one/other, chosen on the count that is also the formatted argument.
        nya_assert(nya_string_equals(nya_string_hud_players(1), "1 player"), "got '%s'", nya_string_hud_players(1));
        nya_assert(nya_string_equals(nya_string_hud_players(0), "0 players"), "got '%s'", nya_string_hud_players(0));
        nya_assert(nya_string_equals(nya_string_hud_players(2), "2 players"), "got '%s'", nya_string_hud_players(2));
        nya_assert(nya_string_equals(nya_string_hud_players(21), "21 players"), "got '%s'", nya_string_hud_players(21));

        // German only distinguishes the label, not the noun here, but the selection still runs.
        NYA_EXPECT(nya_i18n_load("de", NYA_STRING_KEYS, NYA_STRING_COUNT));
        nya_assert(nya_string_equals(nya_string_hud_players(1), "1 Spieler"), "got '%s'", nya_string_hud_players(1));
        nya_assert(nya_string_equals(nya_string_hud_players(3), "3 Spieler"), "got '%s'", nya_string_hud_players(3));

        printf("  PASSED\n");
    }

    // TEST: a category the locale omits falls back to its other variant
    {
        // Bytes that give only `other` for the plural key: Russian would want one/few/many, and every one of them must resolve to the single variant supplied rather than to a placeholder.
        static const char document[] = "{\n"
                                        "  \"hud_players\": { \"other\": \"%u ludzi\" }\n"
                                        "}\n";

        NYA_EXPECT(nya_i18n_load_bytes("ru", (const u8*)document, sizeof(document) - 1, NYA_STRING_KEYS, NYA_STRING_COUNT));

        nya_assert(nya_string_equals(nya_string_hud_players(1), "1 ludzi"), "one falls back to other, got '%s'", nya_string_hud_players(1));
        nya_assert(nya_string_equals(nya_string_hud_players(2), "2 ludzi"), "few falls back to other, got '%s'", nya_string_hud_players(2));
        nya_assert(nya_string_equals(nya_string_hud_players(5), "5 ludzi"), "many falls back to other, got '%s'", nya_string_hud_players(5));

        printf("  PASSED\n");
    }

    printf("PASSED: test_i18n_plural\n");
    return 0;
}
