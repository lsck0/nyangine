#include "nyangine/nyangine.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API DECLARATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

NYA_INTERNAL const u64 FNV_OFFSET_BASIS = 14695981039346656037ULL;
NYA_INTERNAL const u64 FNV_PRIME        = 1099511628211ULL;

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
 * wyhash's default secret. The first word doubles as the seed mix, as in the reference.
 */
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

/*
 * SipHash-2-4, the reference construction. Two compression rounds per 8 byte block and four
 * finalization rounds, which is where the name comes from.
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
    return 1.0F - (f32)((n * (n * n * 15731U + 789221U) + 1376312589U) & 0x7FFFFFFFU) / 1073741824.0F;
}

__attr_no_sanitize("unsigned-integer-overflow") f32 nya_ihash3(s32 x, s32 y, s32 z, u32 seed) {
    u32 n = (u32)(x + y * 57 + z * 131) + seed;
    n     = (n << 13) ^ n;
    return 1.0F - (f32)((n * (n * n * 15731U + 789221U) + 1376312589U) & 0x7FFFFFFFU) / 1073741824.0F;
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * SHA-256
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/*
 * FIPS 180-4, section 6.2.
 *
 * Nothing up my sleeve: the eight initial values are the fractional parts of the square roots of the
 * first eight primes, and the sixty four round constants are the fractional parts of the cube roots of
 * the first sixty four, each taken to thirty two bits. Both are printed in the standard, and neither is
 * a choice anyone here made.
 */

NYA_INTERNAL const u32 _NYA_SHA256_INITIAL[8] = {
    0x6A09E667U, 0xBB67AE85U, 0x3C6EF372U, 0xA54FF53AU, 0x510E527FU, 0x9B05688CU, 0x1F83D9ABU, 0x5BE0CD19U,
};

NYA_INTERNAL const u32 _NYA_SHA256_ROUND[64] = {
    0x428A2F98U, 0x71374491U, 0xB5C0FBCFU, 0xE9B5DBA5U, 0x3956C25BU, 0x59F111F1U, 0x923F82A4U, 0xAB1C5ED5U, 0xD807AA98U, 0x12835B01U, 0x243185BEU,
    0x550C7DC3U, 0x72BE5D74U, 0x80DEB1FEU, 0x9BDC06A7U, 0xC19BF174U, 0xE49B69C1U, 0xEFBE4786U, 0x0FC19DC6U, 0x240CA1CCU, 0x2DE92C6FU, 0x4A7484AAU,
    0x5CB0A9DCU, 0x76F988DAU, 0x983E5152U, 0xA831C66DU, 0xB00327C8U, 0xBF597FC7U, 0xC6E00BF3U, 0xD5A79147U, 0x06CA6351U, 0x14292967U, 0x27B70A85U,
    0x2E1B2138U, 0x4D2C6DFCU, 0x53380D13U, 0x650A7354U, 0x766A0ABBU, 0x81C2C92EU, 0x92722C85U, 0xA2BFE8A1U, 0xA81A664BU, 0xC24B8B70U, 0xC76C51A3U,
    0xD192E819U, 0xD6990624U, 0xF40E3585U, 0x106AA070U, 0x19A4C116U, 0x1E376C08U, 0x2748774CU, 0x34B0BCB5U, 0x391C0CB3U, 0x4ED8AA4AU, 0x5B9CCA4FU,
    0x682E6FF3U, 0x748F82EEU, 0x78A5636FU, 0x84C87814U, 0x8CC70208U, 0x90BEFFFAU, 0xA4506CEBU, 0xBEF9A3F7U, 0xC67178F2U,
};

NYA_INTERNAL u32 _nya_rotate_right_u32(u32 value, u32 bits) {
    nya_assert(bits > 0 && bits < 32);

    return (value >> bits) | (value << (32U - bits));
}

