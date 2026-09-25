/**
 * @file core_asset_crypt.h
 *
 * The framing that at-rest asset obfuscation uses, shared by the three places that must agree on it: the
 * build's bundler (pp/asset.c, which frames every entry), the loader (core_asset.c, which unframes one entry
 * at a time), and the test. One definition so they cannot drift.
 *
 * A framed entry is nonce(24) || ciphertext(plaintext_size) || tag(16). The cipher is XChaCha20-Poly1305,
 * the engine's AEAD; the nonce is a keyed BLAKE2b of the plaintext, so it is unique per distinct content and
 * reproducible across builds without a counter to keep, and reveals nothing without the key. The asset's path
 * is the associated data, so an entry moved to another path fails to open even under the right key, exactly as
 * a serde @secret field is bound to its name (crypto_seal.h).
 *
 * Honest threat model: the key is derived deterministically and ships in the binary's .rodata, so this is
 * obfuscation, not DRM. It defeats a hex editor and casual extraction or swapping of assets, not an attacker
 * who reads the key out of the executable. See pp/asset.h.
 * */
#pragma once

#include "nyangine-core/crypto/crypto_aead.h"
#include "nyangine-core/crypto/crypto_hash.h"
#include "nyangine-std/base/base_memory.h"

#include <string.h>

/** The bytes a frame adds around the ciphertext: the nonce in front and the tag behind. */
#define NYA_ASSET_BLOB_FRAME_OVERHEAD (NYA_CRYPTO_NONCE_BYTES + NYA_CRYPTO_TAG_BYTES)

/**
 * Frames `plain_size` plaintext bytes into `out_framed` (which must hold `plain_size` +
 * NYA_ASSET_BLOB_FRAME_OVERHEAD bytes) under `key`, binding `aad`. Returns the framed length.
 * */
static inline u64 nya_asset_blob_frame(const NYA_CryptoKey32* key, const u8* plain, u64 plain_size, NYA_ConstCString aad, OUT u8* out_framed) {
    NYA_CryptoNonce24 nonce = { 0 };
    nya_crypto_blake2b_keyed(key->bytes, sizeof(key->bytes), plain, plain_size, nonce.bytes, sizeof(nonce.bytes));

    nya_memcpy(out_framed, nonce.bytes, sizeof(nonce.bytes));
    if (plain_size > 0) nya_memcpy(out_framed + NYA_CRYPTO_NONCE_BYTES, plain, plain_size);

    NYA_CryptoTag16 tag = { 0 };
    nya_crypto_aead_encrypt(key, &nonce,
                            (NYA_CryptoAeadMessage){
                                .text            = out_framed + NYA_CRYPTO_NONCE_BYTES,
                                .text_size       = plain_size,
                                .associated      = (const u8*)aad,
                                .associated_size = aad != nullptr ? strlen(aad) : 0,
                            },
                            &tag);
    nya_memcpy(out_framed + NYA_CRYPTO_NONCE_BYTES + plain_size, tag.bytes, sizeof(tag.bytes));

    return NYA_CRYPTO_NONCE_BYTES + plain_size + NYA_CRYPTO_TAG_BYTES;
}

/**
 * Unframes `framed_size` bytes at `framed` under `key` and `aad`, writing the plaintext (framed_size minus
 * the overhead) into `out_plain`. Returns false, writing nothing an attacker can rely on, when the frame is
 * too short, a byte was altered, the path is wrong or the key is wrong — a tampered blob and a wrong key are
 * the same closed answer, as in crypto_seal.
 * */
static inline b8
nya_asset_blob_unframe(const NYA_CryptoKey32* key, const u8* framed, u64 framed_size, NYA_ConstCString aad, OUT u8* out_plain) __attr_no_discard;
static inline b8 nya_asset_blob_unframe(const NYA_CryptoKey32* key, const u8* framed, u64 framed_size, NYA_ConstCString aad, OUT u8* out_plain) {
    if (framed_size < NYA_ASSET_BLOB_FRAME_OVERHEAD) return false;
    u64 plain_size = framed_size - NYA_ASSET_BLOB_FRAME_OVERHEAD;

    NYA_CryptoNonce24 nonce = { 0 };
    nya_memcpy(nonce.bytes, framed, sizeof(nonce.bytes));

    NYA_CryptoTag16 tag = { 0 };
    nya_memcpy(tag.bytes, framed + NYA_CRYPTO_NONCE_BYTES + plain_size, sizeof(tag.bytes));

    // The AEAD decrypts in place, so hand it the plaintext buffer with the ciphertext copied in.
    if (plain_size > 0) nya_memcpy(out_plain, framed + NYA_CRYPTO_NONCE_BYTES, plain_size);

    return nya_crypto_aead_decrypt(key, &nonce,
                                   (NYA_CryptoAeadMessage){
                                       .text            = out_plain,
                                       .text_size       = plain_size,
                                       .associated      = (const u8*)aad,
                                       .associated_size = aad != nullptr ? strlen(aad) : 0,
                                   },
                                   &tag);
}
