/**
 * @file crypto_totp.h
 *
 * RFC 6238 time based one time passwords: a secret and a clock become six digits.
 *
 * The arithmetic and nothing else. No user, no storage, no policy: which codes have already been
 * spent and how often somebody may guess are questions about an account, and they live in
 * http_totp.h. This file is what both an authenticator app and this engine compute, and the two have
 * to agree to the digit.
 *
 * Overview:
 *   NYA_CryptoTotpSecret             the 160 bit shared secret
 *   nya_crypto_totp_secret_create    a fresh one from the OS random source
 *   nya_crypto_totp_secret_destroy   its wipe
 *   nya_crypto_totp_counter          a unix time to the step number it falls in
 *   nya_crypto_totp_code             RFC 4226 HOTP over HMAC-SHA1, as six decimal digits
 *   nya_crypto_totp_code_equals      compare two codes in constant time
 *
 * ```c
 * NYA_CryptoTotpSecret secret = { 0 };
 * NYA_TRY(nya_crypto_totp_secret_create(&secret));
 * defer nya_crypto_totp_secret_destroy(&secret);
 *
 * char code[NYA_CRYPTO_TOTP_CODE_BYTES] = { 0 };
 * nya_crypto_totp_code(secret.bytes, sizeof(secret.bytes), nya_crypto_totp_counter(now_s), code);
 * ```
 *
 * ── the parameters are not configurable ──
 *
 * HMAC-SHA1, six digits, a thirty second step and a zero epoch. RFC 6238 allows other choices and
 * every authenticator people actually have on their phone ignores them: Google Authenticator, Aegis
 * and 1Password read `algorithm` and `digits` out of an otpauth URI and several of them silently use
 * SHA-1 and six anyway. A parameter nobody may change is a constant, and a constant cannot be
 * downgraded by whoever writes the enrolment URI.
 *
 * SHA-1 is fine here and only here: HOTP is an HMAC, whose strength rests on the compression function
 * behaving like a PRF, and no collision buys an attacker a code. crypto_hash.h says the same where
 * nya_crypto_hmac_sha1 is declared.
 *
 * ── what an attacker gets from this ──
 *
 * Given the secret, everything: they can mint every past and future code. So the secret is the whole
 * of the security property, it is never logged, never put in an error message and wiped when it is
 * done with. Given only codes, essentially nothing: one code is twenty bits of HMAC output and
 * recovering the key from it is recovering an HMAC key from one truncated tag.
 *
 * What this file cannot defend against on its own is guessing. Six digits is one in a million per
 * try, which is only safe with a limit on the tries; nothing here counts them, and a caller that
 * verifies without http_totp.h's guard, or one of its own, has built an oracle rather than a factor.
 * */
#pragma once

#include "nyangine/base/base_attributes.h"
#include "nyangine/base/base_error.h"
#include "nyangine/base/base_types.h"

// CONSTANTS

/**
 * 160 bits of secret, which is RFC 4226 section 4's R6: the key must be at least as long as the hash
 * it is used with, and HMAC-SHA1's is twenty bytes.
 * */
#define NYA_CRYPTO_TOTP_SECRET_BYTES 20

/** RFC 6238 section 4: the step X in seconds, and T0, which is the unix epoch itself. */
#define NYA_CRYPTO_TOTP_STEP_S  30
#define NYA_CRYPTO_TOTP_EPOCH_S 0

/** Digits a code is written in, and therefore 10^6 possible codes. */
#define NYA_CRYPTO_TOTP_DIGITS 6

/** A code as text: the digits and a terminator. */
#define NYA_CRYPTO_TOTP_CODE_BYTES (NYA_CRYPTO_TOTP_DIGITS + 1)

// TYPES

typedef struct NYA_CryptoTotpSecret NYA_CryptoTotpSecret;

/**
 * The secret one account shares with one authenticator. A struct rather than a byte pointer for the
 * reason every other secret here is one: it cannot be handed to a parameter expecting a public value.
 * */
struct NYA_CryptoTotpSecret {
    u8 bytes[NYA_CRYPTO_TOTP_SECRET_BYTES];
};

// FUNCTIONS

/** A secret from the operating system's random source. Fails only when that source does, leaving it zero. */
NYA_API NYA_Error nya_crypto_totp_secret_create(OUT NYA_CryptoTotpSecret* out_secret) __attr_no_discard;

/** Wipes the secret. Safe on one that was never created and on one already destroyed. */
NYA_API void nya_crypto_totp_secret_destroy(NYA_CryptoTotpSecret* secret);

/** The step number `unix_s` falls in: RFC 6238's T = (unix - T0) / X. */
NYA_API u64 nya_crypto_totp_counter(u64 unix_s) __attr_no_discard;

/**
 * The six digit code for `counter`, terminated, zero padded on the left.
 *
 * RFC 4226 section 5.3: HMAC-SHA1 of the counter as eight big endian bytes, dynamically truncated at
 * the offset the last nibble names, modulo a million.
 *
 * `key_size` is free rather than fixed at NYA_CRYPTO_TOTP_SECRET_BYTES so the published vectors can be
 * fed the keys they print. An enrolled secret is always twenty bytes; see NYA_CryptoTotpSecret.
 * */
NYA_API void nya_crypto_totp_code(const u8* key, u64 key_size, u64 counter, OUT char out_code[NYA_CRYPTO_TOTP_CODE_BYTES]);

/**
 * Whether two codes are the same, in time that does not depend on where they differ.
 *
 * Both buffers are read whole, terminator included, so a caller with a submission of its own length
 * copies it into a zeroed buffer of this size first and a short one differs rather than matching as a
 * prefix. Use this and never strcmp: how soon a comparison stops is a measurable fact about the right
 * answer, and an attacker who may guess a million times can use it.
 * */
NYA_API b8 nya_crypto_totp_code_equals(const char a[NYA_CRYPTO_TOTP_CODE_BYTES], const char b[NYA_CRYPTO_TOTP_CODE_BYTES]) __attr_no_discard;
