/**
 * @file base_rate.h
 *
 * How often this program may do something, and how long to wait when it may not.
 *
 * ```c
 * // "Discord lets this token do 50 things a second"
 * NYA_RateLimiter* discord = nullptr;
 * NYA_TRY(nya_rate_limiter_create(arena, &discord, .per_second = 50.0, .burst = 50.0));
 *
 * // before every call, which blocks for as long as the budget says and no longer
 * nya_rate_wait(discord, "channels/123/messages");
 *
 * // and when the server disagrees, the server is right
 * if (response.status == 429) nya_rate_told(discord, "channels/123/messages", retry_after_ms);
 * ```
 *
 * ── outbound is not inbound ──
 *
 * The HTTP server has a rate limiter of its own and it is deliberately not this one. Inbound, a
 * stranger over budget is *refused*: a 429 goes back and the connection closes, because the whole
 * point is to spend nothing on them. Outbound, over budget means **wait** — the request is this
 * program's own and dropping it usually loses something a user asked for.
 *
 * The other half of the difference is who decides. A server decides its own inbound limits. A client
 * does not decide anything: the third party publishes the limit, changes it whenever it likes, and
 * answers 429 when this program has it wrong. So nya_rate_told exists, and what it is told always wins
 * over what was configured — a local guess that argued with a `Retry-After` would be a program that
 * gets itself banned while believing it is within the rules.
 *
 * ── keys ──
 *
 * A limiter holds buckets by name, and what a name means is the caller's. A whole API (`"discord"`), a
 * route (`"channels/123/messages"`), or whatever the server itself calls its bucket — Discord names
 * one in `X-RateLimit-Bucket`, and using that name is how several routes that share a budget come to
 * share a bucket here.
 *
 * Past NYA_RATE_MAX_BUCKETS the bucket that has been full longest is reused, and one that is still
 * spent is never given up: a table full of waiting buckets keeps every one of them, and the newcomer
 * waits with them. Handing out a fresh full bucket under pressure is how a limiter becomes a thing
 * that does not limit.
 *
 * ── the backoff is separate, and it is not a limiter ──
 *
 * nya_backoff_ms answers what a retry should wait. That is a different question from what a budget
 * allows: a 500 is not a rate limit, and retrying it immediately is what turns one server's bad minute
 * into every client's stampede. Exponential with jitter, because without jitter every client that
 * failed together retries together and keeps doing so.
 *
 * ── thread safety ──
 *
 * A limiter is one thread's. Two threads sharing one needs a mutex around it, which is the caller's to
 * hold, because a limiter that locked would be a lock taken on every call for the many callers that
 * have one limiter per thread anyway.
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

/** Bytes of a bucket's name, terminator included. A route or a server's own bucket id fits. */
#define NYA_RATE_MAX_KEY 64

/** Buckets one limiter holds. A client talking to one API touches a handful of routes. */
#ifndef NYA_RATE_MAX_BUCKETS
#define NYA_RATE_MAX_BUCKETS 32
#endif

/** What a backoff starts at and what it never passes, in milliseconds, unless a caller says otherwise. */
#define NYA_RATE_BACKOFF_BASE_MS 250
#define NYA_RATE_BACKOFF_CAP_MS  30000

/**
 * The longest one nya_rate_wait will ever sleep for, in milliseconds.
 *
 * A server may answer `Retry-After: 86400`. Sleeping a day inside a call nobody can interrupt is worse
 * than returning and letting the program decide, so the wait stops here and nya_rate_take still says
 * the real number.
 * */
#define NYA_RATE_MAX_WAIT_MS 60000

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * TYPES
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

typedef struct NYA_RateLimiter NYA_RateLimiter;

/** What a limiter is made of. Both are what the third party publishes, not what this program wants. */
typedef struct {
    /**
     * Calls a second one bucket may make. Required.
     *
     * A fraction is fine and often right: a limit of thirty a minute is `0.5`, and expressing it that
     * way is what makes the wait between calls two seconds rather than a burst and then a stall.
     * */
    f64 per_second;

    /**
     * How many may go at once from a full bucket. Zero means `per_second`, which is one second's worth.
     *
     * This is the number that decides whether a burst is allowed at all. A server that says "50 a
     * second" usually means a burst of 50 is fine; one that means "no closer together than 2 seconds"
     * has a burst of 1.
     * */
    f64 burst;
} NYA_RateLimiterOptions;

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * FUNCTIONS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/** Builds a limiter into `arena`. Refuses a `per_second` that is zero, negative, or not a number. */
NYA_API NYA_Error _nya_rate_limiter_create(NYA_Arena* arena, OUT NYA_RateLimiter** out_limiter, NYA_RateLimiterOptions options) __attr_no_discard;

