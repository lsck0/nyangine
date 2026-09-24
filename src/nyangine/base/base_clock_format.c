#include "nyangine/base/base_assert.h"
#include "nyangine/base/base_clock_format.h"
#include "nyangine/base/base_clock_instant.h"

// PRIVATE API DECLARATION

/** Where a parse has got to in its input. Every read goes through it, so none can pass `length`. */
typedef struct {
    const u8* text;
    u64       length;
    u64       position;
} _NYA_TimeCursor;

/** Returns from the enclosing parse when a step fails, leaving the cursor where it failed. */
#define _NYA_TIME_TRY(expr)                                                                                                                          \
    do {                                                                                                                                             \
        NYA_TimeParse _nya_time_result = (expr);                                                                                                     \
        if (_nya_time_result != NYA_TIME_PARSE_OK) return _nya_time_result;                                                                          \
    } while (0)

/** Fails a range rule at the start of the field that broke it, rather than at the byte after it. */
#define _NYA_TIME_FAIL_AT(cursor, field, rule)                                                                                                       \
    do {                                                                                                                                             \
        (cursor)->position = (field);                                                                                                                \
        return (rule);                                                                                                                               \
    } while (0)

/** In ISO order, Monday first, so NYA_Weekday indexes it. */
NYA_INTERNAL const char _NYA_TIME_DAY_NAMES[NYA_WEEKDAY_COUNT][4] = { "Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun" };

NYA_INTERNAL const char _NYA_TIME_MONTH_NAMES[12][4] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };

/** Exactly `count` decimal digits into `out_value`. */
NYA_INTERNAL NYA_TimeParse _nya_time_read_digits(_NYA_TimeCursor* cursor, u32 count, OUT u32* out_value) __attr_no_discard;

/** The byte `expected`, or one of two spellings of it where the grammar is case insensitive. */
NYA_INTERNAL NYA_TimeParse _nya_time_expect(_NYA_TimeCursor* cursor, u8 expected, u8 alternative) __attr_no_discard;

/** Which of `names` the next three bytes spell, case sensitive, or `on_miss` at the field. */
NYA_INTERNAL NYA_TimeParse _nya_time_read_name(_NYA_TimeCursor* cursor, const char (*names)[4], u32 count, NYA_TimeParse on_miss, OUT u32* out_index)
    __attr_no_discard;

/** Hours, minutes and seconds with their range rules, shared by both formats. */
NYA_INTERNAL NYA_TimeParse _nya_time_read_clock(_NYA_TimeCursor* cursor, OUT NYA_TimeOfDay* out_time) __attr_no_discard;

/**
 * The instant a local date and time at `offset_ns` east of UTC name. Wide arithmetic, because a local
 * time just past the range can still be inside it once the offset is taken off: 2262-04-12T00:00+05:00.
 * */
NYA_INTERNAL b8 _nya_time_compose(NYA_Date date, NYA_TimeOfDay time, s64 offset_ns, OUT NYA_Instant* out_instant) __attr_no_discard;

NYA_INTERNAL NYA_TimeParse _nya_rfc3339_parse(_NYA_TimeCursor* cursor, OUT NYA_Instant* out_instant) __attr_no_discard;
NYA_INTERNAL NYA_TimeParse _nya_rfc9110_parse(_NYA_TimeCursor* cursor, OUT NYA_Instant* out_instant) __attr_no_discard;

/** `value` as exactly `count` digits, zero padded. */
NYA_INTERNAL void _nya_time_write_digits(OUT u8* out, u32 value, u32 count);

// PUBLIC API IMPLEMENTATION

// RFC 3339

