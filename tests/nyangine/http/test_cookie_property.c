/**
 * The Cookie-header parser as laws rather than examples: a header built from well formed pairs parses
 * back into exactly those pairs, in order, byte for byte; any header at all — arbitrary bytes, embedded
 * NULs, unbalanced separators — parses to a clean yes-or-no and never a crash or a read past the end,
 * since the header is the one thing an attacker fully controls; and every refusal leaves the count at
 * zero rather than a half-parsed prefix, because a partly read header is exactly the disagreement
 * between two parsers this is written to avoid.
 *
 * The example cases — the __Host- prefix rules, a quoted value refused, the duplicate-name rule — live
 * in test_cookie.c, and the coverage-guided target is tests/fuzz/fuzz_http_cookie.c. This is what makes
 * a regression a failure in the unit suite without a fuzzer installed: the round trip stated over pairs
 * nobody chose, and totality stated over bytes nobody chose.
 **/

// A larger entropy budget than the default 1024 so a header of many pairs, each a full-length name and
// value, is drawn with real bytes rather than the zeroes a draw past the end reads. Set before the
// engine is included, which is what a file naming a NYA_INTERNAL parser does anyway.
#define NYA_PROPERTY_ENTROPY_MAX 4096

#include "nyangine/nyangine.c"
#include "nyangine/nyangine.h"

/** Cases per law. Thousands, so a rare byte in a name or a value has room to turn up. */
#define CASES 4000

/** Fixed, so the suite is the same run every time. "cookie" and "pr" in ASCII. */
#define SEED 0x636F6F6B69657072ULL

/** Pairs a generated header carries at most, one under the parser's own ceiling so the full-table edge is reached. */
#define PAIRS_MAX (NYA_HTTP_MAX_COOKIES)

/** Characters a generated name or value holds at most; short, so a header holds several pairs inside one case. */
#define TEXT_MAX 24

/** Bytes a raw arbitrary header holds at most in the totality law. */
#define RAW_MAX 256

/* HELPERS */

/** The bytes a name may hold: RFC 7230 token characters, every one of which the name predicate accepts. */
static const char NAME_ALPHABET[] = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789!#$%&'*+-.^_`|~";

/** The bytes a value may hold: printable ASCII without the four the cookie-octet grammar forbids. */
static const char VALUE_ALPHABET[] = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789!#$%&'()*+-./:<=>?@[]^_`{|}~";

/**
 * Draws a name of one or more bytes from the token alphabet. Indexing a validated alphabet rather than
 * rejection-sampling arbitrary bytes keeps the draw bounded: once a case runs out of entropy the draws
 * read zero, which is the first character, never a byte the name grammar would reject in a loop.
 * */
static u64 draw_name(NYA_Property* property, OUT char* out, u64 capacity) {
    u64 length = 1 + nya_property_draw_below(property, capacity - 1);
    for (u64 index = 0; index < length; index++) out[index] = NAME_ALPHABET[nya_property_draw_below(property, sizeof(NAME_ALPHABET) - 1)];
    return length;
}

/** Draws a value of zero or more bytes from the cookie-octet alphabet; an empty value is legal and drawn. */
static u64 draw_value(NYA_Property* property, OUT char* out, u64 capacity) {
    u64 length = nya_property_draw_below(property, capacity);
    for (u64 index = 0; index < length; index++) out[index] = VALUE_ALPHABET[nya_property_draw_below(property, sizeof(VALUE_ALPHABET) - 1)];
    return length;
}

/* LAWS */

/**
 * A header assembled from unique well formed pairs, joined the one way the parser accepts — "name=value"
 * separated by exactly "; " — parses back into those very pairs, the same count, the same order, each
 * name and value byte for byte. This is the round trip the reader above the parser is built on.
 * */
