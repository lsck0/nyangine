#include "nyangine/nyangine.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API DECLARATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

const u64 FNV_OFFSET_BASIS = 14695981039346656037ULL;
const u64 FNV_PRIME        = 1099511628211ULL;

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PUBLIC API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

__attr_no_sanitize("unsigned-integer-overflow") u64 nya_hash_fnv1a(const void* data, u64 size) __attr_overloaded {
    nya_assert(data != nullptr);

    const u8* bytes = (const u8*)data;
    u64       hash  = FNV_OFFSET_BASIS;

    for (u64 i = 0; i < size; ++i) {
        hash ^= bytes[i];
        hash *= FNV_PRIME;
    }

    return hash;
}

__attr_no_sanitize("unsigned-integer-overflow") u64 nya_hash_fnv1a(NYA_ConstCString string) __attr_overloaded {
    nya_assert(string != nullptr);

    u64 hash = FNV_OFFSET_BASIS;

    for (u64 i = 0; string[i] != '\0'; ++i) {
        hash ^= (u8)string[i];
        hash *= FNV_PRIME;
    }

    return hash;
}

__attr_no_sanitize("unsigned-integer-overflow") u64 nya_hash_fnv1a(NYA_String string) __attr_overloaded {
    u64 hash = FNV_OFFSET_BASIS;

    for (u64 i = 0; i < string.length; ++i) {
        hash ^= string.items[i];
        hash *= FNV_PRIME;
    }

    return hash;
}

/*
 * SipHash-2-4, the reference construction. Two compression rounds per 8 byte block and four
 * finalization rounds, which is where the name comes from.
 *
 * Wrapping addition is the algorithm, not an accident, so it opts out of the unsigned overflow
 * check the same way the other hashes here do.
 */

#define _NYA_SIPROUND(a, b, c, d)                                                                                                                    \
    do {                                                                                                                                             \
        (a) += (b);                                                                                                                                  \
        (b)  = nya_rotate_left_u64((b), 13);                                                                                                         \
        (b) ^= (a);                                                                                                                                  \
        (a)  = nya_rotate_left_u64((a), 32);                                                                                                         \
        (c) += (d);                                                                                                                                  \
        (d)  = nya_rotate_left_u64((d), 16);                                                                                                         \
        (d) ^= (c);                                                                                                                                  \
        (a) += (d);                                                                                                                                  \
        (d)  = nya_rotate_left_u64((d), 21);                                                                                                         \
        (d) ^= (a);                                                                                                                                  \
        (c) += (b);                                                                                                                                  \
        (b)  = nya_rotate_left_u64((b), 17);                                                                                                         \
        (b) ^= (c);                                                                                                                                  \
        (c)  = nya_rotate_left_u64((c), 32);                                                                                                         \
    } while (0)

__attr_no_sanitize("unsigned-integer-overflow") NYA_INTERNAL u64 nya_rotate_left_u64(u64 value, u32 bits) {
    return (value << bits) | (value >> (64 - bits));
}

__attr_no_sanitize("unsigned-integer-overflow") u64 nya_siphash(const void* data, u64 size, u64 key_low, u64 key_high) {
    nya_assert(data != nullptr || size == 0);

    const u8* bytes = data;

    u64 v0 = 0x736F6D6570736575ULL ^ key_low;
    u64 v1 = 0x646F72616E646F6DULL ^ key_high;
    u64 v2 = 0x6C7967656E657261ULL ^ key_low;
    u64 v3 = 0x7465646279746573ULL ^ key_high;

    u64 whole_blocks = size - (size % 8);

    for (u64 offset = 0; offset < whole_blocks; offset += 8) {
        // Read byte by byte rather than casting to a u64*: the input may be unaligned, and this
        // keeps the result identical on a big endian machine.
        u64 block = 0;
        for (u32 i = 0; i < 8; i++) block |= (u64)bytes[offset + i] << (i * 8);

        v3 ^= block;
        _NYA_SIPROUND(v0, v1, v2, v3);
        _NYA_SIPROUND(v0, v1, v2, v3);
        v0 ^= block;
    }

    // The tail block carries the length in its top byte, which is what stops two inputs differing
    // only in trailing zero bytes from hashing alike.
    u64 tail = (size & 0xFF) << 56;
    for (u64 i = whole_blocks; i < size; i++) tail |= (u64)bytes[i] << ((i - whole_blocks) * 8);

    v3 ^= tail;
    _NYA_SIPROUND(v0, v1, v2, v3);
    _NYA_SIPROUND(v0, v1, v2, v3);
    v0 ^= tail;

    v2 ^= 0xFF;
    _NYA_SIPROUND(v0, v1, v2, v3);
    _NYA_SIPROUND(v0, v1, v2, v3);
    _NYA_SIPROUND(v0, v1, v2, v3);
    _NYA_SIPROUND(v0, v1, v2, v3);

    return v0 ^ v1 ^ v2 ^ v3;
}

__attr_no_sanitize("unsigned-integer-overflow") f32 nya_ihash2(s32 x, s32 y, u32 seed) {
    u32 n = (u32)(x + y * 57) + seed;
    n     = (n << 13) ^ n;
    return 1.0F - (f32)((n * (n * n * 15731u + 789221u) + 1376312589u) & 0x7FFFFFFFu) / 1073741824.0F;
}

__attr_no_sanitize("unsigned-integer-overflow") f32 nya_ihash3(s32 x, s32 y, s32 z, u32 seed) {
    u32 n = (u32)(x + y * 57 + z * 131) + seed;
    n     = (n << 13) ^ n;
    return 1.0F - (f32)((n * (n * n * 15731u + 789221u) + 1376312589u) & 0x7FFFFFFFu) / 1073741824.0F;
}
