/**
 * Arena introspection: the things a debugging session actually asks for. Scopes have test_scope.c.
 **/

// The arena's introspection is debug gated and the suite builds in mode 4, so it is switched on here
// explicitly. Without this the test would compile to a series of no-ops and report a pass.
#define NYA_ARENA_FORCE_DEBUG

#include "nyangine-core/nyangine.c"
#include "nyangine-core/nyangine.h"

/** Distinct helpers so each gets its own row in the callsite table. */
static void* allocate_from(NYA_Arena* arena, u64 size) {
  return nya_arena_alloc(arena, size);
}

static void* allocate_a_lot(NYA_Arena* arena, u64 size) {
  return nya_arena_alloc(arena, size);
}

/** What the action callback saw, by type, and the name of the last arena it heard about. */
static u32              action_counts[NYA_ARENA_ACTION_COUNT];
static NYA_ConstCString action_last_arena = nullptr;

static void count_action(NYA_ArenaAction action) {
  nya_assert((u32)action.type < NYA_ARENA_ACTION_COUNT);
  action_counts[action.type]++;
  action_last_arena = action.arena_name;
}

s32 main(void) {
  // TEST: the action callback hears every call on every arena, and nothing once removed
  {
    nya_memset(action_counts, 0, sizeof(action_counts));
    nya_arena_actions_set_callback(count_action);

    NYA_Arena* arena = nya_arena_create(.name = "watched");
    void*      block = nya_arena_alloc(arena, 64);
    nya_arena_free(arena, block, 64);
    nya_arena_destroy(arena);

    nya_assert(action_counts[NYA_ARENA_ACTION_ARENA_NEW] == 1, "the create was heard, got %u", action_counts[NYA_ARENA_ACTION_ARENA_NEW]);
    nya_assert(action_counts[NYA_ARENA_ACTION_ALLOC] >= 1, "and the allocation");
    nya_assert(action_counts[NYA_ARENA_ACTION_FREE] == 1, "and the free");
    nya_assert(action_counts[NYA_ARENA_ACTION_ARENA_DESTROY] == 1, "and the destroy");
    nya_assert(nya_string_equals(action_last_arena, "watched"), "each named the arena it happened to");

    // no history is kept, and nothing is heard once the callback is gone.
    nya_arena_actions_set_callback(nullptr);
    u32 destroys = action_counts[NYA_ARENA_ACTION_ARENA_DESTROY];
    nya_arena_destroy(nya_arena_create(.name = "unwatched"));
    nya_assert(action_counts[NYA_ARENA_ACTION_ARENA_DESTROY] == destroys, "a removed callback hears nothing");
  }

  // TEST: stats describe an arena beyond the single number usage_bytes gives
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

  // TEST: resident bytes follow what was written, not what was reserved
  {
    NYA_Arena* arena = nya_arena_create(.name = "resident_subject");
    defer      nya_arena_destroy(arena);

    nya_assert(nya_arena_resident_bytes(arena) == 0, "an arena without regions holds nothing");

    u64 page    = nya_os_page_size();
    u8* written = nya_arena_alloc(arena, 64 * page);
    nya_memset(written, 0x5A, 64 * page);

    u64 resident = nya_arena_resident_bytes(arena);
    nya_assert(resident >= 64 * page, "the pages just written are resident, got " FMTu64, resident);
    nya_assert(resident <= nya_arena_stats(arena).reserved_bytes + page, "and no more than the regions span, got " FMTu64, resident);
  }

  // TEST: freeing populates the free list, and stats can see it
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

  // TEST: the registry lists live arenas and forgets destroyed ones
  {
    // This is what answers "memory is climbing, which subsystem". Without it the only way to ask is to already hold the guilty arena's pointer, which is what you do not have.
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

  // TEST: the global arenas are registered, so a report is useful with no setup
  {
    b8 found_global = false;
    for (u32 i = 0; i < nya_arena_registry_count(); i++) {
      NYA_Arena* arena = nya_arena_registry_at(i);
      if (arena == nya_arena_global) found_global = true;
    }
    nya_assert(found_global, "nya_arena_global registers itself like any other arena");
  }

  // TEST: callsites aggregate per line, and live_bytes tracks what is still held
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
    nya_assert(big_live > small_live, "live_bytes distinguishes the two");

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

  // The reports, run for their side effects: they must not fault on real data.
  {
    nya_arena_stats_report();
    nya_arena_callsites_report(5);
    nya_scope_report();

    // Limits that do not make sense are clamped rather than read off the end.
    nya_arena_callsites_report(0);
    nya_arena_callsites_report(100000);
  }

  printf("PASSED: test_introspection\n");
  return 0;
}
