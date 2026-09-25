#include <math.h>
#include <stdio.h>
#include <string.h>

#include "nyangine-std/base/base_assert.h"
#include "nyangine-std/base/base_clock.h"
#include "nyangine-std/base/base_rate.h"
#include "nyangine-std/os/os_random.h"
#include "nyangine-std/os/os_time.h"

// PRIVATE TYPES

/** One named budget: a token bucket, plus whatever the server last said about it. */
typedef struct {
    char key[NYA_RATE_MAX_KEY];

    f64 tokens;
    u64 refilled_at_ns;

    /**
     * Monotonic nanoseconds before which nothing goes, whatever the tokens say.
     *
     * What nya_rate_told writes. Kept beside the bucket rather than folded into it because the two
     * answer different questions — the bucket is what this program believes, and this is what the
     * server said — and when they disagree the server wins.
     * */
    u64 held_until_ns;
} _NYA_RateBucket;

struct NYA_RateLimiter {
    f64 per_second;
    f64 burst;

    _NYA_RateBucket buckets[NYA_RATE_MAX_BUCKETS];
    u32             bucket_count;
};

// PRIVATE API DECLARATION

/** The bucket for `key`, made when there is none. Null only when the table is full of waiting buckets. */
NYA_INTERNAL _NYA_RateBucket* _nya_rate_bucket(NYA_RateLimiter* limiter, NYA_ConstCString key, u64 now_ns) __attr_no_discard;

/** The bucket for `key` if it already exists. What a question uses, so asking costs no slot. */
NYA_INTERNAL const _NYA_RateBucket* _nya_rate_find(const NYA_RateLimiter* limiter, NYA_ConstCString key) __attr_no_discard;

/** Brings a bucket's tokens up to now, bounded by the burst. */
NYA_INTERNAL void _nya_rate_refill(const NYA_RateLimiter* limiter, _NYA_RateBucket* bucket, u64 now_ns);

/** Milliseconds until this bucket may go, given where it is now. */
NYA_INTERNAL u64 _nya_rate_bucket_wait_ms(const NYA_RateLimiter* limiter, const _NYA_RateBucket* bucket, u64 now_ns) __attr_no_discard;

// PUBLIC API IMPLEMENTATION

NYA_Error _nya_rate_limiter_create(NYA_Arena* arena, NYA_RateLimiter** out_limiter, NYA_RateLimiterOptions options) {
    nya_assert(arena != nullptr && out_limiter != nullptr);

    *out_limiter = nullptr;

    // A rate of zero is "never", not "no limit", and a caller that meant no limit wants no limiter at all; refused, along with a NaN, which compares false to everything and would make every wait forever.
    if (!(options.per_second > 0.0) || isnan(options.per_second)) {
        return nya_error(NYA_ERROR_INVALID_ARGUMENT, "a rate limiter needs a positive rate");
    }

    f64 burst = options.burst > 0.0 ? options.burst : options.per_second;

    // A burst under one call means a bucket that can never hold a whole call, a limiter that refuses forever; one is the floor, meaning "one at a time, `per_second` apart".
    if (burst < 1.0) burst = 1.0;

    NYA_RateLimiter* limiter = nya_arena_alloc(arena, sizeof(NYA_RateLimiter));
    if (limiter == nullptr) return nya_error(NYA_ERROR_OUT_OF_MEMORY, "no room for a rate limiter");

    nya_memset(limiter, 0, sizeof(*limiter));

    limiter->per_second = options.per_second;
    limiter->burst      = burst;

    *out_limiter = limiter;

    return NYA_OK;
}

void nya_rate_limiter_destroy(NYA_RateLimiter* limiter) {
    if (limiter == nullptr) return;

    nya_memset(limiter->buckets, 0, sizeof(limiter->buckets));
    limiter->bucket_count = 0;
}

b8 nya_rate_take(NYA_RateLimiter* limiter, NYA_ConstCString key, u64* out_wait_ms) {
    nya_assert(limiter != nullptr && key != nullptr && out_wait_ms != nullptr);

    *out_wait_ms = 0;

    u64 now_ns = nya_clock_get_monotonic_ns();

    _NYA_RateBucket* bucket = _nya_rate_bucket(limiter, key, now_ns);

    // Every bucket in the table is still waiting, so this one waits with them rather than taking a slot; see the header on why a fresh full bucket under pressure is wrong.
    if (bucket == nullptr) {
        *out_wait_ms = 1;
        return false;
    }

    _nya_rate_refill(limiter, bucket, now_ns);

    u64 wait_ms = _nya_rate_bucket_wait_ms(limiter, bucket, now_ns);

    if (wait_ms > 0) {
        *out_wait_ms = wait_ms;
        return false;
    }

    bucket->tokens -= 1.0;

    return true;
}

