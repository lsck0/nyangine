/**
 * The lambda pass, against the functions it made out of the bodies in this file.
 *
 * Every body below is written at the call site and hoisted into the companion header included at the
 * top, so the file compiling at all is half of what is under test; the other half is that the callbacks
 * really run, with the arguments their caller passes and the results their caller reads.
 *
 * What cannot be tested from here is the safety property, because it is a compile error: a body naming
 * a local of the function it was written in does not build, since the function it became is at file
 * scope. See base_lambda.h.
 **/

#include "nyangine/nyangine.h"

#include "nyangine/nyangine.c"

/** What the walk lambda writes into through the `void*` it is handed. */
typedef struct {
  u32 entries;
  b8  found_itself;
} WalkTally;

typedef u32 (*CombineFn)(u32 a, u32 b);

// The bodies below, hoisted out to here before anything compiled, which is why WalkTally is declared
// above it: a body sees what is in scope at this line and nothing further down. See base_lambda.h.
#include "generated/lambdas/tests_nyangine_base_test_lambda_generated_c.h"

s32 main(void) {
  setvbuf(stdout, nullptr, _IONBF, 0);

  NYA_Arena* arena = nya_arena_create(.name = "test_lambda_generated");
  defer      nya_arena_destroy(arena);

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: a comparator written where it is handed over, and really used
  // ─────────────────────────────────────────────────────────────────────────────
  printf("TEST: comparator\n");
  {
    NYA_Arrayᐸu32ᐳ* numbers = nya_array_create(arena, u32);

    const u32 unsorted[] = { 3, 1, 4, 1, 5, 9, 2, 6 };
    for (u32 i = 0; i < nya_carray_length(unsorted); i++) nya_array_push_back(numbers, unsorted[i]);

    // Descending, which no default could have produced, so the order afterwards is this body running.
    nya_array_sort(numbers, nya_lambda(test_lambda_descending, s32, (const u32* a, const u32* b), {
                     if (*a == *b) return 0;
                     return *a > *b ? -1 : 1;
                   }));

    for (u64 i = 1; i < numbers->length; i++) {
      nya_assert(numbers->items[i - 1] >= numbers->items[i], "the sort did not run the lambda: %u before %u", numbers->items[i - 1],
                 numbers->items[i]);
    }

    nya_assert(numbers->items[0] == 9 && numbers->items[numbers->length - 1] == 1);

    printf("  PASSED\n");
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: an engine API calls one, with the arguments it promises
  // ─────────────────────────────────────────────────────────────────────────────
  printf("TEST: walk callback\n");
  {
    WalkTally tally = { 0 };

    NYA_EXPECT(nya_filesystem_walk(arena, "./tests/nyangine/base",
                                   nya_lambda(test_lambda_walk, b8, (NYA_ConstCString path, const NYA_DirectoryEntry* entry, void* user_data), {
                                     WalkTally* seen = user_data;

                                     if (entry->type != NYA_FILE_TYPE_FILE) return true;

                                     seen->entries++;
                                     seen->found_itself = seen->found_itself || nya_string_contains(path, "test_lambda_generated.c");

                                     return true;
                                   }),
                                   &tally));

    // The file this is written in is in that directory, so the walk cannot have missed it unless the
    // callback never ran or was handed something other than a path.
    nya_assert(tally.entries > 0, "the walk called the lambda for nothing");
    nya_assert(tally.found_itself, "the lambda saw %u files and not the one it is written in", tally.entries);

    printf("  PASSED\n");
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: two tags are two functions, each returning its own answer
  // ─────────────────────────────────────────────────────────────────────────────
  printf("TEST: two of them\n");
  {
    CombineFn concatenate = nya_lambda(test_lambda_concatenate, u32, (u32 a, u32 b), { return (a * 10) + b; });
    CombineFn multiply    = nya_lambda(test_lambda_multiply, u32, (u32 a, u32 b), { return a * b; });

    nya_assert(concatenate(4, 2) == 42);
    nya_assert(multiply(4, 2) == 8);
    nya_assert((void*)concatenate != (void*)multiply, "two tags produced one function");

    printf("  PASSED\n");
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: what the pass wrote down about this file
  // ─────────────────────────────────────────────────────────────────────────────
  printf("TEST: the generated tree\n");
  {
    nya_assert(nya_filesystem_exists("./src/generated/lambdas/tests_nyangine_base_test_lambda_generated_c.h"),
               "the companion this file includes is not in the tree");

    NYA_String* manifest = nya_string_create(arena);
    NYA_EXPECT(nya_file_read("./src/generated/lambdas/manifest.txt", manifest));

    NYA_CString text = nya_string_to_cstring(arena, manifest);

    // Every tag, against the file it was written in: the manifest is what says a tag is one name
    // across the whole tree, so a tag missing from it is a tag nothing would have caught twice.
    const NYA_ConstCString tags[] = { "test_lambda_descending", "test_lambda_walk", "test_lambda_concatenate", "test_lambda_multiply" };

    for (u32 i = 0; i < nya_carray_length(tags); i++) {
      NYA_String* line = nya_string_sprintf(arena, "%s\ttests/nyangine/base/test_lambda_generated.c:", tags[i]);
      nya_assert(nya_string_contains(text, nya_string_to_cstring(arena, line)), "the manifest does not record '%s' against this file", tags[i]);
    }

    printf("  PASSED\n");
  }

  printf("PASSED: test_lambda_generated\n");

  return 0;
}
