#include "nyangine-std/base/base_assert.h"
#include "nyangine-std/base/base_memory.h"
#include "nyangine-core/crypto/crypto_hash.h"
#include "nyangine-core/crypto/crypto_secret.h"
#include "monocypher.h"

// PRIVATE API DECLARATION

/** SHA-1 and SHA-256 share their block, their padding and their length encoding, so this is both. */
#define _NYA_CRYPTO_MD_BLOCK_BYTES 64

/** The 64 bit message length in bits, big endian, at the end of the last block. */
#define _NYA_CRYPTO_MD_LENGTH_BYTES 8

/** RFC 2104's pads. Nothing up my sleeve: the RFC picked them for having many bits differ between the two. */
#define _NYA_CRYPTO_HMAC_INNER_PAD 0x36U
#define _NYA_CRYPTO_HMAC_OUTER_PAD 0x5CU

static_assert(NYA_CRYPTO_SHA256_BLOCK_BYTES == _NYA_CRYPTO_MD_BLOCK_BYTES, "the shared padding assumes one block size");
static_assert(NYA_CRYPTO_SHA1_BLOCK_BYTES == _NYA_CRYPTO_MD_BLOCK_BYTES, "the shared padding assumes one block size");

/** One block into a running state, whose width only the function knows. */
typedef void (*_NYA_CryptoCompressFn)(u32* state, const u8 block[_NYA_CRYPTO_MD_BLOCK_BYTES]);

/** A SHA-1 in progress. The streaming form stays private: nothing but the HMAC below may feed SHA-1 in pieces. */
typedef struct {
    u32 state[5];
    u8  block[_NYA_CRYPTO_MD_BLOCK_BYTES];
    u64 total_bytes;
    u32 block_used;
} _NYA_CryptoSha1;

/** What SHA-1 and SHA-256 both keep beside their state, by pointer, so one buffer routine serves both. */
typedef struct {
    u32*                  state;
    u8*                   block;
    u64*                  total_bytes;
    u32*                  block_used;
    _NYA_CryptoCompressFn compress;
} _NYA_CryptoMd;

NYA_INTERNAL void _nya_crypto_md_update(_NYA_CryptoMd md, const u8* data, u64 size);

/** Appends the 0x80 terminator and the length, and compresses what remains. */
NYA_INTERNAL void _nya_crypto_md_finish(_NYA_CryptoMd md);

/** Writes `words` state words big endian, which is how both standards print a digest. */
NYA_INTERNAL void _nya_crypto_md_store(const u32* state, u32 words, OUT u8* out_digest);

NYA_INTERNAL void _nya_crypto_sha256_compress(u32* state, const u8 block[_NYA_CRYPTO_MD_BLOCK_BYTES]);
NYA_INTERNAL void _nya_crypto_sha1_compress(u32* state, const u8 block[_NYA_CRYPTO_MD_BLOCK_BYTES]);

NYA_INTERNAL _NYA_CryptoMd _nya_crypto_sha256_md(NYA_CryptoSha256* sha256) __attr_no_discard;
NYA_INTERNAL _NYA_CryptoMd _nya_crypto_sha1_md(_NYA_CryptoSha1* sha1) __attr_no_discard;

NYA_INTERNAL void _nya_crypto_sha1_begin(OUT _NYA_CryptoSha1* out_sha1);
NYA_INTERNAL void _nya_crypto_sha1_end(_NYA_CryptoSha1* sha1, OUT NYA_CryptoSha1Digest* out_digest);

/** RFC 2104's two pads for `key`: the key becomes one block, hashed when longer and zero padded when shorter. */
NYA_INTERNAL void _nya_crypto_hmac_pads(
    const u8* key,
    u64       key_size,
    void (*hash)(const u8*, u64, u8*),
    u64    digest_bytes,
    OUT u8 out_inner[_NYA_CRYPTO_MD_BLOCK_BYTES],
    OUT u8 out_outer[_NYA_CRYPTO_MD_BLOCK_BYTES]
);

NYA_INTERNAL void _nya_crypto_sha256_bytes(const u8* data, u64 size, OUT u8* out_digest);
NYA_INTERNAL void _nya_crypto_sha1_bytes(const u8* data, u64 size, OUT u8* out_digest);

