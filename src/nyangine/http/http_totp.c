#include "nyangine/base/base_assert.h"
#include "nyangine/base/base_memory.h"
#include "nyangine/base/base_url.h"
#include "nyangine/crypto/crypto_encoding.h"
#include "nyangine/crypto/crypto_hash.h"
#include "nyangine/crypto/crypto_secret.h"
#include "nyangine/crypto/crypto_totp.h"
#include "nyangine/http/http_totp.h"
#include "nyangine/os/os_random.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API DECLARATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/** A recovery code without its dash: sixteen base32 characters, which is what the hash is taken over. */
#define _NYA_HTTP_TOTP_RECOVERY_CHARACTERS NYA_CRYPTO_BASE32_LENGTH(NYA_HTTP_TOTP_RECOVERY_SECRET_BYTES)

/** Longest submission the normalisers will walk at all, so a hostile body is not a loop. */
#define _NYA_HTTP_TOTP_SUBMISSION_MAX 64

/** Percent encoding is at most three bytes out per byte in. */
#define _NYA_HTTP_TOTP_ENCODED_MAX(bound) ((3 * ((bound) - 1)) + 1)

/** A name that may go in an otpauth label: not empty, inside its bound, no colon and no control byte. */
NYA_INTERNAL NYA_Error _nya_http_totp_name_check(NYA_ConstCString name, u64 bound, NYA_ConstCString what, OUT u64* out_length) __attr_no_discard;

/** Everything nya_http_totp_enrol_create produces, so its caller has one place to wipe on failure. */
NYA_INTERNAL NYA_Error _nya_http_totp_enrol_fill(NYA_ConstCString issuer, NYA_ConstCString account, OUT NYA_HttpTotpEnrolment* out_enrolment)
    __attr_no_discard;

/** Exactly NYA_CRYPTO_TOTP_DIGITS digits and a terminator, copied into a buffer the comparison can read whole. */
NYA_INTERNAL b8 _nya_http_totp_code_normalise(NYA_ConstCString code, OUT char out_code[NYA_CRYPTO_TOTP_CODE_BYTES]) __attr_no_discard;

/** The stored form of a recovery code: dashes dropped, letters upper cased, then BLAKE2b-256. */
NYA_INTERNAL b8 _nya_http_totp_recovery_hash(NYA_ConstCString code, OUT NYA_HttpTotpRecoveryHash* out_hash) __attr_no_discard;

/** Spends one attempt from the guard's window, starting a new window when the last one has elapsed. */
NYA_INTERNAL b8 _nya_http_totp_attempt(NYA_HttpTotpGuard* guard, u64 now_s) __attr_no_discard;

/**
 * All ones when `condition` holds and all zeros otherwise, so a choice can be made with arithmetic
 * where an `if` would put which candidate matched into the branch predictor. The wrap is the trick.
 * */
__attr_no_sanitize("unsigned-integer-overflow") NYA_INTERNAL u64 _nya_http_totp_mask(b8 condition) __attr_no_discard;

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

NYA_Error _nya_http_totp_name_check(NYA_ConstCString name, u64 bound, NYA_ConstCString what, OUT u64* out_length) {
    *out_length = 0;

    if (name == nullptr || name[0] == '\0') return nya_error(NYA_ERROR_INVALID_ARGUMENT, "the %s of an enrolment cannot be empty", what);

    u64 length = 0;
    while (length < bound && name[length] != '\0') {
        u8 character = (u8)name[length];

        // a colon separates the label's two halves, so one inside a half would let an app read a
        // smuggled issuer as the account; control bytes have no business in a label at all.
        if (character == ':') return nya_error(NYA_ERROR_INVALID_ARGUMENT, "the %s of an enrolment cannot contain a colon", what);
        if (character < 0x20 || character == 0x7F) return nya_error(NYA_ERROR_INVALID_ARGUMENT, "the %s of an enrolment cannot contain control characters", what);

        length++;
    }

    if (name[length] != '\0') return nya_error(NYA_ERROR_INVALID_ARGUMENT, "the %s of an enrolment is longer than " FMTu64 " characters", what, bound - 1);

    *out_length = length;

    return NYA_OK;
}

