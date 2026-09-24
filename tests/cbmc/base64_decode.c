/**
 * @file base64_decode.c
 *
 * A bounded proof that the base64 decoder never writes past the buffer it reserved, and that the number
 * of bytes it produces is exactly the length its own size formula promises, whatever it is fed.
 *
 * The function under proof is `nya_base64_decode` from src/nyangine/base/base_base64.c. It is reached by
 * every embedded blob in a tilemap and every key in a settings file — anything a player can edit — so a
 * decoder that produced one byte more than it reserved room for would be a heap overflow on untrusted
 * input. The matching fuzz target is tests/fuzz/fuzz_base64.c; this proves for every input under the
 * bound what the fuzzer can only sample.
 *
 * The body below is a verbatim copy of the engine's decoder and its helper, lifted here rather than
 * #included because the real translation unit pulls in the whole engine header and a real arena
 * allocator, neither of which CBMC needs to reason about the decode loop. It is kept byte-for-byte in
 * step with the source. The one thing modelled is `nya_string_reserve`: the harness backs it with an
 * object of *exactly* the requested capacity, so any write one byte past it is a real out-of-bounds
 * store that --bounds-check reports. That is the "never writes past the output capacity" invariant,
 * proved rather than asserted.
 *
 * What CBMC checks here:
 *   - --bounds-check / --pointer-check: every store into the output and every load from the input is in
 *     bounds, for every input the harness admits.
 *   - the functional invariant: the produced length equals (stripped_len * 3) / 4 and so never exceeds
 *     (input_len * 3) / 4 — the decoder cannot be driven to emit more than its formula reserved.
 *
 * Run by `./build verify`. Small bounds keep it to a fraction of a second.
 */

#include <stdint.h>
#include <stdlib.h>

typedef _Bool   b8;
typedef uint8_t u8;
typedef uint32_t u32;
typedef uint64_t u64;

#define nullptr ((void*)0)

#define nya_assert(...) ((void)0)

/* ── the output string, modelled ────────────────────────────────────────────────────────────────── */

/* Only the two fields the decoder touches. `capacity` is what reserve was asked for; `items` is an
 * object of exactly that size, so the bounds checker catches a write at `items[capacity]` or beyond. */
typedef struct {
    u8* items;
    u64 length;
    u64 capacity;
} NYA_String;

static void nya_string_reserve(NYA_String* str, u64 capacity) {
    str->items    = malloc(capacity);
    str->capacity = capacity;
    __CPROVER_assume(capacity == 0 || str->items != nullptr);
}

/* ── mirrored from src/nyangine/base/base_base64.c ──────────────────────────────────────────────── */

static const char BASE64_CHARS[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static u8 _nya_base64_char_to_value(u8 c) {
    if (c >= 'A' && c <= 'Z') return (u8)(c - 'A');
    if (c >= 'a' && c <= 'z') return (u8)(c - 'a' + 26);
    if (c >= '0' && c <= '9') return (u8)(c - '0' + 52);
    if (c == '+') return 62;
    if (c == '/') return 63;

    return 0; // '=' padding or invalid
}

static void nya_base64_decode(NYA_String* base64, const u8* encoded, u64 len) {
    nya_assert(base64 != nullptr);
    nya_assert(encoded != nullptr);

    if (len == 0) return;

    // skip padding
    while (len > 0 && encoded[len - 1] == '=') len--;

    u64 decoded_len = (len * 3) / 4;
    nya_string_reserve(base64, decoded_len + 1);

    u64 i = 0, j = 0;
    while (i + 3 < len) {
        u32 triple = (_nya_base64_char_to_value(encoded[i]) << 18) | (_nya_base64_char_to_value(encoded[i + 1]) << 12) |
                     (_nya_base64_char_to_value(encoded[i + 2]) << 6) | _nya_base64_char_to_value(encoded[i + 3]);

        base64->items[j++] = (u8)((triple >> 16) & 0xFF);
        base64->items[j++] = (u8)((triple >> 8) & 0xFF);
        base64->items[j++] = (u8)(triple & 0xFF);

        i += 4;
    }

    // remaining chars
    if (i < len) {
        u32 triple = _nya_base64_char_to_value(encoded[i]) << 18;
        if (i + 1 < len) {
            triple             |= _nya_base64_char_to_value(encoded[i + 1]) << 12;
            base64->items[j++]  = (u8)((triple >> 16) & 0xFF);

            if (i + 2 < len) {
                triple             |= _nya_base64_char_to_value(encoded[i + 2]) << 6;
                base64->items[j++]  = (u8)((triple >> 8) & 0xFF);
            }
        }
    }

    base64->items[j] = '\0';
    base64->length   = j;
}

/* ── the harness ────────────────────────────────────────────────────────────────────────────────── */

/* Small enough for --unwinding-assertions to prove the padding-strip and decode loops terminate, big
 * enough to reach every branch of the four-char group and its one-, two- and three-char tails. */
#define BASE64_MAX_INPUT 8u

extern u64 nondet_len(void);

int main(void) {
    u64 len = nondet_len();
    __CPROVER_assume(len <= BASE64_MAX_INPUT);

    /* An input object of exactly `len` nondeterministic bytes: a read past it is a real OOB. */
    u8* encoded = malloc(len);
    __CPROVER_assume(len == 0 || encoded != nullptr);

    NYA_String out = { .items = nullptr, .length = 0, .capacity = 0 };

    nya_base64_decode(&out, encoded, len);

    /* The decoder strips trailing '=' before sizing, so the produced length is pinned to the stripped
     * length's formula. Recompute the strip here to state the exact invariant. */
    u64 stripped = len;
    /* This loop mirrors the decoder's own padding strip; bounded by len <= BASE64_MAX_INPUT. */
    while (stripped > 0 && encoded[stripped - 1] == '=') stripped--;

    u64 expected = (stripped * 3) / 4;

    __CPROVER_assert(out.length == expected, "decoded length equals the size formula");
    __CPROVER_assert(out.length <= (len * 3) / 4, "decoded length never exceeds the input-derived bound");

    /* And the write that terminated the output landed inside the reserved capacity. When len is zero the
     * decoder returns before reserving anything, so guard on capacity having been set. */
    if (out.capacity > 0) __CPROVER_assert(out.length < out.capacity, "decode never writes past the reserved capacity");

    return 0;
}