NYA_INTERNAL u32 _nya_crypto_rotate_right(u32 value, u32 bits) __attr_no_discard;
NYA_INTERNAL u32 _nya_crypto_load_u32_be(const u8* bytes) __attr_no_discard;

// CONSTANTS

// FIPS 180-4 5.3.3/4.2.2: initial values from sqrt fractions of the first 8 primes, rounds from cube roots of 64.
NYA_INTERNAL const u32 _NYA_CRYPTO_SHA256_INITIAL[8] = {
    0x6A09E667U, 0xBB67AE85U, 0x3C6EF372U, 0xA54FF53AU, 0x510E527FU, 0x9B05688CU, 0x1F83D9ABU, 0x5BE0CD19U,
};

NYA_INTERNAL const u32 _NYA_CRYPTO_SHA256_ROUND[64] = {
    0x428A2F98U, 0x71374491U, 0xB5C0FBCFU, 0xE9B5DBA5U, 0x3956C25BU, 0x59F111F1U, 0x923F82A4U, 0xAB1C5ED5U, 0xD807AA98U, 0x12835B01U, 0x243185BEU,
    0x550C7DC3U, 0x72BE5D74U, 0x80DEB1FEU, 0x9BDC06A7U, 0xC19BF174U, 0xE49B69C1U, 0xEFBE4786U, 0x0FC19DC6U, 0x240CA1CCU, 0x2DE92C6FU, 0x4A7484AAU,
    0x5CB0A9DCU, 0x76F988DAU, 0x983E5152U, 0xA831C66DU, 0xB00327C8U, 0xBF597FC7U, 0xC6E00BF3U, 0xD5A79147U, 0x06CA6351U, 0x14292967U, 0x27B70A85U,
    0x2E1B2138U, 0x4D2C6DFCU, 0x53380D13U, 0x650A7354U, 0x766A0ABBU, 0x81C2C92EU, 0x92722C85U, 0xA2BFE8A1U, 0xA81A664BU, 0xC24B8B70U, 0xC76C51A3U,
    0xD192E819U, 0xD6990624U, 0xF40E3585U, 0x106AA070U, 0x19A4C116U, 0x1E376C08U, 0x2748774CU, 0x34B0BCB5U, 0x391C0CB3U, 0x4ED8AA4AU, 0x5B9CCA4FU,
    0x682E6FF3U, 0x748F82EEU, 0x78A5636FU, 0x84C87814U, 0x8CC70208U, 0x90BEFFFAU, 0xA4506CEBU, 0xBEF9A3F7U, 0xC67178F2U,
};

// FIPS 180-4 5.3.1/4.2.1: initial values count in nibbles, round constants are sqrt(2,3,5,10) times 2^30.
NYA_INTERNAL const u32 _NYA_CRYPTO_SHA1_INITIAL[5] = { 0x67452301U, 0xEFCDAB89U, 0x98BADCFEU, 0x10325476U, 0xC3D2E1F0U };
NYA_INTERNAL const u32 _NYA_CRYPTO_SHA1_ROUND[4]   = { 0x5A827999U, 0x6ED9EBA1U, 0x8F1BBCDCU, 0xCA62C1D6U };

// PUBLIC API IMPLEMENTATION

// SHA-256

void nya_crypto_sha256(const u8* data, u64 size, OUT NYA_CryptoSha256Digest* out_digest) {
    nya_assert(data != nullptr || size == 0);
    nya_assert(out_digest != nullptr);

    NYA_CryptoSha256 sha256 = { 0 };
    nya_crypto_sha256_begin(&sha256);
    nya_crypto_sha256_update(&sha256, data, size);
    nya_crypto_sha256_end(&sha256, out_digest);
}

void nya_crypto_sha256_begin(OUT NYA_CryptoSha256* out_sha256) {
    nya_assert(out_sha256 != nullptr);

    *out_sha256 = (NYA_CryptoSha256){ 0 };
    nya_memcpy(out_sha256->state, _NYA_CRYPTO_SHA256_INITIAL, sizeof(out_sha256->state));
}

