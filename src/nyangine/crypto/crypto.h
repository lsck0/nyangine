/**
 * @file crypto.h
 *
 * Every cryptographic primitive the engine uses, behind names of its own. Nothing outside this module
 * names monocypher or hand rolls a hash, so there is one implementation of each primitive to audit and
 * one set of published vectors proving it.
 *
 * Overview:
 *   crypto_secret.h     32 byte keys, a constant time compare, wiping
 *   crypto_hash.h       SHA-256, HMAC-SHA256, BLAKE2b, and SHA-1 with HMAC-SHA1 for the two old protocols
 *   crypto_aead.h       XChaCha20-Poly1305: encrypt and authenticate, decrypt or refuse
 *   crypto_exchange.h   X25519 key agreement
 *   crypto_sign.h       Ed25519 signatures, RFC 8032
 *   crypto_kdf.h        Argon2id, for passwords and anything else a person types
 *   crypto_encoding.h   base32, RFC 4648, for TOTP secrets
 *   crypto_totp.h       RFC 6238 one time passwords: a secret and a clock to six digits
 *
 * Randomness is platform/random/random.h, which this module draws from and does not wrap.
 *
 * ```c
 * NYA_CryptoKey32 key = { 0 };
 * NYA_TRY(nya_crypto_key_create(&key));
 * defer nya_crypto_key_destroy(&key);
 *
 * NYA_CryptoNonce24 nonce = { 0 };
 * NYA_TRY(nya_crypto_nonce_random(&nonce));
 *
 * NYA_CryptoTag16 tag = { 0 };
 * nya_crypto_aead_encrypt(&key, &nonce, (NYA_CryptoAeadMessage){ .text = bytes, .text_size = size }, &tag);
 *
 * if (!nya_crypto_aead_decrypt(&key, &nonce, (NYA_CryptoAeadMessage){ .text = bytes, .text_size = size }, &tag)) {
 *     return nya_error(NYA_ERROR_PERMISSION_DENIED, "the message was altered");
 * }
 * ```
 *
 * Why monocypher: one audited C file with no allocation, no global state and no build system, which is
 * the shape the rest of the vendored tree has. libsodium does the same job in forty times the code and a
 * configure step, and OpenSSL is a TLS stack whose primitives come attached to its error queue, its
 * engines and its release cadence. Neither buys a primitive monocypher lacks except SHA-256 and SHA-1,
 * and those are two hundred lines here with their vectors beside them.
 *
 * Why SHA-1 at all: RFC 6238 TOTP is HMAC-SHA1 in every authenticator app people own, and RFC 6455's
 * WebSocket handshake is SHA-1 by definition. Neither relies on collision resistance, which is what
 * broke. Nothing new may reach for it; crypto_hash.h says so where it is declared.
 *
 * Fixed size keys, nonces, tags and signatures are structs rather than byte pointers, so a secret key
 * cannot be passed where a public one belongs and a nonce cannot be passed as a key. Every secret has a
 * `_destroy` that wipes it, and scratch copies inside the module are wiped before it returns.
 * */
#pragma once

#include "nyangine/crypto/crypto_secret.h"
/**/
#include "nyangine/crypto/crypto_aead.h"
#include "nyangine/crypto/crypto_ecdsa.h"
#include "nyangine/crypto/crypto_encoding.h"
#include "nyangine/crypto/crypto_exchange.h"
#include "nyangine/crypto/crypto_hash.h"
#include "nyangine/crypto/crypto_kdf.h"
#include "nyangine/crypto/crypto_rsa.h"
#include "nyangine/crypto/crypto_sign.h"
#include "nyangine/crypto/crypto_totp.h"