/** One 64 byte block into the running state. */
__attr_no_sanitize("unsigned-integer-overflow") NYA_INTERNAL void _nya_sha256_block(u32 state[8], const u8 block[64]) {
    u32 w[64] = { 0 };

    for (u32 i = 0; i < 16; i++) {
        // the index widened before the multiply, not after: a u32 product used as a pointer offset
        // is the shape that silently wraps on a larger input elsewhere.
        u64 at = (u64)i * 4U;

        w[i] = ((u32)block[at] << 24) | ((u32)block[at + 1] << 16) | ((u32)block[at + 2] << 8) | (u32)block[at + 3];
    }

    for (u32 i = 16; i < 64; i++) {
        u32 s0 = _nya_rotate_right_u32(w[i - 15], 7) ^ _nya_rotate_right_u32(w[i - 15], 18) ^ (w[i - 15] >> 3);
        u32 s1 = _nya_rotate_right_u32(w[i - 2], 17) ^ _nya_rotate_right_u32(w[i - 2], 19) ^ (w[i - 2] >> 10);

        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }

    u32 a = state[0];
    u32 b = state[1];
    u32 c = state[2];
    u32 d = state[3];
    u32 e = state[4];
    u32 f = state[5];
    u32 g = state[6];
    u32 h = state[7];

    for (u32 i = 0; i < 64; i++) {
        u32 s1     = _nya_rotate_right_u32(e, 6) ^ _nya_rotate_right_u32(e, 11) ^ _nya_rotate_right_u32(e, 25);
        u32 choice = (e & f) ^ ((~e) & g);
        u32 temp1  = h + s1 + choice + _NYA_SHA256_ROUND[i] + w[i];

        u32 s0       = _nya_rotate_right_u32(a, 2) ^ _nya_rotate_right_u32(a, 13) ^ _nya_rotate_right_u32(a, 22);
        u32 majority = (a & b) ^ (a & c) ^ (b & c);
        u32 temp2    = s0 + majority;

        h = g;
        g = f;
        f = e;
        e = d + temp1;
        d = c;
        c = b;
        b = a;
        a = temp1 + temp2;
    }

    state[0] += a;
    state[1] += b;
    state[2] += c;
    state[3] += d;
    state[4] += e;
    state[5] += f;
    state[6] += g;
    state[7] += h;
}

void nya_sha256(const u8* data, u64 size, OUT u8 out_digest[NYA_SHA256_BYTES]) {
    nya_assert(data != nullptr || size == 0);
    nya_assert(out_digest != nullptr);

    u32 state[8] = { 0 };
    nya_memcpy(state, _NYA_SHA256_INITIAL, sizeof(state));

    u64 whole = size / NYA_SHA256_BLOCK_BYTES;
    for (u64 i = 0; i < whole; i++) _nya_sha256_block(state, data + (i * NYA_SHA256_BLOCK_BYTES));

    /*
     * The tail, its 0x80 terminator and the 64 bit length. Two blocks are always enough: the remainder
     * is under one block, and the standard says to start a second when the terminator and the length do
     * not fit beside it.
     */
    u8  tail[NYA_SHA256_BLOCK_BYTES * 2] = { 0 };
    u64 remainder                        = size - (whole * NYA_SHA256_BLOCK_BYTES);

    nya_assert(remainder < NYA_SHA256_BLOCK_BYTES);
    if (remainder > 0) nya_memcpy(tail, data + (whole * NYA_SHA256_BLOCK_BYTES), remainder);

    tail[remainder] = 0x80U;

    u64 tail_blocks = (remainder + 1 + 8 > NYA_SHA256_BLOCK_BYTES) ? 2 : 1;
    u64 tail_size   = tail_blocks * NYA_SHA256_BLOCK_BYTES;

    // The length in bits, big endian, in the last eight bytes.
    u64 bits = size * 8;
    for (u32 i = 0; i < 8; i++) tail[tail_size - 1 - i] = (u8)((bits >> (8U * i)) & 0xFFU);

    for (u64 i = 0; i < tail_blocks; i++) _nya_sha256_block(state, tail + (i * NYA_SHA256_BLOCK_BYTES));

    for (u32 i = 0; i < 8; i++) {
        u64 at = (u64)i * 4U;

        out_digest[at]     = (u8)((state[i] >> 24) & 0xFFU);
        out_digest[at + 1] = (u8)((state[i] >> 16) & 0xFFU);
        out_digest[at + 2] = (u8)((state[i] >> 8) & 0xFFU);
        out_digest[at + 3] = (u8)(state[i] & 0xFFU);
    }
}

