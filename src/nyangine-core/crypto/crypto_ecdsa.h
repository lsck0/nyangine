/**
 * @file crypto_ecdsa.h
 *
 * ECDSA signature verification over NIST P-256 with SHA-256, which is what `ES256` means in a JWT and
 * the other half of what an OpenID Connect provider may sign an id_token with.
 *
 * ```c
 * NYA_CryptoEcdsaPublicKey key = { 0 };
 * NYA_TRY(nya_crypto_ecdsa_public_key_from_xy(x, x_size, y, y_size, &key));
 *
 * if (!nya_crypto_ecdsa_verify_sha256(&key, message, message_size, signature, signature_size)) refuse();
 * ```
 *
 * ── verify only, for the same reason RSA is ──
 *
 * There is no private key type and no signing. ECDSA signing needs a per-signature random value that
 * is secret, unique and unbiased, and getting any of those wrong hands over the private key — the
 * failure that took Sony's console keys and several bitcoin wallets. Where *this* program signs it
 * uses Ed25519 (crypto_sign.h), which has no such value to get wrong. What the engine needs from
 * P-256 is the other direction: somebody else signed something and this has to decide whether to
 * believe it.
 *
 * So, as in crypto_rsa.h, everything here runs on published numbers and is written for clarity rather
 * than constant time. Do not reach for it with anything secret in hand.
 *
 * ── P-256 and no other curve ──
 *
 * One curve, because `ES256` names exactly one and a verifier that accepted several would be a place
 * for a caller to be talked into the weakest of them. P-384 and P-521 are the same arithmetic with
 * different constants on the day something needs them; secp256k1 is not, and nothing in this engine
 * speaks to a blockchain.
 *
 * ── what a caller must still check ──
 *
 * The same as for RSA: a verified signature says the holder of the private key signed these bytes,
 * and nothing about who that is or whether it still matters. The issuer, the audience, the expiry and
 * the nonce belong to whoever called this.
 * */
#pragma once

#include "nyangine-std/base/base_attributes.h"
#include "nyangine-std/base/base_error.h"
#include "nyangine-std/base/base_types.h"

// CONSTANTS

/** Bytes one coordinate takes, which is also what each half of a signature takes. */
#define NYA_CRYPTO_ECDSA_COORDINATE_BYTES 32

/** Bytes a signature takes: `r` and `s`, each padded to the coordinate size. This is the JWS form. */
#define NYA_CRYPTO_ECDSA_SIGNATURE_BYTES ((u64)NYA_CRYPTO_ECDSA_COORDINATE_BYTES * 2ULL)

// TYPES

typedef struct NYA_CryptoEcdsaPublicKey NYA_CryptoEcdsaPublicKey;

/**
 * One public key: a point on the curve, held in the Montgomery form the arithmetic works in.
 *
 * Converted once when it is read, because a key is read once and used for as long as a provider keeps
 * publishing it, and because that is where the point is checked for being on the curve at all.
 * */
struct NYA_CryptoEcdsaPublicKey {
    u64 x[4];
    u64 y[4];
};

// FUNCTIONS

/**
 * Reads a key from the two big endian coordinates a JWKS carries as `x` and `y`.
 *
 * Refuses a coordinate that is not on the curve, one that is not below the field prime, and the point
 * at infinity. That check is the whole of invalid-curve attack prevention and it costs one field
 * multiplication, so it happens here rather than being left to a caller who would have no idea.
 * */
NYA_API NYA_Error nya_crypto_ecdsa_public_key_from_xy(const u8* x, u64 x_size, const u8* y, u64 y_size, OUT NYA_CryptoEcdsaPublicKey* out_key)
    __attr_no_discard;

/**
 * Whether `signature` is this key's ECDSA signature over SHA-256 of `message`.
 *
 * `signature` is the JWS form: `r` then `s`, each exactly NYA_CRYPTO_ECDSA_COORDINATE_BYTES, which is
 * what a JWT carries. A DER-wrapped signature — what OpenSSL writes by default and what X.509 holds —
 * is a different encoding and is not accepted here; convert it at whatever boundary produced it.
 *
 * False for `r` or `s` outside 1..n-1, which is where a signature of all zeroes and the classic
 * "s and n-s" malleability both land.
 * */
NYA_API b8 nya_crypto_ecdsa_verify_sha256(const NYA_CryptoEcdsaPublicKey* key, const u8* message, u64 message_size, const u8* signature,
                                          u64 signature_size) __attr_no_discard;
