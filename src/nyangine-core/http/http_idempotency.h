/**
 * @file http_idempotency.h
 *
 * A retried unsafe request runs once. The layer that makes `POST` safe to send twice.
 *
 * ```
 * nya_http_idempotency_init   the store: its TTL, its lock, and one ceiling registration
 * nya_http_layer_idempotency  the layer: dedupes an unsafe request by its Idempotency-Key
 * nya_http_idempotency_reset  forgets every key, for a test or a controlled restart
 * nya_http_idempotency_count  how many keys are held, for the ceiling and a test
 * ```
 *
 * ── what it is for ──
 *
 * A network drops the answer to a `POST /orders` and the client, having heard nothing, sends it again.
 * Without this the order is placed twice. With it the client sends the same `Idempotency-Key` on the
 * retry, the layer replays the first answer, and the handler behind it runs exactly once. It is the
 * server half of the retry every resilient client already does; see base_circuit.h for the other half,
 * which is this program deciding not to call something that keeps failing.
 *
 * ── which requests it touches ──
 *
 * Unsafe ones only — POST, PUT, PATCH, DELETE — because a safe method (GET, HEAD, QUERY, OPTIONS) is
 * already a repeat of itself and has nothing to dedupe. A safe method, and an unsafe one that carries no
 * `Idempotency-Key`, pass straight through to the rest of the chain. With a key:
 *
 * - **No entry** → the key is marked in-flight, the chain runs, and its answer (status, media type and
 *   body) is captured under the key before it is returned.
 * - **A completed entry, same request** → the stored answer is replayed WITHOUT running the chain, with
 *   an `Idempotency-Replayed: true` header added. This is the whole point: the handler ran once.
 * - **An in-flight entry** → `409 Conflict`. A concurrent duplicate is refused rather than run, so two
 *   copies of the same request in flight at once still place one order.
 * - **A completed entry, but a different request** (the method, path or body do not match the
 *   fingerprint the first stored) → `422 Unprocessable Content`. Reusing a key for a different payload
 *   is a client bug, and serving it the first answer would be answering a question it did not ask.
 *
 * A key that is empty is treated as no key. A key that is too long or not printable is a malformed
 * header and answers `400 Bad Request` rather than being stored.
 *
 * So a route wrapped in this layer can answer with 400, 409 and 422 on top of whatever its handler
 * declares, and — because a route's `statuses` covers its whole chain, see http_router.h — an unsafe
 * verb behind this layer must list those three. A safe verb behind it cannot reach them and need not.
 *
 * ── the store ──
 *
 * A fixed table of NYA_HTTP_IDEMPOTENCY_MAX_ENTRIES, registered with nya_ceiling_register so its
 * fullness shows up beside every other bounded thing in the program. An entry expires
 * NYA_HTTP_IDEMPOTENCY_TTL_S seconds after it was last touched — long enough for a client's retries,
 * short enough that the table does not fill with keys nobody will send again. When a new key needs a
 * slot and the table is full, an expired entry is reused first and then the oldest completed one; an
 * in-flight entry is never evicted, for the reason base_circuit.h never evicts a tripped breaker. If
 * every slot is a live in-flight request the newcomer is not tracked and passes through, because
 * refusing a request to protect a dedup table would be the layer causing the outage it exists to avoid.
 *
 * An answer larger than NYA_HTTP_IDEMPOTENCY_MAX_BODY_BYTES is not cached — the reservation is dropped
 * and a retry re-runs the handler — because replaying a truncated body is worse than replaying nothing.
 * Every DTO answer in this tree is far inside that bound.
 *
 * ── thread safety ──
 *
 * A layer runs inside nya_http_router_dispatch, which the server calls on each of its worker threads at
 * once (http_server.c), so one store is shared across every connection and must be locked. It carries
 * its own NYA_Mutex, made at init from the caller's arena — the server's own primitive, not a new one —
 * and the lock is taken only around the table's lookups and writes, never across the chain, so handlers
 * still run concurrently. A server with no workers never calls init; the mutex is then null and
 * nya_mutex_lock is a no-op (base_thread.h), which is exactly right for a single-threaded server.
 * */
