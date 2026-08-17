#include "nyangine/nyangine.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API DECLARATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

#define _NYA_ARGS_DESCRIPTION_INDENT 16

/** Room for a name derived from a command path, which is every name on it joined by a separator. */
#define _NYA_ARGS_NAME_MAX 512

NYA_INTERNAL void _nya_args_validate_parser(NYA_ArgParser* parser);
NYA_INTERNAL void _nya_args_validate_command_tree(NYA_ArgCommand* command);
NYA_INTERNAL void _nya_args_print_command_path(NYA_ArgParser* parser, NYA_ArgCommand* command);

/**
 * The recursive half of nya_args_walk_commands.
 *
 * `visit` is one buffer reused down the whole walk rather than a copy per node: the path and flag
 * arrays are large and a command tree is deep enough that copying them per node would be the most
 * expensive thing about generating a completion script.
 * */
NYA_INTERNAL void _nya_args_walk_command(NYA_ArgCommand* command, NYA_ArgCommandVisit* visit, NYA_ArgCommandVisitFn visit_fn);

/** parse `str` into `value` according to `value->type`. returns false on parse failure. */
NYA_INTERNAL b8 _nya_args_parse_value(NYA_Value* value, NYA_CString str);

/*
 * ─────────────────────────────────────────────────────────
 * COMPLETION BACKENDS
 * ─────────────────────────────────────────────────────────
 */

NYA_INTERNAL void _nya_args_zsh_generate(NYA_ArgParser* parser, NYA_ConstCString binary_name, FILE* stream);

/**
 * Every shell that can be generated for.
 *
 * To add one: write a generate function in its own block below, following the zsh one, and add a
 * row here. Nothing else in the codebase names a shell.
 * */
NYA_INTERNAL const NYA_ArgShell _NYA_ARGS_SHELLS[] = {
    {
     .name        = "zsh",
     .description = "Autoloaded from a directory on fpath, as _<binary>.",
     .generate    = &_nya_args_zsh_generate,
     },
};

#define _NYA_ARGS_SHELL_COUNT (sizeof(_NYA_ARGS_SHELLS) / sizeof(_NYA_ARGS_SHELLS[0]))

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PUBLIC API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

