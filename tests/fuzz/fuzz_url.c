/**
 * The URL parser and the percent codec, fed whatever. Every request target the HTTP server takes and
 * every URL a websocket is opened with goes through here first.
 *
 * The same bytes go through both parse calls and through the decoder. Whatever parses has to keep the
 * promises at the top of base_url.h and survive every call that takes an NYA_Url; whatever decodes has
 * to encode and decode back to itself.
 **/

// clang-format off
// the engine defines the feature test macros this build needs, so it comes before any libc header.
#include "nyangine-core/nyangine.c"
#include "nyangine-core/nyangine.h"
// clang-format on

#define FUZZ_TARGET "url"

/** Query names asked for on every URL that parses: a common one, an empty one, and one with a byte that must be decoded to match. */
static const NYA_ConstCString FUZZ_URL_NAMES[] = { "q", "", "a b" };

/** Everything an NYA_Url that parsed promises, and every call that takes one. */
static void fuzz_check_url(const NYA_Url* url) {
    nya_assert(url->scheme < NYA_URL_SCHEME_COUNT && url->host_kind < NYA_URL_HOST_COUNT);
    nya_assert(url->length <= NYA_URL_MAX_BYTES);
    nya_assert(!url->has_port || url->port > 0, "port zero parsed");
    nya_assert((url->scheme == NYA_URL_SCHEME_NONE) == (url->host_kind == NYA_URL_HOST_NONE), "a host without a scheme, or the reverse");

    // the components are stored back to back, so their lengths add up to what was stored.
    NYA_UrlSpan spans[] = { url->host, url->path, url->query, url->fragment };
    u32         total   = 0;

    for (u32 i = 0; i < nya_carray_length(spans); i++) {
        nya_assert((u32)spans[i].offset + spans[i].length <= url->length, "a span reaches past the stored text");
        total += spans[i].length;
    }

    nya_assert(total == url->length, "stored bytes that belong to no component");

    for (u16 i = 0; i < url->length; i++) nya_assert((u8)url->text[i] > 0x20 && (u8)url->text[i] < 0x7F, "a raw control, space or non-ASCII byte was stored");

    // rendering and parsing again is the identity.
    char formatted[NYA_URL_MAX_BYTES + 1];
    u64  formatted_length = 0;
    NYA_EXPECT(nya_url_format(url, formatted, sizeof(formatted), &formatted_length), "a url that parsed could not be rendered");

    NYA_Url   again  = { 0 };
    NYA_Error parsed = url->scheme == NYA_URL_SCHEME_NONE ? nya_url_parse_target(formatted, formatted_length, &again, nullptr)
                                                          : nya_url_parse(formatted, formatted_length, &again, nullptr);

    nya_assert(parsed.ok, "'%s' rendered from a url that parsed, and was refused", formatted);
    nya_assert(again.path.length == url->path.length && again.query.length == url->query.length && again.host.length == url->host.length);

    // the path decodes into a buffer that fits it, and never holds a NUL or a '/' that was not a separator.
    char path[NYA_URL_MAX_BYTES + 1];
    u64  path_length = 0;
    NYA_EXPECT(nya_url_path_decode(url, path, sizeof(path), &path_length), "a parsed path would not decode");
    nya_assert(path_length <= url->path.length);

    u32 raw_slashes     = 0;
    u32 decoded_slashes = 0;
    for (u16 i = 0; i < url->path.length; i++) raw_slashes += url->text[url->path.offset + i] == '/';
    for (u64 i = 0; i < path_length; i++) decoded_slashes += path[i] == '/';
    nya_assert(raw_slashes == decoded_slashes, "decoding moved a segment boundary");

    for (u32 i = 0; i < nya_carray_length(FUZZ_URL_NAMES); i++) {
        char value[64];
        b8   found = false;

        // a repeated name and a value longer than the buffer are answers; a crash is not.
        (void)nya_url_query_find(url, FUZZ_URL_NAMES[i], value, sizeof(value), &found);
        nya_assert(found || value[0] == '\0', "an absent parameter left something in the buffer");
    }
}

static void fuzz_once(const u8* data, u64 size) {
    NYA_Url        url     = { 0 };
    NYA_UrlFailure failure = { 0 };

    if (nya_url_parse((const char*)data, size, &url, &failure).ok) {
        fuzz_check_url(&url);
    } else {
        nya_assert(failure.rule != NYA_URL_RULE_NONE && failure.rule < NYA_URL_RULE_COUNT, "a refusal without a rule");
        nya_assert(failure.offset <= size, "a refusal pointing past the input");
    }

    if (nya_url_parse_target((const char*)data, size, &url, &failure).ok) {
        fuzz_check_url(&url);
    } else {
        nya_assert(failure.rule != NYA_URL_RULE_NONE && failure.rule < NYA_URL_RULE_COUNT, "a refusal without a rule");
        nya_assert(failure.offset <= size, "a refusal pointing past the input");
    }

    /* The decoder on the same bytes. Bounded to what the URL bound allows, since the encoded form is up to three times as long and the buffers below are on the stack. */
    if (size > NYA_URL_MAX_BYTES) return;

    u8  decoded[NYA_URL_MAX_BYTES];
    u64 decoded_length = 0;

    if (!nya_percent_decode((const char*)data, size, decoded, sizeof(decoded), &decoded_length).ok) return;

    nya_assert(decoded_length <= size, "a decode grew");

    char encoded[NYA_URL_MAX_BYTES * 3 + 1];
    u64  encoded_length = 0;
    NYA_EXPECT(nya_percent_encode(decoded, decoded_length, encoded, sizeof(encoded), &encoded_length), "three bytes per byte did not fit");

    u8  again[NYA_URL_MAX_BYTES];
    u64 again_length = 0;
    NYA_EXPECT(nya_percent_decode(encoded, encoded_length, again, sizeof(again), &again_length), "the decoder refused what the encoder wrote");

    nya_assert(again_length == decoded_length && nya_memcmp(again, decoded, decoded_length) == 0, "the percent round trip changed the bytes");
}

#include "tests/fuzz/fuzz.h"
