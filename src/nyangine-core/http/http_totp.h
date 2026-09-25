/**
 * @file http_totp.h
 *
 * The second factor as an account sees it: enrolling one authenticator, and answering one code.
 *
 * crypto_totp.h is the arithmetic. This is the policy around it — the enrolment a phone can scan, the
 * recovery codes for when the phone is gone, the skew a wall clock needs, the replay guard that makes
 * a code single use, and the limit that stops a million guesses.
 *
 * ```
 * nya_http_totp_enrol_create      a secret, the otpauth URI, and the recovery codes, all at once
 * nya_http_totp_enrol_destroy     wipes every one of those
 *
 * nya_http_totp_verify            one submitted code against one secret, through the guard
 * nya_http_totp_recovery_redeem   one submitted recovery code, spent where it matches
 *
 * NYA_HttpTotpSubmission          the body a verify route takes, every field of it `@redact`
 * NYA_HttpTotpEnrolmentDto        the body an enrol route answers with, likewise
 * ```
 *
 * ```c
 * NYA_HttpTotpEnrolment enrolment = { 0 };
 * NYA_TRY(nya_http_totp_enrol_create("gnyame", "luca", &enrolment));
 * defer nya_http_totp_enrol_destroy(&enrolment);
 *
 * // show enrolment.uri as a QR code and enrolment.secret_base32 to type; keep enrolment.secret.
 * // the factor is not on until one code verifies against it:
 * if (nya_http_totp_verify(&guard, &enrolment.secret, submitted, now_s) != NYA_HTTP_TOTP_ACCEPTED) { ... }
 * ```
 *
 * ── it stores nothing ──
 *
 * There is no user store in this engine yet and no login route; TODO.md has both open under "Web". So
 * nothing here reaches for storage it would have to invent. The caller hands in the secret and hands
 * in the guard — the last counter that was accepted and the attempts spent — and hands them back next
 * time, from wherever that caller keeps a user. NYA_HttpTotpGuard is plain data with no pointer in it,
 * so a caller can write it into a row, a `.nya` file or a static array and read it back.
 *
 * That also means the guard is only as good as the caller's persistence. A guard that is thrown away
 * between requests is a replay guard that forgets, and a rate limit that resets: both properties are
 * the caller's to keep, and both are stated on the fields.
 *
 * ── what an attacker gets ──
 *
 * From the enrolment: everything, so it exists for one response and is wiped. `secret`, `uri` and
 * `secret_base32` are three spellings of the same secret, `recovery` is the plaintext codes, and none
 * of the four may be logged, written to disk in the clear or put in an error message.
 *
 * From the guard: nothing worth having. It is a counter and two integers, with no secret in it, which
 * is why it can be stored beside a user and read back without care.
 *
 * From guessing: one in a million per code, and NYA_HTTP_TOTP_ATTEMPTS_MAX tries per window, so a
 * factor that stays enrolled for a year sees far fewer guesses than it has codes. From watching a
 * reply: which half of the login failed is not said — a wrong code, a replayed code and a code for an
 * account that never enrolled are one verdict with one message.
 *
 * ── what is still missing ──
 *
 * The response is not yet padded to a fixed time, so how long a refusal takes is still a signal about
 * how far it got; TODO.md's "constant_time" defence covers that and is not written. The rate limit
 * here is per guard, which is per account: it does not see an attacker spreading guesses over many
 * accounts from one address, which is the server's own bucket in http_server.h. And enrolment issues
 * recovery codes but nothing revokes or reissues them, because that wants a user store too.
 * */
#pragma once

#include "nyangine-std/base/base_attributes.h"
#include "nyangine-std/base/base_error.h"
#include "nyangine-std/base/base_types.h"
#include "nyangine-core/crypto/crypto_encoding.h"
#include "nyangine-core/crypto/crypto_totp.h"

// CONSTANTS

