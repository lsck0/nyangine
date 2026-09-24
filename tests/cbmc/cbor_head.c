/**
 * @file cbor_head.c
 *
 * A bounded proof that the CBOR reader's initial-byte parser is memory safe and never runs its cursor
 * off the end of the buffer, whatever bytes an attacker puts in front of it.
 *
 * The function under proof is `_nya_cbor_head` from src/nyangine/serde/serde_cbor.c: the one place a
 * byte is taken off a CBOR buffer, and so the one place the bounds check for an item's argument lives.
 * Every `nya_cbor_read_*` and the recursive `nya_cbor_skip` reach it, so if it can be driven to read
 * past `size`, the whole reader can. It parses fully untrusted input — a savegame, a reflected blob,
 * anything a player can hand the engine.
 *
 * The body below is a verbatim copy of the engine's function and its constants, lifted here rather than
 * #included because the real translation unit pulls in the whole engine header, which CBMC has no reason
 * to parse to reason about forty lines of pointer arithmetic. It is kept byte-for-byte in step with the
 * source; the two invariants proved are the ones the source comment promises ("no read ever moves it
 * past size").
 *
 * What CBMC checks here:
 *   - --bounds-check / --pointer-check: every `data[...]` read is inside the allocated buffer, for every
 *     input the harness admits. This is the out-of-bounds-read proof.
 *   - the functional invariant: on any outcome the cursor stays within the buffer and only moves
 *     forward, so no caller downstream can be handed an offset past the end.
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
#define false   0
#define true    1

/* Preconditions in the engine are asserted with nya_assert; the harness only ever passes valid
 * pointers, so the checks are a no-op here and the copied body stays verbatim. */
#define nya_assert(...) ((void)0)

/* ── mirrored from src/nyangine/serde/serde_cbor.h ──────────────────────────────────────────────── */

typedef struct {
    const u8* data;
    u64       size;
    u64       offset;
} NYA_CborReader;

/* ── mirrored from src/nyangine/serde/serde_cbor.c ──────────────────────────────────────────────── */

#define _NYA_CBOR_INFO_1_BYTE  24
#define _NYA_CBOR_INFO_2_BYTES 25
#define _NYA_CBOR_INFO_4_BYTES 26
#define _NYA_CBOR_INFO_8_BYTES 27

static b8 _nya_cbor_head(NYA_CborReader* reader, u8* out_major, u64* out_argument) {
    *out_major    = 0;
    *out_argument = 0;

    if (reader->offset >= reader->size) return false;

    u8 initial = reader->data[reader->offset];

    reader->offset += 1;

    u8 major = (u8)(initial >> 5);
    u8 info  = (u8)(initial & 0x1F);

    *out_major = major;

    if (info < _NYA_CBOR_INFO_1_BYTE) {
        *out_argument = info;
        return true;
    }

    u32 width = 0;

    switch (info) {
        case _NYA_CBOR_INFO_1_BYTE: width = 1; break;
        case _NYA_CBOR_INFO_2_BYTES: width = 2; break;
        case _NYA_CBOR_INFO_4_BYTES: width = 4; break;
        case _NYA_CBOR_INFO_8_BYTES: width = 8; break;
        default: return false;
    }

    if ((u64)width > reader->size - reader->offset) return false;

    u64 argument = 0;

    for (u32 index = 0; index < width; index++) {
        argument = (argument << 8) | (u64)reader->data[reader->offset + index];
    }

    reader->offset += width;

    *out_argument = argument;

    return true;
}

/* ── the harness ────────────────────────────────────────────────────────────────────────────────── */

/* Small enough that CBMC's --unwinding-assertions prove the argument-reading loop (at most eight bytes
 * wide) really does exit, yet big enough to admit every argument width and a cursor anywhere inside. */
#define CBOR_MAX_SIZE 12u

extern u64 nondet_size(void);
extern u64 nondet_offset(void);

int main(void) {
    u64 size = nondet_size();
    __CPROVER_assume(size <= CBOR_MAX_SIZE);

    /* An object of exactly `size` bytes with fully nondeterministic contents: reading past `size` is a
     * real out-of-bounds access CBMC will report, which is what makes the bounds check meaningful. */
    u8* data = malloc(size);
    __CPROVER_assume(size == 0 || data != nullptr);

    /* The cursor may sit anywhere from the start to one past the last byte, as it does mid-parse. */
    u64 offset = nondet_offset();
    __CPROVER_assume(offset <= size);

    NYA_CborReader reader = { .data = data, .size = size, .offset = offset };

    u64 offset_before = reader.offset;

    u8  major    = 0;
    u64 argument = 0;

    b8 ok = _nya_cbor_head(&reader, &major, &argument);

    /* Whatever the outcome, the cursor never leaves the buffer and never rewinds: a hostile byte
     * stream cannot make the reader hand a downstream read an offset past the end. */
    __CPROVER_assert(reader.offset <= reader.size, "cbor head keeps the cursor within the buffer");
    __CPROVER_assert(reader.offset >= offset_before, "cbor head only advances the cursor");

    /* On success the head was fully consumed, so at least the initial byte was stepped over. */
    if (ok) __CPROVER_assert(reader.offset > offset_before, "a parsed head advances the cursor");

    return 0;
}
