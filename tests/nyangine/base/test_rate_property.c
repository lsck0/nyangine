/**
 * The outbound budget as laws: a full bucket grants no more than its burst before it has to refill;
 * refill is monotone in the time that passed and never overfills; a backoff lands inside its doubling
 * window and so inside the cap, and that window grows with the attempt; and the server's word — told and
 * observed — only ever moves a wait later or a count lower, never the other way.
 *
 * The token-bucket arithmetic is driven through the module's own internal helpers with a clock the law
 * chooses, so a "window" is exact rather than however many milliseconds a real one happened to tick. The
 * told/observed laws use the real monotonic clock, but only ever compare two reads taken a breath apart,
 * so what is asserted is the ordering the header promises and not an absolute number of milliseconds.
 **/

#include "nyangine/nyangine.c"
#include "nyangine/nyangine.h"

#define CASES 4000

/** Fewer for the two laws that touch the real clock: enough to be a property, few enough to stay quick. */
#define CLOCK_CASES 600

/** "rate" and "prop" in ASCII. */
#define SEED 0x726174657072006FULL

/** A clock base well away from zero, so a "now" and a small offset from it are both ordinary numbers. */
#define NOW_BASE_NS (1000000000000ULL)

/* HELPERS */

/** A rate in (0, 1000] and a burst in [1, 500]: the shape of a real published limit, kept small enough to loop. */
static void draw_shape(NYA_Property* property, OUT f64* out_per_second, OUT f64* out_burst) {
    *out_per_second = (f64)nya_property_draw_f32(property, 0.1f, 1000.0f);
    if (!(*out_per_second > 0.0)) *out_per_second = 0.1; // draw_f32 can land on its low edge; keep it positive.

    *out_burst = (f64)(1 + (u32)nya_property_draw_below(property, 500));
}

/* LAWS */

/** From a full bucket, with no time allowed to pass, exactly floor(burst) calls go and then none do. */
static b8 law_no_more_than_burst_in_a_window(NYA_Property* property) {
    f64 per_second = 0.0;
    f64 burst      = 0.0;
    draw_shape(property, &per_second, &burst);

    NYA_Arena* arena = property->allocator;

    NYA_RateLimiter* limiter = nullptr;
    if (!_nya_rate_limiter_create(arena, &limiter, (NYA_RateLimiterOptions){ .per_second = per_second, .burst = burst }).ok) return false;

    u64 now_ns = NOW_BASE_NS;

    // A fresh bucket starts full at `burst`. Everything happens at one instant, so no refill can add to it.
    _NYA_RateBucket* bucket = _nya_rate_bucket(limiter, "k", now_ns);
    if (bucket == nullptr) return false;

    u32 granted = 0;
    for (u32 index = 0; index < (u32)burst + 8; index++) {
        _nya_rate_refill(limiter, bucket, now_ns);
        if (_nya_rate_bucket_wait_ms(limiter, bucket, now_ns) != 0) break;
        bucket->tokens -= 1.0;
        granted++;
    }

    // burst is a whole number here, so floor(burst) == burst: that many calls, and the next must wait.
    nya_property_note(property, "a burst of %d granted %u calls at one instant", (u32)burst, granted);
    return granted == (u32)burst && _nya_rate_bucket_wait_ms(limiter, bucket, now_ns) > 0;
}

/** More elapsed time never leaves a bucket with fewer tokens, and never with more than the burst. */
static b8 law_refill_is_monotonic_and_capped(NYA_Property* property) {
    f64 per_second = 0.0;
    f64 burst      = 0.0;
    draw_shape(property, &per_second, &burst);

    NYA_Arena* arena = property->allocator;

    NYA_RateLimiter* limiter = nullptr;
    if (!_nya_rate_limiter_create(arena, &limiter, (NYA_RateLimiterOptions){ .per_second = per_second, .burst = burst }).ok) return false;

    f64 start = (f64)nya_property_draw_f32(property, 0.0f, (f32)burst); // somewhere in [0, burst].

    // Two elapsed spans, e1 <= e2, up to a minute expressed in nanoseconds.
    u64 e1 = nya_property_draw_below(property, 60ULL * 1000000000ULL);
    u64 e2 = nya_property_draw_below(property, 60ULL * 1000000000ULL);
    if (e2 < e1) { u64 swap = e1; e1 = e2; e2 = swap; }

    _NYA_RateBucket earlier = { .tokens = start, .refilled_at_ns = NOW_BASE_NS };
    _NYA_RateBucket later   = { .tokens = start, .refilled_at_ns = NOW_BASE_NS };

    _nya_rate_refill(limiter, &earlier, NOW_BASE_NS + e1);
    _nya_rate_refill(limiter, &later, NOW_BASE_NS + e2);

    // A hair of slack for the floating-point of elapsed*rate; the law is the direction, not the last bit.
    b8 monotone = later.tokens >= earlier.tokens - 1e-6;
    b8 capped   = earlier.tokens <= burst + 1e-6 && later.tokens <= burst + 1e-6;
    b8 grew     = earlier.tokens >= start - 1e-6; // refill only ever adds.

    nya_property_note(property, "start %.3f, burst %.1f: %.3f at e1 then %.3f at e2", start, burst, earlier.tokens, later.tokens);
    return monotone && capped && grew;
}

