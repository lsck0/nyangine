#include "nyangine/nyangine.h"

#include "nyangine/net/net_bytes.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

void _nya_net_write_u32(NYA_String* out, u32 value) {
    for (u32 i = 0; i < 4; i++) nya_string_push_back(out, (u8)((value >> (i * 8)) & 0xFF));
}

void _nya_net_write_f32(NYA_String* out, f32 value) {
    // Through a memcpy rather than a pointer cast: type punning through a cast is undefined, and at
    // -O2 clang is entitled to assume it does not happen. The copy compiles to a register move.
    u32 bits = 0;
    nya_memcpy(&bits, &value, sizeof(bits));

    _nya_net_write_u32(out, bits);
}

void _nya_net_write_varint(NYA_String* out, u64 value) {
    while (value >= 0x80) {
        nya_string_push_back(out, (u8)((value & 0x7F) | 0x80));
        value >>= 7;
    }

    nya_string_push_back(out, (u8)value);
}

void _nya_net_write_signed(NYA_String* out, s64 value) {
    _nya_net_write_varint(out, ((u64)value << 1) ^ (u64)(value >> 63));
}

b8 _nya_net_reader_has(_NYA_NetReader* reader, u64 count) {
    if (reader->failed) return false;

    // Written as a subtraction rather than `at + count > size`, because the addition can overflow on
    // a size that came off the wire and then compare as fitting.
    if (count > reader->size - reader->at) {
        reader->failed = true;
        return false;
    }

    return true;
}

u8 _nya_net_read_u8(_NYA_NetReader* reader) {
    if (!_nya_net_reader_has(reader, 1)) return 0;

    return reader->data[reader->at++];
}

u64 _nya_net_read_varint(_NYA_NetReader* reader) {
    u64 value = 0;

    for (u32 shift = 0; shift < 70; shift += 7) {
        if (!_nya_net_reader_has(reader, 1)) return 0;

        u8 byte = reader->data[reader->at++];

        // the tenth byte may only carry the one bit a u64 has left.
        if (shift == 63 && byte > 1) break;

        value |= (u64)(byte & 0x7F) << shift;

        if ((byte & 0x80) == 0) return value;
    }

    reader->failed = true;
    return 0;
}

s64 _nya_net_read_signed(_NYA_NetReader* reader) {
    u64 folded = _nya_net_read_varint(reader);

    return (s64)(folded >> 1) ^ -(s64)(folded & 1);
}

u32 _nya_net_read_u32(_NYA_NetReader* reader) {
    if (!_nya_net_reader_has(reader, 4)) return 0;

    u32 value = 0;
    for (u32 i = 0; i < 4; i++) value |= (u32)reader->data[reader->at + i] << (i * 8);

    reader->at += 4;

    return value;
}

f32 _nya_net_read_f32(_NYA_NetReader* reader) {
    u32 bits = _nya_net_read_u32(reader);

    f32 value = 0.0F;
    nya_memcpy(&value, &bits, sizeof(value));

    return value;
}

u64 _nya_net_elapsed_ns(u64 now, u64 then) {
    return now > then ? now - then : 0;
}

u64 _nya_net_elapsed_ms(u64 now, u64 then) {
    // Saturating, never wrapping. See the note at the declaration for what the wrap actually cost.
    return now > then ? now - then : 0;
}