NYA_Error nya_args_parse(NYA_ArgParser* parser, s32 argc, NYA_CString* argv, OUT NYA_ArgCommand** out_command) {
    _nya_args_validate_parser(parser);

    NYA_ArgCommand* command_to_execute         = nullptr;
    NYA_ArgCommand* path[NYA_ARG_MAX_COMMANDS] = { parser->root_command };
    u32             path_count                 = 1;

    // exec name
    if (parser->executable_name != nullptr) {
        if (!nya_string_equals(argv[0], parser->executable_name)) {
            return nya_error(NYA_ERROR_NOT_OK, "executable name mismatch. Expected '%s' but got '%s'", parser->executable_name, argv[0]);
        }
    }

subcommand_matching:
    for (u32 subcommand_index = 0; subcommand_index < NYA_ARG_MAX_COMMANDS; subcommand_index++) {
        NYA_ArgCommand* subcommand = path[path_count - 1]->subcommands[subcommand_index];
        if (subcommand == nullptr) continue;

        if (argc > 1 && nya_string_equals(argv[1], subcommand->name)) {
            argc--;
            argv++;

            // The command tree is the program's own, not the user's, so nesting this deep is a
            // structural mistake rather than bad input — but the write is unbounded either way, and
            // _nya_args_validate_command_tree recurses without a depth limit, so it would already
            // have run out of stack before reaching here.
            nya_assert(path_count < NYA_ARG_MAX_COMMANDS, "command nesting is deeper than the %d this parser tracks", NYA_ARG_MAX_COMMANDS);

            path[path_count++] = subcommand;
            goto subcommand_matching;
        }
    }

    // final command to execute
    command_to_execute      = path[path_count - 1];
    b8 bad_input_flags_only = false;
    if (command_to_execute->subcommands[0] != nullptr) {
        bad_input_flags_only           = true;
        command_to_execute->incomplete = true;
    }

    // if we have a subcommand, remove it from the args
    if (path_count > 1 && !bad_input_flags_only) {
        argc--;
        argv++;
    }

    // determine where to start processing arguments
    // if we have a subcommand, argv[0] is already the first argument (subcommand name was skipped)
    // if no subcommand, argv[0] is the program name and we should skip it
    s32 start_index = (path_count > 1) ? 0 : 1;

    for (s32 arg_index = start_index; arg_index < argc; arg_index++) {
        if (argv[arg_index] == nullptr) continue;

        if (nya_string_starts_with(argv[arg_index], "--")) {
            // we potentially have a flag
            NYA_CString       flag_name = argv[arg_index] + 2; // skip the '--' prefix
            NYA_ArgParameter* param     = nullptr;

            /*
             * `--flag=value`, split here so the lookup below sees the name alone.
             *
             * Supported for every type, not only booleans, because it is the spelling that has no
             * ambiguity: the value is attached to the flag rather than being whatever token happens
             * to come next. That distinction is what the boolean handling below depends on.
             *
             * Copied into a buffer rather than written into argv, which belongs to the caller — and
             * on some platforms to the loader. Bounded by the longest flag name a parser may declare.
             */
            char        flag_name_buffer[NYA_ARG_MAX_NAME] = { 0 };
            NYA_CString inline_value                       = nullptr;

            for (NYA_CString cursor = flag_name; *cursor != '\0'; cursor++) {
                if (*cursor != '=') continue;

                u64 name_length = (u64)(cursor - flag_name);

                if (name_length >= sizeof(flag_name_buffer)) {
                    return nya_error(NYA_ERROR_NOT_OK, "flag name is longer than the %d this parser tracks: '%s'", NYA_ARG_MAX_NAME, flag_name);
                }

                nya_memcpy(flag_name_buffer, flag_name, name_length);
                flag_name_buffer[name_length] = '\0';

                flag_name    = flag_name_buffer;
                inline_value = cursor + 1;
                break;
            }

            // flags can be of parents too, search the path
            for (u32 path_index = path_count; path_index > 0; path_index--) {
                NYA_ArgCommand* command_in_path = path[path_index - 1];
                for (u32 param_index = 0; param_index < NYA_ARG_MAX_PARAMETERS; param_index++) {
                    NYA_ArgParameter* candidate = command_in_path->parameters[param_index];
                    if (candidate == nullptr) break;
                    if (candidate->kind == NYA_ARG_PARAMETER_KIND_FLAG && nya_string_equals(candidate->name, flag_name)) {
                        param = candidate;
                        goto end_parameter_search;
                    }
                }
            }

        end_parameter_search:
            if (param != nullptr) {
                param->was_matched = true;
            } else {
                return nya_error(NYA_ERROR_NOT_OK, "unexpected flag: '--%s'", flag_name);
            }

            /*
             * A boolean flag is its own value. It never reaches for the next token.
             *
             * It used to: anything after it that did not itself start with `--` was taken as the
             * flag's value, so `./build check --strict src/main.c` consumed the filename and then
             * failed to parse it as a boolean. Every "flag followed by a positional" invocation was
             * broken, and the error message blamed the positional.
             *
             * `--flag=false` is how a boolean is written explicitly, which is unambiguous because the
             * value is attached rather than adjacent.
             */
            if (param->value.type == NYA_TYPE_B8) {
                if (inline_value == nullptr) {
                    param->value.as_b8 = true;
                } else if (!_nya_args_parse_value(&param->value, inline_value)) {
                    return nya_error(NYA_ERROR_NOT_OK, "failed to parse boolean value for flag '--%s': '%s'", param->name, inline_value);
                }
            } else {
                // integer, float and string flags require a value, attached or following.
                NYA_CString flag_value_str = inline_value;

                if (flag_value_str == nullptr) {
                    if (arg_index + 1 >= argc) {
                        return nya_error(NYA_ERROR_NOT_OK, "flag '--%s' expects a value, but none was provided", param->name);
                    }

                    flag_value_str = argv[arg_index + 1];
                    arg_index++;
                }

                if (!_nya_args_parse_value(&param->value, flag_value_str)) {
                    return nya_error(
                        NYA_ERROR_NOT_OK,
                        "failed to parse %s value for flag '--%s': '%s'",
                        NYA_TYPE_NAME_MAP[param->value.type],
                        param->name,
                        flag_value_str
                    );
                }
            }
        } else {
            // positional argument

            // bad input to begin with, but maybe user wanted help on a subcommand
            if (bad_input_flags_only) continue;

            // find first positional parameter that is not filled yet
            NYA_ArgParameter* param = nullptr;
            for (u32 param_index = 0; param_index < NYA_ARG_MAX_PARAMETERS; param_index++) {
                NYA_ArgParameter* candidate = command_to_execute->parameters[param_index];
                if (candidate == nullptr) break;
                if (candidate->kind == NYA_ARG_PARAMETER_KIND_POSITIONAL && !candidate->was_matched) {
                    param = candidate;
                    break;
                }
            }

            if (param != nullptr) {
                param->was_matched = true;
            } else {
                return nya_error(NYA_ERROR_NOT_OK, "unexpected positional argument: '%s'", argv[arg_index]);
            }

            // handle variadic
            if (param->variadic) {
                // consume all remaining args, upto the next --flag (if any)
                while (arg_index < argc) {
                    if (nya_string_starts_with(argv[arg_index], "--")) {
                        arg_index--;
                        break;
                    }

                    /*
                     * Bounded, because `values` is a fixed array and nothing above this stops argv
                     * from being longer than it. It was not, so the two hundred and fifty seventh
                     * value and everything after it landed past the end — and NYA_Value is a large
                     * struct, so it walked a long way into whatever followed the parameter.
                     *
                     * The input is argv, which makes it reachable from the command line of anything
                     * built on this parser, the build tool's own `run test <names...>` included.
                     *
                     * Refused rather than truncated: quietly dropping the excess is how a command
                     * asked to act on four hundred files acts on two hundred and fifty six of them
                     * and reports success.
                     */
                    if (param->values_count >= nya_carray_length(param->values)) {
                        return nya_error(
                            NYA_ERROR_NOT_OK,
                            "'%s' takes at most %zu values, and more than that were given",
                            param->name,
                            nya_carray_length(param->values)
                        );
                    }

                    NYA_Value* slot = &param->values[param->values_count];
                    slot->type      = param->value.type;
                    if (!_nya_args_parse_value(slot, argv[arg_index])) {
                        return nya_error(
                            NYA_ERROR_NOT_OK,
                            "failed to parse %s value for argument '%s': '%s'",
                            NYA_TYPE_NAME_MAP[param->value.type],
                            param->name,
                            argv[arg_index]
                        );
                    }

                    // increment
                    param->values_count++;
                    arg_index++;
                }
            } else {
                if (!_nya_args_parse_value(&param->value, argv[arg_index])) {
                    return nya_error(
                        NYA_ERROR_NOT_OK,
                        "failed to parse %s value for argument '%s': '%s'",
                        NYA_TYPE_NAME_MAP[param->value.type],
                        param->name,
                        argv[arg_index]
                    );
                }
            }
        }
    }

    // check if --help was passed (skip missing args check if so)
    b8 help_requested = false;
    for (u32 path_index = 0; path_index < path_count; path_index++) {
        NYA_ArgCommand* command_in_path = path[path_index];
        for (u32 param_index = 0; param_index < NYA_ARG_MAX_PARAMETERS; param_index++) {
            NYA_ArgParameter* param = command_in_path->parameters[param_index];
            if (param == nullptr) break;
            if (param->kind == NYA_ARG_PARAMETER_KIND_FLAG && nya_string_equals(param->name, "help") && param->was_matched) {
                help_requested = true;
                break;
            }
        }
        if (help_requested) break;
    }

    // recap: report missing and fill defaults
    for (u32 param_index = 0; param_index < NYA_ARG_MAX_PARAMETERS; param_index++) {
        NYA_ArgParameter* param = command_to_execute->parameters[param_index];
        if (param == nullptr) break;

        if (!param->was_matched) {
            if (param->default_value.type != NYA_TYPE_NULL) {
                param->value = param->default_value;
            } else if (param->kind == NYA_ARG_PARAMETER_KIND_FLAG && param->value.type == NYA_TYPE_B8) {
                // boolean flags default to false
                param->value.as_b8 = false;
            } else if (param->kind == NYA_ARG_PARAMETER_KIND_POSITIONAL && param->variadic) {
                // variadic positional arguments are optional by default (values_count stays 0)
            } else if (!help_requested) {
                return nya_error(
                    NYA_ERROR_NOT_OK,
                    "missing required parameter '%s' for command '%s'",
                    param->name,
                    command_to_execute->name ? command_to_execute->name : "root"
                );
            }
        }
    }

    *out_command = command_to_execute;
    return NYA_OK;
}