/** The saturating doubling window this build should compute for an attempt, in milliseconds. */
static u64 expected_window(u32 attempt, u64 base_ms, u64 cap_ms) {
    if (cap_ms < base_ms) cap_ms = base_ms;
    if (attempt >= 32) return cap_ms;

    u64 doubled = base_ms << attempt;
    return (doubled < cap_ms && doubled >= base_ms) ? doubled : cap_ms;
}

/** A backoff is never negative, never past the cap, and never past the doubling window for its attempt. */
static b8 law_backoff_stays_in_its_window(NYA_Property* property) {
    u32 attempt = (u32)nya_property_draw_below(property, 40);
    u64 base_ms = 1 + nya_property_draw_below(property, 1000);
    u64 cap_ms  = base_ms + nya_property_draw_below(property, 60000);

    u64 window = expected_window(attempt, base_ms, cap_ms);

    // Full jitter (the default): a uniform pick from [0, window). Sampled a few times, since it is random.
    for (u32 sample = 0; sample < 8; sample++) {
        u64 wait_ms = nya_backoff_ms(attempt, .base_ms = base_ms, .cap_ms = cap_ms);

        if (wait_ms > window || wait_ms > cap_ms) {
            nya_property_note(property, "attempt %u gave %llu ms, window is %llu, cap %llu", attempt, (unsigned long long)wait_ms,
                              (unsigned long long)window, (unsigned long long)cap_ms);
            return false;
        }
    }

    return true;
}

/** With jitter pinned to almost nothing, the wait is the window itself, and the window grows with the attempt. */
static b8 law_backoff_window_grows(NYA_Property* property) {
    u64 base_ms = 1 + nya_property_draw_below(property, 500);
    u64 cap_ms  = base_ms + nya_property_draw_below(property, 60000);

    u32 attempt = (u32)nya_property_draw_below(property, 20);

    // 0.0001 rather than 0: a literal zero is read as "default", which is full jitter. This tiny value leaves the wait a hair below the window, so the window shows through.
    u64 lower = nya_backoff_ms(attempt, .base_ms = base_ms, .cap_ms = cap_ms, .jitter = 0.0001);
    u64 upper = nya_backoff_ms(attempt + 1, .base_ms = base_ms, .cap_ms = cap_ms, .jitter = 0.0001);

    u64 window_lower = expected_window(attempt, base_ms, cap_ms);
    u64 window_upper = expected_window(attempt + 1, base_ms, cap_ms);

    // The result sits in the top thousandth of its window; a couple of ms covers the truncation.
    u64 slack_lower = window_lower / 1000 + 2;
    u64 slack_upper = window_upper / 1000 + 2;

    b8 is_window = lower + slack_lower >= window_lower && lower <= window_lower && upper + slack_upper >= window_upper && upper <= window_upper;

    // A later attempt's window is at least as large, so its pinned wait is too, allowing for the noise.
    b8 grows = window_upper >= window_lower && upper + slack_upper >= lower;

    nya_property_note(property, "attempt %u -> %llu (window %llu), attempt %u -> %llu (window %llu)", attempt, (unsigned long long)lower,
                      (unsigned long long)window_lower, attempt + 1, (unsigned long long)upper, (unsigned long long)window_upper);
    return is_window && grows;
}