NYA_Error _nya_http_totp_enrol_fill(NYA_ConstCString issuer, NYA_ConstCString account, OUT NYA_HttpTotpEnrolment* out_enrolment) {
    u64 issuer_length  = 0;
    u64 account_length = 0;
    NYA_TRY(_nya_http_totp_name_check(issuer, NYA_HTTP_TOTP_ISSUER_MAX, "issuer", &issuer_length));
    NYA_TRY(_nya_http_totp_name_check(account, NYA_HTTP_TOTP_ACCOUNT_MAX, "account", &account_length));

    NYA_TRY(nya_crypto_totp_secret_create(&out_enrolment->secret));

    u64 text_length = 0;
    NYA_TRY(nya_crypto_base32_encode(
        out_enrolment->secret.bytes,
        sizeof(out_enrolment->secret.bytes),
        out_enrolment->secret_base32,
        sizeof(out_enrolment->secret_base32),
        &text_length
    ));

    char issuer_encoded[_NYA_HTTP_TOTP_ENCODED_MAX(NYA_HTTP_TOTP_ISSUER_MAX)]   = { 0 };
    char account_encoded[_NYA_HTTP_TOTP_ENCODED_MAX(NYA_HTTP_TOTP_ACCOUNT_MAX)] = { 0 };
    u64  encoded_length                                                          = 0;

    NYA_TRY(nya_percent_encode((const u8*)issuer, issuer_length, issuer_encoded, sizeof(issuer_encoded), &encoded_length));
    NYA_TRY(nya_percent_encode((const u8*)account, account_length, account_encoded, sizeof(account_encoded), &encoded_length));

    /*
     * The label is "issuer:account" and `issuer` is repeated as a parameter, which is what the Key Uri
     * Format asks for: older apps read only the label and newer ones only the parameter. The algorithm,
     * digit and period parameters are written out rather than left to the app's default, because an app's
     * default is not something this server can check.
     */
    s32 written = snprintf(
        out_enrolment->uri,
        sizeof(out_enrolment->uri),
        "otpauth://totp/%s:%s?secret=%s&issuer=%s&algorithm=SHA1&digits=%d&period=%d",
        issuer_encoded,
        account_encoded,
        out_enrolment->secret_base32,
        issuer_encoded,
        NYA_CRYPTO_TOTP_DIGITS,
        NYA_CRYPTO_TOTP_STEP_S
    );

    if (written < 0 || (u64)written >= sizeof(out_enrolment->uri)) {
        return nya_error(NYA_ERROR_OUT_OF_MEMORY, "the enrolment URI does not fit " FMTu64 " bytes", (u64)sizeof(out_enrolment->uri));
    }

    for (u32 i = 0; i < NYA_HTTP_TOTP_RECOVERY_CODES; i++) {
        u8 bytes[NYA_HTTP_TOTP_RECOVERY_SECRET_BYTES] = { 0 };
        if (!nya_os_random_bytes(bytes, sizeof(bytes))) return nya_error(NYA_ERROR_NOT_OK, "the system random source failed");

        char plain[_NYA_HTTP_TOTP_RECOVERY_CHARACTERS + 1] = { 0 };
        NYA_Error encoded                                  = nya_crypto_base32_encode(bytes, sizeof(bytes), plain, sizeof(plain), &text_length);
        nya_crypto_wipe(bytes, sizeof(bytes));
        NYA_TRY(encoded);

        // hashed before the dash goes in, so the stored form is the normalised one a submission folds to.
        nya_crypto_blake2b(
            (const u8*)plain,
            (u64)_NYA_HTTP_TOTP_RECOVERY_CHARACTERS,
            out_enrolment->recovery_hash[i].bytes,
            sizeof(out_enrolment->recovery_hash[i].bytes)
        );

        // the dash is only there to be read off paper; nya_http_totp_recovery_redeem ignores it.
        for (u32 at = 0; at < NYA_HTTP_TOTP_RECOVERY_GROUP; at++) out_enrolment->recovery[i][at] = plain[at];
        out_enrolment->recovery[i][NYA_HTTP_TOTP_RECOVERY_GROUP] = '-';
        for (u32 at = NYA_HTTP_TOTP_RECOVERY_GROUP; at < _NYA_HTTP_TOTP_RECOVERY_CHARACTERS; at++) out_enrolment->recovery[i][at + 1] = plain[at];

        nya_crypto_wipe(plain, sizeof(plain));
    }

    return NYA_OK;
}

