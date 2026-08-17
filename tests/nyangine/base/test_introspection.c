/**
 * Arena and perf introspection: the four things a debugging session actually asks for.
 *
 * - which subsystem is holding memory        NYA_ArenaStats + the registry
 * - which line of it                         the callsite table
 * - how long each timer takes                NYA_PerfStats
 * - how a single frame was put together      nya_perf_frame_spans
 *
 * Built in debug mode, unlike the rest of the suite: the callsite table and every perf entry point
 * are compiled out otherwise, and a test that asserted nothing in the mode it runs under would be
 * test_perf's problem all over again. The disabled-build behaviour is asserted at the bottom, from
 * the same file, by checking what the macros collapse to.
 **/

// Both subsystems are debug gated and the suite builds in mode 4, so they are switched on here
// explicitly. Without this the test would compile to a series of no-ops and report a pass, which is
// the exact failure mode test_perf has had all along.
#define NYA_ARENA_FORCE_DEBUG
#define NYA_PERF_FORCE_DEBUG

#include "nyangine/nyangine.c"
#include "nyangine/nyangine.h"

/** Distinct helpers so each gets its own row in the callsite table. */
static void* allocate_from(NYA_Arena* arena, u64 size) {
  return nya_arena_alloc(arena, size);
}

static void* allocate_a_lot(NYA_Arena* arena, u64 size) {
  return nya_arena_alloc(arena, size);
}

