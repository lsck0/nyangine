/**
 * @file clock_format.h
 *
 * Instants as text and back, in the two formats the wire uses: RFC 3339 for JSON, `.nya` and logs, and
 * RFC 9110's IMF-fixdate for HTTP (`Date`, `Last-Modified`, cookies). Output is canonical, one spelling
 * per instant; parsing is strict, and a refusal names the rule that failed and the byte it failed at.
 *
 * Overview:
 *   nya_instant_to_rfc3339 / _from_rfc3339   2026-09-22T14:03:11.5Z
 *   nya_instant_to_rfc9110 / _from_rfc9110   Tue, 22 Sep 2026 14:03:11 GMT
 *   nya_time_parse_text                      a refusal, as words for a log or a 400
 *
 * ```c
 * u8  text[NYA_RFC3339_LENGTH_MAX + 1];
 * u32 length = nya_instant_to_rfc3339(nya_instant_now(), text, sizeof(text));
 *
 * NYA_Instant   instant  = { 0 };
 * u64           position = 0;
 * NYA_TimeParse result   = nya_instant_from_rfc3339(text, length, &instant, &position);
 * if (result != NYA_TIME_PARSE_OK) nya_log_warn("%s at byte %llu", nya_time_parse_text(result), position);
 * ```
 *
 * ## What the writers produce
 *
 * RFC 3339 always in UTC with `Z`, an uppercase `T`, and the fraction only as long as it needs to be:
 * none for a whole second, trailing zeros trimmed otherwise. So one instant has exactly one spelling and
 * no precision is lost. The cost is that two stamps with different fraction lengths do not sort as
 * strings; always nine digits would, and was rejected because `.000000000Z` on every whole second is
 * what a person reading a log or a JSON document then has to skip.
 *
 * IMF-fixdate has whole seconds, so the fraction is dropped, toward the past: a Last-Modified is never
 * later than the change it reports.
 *
 * Both write no allocator, no stdio and no locale, so the crash path may call them.
 *
 * ## What the parsers accept, and what they refuse
 *
 * RFC 3339's `date-time` exactly: `T` or `t`, `Z` or `z` or a numeric offset (`-00:00` is UTC with the
 * local offset unknown, which to an instant is UTC), and one to nine fraction digits. Refused:
 *   - a tenth fraction digit, rather than rounded, since rounding is a repair;
 *   - a space in place of `T`, which RFC 3339 permits an application to choose and this one does not;
 *   - a leap second, `:60`. An instant has no 61st second to put it in, and moving it to `:59` or to the
 *     next second would be a repair that makes two different texts one instant. It gets its own rule,
 *     NYA_TIME_PARSE_LEAP_SECOND, so a caller that wants to accept one can see exactly that and decide.
 *
 * IMF-fixdate only, case sensitive as the grammar says, and the day name has to be the day the date
 * falls on. The two obsolete HTTP-date forms (RFC 850's `Sunday, 06-Nov-94` and asctime's
 * `Sun Nov  6 08:49:37 1994`) are refused as NYA_TIME_PARSE_OBSOLETE_FORMAT. RFC 9110 5.6.7 asks a
 * recipient to accept them; this deviates on purpose, since no sender has emitted them in twenty years,
 * the two-digit year needs a guess, and a format nobody sends is only reachable by someone probing.
 *
 * Either parser reads at most `length` bytes, never a terminator, allocates nothing, and every loop in it
 * is bounded by the grammar, so it is safe on anything a stranger sends; tests/fuzz holds it to that.
 * */
#pragma once

#include "nyangine/base/base_attributes.h"
#include "nyangine/base/base_types.h"
#include "nyangine/base/base_clock_instant.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * CONSTANTS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/**
 * Fraction digits RFC 3339 may carry here. Nine is a nanosecond, which is all an instant holds; a tenth
 * digit could only be rounded away.
 * */
#define NYA_RFC3339_FRACTION_DIGITS_MAX 9

/**
 * Longest RFC 3339 text the writer produces, terminator excluded: `YYYY-MM-DDTHH:MM:SS` is nineteen,
 * then a point and nine digits, then `Z`. Every instant has a four digit year, so it is never more.
 * */
#define NYA_RFC3339_LENGTH_MAX (19 + 1 + NYA_RFC3339_FRACTION_DIGITS_MAX + 1)

/** IMF-fixdate is fixed width: `Sun, 06 Nov 1994 08:49:37 GMT`. */
#define NYA_RFC9110_LENGTH 29

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * TYPES
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