/** Longest issuer and account in an enrolment, terminator included. A program's name and a user's name. */
#define NYA_HTTP_TOTP_ISSUER_MAX  64
#define NYA_HTTP_TOTP_ACCOUNT_MAX 64

/** A twenty byte secret is exactly thirty two base32 characters, so the padding never appears. */
#define NYA_HTTP_TOTP_SECRET_TEXT_BYTES (NYA_CRYPTO_BASE32_LENGTH(NYA_CRYPTO_TOTP_SECRET_BYTES) + 1)

/**
 * Longest otpauth URI, terminator included.
 *
 * The worst case is both names at their bound with every byte percent encoded to three, twice for the
 * issuer since it appears in the label and again as a parameter: about 670 bytes. Rounded up, and
 * NYA_ERROR_OUT_OF_MEMORY rather than a truncated URI if that arithmetic is ever wrong.
 * */
#define NYA_HTTP_TOTP_URI_BYTES 768

/**
 * Recovery codes issued at enrolment, and the entropy in each.
 *
 * Ten is what a person can print on one line of paper and enough to survive losing a phone more than
 * once. Eighty bits is far past guessing and still only sixteen characters to type.
 * */
#define NYA_HTTP_TOTP_RECOVERY_CODES         10
#define NYA_HTTP_TOTP_RECOVERY_SECRET_BYTES  10
#define NYA_HTTP_TOTP_RECOVERY_GROUP         8
#define NYA_HTTP_TOTP_RECOVERY_TEXT_BYTES    (NYA_CRYPTO_BASE32_LENGTH(NYA_HTTP_TOTP_RECOVERY_SECRET_BYTES) + 2)
#define NYA_HTTP_TOTP_RECOVERY_HASH_BYTES    32

/**
 * How many steps either side of the current one a code is still accepted in.
 *
 * One, which RFC 6238 section 6 calls out as the sensible ceiling: it forgives a phone and a server
 * thirty seconds apart, and it widens the guessing target from one code to three rather than to the
 * five or seven a lazier window would.
 * */
#define NYA_HTTP_TOTP_SKEW_STEPS 1

/**
 * Attempts one account may spend on a second factor, and the seconds they are counted over.
 *
 * Five per minute against three live codes is a chance of about one in seventy thousand per minute of
 * sustained guessing, and it is still four more than a person who can read needs. The window is fixed
 * rather than a leaky bucket because it has to survive being written to storage and read back, and a
 * bucket's refill would need a clock reading the caller does not owe us.
 * */
#define NYA_HTTP_TOTP_ATTEMPTS_MAX     5
#define NYA_HTTP_TOTP_ATTEMPT_WINDOW_S 60

// TYPES

typedef enum NYA_HttpTotpVerdict          NYA_HttpTotpVerdict;
typedef struct NYA_HttpTotpRecoveryHash   NYA_HttpTotpRecoveryHash;
typedef struct NYA_HttpTotpEnrolment      NYA_HttpTotpEnrolment;
typedef struct NYA_HttpTotpGuard          NYA_HttpTotpGuard;
typedef struct NYA_HttpTotpSubmission     NYA_HttpTotpSubmission;
typedef struct NYA_HttpTotpRecoveryDto    NYA_HttpTotpRecoveryDto;
typedef struct NYA_HttpTotpEnrolmentDto   NYA_HttpTotpEnrolmentDto;

/**
 * What one attempt came to. Refusal is zero, so a verdict that was never assigned is a refusal and a
 * caller that forgets to check one has failed closed.
 * */
enum NYA_HttpTotpVerdict {
    /**
     * Not this account's code. Wrong digits, a code already used in its window, a code outside the
     * skew, a malformed submission: one value, because a caller that could tell them apart would tell
     * an attacker apart too.
     * */
    NYA_HTTP_TOTP_REFUSED = 0,

    /** The code was this account's, unused, and inside the window. The guard has been advanced. */
    NYA_HTTP_TOTP_ACCEPTED,