/** told never shortens a wait a longer told already set, and a longer told does lengthen it. */
static b8 law_told_moves_later_only(NYA_Property* property) {
    NYA_Arena* arena = property->allocator;

    NYA_RateLimiter* limiter = nullptr;
    if (!_nya_rate_limiter_create(arena, &limiter, (NYA_RateLimiterOptions){ .per_second = 1000.0, .burst = 1000.0 }).ok) return false;

    u64 big   = 2000 + nya_property_draw_below(property, 8000);
    u64 small = 1 + nya_property_draw_below(property, big / 2);

    nya_rate_told(limiter, "k", big);
    u64 after_big = nya_rate_wait_for(limiter, "k");

    nya_rate_told(limiter, "k", small);
    u64 after_small = nya_rate_wait_for(limiter, "k");

    // Two reads a breath apart, so at most a millisecond or two of real time elapses between them.
    #define TOL_MS 25

    // A shorter answer does not shorten the wait the longer one set.
    if (after_small + TOL_MS < after_big) {
        nya_property_note(property, "a told of %llu shortened a wait of %llu (%llu -> %llu)", (unsigned long long)small, (unsigned long long)big,
                          (unsigned long long)after_big, (unsigned long long)after_small);
        return false;
    }

    // And a longer one than what stands does lengthen it.
    u64 longer = after_small + 5000;
    nya_rate_told(limiter, "k", longer);
    u64 after_longer = nya_rate_wait_for(limiter, "k");

    b8 lengthened = after_longer + TOL_MS >= longer && after_longer > after_small;

    nya_property_note(property, "a longer told did not lengthen the wait (%llu -> %llu, wanted ~%llu)", (unsigned long long)after_small,
                      (unsigned long long)after_longer, (unsigned long long)longer);
    #undef TOL_MS
    return lengthened;
}

/** observed only ever lowers what is believed left: a fuller count arriving later is not believed. */
static b8 law_observed_lowers_only(NYA_Property* property) {
    NYA_Arena* arena = property->allocator;

    // A crawling refill rate against a deep burst: the point of the law is that observed does not raise the count, and a fast rate would refill real tokens between takes and make the run's arithmetic about the wall clock rather than about observed.
    NYA_RateLimiter* limiter = nullptr;
    if (!_nya_rate_limiter_create(arena, &limiter, (NYA_RateLimiterOptions){ .per_second = 1.0, .burst = 100000.0 }).ok) return false;

    u64 wait_ms = 0;
    if (!nya_rate_take(limiter, "k", &wait_ms)) return false;

    // "You have `low` left": believed, since it is lower than a fresh full bucket.
    f64 low = (f64)nya_property_draw_below(property, 50);
    nya_rate_observed(limiter, "k", low, 0);

    u64 granted = 0;
    for (u32 index = 0; index < (u32)low + 8; index++) {
        u64 w = 0;
        if (!nya_rate_take(limiter, "k", &w)) break;
        granted++;
    }

    // At most what it was told was left; a fuller count from before must not have raised it.
    if (granted > (u64)low) {
        nya_property_note(property, "observed said %llu left but %llu calls went", (unsigned long long)(u64)low, (unsigned long long)granted);
        return false;
    }

    // A late "you have thousands left" does not refill an emptied bucket.
    nya_rate_observed(limiter, "k", 100000.0, 0);
    u64 after = 0;
    b8  went  = nya_rate_take(limiter, "k", &after);

    nya_property_note(property, "a fuller count from the past refilled the bucket");
    return !went;
}

s32 main(void) {
    setvbuf(stdout, nullptr, _IONBF, 0);

    u32 failures = 0;

    failures += nya_property_check("no more than the burst goes in one instant", CASES, SEED, law_no_more_than_burst_in_a_window);
    failures += nya_property_check("refill is monotone in elapsed time and capped at the burst", CASES, SEED, law_refill_is_monotonic_and_capped);
    failures += nya_property_check("a backoff stays within its window and the cap", CASES, SEED, law_backoff_stays_in_its_window);
    failures += nya_property_check("the backoff window is the doubling and grows with the attempt", CASES, SEED, law_backoff_window_grows);
    failures += nya_property_check("told moves a wait later, never earlier", CLOCK_CASES, SEED, law_told_moves_later_only);
    failures += nya_property_check("observed only lowers what is believed left", CLOCK_CASES, SEED, law_observed_lowers_only);

    return failures == 0 ? 0 : 1;
}
