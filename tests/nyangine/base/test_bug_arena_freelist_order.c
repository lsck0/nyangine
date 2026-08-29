/**
 * Regression test for the insertion order in _nya_arena_free_list_add (base_arena.c).
 *
 * The free list is meant to be sorted by address, which is the invariant
 * _nya_arena_free_list_defragment relies on: it only merges a node with its immediate successor in
 * the list, so two adjacent blocks that are out of order are never coalesced.
 *
 * The insertion walk is
 *
 *     if (free_node->next && (u8*)free_node->next->ptr < (u8*)new_node->ptr) continue;
 *     nya_dll_node_link(free_list, free_node, new_node, free_node->next);
 *
 * which always links *after* the node it stopped at. A block whose address is below the current
 * head therefore lands second rather than first, and the list stops being sorted.
 *
 * Freeing the higher block first is what puts it at the head, so the lower one hits that path.
 * defragmentation_threshold is 1 so the merge runs on every free and the check below does not
 * depend on when the default threshold of 16 happens to fire.
 * */
#include "nyangine/nyangine.c"
#include "nyangine/nyangine.h"

#define BLOCK_SIZE 128

s32 main(void) {
  NYA_Arena* arena = nya_arena_create(.name = "test_bug_arena_freelist_order", .defragmentation_threshold = 1);

  // Three consecutive blocks. The third is never freed, so neither of the other two is ever the
  // region's last allocation — that path returns the bytes by moving `used` back and never touches
  // the free list at all.
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
      "two adjacent free blocks should have coalesced into one node, got " FMTu64 " — the free list is not address ordered",
      stats.free_list_nodes
  );

  nya_arena_destroy(arena);

  nya_log_info("PASSED: the arena free list stays address ordered and coalesces");
  return 0;
}