#pragma once

#include "nyangine-std/base/base_arena.h"
#include "nyangine-std/base/base_attributes.h"
#include "nyangine-std/base/base_error.h"
#include "nyangine-std/base/base_types.h"
#include "nyangine-core/http/http_router.h"
#include "nyangine-core/http/http_types.h"

// CONSTANTS

/** Keys held at once. A handful of clients each retrying a handful of requests is well inside this. */
#ifndef NYA_HTTP_IDEMPOTENCY_MAX_ENTRIES
#define NYA_HTTP_IDEMPOTENCY_MAX_ENTRIES 128
#endif

/**
 * Bytes of a key, terminator included. Past a UUID and past what Stripe accepts, and a key longer than
 * this is a malformed header answered with 400 rather than truncated into a different key.
 * */
#define NYA_HTTP_IDEMPOTENCY_MAX_KEY 200

/**
 * Bytes of a stored answer's body. An answer larger than this is not cached at all, for the reason the
 * header note gives: a truncated replay is a wrong answer, and a wrong answer is worse than none.
 * */
#define NYA_HTTP_IDEMPOTENCY_MAX_BODY_BYTES 2048

/** Bytes of the request fingerprint: 128 bits of BLAKE2b over the method, path and body. */
#define NYA_HTTP_IDEMPOTENCY_FINGERPRINT_BYTES 16

/** How long an entry lives past its last touch, in seconds, when the options leave it unset. */
#define NYA_HTTP_IDEMPOTENCY_TTL_S 300

// TYPES

typedef struct NYA_HttpIdempotencyOptions NYA_HttpIdempotencyOptions;

/** What the store is shaped like. Every field has a usable default, so `{ 0 }` is the sane store. */
struct NYA_HttpIdempotencyOptions {
    /** How long an entry lives past its last touch. Zero means NYA_HTTP_IDEMPOTENCY_TTL_S. */
    u64 ttl_s;
};

// FUNCTIONS

/**
 * Readies the one store the layer uses: sets its TTL, makes its lock out of `arena`, and registers its
 * ceiling once.
 *
 * A server with workers has to call this before installing the layer, so the shared store is locked; a
 * server with none may skip it, and then the lock is null and does nothing. Calling it twice keeps the
 * first lock and only updates the TTL, so a reload cannot leak one.
 * */
NYA_API NYA_Error _nya_http_idempotency_init(NYA_Arena* arena, NYA_HttpIdempotencyOptions options) __attr_no_discard;

/** Takes the options by name. */
#define nya_http_idempotency_init(arena, ...) _nya_http_idempotency_init((arena), (NYA_HttpIdempotencyOptions){ __VA_ARGS__ })

/**
 * Forgets every key and gives the lock back to its arena, so a program that shuts the server down leaves
 * nothing holding it. The ceiling registration is kept — it names the static count, which does not move —
 * so a later init re-arms the store rather than registering a second time.
 * */
NYA_API void nya_http_idempotency_deinit(void);

/**
 * Dedupes an unsafe request by its `Idempotency-Key`. See the file note for the full table of what it
 * does; the short of it is that a retried POST with the same key runs once and replays its first answer.
 *
 * Reads the exchange's `now_s` as its clock, so a test drives expiry by advancing that field rather than
 * by sleeping. Can answer 400, 409 or 422 on its own; otherwise it returns whatever the chain did.
 * */
NYA_API NYA_HttpStatus nya_http_layer_idempotency(NYA_HttpExchange* exchange, NYA_HttpChain* next);

/** Forgets every stored key, keeping the lock and the ceiling registration. For a test or a restart. */
NYA_API void nya_http_idempotency_reset(void);

/** How many keys are held right now, which is the ceiling's live count. */
NYA_API u32 nya_http_idempotency_count(void) __attr_no_discard;
