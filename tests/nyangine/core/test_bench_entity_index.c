/**
 * How long a kind query costs, and what it scales with.
 *
 * Not a correctness test — test_entity covers that. This exists to keep an honest number attached to
 * the index: the whole reason it replaced a linear scan was cost, and a claim about cost that nobody
 * ever measures is a claim that quietly stops being true.
 **/

#include "nyangine/nyangine.c"
#include "nyangine/nyangine.h"

#include "SDL3/SDL_init.h"

#define BENCH_ITERATIONS 20000

static u64 bench_kind(u32 kind, u32 expected) {
  u64 started = nya_clock_get_monotonic_ns();

  u32 seen = 0;
  for (u32 i = 0; i < BENCH_ITERATIONS; i++) {
    nya_entity_foreach_kind (kind, entity) {
      nya_unused(entity);
      seen++;
    }
  }

  u64 elapsed = nya_clock_get_monotonic_ns() - started;

  nya_assert(seen == expected * BENCH_ITERATIONS, "the query found %u, expected %u", seen, expected * BENCH_ITERATIONS);

  return elapsed / BENCH_ITERATIONS;
}

s32 main(void) {
  _NYA_APP_INSTANCE = (NYA_App){ .initialized = true };
  b8 sdl_ok         = SDL_Init(0);
  nya_assert(sdl_ok, "SDL_Init failed: %s", SDL_GetError());

  nya_system_callback_init();

  NYA_World* world = nya_world_create();
  (void)nya_world_set(world);

  defer nya_world_destroy(world);
  defer nya_system_callback_deinit();

  enum { KIND_CRATE = 1, KIND_CAMERA = 2 };

  // The shape the demo actually has: a great many of one thing, a couple of another. Finding the
  // rare one is what every system does every tick.
  nya_info("entities  ns/query(rare)  ns/query(common)");

  const u32 populations[] = { 8, 64, 512, 4096 };

  for (u32 p = 0; p < sizeof(populations) / sizeof(populations[0]); p++) {
    nya_entity_clear();

    u32 crates = populations[p];

    for (u32 i = 0; i < crates; i++) {
      NYA_EntityHandle crate = nya_entity_spawn(.name = "crate", .type = KIND_CRATE);
      nya_assert(nya_entity_is_valid(crate));
    }

    NYA_EntityHandle camera = nya_entity_spawn(.name = "camera", .type = KIND_CAMERA);
    nya_assert(nya_entity_is_valid(camera));

    u64 rare   = bench_kind(KIND_CAMERA, 1);
    u64 common = bench_kind(KIND_CRATE, crates);

    nya_info("%8u  %14" PRIu64 "  %16" PRIu64, crates, rare, common);
  }

  nya_entity_clear();

  nya_info("PASSED: test_bench_entity_index");

  return EXIT_SUCCESS;
}
