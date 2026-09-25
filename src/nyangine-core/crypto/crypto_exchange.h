/**
 * @file crypto_exchange.h
 *
 * X25519 key agreement, RFC 7748.
 *
 * Two parties each keep a secret key, swap public keys, and arrive at the same shared secret without it
 * crossing the wire.
 *
 * Overview:
 *   nya_crypto_exchange_key_pair_create / _destroy   a fresh pair from the system's random source, and its wipe
 *   nya_crypto_exchange_key_pair_from_secret         the pair a stored secret key belongs to
 *   nya_crypto_exchange                              the shared secret, refused when it is all zero
 *
 * ```c
 * NYA_CryptoExchangeKeyPair mine = { 0 };
 * NYA_TRY(nya_crypto_exchange_key_pair_create(&mine));
 * defer nya_crypto_exchange_key_pair_destroy(&mine);
 *
 * NYA_CryptoSharedSecret shared = { 0 };
 * if (!nya_crypto_exchange(&mine.secret_key, &their_public_key, &shared)) return;   // a hostile public key
 * defer nya_crypto_wipe(&shared, sizeof(shared));
 *
 * nya_crypto_blake2b_keyed(shared.bytes, sizeof(shared.bytes), transcript, transcript_size, key.bytes, sizeof(key.bytes));
 * ```
 *
 * The shared secret is its own type and not a key: it is a curve point's coordinate, not uniformly
 * random, and is meant to be hashed together with both public keys before anything encrypts under it.
 * */
#pragma once

#include "nyangine-std/base/base_attributes.h"
#include "nyangine-std/base/base_error.h"
#include "nyangine-std/base/base_types.h"

// CONSTANTS

/** RFC 7748 section 5: scalars, u-coordinates and so every key here are 32 bytes. */
#define NYA_CRYPTO_EXCHANGE_KEY_BYTES 32

// TYPES

typedef struct NYA_CryptoExchangeSecretKey NYA_CryptoExchangeSecretKey;
typedef struct NYA_CryptoExchangePublicKey NYA_CryptoExchangePublicKey;
typedef struct NYA_CryptoExchangeKeyPair   NYA_CryptoExchangeKeyPair;
typedef struct NYA_CryptoSharedSecret      NYA_CryptoSharedSecret;

struct NYA_CryptoExchangeSecretKey {
    u8 bytes[NYA_CRYPTO_EXCHANGE_KEY_BYTES];
};

struct NYA_CryptoExchangePublicKey {
    u8 bytes[NYA_CRYPTO_EXCHANGE_KEY_BYTES];
};

struct NYA_CryptoExchangeKeyPair {
    NYA_CryptoExchangeSecretKey secret_key;
    NYA_CryptoExchangePublicKey public_key;
};

struct NYA_CryptoSharedSecret {
    u8 bytes[NYA_CRYPTO_EXCHANGE_KEY_BYTES];
};

// FUNCTIONS

/** A pair from the operating system's random source. Fails only when that source does, leaving it zero. */
NYA_API NYA_Error nya_crypto_exchange_key_pair_create(OUT NYA_CryptoExchangeKeyPair* out_key_pair) __attr_no_discard;

/** Wipes both halves. Safe on a pair that was never created and on one already destroyed. */
NYA_API void nya_crypto_exchange_key_pair_destroy(NYA_CryptoExchangeKeyPair* key_pair);

/** Any 32 bytes are a valid secret key; the curve clamps them. */
NYA_API void nya_crypto_exchange_key_pair_from_secret(const NYA_CryptoExchangeSecretKey* secret_key, OUT NYA_CryptoExchangeKeyPair* out_key_pair);

/**
 * X25519 of our secret key and their public key.
 *
 * False when the result is all zero, which is what a public key of low order produces: a peer that sends
 * one forces a shared secret everyone knows. RFC 7748 section 6.1 allows the check and it costs one pass
 * over 32 bytes, so it is not optional here. `out_shared` is zero then.
 * */
NYA_API b8 nya_crypto_exchange(
    const NYA_CryptoExchangeSecretKey* secret_key,
    const NYA_CryptoExchangePublicKey* public_key,
    OUT NYA_CryptoSharedSecret*        out_shared
) __attr_no_discard;
