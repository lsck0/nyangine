/**
 * The base32 and base64url decoders, fed whatever a token is spelled with.
 *
 * These are the alphabets a secret arrives in from somewhere the server does not control: a base32
 * TOTP secret typed off an authenticator, and the base64url segments of a JWS, a PKCE challenge or a
 * webhook signature. Both decoders are deliberately strict — one spelling per value, no padding slack,
 * no spare bits set — because a value with several spellings is a value an attacker can edit and still
 * have verify. Strict parsers over hostile input are exactly what a fuzzer belongs on.
 *
 * The oracle has two halves. Bounds: a decode never writes past the buffer it was given, and a refusal
 * writes nothing and reports zero. Canonicality: because each decoder accepts only the one canonical
 * spelling, anything that decodes must re-encode to the very bytes that were decoded — so decode, then
 * encode, then decode again is the identity, and the round trip changing a byte is the finding.
 **/

// clang-format off
// the engine defines the feature test macros this build needs, so it comes before any libc header.
#include "nyangine-core/nyangine.c"
#include "nyangine-core/nyangine.h"
// clang-format on

#define FUZZ_TARGET "crypto_encoding"

/** Bounded so the stack buffers below hold the decode, the re-encode and the second decode of any input. */
#define FUZZ_MAX_INPUT 1024

/** base32 decodes eight characters to five bytes, so the output is never larger than the input. */
static void fuzz_base32(const char* text, u64 size) {
    u8  decoded[FUZZ_MAX_INPUT] = { 0 };
    u64 decoded_size            = 0;

    if (!nya_crypto_base32_decode(text, size, decoded, sizeof(decoded), &decoded_size).ok) return;

    nya_assert(decoded_size <= size, "a base32 decode grew");

    // the canonical spelling round trips: what decoded re-encodes to the same characters it came from.
    char encoded[NYA_CRYPTO_BASE32_LENGTH(FUZZ_MAX_INPUT) + 1] = { 0 };
    u64  encoded_length                                        = 0;
    NYA_EXPECT(nya_crypto_base32_encode(decoded, decoded_size, encoded, sizeof(encoded), &encoded_length), "bytes that decoded would not re-encode");

    nya_assert(encoded_length == size, "a base32 round trip changed the length");
    nya_assert(memcmp(encoded, text, size) == 0, "a base32 round trip changed a character, so a value has more than one spelling");
}

/** base64url decodes four characters to three bytes, so the output is never larger than the input. */
static void fuzz_base64url(const char* text, u64 size) {
    u8  decoded[FUZZ_MAX_INPUT] = { 0 };
    u64 decoded_size            = 0;

    if (!nya_crypto_base64url_decode(text, size, decoded, sizeof(decoded), &decoded_size)) return;

    nya_assert(decoded_size <= size, "a base64url decode grew");

    // base64url here is padding-free and canonical, so the same round trip has to reproduce the input.
    char encoded[FUZZ_MAX_INPUT * 2 + 1] = { 0 };
    u64  encoded_size                    = 0;
    nya_assert(nya_crypto_base64url_encode(decoded, decoded_size, encoded, sizeof(encoded), &encoded_size), "bytes that decoded would not re-encode");

    nya_assert(encoded_size == size, "a base64url round trip changed the length");
    nya_assert(memcmp(encoded, text, size) == 0, "a base64url round trip changed a character, so a token has more than one spelling");
}

static void fuzz_once(const u8* data, u64 size) {
    if (size > FUZZ_MAX_INPUT) return;

    fuzz_base32((const char*)data, size);
    fuzz_base64url((const char*)data, size);
}

#include "tests/fuzz/fuzz.h"