typedef enum NYA_TimeParse NYA_TimeParse;

/** Which rule a parse failed, or that none did. The position beside it says where. */
enum NYA_TimeParse {
    NYA_TIME_PARSE_OK = 0,

    /** The text ended where the grammar needed more. */
    NYA_TIME_PARSE_TRUNCATED,

    NYA_TIME_PARSE_EXPECTED_DIGIT,

    /** A `-`, `:`, `T`, `,` or space the grammar puts here is not. */
    NYA_TIME_PARSE_EXPECTED_SEPARATOR,

    NYA_TIME_PARSE_MONTH_RANGE,

    /** Day zero, or past the end of its month in its year: 2023-02-29 lands here. */
    NYA_TIME_PARSE_DAY_RANGE,

    NYA_TIME_PARSE_HOUR_RANGE,
    NYA_TIME_PARSE_MINUTE_RANGE,
    NYA_TIME_PARSE_SECOND_RANGE,

    /** `:60`. Refused; see the note at the top. */
    NYA_TIME_PARSE_LEAP_SECOND,

    /** More than NYA_RFC3339_FRACTION_DIGITS_MAX digits after the point. */
    NYA_TIME_PARSE_FRACTION_TOO_LONG,

    /** Neither `Z` nor a `+` or `-` offset where RFC 3339 needs one. */
    NYA_TIME_PARSE_EXPECTED_OFFSET,

    /** An offset hour past 23 or minute past 59. */
    NYA_TIME_PARSE_OFFSET_RANGE,

    /** Not one of `Mon` to `Sun`, in that case. */
    NYA_TIME_PARSE_DAY_NAME,

    /** A valid day name that is not the day the date falls on. */
    NYA_TIME_PARSE_WEEKDAY_MISMATCH,

    /** Not one of `Jan` to `Dec`, in that case. */
    NYA_TIME_PARSE_MONTH_NAME,

    /** IMF-fixdate ends in `GMT` and nothing else. */
    NYA_TIME_PARSE_EXPECTED_GMT,

    /** RFC 850 or asctime's form of an HTTP-date, which is refused rather than guessed at. */
    NYA_TIME_PARSE_OBSOLETE_FORMAT,

    /** A valid date and time that NYA_Instant cannot hold: before 1677-09-21 or after 2262-04-11. */
    NYA_TIME_PARSE_OUT_OF_RANGE,

    /** Anything after a complete timestamp. */
    NYA_TIME_PARSE_TRAILING_BYTES,

    NYA_TIME_PARSE_COUNT,
};

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * FUNCTIONS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/**
 * Writes `instant` as canonical RFC 3339 into `buffer`, null terminated, and returns the bytes written
 * excluding the terminator. `capacity` must hold NYA_RFC3339_LENGTH_MAX + 1, which is asserted rather
 * than truncated to: a timestamp cut short is a different timestamp.
 * */
NYA_API u32 nya_instant_to_rfc3339(NYA_Instant instant, OUT u8* buffer, u32 capacity);

/**
 * Parses RFC 3339's `date-time` from exactly `text[0, length)`. On NYA_TIME_PARSE_OK the instant is in
 * `out_instant` and `out_position` is `length`; on anything else `out_instant` is untouched and
 * `out_position` is the byte the failing rule starts at, at most `length`.
 * */
NYA_API NYA_TimeParse nya_instant_from_rfc3339(const u8* text, u64 length, OUT NYA_Instant* out_instant, OUT u64* out_position) __attr_no_discard;

/**
 * Writes `instant` as IMF-fixdate into `buffer`, null terminated, and returns NYA_RFC9110_LENGTH.
 * `capacity` must hold NYA_RFC9110_LENGTH + 1. The fraction of a second is dropped toward the past.
 * */
NYA_API u32 nya_instant_to_rfc9110(NYA_Instant instant, OUT u8* buffer, u32 capacity);

/** Parses IMF-fixdate from exactly `text[0, length)`, with the same contract as nya_instant_from_rfc3339. */
NYA_API NYA_TimeParse nya_instant_from_rfc9110(const u8* text, u64 length, OUT NYA_Instant* out_instant, OUT u64* out_position) __attr_no_discard;

/** The rule, in a few words, for a log line or an error body. */
NYA_API NYA_ConstCString nya_time_parse_text(NYA_TimeParse result) __attr_no_discard;