NYA_Error nya_args_run_command(NYA_ArgCommand* command) {
    nya_assert(command);

    if (command->incomplete) {
        return nya_error(NYA_ERROR_NOT_OK, "no subcommand provided for command '%s'", command->name ? command->name : "root");
    }

    if (command->handler != nullptr) {
        command->handler(command);
    } else if (command->build_rule != nullptr) {
        NYA_TRY(nya_build(command->build_rule));
    } else {
        nya_panic("Command '%s' has neither a handler nor a build rule.", command->name ? command->name : "root");
    }

    return NYA_OK;
}

void nya_args_print_usage(NYA_ArgParser* parser, NYA_ArgCommand* command_override) {
    _nya_args_validate_parser(parser);

    NYA_ArgCommand* command_to_use = command_override ? command_override : parser->root_command;

    // pre-pass
    u8 name_max_length  = 0;
    b8 subcommand_found = false;
    b8 positional_found = false;
    b8 flag_found       = false;
    for (u32 param_index = 0; param_index < NYA_ARG_MAX_PARAMETERS; param_index++) {
        NYA_ArgParameter* param = command_to_use->parameters[param_index];
        if (param == nullptr) break;

        if (param->kind == NYA_ARG_PARAMETER_KIND_POSITIONAL) {
            positional_found = true;
            u8 name_length   = nya_cast_to_u8(strlen(param->name));
            if (name_length > name_max_length) name_max_length = name_length;
        }

        if (param->kind == NYA_ARG_PARAMETER_KIND_FLAG) {
            flag_found     = true;
            u8 name_length = nya_cast_to_u8(strlen(param->name)) + 2; // +2 to account for the '--' prefix
            if (name_length > name_max_length) name_max_length = name_length;
        }
    }
    for (u32 subcommand_index = 0; subcommand_index < NYA_ARG_MAX_COMMANDS; subcommand_index++) {
        NYA_ArgCommand* subcommand = command_to_use->subcommands[subcommand_index];
        if (subcommand == nullptr) break;

        subcommand_found = true;
        u8 name_length   = nya_cast_to_u8(strlen(subcommand->name));
        if (name_length > name_max_length) name_max_length = name_length;
    }

    // print header
    if (command_override == nullptr || command_override->is_root) {
        printf("%s\n", parser->name);
        if (parser->version) printf("Version: %s\n", parser->version);
        if (parser->author) printf("Author: %s\n", parser->author);
        if (parser->description) printf("%s\n", parser->description);
    } else {
        printf("%s\n", command_override->description);
    }

    printf("\n");

    // assemble usage line
    if (subcommand_found) {
        printf("Usage: %s [command]\n", parser->executable_name);
    } else {
        printf("Usage: %s ", parser->executable_name);
        if (command_override) _nya_args_print_command_path(parser, command_override);

        for (u32 param_index = 0; param_index < NYA_ARG_MAX_PARAMETERS; param_index++) {
            NYA_ArgParameter* param = command_to_use->parameters[param_index];
            if (param == nullptr) break;

            if (param->kind == NYA_ARG_PARAMETER_KIND_POSITIONAL) {
                if (param->variadic) {
                    printf("<%s...> ", param->name);
                } else {
                    printf("<%s> ", param->name);
                }
            }
        }

        if (flag_found) printf("[flags]");

        printf("\n");
    }

    // print subcommand details
    if (subcommand_found) {
        printf("\nCommands:\n");
        for (u32 subcommand_index = 0; subcommand_index < NYA_ARG_MAX_COMMANDS; subcommand_index++) {
            NYA_ArgCommand* subcommand = command_to_use->subcommands[subcommand_index];
            if (subcommand == nullptr) break;

            printf(
                "  %-*s   %-*s %s\n",
                name_max_length,
                subcommand->name,
                _NYA_ARGS_DESCRIPTION_INDENT,
                "", // no type for subcommands
                subcommand->description ? subcommand->description : ""
            );
        }
    }

    // print argument details
    if (positional_found) {
        printf("\nArguments:\n");
        for (u32 param_index = 0; param_index < NYA_ARG_MAX_PARAMETERS; param_index++) {
            NYA_ArgParameter* param = command_to_use->parameters[param_index];
            if (param == nullptr) break;
            if (param->kind != NYA_ARG_PARAMETER_KIND_POSITIONAL) continue;

            printf(
                "  %-*s : %-*s %s\n",
                name_max_length,
                param->name,
                _NYA_ARGS_DESCRIPTION_INDENT,
                NYA_TYPE_NAME_MAP[param->value.type],
                param->description ? param->description : ""
            );
        }
    }

    // print flag details
    if (flag_found) {
        printf("\nFlags:\n");
        for (u32 param_index = 0; param_index < NYA_ARG_MAX_PARAMETERS; param_index++) {
            NYA_ArgParameter* param = command_to_use->parameters[param_index];
            if (param == nullptr) break;
            if (param->kind != NYA_ARG_PARAMETER_KIND_FLAG) continue;

            printf(
                "  --%-*s : %-*s %s",
                name_max_length - 2,
                param->name,
                _NYA_ARGS_DESCRIPTION_INDENT,
                NYA_TYPE_NAME_MAP[param->value.type],
                param->description ? param->description : ""
            );

            if (param->default_value.type != NYA_TYPE_NULL) {
                printf(" (default: ");
                switch (param->default_value.type) {
                    case NYA_TYPE_B8:     printf("%s", param->default_value.as_b8 ? "true" : "false"); break;
                    case NYA_TYPE_S64:    printf(FMTs64, param->default_value.as_s64); break;
                    case NYA_TYPE_F64:    printf(FMTf64, param->default_value.as_f64); break;
                    case NYA_TYPE_STRING: printf("%s", param->default_value.as_string); break;
                    default:              printf("unknown"); break;
                }
                printf(")");
            }

            printf("\n");
        }
    }
}

