/**
 * @file crypto_secret.h
 *
 * Secret keys, constant time comparison, and wiping.
 *
 * What every other header here builds on: a 32 byte secret key, a comparison that takes the same time
 * wherever two values differ, and a wipe the compiler may not remove.
 *
 * Overview:
 *   nya_crypto_key_create / _destroy   a fresh key from the system's random source, and its wipe
 *   nya_crypto_equals                  compare two byte strings in constant time
 *   nya_crypto_wipe                    zero memory that held a secret
 *
 * ```c
 * NYA_CryptoKey32 key = { 0 };
 * NYA_TRY(nya_crypto_key_create(&key));
 * defer nya_crypto_key_destroy(&key);
 *
 * if (!nya_crypto_equals(tag, expected, sizeof(expected))) return false;
 * ```
 *
 * Rejected: memset for the wipe. A store to memory that is never read again is dead as far as the
 * optimizer knows, and it is allowed to delete it; monocypher's wipe writes through a volatile pointer,
 * which it may not.
 * */
#pragma once

#include "nyangine-std/base/base_attributes.h"
#include "nyangine-std/base/base_error.h"
#include "nyangine-std/base/base_types.h"

// CONSTANTS

/** 256 bits, the key size of every symmetric primitive here and the seed size of both curves. */
#define NYA_CRYPTO_KEY_BYTES 32

// TYPES

typedef struct NYA_CryptoKey32 NYA_CryptoKey32;

/**
 * A symmetric secret: an AEAD key, a MAC key, a signing seed. A struct so that it cannot be handed to a
 * parameter that wants a nonce or a public key, which a bare `u8*` would accept without a word.
 * */
struct NYA_CryptoKey32 {
    u8 bytes[NYA_CRYPTO_KEY_BYTES];
};

// FUNCTIONS

/** A key from the operating system's random source. Fails only when that source does, leaving `out_key` zero. */
NYA_API NYA_Error nya_crypto_key_create(OUT NYA_CryptoKey32* out_key) __attr_no_discard;

/** Wipes the key. Safe on a key that was never created and on one already destroyed. */
NYA_API void nya_crypto_key_destroy(NYA_CryptoKey32* key);

/**
 * Whether `size` bytes at `a` and `b` are equal, in time that depends on `size` alone.
 *
 * A plain memcmp returns at the first difference, and how soon is a measurable fact about the secret. Use
 * this and nothing else to check a tag, a signature, a token or a code.
 * */
NYA_API b8 nya_crypto_equals(const u8* a, const u8* b, u64 size) __attr_no_discard;

/** Zeroes `size` bytes in a way the optimizer may not remove. */
NYA_API void nya_crypto_wipe(void* secret, u64 size);
