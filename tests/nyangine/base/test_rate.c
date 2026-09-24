/**
 * The outbound budget: what a token bucket allows, what a server's own answer overrides, and what a
 * retry waits.
 *
 * The clock here is the real one, so the waits asserted on are milliseconds rather than seconds and
 * every bound is checked with slack on both sides. What is *not* slack is the ordering: a wait the
 * server asked for is never shortened, and a bucket that is still spent is never given away.
 **/

#include "nyangine/nyangine.c"
#include "nyangine/nyangine.h"

s32 main(void) {
  setvbuf(stdout, nullptr, _IONBF, 0);

  NYA_Arena* arena = nya_arena_create(.name = "test_rate");
  defer      nya_arena_destroy(arena);

  // TEST: what a limiter refuses to be.
  {
    NYA_RateLimiter* limiter = nullptr;

    nya_check(!nya_rate_limiter_create(arena, &limiter, .per_second = 0.0).ok, "a rate of zero is not a limiter");
    nya_check(!nya_rate_limiter_create(arena, &limiter, .per_second = -1.0).ok, "and neither is a negative one");
    nya_check(nya_rate_limiter_create(arena, &limiter, .per_second = 10.0).ok, "a positive rate is");
    nya_check(nya_rate_bucket_count(limiter) == 0, "holding nothing until it is asked");

    u64 wait_ms = 0;
    nya_check(nya_rate_take(limiter, "something", &wait_ms), "and a first call on a fresh key always goes");

    // a reset forgets every bucket without giving the arena's memory back, which is what an arena means.
    nya_rate_limiter_destroy(limiter);

    nya_check(nya_rate_bucket_count(limiter) == 0, "after which it holds nothing again, got %u", nya_rate_bucket_count(limiter));
  }

  // TEST: a burst goes, and then the budget is the budget.
  {
    NYA_RateLimiter* limiter = nullptr;
    NYA_EXPECT(nya_rate_limiter_create(arena, &limiter, .per_second = 100.0, .burst = 3.0));

    u64 wait_ms = 0;

    nya_check(nya_rate_take(limiter, "one", &wait_ms), "the first of three goes");
    nya_check(nya_rate_take(limiter, "one", &wait_ms), "and the second");
    nya_check(nya_rate_take(limiter, "one", &wait_ms), "and the third");

    nya_check(!nya_rate_take(limiter, "one", &wait_ms), "the fourth does not");
    nya_check(wait_ms > 0 && wait_ms <= 20, "and says how long, got %llu ms", (unsigned long long)wait_ms);

    // another key is another budget, which is the whole reason a limiter holds buckets by name.
    nya_check(nya_rate_take(limiter, "two", &wait_ms), "a different bucket is untouched");

    nya_check(nya_rate_bucket_count(limiter) == 2, "two buckets exist, got %u", nya_rate_bucket_count(limiter));

    // and asking costs nothing: the budget is where it was afterwards.
    u64 before = nya_rate_wait_for(limiter, "one");
    u64 after  = nya_rate_wait_for(limiter, "one");

    nya_check(before > 0 && after > 0, "a spent bucket says it is spent without being asked to spend");
    nya_check(after <= before, "and asking twice does not make it worse, got %llu then %llu", (unsigned long long)before,
              (unsigned long long)after);

    // waiting it out is what an outbound caller actually does, and it comes back able to go.
    u64 waited = nya_rate_wait(limiter, "one");

    nya_check(waited > 0 && waited < 200, "waiting takes about the wait, got %llu ms", (unsigned long long)waited);
  }

  // TEST: the server is right, and is never argued with.
  {
    NYA_RateLimiter* limiter = nullptr;
    NYA_EXPECT(nya_rate_limiter_create(arena, &limiter, .per_second = 1000.0, .burst = 1000.0));

    u64 wait_ms = 0;
    nya_check(nya_rate_take(limiter, "api", &wait_ms), "a full bucket goes");

    // a 429 said to wait, so nothing goes, however full this program believed the bucket was.
    nya_rate_told(limiter, "api", 400);

    nya_check(!nya_rate_take(limiter, "api", &wait_ms), "what the server said outranks the local arithmetic");
    nya_check(wait_ms > 300 && wait_ms <= 400, "for about as long as it asked, got %llu ms", (unsigned long long)wait_ms);

    // a second answer that asks for less does not shorten the first: replies arrive out of order.
    nya_rate_told(limiter, "api", 10);

    u64 still = nya_rate_wait_for(limiter, "api");
    nya_check(still > 300, "a shorter answer never shortens a longer one, got %llu ms", (unsigned long long)still);

    // and a longer one does lengthen it.
    nya_rate_told(limiter, "api", 800);
    nya_check(nya_rate_wait_for(limiter, "api") > still, "while a longer one does");
  }

  // TEST: the headers an API publishes on every reply, not only a refused one.
  {
    NYA_RateLimiter* limiter = nullptr;
    NYA_EXPECT(nya_rate_limiter_create(arena, &limiter, .per_second = 1000.0, .burst = 1000.0));

    u64 wait_ms = 0;
    nya_check(nya_rate_take(limiter, "api", &wait_ms), "a call goes");

    // "you have five left" — believed, because it is the server's own count.
    nya_rate_observed(limiter, "api", 5.0, 1000);

    for (u32 index = 0; index < 5; index++) {
      nya_check(nya_rate_take(limiter, "api", &wait_ms), "five more go, this is number %u", index + 1);
    }

    nya_check(!nya_rate_take(limiter, "api", &wait_ms), "and the sixth does not");

    // "you have a thousand left" arriving late does not undo it.
    nya_rate_observed(limiter, "api", 1000.0, 0);
    nya_check(!nya_rate_take(limiter, "api", &wait_ms), "a fuller count from the past is not believed");

    // nothing left with a window to wait out is the same fact a 429 carries, before the 429.
    NYA_RateLimiter* second = nullptr;
    NYA_EXPECT(nya_rate_limiter_create(arena, &second, .per_second = 1000.0, .burst = 1000.0));

    nya_rate_observed(second, "api", 0.0, 500);

    u64 held = nya_rate_wait_for(second, "api");
    nya_check(held > 400 && held <= 500, "an empty bucket waits out its window, got %llu ms", (unsigned long long)held);
  }

  // TEST: a table full of spent buckets gives none of them away.
  {
    NYA_RateLimiter* limiter = nullptr;
    NYA_EXPECT(nya_rate_limiter_create(arena, &limiter, .per_second = 1.0, .burst = 1.0));

    char key[NYA_RATE_MAX_KEY] = { 0 };

    for (u32 index = 0; index < NYA_RATE_MAX_BUCKETS; index++) {
      (void)snprintf(key, sizeof(key), "bucket-%u", index);

      u64 wait_ms = 0;
      nya_check(nya_rate_take(limiter, key, &wait_ms), "bucket %u spends its one call", index);
    }

    nya_check(nya_rate_bucket_count(limiter) == NYA_RATE_MAX_BUCKETS, "the table is full, got %u", nya_rate_bucket_count(limiter));

    // a newcomer waits with them rather than taking a slot: a fresh full bucket under pressure is how
    // a limiter stops limiting.
    u64 wait_ms = 0;
    nya_check(!nya_rate_take(limiter, "newcomer", &wait_ms), "and a new key gets no fresh budget out of it");

    // every bucket that was there is still spent, which is the thing being protected.
    nya_check(nya_rate_wait_for(limiter, "bucket-0") > 0, "the first bucket is still spent");
  }

  // TEST: the backoff, which is a different question from the budget.
  {
    // full jitter by default, so every wait is somewhere inside its window and not at the top of it.
    b8 any_below_half = false;

    for (u32 index = 0; index < 32; index++) {
      u64 wait_ms = nya_backoff_ms(3, .base_ms = 100, .cap_ms = 10000);

      nya_check(wait_ms <= 800, "attempt three is inside its window, got %llu ms", (unsigned long long)wait_ms);

      if (wait_ms < 400) any_below_half = true;
    }

    nya_check(any_below_half, "and is randomised rather than always the whole window");

    // it doubles, and it stops.
    u64 capped = nya_backoff_ms(20, .base_ms = 100, .cap_ms = 5000, .jitter = 0.0001);
    nya_check(capped >= 4000 && capped <= 5000, "a large attempt saturates at the cap, got %llu ms", (unsigned long long)capped);

    u64 enormous = nya_backoff_ms(4000000000U, .base_ms = 100, .cap_ms = 5000, .jitter = 0.0001);
    nya_check(enormous <= 5000, "and an attempt past the width of the shift does too, got %llu ms", (unsigned long long)enormous);

    // no jitter at all is the whole window, which is what a caller asking for none means.
    u64 fixed = nya_backoff_ms(0, .base_ms = 250, .cap_ms = 10000, .jitter = 0.0001);
    nya_check(fixed >= 249 && fixed <= 250, "no jitter is the window itself, got %llu ms", (unsigned long long)fixed);
  }

  // TEST: what is worth sending again, and what is the same answer twice.
  {
    nya_check(nya_retry_is_worthwhile(0), "no answer at all is worth another go");
    nya_check(nya_retry_is_worthwhile(429), "and being told to slow down");
    nya_check(nya_retry_is_worthwhile(408) && nya_retry_is_worthwhile(425), "and a timeout, and too early");
    nya_check(nya_retry_is_worthwhile(500) && nya_retry_is_worthwhile(503), "and the server having a bad minute");

    nya_check(!nya_retry_is_worthwhile(200), "a success is not a retry");
    nya_check(!nya_retry_is_worthwhile(400), "a bad request sent again is the same bad request");
    nya_check(!nya_retry_is_worthwhile(401), "and an unauthorised one is how a token gets locked");
    nya_check(!nya_retry_is_worthwhile(404), "and there is still nothing there");
  }

  return nya_check_failures() == 0 ? 0 : 1;
}
