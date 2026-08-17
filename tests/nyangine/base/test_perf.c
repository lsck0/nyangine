/**
 * THIS FILE WAS CLANKER WANKED !!!
 **/

/*
 * Switches the profiler on for this translation unit, before the engine is included.
 *
 * base_perf.h compiles the timers into development builds only, and a test build is mode 4. This
 * file used to be one `#if NYA_DEBUG` wrapping its entire body, so in the only mode it ever runs in
 * it asserted nothing whatsoever and still reported a pass — the coverage gap base_perf.h's own
 * docblock complains about, which is why NYA_PERF_FORCE_DEBUG was added in the first place. Nothing
 * had picked it up.
 *
 * Defined here rather than added to FLAGS_TEST, so the rest of the suite keeps measuring a build
 * shaped like the one it is testing.
 */
#define NYA_PERF_FORCE_DEBUG

#include "nyangine/nyangine.c"
#include "nyangine/nyangine.h"

#if !NYA_PERF_ENABLED
#error "test_perf.c requires the perf timers; NYA_PERF_FORCE_DEBUG should have switched them on."
#endif

static void sleep_ms(s64 ms) {
  struct timespec ts = { .tv_sec = ms / 1000, .tv_nsec = (ms % 1000L) * 1000000L };
  nanosleep(&ts, nullptr);
}