u64 nya_rate_wait(NYA_RateLimiter* limiter, NYA_ConstCString key) {
    nya_assert(limiter != nullptr && key != nullptr);

    u64 waited_ms = 0;

    for (;;) {
        u64 wait_ms = 0;

        if (nya_rate_take(limiter, key, &wait_ms)) return waited_ms;

        // Bounded, so a server answering `Retry-After: 86400` does not take this thread with it; the caller finds out it still cannot go, and nya_rate_take tells it the real number.
        if (wait_ms > NYA_RATE_MAX_WAIT_MS - waited_ms) {
            u64 left = NYA_RATE_MAX_WAIT_MS > waited_ms ? NYA_RATE_MAX_WAIT_MS - waited_ms : 0;

            if (left > 0) {
                nya_os_time_sleep_ms((u32)left);
                waited_ms += left;
            }

            return waited_ms;
        }

        nya_os_time_sleep_ms((u32)wait_ms);
        waited_ms += wait_ms;
    }
}

void nya_rate_told(NYA_RateLimiter* limiter, NYA_ConstCString key, u64 wait_ms) {
    nya_assert(limiter != nullptr && key != nullptr);

    u64 now_ns = nya_clock_get_monotonic_ns();

    _NYA_RateBucket* bucket = _nya_rate_bucket(limiter, key, now_ns);
    if (bucket == nullptr) return;

    u64 until_ns = now_ns + (wait_ms * 1000000ULL);

    // Later only: two replies about one bucket can arrive out of order, and the one that shortens a wait another reply already lengthened is the one that gets this program banned.
    if (until_ns > bucket->held_until_ns) bucket->held_until_ns = until_ns;

    /* The bucket is empty too, not just held: a server saying "wait" is saying this program's arithmetic was wrong, so the tokens it thought it had were never there. */
    bucket->tokens         = 0.0;
    bucket->refilled_at_ns = now_ns;
}

void nya_rate_observed(NYA_RateLimiter* limiter, NYA_ConstCString key, f64 remaining, u64 reset_ms) {
    nya_assert(limiter != nullptr && key != nullptr);

    if (isnan(remaining) || remaining < 0.0) return;

    u64 now_ns = nya_clock_get_monotonic_ns();

    _NYA_RateBucket* bucket = _nya_rate_bucket(limiter, key, now_ns);
    if (bucket == nullptr) return;

    _nya_rate_refill(limiter, bucket, now_ns);

    // Lower only, as nya_rate_told moves later only: a reply describes the moment it was made, and one arriving late describing a fuller bucket is describing the past.
    if (remaining < bucket->tokens) {
        bucket->tokens         = remaining;
        bucket->refilled_at_ns = now_ns;
    }

    // Nothing left and a window to wait out: that is the same fact a 429 carries, before the 429.
    if (remaining < 1.0 && reset_ms > 0) {
        u64 until_ns = now_ns + (reset_ms * 1000000ULL);

        if (until_ns > bucket->held_until_ns) bucket->held_until_ns = until_ns;
    }
}

u64 nya_rate_wait_for(const NYA_RateLimiter* limiter, NYA_ConstCString key) {
    nya_assert(limiter != nullptr && key != nullptr);

    const _NYA_RateBucket* bucket = _nya_rate_find(limiter, key);

    // Nothing known about it is a full bucket: the first call on a key always goes.
    if (bucket == nullptr) return 0;

    u64 now_ns = nya_clock_get_monotonic_ns();

    // On a copy, because asking is not spending and a question must not move anything.
    _NYA_RateBucket scratch = *bucket;
    _nya_rate_refill(limiter, &scratch, now_ns);

    return _nya_rate_bucket_wait_ms(limiter, &scratch, now_ns);
}

u32 nya_rate_bucket_count(const NYA_RateLimiter* limiter) {
    nya_assert(limiter != nullptr);

    return limiter->bucket_count;
}

