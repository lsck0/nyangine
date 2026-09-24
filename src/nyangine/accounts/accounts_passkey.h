/**
 * @file accounts_passkey.h
 *
 * A passkey as a second factor: a private key the person's device holds and this server never sees.
 *
 * ```c
 * // ── enrolling, while the person is already logged in ──
 * NYA_AccountPasskeyChallenge challenge = { 0 };
 * NYA_TRY(nya_account_passkey_register_begin(arena, user.id, &challenge));
 * // send challenge.challenge (and the RP id, and the user handle) to navigator.credentials.create
 *
 * // the browser answers a clientDataJSON and an attestationObject; hand both back
 * NYA_AccountPasskey stored = { 0 };
 * NYA_AccountPasskeyRegistration reg = {
 *     .rp_id = "example.com", .origin = "https://example.com",
 *     .client_data_json = body_a, .client_data_json_size = size_a,
 *     .attestation_object = body_b, .attestation_object_size = size_b, .name = "my phone",
 * };
 * NYA_TRY(nya_account_passkey_register_finish(arena, user.id, &reg, &stored));
 *
 * // ── logging in, as the second step after the password ──
 * NYA_AccountPasskeyChallenge login = { 0 };
 * NYA_TRY(nya_account_passkey_assert_begin(arena, user.id, &login));
 * // send login.challenge and the ids from nya_account_passkey_list to navigator.credentials.get
 *
 * NYA_AccountPasskeyAssertion got = { ... clientDataJSON, authenticatorData, signature, the credential id ... };
 * if (nya_account_passkey_assert_finish(arena, user.id, &got, nullptr).ok) grant_the_session();
 * ```
 *
 * ── what a passkey is, and why it is the strongest of the three factors ──
 *
 * A TOTP secret and a PGP key are secrets a server helped make and could, in the wrong design, keep. A
 * passkey is a key pair the person's own authenticator generates and never lets leave: the server is
 * told the *public* half at enrolment and nothing else, and every login is the authenticator proving it
 * still holds the private half by signing a challenge. There is nothing here for a database leak to
 * hand over — a stolen public key logs nobody in — and nothing for a phishing page to capture, because
 * the signature is bound to the origin the browser is actually on. That last part is the point TOTP
 * cannot match: a code typed into a lookalike site works there, a passkey signature does not.
 *
 * ── the challenge is the whole security, so it is server-made, single-use, and short-lived ──
 *
 * Both begin calls mint random bytes from the system source, bind them to the one user, and store them
 * with a short expiry. The matching finish call looks the challenge up by its bytes and *deletes the row*
 * the moment it matches, so a challenge works exactly once. A challenge a client chose, or reused, or
 * replayed after its expiry, is the classic way this is got wrong; none of them is possible here.
 *
 * ── one curve, on purpose ──
 *
 * A WebAuthn assertion is signed with the credential's algorithm, and the two the platform authenticators
 * offer are ES256 (ECDSA over NIST P-256, COSE -7) and EdDSA (Ed25519, COSE -8). The crypto this engine
 * vendors — monocypher — does Ed25519 and does not do P-256, so this accepts an Ed25519 credential and
 * refuses an ES256 one with NYA_ERROR_NOT_SUPPORTED rather than pretend to verify a curve it has no code
 * for. A relying party sets `pubKeyCredParams` to `[-8]` so a compliant client only ever offers Ed25519;
 * the refusal is there for the client that ignores that. Verifying ES256 needs a P-256 verifier this does
 * not have yet.
 *
 * ── the public key is stored, and it is the only key a verify ever uses ──
 *
 * The signature is checked against the key that was written at enrolment, never a key read out of the
 * response being checked — a response that carried its own key would be a response that verified itself.
 * The counter each authenticator reports is stored too and must climb on every use: a counter that
 * repeated or went backward is the signature of a cloned authenticator, and it ends the assertion.
 *
 * ── this is a factor, not a session ──
 *
 * A passed assertion proves the person holds the device. Turning that into a session — issuing a token,
 * setting a cookie — is accounts_session.h's, behind whatever the program's login flow does. This module
 * answers "did this device sign this server's challenge just now", the same way accounts_recovery.h
 * answers "is this code one of theirs".
 *
 * Thread safety: none, as with everything over `db`.
 * */
