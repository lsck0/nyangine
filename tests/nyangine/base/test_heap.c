/**
 * THIS FILE WAS CLANKER WANKED !!!
 **/

#include "nyangine/nyangine.c"
#include "nyangine/nyangine.h"

// Compare functions for different types
s32 compare_s32_asc(const s32* a, const s32* b) {
  if (*a < *b) return -1;
  if (*a > *b) return 1;
  return 0;
}

s32 compare_s32_desc(const s32* a, const s32* b) {
  if (*a > *b) return -1;
  if (*a < *b) return 1;
  return 0;
}

s32 compare_u32_asc(const u32* a, const u32* b) {
  if (*a < *b) return -1;
  if (*a > *b) return 1;
  return 0;
}

s32 compare_f32_asc(const f32* a, const f32* b) {
  if (*a < *b) return -1;
  if (*a > *b) return 1;
  return 0;
}

// Test struct for heap operations
typedef struct {
  s32 id;
  f32 priority;
} TestItem;

s32 compare_test_item(const TestItem* a, const TestItem* b) {
  if (a->priority < b->priority) return -1;
  if (a->priority > b->priority) return 1;
  return 0;
}

nya_derive_heap(s32);
nya_derive_heap(u32);
nya_derive_heap(f32);
nya_derive_heap(TestItem);