void nya_crypto_sha256_update(NYA_CryptoSha256* sha256, const u8* data, u64 size) {
    nya_assert(sha256 != nullptr);
    nya_assert(data != nullptr || size == 0);
    nya_assert(sha256->block_used < _NYA_CRYPTO_MD_BLOCK_BYTES, "a SHA-256 whose buffered count is past its block");

    _nya_crypto_md_update(_nya_crypto_sha256_md(sha256), data, size);
}

void nya_crypto_sha256_end(NYA_CryptoSha256* sha256, OUT NYA_CryptoSha256Digest* out_digest) {
    nya_assert(sha256 != nullptr);
    nya_assert(out_digest != nullptr);
    nya_assert(sha256->block_used < _NYA_CRYPTO_MD_BLOCK_BYTES);

    _nya_crypto_md_finish(_nya_crypto_sha256_md(sha256));
    _nya_crypto_md_store(sha256->state, 8, out_digest->bytes);

    nya_crypto_wipe(sha256, sizeof(*sha256));
}

void nya_crypto_hmac_sha256(const u8* key, u64 key_size, const u8* data, u64 size, OUT NYA_CryptoSha256Digest* out_tag) {
    nya_assert(key != nullptr || key_size == 0);
    nya_assert(data != nullptr || size == 0);
    nya_assert(out_tag != nullptr);

    u8 inner_pad[_NYA_CRYPTO_MD_BLOCK_BYTES] = { 0 };
    u8 outer_pad[_NYA_CRYPTO_MD_BLOCK_BYTES] = { 0 };
    _nya_crypto_hmac_pads(key, key_size, _nya_crypto_sha256_bytes, NYA_CRYPTO_SHA256_BYTES, inner_pad, outer_pad);

    NYA_CryptoSha256Digest inner  = { 0 };
    NYA_CryptoSha256       sha256 = { 0 };

    nya_crypto_sha256_begin(&sha256);
    nya_crypto_sha256_update(&sha256, inner_pad, sizeof(inner_pad));
    nya_crypto_sha256_update(&sha256, data, size);
    nya_crypto_sha256_end(&sha256, &inner);

    nya_crypto_sha256_begin(&sha256);
    nya_crypto_sha256_update(&sha256, outer_pad, sizeof(outer_pad));
    nya_crypto_sha256_update(&sha256, inner.bytes, sizeof(inner.bytes));
    nya_crypto_sha256_end(&sha256, out_tag);

    nya_crypto_wipe(inner_pad, sizeof(inner_pad));
    nya_crypto_wipe(outer_pad, sizeof(outer_pad));
    nya_crypto_wipe(&inner, sizeof(inner));
}

// BLAKE2B

void nya_crypto_blake2b(const u8* data, u64 size, OUT u8* out_hash, u64 hash_size) {
    nya_assert(data != nullptr || size == 0);
    nya_assert(out_hash != nullptr);
    nya_assert(hash_size >= 1 && hash_size <= NYA_CRYPTO_BLAKE2B_BYTES_MAX, "a BLAKE2b digest is 1 to 64 bytes, not " FMTu64, hash_size);

    crypto_blake2b(out_hash, hash_size, data, size);
}

void nya_crypto_blake2b_keyed(const u8* key, u64 key_size, const u8* data, u64 size, OUT u8* out_hash, u64 hash_size) {
    nya_assert(key != nullptr);
    nya_assert(key_size >= 1 && key_size <= NYA_CRYPTO_BLAKE2B_KEY_BYTES_MAX, "a BLAKE2b key is 1 to 64 bytes, not " FMTu64, key_size);
    nya_assert(data != nullptr || size == 0);
    nya_assert(out_hash != nullptr);
    nya_assert(hash_size >= 1 && hash_size <= NYA_CRYPTO_BLAKE2B_BYTES_MAX, "a BLAKE2b digest is 1 to 64 bytes, not " FMTu64, hash_size);

    crypto_blake2b_keyed(out_hash, hash_size, key, key_size, data, size);
}

// SHA-1

void nya_crypto_sha1(const u8* data, u64 size, OUT NYA_CryptoSha1Digest* out_digest) {
    nya_assert(data != nullptr || size == 0);
    nya_assert(out_digest != nullptr);

    _NYA_CryptoSha1 sha1 = { 0 };
    _nya_crypto_sha1_begin(&sha1);
    _nya_crypto_md_update(_nya_crypto_sha1_md(&sha1), data, size);
    _nya_crypto_sha1_end(&sha1, out_digest);
}