    /**
     * Too many attempts in this window, and the code was not looked at. Distinct from a refusal
     * because the answer differs — 429 with a Retry-After rather than 401 — and because it says
     * nothing about whether the code was right.
     * */
    NYA_HTTP_TOTP_RATE_LIMITED,
};

/**
 * One recovery code as it is stored: hashed, never the code itself.
 *
 * BLAKE2b-256 over the text, with no salt and no Argon2id. A recovery code is eighty bits straight
 * from the CSPRNG, not something a person chose, so there is no dictionary to run and nothing for a
 * slow hash to buy; paying Argon2id's memory per attempt would only make refusals a way to load the
 * server. A password, which a person does choose, is the opposite case and goes through crypto_kdf.h.
 * */
struct NYA_HttpTotpRecoveryHash {
    u8 bytes[NYA_HTTP_TOTP_RECOVERY_HASH_BYTES];
};

/**
 * Everything one enrolment produces, and all of it secret but the hashes.
 *
 * Built in one call and destroyed in the same scope: the URI, the base32 and the plaintext recovery
 * codes exist to go into exactly one response, and `secret` plus `recovery_hash` are what the caller
 * keeps. RFC 6238 enrolment is two steps, so a caller holds this as *pending* until one code verifies
 * against `secret`, and only then stores anything.
 * */
struct NYA_HttpTotpEnrolment {
    /** The shared secret. What the caller stores, and the only field that has to survive the response. */
    NYA_CryptoTotpSecret secret;

    /** The same secret as unpadded base32, for somebody typing it into an app by hand. */
    char secret_base32[NYA_HTTP_TOTP_SECRET_TEXT_BYTES];

    /** `otpauth://totp/...`, which is what a QR code on the enrolment page encodes. Carries the secret. */
    char uri[NYA_HTTP_TOTP_URI_BYTES];

    /** The recovery codes in the clear. Shown once, to this user, and never stored in this form. */
    char recovery[NYA_HTTP_TOTP_RECOVERY_CODES][NYA_HTTP_TOTP_RECOVERY_TEXT_BYTES];

    /** What the caller stores instead of the codes above, in the same order. */
    NYA_HttpTotpRecoveryHash recovery_hash[NYA_HTTP_TOTP_RECOVERY_CODES];
};

/**
 * What one account's second factor remembers between attempts. Plain data, no secret, caller stored.
 *
 * Zero is a valid starting value and means "enrolled, nothing used yet", which is why the sentinel for
 * the last counter is zero rather than a magic number: step zero was thirty seconds after the epoch.
 * */
struct NYA_HttpTotpGuard {
    /**
     * The step whose code was last accepted, or zero when none has been.
     *
     * This is the replay guard: a code is only accepted for a step strictly later than this one, so
     * the same code presented twice inside its thirty seconds is refused the second time, and so is a
     * code from the step before the one that was just used. **Persist it with the user.**
     * */
    u64 last_counter;

    /** Attempts spent in the window that began at `window_started_s`, counted whether they succeeded or not. */
    u32 attempts;

    /** When the current attempt window began, in seconds since the epoch. Zero before the first attempt. */
    u64 window_started_s;
};

// THE DTOs — what a second factor route takes and answers with; reflected for OpenAPI, tagged @redact so codes never reach a log.

// @reflect
/** One submitted code: six digits from an authenticator, or a recovery code off paper. */
struct NYA_HttpTotpSubmission {
    /** Long enough for either form. Tagged, because this is the guess itself. */
    char code[NYA_HTTP_TOTP_RECOVERY_TEXT_BYTES]; // @redact
};

// @reflect
/** One recovery code in an enrolment answer. A struct rather than a row of a `char[10][18]`, which reflection cannot describe. */
struct NYA_HttpTotpRecoveryDto {
    char code[NYA_HTTP_TOTP_RECOVERY_TEXT_BYTES]; // @redact
};

