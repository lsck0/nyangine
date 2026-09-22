/**
 * @file crypto_encoding.h
 *
 * base32, RFC 4648 section 6, for TOTP secrets.
 *
 * The alphabet an authenticator app reads a secret in, because it has no characters a person confuses
 * when typing it off a screen.
 *
 * Overview:
 *   NYA_CRYPTO_BASE32_LENGTH       characters `size` bytes encode to, padding included
 *   nya_crypto_base32_encode       bytes to padded upper case text
 *   nya_crypto_base32_decode       exactly that form back, anything else refused
 *
 * ```c
 * char text[NYA_CRYPTO_BASE32_LENGTH(20) + 1] = { 0 };
 * u64  length = 0;
 * NYA_TRY(nya_crypto_base32_encode(secret, 20, text, sizeof(text), &length));
 * ```
 *
 * Here and not beside base64 in base, because what it encodes is a secret: both directions run without
 * a branch or a table lookup on the secret's bytes, so how long they take says nothing about them.
 *
 * The decoder is strict. It takes the upper case alphabet with the RFC's padding and nothing else: no
 * lower case, no missing or extra `=`, no whitespace, and no final character whose unused bits are set,
 * so every secret has one spelling. An otpauth URI drops the padding; its builder cuts the encoded text
 * at the first `=`.
 * */
#pragma once

#include "nyangine/base/base_attributes.h"
#include "nyangine/base/base_error.h"
#include "nyangine/base/base_types.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * CONSTANTS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/** Five bytes are one group of eight characters, and a short last group is padded to eight. */
#define NYA_CRYPTO_BASE32_GROUP_BYTES      5
#define NYA_CRYPTO_BASE32_GROUP_CHARACTERS 8

/** Characters `size` bytes encode to, padding included and the terminator not. */
#define NYA_CRYPTO_BASE32_LENGTH(size)                                                                                                               \
    ((((size) + NYA_CRYPTO_BASE32_GROUP_BYTES - 1) / NYA_CRYPTO_BASE32_GROUP_BYTES) * NYA_CRYPTO_BASE32_GROUP_CHARACTERS)

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * FUNCTIONS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/**
 * `size` bytes as padded base32 in `out_text`, terminated, with the length before the terminator in
 * `out_length`. Fails with NYA_ERROR_OUT_OF_MEMORY when `capacity` is under NYA_CRYPTO_BASE32_LENGTH(size) + 1,
 * writing nothing but an empty string.
 * */
NYA_API NYA_Error nya_crypto_base32_encode(const u8* data, u64 size, OUT char* out_text, u64 capacity, OUT u64* out_length) __attr_no_discard;

/**
 * `length` characters of padded base32 back into bytes. NYA_ERROR_PARSE names the character that broke a
 * rule; NYA_ERROR_OUT_OF_MEMORY means `capacity` cannot hold the result. Nothing is written to `out_data`
 * on either, and `out_size` is zero.
 * */
NYA_API NYA_Error nya_crypto_base32_decode(const char* text, u64 length, OUT u8* out_data, u64 capacity, OUT u64* out_size) __attr_no_discard;
