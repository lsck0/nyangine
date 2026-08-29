/**
 * Regression test for the memmove size in nya_array_insert (base_array.h).
 *
 * The macro computes the number of bytes to shift as
 *
 *     (arr_ptr)->length * sizeof(*items) - (index)
 *
 * where the correct expression is ((length - index) * sizeof(*items)). The `- index` sits outside
 * the multiply, so the shift copies `index * (sizeof - 1)` bytes too many.
 *
 * It is invisible for a one byte element type, where the two expressions coincide — which is every
 * NYA_String — and invisible for a small array, where the arena's padding absorbs the overrun. The
 * capacity below is deliberately exactly one above the length so the growth path does not run and
 * the memmove is the only thing that can write past the end.
 * */
#include "nyangine/nyangine.c"
#include "nyangine/nyangine.h"

s32 main(void) {
  NYA_Arena* arena = nya_arena_create(.name = "test_bug_array_insert");

  NYA_Arrayᐸu64ᐳ* numbers = nya_array_create_with_capacity(arena, u64, 1000);
  for (u64 i = 0; i < 999; i++) nya_array_add(numbers, i);

  nya_assert(numbers->length == 999);
  nya_assert(numbers->capacity == 1000, "the growth path must not run, or the overrun is absorbed");

  // 999 * 8 - 1 = 7991 bytes moved to items + 2, ending 7 bytes past the 8000 byte allocation.
  nya_array_insert(numbers, 12345UL, 1);

  nya_assert(numbers->length == 1000);
  nya_assert(numbers->items[0] == 0);
  nya_assert(numbers->items[1] == 12345);
  for (u64 i = 2; i < numbers->length; i++) {
    nya_assert(numbers->items[i] == i - 1, "element " FMTu64 " is " FMTu64 ", expected " FMTu64, i, numbers->items[i], i - 1);
  }

  nya_array_destroy(numbers);
  nya_arena_destroy(arena);

  nya_log_info("PASSED: nya_array_insert stays inside its allocation");
  return 0;
}
