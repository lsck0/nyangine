/**
 * Regression test for growth from a zero capacity array (base_array.h).
 * */
#include "nyangine-core/nyangine.c"
#include "nyangine-core/nyangine.h"

s32 main(void) {
  NYA_Arena* arena = nya_arena_create(.name = "test_bug_array_zero_capacity");

  // TEST: created with zero capacity
  {
    NYA_Arrayᐸu32ᐳ* numbers = nya_array_create_with_capacity(arena, u32, 0);
    nya_assert(numbers->capacity == 0);
    nya_assert(numbers->items == nullptr);

    nya_array_add(numbers, 7U);

    nya_assert(numbers->length == 1);
    nya_assert(numbers->capacity >= 1, "growing from zero must actually allocate");
    nya_assert(numbers->items[0] == 7);
  }

  // TEST: shrunk to fit while empty, then added to
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