s32 main(void) {

  nya_perf_time_this_function();

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: Timer creation and retrieval
  // ─────────────────────────────────────────────────────────────────────────────
  NYA_PerfMeasurement* measurement = nya_perf_timer_get("nonexistent");
  nya_assert(measurement == nullptr);

  nya_perf_timer_start("test_timer");
  measurement = nya_perf_timer_get("test_timer");
  nya_assert(measurement != nullptr);
  nya_assert(nya_string_equals(measurement->name, "test_timer"));
  nya_assert(measurement->is_running == true);
  nya_perf_timer_stop("test_timer");
  nya_assert(measurement->is_running == false);

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: Timer records elapsed time
  // ─────────────────────────────────────────────────────────────────────────────
  nya_perf_timer_start("sleep_timer");
  sleep_ms(10);
  nya_perf_timer_stop("sleep_timer");

  NYA_PerfMeasurement* sleep_measurement = nya_perf_timer_get("sleep_timer");
  nya_assert(sleep_measurement != nullptr);
  nya_assert(sleep_measurement->last_elapsed_ms >= 9);
  nya_assert(sleep_measurement->elapsed_cycles[0] > 0);

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: Multiple samples wrap around
  // ─────────────────────────────────────────────────────────────────────────────
  for (u32 i = 0; i < NYA_PERF_MEASUREMENT_SAMPLES + 5; ++i) {
    nya_perf_timer_start("wrap_timer");
    nya_perf_timer_stop("wrap_timer");
  }
  NYA_PerfMeasurement* wrap_measurement = nya_perf_timer_get("wrap_timer");
  nya_assert(wrap_measurement != nullptr);
  nya_assert(wrap_measurement->current == (NYA_PERF_MEASUREMENT_SAMPLES + 5 - 1) % NYA_PERF_MEASUREMENT_SAMPLES);

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: Get all timers
  // ─────────────────────────────────────────────────────────────────────────────
  NYA_ArrayᐸNYA_PerfMeasurementᐳ* all_timers = nya_perf_timer_get_all();
  nya_assert(all_timers != nullptr);
  nya_assert(all_timers->length >= 3);

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: Scoped timer using cleanup attribute
  // ─────────────────────────────────────────────────────────────────────────────
  {
    nya_perf_time_this_scope("scoped_timer");
    sleep_ms(5);
  }
  NYA_PerfMeasurement* scoped_measurement = nya_perf_timer_get("scoped_timer");
  nya_assert(scoped_measurement != nullptr);
  nya_assert(scoped_measurement->is_running == false);
  nya_assert(scoped_measurement->last_elapsed_ms >= 4);

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: Restarting same timer updates values correctly
  // ─────────────────────────────────────────────────────────────────────────────
  nya_perf_timer_start("restart_timer");
  sleep_ms(5);
  nya_perf_timer_stop("restart_timer");

  NYA_PerfMeasurement* restart_measurement = nya_perf_timer_get("restart_timer");
  u64                  first_elapsed       = restart_measurement->last_elapsed_ms;
  nya_assert(first_elapsed >= 4);

  nya_perf_timer_start("restart_timer");
  sleep_ms(15);
  nya_perf_timer_stop("restart_timer");

  u64 second_elapsed = restart_measurement->last_elapsed_ms;
  nya_assert(second_elapsed >= 14);
  nya_assert(second_elapsed > first_elapsed);

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: Cycles are recorded and positive
  // ─────────────────────────────────────────────────────────────────────────────
  nya_perf_timer_start("cycles_timer");
  volatile u64 sum = 0;
  for (u64 i = 0; i < 100000; ++i) { sum += i; }
  (void)sum;
  nya_perf_timer_stop("cycles_timer");

  NYA_PerfMeasurement* cycles_measurement = nya_perf_timer_get("cycles_timer");
  nya_assert(cycles_measurement != nullptr);
  nya_assert(cycles_measurement->elapsed_cycles[0] > 0);
  nya_assert(cycles_measurement->started_cycles[0] < cycles_measurement->ended_cycles[0]);

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: Multiple concurrent timers
  // ─────────────────────────────────────────────────────────────────────────────
  nya_perf_timer_start("concurrent_a");
  sleep_ms(5);
  nya_perf_timer_start("concurrent_b");
  sleep_ms(5);
  nya_perf_timer_stop("concurrent_a");
  sleep_ms(5);
  nya_perf_timer_stop("concurrent_b");

  NYA_PerfMeasurement* conc_a = nya_perf_timer_get("concurrent_a");
  NYA_PerfMeasurement* conc_b = nya_perf_timer_get("concurrent_b");
  nya_assert(conc_a != nullptr && conc_b != nullptr);
  nya_assert(conc_a->last_elapsed_ms >= 9); // ran for ~10ms
  nya_assert(conc_b->last_elapsed_ms >= 9); // ran for ~10ms
  // A started before B, so A should have ended before B finished
  nya_assert(conc_a->last_elapsed_ms < conc_b->last_elapsed_ms + 5);

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: Timer with zero-duration work
  // ─────────────────────────────────────────────────────────────────────────────
  nya_perf_timer_start("zero_duration");
  nya_perf_timer_stop("zero_duration");
  NYA_PerfMeasurement* zero_m = nya_perf_timer_get("zero_duration");
  nya_assert(zero_m != nullptr);
  // last_elapsed_ms might be 0 or very small, but cycles should still be recorded
  nya_assert(zero_m->elapsed_cycles[0] >= 0);

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: Timer names with various characters
  // ─────────────────────────────────────────────────────────────────────────────
  nya_perf_timer_start("timer_with_underscores");
  nya_perf_timer_stop("timer_with_underscores");
  nya_assert(nya_perf_timer_get("timer_with_underscores") != nullptr);

  nya_perf_timer_start("a");
  nya_perf_timer_stop("a");
  nya_assert(nya_perf_timer_get("a") != nullptr);

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: Many sequential timer samples
  // ─────────────────────────────────────────────────────────────────────────────
  for (u32 i = 0; i < 100; ++i) {
    nya_perf_timer_start("many_samples");
    nya_perf_timer_stop("many_samples");
  }
  NYA_PerfMeasurement* many = nya_perf_timer_get("many_samples");
  nya_assert(many != nullptr);
  nya_assert(many->current == (100 - 1) % NYA_PERF_MEASUREMENT_SAMPLES);

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: Lookup finds a timer by contents, not only by pointer identity
  // ─────────────────────────────────────────────────────────────────────────────
  //
  // _nya_perf_timer_get compares the name pointer before falling back to a contents comparison,
  // because nearly every name reaching it is a pooled literal or __FUNCTION__. That fast path is
  // only correct while the fallback still runs, so this looks a known timer up through a name the
  // compiler cannot have pooled with the literal above.
  NYA_Arena*  runtime_arena = nya_arena_create();
  NYA_String* built_name    = nya_string_create(runtime_arena);
  nya_string_extend(built_name, "many_");
  nya_string_extend(built_name, "samples");

  // Arena memory, so it cannot be the pooled literal the timer was registered under, which is the
  // whole point of looking it up this way.
  NYA_CString built_cstring = nya_string_to_cstring(runtime_arena, built_name);
  nya_assert(nya_perf_timer_get(built_cstring) == many);

  // And a name that matches nothing still misses, rather than the pointer scan falling through into
  // the wrong entry.
  nya_assert(nya_perf_timer_get("many_samples_") == nullptr);

  nya_arena_destroy(runtime_arena);

  return 0;
}
