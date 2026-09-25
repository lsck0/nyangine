/**
 * @file accounts_session.h
 *
 * The fact that somebody is logged in right now, and every way that ends.
 *
 * ```c
 * NYA_AccountSession session = { 0 };
 * NYA_TRY(nya_account_session_issue(arena, user.id, "203.0.113.9", "a browser", &session));
 *
 * // the token is the only secret, and this is the only moment it exists in full
 * send_cookie(session.token);
 *
 * // later, on another request
 * NYA_AccountSession found = { 0 };
 * if (nya_account_session_validate(arena, token, &found).ok) serve(found.user_id);
 * ```
 *
 * ── the token is not in the database ──
 *
 * What is stored is a hash of it. A session token is a password that this program issued, so the
 * reason not to store a password applies unchanged: a database that leaks hands over every live
 * session otherwise, and the ability to impersonate everybody is worse than the ability to guess at
 * their passwords.
 *
 * SHA-256 rather than Argon2id for this one, and the difference is the point: a password is short and
 * chosen by a person, so it must be slow to guess. A token is 256 bits from the system random source,
 * so there is nothing to guess, and a validation that cost 30 ms would be a validation on every
 * request.
 *
 * ── what ends a session ──
 *
 * Its own expiry, its user changing their password, its user being disabled, somebody revoking it by
 * id, or the user revoking all of them. Every one of those is a row change rather than a memory one,
 * so it holds across a restart and across a second process — which is the whole reason sessions are
 * in the database rather than in a table in a server's memory.
 *
 * ── what this does not do ──
 *
 * Decide how a token travels. A cookie, a bearer token and a control socket's handshake are three
 * programs' answers to that, and http_auth.h already has the cookie half. This module answers "is
 * this token a live session, and whose".
 * */
#pragma once

#include "nyangine-std/base/base_arena.h"
#include "nyangine-std/base/base_attributes.h"
#include "nyangine-std/base/base_error.h"
#include "nyangine-std/base/base_types.h"

// CONSTANTS

/**
 * Bytes a session token takes as text, terminator included.
 *
 * 32 random bytes as base64url is 43 characters. The bound is what a cookie, a header and a column
 * all have to hold, so it is stated once here.
 * */
#define NYA_ACCOUNTS_TOKEN_TEXT_BYTES 64

/** Bytes of entropy behind a token. 256 bits: not guessable, and the same size as the hash of it. */
#define NYA_ACCOUNTS_TOKEN_BYTES 32

/** Bytes the stored hash of a token takes as text, terminator included: SHA-256 as hex. */
#define NYA_ACCOUNTS_TOKEN_HASH_BYTES 72

/** Bytes of the address and the agent a session records, terminator included. */
#define NYA_ACCOUNTS_MAX_ADDRESS 64
#define NYA_ACCOUNTS_MAX_AGENT   128

/**
 * How long a session lives without being used, in seconds.
 *
 * Fourteen days is what a person expects of "keep me logged in" on a machine they own, and it is the
 * bound that actually matters: every use pushes it out, so this is how long an abandoned session
 * stays useful to whoever finds it.
 * */
#define NYA_ACCOUNTS_SESSION_IDLE_S (14ULL * 24 * 60 * 60)

/**
 * How long a session lives at all, in seconds, however much it is used.
 *
 * Ninety days, so a token that was stolen and quietly used forever is not a thing that exists. A
 * person who is still there logs in again twice a year.
 * */
#define NYA_ACCOUNTS_SESSION_ABSOLUTE_S (90ULL * 24 * 60 * 60)

/** Sessions one user may hold at once. Past it the oldest is ended, so a login always works. */
#define NYA_ACCOUNTS_MAX_SESSIONS_PER_USER 16

// TYPES

typedef struct NYA_AccountSession NYA_AccountSession;

/**
 * One session, as it is stored — with one field that is not.
 *
 * `token` is filled in only by nya_account_session_issue, is never read back from the database, and is
 * `@skip`ped so the ORM never makes a column for it. That is the difference between a token and its
 * hash said in the type itself.
 * */
// @reflect
struct NYA_AccountSession {
    u64 id; // @key

    /** Whose session this is. */
    u64 user_id;

    /** SHA-256 of the token, as hex. What a validation looks up; see the header. */
    char token_hash[NYA_ACCOUNTS_TOKEN_HASH_BYTES]; // @redact

    /**
     * SHA-256 of the token this session held before its last rotation, or empty for one never rotated.
     *
     * The reuse tripwire: a rotation retires the old token and remembers its hash here. A retired token
     * presented again is a stolen one — the legitimate client rotated past it — so seeing it revokes the
     * whole session. See nya_account_session_rotate.
     * */
    char previous_hash[NYA_ACCOUNTS_TOKEN_HASH_BYTES]; // @redact

    /** Where it was opened from and what opened it, for the list a person sees of their own sessions. */
    char address[NYA_ACCOUNTS_MAX_ADDRESS];
    char agent[NYA_ACCOUNTS_MAX_AGENT];

    /** Seconds since the epoch: when it started, when it was last used, and when it stops being valid. */
    u64 created_at_s;
    u64 used_at_s;
    u64 expires_at_s;

    /** Ended by somebody rather than by time. A revoked row is kept so the list can say it happened. */
    b8 revoked;

    /**
     * The token itself, and only in the answer to nya_account_session_issue.
     *
     * `@skip` keeps it out of the table, `@redact` keeps it out of a log, and both say the same thing
     * about the one moment it exists.
     * */
    char token[NYA_ACCOUNTS_TOKEN_TEXT_BYTES]; // @skip @redact
};

