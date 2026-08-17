/**
 * Regression test for the unbounded variadic parameter in nya_args_parse (base_args.c).
 *
 * A variadic positional collects into `NYA_ArgParameter.values`, which is a fixed
 * `NYA_Value values[NYA_ARG_MAX_PARAMETERS]` — 256 entries. The collecting loop wrote
 *
 *     NYA_Value* slot = &param->values[param->values_count];
 *     ...
 *     param->values_count++;
 *
 * with nothing comparing values_count against that bound, so argument 257 and everything after it
 * landed past the end of the array. NYA_Value is a large struct, so this walks a long way into
 * whatever follows the parameter.
 *
 * The input is argv, which makes it reachable from the command line of anything using this parser —
 * including the build tool's own `./build run test <names...>`, whose test-name parameter is exactly
 * this shape.
 * */
#include "nyangine/nyangine.c"
#include "nyangine/nyangine.h"

/** Comfortably past the 256 the array holds, so the overflow is unambiguous rather than marginal. */
#define ARGUMENT_COUNT 400

s32 main(void) {
  setvbuf(stdout, nullptr, _IONBF, 0);

  NYA_Arena* arena = nya_arena_create(.name = "test_bug_args_variadic_overflow");

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: more variadic arguments than the array holds
  // ─────────────────────────────────────────────────────────────────────────────
  printf("TEST: %d variadic arguments into a %d slot array\n", ARGUMENT_COUNT, NYA_ARG_MAX_PARAMETERS);
  {
    NYA_ArgParameter files = {
      .kind       = NYA_ARG_PARAMETER_KIND_POSITIONAL,
      .variadic   = true,
      .value.type = NYA_TYPE_STRING,
      .name       = "files",
    };

    NYA_ArgCommand root = {
      .is_root    = true,
      .parameters = { &files },
      .handler    = (void*)1,
    };

    NYA_ArgParser parser = {
      .name         = "overflow",
      .root_command = &root,
    };

    // argv[0] is the program name, so ARGUMENT_COUNT values follow it.
    NYA_CString* argv = nya_arena_alloc(arena, (ARGUMENT_COUNT + 1) * sizeof(NYA_CString));
    argv[0]           = "overflow";
    for (u32 i = 0; i < ARGUMENT_COUNT; i++) {
      NYA_String* name = nya_string_sprintf(arena, "file%u.txt", i);
      argv[i + 1]      = nya_string_to_cstring(arena, name);
    }

    NYA_ArgCommand* command = nullptr;
    NYA_Error       error   = nya_args_parse(&parser, ARGUMENT_COUNT + 1, argv, &command);

    // Either answer is defensible — refuse the input, or take what fits and say so — but writing
    // past the array is not one of them. What this pins is that values_count never exceeds the
    // array, whichever way the overflow is resolved.
    nya_assert(
      files.values_count <= NYA_ARG_MAX_PARAMETERS,
      "values_count reached %u, past the %d the array holds",
      files.values_count,
      NYA_ARG_MAX_PARAMETERS
    );

    if (error.ok) {
      printf("  accepted, collected %u of %d\n", files.values_count, ARGUMENT_COUNT);
    } else {
      printf("  rejected, collected %u of %d\n", files.values_count, ARGUMENT_COUNT);
    }

    // Whatever was collected has to be intact, not partially written over.
    for (u32 i = 0; i < files.values_count; i++) {
      NYA_String* expected = nya_string_sprintf(arena, "file%u.txt", i);
      nya_assert(files.values[i].as_string != nullptr, "value %u is null", i);
      nya_assert(
        nya_string_equals(expected, files.values[i].as_string),
        "value %u is '%s', expected '" NYA_FMT_STRING "'",
        i,
        files.values[i].as_string,
        NYA_FMT_STRING_ARG(expected)
      );
    }
  }
  printf("  PASSED\n");

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: exactly the array's worth still works
  // ─────────────────────────────────────────────────────────────────────────────
  printf("TEST: exactly %d arguments\n", NYA_ARG_MAX_PARAMETERS);
  {
    NYA_ArgParameter files = {
      .kind       = NYA_ARG_PARAMETER_KIND_POSITIONAL,
      .variadic   = true,
      .value.type = NYA_TYPE_STRING,
      .name       = "files",
    };

    NYA_ArgCommand root = {
      .is_root    = true,
      .parameters = { &files },
      .handler    = (void*)1,
    };

    NYA_ArgParser parser = { .name = "exact", .root_command = &root };

    NYA_CString* argv = nya_arena_alloc(arena, (NYA_ARG_MAX_PARAMETERS + 1) * sizeof(NYA_CString));
    argv[0]           = "exact";
    for (u32 i = 0; i < NYA_ARG_MAX_PARAMETERS; i++) {
      NYA_String* name = nya_string_sprintf(arena, "f%u", i);
      argv[i + 1]      = nya_string_to_cstring(arena, name);
    }

    NYA_ArgCommand* command = nullptr;
    NYA_Error       error   = nya_args_parse(&parser, NYA_ARG_MAX_PARAMETERS + 1, argv, &command);

    nya_assert(error.ok, "a full but not overfull argument list was rejected");
    nya_assert(files.values_count == NYA_ARG_MAX_PARAMETERS, "collected %u of %d", files.values_count, NYA_ARG_MAX_PARAMETERS);
  }
  printf("  PASSED\n");

  nya_arena_destroy(arena);

  printf("PASSED: test_bug_args_variadic_overflow\n");
  return 0;
}
