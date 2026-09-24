/**
 * @file crypto_rsa.h
 *
 * RSA signature verification, and nothing else: PKCS#1 v1.5 over SHA-256, which is what `RS256` means
 * in a JWT and what every OpenID Connect provider signs an id_token with.
 *
 * ```c
 * NYA_CryptoRsaPublicKey key = { 0 };
 * NYA_TRY(nya_crypto_rsa_public_key_from_parts(modulus, modulus_size, exponent, exponent_size, &key));
 *
 * if (!nya_crypto_rsa_verify_sha256(&key, message, message_size, signature, signature_size)) refuse();
 * ```
 *
 * ── verify only, and why that is the whole file ──
 *
 * Nothing here signs, and no private key type exists. Signing with RSA means holding a private key,
 * which means blinding, constant time division and a CRT implementation that leaks nothing under a
 * fault — a body of work with a much worse failure mode than anything this engine needs. What the
 * engine needs is the other direction: somebody else signed something and this has to decide whether
 * to believe it. Where *this* program is the signer it uses Ed25519 (crypto_sign.h), which is smaller,
 * faster, and has no padding to get wrong.
 *
 * Everything here therefore runs on public data. A modulus, an exponent, a signature and a message
 * are all things the other side already published, so this is written for clarity rather than for
 * constant time, and it says so rather than implying a protection it does not provide. Do not reach
 * for these functions with anything secret in hand.
 *
 * ── what a caller must still check ──
 *
 * A verified signature says the holder of the private key signed these bytes. It says nothing about
 * who that is, what it was for, or whether it is still true. Everything that makes a signature *mean*
 * something — which key was allowed to sign this, the issuer, the audience, the expiry, the nonce —
 * belongs to whoever called this, and for an id_token that is plugins/oidc.
 *
 * ── the bounds ──
 *
 * Keys from 2048 to NYA_CRYPTO_RSA_MAX_BITS bits. Below 2048 is refused outright rather than verified:
 * a 1024-bit RSA signature is not evidence any more, and accepting one because a provider still
 * offers it is how a deployment ends up trusting it. The exponent is at most eight bytes, which covers
 * every public exponent in use (65537 in practice, and this is deliberately not an arbitrary-length
 * exponentiation).
 * */
#pragma once

#include "nyangine/base/base_attributes.h"
#include "nyangine/base/base_error.h"
#include "nyangine/base/base_types.h"

// CONSTANTS

/**
 * The largest key this holds, in bits.
 *
 * 4096 is the largest anybody issues and twice what anybody needs; a key larger than this is refused
 * rather than making every buffer here bigger for a case nothing has.
 * */
#define NYA_CRYPTO_RSA_MAX_BITS 4096

/**
 * The smallest key this will verify with, in bits.
 *
 * 2048 is the floor every standards body has settled on, and it is enforced here rather than left to
 * the caller because the caller is usually a protocol that would happily take whatever a server
 * offered. A 1024-bit signature is refused as if it were invalid, which is what it is.
 * */
#define NYA_CRYPTO_RSA_MIN_BITS 2048

/** Sixty-four bit limbs a key of NYA_CRYPTO_RSA_MAX_BITS takes. */
#define NYA_CRYPTO_RSA_MAX_LIMBS (NYA_CRYPTO_RSA_MAX_BITS / 64)

/** Bytes a key of NYA_CRYPTO_RSA_MAX_BITS takes, which is also the size of a signature made with it. */
#define NYA_CRYPTO_RSA_MAX_BYTES (NYA_CRYPTO_RSA_MAX_BITS / 8)

// TYPES

typedef struct NYA_CryptoRsaPublicKey NYA_CryptoRsaPublicKey;

/**
 * One public key, as the two numbers it is.
 *
 * Held as limbs rather than as the bytes it arrived in, because that is the form the arithmetic wants
 * and converting once at the boundary is what keeps the conversion in one place.
 * */
struct NYA_CryptoRsaPublicKey {
    /** The modulus, little endian by limb. */
    u64 modulus[NYA_CRYPTO_RSA_MAX_LIMBS];

    /** Limbs of `modulus` that are used, and the key's size in bits and bytes. */
    u32 limbs;
    u32 bits;
    u32 bytes;

    /** The public exponent. 65537 in every key anybody issues. */
    u64 exponent;
};

// FUNCTIONS

/**
 * Reads a key from the two big endian numbers a JWKS or a certificate carries.
 *
 * Leading zero bytes are ignored, as they are in both formats. Refuses a modulus outside the bounds
 * above, an even modulus, an exponent that is not odd and greater than one, or an exponent longer
 * than eight bytes.
 * */
NYA_API NYA_Error nya_crypto_rsa_public_key_from_parts(const u8* modulus, u64 modulus_size, const u8* exponent, u64 exponent_size,
                                                       OUT NYA_CryptoRsaPublicKey* out_key) __attr_no_discard;

/**
 * Whether `signature` is this key's PKCS#1 v1.5 signature over SHA-256 of `message`.
 *
 * False for anything that is not exactly that, including a signature of the wrong length, padding
 * that is not what EMSA-PKCS1-v1_5 produces, and the DigestInfo of a different hash. The check is
 * written as "recompute what the padding must have been and compare", never as "parse what arrived
 * and trust the offsets", because parsing is where every Bleichenbacher-style forgery got in.
 * */
NYA_API b8 nya_crypto_rsa_verify_sha256(const NYA_CryptoRsaPublicKey* key, const u8* message, u64 message_size, const u8* signature, u64 signature_size)
    __attr_no_discard;
