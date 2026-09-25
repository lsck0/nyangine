/**
 * The IMF-fixdate parser, fed whatever. Reached by If-Modified-Since, cookie expiries and anything else
 * a client puts in a header, which is to say by every stranger on the network.
 **/

#include "nyangine-core/nyangine.h"

#include "nyangine-core/nyangine.c"

#define FUZZ_TARGET "rfc9110"

static void fuzz_once(const u8* data, u64 size) {
    NYA_Instant   instant  = { 0 };
    u64           position = 0;
    NYA_TimeParse result   = nya_instant_from_rfc9110(data, size, &instant, &position);

    nya_assert(result < NYA_TIME_PARSE_COUNT);
    nya_assert(position <= size, "a refusal pointed past the end of the input");

    if (result != NYA_TIME_PARSE_OK) return;

    // the format is fixed width with no alternative spellings, so what was accepted is what is written.
    nya_assert(size == NYA_RFC9110_LENGTH, "an accepted IMF-fixdate of %llu bytes", (unsigned long long)size);
    nya_assert(instant.ns % NYA_NS_PER_SECOND == 0, "an HTTP date has whole seconds");

    u8 text[NYA_RFC9110_LENGTH + 1] = { 0 };
    nya_assert(nya_instant_to_rfc9110(instant, text, sizeof(text)) == NYA_RFC9110_LENGTH);
    nya_assert(nya_memcmp(text, data, NYA_RFC9110_LENGTH) == 0, "an accepted date is not what its instant writes as");
}

#include "tests/fuzz/fuzz.h"
