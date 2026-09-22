/**
 * @file examples/cli_tool/main.c
 *
 * A plain command line program: no window, no renderer, no game loop. base for memory, strings and
 * logging; base_args for the command tree; serde for the document.
 *
 * ```
 * ./build run example cli_tool
 * ```
 *
 * The binary takes arguments of its own, which `./build run example` does not forward, so run it
 * directly to see the subcommands:
 *
 * ```
 * ./cli_tool.example convert --format json --output out.json
 * ./cli_tool.example --help
 * ```
 * */
#include "nyangine/nyangine.h"

#include "nyangine/nyangine.c"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * THE DOCUMENT
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/** Where a converted document is written when no path is given. */
#define DEFAULT_OUTPUT "cli_tool.out"

/**
 * The document this example works on, built in memory so it needs no input file.
 * */
NYA_INTERNAL NYA_Object* document_create(NYA_Arena* arena) {
    nya_assert(arena != nullptr);

    NYA_Object* engine = nya_object_create(arena);
    nya_object_set(engine, "name", (NYA_Value){ .type = NYA_TYPE_STRING, .as_string = "nyangine" });
    nya_object_set(engine, "modules", (NYA_Value){ .type = NYA_TYPE_U32, .as_u32 = 12 });
    nya_object_set(engine, "assertions_in_release", (NYA_Value){ .type = NYA_TYPE_B8, .as_b8 = true });

    NYA_Object* root = nya_object_create(arena);
    nya_object_set(root, "engine", (NYA_Value){ .type = NYA_TYPE_OBJECT, .as_object = *engine });
    nya_object_set(root, "tick_rate_hz", (NYA_Value){ .type = NYA_TYPE_U32, .as_u32 = 62 });

    return root;
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * COMMAND HANDLERS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

NYA_INTERNAL NYA_ArgParameter format_flag;
NYA_INTERNAL NYA_ArgParameter output_path;

/** Maps the `--format` string onto the enum. NYA_SERDE_FORMAT_COUNT means "not one of ours". */
NYA_INTERNAL NYA_SerdeFormat format_from_name(NYA_ConstCString name) {
    for (u32 i = 0; i < NYA_SERDE_FORMAT_COUNT; i++) {
        if (nya_string_equals(name, NYA_SERDE_FORMAT_NAME_MAP[i])) return (NYA_SerdeFormat)i;
    }

    return NYA_SERDE_FORMAT_COUNT;
}

/** `show`: serializes the document to stdout in every format the engine has. */
NYA_INTERNAL void show_runner(NYA_ArgCommand* command) {
    nya_unused(command);

    NYA_Arena* arena = nya_arena_create(.name = "cli_show");
    defer      nya_arena_destroy(arena);

    NYA_Object* document = document_create(arena);

    for (u32 i = 0; i < NYA_SERDE_FORMAT_COUNT; i++) {
        NYA_String* text = nya_serialize(arena, document, (NYA_SerdeFormat)i, NYA_SERDE_PRETTY);

        // The nya format is binary framed, so its bytes are not printable. Report its size instead
        // of spraying control characters over the terminal.
        if ((NYA_SerdeFormat)i == NYA_SERDE_FORMAT_NYA) {
            nya_log_info("%s: %llu bytes", NYA_SERDE_FORMAT_NAME_MAP[i], (unsigned long long)text->length);
            continue;
        }

        nya_log_info("%s:\n" NYA_FMT_STRING, NYA_SERDE_FORMAT_NAME_MAP[i], NYA_FMT_STRING_ARG(text));
    }
}

/** `convert`: writes the document in one format, reads it back, and checks the round trip. */
NYA_INTERNAL void convert_runner(NYA_ArgCommand* command) {
    nya_assert(command != nullptr);

    NYA_ConstCString name = format_flag.value.as_string;

    NYA_SerdeFormat format = format_from_name(name);
    if (format == NYA_SERDE_FORMAT_COUNT) {
        // Bad input is an operating error: report it and exit non-zero, never crash.
        (void)fprintf(stderr, "Error: '%s' is not a known format.\n", name);
        exit(EXIT_FAILURE);
    }

    // The parser fills `value` from `default_value` when the argument is absent, so there is no
    // second place here that has to know what the default is.
    NYA_ConstCString path = output_path.value.as_string;

    NYA_Arena* arena = nya_arena_create(.name = "cli_convert");
    defer      nya_arena_destroy(arena);

    NYA_Object* document = document_create(arena);
    NYA_String* text     = nya_serialize(arena, document, format, NYA_SERDE_PRETTY);

    NYA_Error written = nya_file_write_atomic(path, text);
    if (!written.ok) {
        (void)fprintf(stderr, "Error: could not write %s: %s\n", path, (NYA_ConstCString)written.message);
        exit(EXIT_FAILURE);
    }

    NYA_Object* restored = nullptr;
    NYA_Error   read     = nya_serde_load_file(arena, path, NYA_SERDE_NONE, &restored);
    if (!read.ok) {
        (void)fprintf(stderr, "Error: could not read %s back: %s\n", path, (NYA_ConstCString)read.message);
        exit(EXIT_FAILURE);
    }

    NYA_Value* tick_rate = nya_object_get(restored, "tick_rate_hz");
    nya_assert(tick_rate != nullptr, "the round trip lost a key the writer put in");

    nya_log_info("Wrote %llu bytes of %s to %s and read it back.", (unsigned long long)text->length, name, path);
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * THE COMMAND TREE
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/*
 * A flag with no default_value is required, whatever its kind, so every optional argument here
 * carries one. The parser then fills `value` from it, and no call site has to know the default.
 */
NYA_INTERNAL NYA_ArgParameter format_flag = {
    .kind          = NYA_ARG_PARAMETER_KIND_FLAG,
    .value.type    = NYA_TYPE_STRING,
    .name          = "format",
    .description   = "Which format to write: nya, json or jsonc. Defaults to json.",
    .default_value = { .type = NYA_TYPE_STRING, .as_string = "json" },
    .completion    = { .kind = NYA_ARG_COMPLETION_KIND_CHOICES, .choices = { "nya", "json", "jsonc", nullptr, }, },
};

/*
 * A flag rather than a positional, because the parser has no optional positional: a positional is
 * required unless it is variadic, and only a flag may carry a default_value (base_args.c:845
 * asserts it). A path this command already has a sensible default for should not be mandatory.
 */
NYA_INTERNAL NYA_ArgParameter output_path = {
    .kind          = NYA_ARG_PARAMETER_KIND_FLAG,
    .value.type    = NYA_TYPE_STRING,
    .name          = "output",
    .description   = "Where to write it. Defaults to " DEFAULT_OUTPUT ".",
    .default_value = { .type = NYA_TYPE_STRING, .as_string = DEFAULT_OUTPUT },
    .completion    = { .kind = NYA_ARG_COMPLETION_KIND_FILE, },
};

NYA_INTERNAL NYA_ArgParameter help_flag = {
    .kind        = NYA_ARG_PARAMETER_KIND_FLAG,
    .value.type  = NYA_TYPE_B8,
    .name        = "help",
    .description = "Show this message.",
};

NYA_INTERNAL NYA_ArgCommand show = {
    .name        = "show",
    .description = "Print the document in every format.",
    .handler     = &show_runner,
};

NYA_INTERNAL NYA_ArgCommand convert = {
    .name        = "convert",
    .description = "Write the document to a file and read it back.",
    .handler     = &convert_runner,
    .parameters  = { &format_flag, &output_path, },
};

NYA_INTERNAL NYA_ArgParser parser = {
    .name        = "cli_tool",
    .version     = NYA_VERSION,
    .description = "A nyangine example with no window in it.",

    .root_command = &(NYA_ArgCommand){
        .is_root     = true,
        .parameters  = { &help_flag, },
        .subcommands = { &show, &convert, },
    },
};

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * MAIN
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

s32 main(s32 argc, NYA_CString* argv) {
    nya_backtrace_init();

    parser.executable_name = argv[0];

    NYA_ArgCommand* command = nullptr;

    NYA_Error parsed = nya_args_parse(&parser, argc, argv, &command);
    if (!parsed.ok) {
        (void)fprintf(stderr, "Error: %s\n\n", parsed.message);
        nya_args_print_usage(&parser, nullptr);
        return EXIT_FAILURE;
    }

    if (help_flag.value.as_b8) {
        nya_args_print_usage(&parser, command);
        return EXIT_SUCCESS;
    }

    // `./build run example` passes no arguments, so with none this shows what the tool can do
    // rather than exiting silently.
    if (command == parser.root_command) {
        show_runner(command);
        nya_log_info("Pass --help to see the rest.");

        nya_backtrace_deinit();
        return EXIT_SUCCESS;
    }

    NYA_Error ran = nya_args_run_command(command);
    if (!ran.ok) {
        (void)fprintf(stderr, "Error: %s\n\n", ran.message);
        return EXIT_FAILURE;
    }

    nya_backtrace_deinit();
    return EXIT_SUCCESS;
}
