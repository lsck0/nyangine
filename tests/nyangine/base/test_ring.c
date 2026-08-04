/**
 * THIS FILE WAS CLANKER WANKED !!!
 **/

#include "nyangine/nyangine.c"
#include "nyangine/nyangine.h"

typedef struct {
  u32 id;
} TestItem;

nya_derive_ring(TestItem);

void test_ring_basic_operations(void) {
  NYA_Arena* arena = nya_arena_create();

  // Test: nya_ring_create and nya_ring_create_with_capacity
  NYA_RingᐸTestItemᐳ* ring = nya_ring_create(arena, TestItem);
  nya_assert(ring != nullptr);
  nya_assert(nya_ring_capacity(ring) == 8);
  nya_assert(nya_ring_length(ring) == 0);
  nya_assert(nya_ring_is_empty(ring));

  // Test: nya_ring_push and basic access
  TestItem item1 = { .id = 1 };
  TestItem item2 = { .id = 2 };

  nya_ring_push(ring, item1);
  nya_assert(!nya_ring_is_empty(ring));
  nya_assert(nya_ring_length(ring) == 1);
  nya_assert(nya_ring_front(ring)->id == 1);
  nya_assert(nya_ring_back(ring)->id == 1);

  nya_ring_push(ring, item2);
  nya_assert(nya_ring_length(ring) == 2);
  nya_assert(nya_ring_front(ring)->id == 1);
  nya_assert(nya_ring_back(ring)->id == 2);

  // Test: nya_ring_at
  nya_assert(nya_ring_at(ring, 0)->id == 1);
  nya_assert(nya_ring_at(ring, 1)->id == 2);

  // Test: nya_ring_pop
  TestItem popped = nya_ring_pop(ring);
  nya_assert(popped.id == 1);
  nya_assert(nya_ring_length(ring) == 1);

  // Test: nya_ring_clear
  nya_ring_clear(ring);
  nya_assert(nya_ring_length(ring) == 0);
  nya_assert(nya_ring_is_empty(ring));

  nya_ring_destroy(ring);
  nya_arena_destroy(arena);
}

void test_ring_wraparound(void) {
  NYA_Arena* arena = nya_arena_create();

  // Create a small ring to test wraparound behavior
  NYA_RingᐸTestItemᐳ* ring = nya_ring_create_with_capacity(arena, TestItem, 3);

  // Fill ring to capacity
  for (u32 i = 0; i < 3; i++) {
    TestItem item = { .id = i };
    nya_ring_push(ring, item);
  }

  // Push another item - should wrap around and overwrite the first
  TestItem wrap_item = { .id = 99 };
  nya_ring_push(ring, wrap_item);

  nya_assert(nya_ring_length(ring) == 3);
  nya_assert(nya_ring_capacity(ring) == 3);
  nya_assert(nya_ring_front(ring)->id == 1); // First item (id=0) was overwritten
  nya_assert(nya_ring_back(ring)->id == 99); // New item is at back

  // Pop items to verify correct order
  TestItem popped1 = nya_ring_pop(ring);
  nya_assert(popped1.id == 1);

  TestItem popped2 = nya_ring_pop(ring);
  nya_assert(popped2.id == 2);

  TestItem popped3 = nya_ring_pop(ring);
  nya_assert(popped3.id == 99);

  nya_assert(nya_ring_is_empty(ring));

  nya_ring_destroy(ring);
  nya_arena_destroy(arena);
}

void test_ring_iterators(void) {
  NYA_Arena* arena = nya_arena_create();

  NYA_RingᐸTestItemᐳ* ring = nya_ring_create(arena, TestItem);

  // Add some items
  TestItem items[] = { { .id = 1 }, { .id = 2 }, { .id = 3 } };
  nya_ring_push_many(ring, items[0], items[1], items[2]);

  // Test: nya_ring_foreach
  u32       foreach_count = 0;
  TestItem* current_item;
  nya_ring_foreach(ring, current_item) {
    nya_assert(current_item->id == items[foreach_count].id);
    foreach_count++;
  }
  nya_assert(foreach_count == 3);

  // Test: nya_ring_foreach_reverse
  u32       reverse_count = 0;
  TestItem* reverse_item;
  nya_ring_foreach_reverse(ring, reverse_item) {
    nya_assert(reverse_item->id == items[2 - reverse_count].id);
    reverse_count++;
  }
  nya_assert(reverse_count == 3);

  nya_ring_destroy(ring);
  nya_arena_destroy(arena);
}

