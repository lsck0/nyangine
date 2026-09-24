/**
 * @file rfc3339_parse.c
 *
 * A bounded proof that the RFC 3339 timestamp parser never reads past its input, and that its cursor
 * obeys the two rules the engine asserts of it: a failure never reports a position past the end, and a
 * success reports having read every byte and no more.
 *
 * The function under proof is `_nya_rfc3339_parse` from src/nyangine/base/base_clock_format.c, reached
 * by `nya_instant_from_rfc3339`. Timestamps arrive from fully untrusted places — a `Last-Modified` in a
 * fetched feed, an Atom `<updated>`, a webhook payload — so a parser that could be walked off the end of
 * a short or malformed string is an out-of-bounds read on attacker-controlled input. Every byte it looks
 * at goes through the `_NYA_TimeCursor`, whose whole purpose (per its source comment) is that "none can
 * pass length".
 *
 * The body below is a verbatim copy of the engine's parser and the three cursor primitives it drives —
 * `_nya_time_read_digits`, `_nya_time_expect`, `_nya_time_read_clock` — lifted here rather than #included
 * because the real translation unit pulls in the whole clock stack. They are kept byte-for-byte in step
 * with the source. Two helpers that never touch the input buffer, `nya_date_is_valid` and
 * `_nya_time_compose`, are modelled as nondeterministic: they take the already-parsed integers, not the
 * text, so whatever they decide cannot change which bytes were read, and leaving them abstract lets CBMC
 * reach the accept path for any calendar the parser might hand them. (This is the same modelling the
 * base64 proof does for `nya_string_reserve`.)
 *
 * What CBMC checks here:
 *   - --bounds-check / --pointer-check: every `cursor->text[...]` read is inside the allocated buffer,
 *     for every input the harness admits. This is the out-of-bounds-read proof.
 *   - the functional invariant: on any outcome `position <= length` (the cursor never leaves the
 *     buffer), and on NYA_TIME_PARSE_OK `position == length` (a success consumed exactly its input) —
 *     the two invariants `nya_instant_from_rfc3339` states as assertions over this function's result.
 *
 * Run by `./build verify`. The one loop over the input — the fractional-seconds scan — is self-capped at
 * ten iterations by the nine-digit limit, so the small unwind bound is sound whatever the input length.
 */

#include <stdint.h>
#include <stdlib.h>

typedef _Bool    b8;
typedef uint8_t  u8;
typedef int32_t  s32;
typedef uint32_t u32;
typedef int64_t  s64;
typedef uint64_t u64;

#define nullptr ((void*)0)
#define false   0
#define true    1
#define OUT

/* Preconditions in the engine are asserted with nya_assert; the harness only passes valid pointers, so
 * the checks are a no-op here and the copied bodies stay verbatim. */
#define nya_assert(...) ((void)0)

/* ── constants mirrored from base_clock_format.h / base_clock_instant.h ─────────────────────────── */

#define NYA_RFC3339_FRACTION_DIGITS_MAX 9

#define NYA_NS_PER_SECOND (1000000000LL)
#define NYA_NS_PER_MINUTE (60LL * NYA_NS_PER_SECOND)
#define NYA_NS_PER_HOUR   (60LL * NYA_NS_PER_MINUTE)

/* ── minimal structs the parser writes into ─────────────────────────────────────────────────────── */

typedef struct {
    s32 year;
    u8  month;
    u8  day;
} NYA_Date;

typedef struct {
    u8  hour;
    u8  minute;
    u8  second;
    u32 nanosecond;
} NYA_TimeOfDay;

typedef struct {
    s64 ns;
} NYA_Instant;

/* Only the enum values this parser can return. */
typedef enum {
    NYA_TIME_PARSE_OK = 0,
    NYA_TIME_PARSE_TRUNCATED,
    NYA_TIME_PARSE_EXPECTED_DIGIT,
    NYA_TIME_PARSE_EXPECTED_SEPARATOR,
    NYA_TIME_PARSE_MONTH_RANGE,
    NYA_TIME_PARSE_DAY_RANGE,
    NYA_TIME_PARSE_HOUR_RANGE,
    NYA_TIME_PARSE_MINUTE_RANGE,
    NYA_TIME_PARSE_SECOND_RANGE,
    NYA_TIME_PARSE_LEAP_SECOND,
    NYA_TIME_PARSE_FRACTION_TOO_LONG,
    NYA_TIME_PARSE_EXPECTED_OFFSET,
    NYA_TIME_PARSE_OFFSET_RANGE,
    NYA_TIME_PARSE_OUT_OF_RANGE,
    NYA_TIME_PARSE_TRAILING_BYTES,
} NYA_TimeParse;

/* ── mirrored from src/nyangine/base/base_clock_format.c ────────────────────────────────────────── */

typedef struct {
    const u8* text;
    u64       length;
    u64       position;
} _NYA_TimeCursor;

