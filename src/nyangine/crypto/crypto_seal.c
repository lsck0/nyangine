#include "nyangine/base/base_assert.h"
#include "nyangine/base/base_memory.h"
#include "nyangine/crypto/crypto_aead.h"
#include "nyangine/crypto/crypto_encoding.h"
#include "nyangine/crypto/crypto_seal.h"
#include "nyangine/crypto/crypto_secret.h"

#include <string.h>

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * CONSTANTS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/** The bytes before the ciphertext: the version byte and the nonce. */
#define _NYA_CRYPTO_SEAL_PREFIX_BYTES (1 + NYA_CRYPTO_NONCE_BYTES)

/** The smallest a box can be: the prefix, an empty ciphertext, and the tag. */
#define _NYA_CRYPTO_SEAL_MIN_BLOB (_NYA_CRYPTO_SEAL_PREFIX_BYTES + NYA_CRYPTO_TAG_BYTES)

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PUBLIC API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

NYA_Error nya_crypto_seal(void* key, NYA_Arena* arena, NYA_ConstCString aad, u8* plaintext, u64 plaintext_size, NYA_String** out_text) {
    nya_assert(arena != nullptr);
    nya_assert(out_text != nullptr);

    *out_text = nullptr;

    // Wiped on every path out, so a refused seal leaves no more of the secret behind than a written one.
    if (key == nullptr || (plaintext == nullptr && plaintext_size != 0)) {
        if (plaintext != nullptr) nya_crypto_wipe(plaintext, plaintext_size);
        return nya_error(NYA_ERROR_INVALID_ARGUMENT, "a seal needs a key and, for a non-zero size, bytes");
    }

    const NYA_CryptoKey32* aead_key = key;

    // A fresh random nonce per seal: 24 bytes are wide enough that random never repeats in practice,
    // which is what lets this keep no counter.
    NYA_CryptoNonce24 nonce = { 0 };
    if (!nya_crypto_nonce_random(&nonce).ok) {
        nya_crypto_wipe(plaintext, plaintext_size);
        return nya_error(NYA_ERROR_NOT_OK, "the system random source failed");
    }

    u64 blob_size = _NYA_CRYPTO_SEAL_PREFIX_BYTES + plaintext_size + NYA_CRYPTO_TAG_BYTES;
    u8* blob      = nya_arena_alloc(arena, blob_size);

    blob[0] = NYA_CRYPTO_SEAL_VERSION;
    nya_memcpy(blob + 1, nonce.bytes, sizeof(nonce.bytes));
    if (plaintext_size > 0) nya_memcpy(blob + _NYA_CRYPTO_SEAL_PREFIX_BYTES, plaintext, plaintext_size);

    // The plaintext is now in the blob, about to be encrypted in place; the caller's copy is done with.
    nya_crypto_wipe(plaintext, plaintext_size);

    NYA_CryptoTag16 tag = { 0 };
    nya_crypto_aead_encrypt(aead_key, &nonce,
                            (NYA_CryptoAeadMessage){
                                .text            = blob + _NYA_CRYPTO_SEAL_PREFIX_BYTES,
                                .text_size       = plaintext_size,
                                .associated      = (const u8*)aad,
                                .associated_size = aad != nullptr ? strlen(aad) : 0,
                            },
                            &tag);

    nya_memcpy(blob + _NYA_CRYPTO_SEAL_PREFIX_BYTES + plaintext_size, tag.bytes, sizeof(tag.bytes));

    // The blob is ciphertext and tag now, no longer a secret. Two bytes of slack over the exact encoded
    // length keep the terminator and any rounding comfortably inside the buffer.
    u64         capacity = blob_size * 2 + 4;
    NYA_String* text     = nya_string_create_with_capacity(arena, capacity);

    u64 written = 0;
    if (!nya_crypto_base64url_encode(blob, blob_size, (char*)text->items, capacity, &written)) {
        return nya_error(NYA_ERROR_OUT_OF_MEMORY, "the sealed value does not fit its buffer");
    }

    text->length = written;

    *out_text = text;
    return NYA_OK;
}

NYA_Error nya_crypto_unseal(void* key, NYA_Arena* arena, NYA_ConstCString aad, const char* text, u64 text_size, NYA_String** out_plaintext) {
    nya_assert(arena != nullptr);
    nya_assert(out_plaintext != nullptr);

    *out_plaintext = nullptr;

    if (key == nullptr || text == nullptr) return nya_error(NYA_ERROR_INVALID_ARGUMENT, "an unseal needs a key and a string");

    const NYA_CryptoKey32* aead_key = key;

    // The decoded blob is never longer than the text that carried it.
    u64 capacity  = text_size + 1;
    u8* blob      = nya_arena_alloc(arena, capacity);
    u64 blob_size = 0;

    // A string that does not decode, or is too short to be a box, is simply not one. The same closed
    // answer as a wrong key, so nothing tells the difference between "not a box" and "not yours".
    if (!nya_crypto_base64url_decode(text, text_size, blob, capacity, &blob_size) || blob_size < _NYA_CRYPTO_SEAL_MIN_BLOB ||
        blob[0] != NYA_CRYPTO_SEAL_VERSION) {
        nya_crypto_wipe(blob, capacity);
        return nya_error(NYA_ERROR_PERMISSION_DENIED, "the sealed value did not open");
    }

    u64 ciphertext_size = blob_size - _NYA_CRYPTO_SEAL_MIN_BLOB;

    NYA_CryptoNonce24 nonce = { 0 };
    nya_memcpy(nonce.bytes, blob + 1, sizeof(nonce.bytes));

    NYA_CryptoTag16 tag = { 0 };
    nya_memcpy(tag.bytes, blob + _NYA_CRYPTO_SEAL_PREFIX_BYTES + ciphertext_size, sizeof(tag.bytes));

    // Decrypts in place only when the tag matches. A wrong key, a wrong field, or a single altered byte
    // all land here as false, and the plaintext is never even looked at.
    b8 opened = nya_crypto_aead_decrypt(aead_key, &nonce,
                                        (NYA_CryptoAeadMessage){
                                            .text            = blob + _NYA_CRYPTO_SEAL_PREFIX_BYTES,
                                            .text_size       = ciphertext_size,
                                            .associated      = (const u8*)aad,
                                            .associated_size = aad != nullptr ? strlen(aad) : 0,
                                        },
                                        &tag);
    if (!opened) {
        nya_crypto_wipe(blob, capacity);
        return nya_error(NYA_ERROR_PERMISSION_DENIED, "the sealed value did not open");
    }

    NYA_String* plaintext = nya_string_create_with_capacity(arena, ciphertext_size + 1);
    if (ciphertext_size > 0) nya_memcpy(plaintext->items, blob + _NYA_CRYPTO_SEAL_PREFIX_BYTES, ciphertext_size);
    plaintext->items[ciphertext_size] = '\0';
    plaintext->length                 = ciphertext_size;

    // The blob held the plaintext once the tag matched; wiped now that it is copied out.
    nya_crypto_wipe(blob, capacity);

    *out_plaintext = plaintext;
    return NYA_OK;
}
