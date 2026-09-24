#include "nyangine/nyangine.h"

// PRIVATE API DECLARATION

// PUBLIC API IMPLEMENTATION

u64 nya_hash_fnv1a(const void* data, u64 size) __attr_overloaded {
    nya_assert(data != nullptr);

    return nya_hash_fnv1a_continue(NYA_HASH_FNV1A_OFFSET_BASIS, data, size);
}

__attr_no_sanitize("unsigned-integer-overflow") u64 nya_hash_fnv1a_continue(u64 hash, const void* data, u64 size) {
    nya_assert(data != nullptr || size == 0);

    const u8* bytes = (const u8*)data;

    for (u64 i = 0; i < size; ++i) {
        hash ^= bytes[i];
        hash *= NYA_HASH_FNV1A_PRIME;
    }

    return hash;
}

__attr_no_sanitize("unsigned-integer-overflow") u64 nya_hash_fnv1a(NYA_ConstCString string) __attr_overloaded {
    nya_assert(string != nullptr);

    u64 hash = NYA_HASH_FNV1A_OFFSET_BASIS;

    for (u64 i = 0; string[i] != '\0'; ++i) {
        hash ^= (u8)string[i];
        hash *= NYA_HASH_FNV1A_PRIME;
    }

    return hash;
}

__attr_no_sanitize("unsigned-integer-overflow") u64 nya_hash_fnv1a(NYA_String string) __attr_overloaded {
    u64 hash = NYA_HASH_FNV1A_OFFSET_BASIS;

    for (u64 i = 0; i < string.length; ++i) {
        hash ^= string.items[i];
        hash *= NYA_HASH_FNV1A_PRIME;
    }

    return hash;
}

// wyhash's default secret. The first word doubles as the seed mix, as in the reference.
NYA_INTERNAL const u64 _NYA_WYHASH_SEED = 0xca813bf4c7abf0a9ULL;
NYA_INTERNAL const u64 _NYA_WYHASH_P0   = 0x2d358dccaa6c78a5ULL;
NYA_INTERNAL const u64 _NYA_WYHASH_P1   = 0x8bb84b93962eacc9ULL;
NYA_INTERNAL const u64 _NYA_WYHASH_P2   = 0x4b33a62ed433d4a3ULL;
NYA_INTERNAL const u64 _NYA_WYHASH_P3   = 0x4d5a2da51de1aa47ULL;

/**
 * nya_hash_wyhash without the precondition, forced inline so the caches in base_cache.c, compiled after this
 * file, pay no call on their lookup path.
 * */
__attribute__((always_inline)) NYA_INTERNAL inline u64 _nya_hash_wyhash(const void* data, u64 size) __attr_no_discard;

/** The full 128 bit product, folded back into both words. */
NYA_INTERNAL inline void _nya_wyhash_mum(u64* a, u64* b) {
    u128 product = (u128)*a * *b;
    *a           = (u64)product;
    *b           = (u64)(product >> 64);
}

NYA_INTERNAL inline u64 _nya_wyhash_mix(u64 a, u64 b) {
    _nya_wyhash_mum(&a, &b);
    return a ^ b;
}

NYA_INTERNAL inline u64 _nya_wyhash_read8(const u8* bytes) {
    u64 value = 0;
    nya_memcpy(&value, bytes, sizeof(value));
    return value;
}

NYA_INTERNAL inline u64 _nya_wyhash_read4(const u8* bytes) {
    u32 value = 0;
    nya_memcpy(&value, bytes, sizeof(value));
    return value;
}

__attr_no_sanitize("unsigned-integer-overflow") u64 nya_hash_wyhash(const void* data, u64 size) {
    nya_assert(data != nullptr || size == 0);
    return _nya_hash_wyhash(data, size);
}

__attr_no_sanitize("unsigned-integer-overflow") u64 _nya_hash_wyhash(const void* data, u64 size) {

    const u8* bytes = (const u8*)data;
    u64       seed  = _NYA_WYHASH_SEED;
    u64       a     = 0;
    u64       b     = 0;

    if (size <= 16) {
        if (size >= 4) {
            // two overlapping four byte reads from each end cover every length from 4 to 16.
            u64 middle = (size >> 3) << 2;
            a          = (_nya_wyhash_read4(bytes) << 32) | _nya_wyhash_read4(bytes + middle);
            b          = (_nya_wyhash_read4(bytes + size - 4) << 32) | _nya_wyhash_read4(bytes + size - 4 - middle);
        } else if (size > 0) {
            a = ((u64)bytes[0] << 16) | ((u64)bytes[size >> 1] << 8) | bytes[size - 1];
        }
    } else {
        u64 remaining = size;

        if (remaining >= 48) {
            u64 seed1 = seed;
            u64 seed2 = seed;

            do {
                seed       = _nya_wyhash_mix(_nya_wyhash_read8(bytes) ^ _NYA_WYHASH_P1, _nya_wyhash_read8(bytes + 8) ^ seed);
                seed1      = _nya_wyhash_mix(_nya_wyhash_read8(bytes + 16) ^ _NYA_WYHASH_P2, _nya_wyhash_read8(bytes + 24) ^ seed1);
                seed2      = _nya_wyhash_mix(_nya_wyhash_read8(bytes + 32) ^ _NYA_WYHASH_P3, _nya_wyhash_read8(bytes + 40) ^ seed2);
                bytes     += 48;
                remaining -= 48;
            } while (remaining >= 48);

            seed ^= seed1 ^ seed2;
        }

        while (remaining > 16) {
            seed       = _nya_wyhash_mix(_nya_wyhash_read8(bytes) ^ _NYA_WYHASH_P1, _nya_wyhash_read8(bytes + 8) ^ seed);
            bytes     += 16;
            remaining -= 16;
        }

        // the last sixteen bytes, overlapping what the loop already took when the length is not a multiple.
        a = _nya_wyhash_read8(bytes + remaining - 16);
        b = _nya_wyhash_read8(bytes + remaining - 8);
    }

    a ^= _NYA_WYHASH_P1;
    b ^= seed;
    _nya_wyhash_mum(&a, &b);

    return _nya_wyhash_mix(a ^ _NYA_WYHASH_P0 ^ size, b ^ _NYA_WYHASH_P1);
}

/* SipHash-2-4, the reference construction: two compression rounds per 8-byte block and four finalization rounds, where the name comes from. */

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
        // Read byte by byte rather than casting to a u64*: the input may be unaligned, and this keeps the result identical on a big-endian machine.
        u64 block = 0;
        for (u32 i = 0; i < 8; i++) block |= (u64)bytes[offset + i] << (i * 8);

        v3 ^= block;
        _NYA_SIPROUND(v0, v1, v2, v3);
        _NYA_SIPROUND(v0, v1, v2, v3);
        v0 ^= block;
    }

    // The tail block carries the length in its top byte, which stops two inputs differing only in trailing zero bytes from hashing alike.
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
    return 1.0F - (f32)((n * (n * n * 15731U + 789221U) + 1376312589U) & 0x7FFFFFFFU) / 1073741824.0F;
}

__attr_no_sanitize("unsigned-integer-overflow") f32 nya_ihash3(s32 x, s32 y, s32 z, u32 seed) {
    u32 n = (u32)(x + y * 57 + z * 131) + seed;
    n     = (n << 13) ^ n;
    return 1.0F - (f32)((n * (n * n * 15731U + 789221U) + 1376312589U) & 0x7FFFFFFFU) / 1073741824.0F;
}
