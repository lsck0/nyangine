/**
 * The unseal path, fed whatever a client can put in a cookie.
 *
 * A sealed token comes back from a place the server does not control — a cookie a browser stored, a
 * value copied off a wire — so the bytes handed to nya_http_unseal are an attacker's to choose. The
 * oracle is not "it opens": almost nothing here will. It is that trying to open arbitrary bytes never
 * reads out of bounds, never asserts, and never reports a plaintext it did not actually produce. A token
 * that does not open is the expected answer and not a finding; a crash on one is.
 *
 * The corpus seeds the base64url decode and the AEAD open with strings of the right shape so a fuzzer
 * reaches past the first `return false`; a genuine round trip is test_seal.c's and test_seal_property.c's.
 **/

// clang-format off
#include "nyangine-core/nyangine.c"
#include "nyangine-core/nyangine.h"
// clang-format on

#define FUZZ_TARGET "http_unseal"

/** A fixed key of the minimum length, so the shape of the secret is never what makes a case interesting. */
static const u8 SECRET[] = "0123456789abcdef0123456789abcdef";
#define SECRET_SIZE (sizeof(SECRET) - 1)

static void fuzz_once(const u8* data, u64 size) {
    u8  plaintext[NYA_HTTP_SEAL_MAX_PLAINTEXT] = { 0 };
    u64 plaintext_size                         = 0;

    // The bytes are the token. A label the server would actually use; a wrong label is one more reason to refuse and is covered by the property tests, not needed to reach the decode and decrypt here.
    b8 opened = nya_http_unseal(SECRET, SECRET_SIZE, "session", (const char*)data, size, plaintext, sizeof(plaintext), &plaintext_size);

    if (!opened) {
        // A refusal produces no plaintext: a caller that ignored the return must find nothing to use.
        nya_assert(plaintext_size == 0, "a refused token reported %llu plaintext bytes", (unsigned long long)plaintext_size);
        return;
    }

    // An open never claims more bytes than the buffer holds, and — the whole promise of a seal — opening the identical bytes a second time gives the identical plaintext, byte for byte.
    nya_assert(plaintext_size <= sizeof(plaintext), "an opened token claimed %llu bytes into a %zu buffer", (unsigned long long)plaintext_size,
               sizeof(plaintext));

    u8  again[NYA_HTTP_SEAL_MAX_PLAINTEXT] = { 0 };
    u64 again_size                         = 0;

    b8 reopened = nya_http_unseal(SECRET, SECRET_SIZE, "session", (const char*)data, size, again, sizeof(again), &again_size);

    nya_assert(reopened && again_size == plaintext_size && memcmp(again, plaintext, plaintext_size) == 0, "unseal was not deterministic for one token");

    // The same token under a different label must not open: the label is authenticated, so a token that opened in one slot cannot open in another.
    u8  wrong_label[NYA_HTTP_SEAL_MAX_PLAINTEXT] = { 0 };
    u64 wrong_label_size                         = 0;

    b8 other = nya_http_unseal(SECRET, SECRET_SIZE, "theme", (const char*)data, size, wrong_label, sizeof(wrong_label), &wrong_label_size);
    nya_assert(!other && wrong_label_size == 0, "a token opened under a label it was not sealed with");
}

#include "tests/fuzz/fuzz.h"