#pragma once

#include "nyangine/accounts/accounts_user.h"
#include "nyangine/base/base_arena.h"
#include "nyangine/base/base_attributes.h"
#include "nyangine/base/base_error.h"
#include "nyangine/base/base_types.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * CONSTANTS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/** Bytes of entropy behind a challenge. Thirty-two, the size WebAuthn recommends and a session token uses. */
#define NYA_ACCOUNTS_PASSKEY_CHALLENGE_BYTES 32

/** Bytes a challenge takes as text, terminator included: 32 bytes as base64url without padding is 43 characters. */
#define NYA_ACCOUNTS_PASSKEY_CHALLENGE_TEXT 45

/** Bytes of an Ed25519 public key: the `x` coordinate of the COSE OKP key, and nothing else for this curve. */
#define NYA_ACCOUNTS_PASSKEY_PUBLIC_KEY_BYTES 32

/** Bytes the stored public key takes as text, terminator included: 32 bytes as base64url. */
#define NYA_ACCOUNTS_PASSKEY_PUBLIC_KEY_TEXT 45

/**
 * The largest raw credential id this accepts, in bytes.
 *
 * WebAuthn says an authenticator should not make one longer than 1023 bytes; real ones are 16 to about
 * 128. Two hundred and fifty-six is well past every authenticator in use and still a bounded column, and
 * a credential id past it is refused rather than stored truncated into one that never matches again.
 * */
#define NYA_ACCOUNTS_PASSKEY_CRED_ID_MAX_BYTES 256

/** Bytes a credential id takes as text, terminator included: NYA_ACCOUNTS_PASSKEY_CRED_ID_MAX_BYTES as base64url. */
#define NYA_ACCOUNTS_PASSKEY_CRED_ID_TEXT 344

/** Bytes of the label a person may put on a credential, terminator included. A display detail, never a secret. */
#define NYA_ACCOUNTS_PASSKEY_NAME_TEXT 64

/**
 * The most bytes any one part of a WebAuthn response may be.
 *
 * A clientDataJSON, an attestation object and an authenticator data are each bounded here before a byte is
 * parsed, so a hostile client cannot make the server allocate or hash without limit. Four kilobytes is far
 * past any honest response — a real attestation object with no attestation statement is a few hundred bytes.
 * */
#define NYA_ACCOUNTS_PASSKEY_MAX_INPUT_BYTES 4096

/** How long a challenge is good for, in seconds. Five minutes: long enough for a person to reach for a device. */
#define NYA_ACCOUNTS_PASSKEY_CHALLENGE_TTL_S (5ULL * 60)

/** Passkeys one user may hold at once. Past it the oldest is dropped, so enrolling a new device always works. */
#define NYA_ACCOUNTS_MAX_PASSKEYS_PER_USER 16

/**
 * What a challenge is for, so a registration challenge cannot be spent as a login one.
 *
 * Stored on the row and matched on the way back, the same idea as a seal's label: a challenge minted for
 * `navigator.credentials.create` does not open a `navigator.credentials.get`, because the purpose it was
 * bound to is part of what the finish looks up.
 * */
#define NYA_ACCOUNTS_PASSKEY_PURPOSE_REGISTER 0
#define NYA_ACCOUNTS_PASSKEY_PURPOSE_ASSERT   1

/**
 * The COSE algorithm identifiers this understands.
 *
 * EdDSA is the one it verifies; ES256 is named only so it can be refused by name rather than as an unknown
 * number. Both are negative because COSE puts signature algorithms in the negative label space.
 * */
#define NYA_ACCOUNTS_PASSKEY_COSE_ALG_EDDSA (-8)
#define NYA_ACCOUNTS_PASSKEY_COSE_ALG_ES256 (-7)

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * TYPES
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

typedef struct NYA_AccountPasskey          NYA_AccountPasskey;
typedef struct NYA_AccountPasskeyChallenge NYA_AccountPasskeyChallenge;
typedef struct NYA_AccountPasskeyRegistration NYA_AccountPasskeyRegistration;
typedef struct NYA_AccountPasskeyAssertion    NYA_AccountPasskeyAssertion;

/**
 * One enrolled credential, as it is stored. There is no secret on it: the public key is public by design.
 *
 * Public because the ORM derives its table from the reflection, which the generator only makes for a type
 * it can see in a header.
 * */
