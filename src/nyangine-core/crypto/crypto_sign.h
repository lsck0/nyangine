/**
 * @file crypto_sign.h
 *
 * Ed25519 signatures, RFC 8032.
 *
 * Sign with a secret key, and let anyone holding the public key check that the message is the one
 * signed.
 *
 * Overview:
 *   nya_crypto_sign_key_pair_create / _destroy   a fresh pair from the system's random source, and its wipe
 *   nya_crypto_sign_key_pair_from_seed           the pair a stored 32 byte seed belongs to
 *   nya_crypto_sign                              a signature over a message
 *   nya_crypto_sign_verify                       whether a signature is the public key's over the message
 *
 * ```c
 * NYA_CryptoSignKeyPair pair = { 0 };
 * NYA_TRY(nya_crypto_sign_key_pair_create(&pair));
 * defer nya_crypto_sign_key_pair_destroy(&pair);
 *
 * NYA_CryptoSignature signature = { 0 };
 * nya_crypto_sign(&pair.secret_key, message, size, &signature);
 *
 * if (!nya_crypto_sign_verify(&pair.public_key, message, size, &signature)) return false;
 * ```
 *
 * RFC 8032's Ed25519 over SHA-512, from monocypher's optional file, and not the core file's EdDSA over
 * BLAKE2b. The latter is a fine signature that nothing else can check: passkeys, OpenPGP and SSH all
 * speak the RFC's, and a signature only this engine accepts is half of a feature.
 * */
#pragma once

#include "nyangine-std/base/base_attributes.h"
#include "nyangine-std/base/base_error.h"
#include "nyangine-std/base/base_types.h"
#include "nyangine-core/crypto/crypto_secret.h"

// CONSTANTS

/** The seed and the public key half, which is how monocypher lays an expanded secret key out. */
#define NYA_CRYPTO_SIGN_SECRET_KEY_BYTES 64
#define NYA_CRYPTO_SIGN_PUBLIC_KEY_BYTES 32
#define NYA_CRYPTO_SIGNATURE_BYTES       64

// TYPES

typedef struct NYA_CryptoSignSecretKey NYA_CryptoSignSecretKey;
typedef struct NYA_CryptoSignPublicKey NYA_CryptoSignPublicKey;
typedef struct NYA_CryptoSignKeyPair   NYA_CryptoSignKeyPair;
typedef struct NYA_CryptoSignature     NYA_CryptoSignature;

/** The 32 byte seed followed by the public key. Store the seed; the rest is derived from it. */
struct NYA_CryptoSignSecretKey {
    u8 bytes[NYA_CRYPTO_SIGN_SECRET_KEY_BYTES];
};

struct NYA_CryptoSignPublicKey {
    u8 bytes[NYA_CRYPTO_SIGN_PUBLIC_KEY_BYTES];
};

struct NYA_CryptoSignKeyPair {
    NYA_CryptoSignSecretKey secret_key;
    NYA_CryptoSignPublicKey public_key;
};

struct NYA_CryptoSignature {
    u8 bytes[NYA_CRYPTO_SIGNATURE_BYTES];
};

// FUNCTIONS

/** A pair from the operating system's random source. Fails only when that source does, leaving it zero. */
NYA_API NYA_Error nya_crypto_sign_key_pair_create(OUT NYA_CryptoSignKeyPair* out_key_pair) __attr_no_discard;

/** Wipes both halves. Safe on a pair that was never created and on one already destroyed. */
NYA_API void nya_crypto_sign_key_pair_destroy(NYA_CryptoSignKeyPair* key_pair);

/** The pair RFC 8032 derives from a seed, which is the "secret key" its test vectors print. */
NYA_API void nya_crypto_sign_key_pair_from_seed(const NYA_CryptoKey32* seed, OUT NYA_CryptoSignKeyPair* out_key_pair);

/** Deterministic: the same key and message always give the same signature, so no nonce can be reused. */
NYA_API void nya_crypto_sign(const NYA_CryptoSignSecretKey* secret_key, const u8* message, u64 size, OUT NYA_CryptoSignature* out_signature);

/**
 * Whether `signature` is `public_key`'s over exactly `message`. False for any altered byte in any of the
 * three; for a signature whose scalar is not reduced, which would otherwise let one signature be written
 * two ways; and for a public key of small order, under which an all zero signature verifies. RFC 8032
 * allows the last; it costs one X25519 per call and closes a forgery, so it is refused here.
 * */
NYA_API b8 nya_crypto_sign_verify(const NYA_CryptoSignPublicKey* public_key, const u8* message, u64 size, const NYA_CryptoSignature* signature)
    __attr_no_discard;
