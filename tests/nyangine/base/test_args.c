/**
 * THIS FILE WAS CLANKER WANKED !!!
 **/

#include "nyangine/nyangine.c"
#include "nyangine/nyangine.h"

#define assert_contains(haystack, needle)                                                                                                            \
  nya_assert(strstr((haystack), (needle)) != nullptr, "expected to find '%s' in:\n%s", (needle), (haystack))

#define assert_not_contains(haystack, needle)                                                                                                        \
  nya_assert(strstr((haystack), (needle)) == nullptr, "expected NOT to find '%s' in:\n%s", (needle), (haystack))

/** Generates a completion script into `buffer`, which is the whole point of the stream taking variant. */
NYA_INTERNAL void capture_completions(NYA_ArgParser* parser, NYA_ConstCString binary, NYA_ConstCString shell, char* buffer, u64 buffer_size) {
  FILE* stream = tmpfile();
  nya_assert(stream != nullptr);

  NYA_EXPECT(nya_args_write_completions(parser, binary, shell, stream));

  (void)fflush(stream);
  rewind(stream);

  u64 read     = fread(buffer, 1, buffer_size - 1, stream);
  buffer[read] = '\0';

  (void)fclose(stream);
}

/** Generates the action for one positional, and returns just the spec line it landed on. */
NYA_INTERNAL void capture_positional_spec(NYA_ArgParameter* param, char* buffer, u64 buffer_size) {
  NYA_ArgCommand root = {
    .is_root    = true,
    .handler    = (void*)1,
    .parameters = { param },
  };

  NYA_ArgParser parser = {
    .name         = "spec",
    .root_command = &root,
  };

  capture_completions(&parser, "spec", "zsh", buffer, buffer_size);
}

typedef struct WalkRecord {
  u32              count;
  NYA_ConstCString names[16];
  u32              path_counts[16];
  u32              flag_counts[16];
  NYA_ConstCString last_flags[16];
} WalkRecord;

NYA_INTERNAL void record_visit(const NYA_ArgCommandVisit* visit) {
  WalkRecord* record = (WalkRecord*)visit->userdata;
  nya_assert(record->count < 16);

  // The root has no name, so it is recorded under one the assertions can read.
  record->names[record->count]       = visit->command->name ? visit->command->name : "<root>";
  record->path_counts[record->count] = visit->path_count;
  record->flag_counts[record->count] = visit->flag_count;
  record->last_flags[record->count]  = visit->flag_count > 0 ? visit->flags[visit->flag_count - 1]->name : nullptr;
  record->count++;
}

NYA_INTERNAL NYA_ConstCString fruit_choices(u32 index) {
  static NYA_ConstCString fruits[] = { "apple", "banana", nullptr };
  return index < 2 ? fruits[index] : nullptr;
}

