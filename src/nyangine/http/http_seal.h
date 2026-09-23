/**
 * @file http_seal.h
 *
 * A little state, put in the client's hands and trusted when it comes back.
 *
 * ```c
 * // going out: the state, a label that says what it is, and how long it is good for
 * char token[NYA_HTTP_SEAL_MAX_TOKEN] = { 0 };
 * NYA_TRY(nya_http_seal(secret, secret_size, "session", (const u8*)&who, sizeof(who), 900, token, sizeof(token)));
 * // ... set it as a cookie value
 *
 * // coming back: the same label, or it does not open
 * Who who = { 0 };
 * u64 size = 0;
 * if (nya_http_unseal(secret, secret_size, "session", cookie.text, cookie.size, (u8*)&who, sizeof(who), &size)) serve(&who);
 * ```
 *
 * ── what a seal is for ──
 *
 * State the server would otherwise keep a row for — a theme, a wizard step, which of two experiments a
 * person is in, or the whole of a small session — carried by the client instead. The server keeps
 * nothing and still trusts what comes back, because the token is sealed with a key only the server
 * has: a client can hold it and send it, and cannot read it or change one byte of it without the
 * unseal failing.
 *
 * This is the primitive HTMX-and-a-cookie server-rendered apps are built on, and the thing that makes
 * "app state in a cookie" safe rather than the classic mistake — a cookie a client can edit, trusted
 * by a server that forgot it can.
 *
 * ── it is sealed, and it expires ──
 *
 * XChaCha20-Poly1305 over a key derived from the server's secret: encrypted, so the client cannot read
 * it, and authenticated, so a single altered byte — in the ciphertext, the nonce or the tag — makes it
 * not open. And every token carries an expiry the server chose, checked on the way in, so a token that
 * was copied off a wire or out of a log stops working on its own. A seal with no expiry is not offered,
 * because a sealed token that is good forever is a password that cannot be changed.
 *
 * ── the label, and why it is not optional ──
 *
 * The label is authenticated but not stored in the token, so the same bytes on the way in are what make
 * it open. It is what stops a token sealed for one thing being sent as another: a `theme` cookie pasted
 * into the `session` slot does not open, because it was sealed under `"theme"` and is being unsealed
 * under `"session"`. A program that used one label for everything would have thrown that away.
 *
 * ── the size, and what does not fit ──
 *
 * A cookie value is bounded (NYA_HTTP_MAX_COOKIE_VALUE) and a browser bounds the whole cookie at about
 * four kilobytes, so a seal holds NYA_HTTP_SEAL_MAX_PLAINTEXT bytes and no more — a few hundred. State
 * that does not fit is not a seal's to carry: that is what the session row and the database are for,
 * and a seal that held the id of one is the right way to reach them. A plaintext past the bound is
 * refused rather than truncated into a token that unseals to the wrong thing.
 *
 * ── the key ──
 *
 * The server's secret, the same one nya_http_jwt_* takes, and derived rather than used raw: a seal key
 * and a JWT key are different keys even from one secret, so a token of one kind can never be mistaken
 * for the other. A secret shorter than NYA_HTTP_SEAL_MIN_SECRET_BYTES is refused, because a short
 * secret is a key an attacker guesses rather than steals.
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

/** The shortest secret a seal accepts, in bytes. Sixteen: a key below this is guessed, not stolen. */
#define NYA_HTTP_SEAL_MIN_SECRET_BYTES 16

/**
 * Bytes of state one seal may carry.
 *
 * Chosen backward from the cookie value bound: the token is a nonce, a version byte, an expiry, the
 * plaintext and a tag, all base64url, and the whole of that has to fit NYA_HTTP_MAX_COOKIE_VALUE. What
 * is left for the plaintext after the fixed parts is this.
 * */
#define NYA_HTTP_SEAL_MAX_PLAINTEXT 300

/**
 * Bytes a sealed token takes as text, terminator included.
 *
 * The fixed overhead — a 24 byte nonce, a version byte, an 8 byte expiry, a 16 byte tag — plus the
 * plaintext, as base64url without padding, with room for the terminator. Under NYA_HTTP_MAX_COOKIE_VALUE
 * at the maximum plaintext, which is the whole point of the bound above.
 * */
#define NYA_HTTP_SEAL_MAX_TOKEN 512

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * FUNCTIONS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/**
 * Seals `plaintext` into `out_token`, good for `ttl_s` seconds and bound to `label`.
 *
 * `ttl_s` must be positive: a seal with no expiry is not offered, for the reason the header gives.
 * `label` is authenticated and must be the same string on the way back; it may not be empty, because a
 * seal that is bound to nothing is one that opens in any slot.
 *
 * Refuses a secret shorter than NYA_HTTP_SEAL_MIN_SECRET_BYTES, a plaintext larger than
 * NYA_HTTP_SEAL_MAX_PLAINTEXT, and a token buffer smaller than the token would be.
 * */
NYA_API NYA_Error nya_http_seal(
    const u8* secret, u64 secret_size, NYA_ConstCString label, const u8* plaintext, u64 plaintext_size, u64 ttl_s, OUT char* out_token, u64 capacity
) __attr_no_discard;

/**
 * Opens `token` into `out_plaintext` when it is a seal this server made, under this `label`, not yet
 * expired.
 *
 * False for anything that is not exactly that: a token altered by one byte, sealed under another label,
 * made with another secret, past its expiry, or too big for `out_plaintext`. A b8 rather than an
 * NYA_Error because a token that does not open is an expected input — a client sends an old or a
 * tampered one, and that is a fact to act on, not a failure worth a message. `out_size` is how many
 * bytes the plaintext was.
 *
 * The expiry is checked against nya_clock_get_timestamp_s, so a token outlives neither its `ttl_s` nor
 * a backward step of that clock past when it was made.
 * */
NYA_API b8 nya_http_unseal(
    const u8* secret, u64 secret_size, NYA_ConstCString label, const char* token, u64 token_size, OUT u8* out_plaintext, u64 capacity,
    OUT u64* out_size
) __attr_no_discard;
