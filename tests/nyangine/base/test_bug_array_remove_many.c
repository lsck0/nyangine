/**
 * Regression test for the memmove size in nya_array_remove_many (base_array.h).
 *
 * The shift is sized
 *
 *     (arr_ptr)->length * sizeof(*items) - (start_index) - (count)
 *
 * instead of ((length - start_index - count) * sizeof(*items)). Unlike the insert variants this
 * reads as well as writes past the end, since the source is items + start_index + count.
 * */
#include "nyangine/nyangine.c"
#include "nyangine/nyangine.h"

s32 main(void) {
  NYA_Arena* arena = nya_arena_create(.name = "test_bug_array_remove_many");

  NYA_Arrayᐸu64ᐳ* numbers = nya_array_create_with_capacity(arena, u64, 1000);
  for (u64 i = 0; i < 1000; i++) nya_array_add(numbers, i);

  nya_assert(numbers->length == 1000);
  nya_assert(numbers->capacity == 1000);

  // 1000 * 8 - 1 - 2 = 7997 bytes read from items + 3, ending 21 bytes past the 8000 byte
  // allocation, and written to items + 1, ending 5 bytes past it.
  nya_array_remove_many(numbers, 1, 2);

  nya_assert(numbers->length == 998);
  nya_assert(numbers->items[0] == 0);
  for (u64 i = 1; i < numbers->length; i++) {
    nya_assert(numbers->items[i] == i + 2, "element " FMTu64 " is " FMTu64 ", expected " FMTu64, i, numbers->items[i], i + 2);
  }

  nya_array_destroy(numbers);
  nya_arena_destroy(arena);

  nya_log_info("PASSED: nya_array_remove_many stays inside its allocation");
  return 0;
}