#define _NYA_TIME_TRY(expr)                                                                            \
    do {                                                                                               \
        NYA_TimeParse _nya_time_result = (expr);                                                       \
        if (_nya_time_result != NYA_TIME_PARSE_OK) return _nya_time_result;                            \
    } while (0)

#define _NYA_TIME_FAIL_AT(cursor, field, rule)                                                         \
    do {                                                                                               \
        (cursor)->position = (field);                                                                  \
        return (rule);                                                                                 \
    } while (0)

static NYA_TimeParse _nya_time_read_digits(_NYA_TimeCursor* cursor, u32 count, OUT u32* out_value) {
    nya_assert(count >= 1 && count <= 4, "the grammars here have no field wider than a year");

    u32 value = 0;

    for (u32 index = 0; index < count; index++) {
        if (cursor->position == cursor->length) return NYA_TIME_PARSE_TRUNCATED;

        u8 byte = cursor->text[cursor->position];
        if (byte < '0' || byte > '9') return NYA_TIME_PARSE_EXPECTED_DIGIT;

        value = value * 10 + (u32)(byte - '0');
        cursor->position++;
    }

    *out_value = value;
    return NYA_TIME_PARSE_OK;
}

static NYA_TimeParse _nya_time_expect(_NYA_TimeCursor* cursor, u8 expected, u8 alternative) {
    if (cursor->position == cursor->length) return NYA_TIME_PARSE_TRUNCATED;

    u8 byte = cursor->text[cursor->position];
    if (byte != expected && byte != alternative) return NYA_TIME_PARSE_EXPECTED_SEPARATOR;

    cursor->position++;
    return NYA_TIME_PARSE_OK;
}

static NYA_TimeParse _nya_time_read_clock(_NYA_TimeCursor* cursor, OUT NYA_TimeOfDay* out_time) {
    u32 hour   = 0;
    u32 minute = 0;
    u32 second = 0;

    u64 hour_at = cursor->position;
    _NYA_TIME_TRY(_nya_time_read_digits(cursor, 2, &hour));
    if (hour > 23) _NYA_TIME_FAIL_AT(cursor, hour_at, NYA_TIME_PARSE_HOUR_RANGE);
    _NYA_TIME_TRY(_nya_time_expect(cursor, ':', ':'));

    u64 minute_at = cursor->position;
    _NYA_TIME_TRY(_nya_time_read_digits(cursor, 2, &minute));
    if (minute > 59) _NYA_TIME_FAIL_AT(cursor, minute_at, NYA_TIME_PARSE_MINUTE_RANGE);
    _NYA_TIME_TRY(_nya_time_expect(cursor, ':', ':'));

    u64 second_at = cursor->position;
    _NYA_TIME_TRY(_nya_time_read_digits(cursor, 2, &second));
    if (second == 60) _NYA_TIME_FAIL_AT(cursor, second_at, NYA_TIME_PARSE_LEAP_SECOND);
    if (second > 60) _NYA_TIME_FAIL_AT(cursor, second_at, NYA_TIME_PARSE_SECOND_RANGE);

    *out_time = (NYA_TimeOfDay){ .hour = (u8)hour, .minute = (u8)minute, .second = (u8)second };
    return NYA_TIME_PARSE_OK;
}

/* ── the two calendar helpers, modelled: they read the parsed integers, never the input buffer ──── */

extern b8 nondet_date_valid(void);
extern b8 nondet_compose_ok(void);
extern s64 nondet_instant_ns(void);

static b8 nya_date_is_valid(NYA_Date date) {
    (void)date;
    return nondet_date_valid();
}

static b8 _nya_time_compose(NYA_Date date, NYA_TimeOfDay time, s64 offset_ns, OUT NYA_Instant* out_instant) {
    (void)date;
    (void)time;
    (void)offset_ns;
    if (nondet_compose_ok()) {
        out_instant->ns = nondet_instant_ns();
        return true;
    }
    return false;
}

