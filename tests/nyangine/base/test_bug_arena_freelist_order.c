/**
 * Regression test for the insertion order in _nya_arena_free_list_add (base_arena.c).
 * */
#include "nyangine/nyangine.c"
#include "nyangine/nyangine.h"

#define BLOCK_SIZE 128

s32 main(void) {
  NYA_Arena* arena = nya_arena_create(.name = "test_bug_arena_freelist_order", .defragmentation_threshold = 1);

  // three consecutive blocks. The third is never freed, so neither of the others is the region's last allocation, which would move `used` back instead of touching the free list.
  u8* lower    = nya_arena_alloc(arena, BLOCK_SIZE);
  u8* higher   = nya_arena_alloc(arena, BLOCK_SIZE);
  u8* sentinel = nya_arena_alloc(arena, BLOCK_SIZE);

  nya_assert(lower != nullptr && higher != nullptr && sentinel != nullptr);
  nya_assert(lower < higher, "the region allocator hands out ascending addresses");
  nya_assert(higher < sentinel);

  // Higher first, so it becomes the head and the lower block has to be inserted before it.
  nya_arena_free(arena, higher, BLOCK_SIZE);
  nya_arena_free(arena, lower, BLOCK_SIZE);

  NYA_ArenaStats stats = nya_arena_stats(arena);
  nya_assert(
      stats.free_list_nodes == 1,
      "two adjacent free blocks should have coalesced into one node, got " FMTu64 "; the free list is not address ordered",
      stats.free_list_nodes
  );

  nya_arena_destroy(arena);

  nya_log_info("PASSED: the arena free list stays address ordered and coalesces");
  return 0;
}
