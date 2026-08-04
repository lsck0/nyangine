/**
 * Ring buffer edge cases.
 *
 * test_ring.c exercises the ordinary path. The interesting states for a ring are the ones where
 * head has moved past tail in the backing array, where the buffer is exactly full, and where a
 * resize has to reconstruct a wrapped layout — none of which the existing tests reach, because they
 * only ever push a couple of items into a fresh buffer.
 **/

#include "nyangine/nyangine.c"
#include "nyangine/nyangine.h"

typedef struct {
  u32 id;
} Item;

nya_derive_ring(Item);

/** The ring's contents, front to back, as a comparable string of ids. */
static void collect(NYA_RingᐸItemᐳ* ring, NYA_Arena* arena, NYA_String* out) {
  nya_string_clear(out);
  for (u64 i = 0; i < nya_ring_length(ring); i++) nya_string_extend_sprintf(out, "%u,", nya_ring_at(ring, i)->id);
  nya_unused(arena);
}

s32 main(void) {
  NYA_Arena* arena = nya_arena_create(.name = "test_ring_edges");
  NYA_String* seen = nya_string_create(arena);

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: filling exactly to capacity
  // ─────────────────────────────────────────────────────────────────────────────
  printf("TEST: exactly full\n");
  {
    NYA_RingᐸItemᐳ* ring = nya_ring_create_with_capacity(arena, Item, 4);

    for (u32 i = 1; i <= 4; i++) nya_ring_push(ring, ((Item){ .id = i }));

    nya_assert(nya_ring_length(ring) == 4);
    nya_assert(nya_ring_capacity(ring) == 4);
    nya_assert(nya_ring_front(ring)->id == 1);
    nya_assert(nya_ring_back(ring)->id == 4);

    collect(ring, arena, seen);
    nya_assert(nya_string_equals(seen, "1,2,3,4,"), "got \"" NYA_FMT_STRING "\"", NYA_FMT_STRING_ARG(seen));
    printf("  PASSED\n");
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: pushing past capacity overwrites the oldest entry
  // ─────────────────────────────────────────────────────────────────────────────
  printf("TEST: overwrite when full\n");
  {
    NYA_RingᐸItemᐳ* ring = nya_ring_create_with_capacity(arena, Item, 4);

    for (u32 i = 1; i <= 6; i++) nya_ring_push(ring, ((Item){ .id = i }));

    // 1 and 2 have been pushed out; the length stays at capacity.
    nya_assert(nya_ring_length(ring) == 4);
    nya_assert(nya_ring_front(ring)->id == 3);
    nya_assert(nya_ring_back(ring)->id == 6);

    collect(ring, arena, seen);
    nya_assert(nya_string_equals(seen, "3,4,5,6,"), "got \"" NYA_FMT_STRING "\"", NYA_FMT_STRING_ARG(seen));

    // Peek agrees with at, and both are relative to the front rather than to the array.
    nya_assert(nya_ring_peek(ring, 0)->id == 3);
    nya_assert(nya_ring_peek(ring, 3)->id == 6);
    printf("  PASSED\n");
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: a wrapped layout, where head sits after tail in the backing array
  // ─────────────────────────────────────────────────────────────────────────────
  printf("TEST: wrapped layout\n");
  {
    NYA_RingᐸItemᐳ* ring = nya_ring_create_with_capacity(arena, Item, 4);

    // Fill, drain most of it, then refill: head is now part way along the array and the contents
    // straddle the end.
    for (u32 i = 1; i <= 4; i++) nya_ring_push(ring, ((Item){ .id = i }));
    (void)nya_ring_pop(ring);
    (void)nya_ring_pop(ring);
    (void)nya_ring_pop(ring);
    nya_ring_push(ring, ((Item){ .id = 5 }));
    nya_ring_push(ring, ((Item){ .id = 6 }));

    nya_assert(ring->head > ring->tail, "this test is pointless unless the contents wrap");
    nya_assert(nya_ring_length(ring) == 3);

    collect(ring, arena, seen);
    nya_assert(nya_string_equals(seen, "4,5,6,"), "got \"" NYA_FMT_STRING "\"", NYA_FMT_STRING_ARG(seen));
    nya_assert(nya_ring_front(ring)->id == 4);
    nya_assert(nya_ring_back(ring)->id == 6);
    printf("  PASSED\n");
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: growing a wrapped ring preserves order
  //
  // The existing resize test grows a ring that never wrapped, so the copy loop's modulo never did
  // anything. This is the case it exists for.
  // ─────────────────────────────────────────────────────────────────────────────
  printf("TEST: resize a wrapped ring\n");
  {
    NYA_RingᐸItemᐳ* ring = nya_ring_create_with_capacity(arena, Item, 4);

    for (u32 i = 1; i <= 6; i++) nya_ring_push(ring, ((Item){ .id = i }));   // wraps, holds 3..6
    nya_assert(ring->head != 0, "expected a wrapped layout before resizing");

    nya_ring_resize(ring, 16);

    nya_assert(nya_ring_capacity(ring) == 16);
    nya_assert(nya_ring_length(ring) == 4);
    nya_assert(ring->head == 0);

    collect(ring, arena, seen);
    nya_assert(nya_string_equals(seen, "3,4,5,6,"), "after resize: \"" NYA_FMT_STRING "\"", NYA_FMT_STRING_ARG(seen));

    // The resized ring still works: push up to the new capacity and back round.
    for (u32 i = 7; i <= 18; i++) nya_ring_push(ring, ((Item){ .id = i }));
    nya_assert(nya_ring_length(ring) == 16);
    nya_assert(nya_ring_back(ring)->id == 18);
    nya_assert(nya_ring_front(ring)->id == 3);
    printf("  PASSED\n");
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: resizing to exactly the current length
  //
  // tail is set to the length after a resize, which is only a valid index while the capacity is
  // strictly greater. When they are equal, tail has to wrap to zero or the next push writes one
  // past the end of the buffer.
  // ─────────────────────────────────────────────────────────────────────────────
  printf("TEST: resize to exactly the length\n");
  {
    NYA_RingᐸItemᐳ* ring = nya_ring_create_with_capacity(arena, Item, 8);

    for (u32 i = 1; i <= 4; i++) nya_ring_push(ring, ((Item){ .id = i }));

    nya_ring_resize(ring, 4);   // capacity now equals length

    nya_assert(nya_ring_capacity(ring) == 4);
    nya_assert(nya_ring_length(ring) == 4);
    nya_assert(ring->tail < ring->capacity, "tail " FMTu64 " is not a valid index into a capacity of " FMTu64, ring->tail, ring->capacity);

    collect(ring, arena, seen);
    nya_assert(nya_string_equals(seen, "1,2,3,4,"), "got \"" NYA_FMT_STRING "\"", NYA_FMT_STRING_ARG(seen));

    // The push that would land on the bad index, overwriting the oldest entry.
    nya_ring_push(ring, ((Item){ .id = 9 }));
    nya_assert(nya_ring_length(ring) == 4);
    nya_assert(nya_ring_back(ring)->id == 9);
    nya_assert(nya_ring_front(ring)->id == 2);
    printf("  PASSED\n");
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: drain to empty and refill
  // ─────────────────────────────────────────────────────────────────────────────
  printf("TEST: drain and refill\n");
  {
    NYA_RingᐸItemᐳ* ring = nya_ring_create_with_capacity(arena, Item, 4);

    for (u32 round = 0; round < 3; round++) {
      for (u32 i = 1; i <= 4; i++) nya_ring_push(ring, ((Item){ .id = round * 10 + i }));
      nya_assert(nya_ring_length(ring) == 4);

      for (u32 i = 1; i <= 4; i++) {
        Item popped = nya_ring_pop(ring);
        nya_assert(popped.id == round * 10 + i);
      }
      nya_assert(nya_ring_is_empty(ring));
    }

    // Popping an empty ring is a programming error, not a silent garbage read.
    nya_expect_crash((void)nya_ring_pop(ring));
    nya_expect_crash((void)nya_ring_front(ring));
    nya_expect_crash((void)nya_ring_back(ring));
    printf("  PASSED\n");
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: clear resets the layout, not just the length
  // ─────────────────────────────────────────────────────────────────────────────
  printf("TEST: clear\n");
  {
    NYA_RingᐸItemᐳ* ring = nya_ring_create_with_capacity(arena, Item, 4);
    for (u32 i = 1; i <= 6; i++) nya_ring_push(ring, ((Item){ .id = i }));   // leaves it wrapped

    nya_ring_clear(ring);
    nya_assert(nya_ring_is_empty(ring));
    nya_assert(nya_ring_length(ring) == 0);

    // Reusing it after a clear behaves like a fresh ring.
    for (u32 i = 1; i <= 3; i++) nya_ring_push(ring, ((Item){ .id = i }));
    collect(ring, arena, seen);
    nya_assert(nya_string_equals(seen, "1,2,3,"), "got \"" NYA_FMT_STRING "\"", NYA_FMT_STRING_ARG(seen));
    printf("  PASSED\n");
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: at() is bounds checked against the length, not the capacity
  // ─────────────────────────────────────────────────────────────────────────────
  printf("TEST: bounds\n");
  {
    NYA_RingᐸItemᐳ* ring = nya_ring_create_with_capacity(arena, Item, 8);
    nya_ring_push(ring, ((Item){ .id = 1 }));
    nya_ring_push(ring, ((Item){ .id = 2 }));

    nya_assert(nya_ring_at(ring, 0)->id == 1);
    nya_assert(nya_ring_at(ring, 1)->id == 2);

    // Index 2 is within the capacity but past the contents.
    nya_expect_crash((void)nya_ring_at(ring, 2));
    nya_expect_crash((void)nya_ring_peek(ring, 2));
    printf("  PASSED\n");
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: copy is independent, and carries a wrapped layout correctly
  // ─────────────────────────────────────────────────────────────────────────────
  printf("TEST: copy\n");
  {
    NYA_RingᐸItemᐳ* ring = nya_ring_create_with_capacity(arena, Item, 4);
    for (u32 i = 1; i <= 6; i++) nya_ring_push(ring, ((Item){ .id = i }));   // wrapped, holds 3..6

    NYA_RingᐸItemᐳ  copy_val = nya_ring_copy(ring);
    NYA_RingᐸItemᐳ* copy     = &copy_val;

    nya_assert(nya_ring_length(copy) == nya_ring_length(ring));
    for (u64 i = 0; i < nya_ring_length(ring); i++) nya_assert(nya_ring_at(copy, i)->id == nya_ring_at(ring, i)->id);

    // Mutating one must not disturb the other.
    nya_ring_push(ring, ((Item){ .id = 99 }));
    nya_assert(nya_ring_back(copy)->id == 6);
    printf("  PASSED\n");
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // CLEANUP
  // ─────────────────────────────────────────────────────────────────────────────
  nya_arena_destroy(arena);

  printf("PASSED: test_ring_edges\n");
  return 0;
}
