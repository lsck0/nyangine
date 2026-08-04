/**
 * Path manipulation. Purely textual: nothing here touches the filesystem.
 *
 * Every expectation below is taken from the contract documented in base_path.h rather than from
 * whatever the implementation happens to do, so a behaviour change has to be a deliberate edit here
 * too.
 **/

#include "nyangine/nyangine.c"
#include "nyangine/nyangine.h"

/** Shorthand: run `fn(arena, input)` and compare the resulting string to `expected`. */
#define ASSERT_PATH(fn, input, expected)                                                                                                             \
  do {                                                                                                                                              \
    NYA_String* _result = fn(arena, (input));                                                                                                        \
    nya_assert(                                                                                                                                     \
        nya_string_equals(_result, (expected)),                                                                                                      \
        #fn "(\"%s\") gave \"" NYA_FMT_STRING "\", expected \"%s\"",                                                                                 \
        (input),                                                                                                                                     \
        NYA_FMT_STRING_ARG(_result),                                                                                                                 \
        (expected)                                                                                                                                   \
    );                                                                                                                                              \
  } while (0)

s32 main(void) {
  NYA_Arena* arena = nya_arena_create(.name = "test_path");

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: nya_path_join - exactly one separator, however many the inputs had
  // ─────────────────────────────────────────────────────────────────────────────
  printf("TEST: nya_path_join\n");
  {
    nya_assert(nya_string_equals(nya_path_join(arena, "a", "b"), "a/b"));
    nya_assert(nya_string_equals(nya_path_join(arena, "a/", "b"), "a/b"));
    nya_assert(nya_string_equals(nya_path_join(arena, "a", "/b"), "/b"));     // absolute tail wins, and stays absolute
    nya_assert(nya_string_equals(nya_path_join(arena, "a///", "///b"), "/b")); // ditto: the tail is absolute
    nya_assert(nya_string_equals(nya_path_join(arena, "a/b", "c/d"), "a/b/c/d"));

    // An absolute tail replaces the head outright, rather than producing "/home/user/etc".
    nya_assert(nya_string_equals(nya_path_join(arena, "/home/user", "/etc"), "/etc"));

    // Empty operands: joining with nothing yields the other side, normalised.
    nya_assert(nya_string_equals(nya_path_join(arena, "", "b"), "b"));
    nya_assert(nya_string_equals(nya_path_join(arena, "a", ""), "a"));
    nya_assert(nya_string_equals(nya_path_join(arena, "", ""), "."));

    // Join normalises, so "." and ".." collapse on the way through.
    nya_assert(nya_string_equals(nya_path_join(arena, "a/./b", "../c"), "a/c"));
    printf("  PASSED\n");
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: nya_path_basename - everything after the last separator
  // ─────────────────────────────────────────────────────────────────────────────
  printf("TEST: nya_path_basename\n");
  {
    ASSERT_PATH(nya_path_basename, "a/b/c.txt", "c.txt");
    ASSERT_PATH(nya_path_basename, "c.txt", "c.txt");
    ASSERT_PATH(nya_path_basename, "/a/b/c", "c");
    ASSERT_PATH(nya_path_basename, "a/b/", "b");   // trailing separator is not part of the name
    // Root has no name after its separator. Same answer Python gives; the header does not
    // specify this case, so this pins it rather than asserting a preference.
    ASSERT_PATH(nya_path_basename, "/", "");
    printf("  PASSED\n");
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: nya_path_dirname - everything before the last separator
  // ─────────────────────────────────────────────────────────────────────────────
  printf("TEST: nya_path_dirname\n");
  {
    ASSERT_PATH(nya_path_dirname, "a/b/c.txt", "a/b");
    ASSERT_PATH(nya_path_dirname, "c.txt", ".");   // no separator at all
    ASSERT_PATH(nya_path_dirname, "/a", "/");      // the root keeps its separator
    ASSERT_PATH(nya_path_dirname, "a/b/", "a");
    printf("  PASSED\n");
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: nya_path_extension - including the dot, and hidden files have none
  // ─────────────────────────────────────────────────────────────────────────────
  printf("TEST: nya_path_extension\n");
  {
    ASSERT_PATH(nya_path_extension, "a/b/c.txt", ".txt");
    ASSERT_PATH(nya_path_extension, "a/b/c.tar.gz", ".gz");   // last extension only
    ASSERT_PATH(nya_path_extension, "a/b/c", "");
    ASSERT_PATH(nya_path_extension, ".bashrc", "");           // hidden file, not an extension
    ASSERT_PATH(nya_path_extension, "a/b/.config", "");
    ASSERT_PATH(nya_path_extension, "archive.", ".");         // a trailing dot is an empty extension
    printf("  PASSED\n");
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: nya_path_stem - basename without the extension
  // ─────────────────────────────────────────────────────────────────────────────
  printf("TEST: nya_path_stem\n");
  {
    ASSERT_PATH(nya_path_stem, "a/b/c.txt", "c");
    ASSERT_PATH(nya_path_stem, "a/b/c.tar.gz", "c.tar");
    ASSERT_PATH(nya_path_stem, "a/b/c", "c");
    ASSERT_PATH(nya_path_stem, ".bashrc", ".bashrc");   // no extension, so nothing is removed
    printf("  PASSED\n");
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: nya_path_with_extension - replaces, or adds when absent
  // ─────────────────────────────────────────────────────────────────────────────
  printf("TEST: nya_path_with_extension\n");
  {
    nya_assert(nya_string_equals(nya_path_with_extension(arena, "a/b/c.txt", "md"), "a/b/c.md"));
    nya_assert(nya_string_equals(nya_path_with_extension(arena, "a/b/c.txt", ".md"), "a/b/c.md"));  // dot optional
    nya_assert(nya_string_equals(nya_path_with_extension(arena, "a/b/c", "md"), "a/b/c.md"));       // added
    nya_assert(nya_string_equals(nya_path_with_extension(arena, "a/b/c.txt", ""), "a/b/c"));        // removed
    printf("  PASSED\n");
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: nya_path_is_absolute
  // ─────────────────────────────────────────────────────────────────────────────
  printf("TEST: nya_path_is_absolute\n");
  {
    nya_assert(nya_path_is_absolute("/etc/hosts") == true);
    nya_assert(nya_path_is_absolute("/") == true);
    nya_assert(nya_path_is_absolute("etc/hosts") == false);
    nya_assert(nya_path_is_absolute("./etc") == false);
    nya_assert(nya_path_is_absolute("../etc") == false);
    nya_assert(nya_path_is_absolute("") == false);

    // Windows shapes are recognised on every host, since paths travel between them.
    nya_assert(nya_path_is_absolute("C:\\Windows") == true);
    nya_assert(nya_path_is_absolute("C:/Windows") == true);
    nya_assert(nya_path_is_absolute("C:") == false);   // relative to that drive's cwd
    printf("  PASSED\n");
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: nya_path_normalize - collapse separators, resolve . and .., convert backslashes
  // ─────────────────────────────────────────────────────────────────────────────
  printf("TEST: nya_path_normalize\n");
  {
    ASSERT_PATH(nya_path_normalize, "a//b///c", "a/b/c");
    ASSERT_PATH(nya_path_normalize, "a/./b", "a/b");
    ASSERT_PATH(nya_path_normalize, "a/b/../c", "a/c");
    ASSERT_PATH(nya_path_normalize, "a/b/../../c", "c");
    ASSERT_PATH(nya_path_normalize, "a\\b\\c", "a/b/c");
    ASSERT_PATH(nya_path_normalize, "/a/b/../c", "/a/c");
    ASSERT_PATH(nya_path_normalize, "./a", "a");

    // Everything cancelling out is the current directory, not the empty string.
    ASSERT_PATH(nya_path_normalize, "a/..", ".");
    ASSERT_PATH(nya_path_normalize, ".", ".");
    ASSERT_PATH(nya_path_normalize, "", ".");

    // A ".." with nothing above it: kept when relative, dropped when absolute.
    ASSERT_PATH(nya_path_normalize, "../a", "../a");
    ASSERT_PATH(nya_path_normalize, "../../a", "../../a");
    ASSERT_PATH(nya_path_normalize, "/../a", "/a");

    // A drive prefix is carried through and is not something ".." may remove.
    ASSERT_PATH(nya_path_normalize, "C:\\a\\..\\b", "C:/b");
    printf("  PASSED\n");
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: normalize is idempotent
  //
  // Worth pinning on its own: these results are baked into generated source by the asset pipeline,
  // so a path that changes when normalised twice would make a build non-reproducible.
  // ─────────────────────────────────────────────────────────────────────────────
  printf("TEST: normalize is idempotent\n");
  {
    NYA_ConstCString inputs[] = { "a//b/../c", "/x/./y", "..", "C:\\a\\b", "a/b/", "", "." };

    for (u64 i = 0; i < sizeof(inputs) / sizeof(inputs[0]); i++) {
      NYA_String* once  = nya_path_normalize(arena, inputs[i]);
      NYA_String* twice = nya_path_normalize(arena, nya_string_to_cstring(arena, once));
      nya_assert(
          nya_string_equals(once, twice),
          "normalize(\"%s\") is not idempotent: \"" NYA_FMT_STRING "\" then \"" NYA_FMT_STRING "\"",
          inputs[i],
          NYA_FMT_STRING_ARG(once),
          NYA_FMT_STRING_ARG(twice)
      );
    }
    printf("  PASSED\n");
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: long paths
  //
  // The scratch arena inside nya_path_normalize is sized from the input length. A path longer than
  // its minimum region has to chain another region rather than overrun the segment table, which is
  // the failure mode that sizing introduced.
  // ─────────────────────────────────────────────────────────────────────────────
  printf("TEST: long paths\n");
  {
    NYA_String* built = nya_string_create(arena);
    for (u32 i = 0; i < 512; i++) nya_string_extend(built, "segment/");
    nya_string_extend(built, "leaf.txt");

    NYA_CString long_path = nya_string_to_cstring(arena, built);
    NYA_String* result    = nya_path_normalize(arena, long_path);

    nya_assert(nya_string_ends_with(result, "segment/leaf.txt"));
    nya_assert(nya_string_equals(nya_path_basename(arena, long_path), "leaf.txt"));
    nya_assert(nya_string_equals(nya_path_extension(arena, long_path), ".txt"));

    // 512 "segment" components and no separators collapsed away, so nothing was truncated.
    nya_assert(nya_string_count(result, "segment") == 512);
    printf("  PASSED\n");
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // CLEANUP
  // ─────────────────────────────────────────────────────────────────────────────
  nya_arena_destroy(arena);

  printf("PASSED: test_path\n");
  return 0;
}
