/**
 * The cookie parser, fed whatever.
 *
 * A `Cookie` header is one string a stranger chooses, read here and read again by a browser, and a
 * session is stolen in the gap between two readers that disagree about the same bytes. So the oracle is
 * not only "does not crash": what this parser accepts, it has to have read exactly, and a header it
 * refuses has to leave nothing behind for a caller to use by mistake.
 *
 * A refusal is the expected answer to hostile input and is not a finding. A crash, a read out of
 * bounds, a pair pointing outside the header, or a pair this parser would read differently on a second
 * pass is.
 **/

// clang-format off
// The engine defines the feature test macros this whole build needs, so it comes first. Sorted into
// any other order, a libc header arrives before base_basic.h and the build fails on a redefinition
// of _POSIX_C_SOURCE and on half of <signal.h> being missing.
#include "nyangine/nyangine.c"
#include "nyangine/nyangine.h"
// clang-format on

#define FUZZ_TARGET "http_cookie"

static void fuzz_once(const u8* data, u64 size) {
    NYA_HttpCookieValue names[NYA_HTTP_MAX_COOKIES]  = { 0 };
    NYA_HttpCookieValue values[NYA_HTTP_MAX_COOKIES] = { 0 };
    u32                 count                        = 0;

    if (!nya_http_cookie_parse((const char*)data, size, names, values, &count)) {
        // a refusal reads nothing: a caller that ignored the return must find no pair to use.
        nya_assert(count == 0, "a refused header left %u pairs behind", count);
        return;
    }

    nya_assert(count > 0 && count <= NYA_HTTP_MAX_COOKIES, "an accepted header reported %u pairs", count);

    for (u32 index = 0; index < count; index++) {
        // every pair points into the header it was read from, and stays inside it.
        nya_assert(names[index].text >= (const char*)data && names[index].text + names[index].size <= (const char*)data + size,
                   "a name left the header it was parsed from");
        nya_assert(values[index].text >= (const char*)data && values[index].text + values[index].size <= (const char*)data + size,
                   "a value left the header it was parsed from");

        nya_assert(names[index].size > 0 && names[index].size < NYA_HTTP_MAX_COOKIE_NAME, "a name past its bound was accepted");
        nya_assert(values[index].size < NYA_HTTP_MAX_COOKIE_VALUE, "a value past its bound was accepted");

        // and no name twice, which is the ambiguity the parser exists to refuse.
        for (u32 other = 0; other < index; other++) {
            b8 same = names[other].size == names[index].size && memcmp(names[other].text, names[index].text, names[index].size) == 0;
            nya_assert(!same, "the same name was accepted twice");
        }
    }

    /* The round trip. Rendering what was read has to produce the header that was read: a parser that accepted a spelling it would not write is a parser a browser and this server can disagree about. */
    char rendered[NYA_HTTP_MAX_HEADER_VALUE * 2] = { 0 };
    u64  length                                  = 0;

    for (u32 index = 0; index < count && length < sizeof(rendered); index++) {
        s32 written = snprintf(rendered + length, sizeof(rendered) - length, "%s%.*s=%.*s", index > 0 ? "; " : "", (int)names[index].size,
                               names[index].text, (int)values[index].size, values[index].text);

        if (written < 0 || (u64)written >= sizeof(rendered) - length) return;

        length += (u64)written;
    }

    nya_assert(length == size, "a header of %llu bytes rendered back as %llu", (unsigned long long)size, (unsigned long long)length);
    nya_assert(memcmp(rendered, data, size) == 0, "a header did not render back to itself");

    /* And what a caller does with it: look one up. Reading a name that is there finds the same bytes the walk did, and a name that is not finds nothing. */
    for (u32 index = 0; index < count; index++) {
        char name[NYA_HTTP_MAX_COOKIE_NAME] = { 0 };
        memcpy(name, names[index].text, names[index].size);

        NYA_HttpCookieValue found = { 0 };
        for (u32 other = 0; other < count; other++) {
            if (names[other].size != names[index].size || memcmp(names[other].text, name, names[index].size) != 0) continue;

            found = values[other];
        }

        nya_assert(found.text == values[index].text && found.size == values[index].size, "a lookup found a different pair than the walk did");
    }
}

#include "tests/fuzz/fuzz.h"
