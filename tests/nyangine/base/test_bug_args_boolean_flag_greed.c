/**
 * Regression test for a boolean flag swallowing the positional after it (base_args.c).
 * */
#include "nyangine/nyangine.c"
#include "nyangine/nyangine.h"

/** A parser shaped like `./build check [--strict] <sources...>`, which is where this was found. */
typedef struct {
  NYA_ArgParameter sources;
  NYA_ArgParameter strict;
  NYA_ArgParameter jobs;
  NYA_ArgCommand   root;
  NYA_ArgParser    parser;
} Fixture;

static void fixture_init(Fixture* fixture) {
  *fixture = (Fixture){
    .sources = {
      .kind       = NYA_ARG_PARAMETER_KIND_POSITIONAL,
      .variadic   = true,
      .value.type = NYA_TYPE_STRING,
      .name       = "sources",
    },
    .strict = {
      .kind       = NYA_ARG_PARAMETER_KIND_FLAG,
      .value.type = NYA_TYPE_B8,
      .name       = "strict",
    },
    // With a default, so it is optional. A typed flag without one is required, and the cases below
    // that leave it off are about the boolean beside it rather than about this.
    .jobs = {
      .kind          = NYA_ARG_PARAMETER_KIND_FLAG,
      .value.type    = NYA_TYPE_S64,
      .name          = "jobs",
      .default_value = { .type = NYA_TYPE_S64, .as_s64 = 1 },
    },
  };

  fixture->root = (NYA_ArgCommand){
    .is_root    = true,
    .parameters = { &fixture->sources, &fixture->strict, &fixture->jobs },
    .handler    = (void*)1,
  };

  fixture->parser = (NYA_ArgParser){ .name = "test", .version = "0", .root_command = &fixture->root };
}