b8 _nya_http_totp_code_normalise(NYA_ConstCString code, OUT char out_code[NYA_CRYPTO_TOTP_CODE_BYTES]) {
    nya_memset(out_code, 0, NYA_CRYPTO_TOTP_CODE_BYTES);

    if (code == nullptr) return false;

    // a short submission stops at its own terminator, which is not a digit, so nothing reads past it.
    for (u32 i = 0; i < NYA_CRYPTO_TOTP_DIGITS; i++) {
        if (code[i] < '0' || code[i] > '9') return false;

        out_code[i] = code[i];
    }

    return code[NYA_CRYPTO_TOTP_DIGITS] == '\0';
}

b8 _nya_http_totp_recovery_hash(NYA_ConstCString code, OUT NYA_HttpTotpRecoveryHash* out_hash) {
    nya_memset(out_hash, 0, sizeof(*out_hash));

    if (code == nullptr) return false;

    char normalised[_NYA_HTTP_TOTP_RECOVERY_CHARACTERS] = { 0 };
    u32  collected                                      = 0;

    for (u32 i = 0; i < _NYA_HTTP_TOTP_SUBMISSION_MAX && code[i] != '\0'; i++) {
        char character = code[i];

        // typed off paper: the grouping dash is decoration and the alphabet has one case.
        if (character == '-') continue;
        if (character >= 'a' && character <= 'z') character = (char)(character - ('a' - 'A'));

        if (collected >= _NYA_HTTP_TOTP_RECOVERY_CHARACTERS) return false;

        normalised[collected] = character;
        collected++;
    }

    if (collected != _NYA_HTTP_TOTP_RECOVERY_CHARACTERS) return false;

    nya_crypto_blake2b((const u8*)normalised, sizeof(normalised), out_hash->bytes, sizeof(out_hash->bytes));
    nya_crypto_wipe(normalised, sizeof(normalised));

    return true;
}

__attr_no_sanitize("unsigned-integer-overflow") u64 _nya_http_totp_mask(b8 condition) {
    return (u64)0 - (u64)(condition != 0);
}

