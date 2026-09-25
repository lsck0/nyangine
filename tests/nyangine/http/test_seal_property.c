/**
 * The sealed cookie, stated as laws rather than examples: over thousands of random plaintexts, labels,
 * ttls and secrets, a seal round trips; a single altered byte anywhere in the token makes it not open;
 * a token sealed under one label never opens under another; a wrong secret never opens; and a plaintext
 * past the bound is refused rather than truncated into a token that unseals to the wrong thing.
 *
 * The example cases — the exact expiry second, the max-size fit — live in test_seal.c. This is the part
 * a stopwatch of examples cannot reach: the same guarantees held against inputs nobody chose. No clock
 * is pinned, so every ttl drawn here is well in the future and every token opens now; expiry is
 * test_seal.c's, since a law cannot sleep out a thousand ttls.
 **/

// A larger entropy budget than the default 1024 so a full-size plaintext (300 bytes) plus a long secret
// fit in one case with real bytes rather than the zeroes a draw past the end reads. Set before the
// engine is included, which is also what makes this file compile its own unity build.
#define NYA_PROPERTY_ENTROPY_MAX 2048

#include "nyangine-core/nyangine.c"
#include "nyangine-core/nyangine.h"

/** Cases per law. Thousands, so a rare byte pattern the crypto mishandles has room to turn up. */
#define CASES 4000

/** Fixed, so the suite is the same run every time. "seal" and "props" in ASCII. */
#define SEED 0x7365616C70726F70ULL

/* HELPERS */

/** One drawn seal: a secret at least the minimum, a non-empty label, a bounded plaintext and a live ttl. */
typedef struct {
    u8   secret[128];
    u64  secret_size;
    char label[NYA_HTTP_MAX_COOKIE_NAME];
    u8   plaintext[NYA_HTTP_SEAL_MAX_PLAINTEXT];
    u64  plaintext_size;
    u64  ttl_s;
} Drawn;

/** A label of one to a few printable, non-separator characters: what a real cookie name looks like. */
static void draw_label(NYA_Property* property, OUT char* out, u64 capacity) {
    u64 length = 1 + nya_property_draw_below(property, 8);
    if (length >= capacity) length = capacity - 1;

    for (u64 index = 0; index < length; index++) {
        // Printable and not a space or a control: a label is authenticated as bytes, but keeping it to what a cookie name may hold is what makes the draw resemble the real caller.
        u8 character = (u8)(0x21 + nya_property_draw_below(property, 0x7E - 0x21));
        out[index]   = (char)character;
    }

    out[length] = '\0';
}

static void draw_seal(NYA_Property* property, OUT Drawn* drawn) {
    // A secret from the minimum up to something longer than blake2b's key, so the fold path is exercised.
    drawn->secret_size = NYA_HTTP_SEAL_MIN_SECRET_BYTES + nya_property_draw_below(property, sizeof(drawn->secret) - NYA_HTTP_SEAL_MIN_SECRET_BYTES);
    nya_property_draw_bytes(property, drawn->secret, (u32)drawn->secret_size);

    draw_label(property, drawn->label, sizeof(drawn->label));

    drawn->plaintext_size = nya_property_draw_below(property, NYA_HTTP_SEAL_MAX_PLAINTEXT + 1);
    nya_property_draw_bytes(property, drawn->plaintext, (u32)drawn->plaintext_size);

    // Well in the future, so the token is live when it is opened a microsecond later; expiry is test_seal.c's, which sleeps a one-second ttl out.
    drawn->ttl_s = 60 + nya_property_draw_below(property, 1000000);
}

/* LAWS */

