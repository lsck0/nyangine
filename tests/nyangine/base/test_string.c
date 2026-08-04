/**
 * THIS FILE WAS CLANKER WANKED !!!
 **/

#include "nyangine/nyangine.c"
#include "nyangine/nyangine.h"

s32 main(void) {
  NYA_Arena* arena = nya_arena_create(.name = "test_string");

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: nya_string_create / nya_string_create_with_capacity
  // ─────────────────────────────────────────────────────────────────────────────
  NYA_String empty_str = *nya_string_create(arena);
  nya_assert(empty_str.length == 0);
  nya_assert(empty_str.items != nullptr);

  NYA_String cap_str = *nya_string_create_with_capacity(arena, 128);
  nya_assert(cap_str.length == 0);
  nya_assert(cap_str.capacity == 128);

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: nya_string_from
  // ─────────────────────────────────────────────────────────────────────────────
  NYA_String hello = *nya_string_from(arena, "hello");
  nya_assert(hello.length == 5);
  nya_assert(nya_memcmp(hello.items, "hello", 5) == 0);

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: nya_string_is_empty
  // ─────────────────────────────────────────────────────────────────────────────
  nya_assert(nya_string_is_empty(&empty_str) == true);
  nya_assert(nya_string_is_empty(&hello) == false);

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: nya_string_equals (cstring overload)
  // ─────────────────────────────────────────────────────────────────────────────
  nya_assert(nya_string_equals(&hello, "hello") == true);
  nya_assert(nya_string_equals(&hello, "Hello") == false);
  nya_assert(nya_string_equals(&hello, "hell") == false);
  nya_assert(nya_string_equals(&hello, "hello!") == false);

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: nya_string_equals (NYA_String overload)
  // ─────────────────────────────────────────────────────────────────────────────
  NYA_String hello2 = *nya_string_from(arena, "hello");
  NYA_String world  = *nya_string_from(arena, "world");
  nya_assert(nya_string_equals(&hello, &hello2) == true);
  nya_assert(nya_string_equals(&hello, &world) == false);

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: nya_string_equals (cstring-cstring overload)
  // ─────────────────────────────────────────────────────────────────────────────
  nya_assert(nya_string_equals("abc", "abc") == true);
  nya_assert(nya_string_equals("abc", "def") == false);
  nya_assert(nya_string_equals("abc", "ab") == false);

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: nya_string_contains (cstring overload)
  // ─────────────────────────────────────────────────────────────────────────────
  NYA_String sentence = *nya_string_from(arena, "hello world");
  nya_assert(nya_string_contains(&sentence, "hello") == true);
  nya_assert(nya_string_contains(&sentence, "world") == true);
  nya_assert(nya_string_contains(&sentence, "lo wo") == true);
  nya_assert(nya_string_contains(&sentence, "xyz") == false);
  nya_assert(nya_string_contains(&sentence, "") == false);

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: nya_string_contains (NYA_String overload)
  // ─────────────────────────────────────────────────────────────────────────────
  NYA_String substr_world = *nya_string_from(arena, "world");
  NYA_String substr_xyz   = *nya_string_from(arena, "xyz");
  nya_assert(nya_string_contains(&sentence, &substr_world) == true);
  nya_assert(nya_string_contains(&sentence, &substr_xyz) == false);

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: nya_string_starts_with
  // ─────────────────────────────────────────────────────────────────────────────
  nya_assert(nya_string_starts_with(&sentence, "hello") == true);
  nya_assert(nya_string_starts_with(&sentence, "hello world") == true);
  nya_assert(nya_string_starts_with(&sentence, "world") == false);
  nya_assert(nya_string_starts_with(&sentence, "hello world!") == false);

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: nya_string_ends_with
  // ─────────────────────────────────────────────────────────────────────────────
  nya_assert(nya_string_ends_with(&sentence, "world") == true);
  nya_assert(nya_string_ends_with(&sentence, "hello world") == true);
  nya_assert(nya_string_ends_with(&sentence, "hello") == false);
  nya_assert(nya_string_ends_with(&sentence, "!hello world") == false);

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: nya_string_clone
  // ─────────────────────────────────────────────────────────────────────────────
  NYA_String cloned = *nya_string_clone(arena, &hello);
  nya_assert(nya_string_equals(&cloned, &hello) == true);
  nya_assert(cloned.items != hello.items);

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: nya_string_concat
  // ─────────────────────────────────────────────────────────────────────────────
  NYA_String space    = *nya_string_from(arena, " ");
  NYA_String combined = *nya_string_concat(arena, &hello, &space);
  combined            = *nya_string_concat(arena, &combined, &world);
  nya_assert(nya_string_equals(&combined, "hello world") == true);

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: nya_string_sprintf
  // ─────────────────────────────────────────────────────────────────────────────
  NYA_String formatted = *nya_string_sprintf(arena, "num=%d str=%s", 42, "test");
  nya_assert(nya_string_equals(&formatted, "num=42 str=test") == true);

  NYA_String empty_fmt = *nya_string_sprintf(arena, "");
  nya_assert(nya_string_is_empty(&empty_fmt) == true);

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: nya_string_substring_excld
  // ─────────────────────────────────────────────────────────────────────────────
  NYA_String sub_excld = *nya_string_substring_excld(arena, &sentence, 0, 5);
  nya_assert(nya_string_equals(&sub_excld, "hello") == true);

  NYA_String sub_excld2 = *nya_string_substring_excld(arena, &sentence, 6, 11);
  nya_assert(nya_string_equals(&sub_excld2, "world") == true);

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: nya_string_substring_incld
  // ─────────────────────────────────────────────────────────────────────────────
  NYA_String sub_incld = *nya_string_substring_incld(arena, &sentence, 0, 4);
  nya_assert(nya_string_equals(&sub_incld, "hello") == true);

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: nya_string_split (cstring separator)
  // ─────────────────────────────────────────────────────────────────────────────
  NYA_String      csv   = *nya_string_from(arena, "a,b,c,d");
  NYA_ArrayᐸNYA_Stringᐳ parts = *nya_string_split(arena, &csv, ",");
  nya_assert(parts.length == 4);
  nya_assert(nya_string_equals(&parts.items[0], "a") == true);
  nya_assert(nya_string_equals(&parts.items[1], "b") == true);
  nya_assert(nya_string_equals(&parts.items[2], "c") == true);
  nya_assert(nya_string_equals(&parts.items[3], "d") == true);

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: nya_string_split (NYA_String separator)
  // ─────────────────────────────────────────────────────────────────────────────
  NYA_String      sep_str = *nya_string_from(arena, "::");
  NYA_String      data    = *nya_string_from(arena, "foo::bar::baz");
  NYA_ArrayᐸNYA_Stringᐳ parts2  = *nya_string_split(arena, &data, &sep_str);
  nya_assert(parts2.length == 3);
  nya_assert(nya_string_equals(&parts2.items[0], "foo") == true);
  nya_assert(nya_string_equals(&parts2.items[1], "bar") == true);
  nya_assert(nya_string_equals(&parts2.items[2], "baz") == true);

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: nya_string_split_lines
  // ─────────────────────────────────────────────────────────────────────────────
  NYA_String      multiline = *nya_string_from(arena, "line1\nline2\nline3");
  NYA_ArrayᐸNYA_Stringᐳ lines     = *nya_string_split_lines(arena, &multiline);
  nya_assert(lines.length == 3);
  nya_assert(nya_string_equals(&lines.items[0], "line1") == true);
  nya_assert(nya_string_equals(&lines.items[1], "line2") == true);
  nya_assert(nya_string_equals(&lines.items[2], "line3") == true);

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: nya_string_split_words
  // ─────────────────────────────────────────────────────────────────────────────
  NYA_String      whitespace = *nya_string_from(arena, "  foo   bar\tbaz  ");
  NYA_ArrayᐸNYA_Stringᐳ words      = *nya_string_split_words(arena, &whitespace);
  nya_assert(words.length == 3);
  nya_assert(nya_string_equals(&words.items[0], "foo") == true);
  nya_assert(nya_string_equals(&words.items[1], "bar") == true);
  nya_assert(nya_string_equals(&words.items[2], "baz") == true);

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: nya_string_join (cstring separator)
  // ─────────────────────────────────────────────────────────────────────────────
  NYA_String joined = *nya_string_join(arena, &parts, "-");
  nya_assert(nya_string_equals(&joined, "a-b-c-d") == true);

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: nya_string_join (NYA_String separator)
  // ─────────────────────────────────────────────────────────────────────────────
  NYA_String sep2    = *nya_string_from(arena, ", ");
  NYA_String joined2 = *nya_string_join(arena, &parts, &sep2);
  nya_assert(nya_string_equals(&joined2, "a, b, c, d") == true);

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: nya_string_count (cstring overload)
  // ─────────────────────────────────────────────────────────────────────────────
  NYA_String repeat = *nya_string_from(arena, "abcabcabc");
  nya_assert(nya_string_count(&repeat, "abc") == 3);
  nya_assert(nya_string_count(&repeat, "ab") == 3);
  nya_assert(nya_string_count(&repeat, "xyz") == 0);
  nya_assert(nya_string_count(&repeat, "") == 0);

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: nya_string_count (NYA_String overload)
  // ─────────────────────────────────────────────────────────────────────────────
  NYA_String abc_substr = *nya_string_from(arena, "abc");
  nya_assert(nya_string_count(&repeat, &abc_substr) == 3);

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: nya_string_clear
  // ─────────────────────────────────────────────────────────────────────────────
  NYA_String clear_str = *nya_string_from(arena, "test");
  nya_assert(nya_string_is_empty(&clear_str) == false);
  nya_string_clear(&clear_str);
  nya_assert(nya_string_is_empty(&clear_str) == true);
  nya_assert(clear_str.capacity > 0);

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: nya_string_extend (cstring overload)
  // ─────────────────────────────────────────────────────────────────────────────
  NYA_String extend_str = *nya_string_from(arena, "hello");
  nya_string_extend(&extend_str, " world");
  nya_assert(nya_string_equals(&extend_str, "hello world") == true);

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: nya_string_extend (NYA_String overload)
  // ─────────────────────────────────────────────────────────────────────────────
  NYA_String extend_str2 = *nya_string_from(arena, "foo");
  NYA_String ext         = *nya_string_from(arena, "bar");
  nya_string_extend(&extend_str2, &ext);
  nya_assert(nya_string_equals(&extend_str2, "foobar") == true);

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: nya_string_remove (cstring overload)
  // ─────────────────────────────────────────────────────────────────────────────
  NYA_String remove_str = *nya_string_from(arena, "hello world world");
  nya_string_remove(&remove_str, "world");
  nya_assert(nya_string_equals(&remove_str, "hello  ") == true);

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: nya_string_remove (NYA_String overload)
  // ─────────────────────────────────────────────────────────────────────────────
  NYA_String remove_str2 = *nya_string_from(arena, "abcXabcXabc");
  nya_string_remove(&remove_str2, "X");
  nya_assert(nya_string_equals(&remove_str2, "abcabcabc") == true);

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: nya_string_replace (cstring overload)
  // ─────────────────────────────────────────────────────────────────────────────
  NYA_String replace_str = *nya_string_from(arena, "hello world");
  nya_string_replace(&replace_str, "world", "universe");
  nya_assert(nya_string_equals(&replace_str, "hello universe") == true);

  NYA_String replace_str2 = *nya_string_from(arena, "aaa");
  nya_string_replace(&replace_str2, "a", "bb");
  nya_assert(nya_string_equals(&replace_str2, "bbbbbb") == true);

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: nya_string_replace (more cases)
  // ─────────────────────────────────────────────────────────────────────────────
  NYA_String replace_str4 = *nya_string_from(arena, "foo bar foo");
  nya_string_replace(&replace_str4, "foo", "baz");
  nya_assert(nya_string_equals(&replace_str4, "baz bar baz") == true);

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: nya_string_reverse
  // ─────────────────────────────────────────────────────────────────────────────
  NYA_String reverse_str = *nya_string_from(arena, "abcde");
  nya_string_reverse(&reverse_str);
  nya_assert(nya_string_equals(&reverse_str, "edcba") == true);

  NYA_String reverse_empty = *nya_string_create(arena);
  nya_string_reverse(&reverse_empty);
  nya_assert(nya_string_is_empty(&reverse_empty) == true);

  NYA_String reverse_single = *nya_string_from(arena, "x");
  nya_string_reverse(&reverse_single);
  nya_assert(nya_string_equals(&reverse_single, "x") == true);

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: nya_string_strip_prefix
  // ─────────────────────────────────────────────────────────────────────────────
  NYA_String prefix_str = *nya_string_from(arena, "prefix_content");
  nya_string_strip_prefix(&prefix_str, "prefix_");
  nya_assert(nya_string_equals(&prefix_str, "content") == true);

  NYA_String prefix_str2 = *nya_string_from(arena, "noprefix");
  nya_string_strip_prefix(&prefix_str2, "xyz");
  nya_assert(nya_string_equals(&prefix_str2, "noprefix") == true);

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: nya_string_strip_suffix
  // ─────────────────────────────────────────────────────────────────────────────
  NYA_String suffix_str = *nya_string_from(arena, "content_suffix");
  nya_string_strip_suffix(&suffix_str, "_suffix");
  nya_assert(nya_string_equals(&suffix_str, "content") == true);

  NYA_String suffix_str2 = *nya_string_from(arena, "nosuffix");
  nya_string_strip_suffix(&suffix_str2, "xyz");
  nya_assert(nya_string_equals(&suffix_str2, "nosuffix") == true);

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: nya_string_to_cstring
  // ─────────────────────────────────────────────────────────────────────────────
  NYA_String  cstr_test = *nya_string_from(arena, "hello");
  NYA_CString cstr      = nya_string_to_cstring(arena, &cstr_test);
  nya_assert(strcmp(cstr, "hello") == 0);
  nya_assert(cstr[5] == '\0');

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: nya_string_to_lower
  // ─────────────────────────────────────────────────────────────────────────────
  NYA_String lower_str = *nya_string_from(arena, "HeLLo WoRLD");
  nya_string_to_lower(&lower_str);
  nya_assert(nya_string_equals(&lower_str, "hello world") == true);

  NYA_String lower_already = *nya_string_from(arena, "already lower");
  nya_string_to_lower(&lower_already);
  nya_assert(nya_string_equals(&lower_already, "already lower") == true);

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: nya_string_to_upper
  // ─────────────────────────────────────────────────────────────────────────────
  NYA_String upper_str = *nya_string_from(arena, "HeLLo WoRLD");
  nya_string_to_upper(&upper_str);
  nya_assert(nya_string_equals(&upper_str, "HELLO WORLD") == true);

  NYA_String upper_already = *nya_string_from(arena, "ALREADY UPPER");
  nya_string_to_upper(&upper_already);
  nya_assert(nya_string_equals(&upper_already, "ALREADY UPPER") == true);

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: nya_string_trim_whitespace
  // ─────────────────────────────────────────────────────────────────────────────
  NYA_String trim_str = *nya_string_from(arena, "  \t hello world \n ");
  nya_string_trim_whitespace(&trim_str);
  nya_assert(nya_string_equals(&trim_str, "hello world") == true);

  NYA_String trim_str2 = *nya_string_from(arena, "no_whitespace");
  nya_string_trim_whitespace(&trim_str2);
  nya_assert(nya_string_equals(&trim_str2, "no_whitespace") == true);

  NYA_String trim_str3 = *nya_string_from(arena, "   ");
  nya_string_trim_whitespace(&trim_str3);
  nya_assert(nya_string_is_empty(&trim_str3) == true);

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: nya_string_sscanf
  // ─────────────────────────────────────────────────────────────────────────────
  NYA_String scanf_str = *nya_string_from(arena, "42 hello 3.14");
  s32        num       = 0;
  char       word[16]  = { 0 };
  f32        fval      = 0.0F;
  s32        ret       = nya_string_sscanf(&scanf_str, "%d %15s %f", &num, word, &fval);
  nya_assert(ret == 3);
  nya_assert(num == 42);
  nya_assert(strcmp(word, "hello") == 0);
  nya_assert(fval > 3.13F && fval < 3.15F);

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: nya_string_reserve
  // ─────────────────────────────────────────────────────────────────────────────
  NYA_String reserve_str = *nya_string_create(arena);
  nya_string_reserve(&reserve_str, 256);
  nya_assert(reserve_str.capacity >= 256);

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: nya_string_extend_front (cstring overload)
  // ─────────────────────────────────────────────────────────────────────────────
  NYA_String extend_front_str = *nya_string_from(arena, "world");
  nya_string_extend_front(&extend_front_str, "hello ");
  nya_assert(nya_string_equals(&extend_front_str, "hello world") == true);

  NYA_String extend_front_empty = *nya_string_create(arena);
  nya_string_extend_front(&extend_front_empty, "prefix");
  nya_assert(nya_string_equals(&extend_front_empty, "prefix") == true);

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: nya_string_extend_front (NYA_String overload)
  // ─────────────────────────────────────────────────────────────────────────────
  NYA_String extend_front_str2 = *nya_string_from(arena, "bar");
  NYA_String ext_front         = *nya_string_from(arena, "foo");
  nya_string_extend_front(&extend_front_str2, &ext_front);
  nya_assert(nya_string_equals(&extend_front_str2, "foobar") == true);

  NYA_String extend_front_empty2 = *nya_string_create(arena);
  NYA_String ext_front2          = *nya_string_from(arena, "start");
  nya_string_extend_front(&extend_front_empty2, &ext_front2);
  nya_assert(nya_string_equals(&extend_front_empty2, "start") == true);

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: edge cases - single character
  // ─────────────────────────────────────────────────────────────────────────────
  NYA_String single = *nya_string_from(arena, "x");
  nya_assert(nya_string_contains(&single, "x") == true);
  nya_assert(nya_string_starts_with(&single, "x") == true);
  nya_assert(nya_string_ends_with(&single, "x") == true);
  nya_assert(nya_string_count(&single, "x") == 1);

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: edge cases - nya_string_join with empty array (potential underflow)
  // ─────────────────────────────────────────────────────────────────────────────
  {
    NYA_ArrayᐸNYA_Stringᐳ empty_arr    = *nya_array_create(arena, NYA_String);
    NYA_String      joined_empty = *nya_string_join(arena, &empty_arr, ",");
    nya_assert(nya_string_is_empty(&joined_empty) == true);
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: edge cases - nya_string_join with single element (no separator needed)
  // ─────────────────────────────────────────────────────────────────────────────
  {
    NYA_ArrayᐸNYA_Stringᐳ single_arr = *nya_array_create(arena, NYA_String);
    NYA_String      elem       = *nya_string_from(arena, "only");
    nya_array_push_back(&single_arr, elem);
    NYA_String joined_single = *nya_string_join(arena, &single_arr, ",");
    nya_assert(nya_string_equals(&joined_single, "only") == true);
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: edge cases - nya_string_split with empty string
  // ─────────────────────────────────────────────────────────────────────────────
  {
    NYA_String      empty_split  = *nya_string_from(arena, "");
    NYA_ArrayᐸNYA_Stringᐳ split_result = *nya_string_split(arena, &empty_split, ",");
    nya_assert(split_result.length == 0);
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: edge cases - nya_string_split separator at boundaries
  // ─────────────────────────────────────────────────────────────────────────────
  {
    // separator at start
    NYA_String      sep_start   = *nya_string_from(arena, ",a,b");
    NYA_ArrayᐸNYA_Stringᐳ parts_start = *nya_string_split(arena, &sep_start, ",");
    nya_assert(parts_start.length == 3);
    nya_assert(nya_string_is_empty(&parts_start.items[0]) == true);
    nya_assert(nya_string_equals(&parts_start.items[1], "a") == true);
    nya_assert(nya_string_equals(&parts_start.items[2], "b") == true);

    // separator at end
    NYA_String      sep_end   = *nya_string_from(arena, "a,b,");
    NYA_ArrayᐸNYA_Stringᐳ parts_end = *nya_string_split(arena, &sep_end, ",");
    nya_assert(parts_end.length == 3);
    nya_assert(nya_string_equals(&parts_end.items[0], "a") == true);
    nya_assert(nya_string_equals(&parts_end.items[1], "b") == true);
    nya_assert(nya_string_is_empty(&parts_end.items[2]) == true);

    // consecutive separators
    NYA_String      consec_sep   = *nya_string_from(arena, "a,,b");
    NYA_ArrayᐸNYA_Stringᐳ parts_consec = *nya_string_split(arena, &consec_sep, ",");
    nya_assert(parts_consec.length == 3);
    nya_assert(nya_string_equals(&parts_consec.items[0], "a") == true);
    nya_assert(nya_string_is_empty(&parts_consec.items[1]) == true);
    nya_assert(nya_string_equals(&parts_consec.items[2], "b") == true);
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: edge cases - nya_string_split with multi-char separator at boundary
  // ─────────────────────────────────────────────────────────────────────────────
  {
    // multi-char separator near end (potential buffer over-read)
    NYA_String      near_end   = *nya_string_from(arena, "abc::d");
    NYA_ArrayᐸNYA_Stringᐳ parts_near = *nya_string_split(arena, &near_end, "::");
    nya_assert(parts_near.length == 2);
    nya_assert(nya_string_equals(&parts_near.items[0], "abc") == true);
    nya_assert(nya_string_equals(&parts_near.items[1], "d") == true);

    // string shorter than separator
    NYA_String      short_str   = *nya_string_from(arena, "a");
    NYA_ArrayᐸNYA_Stringᐳ parts_short = *nya_string_split(arena, &short_str, "::");
    nya_assert(parts_short.length == 1);
    nya_assert(nya_string_equals(&parts_short.items[0], "a") == true);
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: edge cases - nya_string_remove match at start (potential underflow)
  // ─────────────────────────────────────────────────────────────────────────────
  {
    NYA_String remove_start = *nya_string_from(arena, "XXXhello");
    nya_string_remove(&remove_start, "XXX");
    nya_assert(nya_string_equals(&remove_start, "hello") == true);

    // multiple matches at start
    NYA_String remove_multi_start = *nya_string_from(arena, "aaabbb");
    nya_string_remove(&remove_multi_start, "a");
    nya_assert(nya_string_equals(&remove_multi_start, "bbb") == true);
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: edge cases - nya_string_remove entire string
  // ─────────────────────────────────────────────────────────────────────────────
  {
    NYA_String remove_all = *nya_string_from(arena, "xxx");
    nya_string_remove(&remove_all, "xxx");
    nya_assert(nya_string_is_empty(&remove_all) == true);
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: edge cases - nya_string_replace at boundaries
  // ─────────────────────────────────────────────────────────────────────────────
  {
    // replace at start
    NYA_String replace_start = *nya_string_from(arena, "XXXhello");
    nya_string_replace(&replace_start, "XXX", "Y");
    nya_assert(nya_string_equals(&replace_start, "Yhello") == true);

    // replace at end
    NYA_String replace_end = *nya_string_from(arena, "helloXXX");
    nya_string_replace(&replace_end, "XXX", "Y");
    nya_assert(nya_string_equals(&replace_end, "helloY") == true);

    // replace with empty string (effectively remove)
    NYA_String replace_empty = *nya_string_from(arena, "helloXXXworld");
    nya_string_replace(&replace_empty, "XXX", "");
    nya_assert(nya_string_equals(&replace_empty, "helloworld") == true);
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: edge cases - nya_string_count with overlapping patterns
  // ─────────────────────────────────────────────────────────────────────────────
  {
    // "aaa" contains "aa" twice if overlapping, but we expect non-overlapping count
    NYA_String overlap = *nya_string_from(arena, "aaaa");
    nya_assert(nya_string_count(&overlap, "aa") == 2); // non-overlapping: aa|aa
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: edge cases - nya_string_strip operations on exact match
  // ─────────────────────────────────────────────────────────────────────────────
  {
    NYA_String strip_exact = *nya_string_from(arena, "prefix");
    nya_string_strip_prefix(&strip_exact, "prefix");
    nya_assert(nya_string_is_empty(&strip_exact) == true);

    NYA_String strip_exact2 = *nya_string_from(arena, "suffix");
    nya_string_strip_suffix(&strip_exact2, "suffix");
    nya_assert(nya_string_is_empty(&strip_exact2) == true);
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: edge cases - empty string operations
  // ─────────────────────────────────────────────────────────────────────────────
  {
    NYA_String empty_op = *nya_string_from(arena, "");

    // operations on empty string should not crash
    nya_assert(nya_string_contains(&empty_op, "x") == false);
    nya_assert(nya_string_starts_with(&empty_op, "x") == false);
    nya_assert(nya_string_ends_with(&empty_op, "x") == false);
    nya_assert(nya_string_count(&empty_op, "x") == 0);

    nya_string_to_lower(&empty_op);
    nya_assert(nya_string_is_empty(&empty_op) == true);

    nya_string_to_upper(&empty_op);
    nya_assert(nya_string_is_empty(&empty_op) == true);

    nya_string_trim_whitespace(&empty_op);
    nya_assert(nya_string_is_empty(&empty_op) == true);

    nya_string_reverse(&empty_op);
    nya_assert(nya_string_is_empty(&empty_op) == true);
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // CLEANUP
  // ─────────────────────────────────────────────────────────────────────────────
  nya_arena_destroy(arena);

  return 0;
}
