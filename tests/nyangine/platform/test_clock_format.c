/**
 * RFC 3339 and RFC 9110 dates both ways: clock_format.h. The RFCs' own examples and every refusal rule
 * as tables, the round trips as property tests. The fuzz targets in tests/fuzz hold the parsers to
 * hostile input; this holds them to the grammar.
 **/

#include "nyangine/nyangine.h"

#include "nyangine/nyangine.c"

/** Cases per law. */
#define CASES 4000

/** Fixed, so the suite is the same run every time. */
#define SEED 0x72666333333339ULL

/** Offsets the offset law draws, in minutes either side of UTC: RFC 3339 allows up to 23:59. */
#define OFFSET_MINUTES_MAX (23 * 60 + 59)

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * HELPERS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

typedef struct {
    NYA_ConstCString text;
    NYA_TimeParse    result;
    u64              position;

    /** Seconds and nanoseconds since the epoch, when `result` is OK. */
    s64 seconds;
    u32 nanoseconds;
} ParseCase;

typedef NYA_TimeParse (*ParseFn)(const u8* text, u64 length, OUT NYA_Instant* out_instant, OUT u64* out_position);

static void check_parse_cases(NYA_ConstCString format, ParseFn parse, const ParseCase* cases, u32 count) {
    for (u32 i = 0; i < count; i++) {
        const ParseCase* expected = &cases[i];

        NYA_Instant   instant  = { .ns = 7 };
        u64           position = U64_MAX;
        NYA_TimeParse result   = parse((const u8*)expected->text, strlen(expected->text), &instant, &position);

        nya_assert(
            result == expected->result,
            "%s '%s': %s, expected %s",
            format,
            expected->text,
            nya_time_parse_text(result),
            nya_time_parse_text(expected->result)
        );
        nya_assert(
            position == expected->position,
            "%s '%s': failed at byte %llu, expected %llu",
            format,
            expected->text,
            (unsigned long long)position,
            (unsigned long long)expected->position
        );

        if (result == NYA_TIME_PARSE_OK) {
            // wide, because the earliest instant's whole second is before the earliest instant.
            s128 ns = (s128)expected->seconds * NYA_NS_PER_SECOND + expected->nanoseconds;
            nya_assert((s128)instant.ns == ns, "%s '%s': %lld ns", format, expected->text, (long long)instant.ns);
        } else {
            nya_assert(instant.ns == 7, "%s '%s': a refused parse wrote its output anyway", format, expected->text);
        }
    }
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * LAWS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/** Any instant written as RFC 3339 parses back to exactly itself. */
static b8 law_rfc3339_round_trips(NYA_Property* property) {
    NYA_Instant instant = { .ns = (s64)nya_property_draw_u64(property) };

    // whole seconds and whole milliseconds are what real stamps mostly are, and they take other branches.
    u8 shape = nya_property_draw_u8(property) % 3;
    if (shape == 1) instant.ns -= instant.ns % NYA_NS_PER_SECOND;
    if (shape == 2) instant.ns -= instant.ns % 1'000'000;

    u8  text[NYA_RFC3339_LENGTH_MAX + 1] = { 0 };
    u32 length                           = nya_instant_to_rfc3339(instant, text, sizeof(text));

    NYA_Instant   again    = { 0 };
    u64           position = 0;
    NYA_TimeParse result   = nya_instant_from_rfc3339(text, length, &again, &position);

    nya_property_note(property, "%lld ns wrote '%s', which parsed as %s", (long long)instant.ns, (const char*)text, nya_time_parse_text(result));
    return result == NYA_TIME_PARSE_OK && again.ns == instant.ns && position == length && text[length - 1] == 'Z';
}

/**
 * The same instant written in any offset's local time parses to the same instant. Composed by hand here
 * rather than by the writer, which only writes Z.
 * */
static b8 law_rfc3339_offsets_agree(NYA_Property* property) {
    // a day inside either end, so the local time is still a date the instant range can come back from.
    NYA_Instant instant = { .ns = (s64)(nya_property_draw_u64(property) % (u64)(S64_MAX - NYA_NS_PER_DAY)) };
    if (nya_property_draw_bool(property, 50)) instant.ns = -instant.ns;

    s64 minutes = (s64)nya_property_draw_below(property, OFFSET_MINUTES_MAX + 1);
    if (nya_property_draw_bool(property, 50)) minutes = -minutes;

    NYA_Date      day  = { 0 };
    NYA_TimeOfDay time = { 0 };
    nya_instant_to_utc(nya_instant_add_duration(instant, nya_duration_from_s(minutes * 60)), &day, &time);

    s64  magnitude = minutes < 0 ? -minutes : minutes;
    char text[64]  = { 0 };
    s32  length    = snprintf(
        text,
        sizeof(text),
        "%04d-%02u-%02uT%02u:%02u:%02u.%09u%c%02lld:%02lld",
        day.year,
        day.month,
        day.day,
        time.hour,
        time.minute,
        time.second,
        time.nanosecond,
        minutes < 0 ? '-' : '+',
        (long long)(magnitude / 60),
        (long long)(magnitude % 60)
    );

    NYA_Instant   again    = { 0 };
    u64           position = 0;
    NYA_TimeParse result   = nya_instant_from_rfc3339((const u8*)text, (u64)length, &again, &position);

    nya_property_note(
        property,
        "'%s' parsed as %s, %lld ns against %lld",
        text,
        nya_time_parse_text(result),
        (long long)again.ns,
        (long long)instant.ns
    );
    return result == NYA_TIME_PARSE_OK && again.ns == instant.ns;
}

/** Any instant written as IMF-fixdate parses back to itself with the fraction dropped toward the past. */
static b8 law_rfc9110_round_trips(NYA_Property* property) {
    NYA_Instant instant = { .ns = (s64)nya_property_draw_u64(property) };

    u8  text[NYA_RFC9110_LENGTH + 1] = { 0 };
    u32 length                       = nya_instant_to_rfc9110(instant, text, sizeof(text));

    NYA_Instant   again    = { 0 };
    u64           position = 0;
    NYA_TimeParse result   = nya_instant_from_rfc9110(text, length, &again, &position);

    s128 floored = (s128)instant.ns - ((instant.ns % NYA_NS_PER_SECOND) + NYA_NS_PER_SECOND) % NYA_NS_PER_SECOND;

    nya_property_note(property, "%lld ns wrote '%s', which parsed as %s", (long long)instant.ns, (const char*)text, nya_time_parse_text(result));

    // the earliest instant's whole second is before the earliest instant, so it cannot come back, and says so.
    if (floored < (s128)S64_MIN) return result == NYA_TIME_PARSE_OUT_OF_RANGE;

    return length == NYA_RFC9110_LENGTH && result == NYA_TIME_PARSE_OK && (s128)again.ns == floored;
}

s32 main(void) {
    u32 failures = 0;

    // ─────────────────────────────────────────────────────────────────────────────
    // TEST: RFC 3339, its own examples and every rule it can fail
    // ─────────────────────────────────────────────────────────────────────────────
    printf("TEST: RFC 3339 parsing\n");
    {
        const ParseCase cases[] = {
            // section 5.8, in order. The two leap second examples are refused by rule; see clock_format.h.
            { "1985-04-12T23:20:50.52Z",         NYA_TIME_PARSE_OK,                 23, 482'196'050,    520'000'000 },
            { "1996-12-19T16:39:57-08:00",       NYA_TIME_PARSE_OK,                 25, 851'042'397,    0           },
            { "1990-12-31T23:59:60Z",            NYA_TIME_PARSE_LEAP_SECOND,        17, 0,              0           },
            { "1990-12-31T15:59:60-08:00",       NYA_TIME_PARSE_LEAP_SECOND,        17, 0,              0           },
            { "1937-01-01T12:00:27.87+00:20",    NYA_TIME_PARSE_OK,                 28, -1'041'337'173, 870'000'000 },

            // the case insensitive letters, and -00:00 as UTC.
            { "1985-04-12t23:20:50.52z",         NYA_TIME_PARSE_OK,                 23, 482'196'050,    520'000'000 },
            { "1985-04-12T23:20:50-00:00",       NYA_TIME_PARSE_OK,                 25, 482'196'050,    0           },
            { "1970-01-01T00:00:00.000000001Z",  NYA_TIME_PARSE_OK,                 30, 0,              1           },
            { "2024-02-29T00:00:00Z",            NYA_TIME_PARSE_OK,                 20, 1'709'164'800,  0           },

            // the edges of the range, and an offset that brings a local time past the end back inside it.
            { "2262-04-11T23:47:16.854775807Z",  NYA_TIME_PARSE_OK,                 30, 9'223'372'036,  854'775'807 },
            { "1677-09-21T00:12:43.145224192Z",  NYA_TIME_PARSE_OK,                 30, -9'223'372'037, 145'224'192 },
            { "2262-04-12T00:00:00+05:00",       NYA_TIME_PARSE_OK,                 25, 9'223'354'800,  0           },
            { "2262-04-11T23:47:16.854775808Z",  NYA_TIME_PARSE_OUT_OF_RANGE,       0,  0,              0           },
            { "1600-01-01T00:00:00Z",            NYA_TIME_PARSE_OUT_OF_RANGE,       0,  0,              0           },

            { "",                                NYA_TIME_PARSE_TRUNCATED,          0,  0,              0           },
            { "1985-04-12T23:20:50",             NYA_TIME_PARSE_TRUNCATED,          19, 0,              0           },
            { "1985-04-12T23:20:50.52+",         NYA_TIME_PARSE_TRUNCATED,          23, 0,              0           },
            { "1985-04-12T23:20:50.",            NYA_TIME_PARSE_TRUNCATED,          20, 0,              0           },
            { "abcd-04-12T23:20:50Z",            NYA_TIME_PARSE_EXPECTED_DIGIT,     0,  0,              0           },
            { "1985-04-12T23:20:50.Z",           NYA_TIME_PARSE_EXPECTED_DIGIT,     20, 0,              0           },
            { "85-04-12T23:20:50Z",              NYA_TIME_PARSE_EXPECTED_DIGIT,     2,  0,              0           },
            { "1985-04-12 23:20:50Z",            NYA_TIME_PARSE_EXPECTED_SEPARATOR, 10, 0,              0           },
            { "1985/04/12T23:20:50Z",            NYA_TIME_PARSE_EXPECTED_SEPARATOR, 4,  0,              0           },
            { "1985-13-12T23:20:50Z",            NYA_TIME_PARSE_MONTH_RANGE,        5,  0,              0           },
            { "1985-00-12T23:20:50Z",            NYA_TIME_PARSE_MONTH_RANGE,        5,  0,              0           },
            { "2023-02-29T00:00:00Z",            NYA_TIME_PARSE_DAY_RANGE,          8,  0,              0           },
            { "2023-04-00T00:00:00Z",            NYA_TIME_PARSE_DAY_RANGE,          8,  0,              0           },
            { "1985-04-12T24:00:00Z",            NYA_TIME_PARSE_HOUR_RANGE,         11, 0,              0           },
            { "1985-04-12T23:60:00Z",            NYA_TIME_PARSE_MINUTE_RANGE,       14, 0,              0           },
            { "1985-04-12T23:20:61Z",            NYA_TIME_PARSE_SECOND_RANGE,       17, 0,              0           },
            { "1985-04-12T23:20:50.1234567891Z", NYA_TIME_PARSE_FRACTION_TOO_LONG,  29, 0,              0           },
            { "1985-04-12T23:20:50X",            NYA_TIME_PARSE_EXPECTED_OFFSET,    19, 0,              0           },
            { "1985-04-12T23:20:50+24:00",       NYA_TIME_PARSE_OFFSET_RANGE,       20, 0,              0           },
            { "1985-04-12T23:20:50+05:60",       NYA_TIME_PARSE_OFFSET_RANGE,       23, 0,              0           },
            { "1985-04-12T23:20:50+0500",        NYA_TIME_PARSE_EXPECTED_SEPARATOR, 22, 0,              0           },
            { "1985-04-12T23:20:50Z ",           NYA_TIME_PARSE_TRAILING_BYTES,     20, 0,              0           },
            { "1985-04-12T23:20:50ZZ",           NYA_TIME_PARSE_TRAILING_BYTES,     20, 0,              0           },
        };

        check_parse_cases("RFC 3339", nya_instant_from_rfc3339, cases, nya_carray_length(cases));
    }
    printf("  PASSED\n");

    printf("TEST: RFC 3339 output is canonical\n");
    {
        struct {
            s64              ns;
            NYA_ConstCString text;
        } cases[] = {
            { 0,                                               "1970-01-01T00:00:00Z"           },
            { 482'196'050LL * NYA_NS_PER_SECOND + 520'000'000, "1985-04-12T23:20:50.52Z"        },
            { 1,                                               "1970-01-01T00:00:00.000000001Z" },
            { -1,                                              "1969-12-31T23:59:59.999999999Z" },
            { 1'500'000'000,                                   "1970-01-01T00:00:01.5Z"         },
            { S64_MAX,                                         "2262-04-11T23:47:16.854775807Z" },
            { S64_MIN,                                         "1677-09-21T00:12:43.145224192Z" },
        };

        for (u32 i = 0; i < nya_carray_length(cases); i++) {
            u8  text[NYA_RFC3339_LENGTH_MAX + 1] = { 0 };
            u32 length                           = nya_instant_to_rfc3339((NYA_Instant){ .ns = cases[i].ns }, text, sizeof(text));

            nya_assert(length == strlen(cases[i].text) && strcmp((const char*)text, cases[i].text) == 0, "case %u wrote '%s'", i, (const char*)text);
        }
    }
    printf("  PASSED\n");

    // ─────────────────────────────────────────────────────────────────────────────
    // TEST: RFC 9110 IMF-fixdate
    // ─────────────────────────────────────────────────────────────────────────────
    printf("TEST: RFC 9110 parsing\n");
    {
        const ParseCase cases[] = {
            { "Sun, 06 Nov 1994 08:49:37 GMT",  NYA_TIME_PARSE_OK,                 29, 784'111'777,   0 },
            { "Thu, 01 Jan 1970 00:00:00 GMT",  NYA_TIME_PARSE_OK,                 29, 0,             0 },
            { "Thu, 29 Feb 2024 00:00:00 GMT",  NYA_TIME_PARSE_OK,                 29, 1'709'164'800, 0 },

            // the obsolete forms RFC 9110 5.6.7 still lists, refused by name.
            { "Sunday, 06-Nov-94 08:49:37 GMT", NYA_TIME_PARSE_OBSOLETE_FORMAT,    3,  0,             0 },
            { "Sun Nov  6 08:49:37 1994",       NYA_TIME_PARSE_OBSOLETE_FORMAT,    3,  0,             0 },

            { "Mon, 06 Nov 1994 08:49:37 GMT",  NYA_TIME_PARSE_WEEKDAY_MISMATCH,   0,  0,             0 },
            { "sun, 06 Nov 1994 08:49:37 GMT",  NYA_TIME_PARSE_DAY_NAME,           0,  0,             0 },
            { "Sun, 06 nov 1994 08:49:37 GMT",  NYA_TIME_PARSE_MONTH_NAME,         8,  0,             0 },
            { "Sun, 06 Nov 1994 08:49:37 UTC",  NYA_TIME_PARSE_EXPECTED_GMT,       26, 0,             0 },
            { "Sun, 06 Nov 1994 08:49:37 gmt",  NYA_TIME_PARSE_EXPECTED_GMT,       26, 0,             0 },
            { "Sun, 06 Nov 1994 08:49:37 GMT ", NYA_TIME_PARSE_TRAILING_BYTES,     29, 0,             0 },
            { "Wed, 31 Nov 1994 08:49:37 GMT",  NYA_TIME_PARSE_DAY_RANGE,          5,  0,             0 },
            { "Sun, 06 Nov 1994 24:49:37 GMT",  NYA_TIME_PARSE_HOUR_RANGE,         17, 0,             0 },
            { "Sun, 06 Nov 1994 08:49:60 GMT",  NYA_TIME_PARSE_LEAP_SECOND,        23, 0,             0 },
            { "Sun, 6 Nov 1994 08:49:37 GMT",   NYA_TIME_PARSE_EXPECTED_DIGIT,     6,  0,             0 },
            { "Sun,06 Nov 1994 08:49:37 GMT",   NYA_TIME_PARSE_EXPECTED_SEPARATOR, 4,  0,             0 },
            { "Sun; 06 Nov 1994 08:49:37 GMT",  NYA_TIME_PARSE_EXPECTED_SEPARATOR, 3,  0,             0 },
            { "Sun, 06 Nov 1994 08:49",         NYA_TIME_PARSE_TRUNCATED,          22, 0,             0 },
            { "Sun, 06 Nov 1994 08:49:37 GM",   NYA_TIME_PARSE_TRUNCATED,          28, 0,             0 },
            { "Su",                             NYA_TIME_PARSE_TRUNCATED,          2,  0,             0 },
            { "",                               NYA_TIME_PARSE_TRUNCATED,          0,  0,             0 },

            // the cookie that never expires: a real date, and not one an instant holds.
            { "Fri, 31 Dec 9999 23:59:59 GMT",  NYA_TIME_PARSE_OUT_OF_RANGE,       0,  0,             0 },
        };

        check_parse_cases("RFC 9110", nya_instant_from_rfc9110, cases, nya_carray_length(cases));
    }
    printf("  PASSED\n");

    printf("TEST: RFC 9110 output\n");
    {
        u8 text[NYA_RFC9110_LENGTH + 1] = { 0 };

        nya_assert(nya_instant_to_rfc9110((NYA_Instant){ .ns = 784'111'777LL * NYA_NS_PER_SECOND }, text, sizeof(text)) == NYA_RFC9110_LENGTH);
        nya_assert(strcmp((const char*)text, "Sun, 06 Nov 1994 08:49:37 GMT") == 0, "wrote '%s'", (const char*)text);

        // the fraction goes toward the past, so a Last-Modified is never after the change.
        (void)nya_instant_to_rfc9110((NYA_Instant){ .ns = 784'111'777LL * NYA_NS_PER_SECOND + 999'999'999 }, text, sizeof(text));
        nya_assert(strcmp((const char*)text, "Sun, 06 Nov 1994 08:49:37 GMT") == 0, "wrote '%s'", (const char*)text);

        (void)nya_instant_to_rfc9110((NYA_Instant){ .ns = -1 }, text, sizeof(text));
        nya_assert(strcmp((const char*)text, "Wed, 31 Dec 1969 23:59:59 GMT") == 0, "wrote '%s'", (const char*)text);
    }
    printf("  PASSED\n");

    printf("TEST: every refusal has words\n");
    {
        for (u32 rule = 0; rule < NYA_TIME_PARSE_COUNT; rule++) {
            NYA_ConstCString text = nya_time_parse_text((NYA_TimeParse)rule);
            nya_assert(text != nullptr && text[0] != '\0');

            for (u32 other = 0; other < rule; other++)
                nya_assert(strcmp(text, nya_time_parse_text((NYA_TimeParse)other)) != 0, "two rules read the same");
        }
    }
    printf("  PASSED\n");

    printf("TEST: round trips\n");
    {
        failures += nya_property_check("an instant round trips through RFC 3339", CASES, SEED, law_rfc3339_round_trips);
        failures += nya_property_check("every RFC 3339 offset names the same instant", CASES, SEED, law_rfc3339_offsets_agree);
        failures += nya_property_check("an instant round trips through RFC 9110 to the second", CASES, SEED, law_rfc9110_round_trips);
    }
    if (failures == 0) printf("  PASSED\n");

    return failures == 0 ? 0 : 1;
}
