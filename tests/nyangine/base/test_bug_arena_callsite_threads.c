/**
 * Regression test for concurrent updates to the arena callsite table (base_arena.c).
 *
 * An arena is not thread safe and is not shared here — each thread creates its own. The callsite
 * table behind them is process wide though, so every one of those threads records into it, and the
 * row count and the per row totals used to be plain u32/u64 incremented with no ordering at all.
 * Lost updates are the visible symptom: the totals come out below what was actually allocated.
 *
 * Every thread allocates from the same source line through the helper below, so all the traffic
 * lands on one site. Two threads that first reach an unseen site together may each reserve a row for
 * it — that is documented and deliberate — so the assertion is over the sum across every row for
 * that site rather than over a single row.
 *
 * NYA_ARENA_FORCE_DEBUG because the recording lives in the debug proxies, and a test build is
 * NYA_EXECUTION_MODE=4, which would otherwise compile all of this out and report zero rows.
 * */
#define NYA_ARENA_FORCE_DEBUG

#include "nyangine/nyangine.c"
#include "nyangine/nyangine.h"

#include "SDL3/SDL_init.h"

#define THREAD_COUNT      8
#define ALLOCS_PER_THREAD 256
#define ALLOC_SIZE        64

/** The one callsite every thread bills. Kept out of line so there is exactly one file/line pair. */
static void allocate_a_block(NYA_Arena* arena) {
  void* block = nya_arena_alloc(arena, ALLOC_SIZE);
  nya_assert(block != nullptr);
}

static s32 SDLCALL worker(void* data) {
  nya_unused(data);

  // Its own arena. The table is what is shared, not the allocator.
  NYA_Arena* arena = nya_arena_create(.name = "callsite_thread_arena", .region_size = nya_kibyte_to_byte(64UL));

  for (u32 i = 0; i < ALLOCS_PER_THREAD; i++) allocate_a_block(arena);

  nya_arena_destroy(arena);
  return 0;
}

s32 main(void) {
  b8 sdl_ok = SDL_Init(0);
  nya_assert(sdl_ok, "SDL_Init failed: %s", SDL_GetError());

  nya_arena_callsites_reset();

  SDL_Thread* threads[THREAD_COUNT];
  for (u32 i = 0; i < THREAD_COUNT; i++) {
    threads[i] = SDL_CreateThread(worker, "callsite_worker", nullptr);
    nya_assert(threads[i] != nullptr, "SDL_CreateThread failed: %s", SDL_GetError());
  }
  for (u32 i = 0; i < THREAD_COUNT; i++) SDL_WaitThread(threads[i], nullptr);

  // Summed over every row for the helper's line, since a site may legitimately hold more than one.
  u64 alloc_count     = 0;
  u64 allocated_bytes = 0;

  u32 rows = nya_arena_callsite_count();
  nya_assert(rows > 0, "no callsites recorded at all; the debug proxies did not run");

  for (u32 i = 0; i < rows; i++) {
    NYA_ArenaCallsiteStats row = nya_arena_callsite_at(i);
    if (row.function_name == nullptr) continue;
    if (strcmp(row.function_name, "allocate_a_block") != 0) continue;

    alloc_count     += row.alloc_count;
    allocated_bytes += row.allocated_bytes;
  }

  u64 expected_allocs = (u64)THREAD_COUNT * ALLOCS_PER_THREAD;
  u64 expected_bytes  = expected_allocs * ALLOC_SIZE;

  nya_assert(
      alloc_count == expected_allocs,
      "recorded " FMTu64 " allocations of " FMTu64 "; updates were lost to a race",
      alloc_count,
      expected_allocs
  );
  nya_assert(
      allocated_bytes == expected_bytes,
      "recorded " FMTu64 " bytes of " FMTu64 "; updates were lost to a race",
      allocated_bytes,
      expected_bytes
  );

  SDL_Quit();

  nya_info("PASSED: the callsite table survives concurrent recording");
  return 0;
}
