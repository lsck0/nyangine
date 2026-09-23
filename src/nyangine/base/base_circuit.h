/**
 * @file base_circuit.h
 *
 * When to stop calling something that keeps failing, and when to try it again.
 *
 * ```c
 * NYA_CircuitBreaker* upstream = nullptr;
 * NYA_TRY(nya_circuit_breaker_create(arena, &upstream, .failure_threshold = 5, .open_ms = 30000));
 *
 * // before every call to the dependency
 * if (!nya_circuit_allow(upstream, "payments")) return nya_error(NYA_ERROR_TIMEOUT, "circuit open");
 *
 * bool ok = do_the_call();
 * nya_circuit_record(upstream, "payments", ok);   // failure trips it, success heals it
 * ```
 *
 * ── why this is not the rate limiter or the backoff ──
 *
 * [[base_rate]] answers "how often may I call" and "how long before I retry". Both assume the call is
 * worth making. A breaker answers a different question: the dependency is *down*, so stop calling it at
 * all for a while. A retry-with-backoff still sends every request eventually; a stampede of clients all
 * backing off still buries a server that came back up. The breaker's job is to fail fast — return at
 * once without touching the network — so a dead dependency costs this program nothing and the dependency
 * gets quiet air to recover in.
 *
 * ── the three states ──
 *
 * CLOSED is normal: calls go through, consecutive failures are counted, and `failure_threshold` of them
 * in a row trips the breaker OPEN. (A single success in CLOSED clears the count — the threshold is for a
 * run of failures, not failures ever.)
 *
 * OPEN is fail-fast: `nya_circuit_allow` says no and nothing is sent, until `open_ms` has passed. Then
 * the next allow moves it to HALF_OPEN and lets one probe through.
 *
 * HALF_OPEN is the careful retry: up to `half_open_max` probes are allowed through at once while the rest
 * are still refused. `success_threshold` probe successes close the breaker; a single probe failure trips
 * it straight back OPEN for another `open_ms`, because a dependency that fails its probe is still down.
 *
 * ── keys ──
 *
 * One breaker holds a state per key, the same shape as the rate limiter: a key is a dependency this
 * program calls — a whole upstream (`"payments"`), one host, one route. Tripping is per key, so one dead
 * route does not fail-fast the calls that still work. Past NYA_CIRCUIT_MAX_KEYS a CLOSED (healthy) entry
 * is reused first and a tripped one is kept, for the reason the rate limiter keeps a spent bucket: losing
 * an OPEN entry would let calls back through to the thing that is down.
 *
 * ── thread safety ──
 *
 * One breaker is one thread's, like a limiter. Two threads sharing one need the caller's mutex around it.
 * */
#pragma once

#include "nyangine/base/base_arena.h"
#include "nyangine/base/base_attributes.h"
#include "nyangine/base/base_error.h"
#include "nyangine/base/base_types.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * CONSTANTS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/** Bytes of a key's name, terminator included — the same budget a rate bucket's name gets. */
#define NYA_CIRCUIT_MAX_KEY 64

/** Keys one breaker holds. A program calls a handful of distinct dependencies. */
#ifndef NYA_CIRCUIT_MAX_KEYS
#define NYA_CIRCUIT_MAX_KEYS 32
#endif

/** Consecutive failures that trip a key OPEN when the options leave it unset. */
#define NYA_CIRCUIT_FAILURE_THRESHOLD 5

/** Probe successes that close a HALF_OPEN key when the options leave it unset. */
#define NYA_CIRCUIT_SUCCESS_THRESHOLD 1

/** How long a key stays OPEN before a probe is let through, in milliseconds, when unset. */
#define NYA_CIRCUIT_OPEN_MS 30000

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * TYPES
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

typedef struct NYA_CircuitBreaker NYA_CircuitBreaker;

/** Which of the three states a key is in. See the header comment for what each one does. */
typedef enum {
    NYA_CIRCUIT_CLOSED = 0,
    NYA_CIRCUIT_OPEN,
    NYA_CIRCUIT_HALF_OPEN,
} NYA_CircuitState;

/** What a breaker is shaped like. Every field has a usable default, so `{ .failure_threshold = 5 }` works. */
typedef struct {
    /** Consecutive failures in CLOSED that trip the breaker. Zero means NYA_CIRCUIT_FAILURE_THRESHOLD. */
    u32 failure_threshold;

    /** Probe successes in HALF_OPEN that close it. Zero means NYA_CIRCUIT_SUCCESS_THRESHOLD. */
    u32 success_threshold;

    /**
     * Probes let through at once in HALF_OPEN. Zero means one.
     *
     * One is the safe default: it asks the dependency a single question and waits for the answer before
     * risking a second. A higher number recovers faster when the dependency is fine and only briefly
     * blipped, at the cost of more calls to something that might still be down.
     * */
    u32 half_open_max;

    /** How long the breaker stays OPEN before the next allow becomes a HALF_OPEN probe. Zero means NYA_CIRCUIT_OPEN_MS. */
    u64 open_ms;
} NYA_CircuitBreakerOptions;

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * FUNCTIONS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/** Builds a breaker into `arena`. Refuses a `failure_threshold`, `success_threshold` or `half_open_max` of zero only when set to a value that is not representable; the zero defaults above are applied first. */
NYA_API NYA_Error _nya_circuit_breaker_create(NYA_Arena* arena, OUT NYA_CircuitBreaker** out_breaker, NYA_CircuitBreakerOptions options) __attr_no_discard;

/** Takes the options by name. */
#define nya_circuit_breaker_create(arena, out_breaker, ...) _nya_circuit_breaker_create((arena), (out_breaker), (NYA_CircuitBreakerOptions){ __VA_ARGS__ })

/** Forgets every key's history. The arena owns the memory, so this is a reset rather than a free. */
NYA_API void nya_circuit_breaker_destroy(NYA_CircuitBreaker* breaker);

/**
 * Whether a call to `key` may go now.
 *
 * True from CLOSED always, from OPEN once `open_ms` has passed (which moves the key to HALF_OPEN and
 * counts this as its probe), and from HALF_OPEN while probes are still available. False means the
 * breaker is OPEN and the call must not be made — fail fast, spend nothing.
 *
 * A `true` from HALF_OPEN reserves a probe slot, so the caller MUST follow every `true` with exactly one
 * nya_circuit_record; a `true` that never reports back leaks the slot and the breaker never heals.
 * */
NYA_API b8 nya_circuit_allow(NYA_CircuitBreaker* breaker, NYA_ConstCString key) __attr_no_discard;

/**
 * The outcome of a call `nya_circuit_allow` let through: `true` if it succeeded, `false` if it failed.
 *
 * What counts as failure is the caller's — a transport error, a timeout, a 5xx — but NOT a 4xx the
 * dependency answered correctly: a 404 means the dependency is up and working, and tripping on it would
 * take a healthy service down over a bad request. Success in CLOSED clears the failure run; the
 * threshold-th consecutive failure trips it. In HALF_OPEN a success advances toward closing and a
 * failure re-opens at once.
 * */
NYA_API void nya_circuit_record(NYA_CircuitBreaker* breaker, NYA_ConstCString key, b8 success);

/** What state `key` is in right now, resolving an elapsed OPEN to HALF_OPEN. A key never seen reads as CLOSED. */
NYA_API NYA_CircuitState nya_circuit_state(NYA_CircuitBreaker* breaker, NYA_ConstCString key) __attr_no_discard;

/** How many keys the breaker is tracking, for the ceiling audit. */
NYA_API u32 nya_circuit_key_count(const NYA_CircuitBreaker* breaker) __attr_no_discard;
