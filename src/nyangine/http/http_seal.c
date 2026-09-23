#include <string.h>

#include "nyangine/base/base_assert.h"
#include "nyangine/base/base_clock.h"
#include "nyangine/base/base_memory.h"
#include "nyangine/crypto/crypto_aead.h"
#include "nyangine/crypto/crypto_encoding.h"
#include "nyangine/crypto/crypto_hash.h"
#include "nyangine/crypto/crypto_secret.h"
#include "nyangine/http/http_cookie.h"
#include "nyangine/http/http_seal.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * CONSTANTS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/** The one version this reads and writes. A token that starts with anything else was not made here. */
#define _NYA_HTTP_SEAL_VERSION 1

/** The plaintext this actually encrypts: a version byte, an expiry, then the caller's bytes. */
#define _NYA_HTTP_SEAL_HEADER_BYTES 9

/** The sealed blob before base64url: the nonce, then the encrypted header and plaintext, then the tag. */
#define _NYA_HTTP_SEAL_MAX_BLOB (NYA_CRYPTO_NONCE_BYTES + _NYA_HTTP_SEAL_HEADER_BYTES + NYA_HTTP_SEAL_MAX_PLAINTEXT + NYA_CRYPTO_TAG_BYTES)

/**
 * What the seal key is derived from, so it is never the raw secret and never the JWT key.
 *
 * A domain string mixed into the hash: the JWT layer hashes the same secret with a different one (or
 * uses it raw), so one secret makes two keys that can never be mistaken for each other. Change this and
 * every token in the wild stops opening, which is the correct behaviour for a key rotation.
 * */
#define _NYA_HTTP_SEAL_DOMAIN "nyangine-http-seal-v1"

static_assert(
    (_NYA_HTTP_SEAL_MAX_BLOB * 4 + 2) / 3 + 1 <= NYA_HTTP_SEAL_MAX_TOKEN,
    "a sealed token at the maximum plaintext does not fit its own buffer"
);

static_assert(NYA_HTTP_SEAL_MAX_TOKEN <= NYA_HTTP_MAX_COOKIE_VALUE, "a sealed token does not fit a cookie value");

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API DECLARATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/** The 32 byte AEAD key for `label`, derived from the secret and this module's domain. */
NYA_INTERNAL void _nya_http_seal_key(const u8* secret, u64 secret_size, NYA_ConstCString label, OUT NYA_CryptoKey32* out_key);

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PUBLIC API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