// @reflect
struct NYA_AccountPasskey {
    u64 id; // @key

    /** Whose credential this is. */
    u64 user_id;

    /** The authenticator's own id for this credential, as base64url — what a login says it is signing with. */
    char credential_id[NYA_ACCOUNTS_PASSKEY_CRED_ID_TEXT];

    /** The Ed25519 public key, as base64url. The only key a verify ever uses; see the header. */
    char public_key[NYA_ACCOUNTS_PASSKEY_PUBLIC_KEY_TEXT];

    /** The COSE algorithm the credential signs with. NYA_ACCOUNTS_PASSKEY_COSE_ALG_EDDSA for everything stored. */
    s64 algorithm;

    /** The authenticator's last reported signature counter. Must climb on every use, or the credential is a clone. */
    s64 sign_count;

    /** A label a person gives the device, for the list they are shown. A display detail, never anything checked. */
    char name[NYA_ACCOUNTS_PASSKEY_NAME_TEXT];

    /** Seconds since the epoch: when it was enrolled, and when it last signed a login. */
    u64 created_at_s;
    u64 used_at_s;
};

/**
 * One outstanding challenge, bound to a user and a purpose, deleted the moment it is spent.
 *
 * Public for the same reason as above. The challenge itself is not a secret — it is sent to the client —
 * but it is single-use and short-lived, which is what a replay has to get past and cannot.
 * */
// @reflect
struct NYA_AccountPasskeyChallenge {
    u64 id; // @key

    /** Whose challenge this is: a challenge minted for one user does not verify another's assertion. */
    u64 user_id;

    /** NYA_ACCOUNTS_PASSKEY_PURPOSE_REGISTER or _ASSERT: what the challenge may be spent on. */
    s64 purpose;

    /** The random bytes, as base64url without padding — exactly the form the client echoes in clientDataJSON. */
    char challenge[NYA_ACCOUNTS_PASSKEY_CHALLENGE_TEXT];

    /** Seconds since the epoch: when it was minted, and the instant past which it no longer opens. */
    u64 created_at_s;
    u64 expires_at_s;
};

/** The parts of a `navigator.credentials.create` response a registration is checked against. */
struct NYA_AccountPasskeyRegistration {
    /** The relying party id (the effective domain, e.g. "example.com") and origin (e.g. "https://example.com"). */
    NYA_ConstCString rp_id;
    NYA_ConstCString origin;

    /** The clientDataJSON bytes the authenticator signed over, exactly as they arrived. */
    const u8* client_data_json;
    u64       client_data_json_size;

    /** The attestationObject: the CBOR carrying the authenticator data and the new credential's public key. */
    const u8* attestation_object;
    u64       attestation_object_size;

    /** An optional label to store with the credential; may be null. */
    NYA_ConstCString name;
};

/** The parts of a `navigator.credentials.get` response an assertion is verified from. */
struct NYA_AccountPasskeyAssertion {
    /** The relying party id and origin, checked exactly as at registration. */
    NYA_ConstCString rp_id;
    NYA_ConstCString origin;

    /** The credential id the client says it used, as base64url — what names which stored key to verify against. */
    NYA_ConstCString credential_id;

    /** The clientDataJSON bytes, exactly as they arrived. */
    const u8* client_data_json;
    u64       client_data_json_size;

    /** The authenticatorData bytes, exactly as they arrived. */
    const u8* authenticator_data;
    u64       authenticator_data_size;

    /** The raw Ed25519 signature over `authenticatorData || SHA-256(clientDataJSON)`. Sixty-four bytes. */
    const u8* signature;
    u64       signature_size;
};

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * FUNCTIONS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/**
 * Mints a registration challenge for a user and answers it.
 *
 * Random, bound to this user, good for NYA_ACCOUNTS_PASSKEY_CHALLENGE_TTL_S, and stored so the matching
 * finish can find and spend it. The user has to exist and be enabled, for the reason a session does. The
 * caller sends `out_challenge->challenge` to `navigator.credentials.create` as the challenge.
 * */
NYA_API NYA_Error nya_account_passkey_register_begin(NYA_Arena* arena, u64 user_id, OUT NYA_AccountPasskeyChallenge* out_challenge)
    __attr_no_discard;

