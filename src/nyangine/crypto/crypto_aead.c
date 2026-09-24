#include "nyangine/base/base_assert.h"
#include "nyangine/crypto/crypto_aead.h"
#include "nyangine/crypto/crypto_secret.h"
#include "nyangine/os/os_random.h"
#include "monocypher.h"

// PRIVATE API DECLARATION

/** The bytes of a nonce a counter fills. The other sixteen stay zero. */
#define _NYA_CRYPTO_NONCE_COUNTER_BYTES 8

/** Asserts what both directions require of a message. */
NYA_INTERNAL void _nya_crypto_aead_message_check(NYA_CryptoAeadMessage message);

// PUBLIC API IMPLEMENTATION

NYA_Error nya_crypto_nonce_random(OUT NYA_CryptoNonce24* out_nonce) {
    nya_assert(out_nonce != nullptr);

    if (!nya_os_random_bytes(out_nonce->bytes, sizeof(out_nonce->bytes))) {
        *out_nonce = (NYA_CryptoNonce24){ 0 };
        return nya_error(NYA_ERROR_NOT_OK, "the system random source failed");
    }

    return NYA_OK;
}

NYA_CryptoNonce24 nya_crypto_nonce_from_counter(u64 counter) {
    NYA_CryptoNonce24 nonce = { 0 };

    for (u32 i = 0; i < _NYA_CRYPTO_NONCE_COUNTER_BYTES; i++) nonce.bytes[i] = (u8)((counter >> (i * 8U)) & 0xFFU);

    return nonce;
}

void nya_crypto_aead_encrypt(
    const NYA_CryptoKey32*   key,
    const NYA_CryptoNonce24* nonce,
    NYA_CryptoAeadMessage    message,
    OUT NYA_CryptoTag16*     out_tag
) {
    nya_assert(key != nullptr);
    nya_assert(nonce != nullptr);
    nya_assert(out_tag != nullptr);
    _nya_crypto_aead_message_check(message);

    // a tag over the associated data alone still needs somewhere for monocypher to point the text.
    u8  none = 0;
    u8* text = message.text != nullptr ? message.text : &none;

    crypto_aead_lock(text, out_tag->bytes, key->bytes, nonce->bytes, message.associated, message.associated_size, text, message.text_size);
}

b8 nya_crypto_aead_decrypt(const NYA_CryptoKey32* key, const NYA_CryptoNonce24* nonce, NYA_CryptoAeadMessage message, const NYA_CryptoTag16* tag) {
    nya_assert(key != nullptr);
    nya_assert(nonce != nullptr);
    nya_assert(tag != nullptr);
    _nya_crypto_aead_message_check(message);

    u8  none = 0;
    u8* text = message.text != nullptr ? message.text : &none;

    // monocypher checks the tag in constant time before decrypting, so a refused message is left untouched.
    return crypto_aead_unlock(text, tag->bytes, key->bytes, nonce->bytes, message.associated, message.associated_size, text, message.text_size) == 0;
}

// PRIVATE API IMPLEMENTATION

void _nya_crypto_aead_message_check(NYA_CryptoAeadMessage message) {
    nya_assert(message.text != nullptr || message.text_size == 0, "text without a buffer");
    nya_assert(message.associated != nullptr || message.associated_size == 0, "associated data without a buffer");
}