// FUNCTIONS

/**
 * Opens a session for a user and answers it, with the token filled in.
 *
 * This is the only moment the token exists: what is stored is its hash, and nothing can produce the
 * token again. A caller that loses it has to open another session.
 *
 * `address` and `agent` are what the session list shows a person later; either may be null. Past
 * NYA_ACCOUNTS_MAX_SESSIONS_PER_USER the oldest session is ended rather than this one being refused,
 * because a login that fails because of an old browser somewhere is a login that looks broken.
 * */
NYA_API NYA_Error nya_account_session_issue(NYA_Arena* arena, u64 user_id, NYA_ConstCString address, NYA_ConstCString agent,
                                           OUT NYA_AccountSession* out_session) __attr_no_discard;

/**
 * Whether `token` is a live session, and whose.
 *
 * Pushes the idle expiry out, which is what makes a session that is in use stay alive and one that is
 * not stop. NYA_ERROR_PERMISSION_DENIED for a token that is not a session, one that is revoked, one
 * that has expired either way, and one whose user has been disabled — one answer, for the reason
 * accounts.h gives.
 *
 * The answer never carries the token back: `token` in the out parameter stays empty.
 * */
NYA_API NYA_Error nya_account_session_validate(NYA_Arena* arena, NYA_ConstCString token, OUT NYA_AccountSession* out_session) __attr_no_discard;

/**
 * Exchanges a live token for a fresh one on the same session, and detects a stolen one.
 *
 * This is the refresh, and it is where a long-lived session is made safe. Every use of it retires the
 * token that came in and issues a new one for the same row, so a token spends only the moment between
 * two refreshes on the wire. `out_session` carries the new token, exactly as issuing does.
 *
 * The reuse check is the point. If the token presented is one that was *already* rotated away from —
 * the legitimate client has since moved on to a newer one — then two parties hold tokens for this
 * session, which only happens when one was stolen. There is no telling which party is the thief, so the
 * whole session is revoked: the real user is logged out and signs in again, and the thief's copy dies
 * with it. NYA_ERROR_PERMISSION_DENIED, in the same words as every other refusal, and the session ended.
 *
 * A token that is not this session's current or just-previous one, an expired session, or a disabled
 * user are the ordinary refusal, with nothing revoked.
 * */
NYA_API NYA_Error nya_account_session_rotate(
    NYA_Arena* arena, NYA_ConstCString token, NYA_ConstCString address, NYA_ConstCString agent, OUT NYA_AccountSession* out_session
) __attr_no_discard;

/** Ends one session. Idempotent: revoking a session that is already revoked is not an error. */
NYA_API NYA_Error nya_account_session_revoke(NYA_Arena* arena, u64 session_id) __attr_no_discard;

/**
 * Ends every session a user has, and answers how many that was.
 *
 * What a password change does, what disabling an account does, and what "log me out everywhere" is.
 * */
NYA_API NYA_Error nya_account_session_revoke_all(NYA_Arena* arena, u64 user_id, OUT u32* out_ended) __attr_no_discard;

/**
 * The sessions a user holds, newest first, for the list they are shown of their own.
 *
 * Revoked and expired ones are included: "this session was ended on Tuesday" is the sentence that
 * makes the list worth showing. `out_sessions` points into `arena`.
 * */
NYA_API NYA_Error nya_account_session_list(NYA_Arena* arena, u64 user_id, OUT NYA_AccountSession** out_sessions, OUT u32* out_count) __attr_no_discard;

/**
 * Deletes every session row a user has, live ones included, and answers how many.
 *
 * What nya_account_destroy does on the way out, and not what "log me out everywhere" is: a revoked
 * row is kept so a list can say it was ended, and this leaves nothing to say anything about.
 * */
NYA_API NYA_Error nya_account_session_purge(NYA_Arena* arena, u64 user_id, OUT u32* out_removed) __attr_no_discard;

/**
 * Deletes the sessions that ended more than `keep_for_s` seconds ago, and answers how many.
 *
 * A row nobody will ever look at again, removed on a schedule a program picks. Not automatic: a
 * deletion that happened inside a validation would be a request paying for somebody else's tidying.
 * */
NYA_API NYA_Error nya_account_session_prune(NYA_Arena* arena, u64 keep_for_s, OUT u32* out_removed) __attr_no_discard;

/** How many revoked sessions are kept per user before the oldest are deleted. See nya_account_session_sweep. */
#define NYA_ACCOUNTS_SESSION_KEEP_REVOKED 100

/**
 * The housekeeping a server runs on a timer: end the abandoned, and forget the long dead.
 *
 * Two things the reference's database triggers did and this does on a schedule, because SQLCipher has
 * no scheduler. A session unused for longer than NYA_ACCOUNTS_SESSION_IDLE_S is already invalid by its
 * expiry, but its row lingers; this revokes it so a list stops calling it live. And a user's revoked
 * rows are kept only NYA_ACCOUNTS_SESSION_KEEP_REVOKED deep — a person with years of logins does not
 * grow an unbounded table — the oldest beyond that deleted outright.
 *
 * `out_ended` is how many were newly revoked, `out_removed` how many rows were deleted. A program calls
 * this from a timer, never from a request: a login must not pay for the tidying of accounts it will
 * never touch.
 * */
NYA_API NYA_Error nya_account_session_sweep(NYA_Arena* arena, OUT u32* out_ended, OUT u32* out_removed) __attr_no_discard;
