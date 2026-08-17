/**
 * @file net_bytes.h
 *
 * Little-endian readers and writers, and a bounded reader over bytes a peer chose.
 *
 * Shared by every encoder in net/ rather than living in whichever one needed them first. They started
 * in net_snapshot.c, and net_command.c using them from there worked only because the unity build
 * happened to include the two in the right order — a dependency nothing stated and a reordering would
 * have broken.
 *
 * ## Why explicit byte-at-a-time encoding
 *
 * So a big endian host produces the same bytes as a little endian one. A memcpy of a u32 is shorter
 * and produces a stream only machines of the same endianness can read, which is a bug that never
 * appears until somebody plays on hardware nobody tested.
 *
 * ## Why the reader is poisoned rather than returning errors
 *
 * Every field of every entity is a read that could run off the end of an untrusted payload. Threading
 * a result through each one makes a decoder unreadable, and unreadable is how a bounds check gets
 * left out. Instead a read past the end sets `failed` and returns zero, and the decoder checks once —
 * so a truncated payload produces zeroes and one error rather than a fault.
 * */
#pragma once

#include "nyangine/base/base_attributes.h"
#include "nyangine/base/base_string.h"
#include "nyangine/base/base_types.h"
#include "nyangine/math/math_vector.h"

typedef struct _NYA_NetReader _NYA_NetReader;

/** A cursor over a payload, with a limit and a poisoned flag. See the note above. */
struct _NYA_NetReader {
    const u8* data;
    u64       size;
    u64       at;

    /** Set by the first read that did not fit. Never cleared; one failure poisons the whole decode. */
    b8 failed;
};

NYA_INTERNAL void _nya_net_write_u16(NYA_String* out, u16 value);
NYA_INTERNAL void _nya_net_write_u32(NYA_String* out, u32 value);
NYA_INTERNAL void _nya_net_write_u64(NYA_String* out, u64 value);
NYA_INTERNAL void _nya_net_write_f32(NYA_String* out, f32 value);
NYA_INTERNAL void _nya_net_write_f32x3(NYA_String* out, f32x3 value);

NYA_INTERNAL u16   _nya_net_read_u16(_NYA_NetReader* reader);
NYA_INTERNAL u32   _nya_net_read_u32(_NYA_NetReader* reader);
NYA_INTERNAL u64   _nya_net_read_u64(_NYA_NetReader* reader);
NYA_INTERNAL f32   _nya_net_read_f32(_NYA_NetReader* reader);
NYA_INTERNAL f32x3 _nya_net_read_f32x3(_NYA_NetReader* reader);

/** Whether `count` more bytes are available, poisoning the reader if not. */
NYA_INTERNAL b8 _nya_net_reader_has(_NYA_NetReader* reader, u64 count);

/**
 * Milliseconds from `then` to `now`, saturating at zero rather than wrapping.
 *
 * Every timer in net/ is a subtraction of two monotonic clock reads, and `then` can legitimately be the
 * *later* of the two: a timestamp is often recorded by a function that reads the clock itself, called after
 * its caller had already sampled `now`. One millisecond of skew is enough.
 *
 * Unsigned, so that wraps to something near U64_MAX — which every one of these comparisons then reads as
 * "an enormous amount of time has passed". That is not a rounding error, it is the opposite answer: a
 * freshly added peer was immediately declared timed out and removed, so a client added its server and
 * dropped it in the same call and the handshake could never complete. It fired only when the two clock
 * reads straddled a millisecond, which made it a race that in-process tests mostly won and two separate
 * processes did not.
 * */
NYA_INTERNAL u64 _nya_net_elapsed_ms(u64 now, u64 then) __attr_no_discard;
