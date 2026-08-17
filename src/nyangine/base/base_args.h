/**
 * @file base_args.h
 *
 * CLI Argument parsing with integration into the build system.
 *
 * Example:
 * ```c
 *  NYA_ArgParameter help_flag = {
 *   .kind        = NYA_ARG_PARAMETER_KIND_FLAG,
 *   .value.type  = NYA_TYPE_B8,
 *   .name        = "help",
 *   .description = "Show this message.",
 * };
 *
 * NYA_ArgCommand docs = {
 *   .name        = "docs",
 *   .description = "Open doxygen generated documentation.",
 *   .build_rule  = ...,
 * };
 *
 * NYA_ArgCommand run = {
 *   .name = "run",
 *   .description = "Run things.",
 *   .subcommands = {
 *       &(NYA_ArgCommand){
 *           .name        = "debug",
 *           .description = "Run the debug executable.",
 *           .build_rule  = ...,
 *       },
 *       &(NYA_ArgCommand){
 *           .name        = "release",
 *           .description = "Run the release executable.",
 *           .build_rule  = ...,
 *       },
 * }};
 *
 * NYA_ArgParser parser = {
 *     .name = "nyangine build system",
 *     .description = "Build system for the nyangine project.",
 *
 *     .root_command = &(NYA_ArgCommand){
 *         .is_root    = true,
 *         .parameters = {
 *             &help_flag,
 *         },
 *         .subcommands = {
 *             &run,
 *             &docs,
 *         },
 *     },
 * };
 *
 *
 * s32 main(s32 argc, NYA_CString* argv) {
 *   parser.executable_name = argv[0];
 *
 *   NYA_ArgCommand* command;
 *   NYA_Error      parse_result = nya_args_parse(&parser, argc, argv, &command);
 *   if (!parse_result.ok) {
 *     (void)fprintf(stderr, "Error: %s\n\n", parse_result.message);
 *     nya_args_print_usage(&parser, nullptr);
 *     return EXIT_FAILURE;
 *   }
 *
 *   if (help_flag.value.as_b8) {
 *     nya_args_print_usage(&parser, command);
 *     return EXIT_SUCCESS;
 *   }
 *
 *   NYA_Error run_result = nya_args_run_command(command);
 *   if (!run_result.ok) {
 *     (void)fprintf(stderr, "Error: %s\n\n", run_result.message);
 *     nya_args_print_usage(&parser, command);
 *     return EXIT_FAILURE;
 *   }
 *
 *   return EXIT_SUCCESS;
 * }
 * ```
 *
 * For an more complete example see build.c.
 * */
#pragma once

#include "nyangine/base/base_build.h"
#include "nyangine/base/base_object.h"
#include "nyangine/base/base_string.h"
#include "nyangine/base/base_types.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * TYPES
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

#define NYA_ARG_MAX_COMMANDS   256
#define NYA_ARG_MAX_PARAMETERS 256
#define NYA_ARG_MAX_CHOICES    64

/**
 * Longest flag name this parser will match, buffer included.
 *
 * Only `--flag=value` needs it: the name has to be split from the value into somewhere, and argv is
 * the caller's memory to leave alone. A flag name anywhere near this is a usability problem long
 * before it is a buffer problem.
 * */
#define NYA_ARG_MAX_NAME       128

typedef enum NYA_ArgParameterKind  NYA_ArgParameterKind;
typedef enum NYA_ArgCompletionKind NYA_ArgCompletionKind;
typedef struct NYA_ArgParser       NYA_ArgParser;
typedef struct NYA_ArgCommand      NYA_ArgCommand;
typedef struct NYA_ArgParameter    NYA_ArgParameter;
typedef struct NYA_ArgCompletion   NYA_ArgCompletion;
typedef struct NYA_ArgCommandVisit NYA_ArgCommandVisit;
typedef struct NYA_ArgShell        NYA_ArgShell;

enum NYA_ArgParameterKind {
    NYA_ARG_PARAMETER_KIND_FLAG,
    NYA_ARG_PARAMETER_KIND_POSITIONAL,
    NYA_ARG_PARAMETER_KIND_COUNT,
};

