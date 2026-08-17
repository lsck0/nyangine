/**
 * THIS FILE WAS CLANKER WANKED !!!
 **/

#include "nyangine/nyangine.c"
#include "nyangine/nyangine.h"

s32 main(void) {
  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: Basic timestamp retrieval
  // ─────────────────────────────────────────────────────────────────────────────
  u64 t1 = nya_clock_get_timestamp_ms();
  nya_assert(t1 > 0);

  u64 t2 = nya_clock_get_timestamp_ms();
  nya_assert(t2 >= t1);

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: Timestamps are monotonically increasing
  // ─────────────────────────────────────────────────────────────────────────────
  u64 prev = nya_clock_get_timestamp_ms();
  for (u32 i = 0; i < 100; ++i) {
    u64 curr = nya_clock_get_timestamp_ms();
    nya_assert(curr >= prev);
    prev = curr;
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: Elapsed time measurement (spin wait)
  // ─────────────────────────────────────────────────────────────────────────────
  u64 start = nya_clock_get_timestamp_ms();
  // Spin until at least 10ms have passed
  while ((nya_clock_get_timestamp_ms() - start) < 10) {
    // busy wait
  }
  u64 end     = nya_clock_get_timestamp_ms();
  u64 elapsed = end - start;

  // Should have elapsed at least 10ms
  nya_assert(elapsed >= 10);
  nya_assert(elapsed < 1000); // Sanity check

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: Multiple consecutive calls produce consistent results
  // ─────────────────────────────────────────────────────────────────────────────
  u64 times[10];
  for (u32 i = 0; i < 10; ++i) { times[i] = nya_clock_get_timestamp_ms(); }
  for (u32 i = 1; i < 10; ++i) { nya_assert(times[i] >= times[i - 1]); }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: Timestamp is within reasonable range (since 2020)
  // ─────────────────────────────────────────────────────────────────────────────
  u64 now          = nya_clock_get_timestamp_ms();
  u64 year_2020_ms = 1577836800000ULL; // Jan 1, 2020 in milliseconds
  u64 year_2050_ms = 2524608000000ULL; // Jan 1, 2050 in milliseconds
  nya_assert(now > year_2020_ms);
  nya_assert(now < year_2050_ms);

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: the monotonic clock
  // ─────────────────────────────────────────────────────────────────────────────
  //
  // Every duration in the engine used to be two wall clock readings subtracted. That is fine until
  // the system clock moves: a backward NTP step makes the later reading the smaller one, and since
  // these are u64 the subtraction wraps to something near 2^64 rather than going negative. With
  // -fsanitize=unsigned-integer-overflow and -fno-sanitize-recover=all, that aborts the process.
  //
  // A test cannot step the system clock without CAP_SYS_TIME, so what is pinned here is the
  // contract that makes the hazard impossible: a separate clock, never decreasing, with an epoch of
  // its own. The callers that measure durations — frame timing, uptime, the profiler, the NEAT
  // budget, subprocess run time — are all on it now.
  printf("TEST: monotonic clock\n");
  {
    u64 m1 = nya_clock_get_monotonic_ns();
    nya_assert(m1 > 0, "the monotonic clock read zero");

    // Never decreasing, sampled hard enough to catch a counter that wraps or resets.
    u64 previous = nya_clock_get_monotonic_ns();
    for (u32 i = 0; i < 10000; ++i) {
      u64 current = nya_clock_get_monotonic_ns();
      nya_assert(current >= previous, "the monotonic clock went backwards");
      previous = current;
    }

    // A real duration comes back, and the three resolutions agree about it.
    u64 monotonic_start = nya_clock_get_monotonic_ns();
    u64 wall_start      = nya_clock_get_timestamp_ms();
    while ((nya_clock_get_timestamp_ms() - wall_start) < 10) {
      // busy wait
    }
    u64 monotonic_elapsed_ns = nya_clock_get_monotonic_ns() - monotonic_start;

    nya_assert(monotonic_elapsed_ns >= nya_time_ms_to_ns(9), "monotonic reported less than the 10ms that were waited");
    nya_assert(monotonic_elapsed_ns < nya_time_ms_to_ns(2000), "monotonic reported an implausible duration");

    // ms and µs are the same clock at coarser resolution, so they must bracket the ns reading rather
    // than being an independent counter. Sampled around it, so scheduling cannot invert them.
    u64 before_ms = nya_clock_get_monotonic_ms();
    u64 middle_ns = nya_clock_get_monotonic_ns();
    u64 after_µs  = nya_clock_get_monotonic_µs();

    nya_assert(nya_time_ms_to_ns(before_ms) <= middle_ns + nya_time_ms_to_ns(1), "monotonic ms disagrees with ns");
    nya_assert(middle_ns <= nya_time_µs_to_ns(after_µs) + nya_time_ms_to_ns(1), "monotonic µs disagrees with ns");

    // A different epoch from the wall clock, which is the whole point: a monotonic reading is
    // meaningless on its own and must never be mistaken for a Unix timestamp. The wall clock is past
    // 2020, i.e. > 1.5e18 ns; a monotonic clock counting since boot is far below that.
    u64 monotonic_now = nya_clock_get_monotonic_ns();
    nya_assert(monotonic_now < nya_time_ms_to_ns(year_2020_ms), "the monotonic clock is using the Unix epoch");
  }
  printf("  PASSED\n");

  return 0;
}
