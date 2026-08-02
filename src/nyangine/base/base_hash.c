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