s32 main(void) {
  setvbuf(stdout, nullptr, _IONBF, 0);

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: a boolean flag does not eat the positional after it
  // ─────────────────────────────────────────────────────────────────────────────
  printf("TEST: --strict followed by a filename\n");
  {
    Fixture fixture;
    fixture_init(&fixture);

    NYA_CString argv[] = { "test", "--strict", "src/main.c" };

    NYA_ArgCommand* command = nullptr;
    NYA_Error       error   = nya_args_parse(&fixture.parser, 3, argv, &command);

    nya_assert(error.ok, "the filename is a positional, not the flag's value: %s", (NYA_ConstCString)error.message);
    nya_assert(fixture.strict.value.as_b8, "a bare boolean flag is true");
    nya_assert(fixture.sources.values_count == 1, "the filename reached the positional");
    nya_assert(nya_string_equals(fixture.sources.values[0].as_string, "src/main.c"));
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: the silent case: a positional that looks like a boolean
  // ─────────────────────────────────────────────────────────────────────────────
  printf("TEST: --strict followed by a positional that parses as a boolean\n");
  {
    /*
     * The dangerous half: consuming "0" would leave `strict` false and the source list empty, so the
     * command would succeed while checking nothing.
     */
    Fixture fixture;
    fixture_init(&fixture);

    NYA_CString argv[] = { "test", "--strict", "0" };

    NYA_ArgCommand* command = nullptr;
    NYA_Error       error   = nya_args_parse(&fixture.parser, 3, argv, &command);

    nya_assert(error.ok);
    nya_assert(fixture.strict.value.as_b8, "the flag is true; '0' is a positional, not its value");
    nya_assert(fixture.sources.values_count == 1, "and '0' was not swallowed");
    nya_assert(nya_string_equals(fixture.sources.values[0].as_string, "0"));
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: --flag=value is the explicit spelling, in both directions
  // ─────────────────────────────────────────────────────────────────────────────
  printf("TEST: --strict=false and --strict=true\n");
  {
    Fixture fixture;
    fixture_init(&fixture);

    NYA_CString argv[] = { "test", "--strict=false", "a.c" };

    NYA_ArgCommand* command = nullptr;
    NYA_Error       error   = nya_args_parse(&fixture.parser, 3, argv, &command);

    nya_assert(error.ok, "%s", (NYA_ConstCString)error.message);
    nya_assert(!fixture.strict.value.as_b8, "an attached value is honoured, so a flag can be turned off");
    nya_assert(fixture.strict.was_matched, "and it still counts as given");
    nya_assert(fixture.sources.values_count == 1);
  }
  {
    Fixture fixture;
    fixture_init(&fixture);

    NYA_CString argv[] = { "test", "--strict=true" };

    NYA_ArgCommand* command = nullptr;
    NYA_Error       error   = nya_args_parse(&fixture.parser, 2, argv, &command);

    nya_assert(error.ok);
    nya_assert(fixture.strict.value.as_b8);
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: a nonsense attached value is refused rather than ignored
  // ─────────────────────────────────────────────────────────────────────────────
  printf("TEST: --strict=maybe\n");
  {
    Fixture fixture;
    fixture_init(&fixture);

    NYA_CString argv[] = { "test", "--strict=maybe" };

    NYA_ArgCommand* command = nullptr;
    NYA_Error       error   = nya_args_parse(&fixture.parser, 2, argv, &command);

    // attached means the user meant it as the value, so a nonsense value is an error, unlike a following
    // token.
    nya_assert(!error.ok, "an explicit value that is not a boolean is rejected");
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: non-boolean flags still take a following value, and now an attached one too
  // ─────────────────────────────────────────────────────────────────────────────
  printf("TEST: --jobs 8 and --jobs=8\n");
  {
    Fixture fixture;
    fixture_init(&fixture);

    NYA_CString argv[] = { "test", "--jobs", "8", "a.c" };

    NYA_ArgCommand* command = nullptr;
    NYA_Error       error   = nya_args_parse(&fixture.parser, 4, argv, &command);

    nya_assert(error.ok, "%s", (NYA_ConstCString)error.message);
    nya_assert(fixture.jobs.value.as_s64 == 8, "a typed flag still consumes the token after it");
    nya_assert(fixture.sources.values_count == 1, "and the positional after that is still a positional");
  }
  {
    Fixture fixture;
    fixture_init(&fixture);

    NYA_CString argv[] = { "test", "--jobs=8", "a.c", "b.c" };

    NYA_ArgCommand* command = nullptr;
    NYA_Error       error   = nya_args_parse(&fixture.parser, 4, argv, &command);

    nya_assert(error.ok, "%s", (NYA_ConstCString)error.message);
    nya_assert(fixture.jobs.value.as_s64 == 8);
    nya_assert(fixture.sources.values_count == 2, "both positionals survive");
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: a typed flag with nothing after it is still an error
  // ─────────────────────────────────────────────────────────────────────────────
  printf("TEST: --jobs at the end of the line\n");
  {
    Fixture fixture;
    fixture_init(&fixture);

    NYA_CString argv[] = { "test", "--jobs" };

    NYA_ArgCommand* command = nullptr;
    NYA_Error       error   = nya_args_parse(&fixture.parser, 2, argv, &command);

    nya_assert(!error.ok, "a flag that needs a value and has none is refused");
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: an unknown flag name is still unknown once the value is split off
  // ─────────────────────────────────────────────────────────────────────────────
  printf("TEST: --nonsense=1\n");
  {
    Fixture fixture;
    fixture_init(&fixture);

    NYA_CString argv[] = { "test", "--nonsense=1" };

    NYA_ArgCommand* command = nullptr;
    NYA_Error       error   = nya_args_parse(&fixture.parser, 2, argv, &command);

    // The name has to be matched against `nonsense`, not against `nonsense=1`, or every attached
    // value would produce "unknown flag" for a flag that exists.
    nya_assert(!error.ok, "an unknown flag is still rejected");
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: an empty attached value, and one containing an '='
  // ─────────────────────────────────────────────────────────────────────────────
  printf("TEST: --jobs= and a value with an '=' in it\n");
  {
    Fixture fixture;
    fixture_init(&fixture);

    NYA_CString argv[] = { "test", "--jobs=" };

    NYA_ArgCommand* command = nullptr;
    NYA_Error       error   = nya_args_parse(&fixture.parser, 2, argv, &command);

    // empty is a value the user wrote, so it is refused rather than treated as absent. Falling back to
    // the next token would be greedy again.
    nya_assert(!error.ok, "an empty attached value is not a number");
  }
  {
    Fixture fixture;
    fixture_init(&fixture);

    // Only the first '=' separates. A path or a key=value string as a flag's value must survive.
    NYA_ArgParameter tag = {
      .kind          = NYA_ARG_PARAMETER_KIND_FLAG,
      .value.type    = NYA_TYPE_STRING,
      .name          = "tag",
      .default_value = { .type = NYA_TYPE_STRING, .as_string = "" },
    };

    fixture.root.parameters[3] = &tag;

    NYA_CString argv[] = { "test", "--tag=a=b=c" };

    NYA_ArgCommand* command = nullptr;
    NYA_Error       error   = nya_args_parse(&fixture.parser, 2, argv, &command);

    nya_assert(error.ok, "%s", (NYA_ConstCString)error.message);
    nya_assert(nya_string_equals(tag.value.as_string, "a=b=c"), "only the first '=' splits");
  }

  printf("PASSED: test_bug_args_boolean_flag_greed (0 failures)\n");

  return EXIT_SUCCESS;
}