s32 main(void) {
  test_ring_basic_operations();
  test_ring_wraparound();
  test_ring_iterators();

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: nya_ring_create_on_stack / nya_ring_destroy_on_stack
  // ─────────────────────────────────────────────────────────────────────────────
  {
    NYA_Arena*   arena    = nya_arena_create();
    NYA_RingᐸTestItemᐳ on_stack = nya_ring_create_on_stack(arena, TestItem);
    nya_assert(nya_ring_capacity(&on_stack) == 8);
    nya_assert(nya_ring_is_empty(&on_stack));

    nya_ring_push(&on_stack, ((TestItem){ .id = 42 }));
    nya_assert(nya_ring_length(&on_stack) == 1);
    nya_assert(nya_ring_front(&on_stack)->id == 42);

    nya_ring_destroy_on_stack(&on_stack);
    nya_arena_destroy(arena);
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: nya_ring_create_with_capacity_on_stack
  // ─────────────────────────────────────────────────────────────────────────────
  {
    NYA_Arena*   arena    = nya_arena_create();
    NYA_RingᐸTestItemᐳ on_stack = nya_ring_create_with_capacity_on_stack(arena, TestItem, 4);
    nya_assert(nya_ring_capacity(&on_stack) == 4);

    for (u32 i = 0; i < 4; i++) {
      nya_ring_push(&on_stack, ((TestItem){ .id = i }));
    }
    nya_assert(nya_ring_length(&on_stack) == 4);

    nya_ring_destroy_on_stack(&on_stack);
    nya_arena_destroy(arena);
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: nya_ring_peek
  // ─────────────────────────────────────────────────────────────────────────────
  {
    NYA_Arena*    arena = nya_arena_create();
    NYA_RingᐸTestItemᐳ* ring  = nya_ring_create(arena, TestItem);

    nya_ring_push(ring, ((TestItem){ .id = 10 }));
    nya_ring_push(ring, ((TestItem){ .id = 20 }));
    nya_ring_push(ring, ((TestItem){ .id = 30 }));

    nya_assert(nya_ring_peek(ring, 0)->id == 10);
    nya_assert(nya_ring_peek(ring, 1)->id == 20);
    nya_assert(nya_ring_peek(ring, 2)->id == 30);

    nya_ring_destroy(ring);
    nya_arena_destroy(arena);
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: nya_ring_pop_many
  // ─────────────────────────────────────────────────────────────────────────────
  {
    NYA_Arena*    arena = nya_arena_create();
    NYA_RingᐸTestItemᐳ* ring  = nya_ring_create(arena, TestItem);

    for (u32 i = 0; i < 5; i++) {
      nya_ring_push(ring, ((TestItem){ .id = i }));
    }
    nya_assert(nya_ring_length(ring) == 5);

    nya_ring_pop_many(ring, 3);
    nya_assert(nya_ring_length(ring) == 2);
    nya_assert(nya_ring_front(ring)->id == 3);

    nya_ring_destroy(ring);
    nya_arena_destroy(arena);
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: nya_ring_resize
  // ─────────────────────────────────────────────────────────────────────────────
  {
    NYA_Arena*    arena = nya_arena_create();
    NYA_RingᐸTestItemᐳ* ring  = nya_ring_create_with_capacity(arena, TestItem, 4);

    nya_ring_push(ring, ((TestItem){ .id = 1 }));
    nya_ring_push(ring, ((TestItem){ .id = 2 }));

    nya_ring_resize(ring, 16);
    nya_assert(nya_ring_capacity(ring) == 16);
    nya_assert(nya_ring_length(ring) == 2);
    nya_assert(nya_ring_front(ring)->id == 1);
    nya_assert(nya_ring_back(ring)->id == 2);

    nya_ring_destroy(ring);
    nya_arena_destroy(arena);
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: nya_ring_resize with wrapped data preserves logical order
  // ─────────────────────────────────────────────────────────────────────────────
  {
    NYA_Arena*    arena = nya_arena_create();
    NYA_RingᐸTestItemᐳ* ring  = nya_ring_create_with_capacity(arena, TestItem, 4);

    // Fill to capacity then push more to wrap around
    for (u32 i = 0; i < 4; i++) {
      nya_ring_push(ring, ((TestItem){ .id = i }));
    }
    // Pop 2 items, push 2 more — head wraps forward
    nya_ring_pop(ring);
    nya_ring_pop(ring);
    nya_ring_push(ring, ((TestItem){ .id = 10 }));
    nya_ring_push(ring, ((TestItem){ .id = 11 }));

    // Ring is now wrapped: head > 0, data spans the boundary
    nya_assert(nya_ring_length(ring) == 4);
    nya_assert(nya_ring_front(ring)->id == 2);
    nya_assert(nya_ring_back(ring)->id == 11);

    // Resize — must linearize wrapped data
    nya_ring_resize(ring, 16);
    nya_assert(nya_ring_capacity(ring) == 16);
    nya_assert(nya_ring_length(ring) == 4);
    nya_assert(nya_ring_front(ring)->id == 2);
    nya_assert(nya_ring_at(ring, 1)->id == 3);
    nya_assert(nya_ring_at(ring, 2)->id == 10);
    nya_assert(nya_ring_back(ring)->id == 11);

    // Verify we can still push/pop normally after resize
    nya_ring_push(ring, ((TestItem){ .id = 42 }));
    nya_assert(nya_ring_length(ring) == 5);
    nya_assert(nya_ring_back(ring)->id == 42);

    nya_ring_destroy(ring);
    nya_arena_destroy(arena);
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: nya_ring_resize with fully wrapped ring (head at middle)
  // ─────────────────────────────────────────────────────────────────────────────
  {
    NYA_Arena*    arena = nya_arena_create();
    NYA_RingᐸTestItemᐳ* ring  = nya_ring_create_with_capacity(arena, TestItem, 3);

    // Overwrite: push 5 items into a cap-3 ring → wraps multiple times
    for (u32 i = 0; i < 5; i++) {
      nya_ring_push(ring, ((TestItem){ .id = i + 100 }));
    }
    // Should contain [102, 103, 104]
    nya_assert(nya_ring_length(ring) == 3);
    nya_assert(nya_ring_front(ring)->id == 102);
    nya_assert(nya_ring_back(ring)->id == 104);

    nya_ring_resize(ring, 8);
    nya_assert(nya_ring_capacity(ring) == 8);
    nya_assert(nya_ring_length(ring) == 3);
    nya_assert(nya_ring_front(ring)->id == 102);
    nya_assert(nya_ring_at(ring, 1)->id == 103);
    nya_assert(nya_ring_back(ring)->id == 104);

    nya_ring_destroy(ring);
    nya_arena_destroy(arena);
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: nya_ring_copy
  // ─────────────────────────────────────────────────────────────────────────────
  {
    NYA_Arena*    arena = nya_arena_create();
    NYA_RingᐸTestItemᐳ* ring  = nya_ring_create(arena, TestItem);

    nya_ring_push(ring, ((TestItem){ .id = 100 }));
    nya_ring_push(ring, ((TestItem){ .id = 200 }));

    NYA_RingᐸTestItemᐳ copied = nya_ring_copy(ring);
    nya_assert(nya_ring_length(&copied) == 2);
    nya_assert(nya_ring_front(&copied)->id == 100);
    nya_assert(nya_ring_back(&copied)->id == 200);

    // Modifying original shouldn't affect copy
    nya_ring_pop(ring);
    nya_assert(nya_ring_length(ring) == 1);
    nya_assert(nya_ring_length(&copied) == 2);

    nya_ring_destroy_on_stack(&copied);
    nya_ring_destroy(ring);
    nya_arena_destroy(arena);
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: nya_ring_move
  // ─────────────────────────────────────────────────────────────────────────────
  {
    NYA_Arena*    arena1 = nya_arena_create();
    NYA_Arena*    arena2 = nya_arena_create();
    NYA_RingᐸTestItemᐳ* ring   = nya_ring_create(arena1, TestItem);

    nya_ring_push(ring, ((TestItem){ .id = 50 }));
    nya_ring_push(ring, ((TestItem){ .id = 60 }));

    nya_ring_move(ring, arena2);

    nya_assert(nya_ring_length(ring) == 2);
    nya_assert(nya_ring_front(ring)->id == 50);
    nya_assert(nya_ring_back(ring)->id == 60);
    nya_assert(ring->arena == arena2);

    nya_ring_destroy(ring);
    nya_arena_destroy(arena1);
    nya_arena_destroy(arena2);
  }

  return 0;
}