/**
 * Checks a `navigator.credentials.create` response and, if it holds together, stores the new credential.
 *
 * Verifies, in order: the clientDataJSON is a `webauthn.create` whose challenge is one this server minted
 * for this user and has not expired (and the row is deleted, so it is spent); the origin is the one given;
 * the authenticator data's RP id hash is SHA-256 of `rp_id` and its user-present flag is set; and the COSE
 * key is an Ed25519 one. The credential id and public key are then stored with the counter the response
 * reported.
 *
 * NYA_ERROR_NOT_SUPPORTED for an ES256 (COSE -7) credential — see the header. NYA_ERROR_ALREADY_EXISTS for
 * a credential id already enrolled. NYA_ERROR_PARSE for an attestation object or authenticator data that is
 * malformed or truncated. NYA_ERROR_PERMISSION_DENIED for a challenge, origin or RP id hash that does not
 * match. `out_passkey` is the stored row and may be null.
 * */
NYA_API NYA_Error
nya_account_passkey_register_finish(NYA_Arena* arena, u64 user_id, const NYA_AccountPasskeyRegistration* request, OUT NYA_AccountPasskey* out_passkey)
    __attr_no_discard;

/**
 * Mints an assertion (login) challenge for a user and answers it.
 *
 * As nya_account_passkey_register_begin, bound to the assert purpose so it cannot be spent as a
 * registration. The caller sends `out_challenge->challenge` and the ids from nya_account_passkey_list to
 * `navigator.credentials.get`.
 * */
NYA_API NYA_Error nya_account_passkey_assert_begin(NYA_Arena* arena, u64 user_id, OUT NYA_AccountPasskeyChallenge* out_challenge)
    __attr_no_discard;

/**
 * Verifies a `navigator.credentials.get` response against the user's stored credential.
 *
 * Finds the credential named by `request->credential_id` among this user's; checks the clientDataJSON is a
 * `webauthn.get` whose challenge this server minted for this user and has not expired (and spends it); checks
 * the origin, the authenticator data's RP id hash and user-present flag; verifies the Ed25519 signature over
 * `authenticatorData || SHA-256(clientDataJSON)` with the *stored* public key; and requires the reported
 * signature counter to be above the stored one (a counter that did not climb is a cloned authenticator). On
 * success the stored counter and last-used time move forward.
 *
 * NYA_ERROR_PERMISSION_DENIED for every way the assertion is not valid — an unknown credential, a bad
 * challenge, origin or RP id hash, a user-present flag that is clear, a signature that does not verify, or a
 * counter that did not climb. NYA_ERROR_PARSE for authenticator data that is malformed or truncated.
 * `out_passkey` is the credential that verified and may be null.
 * */
NYA_API NYA_Error
nya_account_passkey_assert_finish(NYA_Arena* arena, u64 user_id, const NYA_AccountPasskeyAssertion* request, OUT NYA_AccountPasskey* out_passkey)
    __attr_no_discard;

/**
 * The credentials a user has enrolled, newest first, for the list they are shown and for a login's
 * `allowCredentials`. `out_passkeys` points into `arena`.
 * */
NYA_API NYA_Error nya_account_passkey_list(NYA_Arena* arena, u64 user_id, OUT NYA_AccountPasskey** out_passkeys, OUT u32* out_count)
    __attr_no_discard;

/** Whether a user has any passkey enrolled, for a program deciding whether it may require the factor. */
NYA_API NYA_Error nya_account_passkey_has(NYA_Arena* arena, u64 user_id, OUT b8* out_has) __attr_no_discard;

/** Removes one of a user's credentials by id. Refuses a credential that is not this user's, so nobody drops another's. */
NYA_API NYA_Error nya_account_passkey_remove(NYA_Arena* arena, u64 user_id, u64 passkey_id) __attr_no_discard;

/**
 * Deletes the challenges that expired more than `keep_for_s` seconds ago, and answers how many.
 *
 * Spent challenges are deleted the moment they are used; this is for the ones that were minted and never
 * answered. A program runs it from a timer, never from a request, for the reason accounts_session.h's sweep
 * gives.
 * */
NYA_API NYA_Error nya_account_passkey_challenge_prune(NYA_Arena* arena, u64 keep_for_s, OUT u32* out_removed) __attr_no_discard;