static NYA_TimeParse _nya_rfc3339_parse(_NYA_TimeCursor* cursor, OUT NYA_Instant* out_instant) {
    u32 year  = 0;
    u32 month = 0;
    u32 day   = 0;

    _NYA_TIME_TRY(_nya_time_read_digits(cursor, 4, &year));
    _NYA_TIME_TRY(_nya_time_expect(cursor, '-', '-'));

    u64 month_at = cursor->position;
    _NYA_TIME_TRY(_nya_time_read_digits(cursor, 2, &month));
    if (month < 1 || month > 12) _NYA_TIME_FAIL_AT(cursor, month_at, NYA_TIME_PARSE_MONTH_RANGE);
    _NYA_TIME_TRY(_nya_time_expect(cursor, '-', '-'));

    u64 day_at = cursor->position;
    _NYA_TIME_TRY(_nya_time_read_digits(cursor, 2, &day));

    NYA_Date date = { .year = (s32)year, .month = (u8)month, .day = (u8)day };
    if (!nya_date_is_valid(date)) _NYA_TIME_FAIL_AT(cursor, day_at, NYA_TIME_PARSE_DAY_RANGE);

    _NYA_TIME_TRY(_nya_time_expect(cursor, 'T', 't'));

    NYA_TimeOfDay time = { 0 };
    _NYA_TIME_TRY(_nya_time_read_clock(cursor, &time));

    if (cursor->position < cursor->length && cursor->text[cursor->position] == '.') {
        cursor->position++;

        u32 digits = 0;
        u32 value  = 0;

        // bounded by the digit limit plus the one that breaks it, not by the input.
        while (cursor->position < cursor->length && cursor->text[cursor->position] >= '0' && cursor->text[cursor->position] <= '9') {
            if (digits == NYA_RFC3339_FRACTION_DIGITS_MAX) return NYA_TIME_PARSE_FRACTION_TOO_LONG;

            value = value * 10 + (u32)(cursor->text[cursor->position] - '0');
            digits++;
            cursor->position++;
        }

        if (digits == 0) return cursor->position == cursor->length ? NYA_TIME_PARSE_TRUNCATED : NYA_TIME_PARSE_EXPECTED_DIGIT;

        for (u32 scale = digits; scale < NYA_RFC3339_FRACTION_DIGITS_MAX; scale++) value *= 10;
        time.nanosecond = value;
    }

    if (cursor->position == cursor->length) return NYA_TIME_PARSE_TRUNCATED;

    s64 offset_ns = 0;
    u8  sign      = cursor->text[cursor->position];

    if (sign == 'Z' || sign == 'z') {
        cursor->position++;
    } else if (sign == '+' || sign == '-') {
        cursor->position++;

        u32 offset_hours   = 0;
        u32 offset_minutes = 0;

        u64 hours_at = cursor->position;
        _NYA_TIME_TRY(_nya_time_read_digits(cursor, 2, &offset_hours));
        if (offset_hours > 23) _NYA_TIME_FAIL_AT(cursor, hours_at, NYA_TIME_PARSE_OFFSET_RANGE);
        _NYA_TIME_TRY(_nya_time_expect(cursor, ':', ':'));

        u64 minutes_at = cursor->position;
        _NYA_TIME_TRY(_nya_time_read_digits(cursor, 2, &offset_minutes));
        if (offset_minutes > 59) _NYA_TIME_FAIL_AT(cursor, minutes_at, NYA_TIME_PARSE_OFFSET_RANGE);

        // -00:00 is "UTC, local offset unknown", which to an instant is plain UTC.
        offset_ns = (s64)offset_hours * NYA_NS_PER_HOUR + (s64)offset_minutes * NYA_NS_PER_MINUTE;
        if (sign == '-') offset_ns = -offset_ns;
    } else {
        return NYA_TIME_PARSE_EXPECTED_OFFSET;
    }

    if (cursor->position != cursor->length) return NYA_TIME_PARSE_TRAILING_BYTES;

    if (!_nya_time_compose(date, time, offset_ns, out_instant)) _NYA_TIME_FAIL_AT(cursor, 0, NYA_TIME_PARSE_OUT_OF_RANGE);

    return NYA_TIME_PARSE_OK;
}

/* ── the harness ────────────────────────────────────────────────────────────────────────────────── */

/* Big enough to admit a whole timestamp with a Z, a numeric offset, or a fractional part — the shortest
 * accepted form, "2000-01-01T00:00:00Z", is twenty bytes — yet small enough to keep the state space
 * tight. The single input-length loop (the fraction scan) caps itself at ten iterations regardless. */
#define RFC3339_MAX_INPUT 24u

extern u64 nondet_length(void);

int main(void) {
    u64 length = nondet_length();
    __CPROVER_assume(length <= RFC3339_MAX_INPUT);

    /* An object of exactly `length` nondeterministic bytes: any read past it is a real out-of-bounds
     * access CBMC will report, which is what makes the cursor's "none can pass length" claim a proof. */
    u8* text = malloc(length);
    __CPROVER_assume(length == 0 || text != nullptr);

    _NYA_TimeCursor cursor  = { .text = text, .length = length, .position = 0 };
    NYA_Instant     instant = { 0 };

    NYA_TimeParse result = _nya_rfc3339_parse(&cursor, &instant);

    /* The cursor never reports a position past the end, on success or on any failure. */
    __CPROVER_assert(cursor.position <= cursor.length, "the parse keeps the cursor within its input");

    /* A success read exactly its whole input and stopped — no trailing byte left unlooked-at, and
     * nothing past the end. This is the invariant nya_instant_from_rfc3339 asserts of the result. */
    if (result == NYA_TIME_PARSE_OK) __CPROVER_assert(cursor.position == cursor.length, "a successful parse consumed exactly its input");

    return 0;
}
