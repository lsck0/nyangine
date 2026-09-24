/**
 * @file http_pow.h
 *
 * A proof-of-work wall in front of a route. The abuse layer for a server with no IP to rate-limit.
 *
 * ```
 * nya_http_pow_init    the store: the difficulty, the challenge TTL, its lock, and one ceiling
 * nya_http_layer_pow   the layer: issues a challenge, and verifies the solution on the way back
 * nya_http_pow_solve   finds a solution to a challenge, for a client and for a test
 * nya_http_pow_reset   forgets every spent nonce, for a test or a controlled restart
 * nya_http_pow_count   how many spent nonces are held, for the ceiling and a test
 * ```
 *
 * ── what it is for ──
 *
 * A public server behind Tor has no client IP to count against: every request arrives from the local
 * Tor daemon, so the per-address bounds http_server.h keeps all see one address and a token bucket
 * keyed on it protects nothing. What is left to make an abusive client pay is its own CPU, which is
 * what a proof of work charges: before a guarded route runs, the caller has to find a string whose
 * SHA-256, taken with a nonce the server chose, has a number of leading zero bits the server chose.
 * A browser finds one in a moment; a script hammering the route pays that moment every time.
 *
 * This is a wall, not an identity: it says "you spent some work", never "you are someone". Pair it with
 * the session and the second factor for who, and use this for how-often on the routes that have no
 * other brake — a sign-up, a form post, an expensive query.
 *
 * ── the challenge, and why the client cannot cook it ──
 *
 * A challenge is a random nonce and a difficulty, and it is handed to the client sealed (http_seal.h)
 * under the server's own secret. The seal is encrypted and authenticated, so the client holds the token
 * and sends it back without being able to read the nonce out of it or change one byte of it — including
 * the difficulty, which lives inside the seal and never in a header the client controls. A client that
 * lowered its own difficulty would be editing a sealed token, which does not open. The difficulty this
 * layer verifies against is always the server's, taken from the token it minted.
 *
 * The sealed plaintext is a fixed, unambiguous layout — a version byte, a difficulty byte, and the
 * nonce — so there is no concatenation a second reading could split differently. See NYA_HTTP_POW_SEALED.
 *
 * ── issue, then verify ──
 *
 * The layer does both, on the same route, the way HTTP Basic does with 401:
 *
 * - **No token, or no solution** → the layer answers `401 Unauthorized` WITHOUT running the chain, and
 *   attaches the challenge: `X-Pow-Challenge` (the nonce, base64url), `X-Pow-Difficulty` (the bits, for
 *   a client that wants to show a meter — it is not trusted on the way back), and `X-Pow-Token` (the
 *   sealed token to send back untouched). The body is a NYA_HttpProblem saying what to do.
 * - **A token and a solution** → the layer opens the token, recomputes `SHA-256(nonce || solution)`,
 *   and counts its leading zero bits. At or above the sealed difficulty, and the nonce not already
 *   spent, the chain runs. Below it, or a token that does not open (tampered, forged, or expired), the
 *   layer answers `401` with a fresh challenge — the honest refusal, and one the client can retry.
 * - **A solution header that is not valid base64url, or is too long** → `400 Bad Request`.
 *
 * So a route wrapped in this layer can answer with 400 and 401 on top of whatever its handler declares,
 * and — because a route's `statuses` covers its whole chain, see http_router.h — it must list both.
 *
 * ── single use ──
 *
 * A solved token could otherwise be replayed until it expired: the same nonce and the same solution
 * verify every time. So a nonce that passes is remembered in a small spent set until the challenge would
 * have expired anyway, and a second request carrying it is refused with a fresh challenge. This is the
 * same replay guard the TOTP factor keeps, met from the layer: work proven once buys one request. The
 * set is a fixed table of NYA_HTTP_POW_MAX_SPENT nonces, registered with nya_ceiling_register, swept by
 * the same TTL, and — like the idempotency store — locked, because the layer runs on every worker at
 * once. A server with no workers never inits the store; the lock is then null and does nothing.
 *
 * ── the key, and the difficulty ──
 *
 * The seal key is the exchange's own signing secret, the same one the sessions use, derived by http_seal
 * so a PoW token can never be mistaken for a session. A server with no secret cannot seal, so the layer
 * fails closed with a 503 rather than waving requests through. The difficulty is set once at init and
 * clamped to NYA_HTTP_POW_MAX_DIFFICULTY: it is a cost, and a cost nobody can pay is an outage.
 * */
#pragma once

#include "nyangine/base/base_arena.h"
#include "nyangine/base/base_attributes.h"
#include "nyangine/base/base_error.h"
#include "nyangine/base/base_types.h"
#include "nyangine/http/http_router.h"
#include "nyangine/http/http_types.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * CONSTANTS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/** Bytes of the random nonce the server puts in a challenge. Sixteen: a nonce guessed is a wall walked around. */
#define NYA_HTTP_POW_NONCE_BYTES 16

/** Bytes of the sealed plaintext: a version byte, a difficulty byte, and the nonce. Fixed, so it is unambiguous. */
#define NYA_HTTP_POW_SEALED (2 + NYA_HTTP_POW_NONCE_BYTES)

/** The version byte in the sealed plaintext, so a later layout can be told from this one rather than misread. */
#define NYA_HTTP_POW_VERSION 1