// @reflect
/**
 * Everything an enrolment shows once. Every field is the secret or is derived from it, so every field
 * is tagged: the caller receives all of it, because nya_http_response_reflect writes the answer, and a
 * log holds none of it, because nya_reflect_to_object_redacted writes that.
 * */
struct NYA_HttpTotpEnrolmentDto {
    /** `otpauth://totp/...`, which a QR code on the enrolment page encodes. Carries the secret. */
    char uri[NYA_HTTP_TOTP_URI_BYTES]; // @redact

    /** The same secret as unpadded base32, for somebody typing it into an app by hand. */
    char secret[NYA_HTTP_TOTP_SECRET_TEXT_BYTES]; // @redact

    /** Shown once, to this user, and never stored in this form. Tagged one level down, on the code itself. */
    NYA_HttpTotpRecoveryDto recovery[NYA_HTTP_TOTP_RECOVERY_CODES];
};

// FUNCTIONS

// ENROLMENT

/**
 * A fresh secret, the URI an authenticator scans, and NYA_HTTP_TOTP_RECOVERY_CODES recovery codes.
 *
 * `issuer` names the program and `account` the user; both go into the URI's label, which is what the
 * app shows in its list. NYA_ERROR_INVALID_ARGUMENT when either is empty, over its bound, or contains
 * a colon or a control character — a colon separates the two halves of the label, and an app that
 * splits on the first one would read a smuggled issuer as the account.
 *
 * NYA_ERROR_NOT_OK when the system random source fails, and NYA_ERROR_OUT_OF_MEMORY when the names
 * percent encode past NYA_HTTP_TOTP_URI_BYTES. `out_enrolment` is wiped on every failure, so a caller
 * that ignores the error still has nothing usable.
 * */
NYA_API NYA_Error nya_http_totp_enrol_create(NYA_ConstCString issuer, NYA_ConstCString account, OUT NYA_HttpTotpEnrolment* out_enrolment)
    __attr_no_discard;

/** Wipes the whole enrolment, secret, URI, base32 and codes alike. Safe on one never created. */
NYA_API void nya_http_totp_enrol_destroy(NYA_HttpTotpEnrolment* enrolment);

// VERIFICATION

/**
 * Whether `code` is this secret's, for the step `now_s` falls in or either neighbour.
 *
 * Every attempt costs the guard a token before the code is looked at, so a malformed submission is
 * not a free guess; an accepted code clears the window, since a user who just proved themselves is
 * not the attacker the limit is for. On acceptance the guard's last counter moves to the step that
 * matched, which is what makes a code single use.
 *
 * All three candidate codes are computed and compared whatever the first one says, and compared with
 * nya_crypto_totp_code_equals, so neither which step matched nor where a wrong code first differed is
 * visible in the time this takes.
 * */
NYA_API NYA_HttpTotpVerdict nya_http_totp_verify(NYA_HttpTotpGuard* guard, const NYA_CryptoTotpSecret* secret, NYA_ConstCString code, u64 now_s)
    __attr_no_discard;

/**
 * Whether `code` is one of `count` unspent recovery codes, and spends it if it is.
 *
 * Spending is zeroing the entry in place, so single use is not something a caller can forget to do —
 * but the caller does have to write `hashes` back to wherever it keeps them, or the code is live
 * again. Letters are read case insensitively and a dash may be left out, because these are typed off
 * paper; everything else about the text must match.
 *
 * Counted against the same guard as a code, so an attacker cannot spend the code limit and then guess
 * recovery codes for free. Every entry is compared whether an earlier one matched or not.
 * */
NYA_API NYA_HttpTotpVerdict
nya_http_totp_recovery_redeem(NYA_HttpTotpGuard* guard, NYA_HttpTotpRecoveryHash* hashes, u32 count, NYA_ConstCString code, u64 now_s) __attr_no_discard;