NYA_Error nya_args_print_completions(NYA_ArgParser* parser, NYA_ConstCString binary_name, NYA_ConstCString shell) {
    return nya_args_write_completions(parser, binary_name, shell, stdout);
}

NYA_Error nya_args_write_completions(NYA_ArgParser* parser, NYA_ConstCString binary_name, NYA_ConstCString shell, FILE* stream) {
    nya_assert(parser);
    nya_assert(binary_name);
    nya_assert(shell);
    nya_assert(stream);

    for (u32 shell_index = 0; shell_index < _NYA_ARGS_SHELL_COUNT; shell_index++) {
        if (!nya_string_equals(_NYA_ARGS_SHELLS[shell_index].name, shell)) continue;

        _nya_args_validate_parser(parser);
        _NYA_ARGS_SHELLS[shell_index].generate(parser, binary_name, stream);
        return NYA_OK;
    }

    // Built here rather than listed in the message literal, so a backend added to the table shows up
    // in the error without anyone remembering to also edit this string.
    char supported[_NYA_ARGS_NAME_MAX] = { 0 };
    u64  length                        = 0;
    for (u32 shell_index = 0; shell_index < _NYA_ARGS_SHELL_COUNT; shell_index++) {
        s32 written =
            snprintf(&supported[length], sizeof(supported) - length, "%s%s", shell_index == 0 ? "" : ", ", _NYA_ARGS_SHELLS[shell_index].name);
        if (written <= 0) break;
        length += (u64)written;
        if (length >= sizeof(supported)) break;
    }

    return nya_error(NYA_ERROR_NOT_OK, "unknown shell '%s'. Supported: %s", shell, supported);
}