void nya_crypto_hmac_sha1(const u8* key, u64 key_size, const u8* data, u64 size, OUT NYA_CryptoSha1Digest* out_tag) {
    nya_assert(key != nullptr || key_size == 0);
    nya_assert(data != nullptr || size == 0);
    nya_assert(out_tag != nullptr);

    u8 inner_pad[_NYA_CRYPTO_MD_BLOCK_BYTES] = { 0 };
    u8 outer_pad[_NYA_CRYPTO_MD_BLOCK_BYTES] = { 0 };
    _nya_crypto_hmac_pads(key, key_size, _nya_crypto_sha1_bytes, NYA_CRYPTO_SHA1_BYTES, inner_pad, outer_pad);

    NYA_CryptoSha1Digest inner = { 0 };
    _NYA_CryptoSha1      sha1  = { 0 };

    _nya_crypto_sha1_begin(&sha1);
    _nya_crypto_md_update(_nya_crypto_sha1_md(&sha1), inner_pad, sizeof(inner_pad));
    _nya_crypto_md_update(_nya_crypto_sha1_md(&sha1), data, size);
    _nya_crypto_sha1_end(&sha1, &inner);

    _nya_crypto_sha1_begin(&sha1);
    _nya_crypto_md_update(_nya_crypto_sha1_md(&sha1), outer_pad, sizeof(outer_pad));
    _nya_crypto_md_update(_nya_crypto_sha1_md(&sha1), inner.bytes, sizeof(inner.bytes));
    _nya_crypto_sha1_end(&sha1, out_tag);

    nya_crypto_wipe(inner_pad, sizeof(inner_pad));
    nya_crypto_wipe(outer_pad, sizeof(outer_pad));
    nya_crypto_wipe(&inner, sizeof(inner));
}

// PRIVATE API IMPLEMENTATION

// THE SHARED MERKLE-DAMGARD FRAME

__attr_no_sanitize("unsigned-integer-overflow") void _nya_crypto_md_update(_NYA_CryptoMd md, const u8* data, u64 size) {
    nya_assert(*md.block_used < _NYA_CRYPTO_MD_BLOCK_BYTES);

    // the length field is 64 bits of bits, so a message past 2^61 bytes is undefined; asserted, not wrapped.
    nya_assert(size <= (U64_MAX / 8) - *md.total_bytes, "a message past 2^61 bytes has no SHA length encoding");

    *md.total_bytes += size;

    u64 offset = 0;

    // top up a partial block first, so the loop below only ever compresses straight from `data`.
    if (*md.block_used > 0) {
        u64 room = _NYA_CRYPTO_MD_BLOCK_BYTES - *md.block_used;
        u64 take = size < room ? size : room;

        nya_memcpy(md.block + *md.block_used, data, take);
        *md.block_used += (u32)take;
        offset         += take;

        if (*md.block_used < _NYA_CRYPTO_MD_BLOCK_BYTES) return;

        md.compress(md.state, md.block);
        *md.block_used = 0;
    }

    while (size - offset >= _NYA_CRYPTO_MD_BLOCK_BYTES) {
        md.compress(md.state, data + offset);
        offset += _NYA_CRYPTO_MD_BLOCK_BYTES;
    }

    u64 rest = size - offset;
    nya_assert(rest < _NYA_CRYPTO_MD_BLOCK_BYTES);

    if (rest > 0) nya_memcpy(md.block, data + offset, rest);
    *md.block_used = (u32)rest;
}

