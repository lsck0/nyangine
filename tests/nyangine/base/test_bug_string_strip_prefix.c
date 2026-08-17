/**
 * Regression test for the missing length guard in nya_string_strip_prefix (base_string.c).
 *
 * It compares strlen(prefix) bytes against the string without first checking that the string is at
 * least that long:
 *
 *     if (nya_memcmp(str->items, prefix, prefix_length) == 0) { ... }
 *
 * Its sibling nya_string_strip_suffix does check, and returns early. The prefix below is long
 * enough to reach past the arena's alignment padding, which is what hides a shorter one.
 *
 * src/build/asset.c calls this on paths produced by a filesystem walk, so the input is not always
 * known to be longer than the prefix.
 * */
#include "nyangine/nyangine.c"
#include "nyangine/nyangine.h"

s32 main(void) {
  NYA_Arena* arena = nya_arena_create(.name = "test_bug_string_strip_prefix");

  NYA_String* text = nya_string_from(arena, "ab");
  nya_assert(text->length == 2);

  // A prefix longer than the whole string. Nothing should be stripped, and nothing read past the
  // two bytes the string owns.
  nya_string_strip_prefix(text, "a_prefix_far_longer_than_the_string_it_is_being_compared_against");

  nya_assert(text->length == 2, "nothing matched, so nothing should have been stripped");
  nya_assert(nya_string_equals(text, "ab"));

  // The ordinary case still works.
  nya_string_strip_prefix(text, "a");
  nya_assert(nya_string_equals(text, "b"));

  nya_arena_destroy(arena);

  nya_info("PASSED: nya_string_strip_prefix does not read past the string");
  return 0;
}