b8 _nya_http_totp_attempt(NYA_HttpTotpGuard* guard, u64 now_s) {
    /*
     * A window that has fully elapsed starts again, and so does one that appears to be in the future,
     * which is a clock that went backwards rather than a reason to lock an account out for ever.
     */
    b8 elapsed = now_s < guard->window_started_s || (now_s - guard->window_started_s) >= NYA_HTTP_TOTP_ATTEMPT_WINDOW_S;

    if (elapsed) {
        guard->window_started_s = now_s;
        guard->attempts         = 0;
    }

    if (guard->attempts >= NYA_HTTP_TOTP_ATTEMPTS_MAX) return false;

    guard->attempts++;

    return true;
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PUBLIC API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

NYA_Error nya_http_totp_enrol_create(NYA_ConstCString issuer, NYA_ConstCString account, OUT NYA_HttpTotpEnrolment* out_enrolment) {
    nya_assert(out_enrolment != nullptr);

    nya_memset(out_enrolment, 0, sizeof(*out_enrolment));

    NYA_Error result = _nya_http_totp_enrol_fill(issuer, account, out_enrolment);

    // a half built enrolment still holds a real secret, and a caller that ignored the error would
    // hand it out without the URI that goes with it.
    if (!result.ok) nya_http_totp_enrol_destroy(out_enrolment);

    return result;
}

void nya_http_totp_enrol_destroy(NYA_HttpTotpEnrolment* enrolment) {
    if (enrolment == nullptr) return;

    nya_crypto_wipe(enrolment, sizeof(*enrolment));
}

NYA_HttpTotpVerdict nya_http_totp_verify(NYA_HttpTotpGuard* guard, const NYA_CryptoTotpSecret* secret, NYA_ConstCString code, u64 now_s) {
    nya_assert(guard != nullptr);
    nya_assert(secret != nullptr);

    // spent before the code is read, so a malformed submission is not a free guess.
    if (!_nya_http_totp_attempt(guard, now_s)) return NYA_HTTP_TOTP_RATE_LIMITED;

    char submitted[NYA_CRYPTO_TOTP_CODE_BYTES] = { 0 };
    if (!_nya_http_totp_code_normalise(code, submitted)) return NYA_HTTP_TOTP_REFUSED;

    u64 now_counter = nya_crypto_totp_counter(now_s);
    u64 first       = now_counter > NYA_HTTP_TOTP_SKEW_STEPS ? now_counter - NYA_HTTP_TOTP_SKEW_STEPS : 0;
    u64 last        = now_counter + NYA_HTTP_TOTP_SKEW_STEPS;

    u64 matched = 0;
    b8  any     = false;

    for (u64 counter = first; counter <= last; counter++) {
        char expected[NYA_CRYPTO_TOTP_CODE_BYTES] = { 0 };
        nya_crypto_totp_code(secret->bytes, sizeof(secret->bytes), counter, expected);

        b8 same = nya_crypto_totp_code_equals(expected, submitted);

        // arithmetic rather than an if, so which of the three steps matched is not a branch either.
        u64 mask = _nya_http_totp_mask(same);
        matched  = (matched & ~mask) | (counter & mask);
        any      = any | same;

        nya_crypto_wipe(expected, sizeof(expected));
    }

    nya_crypto_wipe(submitted, sizeof(submitted));

    // one verdict for a wrong code and for a replayed one: a caller that could tell them apart would
    // be telling an attacker which of their guesses had once been somebody's real code.
    if (!any || matched <= guard->last_counter) return NYA_HTTP_TOTP_REFUSED;

    guard->last_counter = matched;

    // the limit is there for whoever cannot answer; whoever just did is not it.
    guard->attempts         = 0;
    guard->window_started_s = now_s;

    return NYA_HTTP_TOTP_ACCEPTED;
}

NYA_HttpTotpVerdict nya_http_totp_recovery_redeem(NYA_HttpTotpGuard* guard, NYA_HttpTotpRecoveryHash* hashes, u32 count, NYA_ConstCString code, u64 now_s) {
    nya_assert(guard != nullptr);
    nya_assert(hashes != nullptr || count == 0);

    if (!_nya_http_totp_attempt(guard, now_s)) return NYA_HTTP_TOTP_RATE_LIMITED;

    NYA_HttpTotpRecoveryHash submitted = { 0 };
    if (!_nya_http_totp_recovery_hash(code, &submitted)) return NYA_HTTP_TOTP_REFUSED;

    u32 matched = 0;
    b8  any     = false;

    for (u32 i = 0; i < count; i++) {
        b8 same = nya_crypto_equals(hashes[i].bytes, submitted.bytes, sizeof(submitted.bytes));

        u32 mask = (u32)_nya_http_totp_mask(same);
        matched  = (matched & ~mask) | (i & mask);
        any      = any | same;
    }

    nya_crypto_wipe(&submitted, sizeof(submitted));

    if (!any) return NYA_HTTP_TOTP_REFUSED;

    // spent in place: single use is not a step the caller can forget, only one it can fail to persist.
    nya_crypto_wipe(hashes[matched].bytes, sizeof(hashes[matched].bytes));

    guard->attempts         = 0;
    guard->window_started_s = now_s;

    return NYA_HTTP_TOTP_ACCEPTED;
}
