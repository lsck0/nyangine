/**
 * @file url_decode_run.c
 *
 * A bounded proof that the URL percent-decoder never reads past its input, never writes past the
 * caller's buffer, and reports a length and an offset that stay inside those two spans, whatever bytes
 * it is fed.
 *
 * The functions under proof are `_nya_url_decode_run` and its helpers `_nya_url_decode_next` and
 * `_nya_url_hex_value` from src/nyangine/base/base_url.c. They decode `%XX` escapes (and, form style, a
 * `+`) out of an untrusted URL run — a request target's query, a form field — into a fixed buffer the
 * caller sized. A read one byte past the input on a `%` at the very end, or a store one byte past the
 * buffer, would be an overread or a heap overflow on attacker-controlled input. The matching fuzzers
 * sample this; the proof covers every input under the bound.
 *
 * The bodies below are a verbatim copy of the engine's, lifted rather than #included because the real
 * translation unit pulls in the whole base header. They are kept byte-for-byte in step with the source.
 * The one thing modelled is the caller's storage: `buffer` is an object of *exactly* `capacity` bytes,
 * so any store at `buffer[capacity]` or beyond is a real out-of-bounds write that --bounds-check reports.
 *
 * What CBMC checks here:
 *   - --bounds-check / --pointer-check: every `text[...]` load is in [0, size) and every `buffer[...]`
 *     store is in [0, capacity), for every input the harness admits — including a `%` with zero, one or
 *     two bytes left after it, which is the overread case.
 *   - the functional invariants: `*out_length <= capacity` and `*out_offset <= size` on every outcome;
 *     on success `*out_offset == size` and `*out_length <= size` (each output byte consumes at least one
 *     input byte, so the decode can never produce more bytes than it read).
 *
 * Run by `./build verify`. Small bounds keep it to a fraction of a second.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

typedef _Bool    b8;
typedef char     c8;
typedef uint8_t  u8;
typedef uint32_t u32;
typedef uint64_t u64;

#define nullptr ((void*)0)

#define nya_assert(...) ((void)0)

#define OUT

/* ── the functions under proof, verbatim from src/nyangine/base/base_url.c ───────────────────────── */

static b8 _nya_url_hex_value(char character, OUT u8* out_value) {
    if (character >= '0' && character <= '9') {
        *out_value = (u8)(character - '0');
        return true;
    }

    if (character >= 'a' && character <= 'f') {
        *out_value = (u8)(character - 'a' + 10);
        return true;
    }

    if (character >= 'A' && character <= 'F') {
        *out_value = (u8)(character - 'A' + 10);
        return true;
    }

    return false;
}

static b8 _nya_url_decode_next(const char* text, u64 size, u64* index, b8 plus_is_space, OUT u8* out_byte) {
    nya_assert(*index < size);

    char character = text[*index];

    if (character == '%') {
        u8 high = 0;
        u8 low  = 0;

        // size - *index rather than *index + 2, so nothing is ever added to an index before the compare.
        if (size - *index <= 2) return false;
        if (!_nya_url_hex_value(text[*index + 1], &high) || !_nya_url_hex_value(text[*index + 2], &low)) return false;

        *out_byte  = (u8)((u32)high * 16U + (u32)low);
        *index    += 3;

        return true;
    }

    *out_byte  = plus_is_space && character == '+' ? (u8)' ' : (u8)character;
    *index    += 1;

    return true;
}

static b8 _nya_url_decode_run(const char* text, u64 size, b8 plus_is_space, OUT u8* buffer, u64 capacity, OUT u64* out_length, OUT u64* out_offset) {
    u64 index   = 0;
    u64 written = 0;

    *out_length = 0;
    *out_offset = size;

    while (index < size) {
        u64 at   = index;
        u8  byte = 0;

        if (!_nya_url_decode_next(text, size, &index, plus_is_space, &byte)) {
            *out_offset = at;
            return false;
        }

        if (written >= capacity) return false;

        buffer[written++] = byte;
    }

    *out_length = written;

    return true;
}

/* ── the harness ─────────────────────────────────────────────────────────────────────────────────── */

#define MAX_SIZE     6
#define MAX_CAPACITY 6

int main(void) {
    u64 size = nondet_u64();
    __CPROVER_assume(size <= MAX_SIZE);

    u64 capacity = nondet_u64();
    __CPROVER_assume(capacity <= MAX_CAPACITY);

    /* Exactly `size` input bytes and exactly `capacity` output bytes, so any load past the input or any
     * store past the buffer is a real out-of-bounds access the checker reports. */
    char* text   = size == 0 ? nullptr : malloc(size);
    u8*   buffer = capacity == 0 ? nullptr : malloc(capacity);
    __CPROVER_assume(size == 0 || text != nullptr);
    __CPROVER_assume(capacity == 0 || buffer != nullptr);

    b8  plus_is_space = nondet_bool();
    u64 out_length    = 0;
    u64 out_offset    = 0;

    b8 ok = _nya_url_decode_run(text, size, plus_is_space, buffer, capacity, &out_length, &out_offset);

    /* Whatever the outcome: the produced length never exceeds the buffer, and the reported offset never
     * points past the input — the two things a caller indexes with. */
    __CPROVER_assert(out_length <= capacity, "the decoded length never exceeds the buffer capacity");
    __CPROVER_assert(out_offset <= size, "the reported offset never points past the input");

    if (ok) {
        /* A clean decode consumed the whole input, and could not have emitted more bytes than it read. */
        __CPROVER_assert(out_offset == size, "a successful decode consumes the whole input");
        __CPROVER_assert(out_length <= size, "a successful decode emits no more bytes than it read");
    }

    return 0;
}