void _nya_crypto_md_finish(_NYA_CryptoMd md) {
    nya_assert(*md.block_used < _NYA_CRYPTO_MD_BLOCK_BYTES);

    u64 bits = *md.total_bytes * 8;
    u32 used = *md.block_used;

    md.block[used++] = 0x80U;

    // no room for the length beside the terminator means a padding-only block (the 56 byte message case).
    if (used > _NYA_CRYPTO_MD_BLOCK_BYTES - _NYA_CRYPTO_MD_LENGTH_BYTES) {
        nya_memset(md.block + used, 0, _NYA_CRYPTO_MD_BLOCK_BYTES - used);
        md.compress(md.state, md.block);
        used = 0;
    }

    nya_memset(md.block + used, 0, _NYA_CRYPTO_MD_BLOCK_BYTES - _NYA_CRYPTO_MD_LENGTH_BYTES - used);

    for (u32 i = 0; i < _NYA_CRYPTO_MD_LENGTH_BYTES; i++) { md.block[_NYA_CRYPTO_MD_BLOCK_BYTES - 1 - i] = (u8)((bits >> (8U * i)) & 0xFFU); }

    md.compress(md.state, md.block);
    *md.block_used = 0;
}

void _nya_crypto_md_store(const u32* state, u32 words, OUT u8* out_digest) {
    for (u32 i = 0; i < words; i++) {
        // index widened before the multiply, else a u32 product used as an offset could silently wrap.
        u64 at = (u64)i * 4U;

        out_digest[at]     = (u8)((state[i] >> 24) & 0xFFU);
        out_digest[at + 1] = (u8)((state[i] >> 16) & 0xFFU);
        out_digest[at + 2] = (u8)((state[i] >> 8) & 0xFFU);
        out_digest[at + 3] = (u8)(state[i] & 0xFFU);
    }
}

u32 _nya_crypto_rotate_right(u32 value, u32 bits) {
    nya_assert(bits > 0 && bits < 32);

    return (value >> bits) | (value << (32U - bits));
}

u32 _nya_crypto_load_u32_be(const u8* bytes) {
    return ((u32)bytes[0] << 24) | ((u32)bytes[1] << 16) | ((u32)bytes[2] << 8) | (u32)bytes[3];
}

// SHA-256

_NYA_CryptoMd _nya_crypto_sha256_md(NYA_CryptoSha256* sha256) {
    return (_NYA_CryptoMd){
        .state       = sha256->state,
        .block       = sha256->block,
        .total_bytes = &sha256->total_bytes,
        .block_used  = &sha256->block_used,
        .compress    = _nya_crypto_sha256_compress,
    };
}

void _nya_crypto_sha256_bytes(const u8* data, u64 size, OUT u8* out_digest) {
    NYA_CryptoSha256Digest digest = { 0 };
    nya_crypto_sha256(data, size, &digest);

    nya_memcpy(out_digest, digest.bytes, sizeof(digest.bytes));
    nya_crypto_wipe(&digest, sizeof(digest));
}

