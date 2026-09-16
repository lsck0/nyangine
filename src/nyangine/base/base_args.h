/**
 * @file base_args.h
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
 * Longest flag name this parser will match, buffer included. Only `--flag=value` needs it: the name
 * must be split from the value somewhere, and argv is the caller's memory to leave alone. A flag
 * name anywhere near this is a usability problem long before it's a buffer problem.
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
     * */
    NYA_ConstCString (*choices_fn)(u32 index);
};

/**
 * NYA_ArgParameter
 * */
struct NYA_ArgParameter {
    NYA_ArgParameterKind kind;
    b8                   variadic;

    NYA_ConstCString name;
    NYA_ConstCString description;
    NYA_Value        default_value;

    /**
     * What shell completion should offer for this parameter. Ignored by the parser itself.
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
 * */
NYA_API NYA_Error nya_args_print_completions(NYA_ArgParser* parser, NYA_ConstCString binary_name, NYA_ConstCString shell) __attr_no_discard;

/** As nya_args_print_completions, to an arbitrary stream. What makes a generated script assertable in a test. */
NYA_API NYA_Error nya_args_write_completions(NYA_ArgParser* parser, NYA_ConstCString binary_name, NYA_ConstCString shell, FILE* stream)
    __attr_no_discard;

/** Name of the registered shell at `index`, or nullptr past the last one. Fits NYA_ArgCompletion.choices_fn. */
NYA_API NYA_ConstCString nya_args_completion_shell_name(u32 index);

/**
 * Calls `visit_fn` for the root command and then, depth first, for every command under it.
 * */
NYA_API void nya_args_walk_commands(NYA_ArgParser* parser, NYA_ArgCommandVisitFn visit_fn, void* userdata);

/**
 * Joins a visited command's path into `buffer`, e.g. "_build_run_debug" or "build run debug".
 * */
NYA_API void
nya_args_command_path_join(const NYA_ArgCommandVisit* visit, NYA_ConstCString prefix, NYA_ConstCString separator, OUT char* buffer, u64 buffer_size);
