/**
 * Regression test for the memmove size in nya_array_insert_many (base_array.h).
 *
 * Same defect as nya_array_insert: the shift is sized
 *
 *     (arr_ptr)->length * sizeof(*items) - (start_index)
 *
 * instead of ((length - start_index) * sizeof(*items)). Capacity is preallocated to exactly what
 * the reserve would ask for, so no growth happens and the memmove is the only writer past the end.
 * */
#include "nyangine/nyangine.c"
#include "nyangine/nyangine.h"

s32 main(void) {
  NYA_Arena* arena = nya_arena_create(.name = "test_bug_array_insert_many");

  NYA_Arrayᐸu64ᐳ* numbers = nya_array_create_with_capacity(arena, u64, 1002);
  for (u64 i = 0; i < 1000; i++) nya_array_add(numbers, i);

  nya_assert(numbers->length == 1000);
  nya_assert(numbers->capacity == 1002, "the growth path must not run, or the overrun is absorbed");

  // 1000 * 8 - 1 = 7999 bytes moved to items + 3, ending 7 bytes past the 8016 byte allocation.
  nya_array_insert_many(numbers, 1, 111UL, 222UL);

  nya_assert(numbers->length == 1002);
  nya_assert(numbers->items[0] == 0);
  nya_assert(numbers->items[1] == 111);
  nya_assert(numbers->items[2] == 222);
  for (u64 i = 3; i < numbers->length; i++) {
    nya_assert(numbers->items[i] == i - 2, "element " FMTu64 " is " FMTu64 ", expected " FMTu64, i, numbers->items[i], i - 2);
  }

  nya_array_destroy(numbers);
  nya_arena_destroy(arena);

  nya_log_info("PASSED: nya_array_insert_many stays inside its allocation");
  return 0;
}