// additions are modular by definition (FIPS 180-4's 32 bit words), so the overflow sanitizer is turned off here.
__attr_no_sanitize("unsigned-integer-overflow") void _nya_crypto_sha256_compress(u32* state, const u8 block[_NYA_CRYPTO_MD_BLOCK_BYTES]) {
    u32 w[64] = { 0 };

    for (u32 i = 0; i < 16; i++) w[i] = _nya_crypto_load_u32_be(block + ((u64)i * 4U));

    for (u32 i = 16; i < 64; i++) {
        u32 s0 = _nya_crypto_rotate_right(w[i - 15], 7) ^ _nya_crypto_rotate_right(w[i - 15], 18) ^ (w[i - 15] >> 3);
        u32 s1 = _nya_crypto_rotate_right(w[i - 2], 17) ^ _nya_crypto_rotate_right(w[i - 2], 19) ^ (w[i - 2] >> 10);

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
        u32 s1     = _nya_crypto_rotate_right(e, 6) ^ _nya_crypto_rotate_right(e, 11) ^ _nya_crypto_rotate_right(e, 25);
        u32 choice = (e & f) ^ ((~e) & g);
        u32 temp1  = h + s1 + choice + _NYA_CRYPTO_SHA256_ROUND[i] + w[i];

        u32 s0       = _nya_crypto_rotate_right(a, 2) ^ _nya_crypto_rotate_right(a, 13) ^ _nya_crypto_rotate_right(a, 22);
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

    // the schedule is the block expanded, and under an HMAC the block is the key.
    nya_crypto_wipe(w, sizeof(w));
}

// SHA-1

_NYA_CryptoMd _nya_crypto_sha1_md(_NYA_CryptoSha1* sha1) {
    return (_NYA_CryptoMd){
        .state       = sha1->state,
        .block       = sha1->block,
        .total_bytes = &sha1->total_bytes,
        .block_used  = &sha1->block_used,
        .compress    = _nya_crypto_sha1_compress,
    };
}

void _nya_crypto_sha1_begin(OUT _NYA_CryptoSha1* out_sha1) {
    *out_sha1 = (_NYA_CryptoSha1){ 0 };
    nya_memcpy(out_sha1->state, _NYA_CRYPTO_SHA1_INITIAL, sizeof(out_sha1->state));
}

void _nya_crypto_sha1_end(_NYA_CryptoSha1* sha1, OUT NYA_CryptoSha1Digest* out_digest) {
    _nya_crypto_md_finish(_nya_crypto_sha1_md(sha1));
    _nya_crypto_md_store(sha1->state, 5, out_digest->bytes);

    nya_crypto_wipe(sha1, sizeof(*sha1));
}

void _nya_crypto_sha1_bytes(const u8* data, u64 size, OUT u8* out_digest) {
    NYA_CryptoSha1Digest digest = { 0 };
    nya_crypto_sha1(data, size, &digest);

    nya_memcpy(out_digest, digest.bytes, sizeof(digest.bytes));
    nya_crypto_wipe(&digest, sizeof(digest));
}

// modular for the reason _nya_crypto_sha256_compress gives.
__attr_no_sanitize("unsigned-integer-overflow") void _nya_crypto_sha1_compress(u32* state, const u8 block[_NYA_CRYPTO_MD_BLOCK_BYTES]) {
    u32 w[80] = { 0 };

    for (u32 i = 0; i < 16; i++) w[i] = _nya_crypto_load_u32_be(block + ((u64)i * 4U));

    // FIPS 180-4 writes this as a left rotation by one, which is a right rotation by thirty one.
    for (u32 i = 16; i < 80; i++) w[i] = _nya_crypto_rotate_right(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 31);

    u32 a = state[0];
    u32 b = state[1];
    u32 c = state[2];
    u32 d = state[3];
    u32 e = state[4];

    for (u32 i = 0; i < 80; i++) {
        u32 mix = 0;

        switch (i / 20) {
            case 0:  mix = (b & c) | ((~b) & d); break;
            case 1:  mix = b ^ c ^ d; break;
            case 2:  mix = (b & c) | (b & d) | (c & d); break;
            case 3:  mix = b ^ c ^ d; break;
            default: nya_unreachable();
        }

        u32 temp = _nya_crypto_rotate_right(a, 27) + mix + e + _NYA_CRYPTO_SHA1_ROUND[i / 20] + w[i];

        e = d;
        d = c;
        c = _nya_crypto_rotate_right(b, 2);
        b = a;
        a = temp;
    }

    state[0] += a;
    state[1] += b;
    state[2] += c;
    state[3] += d;
    state[4] += e;

    nya_crypto_wipe(w, sizeof(w));
}

// HMAC

void _nya_crypto_hmac_pads(
    const u8* key,
    u64       key_size,
    void (*hash)(const u8*, u64, u8*),
    u64    digest_bytes,
    OUT u8 out_inner[_NYA_CRYPTO_MD_BLOCK_BYTES],
    OUT u8 out_outer[_NYA_CRYPTO_MD_BLOCK_BYTES]
) {
    nya_assert(digest_bytes <= _NYA_CRYPTO_MD_BLOCK_BYTES);

    u8 block[_NYA_CRYPTO_MD_BLOCK_BYTES] = { 0 };

    if (key_size > _NYA_CRYPTO_MD_BLOCK_BYTES) {
        hash(key, key_size, block);
    } else if (key_size > 0) {
        nya_memcpy(block, key, key_size);
    }

    for (u32 i = 0; i < _NYA_CRYPTO_MD_BLOCK_BYTES; i++) {
        out_inner[i] = (u8)(block[i] ^ _NYA_CRYPTO_HMAC_INNER_PAD);
        out_outer[i] = (u8)(block[i] ^ _NYA_CRYPTO_HMAC_OUTER_PAD);
    }

    nya_crypto_wipe(block, sizeof(block));
}
