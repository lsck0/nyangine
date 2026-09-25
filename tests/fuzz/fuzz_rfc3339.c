/**
 * The RFC 3339 parser, fed whatever. Reached by every timestamp in a JSON body, a `.nya` document or a
 * log a user hands back, which is to say by anyone who can send the program a string.
 **/

#include "nyangine-core/nyangine.h"

#include "nyangine-core/nyangine.c"

#define FUZZ_TARGET "rfc3339"

static void fuzz_once(const u8* data, u64 size) {
    NYA_Instant   instant  = { 0 };
    u64           position = 0;
    NYA_TimeParse result   = nya_instant_from_rfc3339(data, size, &instant, &position);

    nya_assert(result < NYA_TIME_PARSE_COUNT);
    nya_assert(position <= size, "a refusal pointed past the end of the input");

    if (result != NYA_TIME_PARSE_OK) return;

    // whatever was accepted has a canonical spelling, and that spelling is the same instant and is itself canonical: written again, it does not change.
    u8  text[NYA_RFC3339_LENGTH_MAX + 1] = { 0 };
    u32 length                           = nya_instant_to_rfc3339(instant, text, sizeof(text));

    NYA_Instant again = { 0 };
    nya_assert(nya_instant_from_rfc3339(text, length, &again, &position) == NYA_TIME_PARSE_OK, "the canonical form of an accepted stamp was refused");
    nya_assert(again.ns == instant.ns, "the canonical form is a different instant");

    u8 rewritten[NYA_RFC3339_LENGTH_MAX + 1] = { 0 };
    nya_assert(nya_instant_to_rfc3339(again, rewritten, sizeof(rewritten)) == length && nya_memcmp(text, rewritten, length) == 0, "not canonical");
}

#include "tests/fuzz/fuzz.h"