NYA_Error nya_http_seal(
    const u8* secret, u64 secret_size, NYA_ConstCString label, const u8* plaintext, u64 plaintext_size, u64 ttl_s, char* out_token, u64 capacity
) {
    nya_assert(out_token != nullptr && capacity > 0);

    out_token[0] = '\0';

    if (secret == nullptr || secret_size < NYA_HTTP_SEAL_MIN_SECRET_BYTES) {
        return nya_error(NYA_ERROR_INVALID_ARGUMENT, "a seal secret is at least %d bytes", NYA_HTTP_SEAL_MIN_SECRET_BYTES);
    }

    if (label == nullptr || label[0] == '\0') return nya_error(NYA_ERROR_INVALID_ARGUMENT, "a seal needs a label; see http_seal.h");
    if (ttl_s == 0) return nya_error(NYA_ERROR_INVALID_ARGUMENT, "a seal needs an expiry; a token good forever is a password that cannot change");
    if (plaintext == nullptr && plaintext_size != 0) return nya_error(NYA_ERROR_INVALID_ARGUMENT, "a seal was given a size but no bytes");

    if (plaintext_size > NYA_HTTP_SEAL_MAX_PLAINTEXT) {
        return nya_error(NYA_ERROR_OUT_OF_MEMORY, "a seal carries at most %d bytes; put the rest in a row and seal its id", NYA_HTTP_SEAL_MAX_PLAINTEXT);
    }

    // version, expiry, then the plaintext, all encrypted together.
    u8 message[_NYA_HTTP_SEAL_HEADER_BYTES + NYA_HTTP_SEAL_MAX_PLAINTEXT] = { 0 };

    u64 expires_at_s = nya_clock_get_timestamp_s() + ttl_s;

    message[0] = _NYA_HTTP_SEAL_VERSION;

    for (u32 index = 0; index < 8; index++) message[1 + index] = (u8)(expires_at_s >> (index * 8));

    if (plaintext_size > 0) nya_memcpy(message + _NYA_HTTP_SEAL_HEADER_BYTES, plaintext, plaintext_size);

    u64 message_size = _NYA_HTTP_SEAL_HEADER_BYTES + plaintext_size;

    NYA_CryptoKey32 key = { 0 };
    _nya_http_seal_key(secret, secret_size, label, &key);

    // A fresh random nonce per seal: XChaCha20's 24 bytes are wide enough that random never repeats in
    // practice, which is what lets this be stateless — there is no counter to keep.
    NYA_CryptoNonce24 nonce = { 0 };

    if (!nya_crypto_nonce_random(&nonce).ok) {
        nya_crypto_key_destroy(&key);
        return nya_error(NYA_ERROR_NOT_OK, "the system random source failed");
    }

    // The blob: nonce, then the ciphertext in place, then the tag. The label is authenticated as
    // associated data, so it is not in the blob and yet a wrong one makes the tag not match.
    u8 blob[_NYA_HTTP_SEAL_MAX_BLOB] = { 0 };

    nya_memcpy(blob, nonce.bytes, sizeof(nonce.bytes));
    nya_memcpy(blob + NYA_CRYPTO_NONCE_BYTES, message, message_size);

    NYA_CryptoTag16 tag = { 0 };

    nya_crypto_aead_encrypt(
        &key, &nonce,
        (NYA_CryptoAeadMessage){
            .text            = blob + NYA_CRYPTO_NONCE_BYTES,
            .text_size       = message_size,
            .associated      = (const u8*)label,
            .associated_size = strlen(label),
        },
        &tag
    );

    nya_memcpy(blob + NYA_CRYPTO_NONCE_BYTES + message_size, tag.bytes, sizeof(tag.bytes));

    u64 blob_size = NYA_CRYPTO_NONCE_BYTES + message_size + NYA_CRYPTO_TAG_BYTES;

    nya_crypto_key_destroy(&key);
    nya_crypto_wipe(message, sizeof(message));

    u64 written = 0;

    if (!nya_crypto_base64url_encode(blob, blob_size, out_token, capacity, &written)) {
        nya_crypto_wipe(blob, sizeof(blob));
        return nya_error(NYA_ERROR_OUT_OF_MEMORY, "the sealed token does not fit the buffer");
    }

    nya_crypto_wipe(blob, sizeof(blob));

    return NYA_OK;
}

