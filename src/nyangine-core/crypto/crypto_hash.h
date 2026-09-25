/**
 * @file crypto_hash.h
 *
 * Hashes and MACs: SHA-256, BLAKE2b, and SHA-1 for two old protocols.
 *
 * SHA-256 and HMAC-SHA256 for anything another program has to agree with, BLAKE2b for anything only this
 * engine reads, and SHA-1 for the two protocols that fixed it before it broke.
 *
 * Overview:
 *   nya_crypto_sha256                 one shot digest
 *   nya_crypto_sha256_begin / _end    the same over input that arrives in pieces, fed by _update
 *   nya_crypto_hmac_sha256            RFC 2104 over SHA-256: JWT HS256, challenges
 *   nya_crypto_blake2b                RFC 7693, 1 to 64 bytes of output
 *   nya_crypto_blake2b_keyed          the same as a MAC or a key derivation
 *   nya_crypto_sha1                   TOTP and the WebSocket handshake only
 *   nya_crypto_hmac_sha1              TOTP only
 *
 * ```c
 * NYA_CryptoSha256Digest digest = { 0 };
 * nya_crypto_sha256((const u8*)text, strlen(text), &digest);
 *
 * NYA_CryptoSha256Digest tag = { 0 };
 * nya_crypto_hmac_sha256(secret, secret_size, (const u8*)message, message_size, &tag);
 * ```
 *
 * SHA-256 and SHA-1 are written here, FIPS 180-4 section 6, since monocypher has neither; BLAKE2b is
 * monocypher's. The streaming form exists so an HMAC can hash a pad and a message without first copying
 * them together, which is what the version this replaced did through an arena as long as the message.
 * */
#pragma once

#include "nyangine-std/base/base_attributes.h"
#include "nyangine-std/base/base_types.h"

// CONSTANTS

/** FIPS 180-4 fixes both, and nothing about them is configurable. */
#define NYA_CRYPTO_SHA256_BYTES       32
#define NYA_CRYPTO_SHA256_BLOCK_BYTES 64

#define NYA_CRYPTO_SHA1_BYTES       20
#define NYA_CRYPTO_SHA1_BLOCK_BYTES 64

/** RFC 7693: a BLAKE2b digest is 1 to 64 bytes and its key 0 to 64. */
#define NYA_CRYPTO_BLAKE2B_BYTES_MAX     64
#define NYA_CRYPTO_BLAKE2B_KEY_BYTES_MAX 64

// TYPES

typedef struct NYA_CryptoSha256Digest NYA_CryptoSha256Digest;
typedef struct NYA_CryptoSha1Digest   NYA_CryptoSha1Digest;
typedef struct NYA_CryptoSha256       NYA_CryptoSha256;

/** A SHA-256 digest, and an HMAC-SHA256 tag, which has the same shape. */
struct NYA_CryptoSha256Digest {
    u8 bytes[NYA_CRYPTO_SHA256_BYTES];
};

/** A SHA-1 digest, and an HMAC-SHA1 tag. */
struct NYA_CryptoSha1Digest {
    u8 bytes[NYA_CRYPTO_SHA1_BYTES];
};

/**
 * A SHA-256 in progress. Plain data: copying it forks the hash, which is how an HMAC could reuse a keyed
 * prefix. Wiped by _end, since under an HMAC it holds the key.
 * */
struct NYA_CryptoSha256 {
    u32 state[8];
    u8  block[NYA_CRYPTO_SHA256_BLOCK_BYTES];
    u64 total_bytes;
    u32 block_used;
};

// SHA-256

NYA_API void nya_crypto_sha256(const u8* data, u64 size, OUT NYA_CryptoSha256Digest* out_digest);

NYA_API void nya_crypto_sha256_begin(OUT NYA_CryptoSha256* out_sha256);
NYA_API void nya_crypto_sha256_update(NYA_CryptoSha256* sha256, const u8* data, u64 size);

/** Writes the digest and wipes `sha256`, which must be begun again before it is fed. */
NYA_API void nya_crypto_sha256_end(NYA_CryptoSha256* sha256, OUT NYA_CryptoSha256Digest* out_digest);

/**
 * HMAC-SHA256, RFC 2104. A key longer than a block is hashed first, as the RFC requires, so any key
 * length is accepted and no caller has to know that rule.
 * */
NYA_API void nya_crypto_hmac_sha256(const u8* key, u64 key_size, const u8* data, u64 size, OUT NYA_CryptoSha256Digest* out_tag);

// BLAKE2B

/** `hash_size` from 1 to NYA_CRYPTO_BLAKE2B_BYTES_MAX; anything else asserts. */
NYA_API void nya_crypto_blake2b(const u8* data, u64 size, OUT u8* out_hash, u64 hash_size);

/**
 * BLAKE2b under a key of 1 to NYA_CRYPTO_BLAKE2B_KEY_BYTES_MAX bytes. A MAC by itself, and a key derivation
 * when the key is a shared secret and the data says what the output is for.
 * */
NYA_API void nya_crypto_blake2b_keyed(const u8* key, u64 key_size, const u8* data, u64 size, OUT u8* out_hash, u64 hash_size);

// SHA-1

/**
 * SHA-1. For RFC 6455's Sec-WebSocket-Accept and RFC 6238's TOTP, and for nothing else.
 *
 * Collisions are practical (SHAttered, 2017), so SHA-1 must never sign, fingerprint or deduplicate
 * anything. Both callers survive that only because neither needs collision resistance: the handshake is
 * not a security property at all, and an HMAC's strength rests on the compression function's PRF
 * behaviour. A new caller wants SHA-256 or BLAKE2b.
 * */
NYA_API void nya_crypto_sha1(const u8* data, u64 size, OUT NYA_CryptoSha1Digest* out_digest);

/** HMAC-SHA1, RFC 2104, which is what RFC 6238 TOTP is defined over. Nothing else may use it. */
NYA_API void nya_crypto_hmac_sha1(const u8* key, u64 key_size, const u8* data, u64 size, OUT NYA_CryptoSha1Digest* out_tag);