/** Takes the options by name. */
#define nya_rate_limiter_create(arena, out_limiter, ...) _nya_rate_limiter_create((arena), (out_limiter), (NYA_RateLimiterOptions){ __VA_ARGS__ })

/** Empties every bucket's history. The arena owns the memory, so this is a reset rather than a free. */
NYA_API void nya_rate_limiter_destroy(NYA_RateLimiter* limiter);

/**
 * Spends one call from `key`'s budget if there is one, and says how long until there is if there is not.
 *
 * True means go now. False means wait `out_wait_ms` and ask again — nothing was spent, so a caller
 * that gives up instead has cost the budget nothing.
 * */
NYA_API b8 nya_rate_take(NYA_RateLimiter* limiter, NYA_ConstCString key, OUT u64* out_wait_ms) __attr_no_discard;

/**
 * Waits until `key` may go, then spends it. Answers how long it actually waited.
 *
 * Never longer than NYA_RATE_MAX_WAIT_MS in one call, so a server that asks for a day back does not
 * take the calling thread with it; a caller that finds it still cannot go has the real number from
 * nya_rate_take and may decide what that is worth.
 * */
NYA_API u64 nya_rate_wait(NYA_RateLimiter* limiter, NYA_ConstCString key);

/**
 * What the server said: nothing on `key` until `wait_ms` from now.
 *
 * This is a 429's `Retry-After`, or Discord's `X-RateLimit-Reset-After` on an empty bucket. It
 * overrides whatever the local arithmetic believed, because the server is the only one who knows —
 * and it only ever moves the wait *later*, so an answer that arrives out of order cannot shorten one.
 * */
NYA_API void nya_rate_told(NYA_RateLimiter* limiter, NYA_ConstCString key, u64 wait_ms);

/**
 * What the server said about how much is left, from the headers a reply carries.
 *
 * `remaining` calls before `reset_ms` from now. For an API that publishes its budget — Discord's
 * `X-RateLimit-Remaining` and `-Reset-After`, GitHub's `X-RateLimit-*` — this is how the local bucket
 * is corrected on every reply rather than only when one is refused. Only ever lowers what is believed
 * to be left, for the reason nya_rate_told only moves later.
 * */
NYA_API void nya_rate_observed(NYA_RateLimiter* limiter, NYA_ConstCString key, f64 remaining, u64 reset_ms);

/** Milliseconds until `key` may go, without spending anything. Zero means now. */
NYA_API u64 nya_rate_wait_for(const NYA_RateLimiter* limiter, NYA_ConstCString key) __attr_no_discard;

/** How many buckets are in use, for the ceiling audit. */
NYA_API u32 nya_rate_bucket_count(const NYA_RateLimiter* limiter) __attr_no_discard;

/*
 * ─────────────────────────────────────────────────────────
 * BACKOFF
 * ─────────────────────────────────────────────────────────
 */

/** What a backoff is shaped like. Zero for either bound means the NYA_RATE_BACKOFF_ one. */
typedef struct {
    u64 base_ms;
    u64 cap_ms;

    /**
     * How much of the wait is random, from 0 to 1. Zero is the default and means *full* jitter.
     *
     * Full jitter — a uniform pick from the whole window rather than a fixed wait give or take a
     * little — is what AWS measured as the best of the family, and the reason is the failure it
     * prevents: clients that failed together and backed off by the same doubling arrive together
     * again, and keep arriving together until they give up.
     * */
    f64 jitter;
} NYA_BackoffOptions;

/**
 * How long to wait before retry number `attempt`, counting from zero.
 *
 * Doubles from `base_ms`, stops at `cap_ms`, and the result is random inside that window; the shift is
 * bounded so a large attempt number saturates rather than wrapping.
 *
 * This is for a failure that might not happen again — a 5xx, a connection reset, a timeout. A 429 is
 * not one of those: it is the server saying exactly how long, and nya_rate_told is what to do with it.
 * */
NYA_API u64 _nya_backoff_ms(u32 attempt, NYA_BackoffOptions options) __attr_no_discard;

/** Takes the options by name. */
#define nya_backoff_ms(attempt, ...) _nya_backoff_ms((attempt), (NYA_BackoffOptions){ __VA_ARGS__ })

/**
 * Whether a failed attempt is worth another go.
 *
 * True for 408, 425, 429, every 5xx, and for a `status` of zero, which is how this tree says the
 * transport failed and no answer ever came. False for everything else: a 400 sent again is the same
 * 400, and a 401 sent again is how a token gets locked.
 * */
NYA_API b8 nya_retry_is_worthwhile(u32 status) __attr_no_discard;