/**
 * What an argument can be, described in terms of the argument rather than of any one shell.
 *
 * Every shell expresses these differently — zsh has _files, bash has compgen and a helper that may
 * not be installed — so a parameter says what it wants and each backend says how. Anything phrased
 * as shell source here would be a completion script for one shell hiding in the parser.
 * */
enum NYA_ArgCompletionKind {
    /** Guessed from the parameter's type: strings complete paths, numbers complete nothing. */
    NYA_ARG_COMPLETION_KIND_DEFAULT = 0,
    NYA_ARG_COMPLETION_KIND_NONE,
    NYA_ARG_COMPLETION_KIND_FILE,
    NYA_ARG_COMPLETION_KIND_DIRECTORY,
    NYA_ARG_COMPLETION_KIND_CHOICES,
    NYA_ARG_COMPLETION_KIND_COUNT,
};

struct NYA_ArgCompletion {
    NYA_ArgCompletionKind kind;

    /** FILE and DIRECTORY: complete inside this directory rather than the working directory. Relative to it. */
    NYA_ConstCString directory;

    /** FILE: only names matching this glob, e.g. "*.c". */
    NYA_ConstCString glob;

    /** CHOICES: the fixed set, terminated by a nullptr entry. */
    NYA_ConstCString choices[NYA_ARG_MAX_CHOICES];

    /**
     * CHOICES: a set that is only known at runtime. Returns nullptr past the last entry.
     *
     * Takes precedence over `choices`. Exists so a list the program already owns does not have to
     * be restated as a literal that can fall out of date — nya_args_completion_shell_name is one.
     * */
    NYA_ConstCString (*choices_fn)(u32 index);
};

/**
 * NYA_ArgParameter
 *
 * Parameters have to follow these rules:
 * - Name is required.
 * - Description is optional.
 * - Types can be B8, S64, F64 or STRING.
 * - Flags can have default values, positionals cannot.
 * - Only the last positional parameter can be variadic.
 *
 * For more details, see `_nya_args_validate_parser` in base_args.c.
 * */
struct NYA_ArgParameter {
    NYA_ArgParameterKind kind;
    b8                   variadic;

    NYA_ConstCString name;
    NYA_ConstCString description;
    NYA_Value        default_value;

    /**
     * What shell completion should offer for this parameter. Ignored by the parser itself.
     *
     * Only the caller knows what a given argument can be, and a generator would otherwise fall back
     * to completing paths in the working directory, which is wrong for anything that is not one.
     * */
    NYA_ArgCompletion completion;

    /* will be filled after parsing */

    b8 was_matched;

    /** used for single parameters */
    NYA_Value value;

    /** used for variadic parameters. default: 0*/
    u32       values_count;
    NYA_Value values[NYA_ARG_MAX_PARAMETERS];
};

/**
 * NYA_ArgCommand
 *
 * Commands have to follow these rules:
 * - Root command cannot have a name or description.
 * - Non-root commands must have a name.
 * - Description is optional.
 * - Commands can either have positional parameters or subcommands, not both.
 * - Commands with positional parameters must have either a handler or a build rule.
 *
 * For more details, see `_nya_args_validate_parser` in base_args.c.
 * */
struct NYA_ArgCommand {
    b8 is_root;

    NYA_ConstCString name;
    NYA_ConstCString description;

    NYA_ArgCommand*   subcommands[NYA_ARG_MAX_COMMANDS];
    NYA_ArgParameter* parameters[NYA_ARG_MAX_PARAMETERS];

    void (*handler)(NYA_ArgCommand* command);
    NYA_BuildRule* build_rule;

    /* will be filled after parsing */

    /** bad subcommand, only flags are parsed */
    b8 incomplete;
};

/**
 * NYA_ArgParser
 *
 * The main parser has to follow these rules:
 * - Name is required.
 * - Version, author and description are optional.
 * - Executable name is optional, if provided argv[0] must match it.
 *
 * For more details, see `_nya_args_validate_parser` in base_args.c.
 *
 * Special cases:
 * - If bad positiona arguments are provided, the flags will still be parsed if possible. The returned command
 *   will be marked incomplete.
 * - If "--help" flag is registered and provided, then no parameter validation will be done. The returned command
 *   will be marked incomplete.
 * */