u64 _nya_backoff_ms(u32 attempt, NYA_BackoffOptions options) {
    u64 base_ms = options.base_ms != 0 ? options.base_ms : NYA_RATE_BACKOFF_BASE_MS;
    u64 cap_ms  = options.cap_ms != 0 ? options.cap_ms : NYA_RATE_BACKOFF_CAP_MS;

    if (cap_ms < base_ms) cap_ms = base_ms;

    // Saturated rather than shifted past the width of the type: attempt 64 would otherwise be undefined, and what it should mean is obvious.
    u64 window_ms = cap_ms;

    if (attempt < 32) {
        u64 doubled = base_ms << attempt;

        window_ms = doubled < cap_ms && doubled >= base_ms ? doubled : cap_ms;
    }

    f64 jitter = options.jitter;

    if (jitter <= 0.0 || isnan(jitter)) jitter = 1.0;
    if (jitter > 1.0) jitter = 1.0;

    /* Full jitter by default, a uniform pick from the whole window: without it every client that failed at the same moment retries together, the thundering herd. */
    u64 random = 0;
    if (!nya_os_random_bytes((u8*)&random, sizeof(random))) return window_ms;

    f64 fraction = (f64)(random % 1000000ULL) / 1000000.0;

    f64 fixed   = (f64)window_ms * (1.0 - jitter);
    f64 random_part = (f64)window_ms * jitter * fraction;

    return (u64)(fixed + random_part);
}

b8 nya_retry_is_worthwhile(u32 status) {
    // Zero is this tree's "no answer ever came": a reset, a timeout, a name that did not resolve.
    if (status == 0) return true;

    switch (status) {
        // Request Timeout, Too Early, Too Many Requests; the last is a wait rather than a doubling (see nya_rate_told) but still worth sending again afterwards.
        case 408:
        case 425:
        case 429: return true;

        default: break;
    }

    return status >= 500 && status < 600;
}

// PRIVATE API IMPLEMENTATION

const _NYA_RateBucket* _nya_rate_find(const NYA_RateLimiter* limiter, NYA_ConstCString key) {
    for (u32 index = 0; index < limiter->bucket_count; index++) {
        if (strcmp(limiter->buckets[index].key, key) == 0) return &limiter->buckets[index];
    }

    return nullptr;
}

_NYA_RateBucket* _nya_rate_bucket(NYA_RateLimiter* limiter, NYA_ConstCString key, u64 now_ns) {
    for (u32 index = 0; index < limiter->bucket_count; index++) {
        if (strcmp(limiter->buckets[index].key, key) == 0) return &limiter->buckets[index];
    }

    _NYA_RateBucket* bucket = nullptr;

    if (limiter->bucket_count < NYA_RATE_MAX_BUCKETS) {
        bucket = &limiter->buckets[limiter->bucket_count++];
    } else {
        /* Only a full, unheld bucket may be reused, the stalest among them: a waiting bucket is budget already spent, and giving its slot away would hand the newcomer a full one and stop the limiter limiting. */
        for (u32 index = 0; index < NYA_RATE_MAX_BUCKETS; index++) {
            _NYA_RateBucket* candidate = &limiter->buckets[index];

            if (candidate->held_until_ns > now_ns) continue;

            _NYA_RateBucket scratch = *candidate;
            _nya_rate_refill(limiter, &scratch, now_ns);

            if (scratch.tokens < limiter->burst) continue;
            if (bucket == nullptr || candidate->refilled_at_ns < bucket->refilled_at_ns) bucket = candidate;
        }

        if (bucket == nullptr) return nullptr;
    }

    nya_memset(bucket, 0, sizeof(*bucket));

    (void)snprintf(bucket->key, sizeof(bucket->key), "%s", key);

    bucket->tokens         = limiter->burst;
    bucket->refilled_at_ns = now_ns;

    return bucket;
}

void _nya_rate_refill(const NYA_RateLimiter* limiter, _NYA_RateBucket* bucket, u64 now_ns) {
    if (now_ns <= bucket->refilled_at_ns) return;

    f64 elapsed_s = (f64)(now_ns - bucket->refilled_at_ns) / 1e9;

    bucket->tokens += elapsed_s * limiter->per_second;

    if (bucket->tokens > limiter->burst) bucket->tokens = limiter->burst;

    bucket->refilled_at_ns = now_ns;
}

u64 _nya_rate_bucket_wait_ms(const NYA_RateLimiter* limiter, const _NYA_RateBucket* bucket, u64 now_ns) {
    // What the server said comes first: it is the only one of the two that knows.
    if (bucket->held_until_ns > now_ns) {
        u64 held_ms = (bucket->held_until_ns - now_ns) / 1000000ULL;

        return held_ms > 0 ? held_ms : 1;
    }

    if (bucket->tokens >= 1.0) return 0;

    // A whole token's refill, rounded up: asking again a moment early is asking again for nothing.
    f64 seconds = (1.0 - bucket->tokens) / limiter->per_second;
    u64 wait_ms = (u64)ceil(seconds * 1000.0);

    return wait_ms > 0 ? wait_ms : 1;
}
