/**
 * The circuit breaker: a run of failures trips it, an open breaker fails fast, and a probe after the
 * cooldown either heals it or trips it straight back.
 *
 * The clock is injected through the internal `_at` entry points, so the cooldown is tested exactly
 * rather than by sleeping: `open_ms` becomes a number of nanoseconds this test steps a local `now`
 * across, and every transition is asserted at a known instant.
 **/

#include "nyangine/nyangine.c"
#include "nyangine/nyangine.h"

#define MS 1000000ULL

s32 main(void) {
  setvbuf(stdout, nullptr, _IONBF, 0);

  NYA_Arena* arena = nya_arena_create(.name = "test_circuit");
  defer      nya_arena_destroy(arena);

  // TEST: a fresh breaker is closed and lets everything through.
  {
    NYA_CircuitBreaker* breaker = nullptr;
    nya_check(nya_circuit_breaker_create(arena, &breaker, .failure_threshold = 3, .open_ms = 1000).ok, "a breaker builds");
    nya_check(nya_circuit_key_count(breaker) == 0, "holding nothing until asked");

    u64 now = 0;
    nya_check(_nya_circuit_allow_at(breaker, "up", now), "a first call on a fresh key always goes");
    nya_check(nya_circuit_state(breaker, "unseen") == NYA_CIRCUIT_CLOSED, "a key never seen reads closed");
  }

  // TEST: failures below the threshold do not trip, and a success ends the run.
  {
    NYA_CircuitBreaker* breaker = nullptr;
    (void)nya_circuit_breaker_create(arena, &breaker, .failure_threshold = 3, .open_ms = 1000);

    u64 now = 0;
    for (u32 i = 0; i < 2; i++) {
      nya_check(_nya_circuit_allow_at(breaker, "up", now), "still closed after %u failures", i);
      _nya_circuit_record_at(breaker, "up", false, now);
    }
    // one success resets the consecutive-failure run, so the next two failures still do not trip.
    nya_check(_nya_circuit_allow_at(breaker, "up", now), "closed before the success");
    _nya_circuit_record_at(breaker, "up", true, now);
    for (u32 i = 0; i < 2; i++) {
      nya_check(_nya_circuit_allow_at(breaker, "up", now), "the success cleared the run, so failure %u does not trip", i);
      _nya_circuit_record_at(breaker, "up", false, now);
    }
  }

  // TEST: the threshold-th consecutive failure trips OPEN, and OPEN fails fast.
  {
    NYA_CircuitBreaker* breaker = nullptr;
    (void)nya_circuit_breaker_create(arena, &breaker, .failure_threshold = 3, .open_ms = 1000);

    u64 now = 0;
    for (u32 i = 0; i < 3; i++) {
      nya_check(_nya_circuit_allow_at(breaker, "up", now), "closed while failing, before the trip");
      _nya_circuit_record_at(breaker, "up", false, now);
    }
    nya_check(!_nya_circuit_allow_at(breaker, "up", now), "the third consecutive failure trips it open");
    nya_check(_nya_circuit_state_at(breaker, "up", now) == NYA_CIRCUIT_OPEN, "and it reads open");

    // still open just before the cooldown ends.
    nya_check(!_nya_circuit_allow_at(breaker, "up", now + 999 * MS), "open until the cooldown elapses");
  }

  // TEST: after the cooldown one probe goes (half_open_max = 1), the rest wait, a probe success closes it.
  {
    NYA_CircuitBreaker* breaker = nullptr;
    (void)nya_circuit_breaker_create(arena, &breaker, .failure_threshold = 1, .success_threshold = 1, .open_ms = 1000);

    u64 now = 0;
    nya_check(_nya_circuit_allow_at(breaker, "up", now), "the call that then fails goes");
    _nya_circuit_record_at(breaker, "up", false, now);   // threshold 1: one failure trips
    nya_check(!_nya_circuit_allow_at(breaker, "up", now), "open right after the trip");

    u64 after = now + 1000 * MS;
    nya_check(_nya_circuit_allow_at(breaker, "up", after), "the first call past the cooldown is the half-open probe");
    nya_check(!_nya_circuit_allow_at(breaker, "up", after), "and a second probe is refused while the first is unresolved");

    _nya_circuit_record_at(breaker, "up", true, after);
    nya_check(_nya_circuit_state_at(breaker, "up", after) == NYA_CIRCUIT_CLOSED, "a good probe closes the breaker");
    nya_check(_nya_circuit_allow_at(breaker, "up", after), "and calls flow again");
  }

  // TEST: a failed probe trips straight back OPEN for another cooldown.
  {
    NYA_CircuitBreaker* breaker = nullptr;
    (void)nya_circuit_breaker_create(arena, &breaker, .failure_threshold = 1, .open_ms = 1000);

    u64 now = 0;
    nya_check(_nya_circuit_allow_at(breaker, "up", now), "the call that fails goes");
    _nya_circuit_record_at(breaker, "up", false, now);
    u64 probe = now + 1000 * MS;
    nya_check(_nya_circuit_allow_at(breaker, "up", probe), "the probe goes");
    _nya_circuit_record_at(breaker, "up", false, probe);   // probe fails
    nya_check(!_nya_circuit_allow_at(breaker, "up", probe), "a failed probe re-opens at once");
    nya_check(!_nya_circuit_allow_at(breaker, "up", probe + 999 * MS), "for a fresh full cooldown measured from the probe");
    nya_check(_nya_circuit_allow_at(breaker, "up", probe + 1000 * MS), "then a new probe is allowed");
  }

  // TEST: half_open_max lets that many probes through at once.
  {
    NYA_CircuitBreaker* breaker = nullptr;
    (void)nya_circuit_breaker_create(arena, &breaker, .failure_threshold = 1, .success_threshold = 3, .half_open_max = 2, .open_ms = 1000);

    u64 now = 0;
    nya_check(_nya_circuit_allow_at(breaker, "up", now), "the call that fails goes");
    _nya_circuit_record_at(breaker, "up", false, now);
    u64 after = now + 1000 * MS;
    nya_check(_nya_circuit_allow_at(breaker, "up", after), "first probe");
    nya_check(_nya_circuit_allow_at(breaker, "up", after), "second probe within half_open_max");
    nya_check(!_nya_circuit_allow_at(breaker, "up", after), "third refused past half_open_max");
  }

  // TEST: tripping is per key.
  {
    NYA_CircuitBreaker* breaker = nullptr;
    (void)nya_circuit_breaker_create(arena, &breaker, .failure_threshold = 1, .open_ms = 1000);

    u64 now = 0;
    nya_check(_nya_circuit_allow_at(breaker, "down", now), "the call that fails goes");
    _nya_circuit_record_at(breaker, "down", false, now);
    nya_check(!_nya_circuit_allow_at(breaker, "down", now), "the failing key is open");
    nya_check(_nya_circuit_allow_at(breaker, "healthy", now), "a different key is unaffected");
    nya_check(nya_circuit_key_count(breaker) == 2, "two keys tracked, got %u", nya_circuit_key_count(breaker));
  }

  // TEST: a 4xx-style success (the dependency answered) never trips it.
  {
    NYA_CircuitBreaker* breaker = nullptr;
    (void)nya_circuit_breaker_create(arena, &breaker, .failure_threshold = 2, .open_ms = 1000);

    u64 now = 0;
    // the caller decides a well-formed 4xx is a success for the breaker; a long run of them never trips.
    for (u32 i = 0; i < 20; i++) {
      nya_check(_nya_circuit_allow_at(breaker, "up", now), "a working dependency stays closed");
      _nya_circuit_record_at(breaker, "up", true, now);
    }
  }

  // TEST: the public API on the real clock, and destroy resets it.
  {
    NYA_CircuitBreaker* breaker = nullptr;
    (void)nya_circuit_breaker_create(arena, &breaker, .failure_threshold = 2, .open_ms = 30000);

    nya_check(nya_circuit_allow(breaker, "real"), "the public allow lets a fresh key through");
    nya_circuit_record(breaker, "real", false);
    nya_check(nya_circuit_allow(breaker, "real"), "one failure has not tripped it");
    nya_circuit_record(breaker, "real", false);
    nya_check(!nya_circuit_allow(breaker, "real"), "the second consecutive failure trips it on the real clock");
    nya_check(nya_circuit_state(breaker, "real") == NYA_CIRCUIT_OPEN, "and the public state reads open");

    nya_circuit_breaker_destroy(breaker);
    nya_check(nya_circuit_key_count(breaker) == 0, "destroy forgets every key, got %u", nya_circuit_key_count(breaker));
    nya_check(nya_circuit_allow(breaker, "real"), "so the key is fresh and open again after a reset");
  }

  printf("test_circuit: all passed\n");
  return 0;
}
