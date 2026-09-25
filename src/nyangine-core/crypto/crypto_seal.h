/**
 * @file crypto_seal.h
 *
 * A sealed box: a value encrypted under a key, authenticated, and written as one base64url string.
 *
 * This is the codec behind serde's `@secret` fields (see serde_reflect.h). serde owns the reflection
 * and the value↔bytes step and knows nothing of keys; this owns the key, the AEAD and the encoding and
 * knows nothing of reflection. The two meet at the NYA_SerdeSecret function pointers, whose shape these
 * two functions have exactly, so they wire straight into it with the key as the codec's `user`.
 *
 * Overview:
 *   nya_crypto_seal     plaintext bytes to a sealed base64url string, the plaintext wiped as it goes
 *   nya_crypto_unseal   that string back to the plaintext, or an error when the tag does not match
 *
 * ```c
 * NYA_SerdeSecret secret = { .seal = nya_crypto_seal, .unseal = nya_crypto_unseal, .user = &key };
 * NYA_TRY(nya_reflect_save_file_secret(nya_reflect_of(Credentials), &creds, "creds.nya", NYA_SERDE_PRETTY, secret));
 * ```
 *
 * The box, before base64url: a version byte, then a fresh random 24 byte nonce, then the ciphertext in
 * place, then the 16 byte Poly1305 tag. XChaCha20-Poly1305, so the nonce is wide enough to draw at
 * random per seal without a counter to keep. The `aad` — the field's name where serde is the caller —
 * is authenticated but not stored, so a ciphertext moved to another field fails to open even though
 * the key is right. A wrong key, a wrong field, or a single altered byte all fail closed: the
 * plaintext is never returned. Nothing here logs the key or the plaintext.
 * */
#pragma once

#include "nyangine-std/base/base_arena.h"
#include "nyangine-std/base/base_attributes.h"
#include "nyangine-std/base/base_error.h"
#include "nyangine-std/base/base_string.h"
#include "nyangine-std/base/base_types.h"

// CONSTANTS

/** The one box version this reads and writes. A blob that starts with anything else was not made here. */
#define NYA_CRYPTO_SEAL_VERSION 1

// FUNCTIONS

/**
 * Seals `plaintext_size` bytes under `key` and writes the box as a base64url string into `out_text`,
 * allocated from `arena`. `aad` is authenticated with the ciphertext and not stored.
 *
 * `key` is a `const NYA_CryptoKey32*`, taken as `void*` so this is the exact shape of a NYA_SerdeSecret
 * seal. `plaintext` is wiped before returning, success or failure, so the secret does not linger in the
 * caller's scratch. Fails only when the system random source does.
 * */
NYA_API NYA_Error nya_crypto_seal(void* key, NYA_Arena* arena, NYA_ConstCString aad, u8* plaintext, u64 plaintext_size, OUT NYA_String** out_text)
    __attr_no_discard;

/**
 * Opens the base64url box in `text` (`text_size` characters) under `key`, writing the plaintext into
 * `out_plaintext`, allocated from `arena`. `aad` must be what it was sealed with.
 *
 * Returns NYA_ERROR_PERMISSION_DENIED, and writes nothing, when the string is not a box, is a version
 * this does not read, or does not authenticate under this key, field and every byte as they are — an
 * altered ciphertext and a wrong key are the same answer. `key` is a `const NYA_CryptoKey32*` as a
 * `void*`, for the reason nya_crypto_seal's is.
 * */
NYA_API NYA_Error
nya_crypto_unseal(void* key, NYA_Arena* arena, NYA_ConstCString aad, const char* text, u64 text_size, OUT NYA_String** out_plaintext) __attr_no_discard;
