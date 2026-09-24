/**
 * @file crypto_aead.h
 *
 * XChaCha20-Poly1305: authenticated encryption, in place.
 *
 * Encrypt a message and tag it together with data that travels in the clear; or check the tag and
 * decrypt, or refuse and leave the bytes as they came.
 *
 * Overview:
 *   nya_crypto_nonce_random         24 unpredictable bytes, safe to pick at random per message
 *   nya_crypto_nonce_from_counter   a counter as a nonce, for a stream where the counter never repeats
 *   nya_crypto_aead_encrypt         in place, writes the tag
 *   nya_crypto_aead_decrypt         in place when the tag matches, untouched and false when it does not
 *
 * ```c
 * NYA_CryptoNonce24 nonce = nya_crypto_nonce_from_counter(sequence);
 * NYA_CryptoTag16   tag   = { 0 };
 *
 * NYA_CryptoAeadMessage message = { .text = body, .text_size = body_size, .associated = header, .associated_size = header_size };
 * nya_crypto_aead_encrypt(&key, &nonce, message, &tag);
 * ```
 *
 * The extended nonce is why this and not RFC 8439's ChaCha20-Poly1305: 192 bits can be drawn at random for
 * every message without a birthday bound worth counting, where 96 bits need a counter the caller has to
 * keep from ever repeating. The core is the same RFC 8439 construction behind an HChaCha20 subkey.
 *
 * The message is a struct because text and associated data are both a pointer and a size, and a call that
 * swapped them positionally would compile and quietly authenticate the wrong bytes.
 * */
#pragma once

#include "nyangine/base/base_attributes.h"
#include "nyangine/base/base_error.h"
#include "nyangine/base/base_types.h"
#include "nyangine/crypto/crypto_secret.h"

// CONSTANTS

#define NYA_CRYPTO_NONCE_BYTES 24
#define NYA_CRYPTO_TAG_BYTES   16

// TYPES

typedef struct NYA_CryptoNonce24     NYA_CryptoNonce24;
typedef struct NYA_CryptoTag16       NYA_CryptoTag16;
typedef struct NYA_CryptoAeadMessage NYA_CryptoAeadMessage;

/** Never reused under one key: two messages under one nonce leak their XOR and let the tag be forged. */
struct NYA_CryptoNonce24 {
    u8 bytes[NYA_CRYPTO_NONCE_BYTES];
};

/** The Poly1305 tag that travels with a ciphertext. */
struct NYA_CryptoTag16 {
    u8 bytes[NYA_CRYPTO_TAG_BYTES];
};

/**
 * What one call encrypts and what it only authenticates. `text` may be null when `text_size` is zero, for a
 * tag over the associated data alone; so may `associated`.
 * */
struct NYA_CryptoAeadMessage {
    u8*       text;
    u64       text_size;
    const u8* associated;
    u64       associated_size;
};

// FUNCTIONS

/** A nonce from the operating system's random source. Fails only when that source does. */
NYA_API NYA_Error nya_crypto_nonce_random(OUT NYA_CryptoNonce24* out_nonce) __attr_no_discard;

/** `counter` little endian in the first eight bytes, the rest zero. */
NYA_API NYA_CryptoNonce24 nya_crypto_nonce_from_counter(u64 counter) __attr_no_discard;

/** Encrypts `message.text` in place and writes the tag over it and `message.associated`. */
NYA_API void
nya_crypto_aead_encrypt(const NYA_CryptoKey32* key, const NYA_CryptoNonce24* nonce, NYA_CryptoAeadMessage message, OUT NYA_CryptoTag16* out_tag);

/**
 * Decrypts `message.text` in place when `tag` matches, and returns true. When it does not, which is what
 * an altered ciphertext, associated data, nonce or tag all look like, the text is left exactly as it came
 * and the answer is false. A b8 rather than an NYA_Error because this runs per packet and a forged
 * packet is an expected input, not a failure worth a message.
 * */
NYA_API b8
nya_crypto_aead_decrypt(const NYA_CryptoKey32* key, const NYA_CryptoNonce24* nonce, NYA_CryptoAeadMessage message, const NYA_CryptoTag16* tag)
    __attr_no_discard;