/** unseal(seal(x)) == x: the same bytes, the same size, under the same secret and label. */
static b8 law_round_trip(NYA_Property* property) {
    Drawn drawn = { 0 };
    draw_seal(property, &drawn);

    char token[NYA_HTTP_SEAL_MAX_TOKEN] = { 0 };
    NYA_Error sealed = nya_http_seal(drawn.secret, drawn.secret_size, drawn.label, drawn.plaintext, drawn.plaintext_size, drawn.ttl_s, token, sizeof(token));

    nya_property_note(property, "%llu-byte plaintext under a %llu-byte secret did not seal", (unsigned long long)drawn.plaintext_size,
                      (unsigned long long)drawn.secret_size);
    if (!sealed.ok) return false;

    u8  back[NYA_HTTP_SEAL_MAX_PLAINTEXT] = { 0 };
    u64 size                             = 0;

    b8 opened = nya_http_unseal(drawn.secret, drawn.secret_size, drawn.label, token, strlen(token), back, sizeof(back), &size);

    nya_property_note(property, "a %llu-byte plaintext did not round trip", (unsigned long long)drawn.plaintext_size);
    return opened && size == drawn.plaintext_size && memcmp(back, drawn.plaintext, drawn.plaintext_size) == 0;
}

/** One flipped bit anywhere in the token — nonce, version, expiry, ciphertext or tag — makes it not open. */
static b8 law_any_flipped_byte_is_refused(NYA_Property* property) {
    Drawn drawn = { 0 };
    draw_seal(property, &drawn);

    char token[NYA_HTTP_SEAL_MAX_TOKEN] = { 0 };
    if (!nya_http_seal(drawn.secret, drawn.secret_size, drawn.label, drawn.plaintext, drawn.plaintext_size, drawn.ttl_s, token, sizeof(token)).ok) {
        return true; // a plaintext that would not seal has no token to tamper with; not this law's business.
    }

    u64 length = strlen(token);
    if (length == 0) return true;

    u64 position = nya_property_draw_below(property, length);
    u8  mask     = (u8)(1U + nya_property_draw_below(property, 255)); // never zero: a no-op flip is not a flip.

    token[position] = (char)((u8)token[position] ^ mask);

    u8  back[NYA_HTTP_SEAL_MAX_PLAINTEXT] = { 0 };
    u64 size                             = 0;

    // A base64url alphabet is 64 of 256 byte values, so most flips make the token stop decoding and the rest make the tag stop matching; either way it must not open, and it must report no plaintext.
    b8 opened = nya_http_unseal(drawn.secret, drawn.secret_size, drawn.label, token, length, back, sizeof(back), &size);

    nya_property_note(property, "a flip of byte %llu of %llu was accepted", (unsigned long long)position, (unsigned long long)length);
    return !opened && size == 0;
}

/** A token sealed under label A never opens under a different label B, whatever the bytes. */
static b8 law_wrong_label_is_refused(NYA_Property* property) {
    Drawn drawn = { 0 };
    draw_seal(property, &drawn);

    char other[NYA_HTTP_MAX_COOKIE_NAME] = { 0 };
    draw_label(property, other, sizeof(other));

    // The law is about two different labels; identical ones are the round-trip law's job.
    if (strcmp(drawn.label, other) == 0) return true;

    char token[NYA_HTTP_SEAL_MAX_TOKEN] = { 0 };
    if (!nya_http_seal(drawn.secret, drawn.secret_size, drawn.label, drawn.plaintext, drawn.plaintext_size, drawn.ttl_s, token, sizeof(token)).ok) {
        return true;
    }

    u8  back[NYA_HTTP_SEAL_MAX_PLAINTEXT] = { 0 };
    u64 size                             = 0;

    b8 opened = nya_http_unseal(drawn.secret, drawn.secret_size, other, token, strlen(token), back, sizeof(back), &size);

    nya_property_note(property, "sealed under '%s', opened under '%s'", drawn.label, other);
    return !opened && size == 0;
}

/** A token opened with any other secret never opens: the key is the whole of the trust. */
static b8 law_wrong_secret_is_refused(NYA_Property* property) {
    Drawn drawn = { 0 };
    draw_seal(property, &drawn);

    u8  other[128]   = { 0 };
    u64 other_size   = NYA_HTTP_SEAL_MIN_SECRET_BYTES + nya_property_draw_below(property, sizeof(other) - NYA_HTTP_SEAL_MIN_SECRET_BYTES);
    nya_property_draw_bytes(property, other, (u32)other_size);

    // Two secrets that happen to be equal are the round-trip law's; only a genuinely different one tests this.
    if (other_size == drawn.secret_size && memcmp(other, drawn.secret, other_size) == 0) return true;

    char token[NYA_HTTP_SEAL_MAX_TOKEN] = { 0 };
    if (!nya_http_seal(drawn.secret, drawn.secret_size, drawn.label, drawn.plaintext, drawn.plaintext_size, drawn.ttl_s, token, sizeof(token)).ok) {
        return true;
    }

    u8  back[NYA_HTTP_SEAL_MAX_PLAINTEXT] = { 0 };
    u64 size                             = 0;

    b8 opened = nya_http_unseal(other, other_size, drawn.label, token, strlen(token), back, sizeof(back), &size);

    nya_property_note(property, "a token opened under a %llu-byte foreign secret", (unsigned long long)other_size);
    return !opened && size == 0;
}

