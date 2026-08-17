/**
 * base_string edge cases.
 *
 * test_string.c covers the ordinary path of most of these. This file is deliberately adversarial:
 * empty strings, single characters, needles longer than the haystack, matches at the very start and
 * very end, overlapping occurrences, and the growth boundary where a buffer has to reallocate.
 * Those are where string code goes wrong, and none of them were exercised.
 **/

#include "nyangine/nyangine.c"
#include "nyangine/nyangine.h"

#define ASSERT_STR(str, expected)                                                                                                                    \
  nya_assert(nya_string_equals((str), (expected)), "got \"" NYA_FMT_STRING "\", expected \"%s\"", NYA_FMT_STRING_ARG(str), (expected))

s32 main(void) {
  NYA_Arena* arena = nya_arena_create(.name = "test_string_edges");

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: nya_string_extend_sprintf, including onto an empty string
  // ─────────────────────────────────────────────────────────────────────────────
  printf("TEST: extend_sprintf\n");
  {
    NYA_String* s = nya_string_create(arena);
    nya_string_extend_sprintf(s, "%d", 42);
    ASSERT_STR(s, "42");

    nya_string_extend_sprintf(s, "-%s", "tail");
    ASSERT_STR(s, "42-tail");

    // A format producing nothing must leave the string alone.
    nya_string_extend_sprintf(s, "%s", "");
    ASSERT_STR(s, "42-tail");

    // Long enough to force a reallocation part way through.
    NYA_String* big = nya_string_create(arena);
    for (u32 i = 0; i < 200; i++) nya_string_extend_sprintf(big, "%03u,", i);
    nya_assert(big->length == 200 * 4);
    nya_assert(nya_string_starts_with(big, "000,001,"));
    nya_assert(nya_string_ends_with(big, ",199,"));
    printf("  PASSED\n");
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: nya_string_extend_front_sprintf
  //
  // The prepend has to survive content already being there: the formatted text goes at the front
  // and everything that was in the string stays intact behind it.
  // ─────────────────────────────────────────────────────────────────────────────
  printf("TEST: extend_front_sprintf\n");
  {
    // Onto an empty string first, which is the simplest case.
    NYA_String* empty = nya_string_create(arena);
    nya_string_extend_front_sprintf(empty, "%d", 7);
    ASSERT_STR(empty, "7");

    // Onto existing content: this is the case that matters.
    NYA_String* s = nya_string_from(arena, "world");
    nya_string_extend_front_sprintf(s, "%s ", "hello");
    ASSERT_STR(s, "hello world");

    // Twice, so the second prepend has to move content that was itself prepended.
    nya_string_extend_front_sprintf(s, "[%d] ", 1);
    ASSERT_STR(s, "[1] hello world");
    printf("  PASSED\n");
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: nya_string_push_back and the growth boundary
  // ─────────────────────────────────────────────────────────────────────────────
  printf("TEST: push_back\n");
  {
    NYA_String* s = nya_string_create_with_capacity(arena, 4);
    for (u8 c = 'a'; c <= 'z'; c++) nya_string_push_back(s, c);

    nya_assert(s->length == 26);
    ASSERT_STR(s, "abcdefghijklmnopqrstuvwxyz");
    nya_assert(s->capacity >= 26);

    // A zero byte is a byte like any other: NYA_String carries a length, not a terminator.
    NYA_String* withnul = nya_string_create(arena);
    nya_string_push_back(withnul, 'a');
    nya_string_push_back(withnul, '\0');
    nya_string_push_back(withnul, 'b');
    nya_assert(withnul->length == 3);
    nya_assert(withnul->items[1] == '\0');
    nya_assert(withnul->items[2] == 'b');
    printf("  PASSED\n");
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: nya_string_shrink_to_fit
  // ─────────────────────────────────────────────────────────────────────────────
  printf("TEST: shrink_to_fit\n");
  {
    NYA_String* s = nya_string_create_with_capacity(arena, 256);
    nya_string_extend(s, "small");
    nya_assert(s->capacity >= 256);

    nya_string_shrink_to_fit(s);
    nya_assert(s->capacity == s->length);
    ASSERT_STR(s, "small");   // shrinking must not lose the contents

    // Shrinking an empty string is not a division by zero or a null deref.
    NYA_String* e = nya_string_create_with_capacity(arena, 64);
    nya_string_shrink_to_fit(e);
    nya_assert(e->length == 0);
    printf("  PASSED\n");
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: substring boundaries
  // ─────────────────────────────────────────────────────────────────────────────
  printf("TEST: substring boundaries\n");
  {
    NYA_String* s = nya_string_from(arena, "abcdef");

    ASSERT_STR(nya_string_substring_excld(arena, s, 0, 6), "abcdef");   // the whole thing
    ASSERT_STR(nya_string_substring_excld(arena, s, 0, 0), "");         // empty at the start
    ASSERT_STR(nya_string_substring_excld(arena, s, 6, 6), "");         // empty at the very end
    ASSERT_STR(nya_string_substring_excld(arena, s, 5, 6), "f");        // last character
    ASSERT_STR(nya_string_substring_excld(arena, s, 0, 1), "a");        // first character

    ASSERT_STR(nya_string_substring_incld(arena, s, 0, 5), "abcdef");   // inclusive end
    ASSERT_STR(nya_string_substring_incld(arena, s, 2, 2), "c");        // single character

    // Out of range is a programming error, not a silently clamped result.
    nya_expect_crash((void)nya_string_substring_excld(arena, s, 0, 7));
    nya_expect_crash((void)nya_string_substring_excld(arena, s, 4, 2));
    nya_expect_crash((void)nya_string_substring_incld(arena, s, 0, 6));
    printf("  PASSED\n");
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: contains / starts_with / ends_with at the edges
  // ─────────────────────────────────────────────────────────────────────────────
  printf("TEST: search edges\n");
  {
    NYA_String* s     = nya_string_from(arena, "abc");
    NYA_String* empty = nya_string_create(arena);

    nya_assert(nya_string_contains(s, "a") == true);     // at the very start
    nya_assert(nya_string_contains(s, "c") == true);     // at the very end
    nya_assert(nya_string_contains(s, "abc") == true);   // the whole string
    nya_assert(nya_string_contains(s, "abcd") == false); // needle longer than haystack
    nya_assert(nya_string_contains(s, "d") == false);
    nya_assert(nya_string_contains(empty, "a") == false);

    nya_assert(nya_string_starts_with(s, "abc") == true);
    nya_assert(nya_string_starts_with(s, "abcd") == false);
    nya_assert(nya_string_starts_with(empty, "a") == false);

    nya_assert(nya_string_ends_with(s, "abc") == true);
    nya_assert(nya_string_ends_with(s, "abcd") == false);
    nya_assert(nya_string_ends_with(empty, "a") == false);
    printf("  PASSED\n");
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: count, including overlapping candidates
  // ─────────────────────────────────────────────────────────────────────────────
  printf("TEST: count\n");
  {
    NYA_String* aaa = nya_string_from(arena, "aaaa");

    nya_assert(nya_string_count(aaa, "a") == 4);
    // Non-overlapping, so "aaaa" holds two "aa" rather than three.
    nya_assert(nya_string_count(aaa, "aa") == 2);
    nya_assert(nya_string_count(aaa, "aaaa") == 1);
    nya_assert(nya_string_count(aaa, "aaaaa") == 0);   // longer than the haystack
    nya_assert(nya_string_count(aaa, "b") == 0);

    NYA_String* empty = nya_string_create(arena);
    nya_assert(nya_string_count(empty, "a") == 0);
    printf("  PASSED\n");
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: replace and remove at the edges
  // ─────────────────────────────────────────────────────────────────────────────
  printf("TEST: replace and remove\n");
  {
    // Replacement longer than the original, at the front and the back.
    NYA_String* a = nya_string_from(arena, "xbx");
    nya_string_replace(a, "x", "yy");
    ASSERT_STR(a, "yybyy");

    // Replacement shorter than the original.
    NYA_String* b = nya_string_from(arena, "aaXbbXcc");
    nya_string_replace(b, "X", "");
    ASSERT_STR(b, "aabbcc");

    // Every character replaced.
    NYA_String* c = nya_string_from(arena, "aaa");
    nya_string_replace(c, "a", "b");
    ASSERT_STR(c, "bbb");

    // Absent needle leaves the string alone.
    NYA_String* d = nya_string_from(arena, "hello");
    nya_string_replace(d, "z", "!");
    ASSERT_STR(d, "hello");

    // Removing everything gives the empty string, not a broken one.
    NYA_String* e = nya_string_from(arena, "zzz");
    nya_string_remove(e, "z");
    nya_assert(nya_string_is_empty(e));
    nya_assert(e->length == 0);
    printf("  PASSED\n");
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: strip_prefix / strip_suffix when the affix is the whole string
  // ─────────────────────────────────────────────────────────────────────────────
  printf("TEST: strip edges\n");
  {
    NYA_String* whole = nya_string_from(arena, "abc");
    nya_string_strip_prefix(whole, "abc");
    nya_assert(nya_string_is_empty(whole));

    NYA_String* whole2 = nya_string_from(arena, "abc");
    nya_string_strip_suffix(whole2, "abc");
    nya_assert(nya_string_is_empty(whole2));

    // A non-matching affix must not remove anything.
    NYA_String* keep = nya_string_from(arena, "abc");
    nya_string_strip_prefix(keep, "x");
    nya_string_strip_suffix(keep, "x");
    ASSERT_STR(keep, "abc");

    // An affix longer than the string is simply not a match.
    NYA_String* shorter = nya_string_from(arena, "ab");
    nya_string_strip_prefix(shorter, "abc");
    ASSERT_STR(shorter, "ab");

    // Stripping is done once, not repeatedly.
    NYA_String* twice = nya_string_from(arena, "aab");
    nya_string_strip_prefix(twice, "a");
    ASSERT_STR(twice, "ab");
    printf("  PASSED\n");
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: trim_whitespace
  // ─────────────────────────────────────────────────────────────────────────────
  printf("TEST: trim_whitespace\n");
  {
    NYA_String* both = nya_string_from(arena, "  \t\n hi \r\n ");
    nya_string_trim_whitespace(both);
    ASSERT_STR(both, "hi");

    // Nothing but whitespace collapses to empty rather than underflowing a length.
    NYA_String* blank = nya_string_from(arena, " \t\r\n ");
    nya_string_trim_whitespace(blank);
    nya_assert(nya_string_is_empty(blank));

    // Already trimmed is left alone, and interior whitespace is kept.
    NYA_String* inner = nya_string_from(arena, "a b");
    nya_string_trim_whitespace(inner);
    ASSERT_STR(inner, "a b");

    NYA_String* empty = nya_string_create(arena);
    nya_string_trim_whitespace(empty);
    nya_assert(nya_string_is_empty(empty));
    printf("  PASSED\n");
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: reverse
  // ─────────────────────────────────────────────────────────────────────────────
  printf("TEST: reverse\n");
  {
    NYA_String* odd = nya_string_from(arena, "abc");
    nya_string_reverse(odd);
    ASSERT_STR(odd, "cba");

    NYA_String* even = nya_string_from(arena, "abcd");
    nya_string_reverse(even);
    ASSERT_STR(even, "dcba");

    NYA_String* one = nya_string_from(arena, "a");
    nya_string_reverse(one);
    ASSERT_STR(one, "a");

    NYA_String* empty = nya_string_create(arena);
    nya_string_reverse(empty);
    nya_assert(nya_string_is_empty(empty));

    // Reversing twice is the identity.
    NYA_String* there = nya_string_from(arena, "nyangine");
    nya_string_reverse(there);
    nya_string_reverse(there);
    ASSERT_STR(there, "nyangine");
    printf("  PASSED\n");
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: split edges
  // ─────────────────────────────────────────────────────────────────────────────
  printf("TEST: split edges\n");
  {
    // Leading and trailing separators produce empty fields rather than being swallowed.
    NYA_String*            edged  = nya_string_from(arena, ",a,,b,");
    NYA_ArrayᐸNYA_Stringᐳ* fields = nya_string_split(arena, edged, ",");
    nya_assert(fields->length == 5, "expected 5 fields, got " FMTu64, fields->length);
    nya_assert(nya_string_is_empty(&fields->items[0]));
    ASSERT_STR(&fields->items[1], "a");
    nya_assert(nya_string_is_empty(&fields->items[2]));
    ASSERT_STR(&fields->items[3], "b");
    nya_assert(nya_string_is_empty(&fields->items[4]));

    // No separator present gives the whole string as one field.
    NYA_String*            whole = nya_string_from(arena, "abc");
    NYA_ArrayᐸNYA_Stringᐳ* one   = nya_string_split(arena, whole, ",");
    nya_assert(one->length == 1);
    ASSERT_STR(&one->items[0], "abc");

    // A multi character separator.
    NYA_String*            multi  = nya_string_from(arena, "a::b::c");
    NYA_ArrayᐸNYA_Stringᐳ* parts  = nya_string_split(arena, multi, "::");
    nya_assert(parts->length == 3);
    ASSERT_STR(&parts->items[2], "c");

    // Splitting nothing gives nothing.
    NYA_String*            empty  = nya_string_create(arena);
    NYA_ArrayᐸNYA_Stringᐳ* none   = nya_string_split(arena, empty, ",");
    nya_assert(none->length == 0);
    printf("  PASSED\n");
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: split_lines with the line ending styles that actually occur
  // ─────────────────────────────────────────────────────────────────────────────
  printf("TEST: split_lines\n");
  {
    NYA_String*            unix_style = nya_string_from(arena, "a\nb\nc");
    NYA_ArrayᐸNYA_Stringᐳ* lines      = nya_string_split_lines(arena, unix_style);
    nya_assert(lines->length == 3);
    ASSERT_STR(&lines->items[0], "a");
    ASSERT_STR(&lines->items[2], "c");

    // A trailing newline: the text ends, it does not begin an empty final line with content.
    NYA_String*            trailing = nya_string_from(arena, "a\nb\n");
    NYA_ArrayᐸNYA_Stringᐳ* tl       = nya_string_split_lines(arena, trailing);
    nya_assert(tl->length >= 2);
    ASSERT_STR(&tl->items[0], "a");
    ASSERT_STR(&tl->items[1], "b");
    printf("  PASSED\n");
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: to_upper / to_lower leave non-letters alone
  // ─────────────────────────────────────────────────────────────────────────────
  printf("TEST: case conversion\n");
  {
    NYA_String* mixed = nya_string_from(arena, "aB1_ ~z");
    nya_string_to_upper(mixed);
    ASSERT_STR(mixed, "AB1_ ~Z");
    nya_string_to_lower(mixed);
    ASSERT_STR(mixed, "ab1_ ~z");

    NYA_String* empty = nya_string_create(arena);
    nya_string_to_upper(empty);
    nya_assert(nya_string_is_empty(empty));
    printf("  PASSED\n");
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: to_cstring is NUL terminated and does not alias the source
  // ─────────────────────────────────────────────────────────────────────────────
  printf("TEST: to_cstring\n");
  {
    NYA_String* s = nya_string_from(arena, "abc");
    NYA_CString c = nya_string_to_cstring(arena, s);

    nya_assert(strlen(c) == 3);
    nya_assert(c[3] == '\0');
    nya_assert(strcmp(c, "abc") == 0);

    // Mutating the source afterwards must not change the copy.
    nya_string_extend(s, "def");
    nya_assert(strcmp(c, "abc") == 0);

    // An empty string still yields a valid empty C string.
    NYA_CString e = nya_string_to_cstring(arena, nya_string_create(arena));
    nya_assert(e != nullptr);
    nya_assert(e[0] == '\0');
    printf("  PASSED\n");
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: clone and concat are independent of their sources
  // ─────────────────────────────────────────────────────────────────────────────
  printf("TEST: clone and concat independence\n");
  {
    NYA_String* original = nya_string_from(arena, "abc");
    NYA_String* copy     = nya_string_clone(arena, original);

    nya_string_extend(original, "XYZ");
    ASSERT_STR(copy, "abc");

    NYA_String* left   = nya_string_from(arena, "ab");
    NYA_String* right  = nya_string_from(arena, "cd");
    NYA_String* joined = nya_string_concat(arena, left, right);
    ASSERT_STR(joined, "abcd");

    nya_string_extend(left, "!");
    ASSERT_STR(joined, "abcd");

    // Concatenating with empty on either side.
    NYA_String* empty = nya_string_create(arena);
    ASSERT_STR(nya_string_concat(arena, left, empty), "ab!");
    ASSERT_STR(nya_string_concat(arena, empty, right), "cd");
    ASSERT_STR(nya_string_concat(arena, empty, empty), "");
    printf("  PASSED\n");
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: extend with itself
  //
  // Self-extension has to read the original bytes even though the buffer may move underneath it.
  // ─────────────────────────────────────────────────────────────────────────────
  printf("TEST: self extend\n");
  {
    NYA_String* s = nya_string_from(arena, "ab");
    nya_string_extend(s, s);
    ASSERT_STR(s, "abab");
    printf("  PASSED\n");
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: an oversized separator dies on the bound rather than on the stack
  // ─────────────────────────────────────────────────────────────────────────────
  {
    printf("TEST: oversized separator\n");

    /*
     * nya_string_split and nya_string_count copy their needle onto the stack with nya_alloca to get
     * a null terminated form of it. The length comes from the caller, so a long enough needle used
     * to run off the end of the stack — a fault at whatever address the next frame would have
     * touched, with nothing pointing back at the split that caused it.
     *
     * nya_alloca is bounded now, so the same call dies at a named limit with the size in the
     * message. That is the behaviour being pinned here: predictable and attributable, not survival.
     */
    NYA_String* haystack = nya_string_from(arena, "the quick brown fox");

    NYA_String* huge = nya_string_create_with_capacity(arena, NYA_ALLOCA_MAX + 64);
    for (u64 i = 0; i < NYA_ALLOCA_MAX + 8; i++) nya_string_push_back(huge, 'x');
    nya_assert(huge->length > NYA_ALLOCA_MAX, "the separator has to exceed the bound to test it");

    nya_expect_crash((void)nya_string_split(arena, haystack, huge));
    nya_expect_crash((void)nya_string_count(haystack, huge));

    /*
     * The boundary itself still works, so the bound is a ceiling rather than a haircut. One under
     * the limit, because the copy is the separator plus its terminator.
     */
    NYA_String* at_limit = nya_string_create_with_capacity(arena, NYA_ALLOCA_MAX);
    for (u64 i = 0; i < NYA_ALLOCA_MAX - 1; i++) nya_string_push_back(at_limit, 'y');

    NYA_ArrayᐸNYA_Stringᐳ* unsplit = nya_string_split(arena, haystack, at_limit);
    nya_assert(unsplit->length == 1, "a separator that does not occur must leave the string whole");
    nya_assert(nya_string_count(haystack, at_limit) == 0, "a separator that does not occur must count zero");

    // And the ordinary case still behaves, so none of the above broke the common path.
    NYA_String*            space  = nya_string_from(arena, " ");
    NYA_ArrayᐸNYA_Stringᐳ* fields = nya_string_split(arena, haystack, space);
    nya_assert(fields->length == 4, "expected four fields, got " FMTu64, fields->length);

    printf("  PASSED\n");
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // CLEANUP
  // ─────────────────────────────────────────────────────────────────────────────
  nya_arena_destroy(arena);

  printf("PASSED: test_string_edges\n");
  return 0;
}
