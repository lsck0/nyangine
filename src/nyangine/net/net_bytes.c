#include "nyangine/nyangine.h"

#include "nyangine/net/net_bytes.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

void _nya_net_write_u16(NYA_String* out, u16 value) {
    nya_string_push_back(out, (u8)(value & 0xFF));
    nya_string_push_back(out, (u8)((value >> 8) & 0xFF));
}

void _nya_net_write_u32(NYA_String* out, u32 value) {
    for (u32 i = 0; i < 4; i++) nya_string_push_back(out, (u8)((value >> (i * 8)) & 0xFF));
}

void _nya_net_write_u64(NYA_String* out, u64 value) {
    for (u32 i = 0; i < 8; i++) nya_string_push_back(out, (u8)((value >> (i * 8)) & 0xFF));
}

void _nya_net_write_f32(NYA_String* out, f32 value) {
    // Through a memcpy rather than a pointer cast: type punning through a cast is undefined, and at
    // -O2 clang is entitled to assume it does not happen. The copy compiles to a register move.
    u32 bits = 0;
    nya_memcpy(&bits, &value, sizeof(bits));

    _nya_net_write_u32(out, bits);
}

void _nya_net_write_f32x3(NYA_String* out, f32x3 value) {
    _nya_net_write_f32(out, value.x);
    _nya_net_write_f32(out, value.y);
    _nya_net_write_f32(out, value.z);
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

u16 _nya_net_read_u16(_NYA_NetReader* reader) {
    if (!_nya_net_reader_has(reader, 2)) return 0;

    u16 value = (u16)((u16)reader->data[reader->at] | ((u16)reader->data[reader->at + 1] << 8));
    reader->at += 2;

    return value;
}

u32 _nya_net_read_u32(_NYA_NetReader* reader) {
    if (!_nya_net_reader_has(reader, 4)) return 0;

    u32 value = 0;
    for (u32 i = 0; i < 4; i++) value |= (u32)reader->data[reader->at + i] << (i * 8);

    reader->at += 4;

    return value;
}

u64 _nya_net_read_u64(_NYA_NetReader* reader) {
    if (!_nya_net_reader_has(reader, 8)) return 0;

    u64 value = 0;
    for (u32 i = 0; i < 8; i++) value |= (u64)reader->data[reader->at + i] << (i * 8);

    reader->at += 8;

    return value;
}

f32 _nya_net_read_f32(_NYA_NetReader* reader) {
    u32 bits = _nya_net_read_u32(reader);

    f32 value = 0.0F;
    nya_memcpy(&value, &bits, sizeof(value));

    return value;
}

f32x3 _nya_net_read_f32x3(_NYA_NetReader* reader) {
    // Read in order into named locals rather than into a compound literal's fields, because argument
    // evaluation order is unspecified and this has to consume the stream left to right.
    f32 x = _nya_net_read_f32(reader);
    f32 y = _nya_net_read_f32(reader);
    f32 z = _nya_net_read_f32(reader);

    return (f32x3){ x, y, z };
}

u64 _nya_net_elapsed_ms(u64 now, u64 then) {
    // Saturating, never wrapping. See the note at the declaration for what the wrap actually cost.
    return now > then ? now - then : 0;
}
