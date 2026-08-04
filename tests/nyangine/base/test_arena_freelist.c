/**
 * Arena free list behaviour.
 *
 * test_arena.c covers allocation, reallocation and destruction. What it does not reach is what
 * happens to the free list over many alloc/free cycles: whether freed blocks come back, whether
 * adjacent free blocks coalesce, and whether the arena can serve an allocation the size of the
 * space it has released. That is where an allocator quietly leaks address space.
 **/

#include "nyangine/nyangine.c"
#include "nyangine/nyangine.h"

/** Free nodes across every region, which is the fragmentation the defragmenter exists to undo. */
static u64 free_node_count(NYA_Arena* arena) {
  u64 count = 0;
  nya_dll_foreach (arena, region) {
    if (region->free_list == nullptr) continue;
    for (NYA_ArenaFreeListNode* node = region->free_list->head; node != nullptr; node = node->next) count++;
  }
  return count;
}

s32 main(void) {
  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: a freed block is reused rather than growing the region
  // ─────────────────────────────────────────────────────────────────────────────
  printf("TEST: freed blocks are reused\n");
  {
    NYA_Arena* arena = nya_arena_create(.name = "reuse", .region_size = nya_kibyte_to_byte(64UL));

    // Two allocations so the first is not the most recent, which takes the "last allocation" path
    // in free rather than going onto the free list.
    void* first  = nya_arena_alloc(arena, 256);
    void* second = nya_arena_alloc(arena, 256);
    nya_assert(first != nullptr && second != nullptr);

    u64 used_before = arena->head->used;
    nya_arena_free(arena, first, 256);

    // The same size again should come out of the free list, leaving `used` where it was.
    void* recycled = nya_arena_alloc(arena, 256);
    nya_assert(recycled != nullptr);
    nya_assert(arena->head->used == used_before, "a freed block was not reused: used went from " FMTu64 " to " FMTu64, used_before, arena->head->used);

    nya_arena_free(arena, second, 256);
    nya_arena_free(arena, recycled, 256);
    nya_arena_destroy(arena);
    printf("  PASSED\n");
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: adjacent freed blocks coalesce
  //
  // Freeing a run of neighbouring blocks leaves a contiguous span. If the free list keeps them as
  // separate nodes, an allocation the size of the whole span cannot be served from it even though
  // the bytes are sitting there, and the region grows instead.
  // ─────────────────────────────────────────────────────────────────────────────
  printf("TEST: adjacent frees coalesce\n");
  {
    NYA_Arena* arena = nya_arena_create(.name = "coalesce", .region_size = nya_kibyte_to_byte(64UL));

    enum { BLOCKS = 32, BLOCK_SIZE = 128 };

    void* blocks[BLOCKS];
    for (u32 i = 0; i < BLOCKS; i++) blocks[i] = nya_arena_alloc(arena, BLOCK_SIZE);

    // A tail allocation, so freeing the blocks above cannot take the "last allocation" shortcut and
    // every one of them really does go onto the free list.
    void* tail = nya_arena_alloc(arena, 64);
    nya_assert(tail != nullptr);

    for (u32 i = 0; i < BLOCKS; i++) nya_arena_free(arena, blocks[i], BLOCK_SIZE);

    u64 nodes = free_node_count(arena);
    nya_assert(
        nodes < BLOCKS,
        "%u adjacent blocks were freed and the free list still holds " FMTu64 " nodes, so nothing coalesced",
        (u32)BLOCKS,
        nodes
    );

    nya_arena_free(arena, tail, 64);
    nya_arena_destroy(arena);
    printf("  PASSED\n");
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: churn does not grow the region without bound
  //
  // Allocating and freeing the same size repeatedly should settle: the block comes back each time
  // rather than the region marching forward.
  // ─────────────────────────────────────────────────────────────────────────────
  printf("TEST: churn is bounded\n");
  {
    NYA_Arena* arena = nya_arena_create(.name = "churn", .region_size = nya_kibyte_to_byte(64UL));

    void* anchor = nya_arena_alloc(arena, 64);   // keeps the churn off the last-allocation path
    nya_assert(anchor != nullptr);

    void* p = nya_arena_alloc(arena, 512);
    nya_arena_free(arena, p, 512);

    u64 used_after_first = arena->head->used;

    for (u32 i = 0; i < 500; i++) {
      void* q = nya_arena_alloc(arena, 512);
      nya_assert(q != nullptr);
      nya_arena_free(arena, q, 512);
    }

    nya_assert(
        arena->head->used == used_after_first,
        "500 identical alloc/free cycles moved used from " FMTu64 " to " FMTu64,
        used_after_first,
        arena->head->used
    );

    // And it never needed a second region.
    nya_assert(arena->head->next == nullptr, "churn spilled into extra regions");

    nya_arena_free(arena, anchor, 64);
    nya_arena_destroy(arena);
    printf("  PASSED\n");
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: a span released in pieces can be handed back as one allocation
  // ─────────────────────────────────────────────────────────────────────────────
  printf("TEST: coalesced span is usable\n");
  {
    NYA_Arena* arena = nya_arena_create(.name = "span", .region_size = nya_kibyte_to_byte(64UL));

    enum { PIECES = 16, PIECE = 256 };

    void* pieces[PIECES];
    for (u32 i = 0; i < PIECES; i++) pieces[i] = nya_arena_alloc(arena, PIECE);
    void* tail = nya_arena_alloc(arena, 64);

    u64 used_before = arena->head->used;
    for (u32 i = 0; i < PIECES; i++) nya_arena_free(arena, pieces[i], PIECE);

    // Ask for most of what was just released, in one piece.
    void* big = nya_arena_alloc(arena, PIECES * PIECE / 2);
    nya_assert(big != nullptr);
    nya_assert(
        arena->head->used <= used_before,
        "an allocation of " FMTu64 " bytes grew the region even though twice that had just been freed",
        (u64)(PIECES * PIECE / 2)
    );

    nya_arena_free(arena, tail, 64);
    nya_arena_destroy(arena);
    printf("  PASSED\n");
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: realloc shrink then grow keeps the contents
  // ─────────────────────────────────────────────────────────────────────────────
  printf("TEST: realloc round trip\n");
  {
    NYA_Arena* arena = nya_arena_create(.name = "realloc", .region_size = nya_kibyte_to_byte(64UL));

    u8* data = nya_arena_alloc(arena, 1024);
    for (u32 i = 0; i < 1024; i++) data[i] = (u8)(i & 0xFF);

    u8* smaller = nya_arena_realloc(arena, data, 1024, 256);
    for (u32 i = 0; i < 256; i++) nya_assert(smaller[i] == (u8)(i & 0xFF), "shrink lost byte %u", i);

    u8* bigger = nya_arena_realloc(arena, smaller, 256, 4096);
    for (u32 i = 0; i < 256; i++) nya_assert(bigger[i] == (u8)(i & 0xFF), "grow lost byte %u", i);

    nya_arena_free(arena, bigger, 4096);
    nya_arena_destroy(arena);
    printf("  PASSED\n");
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: an allocation larger than the region size still works
  // ─────────────────────────────────────────────────────────────────────────────
  printf("TEST: oversized allocation\n");
  {
    NYA_Arena* arena = nya_arena_create(.name = "oversized", .region_size = nya_kibyte_to_byte(4UL));

    u8* big = nya_arena_alloc(arena, nya_kibyte_to_byte(64UL));
    nya_assert(big != nullptr);

    // Writable across its whole length, which is what ASan is here to confirm.
    nya_memset(big, 0xAB, nya_kibyte_to_byte(64UL));
    nya_assert(big[0] == 0xAB);
    nya_assert(big[nya_kibyte_to_byte(64UL) - 1] == 0xAB);

    nya_arena_free(arena, big, nya_kibyte_to_byte(64UL));
    nya_arena_destroy(arena);
    printf("  PASSED\n");
  }

  printf("PASSED: test_arena_freelist\n");
  return 0;
}
