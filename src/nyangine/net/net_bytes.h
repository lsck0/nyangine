/**
 * @file net_bytes.h
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

NYA_INTERNAL void _nya_net_write_u32(NYA_String* out, u32 value);
NYA_INTERNAL void _nya_net_write_f32(NYA_String* out, f32 value);

/** LEB128: seven bits a byte, low first, so small numbers cost one byte. */
NYA_INTERNAL void _nya_net_write_varint(NYA_String* out, u64 value);

/** A signed number folded so small magnitudes of either sign stay small: 0, -1, 1, -2 become 0, 1, 2, 3. */
NYA_INTERNAL void _nya_net_write_signed(NYA_String* out, s64 value);

NYA_INTERNAL u32   _nya_net_read_u32(_NYA_NetReader* reader);
NYA_INTERNAL f32   _nya_net_read_f32(_NYA_NetReader* reader);
NYA_INTERNAL u8    _nya_net_read_u8(_NYA_NetReader* reader);

/** At most ten bytes; a longer or unterminated one poisons the reader. */
NYA_INTERNAL u64 _nya_net_read_varint(_NYA_NetReader* reader);
NYA_INTERNAL s64 _nya_net_read_signed(_NYA_NetReader* reader);

/** Whether `count` more bytes are available, poisoning the reader if not. */
NYA_INTERNAL b8 _nya_net_reader_has(_NYA_NetReader* reader, u64 count);

/**
 * Milliseconds from `then` to `now`, saturating at zero rather than wrapping.
 * */
NYA_INTERNAL u64 _nya_net_elapsed_ms(u64 now, u64 then) __attr_no_discard;