b8 nya_http_unseal(
    const u8* secret, u64 secret_size, NYA_ConstCString label, const char* token, u64 token_size, u8* out_plaintext, u64 capacity, u64* out_size
) {
    nya_assert(out_size != nullptr);

    *out_size = 0;

    if (out_plaintext != nullptr && capacity > 0) out_plaintext[0] = 0;

    if (secret == nullptr || secret_size < NYA_HTTP_SEAL_MIN_SECRET_BYTES) return false;
    if (label == nullptr || label[0] == '\0') return false;
    if (token == nullptr || token_size == 0) return false;

    u8  blob[_NYA_HTTP_SEAL_MAX_BLOB] = { 0 };
    u64 blob_size                     = 0;

    // A token that does not decode, or that is too long or too short to be one, is simply not a seal.
    if (!nya_crypto_base64url_decode(token, token_size, blob, sizeof(blob), &blob_size)) return false;
    if (blob_size < NYA_CRYPTO_NONCE_BYTES + _NYA_HTTP_SEAL_HEADER_BYTES + NYA_CRYPTO_TAG_BYTES) return false;

    u64 message_size = blob_size - NYA_CRYPTO_NONCE_BYTES - NYA_CRYPTO_TAG_BYTES;

    NYA_CryptoNonce24 nonce = { 0 };
    nya_memcpy(nonce.bytes, blob, sizeof(nonce.bytes));

    NYA_CryptoTag16 tag = { 0 };
    nya_memcpy(tag.bytes, blob + NYA_CRYPTO_NONCE_BYTES + message_size, sizeof(tag.bytes));

    NYA_CryptoKey32 key = { 0 };
    _nya_http_seal_key(secret, secret_size, label, &key);

    // Decrypts in place only when the tag matches. A wrong secret, a wrong label, or a single altered
    // byte all land here as false, and the plaintext is never even looked at.
    b8 opened = nya_crypto_aead_decrypt(
        &key, &nonce,
        (NYA_CryptoAeadMessage){
            .text            = blob + NYA_CRYPTO_NONCE_BYTES,
            .text_size       = message_size,
            .associated      = (const u8*)label,
            .associated_size = strlen(label),
        },
        &tag
    );

    nya_crypto_key_destroy(&key);

    if (!opened) {
        nya_crypto_wipe(blob, sizeof(blob));
        return false;
    }

    const u8* message = blob + NYA_CRYPTO_NONCE_BYTES;

    // Made here, and this version. A token from a future version of this format is not one to guess at.
    if (message[0] != _NYA_HTTP_SEAL_VERSION) {
        nya_crypto_wipe(blob, sizeof(blob));
        return false;
    }

    u64 expires_at_s = 0;
    for (u32 index = 0; index < 8; index++) expires_at_s |= (u64)message[1 + index] << (index * 8);

    // Expired is not open. The check is against the wall clock, so a token also stops opening if that
    // clock is set back past when it was made, which is the safe direction to be wrong in.
    if (nya_clock_get_timestamp_s() >= expires_at_s) {
        nya_crypto_wipe(blob, sizeof(blob));
        return false;
    }

    u64 plaintext_size = message_size - _NYA_HTTP_SEAL_HEADER_BYTES;

    if (plaintext_size > capacity) {
        nya_crypto_wipe(blob, sizeof(blob));
        return false;
    }

    if (plaintext_size > 0 && out_plaintext != nullptr) nya_memcpy(out_plaintext, message + _NYA_HTTP_SEAL_HEADER_BYTES, plaintext_size);

    *out_size = plaintext_size;

    nya_crypto_wipe(blob, sizeof(blob));

    return true;
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

void _nya_http_seal_key(const u8* secret, u64 secret_size, NYA_ConstCString label, NYA_CryptoKey32* out_key) {
    /*
     * blake2b keyed by the secret, over the domain and the label. Keying with the secret rather than
     * hashing it in is what makes this a proper key derivation: the domain separates this key from every
     * other use of the same secret, and the label is in the message so two labels are two keys — a token
     * cannot be moved between them even by somebody who could forge one, which is belt and suspenders
     * over the associated data that already binds the label.
     *
     * The secret is bounded here because blake2b's key is at most 64 bytes; a longer one is folded to a
     * key first, so the whole of it still matters.
     */
    u8 seal_key[NYA_CRYPTO_BLAKE2B_KEY_BYTES_MAX] = { 0 };
    u64 seal_key_size = secret_size;

    if (seal_key_size > sizeof(seal_key)) {
        nya_crypto_blake2b(secret, secret_size, seal_key, sizeof(seal_key));
        seal_key_size = sizeof(seal_key);
    } else {
        nya_memcpy(seal_key, secret, secret_size);
    }

    // domain, a separator, then the label: the message keyed blake2b runs over.
    u8  message[sizeof(_NYA_HTTP_SEAL_DOMAIN) + NYA_HTTP_MAX_COOKIE_NAME + 2] = { 0 };
    u64 message_size                                                          = 0;

    u64 domain_size = sizeof(_NYA_HTTP_SEAL_DOMAIN) - 1;
    nya_memcpy(message, _NYA_HTTP_SEAL_DOMAIN, domain_size);
    message_size = domain_size;

    message[message_size++] = 0;

    u64 label_size = strlen(label);
    if (label_size > NYA_HTTP_MAX_COOKIE_NAME) label_size = NYA_HTTP_MAX_COOKIE_NAME;

    nya_memcpy(message + message_size, label, label_size);
    message_size += label_size;

    nya_crypto_blake2b_keyed(seal_key, seal_key_size, message, message_size, out_key->bytes, sizeof(out_key->bytes));

    nya_crypto_wipe(seal_key, sizeof(seal_key));
}