s32 main(void) {
  NYA_Arena* arena = nya_arena_create(.name = "test_heap");

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: basic heap creation
  // ─────────────────────────────────────────────────────────────────────────────
  NYA_Heapᐸs32ᐳ* heap = nya_heap_create(arena, s32, compare_s32_asc);
  nya_assert(heap->length == 0);
  nya_assert(heap->capacity == _NYA_HEAP_DEFAULT_CAPACITY);
  nya_assert(heap->items != nullptr);
  nya_assert(heap->arena == arena);
  nya_assert(heap->compare == compare_s32_asc);
  nya_heap_destroy(heap);

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: creation with custom capacity
  // ─────────────────────────────────────────────────────────────────────────────
  NYA_Heapᐸs32ᐳ* heap_cap = nya_heap_create_with_capacity(arena, s32, compare_s32_asc, 128);
  nya_assert(heap_cap->length == 0);
  nya_assert(heap_cap->capacity == 128);
  nya_heap_destroy(heap_cap);

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: nya_heap_push (basic)
  // ─────────────────────────────────────────────────────────────────────────────
  NYA_Heapᐸs32ᐳ* push_heap = nya_heap_create(arena, s32, compare_s32_asc);
  nya_heap_push(push_heap, 5);
  nya_assert(push_heap->length == 1);
  nya_assert(push_heap->items[0] == 5);

  nya_heap_push(push_heap, 3);
  nya_assert(push_heap->length == 2);
  nya_assert(push_heap->items[0] == 3); // 3 should be at root (min-heap)

  nya_heap_push(push_heap, 8);
  nya_assert(push_heap->length == 3);
  nya_assert(push_heap->items[0] == 3); // 3 still at root
  nya_heap_destroy(push_heap);

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: heap property maintenance after multiple pushes
  // ─────────────────────────────────────────────────────────────────────────────
  NYA_Heapᐸs32ᐳ* prop_heap = nya_heap_create(arena, s32, compare_s32_asc);
  nya_heap_push(prop_heap, 5);
  nya_heap_push(prop_heap, 3);
  nya_heap_push(prop_heap, 8);
  nya_heap_push(prop_heap, 1);
  nya_heap_push(prop_heap, 4);
  nya_heap_push(prop_heap, 2);

  nya_assert(prop_heap->length == 6);
  nya_assert(prop_heap->items[0] == 1); // Minimum should be at root

  // Check heap property: parent <= children
  for (u64 i = 0; i < prop_heap->length; i++) {
    u64 left_child  = 2 * i + 1;
    u64 right_child = 2 * i + 2;

    if (left_child < prop_heap->length) { nya_assert(prop_heap->items[i] <= prop_heap->items[left_child]); }
    if (right_child < prop_heap->length) { nya_assert(prop_heap->items[i] <= prop_heap->items[right_child]); }
  }
  nya_heap_destroy(prop_heap);

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: nya_heap_pop (basic)
  // ─────────────────────────────────────────────────────────────────────────────
  NYA_Heapᐸs32ᐳ* pop_heap = nya_heap_create(arena, s32, compare_s32_asc);
  nya_heap_push(pop_heap, 5);
  nya_heap_push(pop_heap, 3);
  nya_heap_push(pop_heap, 8);

  s32 popped = nya_heap_pop(pop_heap);
  nya_assert(popped == 3); // Should pop minimum
  nya_assert(pop_heap->length == 2);
  nya_assert(pop_heap->items[0] == 5); // New minimum should be 5

  popped = nya_heap_pop(pop_heap);
  nya_assert(popped == 5);
  nya_assert(pop_heap->length == 1);
  nya_assert(pop_heap->items[0] == 8);
  nya_heap_destroy(pop_heap);

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: complete heap pop sequence (sorted output)
  // ─────────────────────────────────────────────────────────────────────────────
  NYA_Heapᐸs32ᐳ* seq_heap = nya_heap_create(arena, s32, compare_s32_asc);
  s32      values[] = { 5, 3, 8, 1, 4, 2, 7, 6 };
  for (u64 i = 0; i < nya_carray_length(values); i++) { nya_heap_push(seq_heap, values[i]); }

  s32 expected[] = { 1, 2, 3, 4, 5, 6, 7, 8 };
  for (u64 i = 0; i < nya_carray_length(expected); i++) {
    s32 popped = nya_heap_pop(seq_heap);
    nya_assert(popped == expected[i]);
  }
  nya_assert(seq_heap->length == 0);
  nya_heap_destroy(seq_heap);

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: max-heap (descending order)
  // ─────────────────────────────────────────────────────────────────────────────
  NYA_Heapᐸs32ᐳ* max_heap     = nya_heap_create(arena, s32, compare_s32_desc);
  s32      max_values[] = { 1, 3, 8, 5, 2, 4, 7, 6 };
  for (u64 i = 0; i < nya_carray_length(max_values); i++) { nya_heap_push(max_heap, max_values[i]); }

  s32 max_expected[] = { 8, 7, 6, 5, 4, 3, 2, 1 };
  for (u64 i = 0; i < nya_carray_length(max_expected); i++) {
    s32 popped = nya_heap_pop(max_heap);
    nya_assert(popped == max_expected[i]);
  }
  nya_assert(max_heap->length == 0);
  nya_heap_destroy(max_heap);

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: nya_heap_from_carray
  // ─────────────────────────────────────────────────────────────────────────────
  s32      carray_values[] = { 5, 3, 8, 1, 4, 2 };
  s32*     carray_ptr      = carray_values;
  NYA_Heapᐸs32ᐳ* from_array      = nya_heap_create_with_capacity(arena, s32, compare_s32_asc, 6UL);
  for (u64 i = 0; i < 6; i++) { nya_heap_push(from_array, carray_ptr[i]); }
  nya_assert(from_array->length == 6);

  // Should produce sorted output
  s32 from_expected[] = { 1, 2, 3, 4, 5, 8 };
  for (u64 i = 0; i < 6; i++) {
    s32 popped_val = nya_heap_pop(from_array);
    nya_assert(popped_val == from_expected[i]);
  }
  nya_assert(from_array->length == 0);
  nya_heap_destroy(from_array);

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: u32 heap
  // ─────────────────────────────────────────────────────────────────────────────
  NYA_Heapᐸu32ᐳ* u32_heap     = nya_heap_create(arena, u32, compare_u32_asc);
  u32      u32_values[] = { 10, 5, 15, 2, 12 };
  for (u64 i = 0; i < nya_carray_length(u32_values); i++) { nya_heap_push(u32_heap, u32_values[i]); }

  u32 u32_expected[] = { 2, 5, 10, 12, 15 };
  for (u64 i = 0; i < nya_carray_length(u32_expected); i++) {
    u32 popped = nya_heap_pop(u32_heap);
    nya_assert(popped == u32_expected[i]);
  }
  nya_assert(u32_heap->length == 0);
  nya_heap_destroy(u32_heap);

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: f32 heap
  // ─────────────────────────────────────────────────────────────────────────────
  NYA_Heapᐸf32ᐳ* f32_heap     = nya_heap_create(arena, f32, compare_f32_asc);
  f32      f32_values[] = { 3.5F, 1.2F, 4.8F, 0.5F, 2.7F };
  for (u64 i = 0; i < nya_carray_length(f32_values); i++) { nya_heap_push(f32_heap, f32_values[i]); }

  f32 f32_expected[] = { 0.5F, 1.2F, 2.7F, 3.5F, 4.8F };
  for (u64 i = 0; i < nya_carray_length(f32_expected); i++) {
    f32 popped = nya_heap_pop(f32_heap);
    nya_assert(popped == f32_expected[i]);
  }
  nya_assert(f32_heap->length == 0);
  nya_heap_destroy(f32_heap);

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: custom struct heap
  // ─────────────────────────────────────────────────────────────────────────────
  NYA_HeapᐸTestItemᐳ* item_heap = nya_heap_create(arena, TestItem, compare_test_item);
  TestItem      items[]   = {
    { .id = 1, .priority = 3.5F },
    { .id = 2, .priority = 1.2F },
    { .id = 3, .priority = 4.8F },
    { .id = 4, .priority = 0.5F }
  };
  for (u64 i = 0; i < nya_carray_length(items); i++) { nya_heap_push(item_heap, items[i]); }

  TestItem popped_item = nya_heap_pop(item_heap);
  nya_assert(popped_item.id == 4 && popped_item.priority == 0.5F);

  popped_item = nya_heap_pop(item_heap);
  nya_assert(popped_item.id == 2 && popped_item.priority == 1.2F);

  popped_item = nya_heap_pop(item_heap);
  nya_assert(popped_item.id == 1 && popped_item.priority == 3.5F);

  popped_item = nya_heap_pop(item_heap);
  nya_assert(popped_item.id == 3 && popped_item.priority == 4.8F);

  nya_assert(item_heap->length == 0);
  nya_heap_destroy(item_heap);

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: nya_heap_clear
  // ─────────────────────────────────────────────────────────────────────────────
  NYA_Heapᐸs32ᐳ* clear_heap = nya_heap_create(arena, s32, compare_s32_asc);
  nya_heap_push(clear_heap, 5);
  nya_heap_push(clear_heap, 3);
  nya_heap_push(clear_heap, 8);
  nya_assert(clear_heap->length == 3);

  u64 cap_before_clear = clear_heap->capacity;
  nya_heap_clear(clear_heap);
  nya_assert(clear_heap->length == 0);
  nya_assert(clear_heap->capacity == cap_before_clear);

  // Should be able to push after clear
  nya_heap_push(clear_heap, 10);
  nya_assert(clear_heap->length == 1);
  nya_assert(clear_heap->items[0] == 10);
  nya_heap_destroy(clear_heap);

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: automatic resize on capacity overflow
  // ─────────────────────────────────────────────────────────────────────────────
  NYA_Heapᐸs32ᐳ* resize_heap = nya_heap_create_with_capacity(arena, s32, compare_s32_asc, 4);
  nya_assert(resize_heap->capacity == 4);

  for (s32 i = 0; i < 10; i++) { nya_heap_push(resize_heap, i * 10); }
  nya_assert(resize_heap->length == 10);
  nya_assert(resize_heap->capacity >= 10);

  // Check heap property still holds after resize
  for (u64 i = 0; i < resize_heap->length; i++) {
    u64 left_child  = 2 * i + 1;
    u64 right_child = 2 * i + 2;

    if (left_child < resize_heap->length) { nya_assert(resize_heap->items[i] <= resize_heap->items[left_child]); }
    if (right_child < resize_heap->length) { nya_assert(resize_heap->items[i] <= resize_heap->items[right_child]); }
  }
  nya_heap_destroy(resize_heap);

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: nya_heap_reserve
  // ─────────────────────────────────────────────────────────────────────────────
  NYA_Heapᐸs32ᐳ* reserve_heap = nya_heap_create_with_capacity(arena, s32, compare_s32_asc, 4);
  nya_heap_push(reserve_heap, 10);
  nya_heap_push(reserve_heap, 20);
  nya_assert(reserve_heap->capacity == 4);

  nya_heap_reserve(reserve_heap, 100UL);
  nya_assert(reserve_heap->capacity >= 100UL);
  nya_assert(reserve_heap->length == 2);
  nya_assert(reserve_heap->items[0] == 10);
  nya_assert(reserve_heap->items[1] == 20);

  // Reserve with smaller capacity should not change
  u64 cap = reserve_heap->capacity;
  nya_heap_reserve(reserve_heap, 50UL);
  nya_assert(reserve_heap->capacity == cap);
  nya_heap_destroy(reserve_heap);

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: edge case - empty heap operations
  // ─────────────────────────────────────────────────────────────────────────────
  NYA_Heapᐸs32ᐳ* empty_heap = nya_heap_create(arena, s32, compare_s32_asc);
  nya_assert(empty_heap->length == 0);

  // Clear should work on empty heap
  nya_heap_clear(empty_heap);
  nya_assert(empty_heap->length == 0);
  nya_heap_destroy(empty_heap);

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: edge case - single element heap
  // ─────────────────────────────────────────────────────────────────────────────
  NYA_Heapᐸs32ᐳ* single_heap = nya_heap_create(arena, s32, compare_s32_asc);
  nya_heap_push(single_heap, 42);
  nya_assert(single_heap->length == 1);
  nya_assert(single_heap->items[0] == 42);

  s32 single_popped = nya_heap_pop(single_heap);
  nya_assert(single_popped == 42);
  nya_assert(single_heap->length == 0);
  nya_heap_destroy(single_heap);

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: stress test with many elements
  // ─────────────────────────────────────────────────────────────────────────────
  NYA_Heapᐸs32ᐳ*  stress_heap  = nya_heap_create(arena, s32, compare_s32_asc);
  const s32 stress_count = 1000;

  // Insert elements in random order
  for (s32 i = 0; i < stress_count; i++) {
    nya_heap_push(stress_heap, stress_count - i); // Insert in descending order
  }

  nya_assert(stress_heap->length == stress_count);

  // Should come out in ascending order
  for (s32 i = 1; i <= stress_count; i++) {
    s32 popped = nya_heap_pop(stress_heap);
    nya_assert(popped == i);
  }
  nya_assert(stress_heap->length == 0);
  nya_heap_destroy(stress_heap);

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: duplicate values
  // ─────────────────────────────────────────────────────────────────────────────
  NYA_Heapᐸs32ᐳ* dup_heap     = nya_heap_create(arena, s32, compare_s32_asc);
  s32      dup_values[] = { 5, 3, 5, 1, 3, 1, 4, 2 };
  for (u64 i = 0; i < nya_carray_length(dup_values); i++) { nya_heap_push(dup_heap, dup_values[i]); }

  s32 dup_expected[] = { 1, 1, 2, 3, 3, 4, 5, 5 };
  for (u64 i = 0; i < nya_carray_length(dup_expected); i++) {
    s32 popped = nya_heap_pop(dup_heap);
    nya_assert(popped == dup_expected[i]);
  }
  nya_assert(dup_heap->length == 0);
  nya_heap_destroy(dup_heap);

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: negative values (s32 heap)
  // ─────────────────────────────────────────────────────────────────────────────
  NYA_Heapᐸs32ᐳ* neg_heap     = nya_heap_create(arena, s32, compare_s32_asc);
  s32      neg_values[] = { 5, -3, 8, -1, 4, -2, 0, -5 };
  for (u64 i = 0; i < nya_carray_length(neg_values); i++) { nya_heap_push(neg_heap, neg_values[i]); }

  s32 neg_expected[] = { -5, -3, -2, -1, 0, 4, 5, 8 };
  for (u64 i = 0; i < nya_carray_length(neg_expected); i++) {
    s32 popped = nya_heap_pop(neg_heap);
    nya_assert(popped == neg_expected[i]);
  }
  nya_assert(neg_heap->length == 0);
  nya_heap_destroy(neg_heap);

  // ─────────────────────────────────────────────────────────────────────────────
  // CLEANUP
  // ─────────────────────────────────────────────────────────────────────────────
  nya_arena_destroy(arena);

  return 0;
}