void nya_hmac_sha256(const u8* key, u64 key_size, const u8* data, u64 size, OUT u8 out_tag[NYA_SHA256_BYTES]) {
    nya_assert(key != nullptr || key_size == 0);
    nya_assert(data != nullptr || size == 0);
    nya_assert(out_tag != nullptr);

    /*
     * RFC 2104. The key is reduced to one block: hashed when it is longer, zero padded when it is
     * shorter, which is why any key length is accepted here and no caller has to know the rule.
     */
    u8 block[NYA_SHA256_BLOCK_BYTES] = { 0 };

    if (key_size > NYA_SHA256_BLOCK_BYTES) {
        u8 hashed[NYA_SHA256_BYTES] = { 0 };
        nya_sha256(key, key_size, hashed);
        nya_memcpy(block, hashed, sizeof(hashed));
    } else if (key_size > 0) {
        nya_memcpy(block, key, key_size);
    }

    u8 inner_pad[NYA_SHA256_BLOCK_BYTES] = { 0 };
    u8 outer_pad[NYA_SHA256_BLOCK_BYTES] = { 0 };

    for (u32 i = 0; i < NYA_SHA256_BLOCK_BYTES; i++) {
        inner_pad[i] = (u8)(block[i] ^ 0x36U);
        outer_pad[i] = (u8)(block[i] ^ 0x5CU);
    }

    /*
     * The inner hash runs over a pad followed by the whole message, and nya_sha256 has no streaming
     * form to feed the two to. The buffer is therefore as long as the message, which is why it comes
     * from an arena rather than the stack: a caller signing a megabyte would otherwise clash it.
     */
    u8 inner_digest[NYA_SHA256_BYTES] = { 0 };

    {
        NYA_Arena scratch = nya_arena_create_on_stack(.name = "hmac_sha256");
        defer     nya_arena_destroy_on_stack(&scratch);

        u8* inner = nya_arena_alloc(&scratch, NYA_SHA256_BLOCK_BYTES + size);
        nya_assert(inner != nullptr, "no room to build an hmac over " FMTu64 " bytes", size);

        nya_memcpy(inner, inner_pad, sizeof(inner_pad));
        if (size > 0) nya_memcpy(inner + NYA_SHA256_BLOCK_BYTES, data, size);

        nya_sha256(inner, NYA_SHA256_BLOCK_BYTES + size, inner_digest);
    }

    u8 outer[NYA_SHA256_BLOCK_BYTES + NYA_SHA256_BYTES] = { 0 };

    nya_memcpy(outer, outer_pad, sizeof(outer_pad));
    nya_memcpy(outer + NYA_SHA256_BLOCK_BYTES, inner_digest, sizeof(inner_digest));

    nya_sha256(outer, sizeof(outer), out_tag);
}

b8 nya_hash_equals_constant_time(const u8* a, const u8* b, u64 size) {
    nya_assert(a != nullptr || size == 0);
    nya_assert(b != nullptr || size == 0);

    // Every byte is read and folded into one accumulator, so the loop takes the same time whatever the
    // inputs are. A short circuit here would leak how many leading bytes an attacker had guessed.
    u8 difference = 0;
    for (u64 i = 0; i < size; i++) difference |= (u8)(a[i] ^ b[i]);

    return difference == 0;
}