u32 nya_instant_to_rfc3339(NYA_Instant instant, OUT u8* buffer, u32 capacity) {
    nya_assert(buffer != nullptr);
    nya_assert(capacity > NYA_RFC3339_LENGTH_MAX, "an RFC 3339 buffer holds %d bytes and a terminator, not %u", NYA_RFC3339_LENGTH_MAX, capacity);

    NYA_Date      date = { 0 };
    NYA_TimeOfDay time = { 0 };
    nya_instant_to_utc(instant, &date, &time);

    nya_assert(date.year >= 0 && date.year <= 9'999, "every instant has a four digit year");

    _nya_time_write_digits(&buffer[0], (u32)date.year, 4);
    buffer[4] = '-';
    _nya_time_write_digits(&buffer[5], date.month, 2);
    buffer[7] = '-';
    _nya_time_write_digits(&buffer[8], date.day, 2);
    buffer[10] = 'T';
    _nya_time_write_digits(&buffer[11], time.hour, 2);
    buffer[13] = ':';
    _nya_time_write_digits(&buffer[14], time.minute, 2);
    buffer[16] = ':';
    _nya_time_write_digits(&buffer[17], time.second, 2);

    u32 length = 19;

    if (time.nanosecond != 0) {
        buffer[length++] = '.';
        _nya_time_write_digits(&buffer[length], time.nanosecond, NYA_RFC3339_FRACTION_DIGITS_MAX);
        length += NYA_RFC3339_FRACTION_DIGITS_MAX;

        // shortest exact: never below one digit, since the fraction is not zero.
        while (buffer[length - 1] == '0') length--;
    }

    buffer[length++] = 'Z';
    buffer[length]   = '\0';

    nya_assert(length <= NYA_RFC3339_LENGTH_MAX);
    return length;
}

NYA_TimeParse nya_instant_from_rfc3339(const u8* text, u64 length, OUT NYA_Instant* out_instant, OUT u64* out_position) {
    nya_assert(text != nullptr || length == 0);
    nya_assert(out_instant != nullptr);
    nya_assert(out_position != nullptr);

    _NYA_TimeCursor cursor  = { .text = text, .length = length };
    NYA_Instant     instant = { 0 };
    NYA_TimeParse   result  = _nya_rfc3339_parse(&cursor, &instant);

    nya_assert(result < NYA_TIME_PARSE_COUNT);
    nya_assert(cursor.position <= length, "a parse reported a failure past the end of its input");
    nya_assert(result != NYA_TIME_PARSE_OK || cursor.position == length, "a parse succeeded without reading its whole input");

    *out_position = cursor.position;
    if (result == NYA_TIME_PARSE_OK) *out_instant = instant;

    return result;
}

// RFC 9110

u32 nya_instant_to_rfc9110(NYA_Instant instant, OUT u8* buffer, u32 capacity) {
    nya_assert(buffer != nullptr);
    nya_assert(capacity > NYA_RFC9110_LENGTH, "an IMF-fixdate buffer holds %d bytes and a terminator, not %u", NYA_RFC9110_LENGTH, capacity);

    // to_utc floors, which is what drops the fraction toward the past.
    NYA_Date      date = { 0 };
    NYA_TimeOfDay time = { 0 };
    nya_instant_to_utc(instant, &date, &time);

    const char* day   = _NYA_TIME_DAY_NAMES[nya_date_weekday(date)];
    const char* month = _NYA_TIME_MONTH_NAMES[date.month - 1];

    nya_memcpy(&buffer[0], day, 3);
    buffer[3] = ',';
    buffer[4] = ' ';
    _nya_time_write_digits(&buffer[5], date.day, 2);
    buffer[7] = ' ';
    nya_memcpy(&buffer[8], month, 3);
    buffer[11] = ' ';
    _nya_time_write_digits(&buffer[12], (u32)date.year, 4);
    buffer[16] = ' ';
    _nya_time_write_digits(&buffer[17], time.hour, 2);
    buffer[19] = ':';
    _nya_time_write_digits(&buffer[20], time.minute, 2);
    buffer[22] = ':';
    _nya_time_write_digits(&buffer[23], time.second, 2);
    nya_memcpy(&buffer[25], " GMT", 4);
    buffer[NYA_RFC9110_LENGTH] = '\0';

    return NYA_RFC9110_LENGTH;
}

NYA_TimeParse nya_instant_from_rfc9110(const u8* text, u64 length, OUT NYA_Instant* out_instant, OUT u64* out_position) {
    nya_assert(text != nullptr || length == 0);
    nya_assert(out_instant != nullptr);
    nya_assert(out_position != nullptr);

    _NYA_TimeCursor cursor  = { .text = text, .length = length };
    NYA_Instant     instant = { 0 };
    NYA_TimeParse   result  = _nya_rfc9110_parse(&cursor, &instant);

    nya_assert(result < NYA_TIME_PARSE_COUNT);
    nya_assert(cursor.position <= length, "a parse reported a failure past the end of its input");
    nya_assert(result != NYA_TIME_PARSE_OK || cursor.position == length, "a parse succeeded without reading its whole input");

    *out_position = cursor.position;
    if (result == NYA_TIME_PARSE_OK) *out_instant = instant;

    return result;
}

NYA_ConstCString nya_time_parse_text(NYA_TimeParse result) {
    switch (result) {
        case NYA_TIME_PARSE_OK:                 return "ok";
        case NYA_TIME_PARSE_TRUNCATED:          return "the text ends too early";
        case NYA_TIME_PARSE_EXPECTED_DIGIT:     return "expected a digit";
        case NYA_TIME_PARSE_EXPECTED_SEPARATOR: return "expected a separator";
        case NYA_TIME_PARSE_MONTH_RANGE:        return "the month is not 01 to 12";
        case NYA_TIME_PARSE_DAY_RANGE:          return "the day is not in the month";
        case NYA_TIME_PARSE_HOUR_RANGE:         return "the hour is past 23";
        case NYA_TIME_PARSE_MINUTE_RANGE:       return "the minute is past 59";
        case NYA_TIME_PARSE_SECOND_RANGE:       return "the second is past 59";
        case NYA_TIME_PARSE_LEAP_SECOND:        return "leap seconds are refused";
        case NYA_TIME_PARSE_FRACTION_TOO_LONG:  return "more than nine fraction digits";
        case NYA_TIME_PARSE_EXPECTED_OFFSET:    return "expected Z or a numeric offset";
        case NYA_TIME_PARSE_OFFSET_RANGE:       return "the offset is not a time of day";
        case NYA_TIME_PARSE_DAY_NAME:           return "not a day name";
        case NYA_TIME_PARSE_WEEKDAY_MISMATCH:   return "the day name is not the day of that date";
        case NYA_TIME_PARSE_MONTH_NAME:         return "not a month name";
        case NYA_TIME_PARSE_EXPECTED_GMT:       return "expected GMT";
        case NYA_TIME_PARSE_OBSOLETE_FORMAT:    return "an obsolete HTTP date format";
        case NYA_TIME_PARSE_OUT_OF_RANGE:       return "outside 1677-09-21 to 2262-04-11";
        case NYA_TIME_PARSE_TRAILING_BYTES:     return "bytes after the timestamp";
        default:                                nya_unreachable();
    }
    static_assert(NYA_TIME_PARSE_COUNT == 20, "Unhandled NYA_TimeParse enum value.");
}

// PRIVATE API IMPLEMENTATION

NYA_TimeParse _nya_rfc3339_parse(_NYA_TimeCursor* cursor, OUT NYA_Instant* out_instant) {
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

NYA_TimeParse _nya_rfc9110_parse(_NYA_TimeCursor* cursor, OUT NYA_Instant* out_instant) {
    u32 weekday = 0;
    _NYA_TIME_TRY(_nya_time_read_name(cursor, _NYA_TIME_DAY_NAMES, NYA_WEEKDAY_COUNT, NYA_TIME_PARSE_DAY_NAME, &weekday));

    if (cursor->position == cursor->length) return NYA_TIME_PARSE_TRUNCATED;

    // "Sunday, 06-Nov-94" carries on with the day's name; "Sun Nov  6" has a space. Named for what they are, so a refusal says which rule, not just that a comma was missing.
    u8 after_name = cursor->text[cursor->position];
    if ((after_name >= 'a' && after_name <= 'z') || after_name == ' ') return NYA_TIME_PARSE_OBSOLETE_FORMAT;

    _NYA_TIME_TRY(_nya_time_expect(cursor, ',', ','));
    _NYA_TIME_TRY(_nya_time_expect(cursor, ' ', ' '));

    u32 day   = 0;
    u32 month = 0;
    u32 year  = 0;

    u64 day_at = cursor->position;
    _NYA_TIME_TRY(_nya_time_read_digits(cursor, 2, &day));
    _NYA_TIME_TRY(_nya_time_expect(cursor, ' ', ' '));
    _NYA_TIME_TRY(_nya_time_read_name(cursor, _NYA_TIME_MONTH_NAMES, 12, NYA_TIME_PARSE_MONTH_NAME, &month));
    _NYA_TIME_TRY(_nya_time_expect(cursor, ' ', ' '));
    _NYA_TIME_TRY(_nya_time_read_digits(cursor, 4, &year));

    // the day's range depends on the month and the year, which come after it.
    NYA_Date date = { .year = (s32)year, .month = (u8)(month + 1), .day = (u8)day };
    if (!nya_date_is_valid(date)) _NYA_TIME_FAIL_AT(cursor, day_at, NYA_TIME_PARSE_DAY_RANGE);

    _NYA_TIME_TRY(_nya_time_expect(cursor, ' ', ' '));

    NYA_TimeOfDay time = { 0 };
    _NYA_TIME_TRY(_nya_time_read_clock(cursor, &time));
    _NYA_TIME_TRY(_nya_time_expect(cursor, ' ', ' '));

    u64 zone_at = cursor->position;
    if (cursor->length - cursor->position < 3) {
        cursor->position = cursor->length;
        return NYA_TIME_PARSE_TRUNCATED;
    }
    if (nya_memcmp(&cursor->text[zone_at], "GMT", 3) != 0) return NYA_TIME_PARSE_EXPECTED_GMT;
    cursor->position += 3;

    if (cursor->position != cursor->length) return NYA_TIME_PARSE_TRAILING_BYTES;

    // A mismatched day name means the sender computed the date wrong or edited half of it; either way this is not the moment it meant, and trusting the numbers over the name would be a guess.
    if ((u32)nya_date_weekday(date) != weekday) _NYA_TIME_FAIL_AT(cursor, 0, NYA_TIME_PARSE_WEEKDAY_MISMATCH);

    if (!_nya_time_compose(date, time, 0, out_instant)) _NYA_TIME_FAIL_AT(cursor, 0, NYA_TIME_PARSE_OUT_OF_RANGE);

    return NYA_TIME_PARSE_OK;
}

NYA_TimeParse _nya_time_read_clock(_NYA_TimeCursor* cursor, OUT NYA_TimeOfDay* out_time) {
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

NYA_TimeParse _nya_time_read_digits(_NYA_TimeCursor* cursor, u32 count, OUT u32* out_value) {
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

NYA_TimeParse _nya_time_expect(_NYA_TimeCursor* cursor, u8 expected, u8 alternative) {
    if (cursor->position == cursor->length) return NYA_TIME_PARSE_TRUNCATED;

    u8 byte = cursor->text[cursor->position];
    if (byte != expected && byte != alternative) return NYA_TIME_PARSE_EXPECTED_SEPARATOR;

    cursor->position++;
    return NYA_TIME_PARSE_OK;
}

NYA_TimeParse _nya_time_read_name(_NYA_TimeCursor* cursor, const char (*names)[4], u32 count, NYA_TimeParse on_miss, OUT u32* out_index) {
    if (cursor->length - cursor->position < 3) {
        cursor->position = cursor->length;
        return NYA_TIME_PARSE_TRUNCATED;
    }

    for (u32 index = 0; index < count; index++) {
        if (nya_memcmp(&cursor->text[cursor->position], names[index], 3) != 0) continue;

        cursor->position += 3;
        *out_index        = index;
        return NYA_TIME_PARSE_OK;
    }

    return on_miss;
}

b8 _nya_time_compose(NYA_Date date, NYA_TimeOfDay time, s64 offset_ns, OUT NYA_Instant* out_instant) {
    nya_assert(nya_date_is_valid(date));
    nya_assert(nya_time_of_day_is_valid(time));

    // ten thousand years of nanoseconds is about 2^78, well inside 128 bits.
    s128 ns = (s128)nya_date_to_days(date) * (s128)NYA_NS_PER_DAY + nya_time_of_day_to_duration(time).ns - offset_ns;

    if (ns < (s128)S64_MIN || ns > (s128)S64_MAX) return false;

    *out_instant = (NYA_Instant){ .ns = (s64)ns };
    return true;
}

void _nya_time_write_digits(OUT u8* out, u32 value, u32 count) {
    for (u32 index = count; index > 0; index--) {
        out[index - 1]  = (u8)('0' + value % 10);
        value          /= 10;
    }

    nya_assert(value == 0, "a value wider than its field");
}