/**
 * The most leading-zero bits a difficulty may demand.
 *
 * Thirty-two is already ~4 billion hashes on average, well past a wall and into an outage; the clamp is
 * here so a misconfiguration cannot lock every client out. A SHA-256 is 32 bytes, so the field could
 * name up to 256, but nothing sane goes near it.
 * */
#define NYA_HTTP_POW_MAX_DIFFICULTY 32

/** The difficulty a store takes when its options leave it zero. About a million hashes: a blink, then a wall. */
#define NYA_HTTP_POW_DEFAULT_DIFFICULTY 20

/** How long a challenge is good for when the options leave the TTL zero. Long enough to solve, short enough not to hoard. */
#define NYA_HTTP_POW_DEFAULT_TTL_S 300

/** The longest solution suffix the layer will hash, in bytes. A found suffix is a handful of bytes; this is slack. */
#define NYA_HTTP_POW_MAX_SOLUTION 64

/** Spent nonces held at once. A slot per request in flight through the TTL window, and then some. */
#ifndef NYA_HTTP_POW_MAX_SPENT
#define NYA_HTTP_POW_MAX_SPENT 256
#endif

/** The seal label a PoW token is bound to, so a token of any other kind sent in its place does not open. */
#define NYA_HTTP_POW_LABEL "pow-challenge"

/** The nonce, base64url, on the response that issues a challenge. */
#define NYA_HTTP_POW_CHALLENGE_HEADER "X-Pow-Challenge"

/** The difficulty, on the response that issues a challenge. Informational: the layer trusts the sealed one, not this. */
#define NYA_HTTP_POW_DIFFICULTY_HEADER "X-Pow-Difficulty"

/** The sealed token, on the response that issues a challenge and on the request that answers it — sent back untouched. */
#define NYA_HTTP_POW_TOKEN_HEADER "X-Pow-Token"

/** The found suffix, base64url, on the request that answers a challenge. */
#define NYA_HTTP_POW_SOLUTION_HEADER "X-Pow-Solution"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * TYPES
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

typedef struct NYA_HttpPowOptions NYA_HttpPowOptions;

/** What the wall is shaped like. Every field has a usable default, so `{ 0 }` is the sane wall. */
struct NYA_HttpPowOptions {
    /** Leading zero bits a solution must have. Zero means NYA_HTTP_POW_DEFAULT_DIFFICULTY; clamped to the max. */
    u8 difficulty;

    /** How long a challenge is good for, in seconds. Zero means NYA_HTTP_POW_DEFAULT_TTL_S. */
    u64 ttl_s;
};

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * FUNCTIONS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/**
 * Readies the one store the layer uses: sets its difficulty and TTL, makes its spent-nonce lock out of
 * `arena`, and registers its ceiling once.
 *
 * A server with workers has to call this before installing the layer, so the shared spent set is locked;
 * a server with none may skip it, and then the lock is null and does nothing. Calling it twice keeps the
 * first lock and only updates the difficulty and TTL, so a reload cannot leak one.
 * */
NYA_API NYA_Error _nya_http_pow_init(NYA_Arena* arena, NYA_HttpPowOptions options) __attr_no_discard;

/** Takes the options by name. */
#define nya_http_pow_init(arena, ...) _nya_http_pow_init((arena), (NYA_HttpPowOptions){ __VA_ARGS__ })

/**
 * Forgets every spent nonce and gives the lock back to its arena. The ceiling registration is kept — it
 * names the static count, which does not move — so a later init re-arms the store rather than registering
 * a second time.
 * */
NYA_API void nya_http_pow_deinit(void);

/**
 * The wall. Issues a challenge to a request that carries no valid solution, and runs the chain for one
 * that does. See the file note for the full table; the short of it is a 401 with a challenge, or through.
 *
 * Reads the exchange's `now_s` as its clock, so a test drives expiry by advancing that field rather than
 * by sleeping, and seals with the exchange's own secret. Can answer 400, 401 or 503 on its own; otherwise
 * it returns whatever the chain did.
 * */
NYA_API NYA_HttpStatus nya_http_layer_pow(NYA_HttpExchange* exchange, NYA_HttpChain* next);

/**
 * Finds a solution: a suffix whose `SHA-256(nonce || suffix)` has at least `difficulty` leading zero
 * bits. What a client does in the browser, and what a test does to prove the layer accepts a real one.
 *
 * Deterministic — it walks an eight-byte counter from zero — so the same challenge always yields the same
 * suffix, which is what lets a test assert on it without a clock or a race. Returns the suffix in
 * `out_solution` and its length in `out_size`. False only for a difficulty past NYA_HTTP_POW_MAX_DIFFICULTY
 * or a buffer too small for the counter, neither of which a caller in this tree hits.
 * */
NYA_API b8 nya_http_pow_solve(const u8* nonce, u64 nonce_size, u8 difficulty, OUT u8* out_solution, u64 capacity, OUT u64* out_size)
    __attr_no_discard;

/**
 * Leading zero bits of a 32-byte SHA-256 digest, counted from the most significant bit of the first byte.
 * The measure the difficulty is in, exposed so a client and a test count it exactly as the layer does.
 * */
NYA_API u32 nya_http_pow_leading_zero_bits(const u8* digest, u64 size) __attr_no_discard;

/** Forgets every spent nonce, keeping the lock and the ceiling registration. For a test or a restart. */
NYA_API void nya_http_pow_reset(void);

/** How many spent nonces are held right now, which is the ceiling's live count. */
NYA_API u32 nya_http_pow_count(void) __attr_no_discard;
