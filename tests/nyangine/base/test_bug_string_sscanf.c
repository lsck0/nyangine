/**
 * Regression test for nya_string_sscanf reading past the string (base_string.c).
 *
 * NYA_String is length counted and carries no terminator, so handing str->items straight to vsscanf
 * let it run to whatever zero byte came next in the arena. A string whose capacity exactly equals
 * its length — which is what nya_string_shrink_to_fit produces — has no such byte inside its own
 * allocation.
 *
 * Asserted on the value rather than on a sanitizer report, deliberately. glibc's sscanf measures its
 * input with an internal strlen that ASAN cannot intercept, so the overrun is invisible to it; and a
 * freshly mapped arena region is zero filled, so an overrun off the end of a string usually stops
 * immediately and reads correctly by luck. Neither makes the bug less real, they just make it
 * undetectable that way.
 *
 * So the byte after the string is made a digit, inside the string's own capacity. Writing two
 * characters into a buffer that already held sixteen leaves length at 2 with items[2] still '3',
 * which is exactly the shape the bug produces: a scan with no terminator to stop at reads the lot.
 * */
#include "nyangine/nyangine.c"
#include "nyangine/nyangine.h"

s32 main(void) {
  NYA_Arena* arena = nya_arena_create(.name = "test_bug_string_sscanf");

  NYA_String* digits = nya_string_from(arena, "1234567890123456");
  nya_assert(digits->length == 16);
  nya_assert(digits->capacity == 16, "the string must own exactly its bytes and no slack");

  // Back to "12", over a buffer whose remaining bytes are still digits.
  nya_string_clear(digits);
  nya_string_push_back(digits, '1');
  nya_string_push_back(digits, '2');

  nya_assert(digits->length == 2);
  nya_assert(digits->items[2] == '3', "the test needs a digit immediately past the string");

  long long value = 0;
  s32       count = nya_string_sscanf(digits, "%lld", &value);

  nya_assert(count == 1, "expected one conversion, got %d", count);
  nya_assert(value == 12, "read %lld, so the scan ran past the end of a 2 byte string", value);

  // The ordinary case still works, including a format with more than one conversion.
  NYA_String* pair = nya_string_from(arena, "7 42");
  nya_string_shrink_to_fit(pair);

  s32 a = 0, b = 0;
  count = nya_string_sscanf(pair, "%d %d", &a, &b);
  nya_assert(count == 2);
  nya_assert(a == 7 && b == 42, "got %d and %d", a, b);

  nya_arena_destroy(arena);

  nya_info("PASSED: nya_string_sscanf does not read past the string");
  return 0;
}
