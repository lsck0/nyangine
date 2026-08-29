/**
 * Regression test for growth from a zero capacity array (base_array.h).
 *
 * nya_array_add grows with
 *
 *     if (length == capacity) nya_array_resize(arr_ptr, 2UL * capacity);
 *
 * which is a no-op at capacity zero, and the element is then written through a null items pointer —
 * nya_array_create_with_capacity_on_stack leaves it null when the initial capacity is zero.
 *
 * Two ways in, both exercised below: asking for a zero capacity outright, and shrinking an empty
 * array to fit, which is what nya_string_shrink_to_fit does to an empty string.
 *
 * nya_array_insert grows the same way and has the same hole.
 * */
#include "nyangine/nyangine.c"
#include "nyangine/nyangine.h"

s32 main(void) {
  NYA_Arena* arena = nya_arena_create(.name = "test_bug_array_zero_capacity");

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: created with zero capacity
  // ─────────────────────────────────────────────────────────────────────────────
  {
    NYA_Arrayᐸu32ᐳ* numbers = nya_array_create_with_capacity(arena, u32, 0);
    nya_assert(numbers->capacity == 0);
    nya_assert(numbers->items == nullptr);

    nya_array_add(numbers, 7U);

    nya_assert(numbers->length == 1);
    nya_assert(numbers->capacity >= 1, "growing from zero must actually allocate");
    nya_assert(numbers->items[0] == 7);
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: shrunk to fit while empty, then added to
  // ─────────────────────────────────────────────────────────────────────────────
  {
    NYA_Arrayᐸu32ᐳ* numbers = nya_array_create(arena, u32);
    nya_array_shrink_to_fit(numbers);
    nya_assert(numbers->capacity == 0);

    nya_array_add(numbers, 9U);

    nya_assert(numbers->length == 1);
    nya_assert(numbers->items[0] == 9);
  }

  nya_arena_destroy(arena);

  nya_log_info("PASSED: an array grows correctly from zero capacity");
  return 0;
}
