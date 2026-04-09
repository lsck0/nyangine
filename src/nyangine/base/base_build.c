#include "nyangine/nyangine.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API DECLARATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

#define _NYA_BUILD_MAX_BUILD_DEPTH 64

NYA_INTERNAL NYA_Result _nya_build_always(NYA_BuildRule* build_rule);

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PUBLIC API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

NYA_Result nya_build(NYA_BuildRule* build_rule) {
    nya_assert(build_rule != nullptr);

    switch (build_rule->policy) {
        case NYA_BUILD_ALWAYS: return _nya_build_always(build_rule);

        case NYA_BUILD_ONCE:   {
            nya_assert(build_rule->output_file, "NYA_BUILD_ONCE rules must specify an output_file.");
            if (nya_filesystem_exists(build_rule->output_file)) return NYA_OK;
            return _nya_build_always(build_rule);
        }

        case NYA_BUILD_IF_OUTDATED: {
            nya_assert(build_rule->input_file, "NYA_BUILD_IF_OUTDATED rules must specify an input_file.");
            nya_assert(build_rule->output_file, "NYA_BUILD_IF_OUTDATED rules must specify an output_file.");

            if (!nya_filesystem_exists(build_rule->output_file)) return _nya_build_always(build_rule);

            u64 input_mod_time  = 0;
            u64 output_mod_time = 0;
            NYA_EXPECT(nya_filesystem_last_modified(build_rule->input_file, &input_mod_time));
            NYA_EXPECT(nya_filesystem_last_modified(build_rule->output_file, &output_mod_time));

            if (input_mod_time > output_mod_time) return _nya_build_always(build_rule);

            return NYA_OK;
        }

        default: nya_unreachable();
    }
    static_assert(NYA_BUILD_COUNT == 3, "Unhandled NYA_BuildRulePolicy enum value.");

    nya_unreachable();
}

void nya_rebuild_yourself(s32* argc, NYA_CString* argv, NYA_Command cmd) {
    NYA_CString marker = "--no-rebuild"; // appended to argv

    // check if we've already rebuilt ourselves
    if (nya_string_equals(argv[*argc - 1], marker)) {
        *argc -= 1;
        return;
    }

    NYA_BuildRule rule = {
        .name    = "Rebuild Build System",
        .policy  = NYA_BUILD_ALWAYS,
        .command = cmd,
    };

    // backup, build, restore

    // need to not only copy backup, but also permissions and i cant be bothered
    NYA_Command copy_command = {
        .program   = "cp",
        .arguments = { argv[0], ".backup_build_executable" },
    };
    NYA_EXPECT(nya_command_run(&copy_command));
    nya_assert(copy_command.exit_code == 0, "Failed to backup build executable before rebuild.");

    NYA_Result build_result = nya_build(&rule);
    if (build_result.error != NYA_ERROR_NONE) {
        NYA_EXPECT(nya_filesystem_move(".backup_build_executable", argv[0]));
        exit(1);
    }

    NYA_EXPECT(nya_filesystem_delete(".backup_build_executable"));

    // build new argv with marker
    NYA_CString* new_argv = nya_alloca((*argc + 2) * sizeof(NYA_CString));
    nya_memcpy(new_argv, argv, (*argc) * sizeof(NYA_CString));
    new_argv[*argc]      = marker;
    new_argv[*argc + 1]  = nullptr;
    *argc               += 1;

    // replace process with the new binary
    execvp(argv[0], (char* const*)new_argv);
    perror("execvp");
    exit(EXIT_FAILURE);
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

NYA_Result _nya_build_always(NYA_BuildRule* build_rule) {
    static u64 depth = 0;

    nya_assert(build_rule != nullptr);

    // build dependencies first
    for (u64 i = 0; i < NYA_BUILD_MAX_DEPENDENCIES; i++) {
        nya_assert(depth <= _NYA_BUILD_MAX_BUILD_DEPTH, "Maximum build depth exceeded (possible circular dependency).");

        NYA_BuildRule* dependency = build_rule->dependencies[i];
        if (!dependency) break;

        depth++;
        NYA_Result result = nya_build(dependency);
        depth--;

        if (result.error != NYA_ERROR_NONE) return result;
    }

    // pre-build hooks
    for (u64 i = 0; i < NYA_BUILD_MAX_DEPENDENCIES; i++) {
        void (*hook)(NYA_BuildRule* rule) = build_rule->pre_build_hooks[i];
        if (!hook) break;
        hook(build_rule);
    }

    // running the command
    b8 ok = true;
    if (build_rule->is_metarule) {
        printf("[BUILDING META] %s \n\n", build_rule->name);

        goto skip_build;
    }

    printf("[BUILDING] %s \n", build_rule->name);
    printf("[CMD] %s ", build_rule->command.program);
    for (u64 i = 0; i < NYA_COMMAND_MAX_ARGUMENTS; i++) {
        NYA_ConstCString arg = build_rule->command.arguments[i];
        if (!arg) break;
        printf("%s ", arg);
    }
    printf("\n");

    NYA_EXPECT(nya_command_run(&build_rule->command));
    ok = build_rule->command.exit_code == 0;
    if (ok) {
        printf("[OK] Took " FMTu64 " ms.\n\n", build_rule->command.execution_time_ms);
    } else {
        printf("[FAILED] Exit code: %d\n", build_rule->command.exit_code);
        NYA_String  empty_str       = { .length = 0, .items = (u8*)"" };
        NYA_String* stdout_to_print = build_rule->command.stdout_content ? build_rule->command.stdout_content : &empty_str;
        printf("------- STDOUT -------\n" NYA_FMT_STRING "\n", NYA_FMT_STRING_ARG(stdout_to_print));
        NYA_String* stderr_to_print = build_rule->command.stderr_content ? build_rule->command.stderr_content : &empty_str;
        printf("------- STDERR -------\n" NYA_FMT_STRING "\n", NYA_FMT_STRING_ARG(stderr_to_print));
    }

skip_build:

    // post-build hooks
    for (u64 i = 0; i < NYA_BUILD_MAX_DEPENDENCIES; i++) {
        void (*hook)(NYA_BuildRule* rule) = build_rule->post_build_hooks[i];
        if (!hook) break;
        hook(build_rule);
    }

    return ok ? NYA_OK : nya_err(NYA_ERROR_GENERIC, "Build rule '%s' failed with exit code %d.", build_rule->name, build_rule->command.exit_code);
}
