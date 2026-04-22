/**
 * @file base_args.h
 *
 * CLI Argument parsing with integration into the build system.
 *
 * Example:
 * ```c
 *  NYA_ArgParameter help_flag = {
 *   .kind        = NYA_ARG_PARAMETER_KIND_FLAG,
 *   .value.type  = NYA_TYPE_BOOL,
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
 *   NYA_Result      parse_result = nya_args_parse(&parser, argc, argv, &command);
 *   if (parse_result.error != NYA_ERROR_NONE) {
 *     (void)fprintf(stderr, "Error: %s\n\n", parse_result.message);
 *     nya_args_print_usage(&parser, nullptr);
 *     return EXIT_FAILURE;
 *   }
 *
 *   if (help_flag.value.as_bool) {
 *     nya_args_print_usage(&parser, command);
 *     return EXIT_SUCCESS;
 *   }
 *
 *   NYA_Result run_result = nya_args_run_command(command);
 *   if (run_result.error != NYA_ERROR_NONE) {
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

typedef enum NYA_ArgParameterKind NYA_ArgParameterKind;
typedef struct NYA_ArgParser      NYA_ArgParser;
typedef struct NYA_ArgCommand     NYA_ArgCommand;
typedef struct NYA_ArgParameter   NYA_ArgParameter;

enum NYA_ArgParameterKind {
  NYA_ARG_PARAMETER_KIND_FLAG,
  NYA_ARG_PARAMETER_KIND_POSITIONAL,
  NYA_ARG_PARAMETER_KIND_COUNT,
};

/**
 * NYA_ArgParameter
 *
 * Parameters have to follow these rules:
 * - Name is required.
 * - Description is optional.
 * - Types can be BOOL, INTEGER, FLOAT or STRING.
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

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * FUNCTIONS AND MACROS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

NYA_API NYA_EXTERN NYA_Result nya_args_parse(NYA_ArgParser* parser, s32 argc, NYA_CString* argv, OUT NYA_ArgCommand** out_command) __attr_no_discard;
NYA_API NYA_EXTERN NYA_Result nya_args_run_command(NYA_ArgCommand* command) __attr_no_discard;
NYA_API NYA_EXTERN void       nya_args_print_usage(NYA_ArgParser* parser, NYA_ArgCommand* command_override);