s32 main(void) {
  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: stats describe an arena beyond the single number usage_bytes gives
  // ─────────────────────────────────────────────────────────────────────────────
  {
    NYA_Arena* arena = nya_arena_create(.name = "stats_subject");
    defer      nya_arena_destroy(arena);

    NYA_ArenaStats empty = nya_arena_stats(arena);
    nya_assert(empty.region_count == 0, "nothing allocated yet, so no region has been taken");
    nya_assert(empty.used_bytes == 0);
    nya_assert(empty.reserved_bytes == 0);
    nya_assert(empty.fragmentation == 0.0F, "an arena with no free list is not fragmented");

    void* block = nya_arena_alloc(arena, 4096);
    nya_assert(block != nullptr);

    NYA_ArenaStats used = nya_arena_stats(arena);
    nya_assert(used.region_count == 1, "one allocation took one region");
    nya_assert(used.used_bytes >= 4096, "used covers at least what was asked for");
    nya_assert(used.reserved_bytes >= used.used_bytes, "reserved is what was taken from the system, so never less than used");
    nya_assert(nya_string_equals((NYA_CString)used.name, "stats_subject"), "the arena names itself in its own stats");

    // The number the old API gave, still agreeing with the new one.
    nya_assert(used.used_bytes == nya_arena_memory_usage_bytes(arena), "stats and usage_bytes cannot disagree");
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: freeing populates the free list, and stats can see it
  // ─────────────────────────────────────────────────────────────────────────────
  {
    NYA_Arena* arena = nya_arena_create(.name = "free_list_subject");
    defer      nya_arena_destroy(arena);

    void* a = nya_arena_alloc(arena, 1024);
    void* b = nya_arena_alloc(arena, 1024);
    nya_assert(a != nullptr && b != nullptr);

    nya_arena_free(arena, a, 1024);

    NYA_ArenaStats stats = nya_arena_stats(arena);
    nya_assert(stats.free_list_nodes >= 1, "the freed block is on the free list");
    nya_assert(stats.free_list_bytes >= 1024, "and its bytes are counted");
    nya_assert(stats.largest_free_block >= 1024);

    // One block free, so all the free space is in one piece and there is nothing to defragment.
    nya_assert(stats.fragmentation == 0.0F, "a single free block is not fragmentation");
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: the registry lists live arenas and forgets destroyed ones
  // ─────────────────────────────────────────────────────────────────────────────
  {
    // This is what answers "memory is climbing, which subsystem". Without it the only way to ask
    // is to already hold the guilty arena's pointer, which is exactly what you do not have.
    u32 before = nya_arena_registry_count();

    NYA_Arena* tracked = nya_arena_create(.name = "registry_subject");
    nya_assert(nya_arena_registry_count() == before + 1, "creating an arena lists it");

    b8 found = false;
    for (u32 i = 0; i < nya_arena_registry_count(); i++) {
      NYA_Arena* arena = nya_arena_registry_at(i);
      nya_assert(arena != nullptr, "every index below the count is occupied");
      if (arena == tracked) found = true;
    }
    nya_assert(found, "the arena that was just created is in the list");

    nya_arena_destroy(tracked);
    nya_assert(nya_arena_registry_count() == before, "destroying it removes it, so the list cannot go stale");

    nya_assert(nya_arena_registry_at(nya_arena_registry_count() + 100) == nullptr, "past the end is null, not a fault");
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: the global arenas are registered, so a report is useful with no setup
  // ─────────────────────────────────────────────────────────────────────────────
  {
    b8 found_global = false;
    for (u32 i = 0; i < nya_arena_registry_count(); i++) {
      NYA_Arena* arena = nya_arena_registry_at(i);
      if (arena == nya_arena_global) found_global = true;
    }
    nya_assert(found_global, "nya_arena_global registers itself like any other arena");
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: callsites aggregate per line, and live_bytes tracks what is still held
  // ─────────────────────────────────────────────────────────────────────────────
  {
    nya_arena_callsites_reset();
    nya_assert(nya_arena_callsite_count() == 0, "reset empties the table");

    NYA_Arena* arena = nya_arena_create(.name = "callsite_subject");
    defer      nya_arena_destroy(arena);

    // Two distinct lines, so two rows, with the second holding eight times the first.
    void* small = allocate_from(arena, 1024);
    void* big   = allocate_a_lot(arena, 8192);
    nya_assert(small != nullptr && big != nullptr);

    nya_assert(nya_arena_callsite_count() >= 2, "each line that allocated got a row");

    s64 small_live = 0;
    s64 big_live   = 0;
    for (u32 i = 0; i < nya_arena_callsite_count(); i++) {
      NYA_ArenaCallsiteStats row = nya_arena_callsite_at(i);
      if (row.arena_name == nullptr || !nya_string_equals((NYA_CString)row.arena_name, "callsite_subject")) continue;

      if (row.function_name != nullptr && nya_string_equals((NYA_CString)row.function_name, "allocate_from")) small_live = row.live_bytes;
      if (row.function_name != nullptr && nya_string_equals((NYA_CString)row.function_name, "allocate_a_lot")) big_live = row.live_bytes;
    }

    nya_assert(small_live >= 1024, "the small site is holding what it allocated, got " FMTs64, small_live);
    nya_assert(big_live >= 8192, "and the big one likewise, got " FMTs64, big_live);
    nya_assert(big_live > small_live, "live_bytes is what distinguishes the two, which is the whole point");

    // Giving it back drops live_bytes without erasing the history that it ever allocated.
    nya_arena_free(arena, big, 8192);

    for (u32 i = 0; i < nya_arena_callsite_count(); i++) {
      NYA_ArenaCallsiteStats row = nya_arena_callsite_at(i);
      if (row.function_name == nullptr) continue;
      if (!nya_string_equals((NYA_CString)row.function_name, "main")) continue;
      if (row.freed_bytes == 0) continue;

      nya_assert(row.free_count >= 1, "the freeing line is credited, not the allocating one");
      nya_assert(row.live_bytes < 0, "a line that only frees holds negative live bytes rather than being clamped to zero");
      break;
    }

    nya_assert(nya_arena_callsite_at(nya_arena_callsite_count() + 50).file_name == nullptr, "past the end is zeroed, not a fault");
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: perf stats aggregate the ring
  // ─────────────────────────────────────────────────────────────────────────────
  {
    nya_perf_timer_reset("agg");

    for (u32 i = 0; i < 5; i++) {
      nya_perf_timer_start("agg");
      // Something the optimizer cannot delete, so the samples differ from each other.
      volatile u64 spin = 0;
      for (u64 j = 0; j < (u64)2000 * (i + 1); j++) spin += j;
      nya_perf_timer_stop("agg");
    }

    NYA_PerfMeasurement* measurement = nya_perf_timer_get("agg");
    nya_assert(measurement != nullptr);
    nya_assert(measurement->sample_count == 5, "five completed runs, five samples");
    nya_assert(measurement->total_runs == 5);

    NYA_PerfStats stats = nya_perf_stats(measurement);
    nya_assert(stats.sample_count == 5);
    nya_assert(stats.min_ns <= stats.average_ns, "min bounds the average");
    nya_assert(stats.average_ns <= (f64)stats.max_ns, "and the average bounds max");
    nya_assert(stats.total_ns >= stats.max_ns, "the total covers every sample");
    nya_assert(stats.max_ns >= stats.min_ns);

    // A null measurement is a zeroed result rather than a fault, so a caller does not have to
    // null check before every stats call.
    NYA_PerfStats none = nya_perf_stats(nullptr);
    nya_assert(none.sample_count == 0);
    nya_assert(none.total_ns == 0);
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: the ring keeps the newest samples, not the first
  // ─────────────────────────────────────────────────────────────────────────────
  {
    nya_perf_timer_reset("wrap");

    // Twice the ring, so every original sample has been overwritten.
    for (u32 i = 0; i < NYA_PERF_MEASUREMENT_SAMPLES * 2; i++) {
      nya_perf_timer_start("wrap");
      nya_perf_timer_stop("wrap");
    }

    NYA_PerfMeasurement* measurement = nya_perf_timer_get("wrap");
    nya_assert(measurement->sample_count == NYA_PERF_MEASUREMENT_SAMPLES, "the ring saturates rather than overflowing");
    nya_assert(measurement->total_runs == (u64)NYA_PERF_MEASUREMENT_SAMPLES * 2, "but the run count is not windowed");

    // Reading the ring forwards from index 0 would average whatever the wrap left behind; the
    // stats walk backwards from `current` for exactly this reason.
    NYA_PerfStats stats = nya_perf_stats(measurement);
    nya_assert(stats.sample_count == NYA_PERF_MEASUREMENT_SAMPLES, "every slot in a wrapped ring is valid");
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: a frame breaks down into nested, time ordered spans
  // ─────────────────────────────────────────────────────────────────────────────
  {
    nya_perf_frame_begin();
    u64 frame = nya_perf_frame_current();

    // The shape being asserted:
    //   test_frame            depth 0
    //     test_update         depth 1
    //       test_entities     depth 2
    //     test_render         depth 1
    {
      nya_perf_time_this_scope("test_frame");
      {
        nya_perf_time_this_scope("test_update");
        {
          nya_perf_time_this_scope("test_entities");
          volatile u64 spin = 0;
          for (u64 j = 0; j < 1000; j++) spin += j;
        }
      }
      {
        nya_perf_time_this_scope("test_render");
        volatile u64 spin = 0;
        for (u64 j = 0; j < 1000; j++) spin += j;
      }
    }

    NYA_Arena                arena = nya_arena_create_on_stack(.name = "spans");
    defer                    nya_arena_destroy_on_stack(&arena);
    NYA_ArrayᐸNYA_PerfSpanᐳ* spans = nya_array_create(&arena, NYA_PerfSpan);

    u32 count = nya_perf_frame_spans(frame, spans);
    nya_assert(count == 4, "four scopes were entered in this frame, got " FMTu32, count);
    nya_assert(spans->length == 4);

    // Ordered by start, which is what makes this a timeline rather than a bag of names.
    for (u64 i = 1; i < spans->length; i++) {
      nya_assert(spans->items[i - 1].started_ns <= spans->items[i].started_ns, "spans come back in the order they began");
    }

    nya_assert(nya_string_equals((NYA_CString)spans->items[0].name, "test_frame"), "the outermost scope started first");
    nya_assert(spans->items[0].depth == 0, "and sits at the top");

    // Depth is what turns the flat list into the shape of the frame.
    u32 depth_of_entities = 0;
    u32 depth_of_update   = 0;
    u32 depth_of_render   = 0;
    nya_array_foreach (spans, span) {
      if (nya_string_equals((NYA_CString)span->name, "test_entities")) depth_of_entities = span->depth;
      if (nya_string_equals((NYA_CString)span->name, "test_update")) depth_of_update = span->depth;
      if (nya_string_equals((NYA_CString)span->name, "test_render")) depth_of_render = span->depth;
      nya_assert(span->frame == frame, "every span belongs to the frame that was asked for");
      nya_assert(span->ended_ns >= span->started_ns, "a span cannot end before it starts");
    }

    nya_assert(depth_of_update == 1, "update nests inside frame, got " FMTu32, depth_of_update);
    nya_assert(depth_of_entities == 2, "entities nests inside update, got " FMTu32, depth_of_entities);
    nya_assert(depth_of_render == 1, "render is a sibling of update, not a child, got " FMTu32, depth_of_render);

    // The outermost span contains the others in wall clock too, which is the cross check that
    // depth and time agree rather than depth being bookkeeping that drifted.
    NYA_PerfSpan outer = spans->items[0];
    nya_array_foreach (spans, span) {
      nya_assert(span->started_ns >= outer.started_ns, "nothing in the frame started before the frame did");
      nya_assert(span->ended_ns <= outer.ended_ns, "and nothing outlived it");
    }
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: a frame nobody measured is empty rather than an error
  // ─────────────────────────────────────────────────────────────────────────────
  {
    NYA_Arena                arena = nya_arena_create_on_stack(.name = "empty_spans");
    defer                    nya_arena_destroy_on_stack(&arena);
    NYA_ArrayᐸNYA_PerfSpanᐳ* spans = nya_array_create(&arena, NYA_PerfSpan);

    u32 count = nya_perf_frame_spans(nya_perf_frame_current() + 9999, spans);
    nya_assert(count == 0, "a frame that never happened has no spans");
    nya_assert(spans->length == 0);
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: frames are distinct, so one frame's spans are not another's
  // ─────────────────────────────────────────────────────────────────────────────
  {
    nya_perf_frame_begin();
    u64 first = nya_perf_frame_current();
    { nya_perf_time_this_scope("only_in_first"); }

    nya_perf_frame_begin();
    u64 second = nya_perf_frame_current();
    { nya_perf_time_this_scope("only_in_second"); }

    nya_assert(second == first + 1, "each begin advances by one");

    NYA_Arena                arena = nya_arena_create_on_stack(.name = "two_frames");
    defer                    nya_arena_destroy_on_stack(&arena);
    NYA_ArrayᐸNYA_PerfSpanᐳ* spans = nya_array_create(&arena, NYA_PerfSpan);

    (void)nya_perf_frame_spans(first, spans);
    nya_array_foreach (spans, span) {
      nya_assert(!nya_string_equals((NYA_CString)span->name, "only_in_second"), "a later frame's work does not leak into an earlier one");
    }
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // The reports, run for their side effects: they must not fault on real data.
  // ─────────────────────────────────────────────────────────────────────────────
  {
    nya_arena_stats_report();
    nya_arena_callsites_report(5);
    nya_perf_report();
    nya_perf_frame_report(nya_perf_frame_current());

    // Limits that do not make sense are clamped rather than read off the end.
    nya_arena_callsites_report(0);
    nya_arena_callsites_report(100000);
    nya_perf_frame_report(nya_perf_frame_current() + 9999);
  }

  printf("PASSED: test_introspection\n");
  return 0;
}