/** A plaintext past the bound is refused, not truncated: no token comes back to unseal to the wrong thing. */
static b8 law_oversized_is_refused(NYA_Property* property) {
    u8  secret[64]  = { 0 };
    u64 secret_size = NYA_HTTP_SEAL_MIN_SECRET_BYTES + nya_property_draw_below(property, sizeof(secret) - NYA_HTTP_SEAL_MIN_SECRET_BYTES);
    nya_property_draw_bytes(property, secret, (u32)secret_size);

    // One past the bound, up to a few hundred over: the size is the attacker's to pick, and every value above the bound must be a refusal.
    u64 oversized = NYA_HTTP_SEAL_MAX_PLAINTEXT + 1 + nya_property_draw_below(property, 512);

    // The plaintext buffer only needs to be a valid pointer of that size; its contents do not matter, because a refused seal never reads them into a token, so they are left zeroed rather than drawn.
    u8* plaintext = nya_arena_alloc(property->allocator, oversized);

    char token[NYA_HTTP_SEAL_MAX_TOKEN] = { 0 };
    NYA_Error sealed = nya_http_seal(secret, secret_size, "s", plaintext, oversized, 900, token, sizeof(token));

    nya_property_note(property, "a %llu-byte plaintext (bound is %d) was not refused", (unsigned long long)oversized, NYA_HTTP_SEAL_MAX_PLAINTEXT);
    // Refused, and the out buffer left as an empty string rather than a partial token.
    return !sealed.ok && token[0] == '\0';
}

/** A secret below the minimum is refused, both to seal and to unseal: a short key is guessed, not stolen. */
static b8 law_short_secret_is_refused(NYA_Property* property) {
    u64 secret_size = nya_property_draw_below(property, NYA_HTTP_SEAL_MIN_SECRET_BYTES); // 0 .. min-1
    u8  secret[NYA_HTTP_SEAL_MIN_SECRET_BYTES] = { 0 };
    nya_property_draw_bytes(property, secret, (u32)secret_size);

    char token[NYA_HTTP_SEAL_MAX_TOKEN] = { 0 };
    NYA_Error sealed = nya_http_seal(secret, secret_size, "s", (const u8*)"x", 1, 900, token, sizeof(token));

    // And unseal of anything under a short secret is a plain false, never a read of the buffer.
    u8  back[8] = { 0 };
    u64 size    = 0;
    b8  opened  = nya_http_unseal(secret, secret_size, "s", "whatever", 8, back, sizeof(back), &size);

    nya_property_note(property, "a %llu-byte secret (minimum is %d) was accepted", (unsigned long long)secret_size, NYA_HTTP_SEAL_MIN_SECRET_BYTES);
    return !sealed.ok && token[0] == '\0' && !opened && size == 0;
}

s32 main(void) {
    setvbuf(stdout, nullptr, _IONBF, 0);

    u32 failures = 0;

    failures += nya_property_check("unseal undoes seal", CASES, SEED, law_round_trip);
    failures += nya_property_check("any flipped byte is refused and reads nothing", CASES, SEED, law_any_flipped_byte_is_refused);
    failures += nya_property_check("a wrong label never opens", CASES, SEED, law_wrong_label_is_refused);
    failures += nya_property_check("a wrong secret never opens", CASES, SEED, law_wrong_secret_is_refused);
    failures += nya_property_check("an oversized plaintext is refused, not truncated", CASES, SEED, law_oversized_is_refused);
    failures += nya_property_check("a secret below the minimum is refused", CASES, SEED, law_short_secret_is_refused);

    return failures == 0 ? 0 : 1;
}