s32 main(void) {
  NYA_Arena* arena = nya_arena_create(.name = "test_args");

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: basic flag parameter
  // ─────────────────────────────────────────────────────────────────────────────
  {
    NYA_ArgParameter flag = {
      .kind        = NYA_ARG_PARAMETER_KIND_FLAG,
      .value.type  = NYA_TYPE_B8,
      .name        = "verbose",
      .description = "Enable verbose output",
    };

    NYA_ArgCommand root = {
      .is_root    = true,
      .parameters = { &flag },
    };

    NYA_ArgParser parser = {
      .name         = "test_app",
      .root_command = &root,
    };

    const char*     argv[] = { "app", "--verbose" };
    NYA_ArgCommand* cmd    = nullptr;
    NYA_EXPECT(nya_args_parse(&parser, 2, (NYA_CString*)argv, &cmd));
    nya_assert(cmd != nullptr);
    nya_assert(flag.value.as_b8 == true);
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: flag with explicit value, which is attached rather than adjacent
  // ─────────────────────────────────────────────────────────────────────────────
  {
    /*
     * This used to read `{ "app", "--debug", "true" }`, and that spelling is gone on purpose.
     *
     * Letting a boolean take the *next token* makes every positional after a flag ambiguous, and the
     * parser resolved the ambiguity in the flag's favour — so `./build check --strict src/main.c`
     * consumed the filename and failed, and `--strict 0` consumed the positional silently. A boolean
     * flag now never reaches forward, and `--debug=false` is how one is written explicitly.
     *
     * See test_bug_args_boolean_flag_greed.c for the full case.
     */
    NYA_ArgParameter flag = {
      .kind       = NYA_ARG_PARAMETER_KIND_FLAG,
      .value.type = NYA_TYPE_B8,
      .name       = "debug",
    };

    NYA_ArgCommand root = {
      .is_root    = true,
      .parameters = { &flag },
    };

    NYA_ArgParser parser = {
      .name         = "test_app",
      .root_command = &root,
    };

    const char*     argv[] = { "app", "--debug=true" };
    NYA_ArgCommand* cmd    = nullptr;
    NYA_EXPECT(nya_args_parse(&parser, 2, (NYA_CString*)argv, &cmd));
    nya_assert(cmd != nullptr);
    nya_assert(flag.value.as_b8 == true);

    const char* argv2[] = { "app", "--debug=false" };
    cmd                 = nullptr;
    NYA_EXPECT(nya_args_parse(&parser, 2, (NYA_CString*)argv2, &cmd));
    nya_assert(cmd != nullptr);
    nya_assert(flag.value.as_b8 == false);
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: integer flag parameter
  // ─────────────────────────────────────────────────────────────────────────────
  {
    NYA_ArgParameter count = {
      .kind       = NYA_ARG_PARAMETER_KIND_FLAG,
      .value.type = NYA_TYPE_S64,
      .name       = "count",
    };

    NYA_ArgCommand root = {
      .is_root    = true,
      .parameters = { &count },
    };

    NYA_ArgParser parser = {
      .name         = "test_app",
      .root_command = &root,
    };

    const char*     argv[] = { "app", "--count", "42" };
    NYA_ArgCommand* cmd    = nullptr;
    NYA_EXPECT(nya_args_parse(&parser, 3, (NYA_CString*)argv, &cmd));
    nya_assert(cmd != nullptr);
    nya_assert(count.value.as_s64 == 42);
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: float flag parameter
  // ─────────────────────────────────────────────────────────────────────────────
  {
    NYA_ArgParameter rate = {
      .kind       = NYA_ARG_PARAMETER_KIND_FLAG,
      .value.type = NYA_TYPE_F64,
      .name       = "rate",
    };

    NYA_ArgCommand root = {
      .is_root    = true,
      .parameters = { &rate },
    };

    NYA_ArgParser parser = {
      .name         = "test_app",
      .root_command = &root,
    };

    const char*     argv[] = { "app", "--rate", "3.14159" };
    NYA_ArgCommand* cmd    = nullptr;
    NYA_EXPECT(nya_args_parse(&parser, 3, (NYA_CString*)argv, &cmd));
    nya_assert(cmd != nullptr);
    nya_assert(rate.value.as_f64 > 3.14 && rate.value.as_f64 < 3.15);
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: string flag parameter
  // ─────────────────────────────────────────────────────────────────────────────
  {
    NYA_ArgParameter output = {
      .kind       = NYA_ARG_PARAMETER_KIND_FLAG,
      .value.type = NYA_TYPE_STRING,
      .name       = "output",
    };

    NYA_ArgCommand root = {
      .is_root    = true,
      .parameters = { &output },
    };

    NYA_ArgParser parser = {
      .name         = "test_app",
      .root_command = &root,
    };

    const char*     argv[] = { "app", "--output", "file.txt" };
    NYA_ArgCommand* cmd    = nullptr;
    NYA_EXPECT(nya_args_parse(&parser, 3, (NYA_CString*)argv, &cmd));
    nya_assert(cmd != nullptr);
    nya_assert(strcmp(output.value.as_string, "file.txt") == 0);
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: multiple flags
  // ─────────────────────────────────────────────────────────────────────────────
  {
    NYA_ArgParameter flag1 = {
      .kind       = NYA_ARG_PARAMETER_KIND_FLAG,
      .value.type = NYA_TYPE_B8,
      .name       = "flag1",
    };
    NYA_ArgParameter flag2 = {
      .kind       = NYA_ARG_PARAMETER_KIND_FLAG,
      .value.type = NYA_TYPE_B8,
      .name       = "flag2",
    };
    NYA_ArgParameter flag3 = {
      .kind       = NYA_ARG_PARAMETER_KIND_FLAG,
      .value.type = NYA_TYPE_B8,
      .name       = "flag3",
    };

    NYA_ArgCommand root = {
      .is_root    = true,
      .parameters = { &flag1, &flag2, &flag3 },
    };

    NYA_ArgParser parser = {
      .name         = "test_app",
      .root_command = &root,
    };

    const char*     argv[] = { "app", "--flag1", "--flag2", "--flag3" };
    NYA_ArgCommand* cmd    = nullptr;
    NYA_EXPECT(nya_args_parse(&parser, 4, (NYA_CString*)argv, &cmd));
    nya_assert(cmd != nullptr);
    nya_assert(flag1.value.as_b8 == true);
    nya_assert(flag2.value.as_b8 == true);
    nya_assert(flag3.value.as_b8 == true);
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: subcommand
  // ─────────────────────────────────────────────────────────────────────────────
  {
    NYA_ArgParameter verbose = {
      .kind       = NYA_ARG_PARAMETER_KIND_FLAG,
      .value.type = NYA_TYPE_B8,
      .name       = "verbose",
    };

    NYA_ArgCommand build = {
      .name        = "build",
      .description = "Build project",
      .parameters  = { &verbose },
    };

    NYA_ArgCommand root = {
      .is_root     = true,
      .subcommands = { &build },
    };

    NYA_ArgParser parser = {
      .name         = "test_app",
      .root_command = &root,
    };

    const char*     argv[] = { "app", "build", "--verbose" };
    NYA_ArgCommand* cmd    = nullptr;
    NYA_EXPECT(nya_args_parse(&parser, 3, (NYA_CString*)argv, &cmd));
    nya_assert(cmd != nullptr);
    nya_assert(cmd == &build);
    nya_assert(verbose.value.as_b8 == true);
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: positional parameter
  // ─────────────────────────────────────────────────────────────────────────────
  {
    NYA_ArgParameter input = {
      .kind       = NYA_ARG_PARAMETER_KIND_POSITIONAL,
      .value.type = NYA_TYPE_STRING,
      .name       = "input",
    };

    NYA_ArgCommand root = {
      .is_root    = true,
      .parameters = { &input },
      .handler    = (void*)1, // dummy handler to satisfy validation
    };

    NYA_ArgParser parser = {
      .name         = "test_app",
      .root_command = &root,
    };

    const char*     argv[] = { "app", "input.txt" };
    NYA_ArgCommand* cmd    = nullptr;
    NYA_EXPECT(nya_args_parse(&parser, 2, (NYA_CString*)argv, &cmd));
    nya_assert(cmd != nullptr);
    nya_assert(strcmp(input.value.as_string, "input.txt") == 0);
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: variadic positional parameter
  // ─────────────────────────────────────────────────────────────────────────────
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
      .handler    = (void*)1, // dummy handler to satisfy validation
    };

    NYA_ArgParser parser = {
      .name         = "test_app",
      .root_command = &root,
    };

    const char*     argv[] = { "app", "file1.txt", "file2.txt", "file3.txt" };
    NYA_ArgCommand* cmd    = nullptr;
    NYA_EXPECT(nya_args_parse(&parser, 4, (NYA_CString*)argv, &cmd));
    nya_assert(cmd != nullptr);
    nya_assert(files.values_count == 3);
    nya_assert(strcmp(files.values[0].as_string, "file1.txt") == 0);
    nya_assert(strcmp(files.values[1].as_string, "file2.txt") == 0);
    nya_assert(strcmp(files.values[2].as_string, "file3.txt") == 0);
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: subcommand validation works for commands with zero parameters
  // ─────────────────────────────────────────────────────────────────────────────
  {
    NYA_ArgCommand leaf = {
      .name        = "leaf",
      .description = "A leaf command",
      .handler     = (void*)1,
    };

    NYA_ArgCommand mid = {
      .name        = "mid",
      .description = "A mid command",
      .subcommands = { &leaf },
    };

    NYA_ArgCommand root = {
      .is_root     = true,
      .subcommands = { &mid },
    };

    NYA_ArgParser parser = {
      .name         = "test_app",
      .root_command = &root,
    };

    // Validation should pass without crashing (validates paramless commands' subcommands)
    const char*     argv[] = { "app", "mid", "leaf" };
    NYA_ArgCommand* cmd    = nullptr;
    NYA_EXPECT(nya_args_parse(&parser, 3, (NYA_CString*)argv, &cmd));
    nya_assert(cmd == &leaf);
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: command path printing works multiple times (static state reset)
  // ─────────────────────────────────────────────────────────────────────────────
  {
    NYA_ArgParameter help1 = {
      .kind       = NYA_ARG_PARAMETER_KIND_FLAG,
      .value.type = NYA_TYPE_B8,
      .name       = "help",
    };
    NYA_ArgParameter help2 = {
      .kind       = NYA_ARG_PARAMETER_KIND_FLAG,
      .value.type = NYA_TYPE_B8,
      .name       = "help",
    };

    NYA_ArgCommand sub1 = {
      .name        = "sub1",
      .description = "First subcommand",
      .handler     = (void*)1,
      .parameters  = { &help1 },
    };
    NYA_ArgCommand sub2 = {
      .name        = "sub2",
      .description = "Second subcommand",
      .handler     = (void*)1,
      .parameters  = { &help2 },
    };

    NYA_ArgCommand root = {
      .is_root     = true,
      .subcommands = { &sub1, &sub2 },
    };

    NYA_ArgParser parser = {
      .name            = "test_app",
      .executable_name = "app",
      .root_command    = &root,
    };

    // Print usage for sub1, then sub2 — should not corrupt static state
    nya_args_print_usage(&parser, &sub1);
    nya_args_print_usage(&parser, &sub2);
    // If we get here without crashing, the static state reset is working
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: command path joining
  // ─────────────────────────────────────────────────────────────────────────────
  {
    NYA_ArgCommand leaf = { .name = "debug", .handler = (void*)1 };
    NYA_ArgCommand mid  = { .name = "run", .subcommands = { &leaf } };
    NYA_ArgCommand root = { .is_root = true, .subcommands = { &mid } };

    NYA_ArgCommandVisit visit = {
      .command    = &leaf,
      .path       = { &root, &mid, &leaf },
      .path_count = 3,
    };

    char buffer[128];

    nya_args_command_path_join(&visit, "_build", "_", buffer, sizeof(buffer));
    nya_assert(nya_string_equals(buffer, "_build_run_debug"), "got '%s'", buffer);

    // A different separator is what a shell that names things by command line rather than by
    // function would ask for.
    nya_args_command_path_join(&visit, "build", " ", buffer, sizeof(buffer));
    nya_assert(nya_string_equals(buffer, "build run debug"), "got '%s'", buffer);

    // No prefix: the first name must not be preceded by a separator.
    nya_args_command_path_join(&visit, nullptr, "_", buffer, sizeof(buffer));
    nya_assert(nya_string_equals(buffer, "run_debug"), "got '%s'", buffer);

    // The unnamed root contributes the prefix and nothing else.
    NYA_ArgCommandVisit root_visit = { .command = &root, .path = { &root }, .path_count = 1 };
    nya_args_command_path_join(&root_visit, "_build", "_", buffer, sizeof(buffer));
    nya_assert(nya_string_equals(buffer, "_build"), "got '%s'", buffer);

    // Truncated rather than overrun when the path does not fit.
    char small[8];
    nya_args_command_path_join(&visit, "_build", "_", small, sizeof(small));
    nya_assert(strlen(small) < sizeof(small), "wrote past the end of the buffer");
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: walking the command tree
  // ─────────────────────────────────────────────────────────────────────────────
  {
    NYA_ArgParameter root_flag = { .kind = NYA_ARG_PARAMETER_KIND_FLAG, .value.type = NYA_TYPE_B8, .name = "help" };
    NYA_ArgParameter run_flag  = { .kind = NYA_ARG_PARAMETER_KIND_FLAG, .value.type = NYA_TYPE_B8, .name = "quiet" };

    NYA_ArgCommand debug = { .name = "debug", .handler = (void*)1 };
    NYA_ArgCommand dev   = { .name = "dev", .handler = (void*)1 };
    NYA_ArgCommand run   = { .name = "run", .parameters = { &run_flag }, .subcommands = { &debug, &dev } };
    NYA_ArgCommand docs  = { .name = "docs", .handler = (void*)1 };
    NYA_ArgCommand root  = { .is_root = true, .parameters = { &root_flag }, .subcommands = { &run, &docs } };

    NYA_ArgParser parser = { .name = "test_app", .root_command = &root };

    WalkRecord record = { 0 };
    nya_args_walk_commands(&parser, &record_visit, &record);

    // Depth first, in declaration order.
    nya_assert(record.count == 5, "visited %u commands", record.count);
    nya_assert(nya_string_equals(record.names[0], "<root>"));
    nya_assert(nya_string_equals(record.names[1], "run"));
    nya_assert(nya_string_equals(record.names[2], "debug"));
    nya_assert(nya_string_equals(record.names[3], "dev"));
    nya_assert(nya_string_equals(record.names[4], "docs"));

    // Depth, as the path the command was reached through.
    nya_assert(record.path_counts[0] == 1);
    nya_assert(record.path_counts[1] == 2);
    nya_assert(record.path_counts[2] == 3);
    nya_assert(record.path_counts[4] == 2);

    // Flags accumulate down the path: root's everywhere, run's only under run.
    nya_assert(record.flag_counts[0] == 1);
    nya_assert(record.flag_counts[1] == 2);
    nya_assert(record.flag_counts[2] == 2);
    nya_assert(nya_string_equals(record.last_flags[2], "quiet"));

    // And are restored on the way back out, or docs would inherit run's.
    nya_assert(record.flag_counts[4] == 1, "docs saw %u flags", record.flag_counts[4]);
    nya_assert(nya_string_equals(record.last_flags[4], "help"));
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: shell registry
  // ─────────────────────────────────────────────────────────────────────────────
  {
    nya_assert(nya_args_completion_shell_name(0) != nullptr, "no shell backend is registered");
    nya_assert(nya_string_equals(nya_args_completion_shell_name(0), "zsh"));

    // The list has to terminate, since choices_fn consumers walk it until nullptr.
    u32 count = 0;
    while (nya_args_completion_shell_name(count) != nullptr) {
      count++;
      nya_assert(count < 64, "shell list never terminated");
    }
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: unknown shell is an error, and writes nothing
  // ─────────────────────────────────────────────────────────────────────────────
  {
    NYA_ArgCommand root   = { .is_root = true, .handler = (void*)1 };
    NYA_ArgParser  parser = { .name = "test_app", .root_command = &root };

    FILE* stream = tmpfile();
    nya_assert(stream != nullptr);

    NYA_Error result = nya_args_write_completions(&parser, "test_app", "definitely-not-a-shell", stream);
    nya_assert(!result.ok, "unknown shell was accepted");

    // A partial script is worse than none: it would be sourced and silently break completion.
    (void)fflush(stream);
    nya_assert(ftell(stream) == 0, "wrote output for an unknown shell");

    (void)fclose(stream);
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: generated zsh script covers the whole tree
  // ─────────────────────────────────────────────────────────────────────────────
  {
    NYA_ArgParameter help_flag = {
      .kind        = NYA_ARG_PARAMETER_KIND_FLAG,
      .value.type  = NYA_TYPE_B8,
      .name        = "help",
      .description = "Show this message.",
    };

    NYA_ArgCommand debug = { .name = "debug", .description = "Run the debug build.", .handler = (void*)1 };
    NYA_ArgCommand run   = { .name = "run", .description = "Run things.", .subcommands = { &debug } };
    NYA_ArgCommand root  = { .is_root = true, .parameters = { &help_flag }, .subcommands = { &run } };

    NYA_ArgParser parser = { .name = "test_app", .version = "1.2.3", .root_command = &root };

    char script[8192];
    capture_completions(&parser, "mytool", "zsh", script, sizeof(script));

    assert_contains(script, "#compdef mytool");
    assert_contains(script, "1.2.3");

    // One function per command, named after its path, plus the call that runs the root one.
    assert_contains(script, "_mytool() {");
    assert_contains(script, "_mytool_run() {");
    assert_contains(script, "_mytool_run_debug() {");
    assert_contains(script, "_mytool \"$@\"");

    // Subcommands are offered with their descriptions and dispatched to their functions.
    assert_contains(script, "'run:Run things.'");
    assert_contains(script, "'debug:Run the debug build.'");
    assert_contains(script, "run) _mytool_run");

    // A flag declared on the root is in scope everywhere below it, matching what the parser accepts.
    assert_contains(script, "'--help[Show this message.]'");
    nya_assert(strstr(strstr(script, "_mytool_run_debug() {"), "--help") != nullptr, "root flag missing from the deepest command");
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: zsh escaping of descriptions
  // ─────────────────────────────────────────────────────────────────────────────
  {
    NYA_ArgParameter flag = {
      .kind        = NYA_ARG_PARAMETER_KIND_FLAG,
      .value.type  = NYA_TYPE_B8,
      .name        = "tricky",
      .description = "Don't [do] this: really",
    };

    NYA_ArgCommand sub  = { .name = "sub", .description = "Colons: and 'quotes'", .handler = (void*)1 };
    NYA_ArgCommand root = { .is_root = true, .parameters = { &flag }, .subcommands = { &sub } };

    NYA_ArgParser parser = { .name = "test_app", .root_command = &root };

    char script[8192];
    capture_completions(&parser, "mytool", "zsh", script, sizeof(script));

    // In an _arguments spec: quote closed and reopened, brackets and colons escaped so the
    // description does not terminate early.
    assert_contains(script, "'--tricky[Don'\\''t \\[do\\] this\\: really]'");

    // In a _describe entry the text is literal past the first colon, so only the colon and the
    // quote need handling — escaped brackets would show as backslashes in the listing.
    assert_contains(script, "'sub:Colons\\: and '\\''quotes'\\'''");
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: completion descriptor to zsh action
  // ─────────────────────────────────────────────────────────────────────────────
  {
    char spec[4096];

    // DEFAULT on a string guesses paths, on a number guesses nothing.
    NYA_ArgParameter as_string = { .kind = NYA_ARG_PARAMETER_KIND_POSITIONAL, .value.type = NYA_TYPE_STRING, .name = "path" };
    capture_positional_spec(&as_string, spec, sizeof(spec));
    assert_contains(spec, "':path:_files'");

    NYA_ArgParameter as_number = { .kind = NYA_ARG_PARAMETER_KIND_POSITIONAL, .value.type = NYA_TYPE_S64, .name = "count" };
    capture_positional_spec(&as_number, spec, sizeof(spec));
    assert_contains(spec, "':count:'");

    // NONE is the way to say a string argument is not a path.
    NYA_ArgParameter none = {
      .kind       = NYA_ARG_PARAMETER_KIND_POSITIONAL,
      .value.type = NYA_TYPE_STRING,
      .name       = "name",
      .completion = { .kind = NYA_ARG_COMPLETION_KIND_NONE },
    };
    capture_positional_spec(&none, spec, sizeof(spec));
    assert_contains(spec, "':name:'");
    assert_not_contains(spec, "_files");

    // A relative directory is resolved by the shell at completion time, because _files given a
    // relative -W silently completes from the filesystem root instead.
    NYA_ArgParameter relative = {
      .kind       = NYA_ARG_PARAMETER_KIND_POSITIONAL,
      .variadic   = true,
      .value.type = NYA_TYPE_STRING,
      .name       = "tests",
      .completion = { .kind = NYA_ARG_COMPLETION_KIND_FILE, .directory = "tests", .glob = "*.c" },
    };
    capture_positional_spec(&relative, spec, sizeof(spec));
    assert_contains(spec, "'*:tests:_files -W \"${PWD}/tests\" -g \"*.c\"'");

    // An absolute one is already usable, so it is left alone.
    NYA_ArgParameter absolute = {
      .kind       = NYA_ARG_PARAMETER_KIND_POSITIONAL,
      .value.type = NYA_TYPE_STRING,
      .name       = "conf",
      .completion = { .kind = NYA_ARG_COMPLETION_KIND_FILE, .directory = "/etc" },
    };
    capture_positional_spec(&absolute, spec, sizeof(spec));
    assert_contains(spec, "':conf:_files -W \"/etc\"'");
    assert_not_contains(spec, "${PWD}");

    NYA_ArgParameter directory = {
      .kind       = NYA_ARG_PARAMETER_KIND_POSITIONAL,
      .value.type = NYA_TYPE_STRING,
      .name       = "out",
      .completion = { .kind = NYA_ARG_COMPLETION_KIND_DIRECTORY },
    };
    capture_positional_spec(&directory, spec, sizeof(spec));
    assert_contains(spec, "':out:_files -/'");

    NYA_ArgParameter choices = {
      .kind       = NYA_ARG_PARAMETER_KIND_POSITIONAL,
      .value.type = NYA_TYPE_STRING,
      .name       = "mode",
      .completion = { .kind = NYA_ARG_COMPLETION_KIND_CHOICES, .choices = { "fast", "slow" } },
    };
    capture_positional_spec(&choices, spec, sizeof(spec));
    assert_contains(spec, "':mode:(fast slow)'");

    // A list the program already owns, rather than a literal that can fall out of date.
    NYA_ArgParameter dynamic = {
      .kind       = NYA_ARG_PARAMETER_KIND_POSITIONAL,
      .value.type = NYA_TYPE_STRING,
      .name       = "fruit",
      .completion = { .kind = NYA_ARG_COMPLETION_KIND_CHOICES, .choices_fn = &fruit_choices },
    };
    capture_positional_spec(&dynamic, spec, sizeof(spec));
    assert_contains(spec, "':fruit:(apple banana)'");

    // choices_fn wins, so a caller cannot end up offering a stale literal list alongside it.
    NYA_ArgParameter both = {
      .kind       = NYA_ARG_PARAMETER_KIND_POSITIONAL,
      .value.type = NYA_TYPE_STRING,
      .name       = "fruit",
      .completion = { .kind = NYA_ARG_COMPLETION_KIND_CHOICES, .choices = { "stale" }, .choices_fn = &fruit_choices },
    };
    capture_positional_spec(&both, spec, sizeof(spec));
    assert_contains(spec, "':fruit:(apple banana)'");
    assert_not_contains(spec, "stale");
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: the generated zsh script parses in a real zsh
  // ─────────────────────────────────────────────────────────────────────────────
  {
    // Asserting on substrings cannot catch an unbalanced quote three lines away, and a completion
    // script that does not parse fails silently at use. Skipped where there is no zsh to ask.
    if (system("command -v zsh > /dev/null 2>&1") == 0) {
      NYA_ArgParameter tricky_flag = {
        .kind        = NYA_ARG_PARAMETER_KIND_FLAG,
        .value.type  = NYA_TYPE_B8,
        .name        = "tricky",
        .description = "Don't [do] this: really",
      };

      NYA_ArgParameter tests = {
        .kind        = NYA_ARG_PARAMETER_KIND_POSITIONAL,
        .variadic    = true,
        .value.type  = NYA_TYPE_STRING,
        .name        = "tests",
        .description = "Which tests to run.",
        .completion  = { .kind = NYA_ARG_COMPLETION_KIND_FILE, .directory = "tests", .glob = "*.c" },
      };

      NYA_ArgParameter mode = {
        .kind       = NYA_ARG_PARAMETER_KIND_POSITIONAL,
        .value.type = NYA_TYPE_STRING,
        .name       = "mode",
        .completion = { .kind = NYA_ARG_COMPLETION_KIND_CHOICES, .choices = { "fast", "slow" } },
      };

      NYA_ArgCommand run_test = { .name = "test", .description = "Colons: and 'quotes'", .handler = (void*)1, .parameters = { &tests } };
      NYA_ArgCommand set_mode = { .name = "mode", .description = "Set the mode.", .handler = (void*)1, .parameters = { &mode } };
      NYA_ArgCommand run      = { .name = "run", .description = "Run things.", .subcommands = { &run_test, &set_mode } };
      NYA_ArgCommand root     = { .is_root = true, .parameters = { &tricky_flag }, .subcommands = { &run } };

      NYA_ArgParser parser = { .name = "test_app", .version = "1.2.3", .root_command = &root };

      char script[8192];
      capture_completions(&parser, "mytool", "zsh", script, sizeof(script));

      NYA_ConstCString path = "./_test_args_completion.zsh";
      FILE*            file = fopen(path, "w");
      nya_assert(file != nullptr, "could not write the script to check");
      (void)fwrite(script, 1, strlen(script), file);
      (void)fclose(file);

      s32 status = system("zsh -n ./_test_args_completion.zsh");
      (void)remove(path);

      nya_assert(status == 0, "generated zsh script does not parse:\n%s", script);
    }
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // CLEANUP
  // ─────────────────────────────────────────────────────────────────────────────
  nya_arena_destroy(arena);

  return 0;
}