static b8 law_well_formed_header_round_trips(NYA_Property* property) {
    u32  pair_count = 1 + (u32)nya_property_draw_below(property, PAIRS_MAX);

    char names[PAIRS_MAX][TEXT_MAX];
    u64  name_sizes[PAIRS_MAX];
    char values[PAIRS_MAX][TEXT_MAX];
    u64  value_sizes[PAIRS_MAX];

    // The header is built into an arena buffer large enough for every pair, its '=', and its "; " glue.
    u64   capacity = (u64)pair_count * (2 * TEXT_MAX + 3) + 1;
    char* header   = nya_arena_alloc(property->allocator, capacity);
    u64   length   = 0;

    u32 kept = 0;
    for (u32 index = 0; index < pair_count; index++) {
        char name[TEXT_MAX];
        u64  name_size = draw_name(property, name, TEXT_MAX);

        // A name already used is skipped: the parser refuses a repeated name, so the round trip is stated
        // over the unique pairs a well formed header actually carries.
        b8 duplicate = false;
        for (u32 seen = 0; seen < kept; seen++) {
            if (name_sizes[seen] == name_size && memcmp(names[seen], name, name_size) == 0) {
                duplicate = true;
                break;
            }
        }
        if (duplicate) continue;

        memcpy(names[kept], name, name_size);
        name_sizes[kept]  = name_size;
        value_sizes[kept] = draw_value(property, values[kept], TEXT_MAX);

        if (kept > 0) {
            header[length++] = ';';
            header[length++] = ' ';
        }
        memcpy(header + length, names[kept], name_sizes[kept]);
        length += name_sizes[kept];
        header[length++] = '=';
        memcpy(header + length, values[kept], value_sizes[kept]);
        length += value_sizes[kept];

        kept++;
    }

    NYA_HttpCookieValue out_names[NYA_HTTP_MAX_COOKIES]  = { 0 };
    NYA_HttpCookieValue out_values[NYA_HTTP_MAX_COOKIES] = { 0 };
    u32                 out_count                        = 0;

    b8 parsed = nya_http_cookie_parse(header, length, out_names, out_values, &out_count);

    if (!parsed || out_count != kept) {
        nya_property_note(property, "a header of %u well formed pairs parsed to %u", kept, parsed ? out_count : 0);
        return false;
    }

    for (u32 index = 0; index < kept; index++) {
        if (out_names[index].size != name_sizes[index] || memcmp(out_names[index].text, names[index], name_sizes[index]) != 0) {
            nya_property_note(property, "the name of pair %u did not round trip", index);
            return false;
        }
        if (out_values[index].size != value_sizes[index] || memcmp(out_values[index].text, values[index], value_sizes[index]) != 0) {
            nya_property_note(property, "the value of pair %u did not round trip", index);
            return false;
        }
    }

    return true;
}

/**
 * Any bytes at all parse to a clean answer: never a crash, and on a refusal the count is left at zero
 * rather than a half-parsed prefix. The sanitizers are the oracle for the reads; the count is the oracle
 * for the promise that a refused header hands back nothing.
 * */
static b8 law_any_header_is_total(NYA_Property* property) {
    u64 size = nya_property_draw_below(property, RAW_MAX + 1);
    u8* raw  = nya_arena_alloc(property->allocator, size == 0 ? 1 : size);

    // Bytes from the whole range, NULs included: an embedded NUL just ends a name or a value early, which
    // the length-driven parser must take in stride rather than walk off the buffer on.
    for (u64 index = 0; index < size; index++) raw[index] = nya_property_draw_u8(property);

    NYA_HttpCookieValue out_names[NYA_HTTP_MAX_COOKIES]  = { 0 };
    NYA_HttpCookieValue out_values[NYA_HTTP_MAX_COOKIES] = { 0 };
    u32                 out_count                        = 0;

    b8 parsed = nya_http_cookie_parse((const char*)raw, size, out_names, out_values, &out_count);

    // Accepted or refused, the count agrees with the verdict, and it never exceeds the table it fills.
    nya_property_note(property, "a %llu-byte header returned %d with count %u", (unsigned long long)size, parsed, out_count);
    if (!parsed) return out_count == 0;
    return out_count > 0 && out_count <= NYA_HTTP_MAX_COOKIES;
}

s32 main(void) {
    setvbuf(stdout, nullptr, _IONBF, 0);

    // The alphabets are the round trip's premise: every byte in them must be one the parser accepts, or
    // the "well formed" header the law builds would not be, and a failure would be the test's, not the code's.
    for (u64 index = 0; index < sizeof(NAME_ALPHABET) - 1; index++) nya_assert(_nya_http_cookie_name_char(NAME_ALPHABET[index]), "a name alphabet byte is not a name character");
    for (u64 index = 0; index < sizeof(VALUE_ALPHABET) - 1; index++) nya_assert(_nya_http_cookie_value_char(VALUE_ALPHABET[index]), "a value alphabet byte is not a value character");

    u32 failures = 0;

    failures += nya_property_check("a well formed cookie header round trips", CASES, SEED, law_well_formed_header_round_trips);
    failures += nya_property_check("any cookie header parses cleanly and reads nothing on refusal", CASES, SEED, law_any_header_is_total);

    return failures == 0 ? 0 : 1;
}