NYA_ConstCString nya_args_completion_shell_name(u32 index) {
    if (index >= _NYA_ARGS_SHELL_COUNT) return nullptr;
    return _NYA_ARGS_SHELLS[index].name;
}

void nya_args_walk_commands(NYA_ArgParser* parser, NYA_ArgCommandVisitFn visit_fn, void* userdata) {
    nya_assert(parser);
    nya_assert(visit_fn);

    NYA_ArgCommandVisit visit = { .userdata = userdata };
    _nya_args_walk_command(parser->root_command, &visit, visit_fn);
}

void nya_args_command_path_join(
    const NYA_ArgCommandVisit* visit,
    NYA_ConstCString           prefix,
    NYA_ConstCString           separator,
    OUT char*                  buffer,
    u64                        buffer_size
) {
    nya_assert(visit);
    nya_assert(buffer);
    nya_assert(buffer_size > 0);

    u64 length = 0;
    buffer[0]  = '\0';

    if (prefix != nullptr) {
        s32 written = snprintf(buffer, buffer_size, "%s", prefix);
        if (written > 0) length = (u64)written;
    }

    for (u32 path_index = 0; path_index < visit->path_count; path_index++) {
        NYA_ConstCString name = visit->path[path_index]->name;
        if (name == nullptr) continue; // the root, which is only ever the prefix
        if (length >= buffer_size) break;

        s32 written = snprintf(&buffer[length], buffer_size - length, "%s%s", length == 0 ? "" : separator, name);
        if (written <= 0) break;
        length += (u64)written;
    }
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

void _nya_args_walk_command(NYA_ArgCommand* command, NYA_ArgCommandVisit* visit, NYA_ArgCommandVisitFn visit_fn) {
    nya_assert(command);
    nya_assert(visit->path_count < NYA_ARG_MAX_COMMANDS, "Command tree is deeper than NYA_ARG_MAX_COMMANDS.");

    visit->command                 = command;
    visit->path[visit->path_count] = command;
    visit->path_count++;

    // Flags are matched against the whole command path by nya_args_parse, so a subcommand is in
    // scope of its parents' flags as well as its own. Recorded once here so no consumer has to
    // rediscover it, and restored on the way back out.
    u32 inherited_count = visit->flag_count;
    for (u32 param_index = 0; param_index < NYA_ARG_MAX_PARAMETERS; param_index++) {
        NYA_ArgParameter* param = command->parameters[param_index];
        if (param == nullptr) break;
        if (param->kind != NYA_ARG_PARAMETER_KIND_FLAG) continue;

        nya_assert(visit->flag_count < NYA_ARG_MAX_PARAMETERS, "Too many flags in scope for command '%s'.", command->name ? command->name : "root");
        visit->flags[visit->flag_count] = param;
        visit->flag_count++;
    }

    visit_fn(visit);

    for (u32 subcommand_index = 0; subcommand_index < NYA_ARG_MAX_COMMANDS; subcommand_index++) {
        NYA_ArgCommand* subcommand = command->subcommands[subcommand_index];
        if (subcommand == nullptr) break;

        _nya_args_walk_command(subcommand, visit, visit_fn);

        // The visitor may have moved on, but the walk has not: everything below this node was
        // reached through it.
        visit->command = command;
    }

    visit->flag_count = inherited_count;
    visit->path_count--;
}

/*
 * ─────────────────────────────────────────────────────────
 * ZSH BACKEND
 * ─────────────────────────────────────────────────────────
 */

/**
 * Writes `text` as the inside of a zsh single quoted word.
 *
 * `escape_brackets` is the difference between the two places a description can land: _arguments
 * parses its specs, so a bracket or a colon in there has to be escaped or it terminates the
 * description early, while a _describe entry is plain text past the first colon and would show the
 * backslashes verbatim.
 * */
NYA_INTERNAL void _nya_args_zsh_print_escaped(FILE* stream, NYA_ConstCString text, b8 escape_brackets) {
    if (text == nullptr) return;

    for (NYA_ConstCString cursor = text; *cursor != '\0'; cursor++) {
        switch (*cursor) {
            // Close the word, hand zsh one escaped quote, reopen it. The only way a single quote
            // gets into a single quoted shell word.
            case '\'': fprintf(stream, "'\\''"); break;
            case ':':  fprintf(stream, "\\:"); break;
            case '[':
            case ']':
                if (escape_brackets) fprintf(stream, "\\");
                fprintf(stream, "%c", *cursor);
                break;
            // A description is one line in the completion listing, so anything that would break it
            // into two becomes a space.
            case '\n':
            case '\r': fprintf(stream, " "); break;
            default:   fprintf(stream, "%c", *cursor); break;
        }
    }
}

/** Writes `text` as a double quoted word nested inside the single quoted spec, for paths and globs. */
NYA_INTERNAL void _nya_args_zsh_print_quoted(FILE* stream, NYA_ConstCString text) {
    fprintf(stream, "\"");
    for (NYA_ConstCString cursor = text; *cursor != '\0'; cursor++) {
        // A glob's own metacharacters have to survive, so only what the shell would act on inside
        // double quotes is escaped. ${PWD} below is deliberately left expandable.
        if (*cursor == '"' || *cursor == '\\' || *cursor == '`') fprintf(stream, "\\");
        fprintf(stream, "%c", *cursor);
    }
    fprintf(stream, "\"");
}

/** Translates the shell independent completion descriptor into the action half of an _arguments spec. */
NYA_INTERNAL void _nya_args_zsh_print_action(FILE* stream, const NYA_ArgParameter* param) {
    NYA_ArgCompletionKind kind = param->completion.kind;

    // A string positional is a path often enough to be worth guessing at; a number never is.
    if (kind == NYA_ARG_COMPLETION_KIND_DEFAULT)
        kind = param->value.type == NYA_TYPE_STRING ? NYA_ARG_COMPLETION_KIND_FILE : NYA_ARG_COMPLETION_KIND_NONE;

    switch (kind) {
        case NYA_ARG_COMPLETION_KIND_FILE:
        case NYA_ARG_COMPLETION_KIND_DIRECTORY: {
            fprintf(stream, "_files");
            if (kind == NYA_ARG_COMPLETION_KIND_DIRECTORY) fprintf(stream, " -/");

            if (param->completion.directory != nullptr) {
                fprintf(stream, " -W ");
                // _files takes -W as an absolute path. Given a relative one it silently completes
                // from the filesystem root instead, which looks like the completion simply not
                // working, so the working directory is spliced in by the shell at completion time.
                b8 is_absolute = param->completion.directory[0] == '/';
                if (!is_absolute) {
                    fprintf(stream, "\"${PWD}/");
                    for (NYA_ConstCString cursor = param->completion.directory; *cursor != '\0'; cursor++) {
                        if (*cursor == '"' || *cursor == '\\' || *cursor == '`') fprintf(stream, "\\");
                        fprintf(stream, "%c", *cursor);
                    }
                    fprintf(stream, "\"");
                } else {
                    _nya_args_zsh_print_quoted(stream, param->completion.directory);
                }
            }

            if (kind == NYA_ARG_COMPLETION_KIND_FILE && param->completion.glob != nullptr) {
                fprintf(stream, " -g ");
                _nya_args_zsh_print_quoted(stream, param->completion.glob);
            }
        } break;

        case NYA_ARG_COMPLETION_KIND_CHOICES: {
            fprintf(stream, "(");
            for (u32 choice_index = 0; choice_index < NYA_ARG_MAX_CHOICES; choice_index++) {
                NYA_ConstCString choice =
                    param->completion.choices_fn != nullptr ? param->completion.choices_fn(choice_index) : param->completion.choices[choice_index];
                if (choice == nullptr) break;

                if (choice_index > 0) fprintf(stream, " ");
                for (NYA_ConstCString cursor = choice; *cursor != '\0'; cursor++) {
                    if (*cursor == ' ' || *cursor == '(' || *cursor == ')' || *cursor == '\'') fprintf(stream, "\\");
                    fprintf(stream, "%c", *cursor);
                }
            }
            fprintf(stream, ")");
        } break;

        // An empty action still tells _arguments the argument exists and takes a word, which is all
        // that is known about it.
        case NYA_ARG_COMPLETION_KIND_NONE:
        default:                           break;
    }
}

/** What the walker carries for the zsh backend, since a visitor gets one userdata pointer. */
typedef struct _NYA_ArgZshContext {
    FILE*            stream;
    NYA_ConstCString root_function;
} _NYA_ArgZshContext;

/** Emits one completion function, named after the command's path. Called once per command by the walker. */
NYA_INTERNAL void _nya_args_zsh_print_command(const NYA_ArgCommandVisit* visit) {
    _NYA_ArgZshContext* context         = (_NYA_ArgZshContext*)visit->userdata;
    FILE*               stream          = context->stream;
    NYA_ArgCommand*     command         = visit->command;
    b8                  has_subcommands = command->subcommands[0] != nullptr;

    char function_name[_NYA_ARGS_NAME_MAX];
    nya_args_command_path_join(visit, context->root_function, "_", function_name, sizeof(function_name));

    fprintf(stream, "%s() {\n", function_name);
    fprintf(stream, "    local curcontext=\"$curcontext\" state line ret=1\n");
    fprintf(stream, "    typeset -A opt_args\n\n");
    fprintf(stream, "    _arguments -C \\\n");

    for (u32 flag_index = 0; flag_index < visit->flag_count; flag_index++) {
        NYA_ArgParameter* flag = visit->flags[flag_index];

        fprintf(stream, "        '--%s[", flag->name);
        _nya_args_zsh_print_escaped(stream, flag->description, true);
        fprintf(stream, "]");

        // A boolean flag may be written bare, everything else consumes the next word.
        if (flag->value.type != NYA_TYPE_B8) {
            fprintf(stream, ":%s:", flag->name);
            _nya_args_zsh_print_action(stream, flag);
        }

        fprintf(stream, "' \\\n");
    }

    if (has_subcommands) {
        fprintf(stream, "        '1: :->command' \\\n");
        fprintf(stream, "        '*:: :->argument' \\\n");
    } else {
        for (u32 param_index = 0; param_index < NYA_ARG_MAX_PARAMETERS; param_index++) {
            NYA_ArgParameter* param = command->parameters[param_index];
            if (param == nullptr) break;
            if (param->kind != NYA_ARG_PARAMETER_KIND_POSITIONAL) continue;

            fprintf(stream, "        '%s:%s:", param->variadic ? "*" : "", param->name);
            _nya_args_zsh_print_action(stream, param);
            fprintf(stream, "' \\\n");
        }
    }

    fprintf(stream, "        && ret=0\n");

    if (has_subcommands) {
        fprintf(stream, "\n    case $state in\n");
        fprintf(stream, "        command)\n");
        fprintf(stream, "            local -a commands\n");
        fprintf(stream, "            commands=(\n");

        for (u32 subcommand_index = 0; subcommand_index < NYA_ARG_MAX_COMMANDS; subcommand_index++) {
            NYA_ArgCommand* subcommand = command->subcommands[subcommand_index];
            if (subcommand == nullptr) break;

            fprintf(stream, "                '%s:", subcommand->name);
            _nya_args_zsh_print_escaped(stream, subcommand->description, false);
            fprintf(stream, "'\n");
        }

        fprintf(stream, "            )\n");
        fprintf(stream, "            _describe -t commands '%s' commands && ret=0\n", command->name ? command->name : "command");
        fprintf(stream, "            ;;\n");
        fprintf(stream, "        argument)\n");
        fprintf(stream, "            case $line[1] in\n");

        for (u32 subcommand_index = 0; subcommand_index < NYA_ARG_MAX_COMMANDS; subcommand_index++) {
            NYA_ArgCommand* subcommand = command->subcommands[subcommand_index];
            if (subcommand == nullptr) break;

            fprintf(stream, "                %s) %s_%s && ret=0 ;;\n", subcommand->name, function_name, subcommand->name);
        }

        fprintf(stream, "            esac\n");
        fprintf(stream, "            ;;\n");
        fprintf(stream, "    esac\n");
    }

    fprintf(stream, "\n    return ret\n");
    fprintf(stream, "}\n\n");
}

void _nya_args_zsh_generate(NYA_ArgParser* parser, NYA_ConstCString binary_name, FILE* stream) {
    fprintf(stream, "#compdef %s\n", binary_name);
    fprintf(stream, "# Generated by %s", parser->name);
    if (parser->version) fprintf(stream, " %s", parser->version);
    fprintf(stream, ". Regenerate with '%s completions zsh', do not edit by hand.\n\n", binary_name);

    char root_function[_NYA_ARGS_NAME_MAX];
    (void)snprintf(root_function, sizeof(root_function), "_%s", binary_name);

    _NYA_ArgZshContext context = { .stream = stream, .root_function = root_function };
    nya_args_walk_commands(parser, &_nya_args_zsh_print_command, &context);

    // Autoloaded #compdef files define their function and then run it, because compinit sources the
    // file in place of the call it was standing in for.
    fprintf(stream, "%s \"$@\"\n", root_function);
}

b8 _nya_args_parse_value(NYA_Value* value, NYA_CString str) {
    nya_assert(value);
    nya_assert(str);

    u64 length = strlen(str);

    switch (value->type) {
        case NYA_TYPE_B8:     return nya_type_parse(NYA_TYPE_B8, (const u8*)str, length, &value->as_b8);
        case NYA_TYPE_S64:    return nya_type_parse(NYA_TYPE_S64, (const u8*)str, length, &value->as_s64);
        case NYA_TYPE_F64:    return nya_type_parse(NYA_TYPE_F64, (const u8*)str, length, &value->as_f64);
        case NYA_TYPE_STRING: value->as_string = str; return true;
        default:              nya_unreachable();
    }
}

void _nya_args_validate_parser(NYA_ArgParser* parser) {
    nya_assert(parser);

    nya_assert(parser->name);
    _nya_args_validate_command_tree(parser->root_command);
}

void _nya_args_validate_command_tree(NYA_ArgCommand* command) {
    nya_assert(command);

    // validate command properties
    if (command->is_root) {
        nya_assert(command->name == nullptr, "Root command should not have a name.");
        nya_assert(command->description == nullptr, "Root command should not have a description.");
    } else {
        nya_assert(command->name, "Non-root command must have a name.");
    }

    // validate parameters
    b8 has_positionals = false;
    b8 variadic_found  = false;
    for (u32 param_index = 0; param_index < NYA_ARG_MAX_PARAMETERS; param_index++) {
        NYA_ArgParameter* param = command->parameters[param_index];
        if (param == nullptr) break;

        nya_assert(param->name);
        if (param->value.type != NYA_TYPE_B8 && param->value.type != NYA_TYPE_S64 && param->value.type != NYA_TYPE_F64 &&
            param->value.type != NYA_TYPE_STRING) {
            nya_panic("Parameter '%s' in command '%s' has invalid type. Has to be B8, S64, F64 or STRING.", param->name, command->name);
        }

        if (param->kind == NYA_ARG_PARAMETER_KIND_POSITIONAL) {
            nya_assert(command->subcommands[0] == nullptr, "Can either have positional arguments or subcommands, not both.");
            nya_assert(param->default_value.type == NYA_TYPE_NULL, "Only flag parameters can have default values.");

            has_positionals = true;

            if (variadic_found) {
                nya_panic(
                    "Only the last positional argument in command '%s' can be variadic. "
                    "Parameter '%s' is variadic but is not the last positional argument.",
                    command->name ? command->name : "root",
                    param->name
                );
            } else {
                if (param->variadic) variadic_found = true;
            }
        }

        if (param->kind == NYA_ARG_PARAMETER_KIND_FLAG) {
            nya_assert(!param->variadic, "Flag cannot be variadic.");

            if (param->default_value.type != NYA_TYPE_NULL && param->default_value.type != param->value.type) {
                nya_panic(
                    "Parameter '%s' in command '%s' has mismatched default value type. Expected %s but got %s.",
                    param->name,
                    command->name,
                    NYA_TYPE_NAME_MAP[param->value.type],
                    NYA_TYPE_NAME_MAP[param->default_value.type]
                );
            }
        }
    }

    // validate subcommands
    for (u32 subcommand_index = 0; subcommand_index < NYA_ARG_MAX_COMMANDS; subcommand_index++) {
        NYA_ArgCommand* subcommand = command->subcommands[subcommand_index];
        if (subcommand == nullptr) break;

        if (has_positionals) {
            nya_panic("Command '%s' cannot have both positional parameters and subcommands.", command->name ? command->name : "root");
        }

        _nya_args_validate_command_tree(subcommand);
    }

    if (has_positionals) {
        nya_assert(
            command->handler != nullptr || command->build_rule != nullptr,
            "Command '%s' must have either a handler or a build rule associated to it.",
            command->name ? command->name : "root"
        );
    }
}

void _nya_args_print_command_path(NYA_ArgParser* parser, NYA_ArgCommand* command) {
    static NYA_ArgCommand* path[NYA_ARG_MAX_COMMANDS];
    static u32             path_length = 0;
    static b8              found       = false;

    b8 is_top_level = (path_length == 0);
    if (is_top_level) path[path_length++] = parser->root_command;

    for (u32 subcommand_index = 0; subcommand_index < NYA_ARG_MAX_COMMANDS; subcommand_index++) {
        NYA_ArgCommand* subcommand = path[path_length - 1]->subcommands[subcommand_index];
        if (subcommand == nullptr) continue;

        path[path_length++] = subcommand;

        if (subcommand == command) {
            found = true;
            for (u32 path_index = 0; path_index < path_length; path_index++) {
                if (path[path_index]->name) printf("%s ", path[path_index]->name);
            }
            break;
        }

        _nya_args_print_command_path(parser, command);
        if (found) break;
        path_length--;
    }

    if (is_top_level) {
        if (!found) nya_panic("Command '%s' not found in parser '%s'.", command->name, parser->name);
        path_length = 0;
        found       = false;
    }
}