struct NYA_ArgParser {
    NYA_ConstCString name;
    NYA_ConstCString version;
    NYA_ConstCString author;
    NYA_ConstCString description;

    NYA_CString     executable_name;
    NYA_ArgCommand* root_command;
};

/**
 * One command as a completion backend sees it: the node, how it was reached, and what flags apply.
 *
 * The flags are the reason this is not just a command pointer. nya_args_parse matches a flag against
 * every command on the path, so a backend that only looked at command->parameters would complete
 * less than the parser accepts, and every backend would have to rediscover that independently.
 * */
struct NYA_ArgCommandVisit {
    NYA_ArgCommand* command;

    /** Root first, `command` last. The root has no name, so path[0]->name is nullptr. */
    NYA_ArgCommand* path[NYA_ARG_MAX_COMMANDS];
    u32             path_count;

    /** Inherited from the path, then the command's own, in the order a user would see them. */
    NYA_ArgParameter* flags[NYA_ARG_MAX_PARAMETERS];
    u32               flag_count;

    void* userdata;
};

typedef void (*NYA_ArgCommandVisitFn)(const NYA_ArgCommandVisit* visit);

/**
 * A completion script generator for one shell.
 *
 * Adding a shell is writing `generate` and adding an entry to the table in base_args.c. Nothing
 * outside that file needs to learn the new name: the CLI takes the shell as an argument and looks
 * it up, and nya_args_completion_shell_name feeds the list back to completion itself.
 * */
struct NYA_ArgShell {
    NYA_ConstCString name;
    NYA_ConstCString description;

    /** Writes a complete, self contained completion script to `stream`. */
    void (*generate)(NYA_ArgParser* parser, NYA_ConstCString binary_name, FILE* stream);
};

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * FUNCTIONS AND MACROS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

NYA_API NYA_Error nya_args_parse(NYA_ArgParser* parser, s32 argc, NYA_CString* argv, OUT NYA_ArgCommand** out_command) __attr_no_discard;
NYA_API NYA_Error nya_args_run_command(NYA_ArgCommand* command) __attr_no_discard;
NYA_API void      nya_args_print_usage(NYA_ArgParser* parser, NYA_ArgCommand* command_override);

/**
 * Writes a completion script for `shell` covering the whole command tree to stdout.
 *
 * Generated rather than hand written, so a subcommand added to the parser is completable without
 * anyone remembering to touch a second file. Descriptions come along wherever the shell can show
 * them.
 *
 * `binary_name` is what the user types, which is not parser->executable_name: that one is a path
 * like "./build" and a completion script wants the bare word.
 *
 * Fails, rather than writing a partial script, if no backend is registered under that name.
 * */
NYA_API NYA_Error nya_args_print_completions(NYA_ArgParser* parser, NYA_ConstCString binary_name, NYA_ConstCString shell) __attr_no_discard;

/** As nya_args_print_completions, to an arbitrary stream. What makes a generated script assertable in a test. */
NYA_API NYA_Error nya_args_write_completions(NYA_ArgParser* parser, NYA_ConstCString binary_name, NYA_ConstCString shell, FILE* stream)
    __attr_no_discard;

/** Name of the registered shell at `index`, or nullptr past the last one. Fits NYA_ArgCompletion.choices_fn. */
NYA_API NYA_ConstCString nya_args_completion_shell_name(u32 index);

/**
 * Calls `visit_fn` for the root command and then, depth first, for every command under it.
 *
 * The shared half of generating completions: walking the tree and working out which flags are in
 * scope is the same job for every shell, and only what gets printed per command differs.
 * */
NYA_API void nya_args_walk_commands(NYA_ArgParser* parser, NYA_ArgCommandVisitFn visit_fn, void* userdata);

/**
 * Joins a visited command's path into `buffer`, e.g. "_build_run_debug" or "build run debug".
 *
 * Shells name things per command — a function, a case label — and all of them need a unique, stable
 * string per node. The unnamed root contributes `prefix` alone.
 * */
NYA_API void
nya_args_command_path_join(const NYA_ArgCommandVisit* visit, NYA_ConstCString prefix, NYA_ConstCString separator, OUT char* buffer, u64 buffer_size);
