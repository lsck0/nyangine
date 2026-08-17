/**
 * Regression tests for the container and arena edge cases found alongside the three larger bugs.
 *
 * Each section pins one of them:
 *
 *  - nya_heap_from_carray did not compile at all, so nothing could have called it.
 *  - nya_heap_resize could not grow a heap whose capacity started at zero, because
 *    nya_arena_realloc returns null for a null pointer by design.
 *  - nya_heap_destroy_on_stack cleared `items` but left `capacity` and `length` claiming it.
 *  - nya_ring_pop_many re-read its bound every iteration, so draining a ring with its own length
 *    stopped half way.
 *  - The arena's alignment check only tested for evenness, and an oversized allocation on an arena
 *    aligned beyond malloc's guarantee ran off the end of its region.
 * */
#include "nyangine/nyangine.c"
#include "nyangine/nyangine.h"

nya_derive_heap(u32);
nya_derive_ring(u32);

static s32 compare_u32_ascending(const u32* a, const u32* b) {
  return *a < *b ? -1 : (*a > *b ? 1 : 0);
}

s32 main(void) {
  setvbuf(stdout, nullptr, _IONBF, 0);

  NYA_Arena* arena = nya_arena_create(.name = "test_bug_container_edges");

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: nya_heap_from_carray builds a heap and pops in order
  // ─────────────────────────────────────────────────────────────────────────────
  printf("TEST: nya_heap_from_carray\n");
  {
    u32 values[]   = { 5, 3, 8, 1, 4, 2 };
    u32 expected[] = { 1, 2, 3, 4, 5, 8 };

    NYA_Heapᐸu32ᐳ* heap = nya_heap_from_carray(arena, u32, values, nya_carray_length(values), &compare_u32_ascending);

    nya_assert(nya_heap_length(heap) == nya_carray_length(values), "heap holds " FMTu64 " items", nya_heap_length(heap));
    for (u64 i = 0; i < nya_carray_length(expected); i++) {
      u32 popped = nya_heap_pop(heap);
      nya_assert(popped == expected[i], "popped " FMTu32 ", expected " FMTu32 " at " FMTu64, popped, expected[i], i);
    }

    nya_heap_destroy(heap);
  }
  printf("  PASSED\n");

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: an empty carray, which is the capacity-zero path
  // ─────────────────────────────────────────────────────────────────────────────
  printf("TEST: nya_heap_from_carray on an empty array\n");
  {
    u32*           nothing = nullptr;
    NYA_Heapᐸu32ᐳ* heap    = nya_heap_from_carray(arena, u32, nothing, 0UL, &compare_u32_ascending);

    // Growing from zero is proven by the next section; here it is only that an empty source is
    // accepted at all rather than tripping the capacity-zero allocation path.
    nya_assert(nya_heap_length(heap) == 0, "an empty carray produced " FMTu64 " items", nya_heap_length(heap));

    nya_heap_destroy(heap);
  }
  printf("  PASSED\n");

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: a heap created with capacity zero can grow
  // ─────────────────────────────────────────────────────────────────────────────
  printf("TEST: growing a zero capacity heap\n");
  {
    NYA_Heapᐸu32ᐳ* heap = nya_heap_create_with_capacity(arena, u32, &compare_u32_ascending, 0UL);
    nya_assert(heap->items == nullptr, "capacity zero should not allocate");

    for (u32 i = 10; i > 0; i--) nya_heap_push(heap, i);

    nya_assert(nya_heap_length(heap) == 10, "heap holds " FMTu64 " items, expected 10", nya_heap_length(heap));
    for (u32 i = 1; i <= 10; i++) {
      u32 popped = nya_heap_pop(heap);
      nya_assert(popped == i, "popped " FMTu32 ", expected " FMTu32, popped, i);
    }

    nya_heap_destroy(heap);
  }
  printf("  PASSED\n");

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: destroying an on-stack heap leaves it empty rather than half owned
  // ─────────────────────────────────────────────────────────────────────────────
  printf("TEST: nya_heap_destroy_on_stack resets the state\n");
  {
    NYA_Heapᐸu32ᐳ heap = nya_heap_create_with_capacity_on_stack(arena, u32, &compare_u32_ascending, 4UL);
    nya_heap_push(&heap, 3U);
    nya_heap_push(&heap, 1U);

    nya_heap_destroy_on_stack(heap);

    nya_assert(heap.items == nullptr, "items survived the destroy");
    nya_assert(heap.capacity == 0, "capacity is " FMTu64 " after destroy, expected 0", heap.capacity);
    nya_assert(heap.length == 0, "length is " FMTu64 " after destroy, expected 0", heap.length);

    // With the state consistent this is a fresh heap again rather than a null write.
    nya_heap_push(&heap, 9U);
    nya_assert(heap.items != nullptr, "pushing onto a destroyed heap did not reallocate");
    nya_assert(nya_heap_pop(&heap) == 9, "the reused heap lost its item");

    nya_heap_destroy_on_stack(heap);
  }
  printf("  PASSED\n");

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: draining a ring with its own length
  // ─────────────────────────────────────────────────────────────────────────────
  printf("TEST: nya_ring_pop_many with a count read off the ring\n");
  {
    NYA_Ringᐸu32ᐳ* ring = nya_ring_create_with_capacity(arena, u32, 8);
    for (u32 i = 0; i < 6; i++) nya_ring_push(ring, i);

    nya_ring_pop_many(ring, nya_ring_length(ring));

    nya_assert(nya_ring_is_empty(ring), "the ring still holds " FMTu64 " items after draining it", nya_ring_length(ring));

    nya_ring_destroy(ring);
  }
  printf("  PASSED\n");

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: an oversized allocation on an arena aligned beyond malloc's guarantee
  // ─────────────────────────────────────────────────────────────────────────────
  printf("TEST: oversized allocation on a 64 byte aligned arena\n");
  {
    // region_size is deliberately smaller than the allocation, so each one takes a region of its
    // own and the allocation is what decides that region's size.
    NYA_Arena* aligned = nya_arena_create(.name = "aligned", .alignment = 64, .region_size = nya_kibyte_to_byte(4));

    for (u32 i = 0; i < 16; i++) {
      u8* block = nya_arena_alloc(aligned, 5000);
      nya_assert(((uintptr_t)block % 64) == 0, "allocation is not 64 byte aligned");
      nya_memset(block, 0xAB, 5000);
    }

    nya_dll_foreach (aligned, region) {
      nya_assert(
        region->used <= region->capacity,
        "region reports " FMTu64 " bytes used of a " FMTu64 " byte capacity",
        region->used,
        region->capacity
      );
    }

    nya_arena_destroy(aligned);
  }
  printf("  PASSED\n");

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: the alignment check rejects an even non-power-of-two
  // ─────────────────────────────────────────────────────────────────────────────
  printf("TEST: an alignment of 24 is rejected\n");
  {
    // The on-stack constructor, because the heap one mallocs the NYA_Arena *before* it validates the
    // options — so the assertion below longjmps straight past the free and LeakSanitizer, correctly,
    // reports the 48 bytes. That is a property of unwinding out of C rather than anything to pin.
    // 48 KiB divides by 24, so the region-size check passes and the alignment check is what fires.
    nya_expect_crash({
      NYA_Arena bad = nya_arena_create_on_stack(.name = "bad", .alignment = 24, .region_size = nya_kibyte_to_byte(48));
      (void)bad;
    });
    nya_assert(nya_crash_caught()->source == NYA_CRASH_SOURCE_ASSERT, "expected an assertion, got something else");
  }
  printf("  PASSED\n");

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: nya_array_from_argv
  // ─────────────────────────────────────────────────────────────────────────────
  //
  // Exercised at all, which it had never been. The macro carried three compile errors — a `s32(0)`
  // function style cast, and an NYA_String* pushed into an array of NYA_String — none of which a
  // macro reports until something expands it. Nothing did, while it sat in base_array.h's public API
  // overview the whole time.
  printf("TEST: nya_array_from_argv\n");
  {
    const char* fake_argv[] = { "build", "run", "test" };

    NYA_ArrayᐸNYA_Stringᐳ* args = nya_array_from_argv(arena, 3, fake_argv);
    nya_assert(args->length == 3, "expected 3 arguments, got " FMTu64, args->length);
    nya_assert(nya_string_equals(nya_array_get(args, 0), "build"), "argument 0 is not \"build\"");
    nya_assert(nya_string_equals(nya_array_get(args, 2), "test"), "argument 2 is not \"test\"");

    // A zero argc gives an empty array rather than a zero capacity one that cannot be pushed to,
    // and a negative one is clamped rather than becoming an enormous capacity.
    NYA_ArrayᐸNYA_Stringᐳ* none = nya_array_from_argv(arena, 0, fake_argv);
    nya_assert(none->length == 0, "expected no arguments, got " FMTu64, none->length);

    NYA_ArrayᐸNYA_Stringᐳ* negative = nya_array_from_argv(arena, -1, fake_argv);
    nya_assert(negative->length == 0, "expected no arguments from a negative argc, got " FMTu64, negative->length);

    // The array is still usable after the zero-capacity start, which is the other half of the fix.
    nya_array_add(none, *nya_string_from(arena, "late"));
    nya_assert(none->length == 1, "could not push onto an array built from an empty argv");
  }
  printf("  PASSED\n");

  nya_arena_destroy(arena);

  printf("PASSED: test_bug_container_edges\n");
  return 0;
}
