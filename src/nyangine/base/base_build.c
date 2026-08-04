#include "nyangine/nyangine.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API DECLARATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

#define _NYA_BUILD_MAX_BUILD_DEPTH 64

/**
 * Which build we are in. Incremented once per top level nya_build, never per dependency.
 *
 * Starts at 1 so that a rule's zeroed last_built_epoch cannot be mistaken for "already built".
 * */
NYA_INTERNAL u64 _nya_build_epoch = 1;
NYA_INTERNAL u64 _nya_build_depth = 0;

NYA_INTERNAL NYA_Error _nya_build_dispatch(NYA_BuildRule* build_rule);
NYA_INTERNAL NYA_Error _nya_build_always(NYA_BuildRule* build_rule);
NYA_INTERNAL NYA_Error _nya_build_run(NYA_BuildRule* build_rule);
NYA_INTERNAL u32       _nya_build_argument_count(const NYA_Command* command);
NYA_INTERNAL u32       _nya_build_append_flags(NYA_BuildRule* build_rule, u32 at, NYA_ConstCString const* flags);
NYA_INTERNAL u32       _nya_build_apply_vendors(NYA_BuildRule* build_rule);

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PUBLIC API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

NYA_Error nya_build(NYA_BuildRule* build_rule) {
    nya_assert(build_rule != nullptr);

    // A new top level build starts a new epoch. Nested calls share it, which is what makes the memo
    // below scoped to one invocation rather than to the lifetime of the process — NYA_BUILD_ALWAYS
    // still means always, just not twice for the same graph walk.
    if (_nya_build_depth == 0) _nya_build_epoch++;

    if (build_rule->last_built_epoch == _nya_build_epoch) return NYA_OK;

    _nya_build_depth++;
    NYA_Error result = _nya_build_dispatch(build_rule);
    _nya_build_depth--;

    // Only successes are remembered. A failure aborts the whole build anyway, and memoizing one
    // would mean a retry silently skipped the rule that failed.
    if (result.kind == NYA_ERROR_NONE) build_rule->last_built_epoch = _nya_build_epoch;

    return result;
}

NYA_INTERNAL NYA_Error _nya_build_dispatch(NYA_BuildRule* build_rule) {
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
            NYA_TRY(nya_filesystem_last_modified(build_rule->input_file, &input_mod_time));
            NYA_TRY(nya_filesystem_last_modified(build_rule->output_file, &output_mod_time));

            if (input_mod_time > output_mod_time) return _nya_build_always(build_rule);

            return NYA_OK;
        }

        default: nya_unreachable();
    }
    static_assert(NYA_BUILD_COUNT == 3, "Unhandled NYA_BuildRulePolicy enum value.");

    nya_unreachable();
}

NYA_Error nya_vendor_build(NYA_VendorRule* vendor) {
    nya_assert(vendor != nullptr);

    for (u32 i = 0; i < NYA_VENDOR_MAX_PARTS; i++) {
        NYA_BuildRule* part = vendor->parts[i];
        if (!part) break;

        NYA_TRY(nya_build(part));
    }

    return NYA_OK;
}

NYA_Error nya_vendor_build_all(NYA_VendorRule** vendors) {
    nya_assert(vendors != nullptr);

    for (u32 i = 0; vendors[i] != nullptr; i++) { NYA_TRY(nya_vendor_build(vendors[i])); }

    return NYA_OK;
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

    // The backup path carries the pid. A fixed name is shared state between every concurrently
    // running copy of this tool, and two invocations, a CI job beside a local one or simply two
    // terminals, would then delete each other's backup and abort on the way out.
    char backup_path[64];
    (void)snprintf(backup_path, sizeof(backup_path), ".backup_build_executable.%d", (int)getpid());

    // backup, build, restore
    NYA_EXPECT(nya_filesystem_copy(argv[0], backup_path), "while backing up the build executable");

    NYA_Error build_result = nya_build(&rule);
    if (build_result.kind != NYA_ERROR_NONE) {
        NYA_EXPECT(nya_filesystem_move(backup_path, argv[0]), "while restoring the build executable after a failed rebuild");
        exit(1);
    }

    // Best effort: this is cleanup of our own temporary, and the desired end state is "the file is
    // not there". Aborting the whole tool because it already is not there would turn a successful
    // rebuild into a failure.
    (void)nya_filesystem_delete(backup_path);

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

NYA_Error _nya_build_always(NYA_BuildRule* build_rule) {
    nya_assert(build_rule != nullptr);

    // Vendors are not built here. They are all built up front by nya_vendor_build_all, so by the
    // time any rule runs every dependency already exists. Listing a vendor on a rule only asks for
    // its flags.

    // build dependencies first
    // nya_build maintains the depth now, so the guard reads the same counter the memo does rather
    // than a second one that only this function knew about.
    nya_assert(_nya_build_depth <= _NYA_BUILD_MAX_BUILD_DEPTH, "Maximum build depth exceeded (possible circular dependency).");

    for (u64 i = 0; i < NYA_BUILD_MAX_DEPENDENCIES; i++) {
        NYA_BuildRule* dependency = build_rule->dependencies[i];
        if (!dependency) break;

        NYA_TRY(nya_build(dependency));
    }

    // pre-build hooks
    for (u64 i = 0; i < NYA_BUILD_MAX_DEPENDENCIES; i++) {
        void (*hook)(NYA_BuildRule* rule) = build_rule->pre_build_hooks[i];
        if (!hook) break;
        hook(build_rule);
    }

    // Splice in the vendor flags. Done after the pre-build hooks so that a hook appending its own
    // arguments cannot land after the linker flags, and recorded so it can be undone below.
    u32 arguments_before_vendors = _nya_build_apply_vendors(build_rule);

    // One call, so there is no path around the cleanup that follows. The command logic lives in its
    // own function precisely so it can return early as often as it likes without anyone having to
    // remember that this function has unwinding left to do.
    NYA_Error result = _nya_build_run(build_rule);

    // Undo the splice so a rule built more than once, NYA_BUILD_ALWAYS inside a loop for instance,
    // does not accumulate the same vendor flags over and over.
    build_rule->command.arguments[arguments_before_vendors] = nullptr;

    // Post-build hooks, but only if there is a build to post-process.
    //
    // They exist to do something *to the artifact*: stamp a hash into it, move it, bundle it. When
    // the command failed there is no artifact, so running them turns one clear error into two, and
    // the second one is louder and points somewhere else entirely — an integrity hook reporting
    // "no such file" buries the linker error that is the actual problem.
    //
    // Note this is deliberately after the un-splice above, which is cleanup and must happen either
    // way.
    if (result.kind != NYA_ERROR_NONE) return result;

    for (u64 i = 0; i < NYA_BUILD_MAX_DEPENDENCIES; i++) {
        void (*hook)(NYA_BuildRule* rule) = build_rule->post_build_hooks[i];
        if (!hook) break;
        hook(build_rule);
    }

    return result;
}

/**
 * Runs a rule's command and reports what happened.
 *
 * Split out from _nya_build_always so that the caller's cleanup, undoing the vendor flag splice and
 * running the post-build hooks, cannot be jumped over. This used to be inline behind a `goto
 * skip_build`, which meant any NYA_TRY added here would silently skip the un-splice and leave the
 * rule accumulating duplicate flags on every subsequent build.
 * */
NYA_INTERNAL NYA_Error _nya_build_run(NYA_BuildRule* build_rule) {
    if (build_rule->is_metarule) {
        printf("[BUILDING META] %s \n\n", build_rule->name);
        return NYA_OK;
    }

    printf("[BUILDING] %s \n", build_rule->name);
    printf("[CMD] %s ", build_rule->command.program);
    for (u64 i = 0; i < NYA_COMMAND_MAX_ARGUMENTS; i++) {
        NYA_ConstCString arg = build_rule->command.arguments[i];
        if (!arg) break;
        printf("%s ", arg);
    }
    printf("\n");

    NYA_TRY(nya_command_run(&build_rule->command));

    if (build_rule->command.exit_code == 0) {
        printf("[OK] Took " FMTu64 " ms.\n\n", build_rule->command.execution_time_ms);
        return NYA_OK;
    }

    printf("[FAILED] Exit code: %d\n", build_rule->command.exit_code);
    NYA_String  empty_str       = { .length = 0, .items = (u8*)"" };
    NYA_String* stdout_to_print = build_rule->command.stdout_content ? build_rule->command.stdout_content : &empty_str;
    printf("------- STDOUT -------\n" NYA_FMT_STRING "\n", NYA_FMT_STRING_ARG(stdout_to_print));
    NYA_String* stderr_to_print = build_rule->command.stderr_content ? build_rule->command.stderr_content : &empty_str;
    printf("------- STDERR -------\n" NYA_FMT_STRING "\n", NYA_FMT_STRING_ARG(stderr_to_print));

    return nya_error(NYA_ERROR_NOT_OK, "Build rule '%s' failed with exit code %d.", build_rule->name, build_rule->command.exit_code);
}

NYA_INTERNAL u32 _nya_build_argument_count(const NYA_Command* command) {
    u32 count = 0;
    while (count < NYA_COMMAND_MAX_ARGUMENTS && command->arguments[count] != nullptr) count++;

    return count;
}

/** Appends a null terminated flag array at `at`, returning the new argument count. */
NYA_INTERNAL u32 _nya_build_append_flags(NYA_BuildRule* build_rule, u32 at, NYA_ConstCString const* flags) {
    for (u32 i = 0; i < NYA_VENDOR_MAX_FLAGS; i++) {
        if (!flags[i]) break;

        nya_assert(at < NYA_COMMAND_MAX_ARGUMENTS, "Rule '%s' exceeded NYA_COMMAND_MAX_ARGUMENTS while applying vendor flags.", build_rule->name);
        build_rule->command.arguments[at++] = flags[i];
    }

    return at;
}

/**
 * Appends every listed vendor's includes, cflags and linker flags to the command, and returns the
 * argument count from before, so the caller can restore it afterwards.
 *
 * Includes and cflags for all vendors go first, then the linker flags for all vendors. Archives
 * and -l flags have to sit after the sources they resolve symbols for, and grouping them keeps the
 * order predictable when one vendor needs symbols from another.
 * */
NYA_INTERNAL u32 _nya_build_apply_vendors(NYA_BuildRule* build_rule) {
    u32 original_count = _nya_build_argument_count(&build_rule->command);
    if (build_rule->is_metarule || build_rule->vendors[0] == nullptr) return original_count;

    u32 count = original_count;

    for (u32 i = 0; i < NYA_BUILD_MAX_VENDORS; i++) {
        NYA_VendorRule* vendor = build_rule->vendors[i];
        if (!vendor) break;

        count = _nya_build_append_flags(build_rule, count, vendor->includes);
        count = _nya_build_append_flags(build_rule, count, vendor->cflags);
    }

    for (u32 i = 0; i < NYA_BUILD_MAX_VENDORS; i++) {
        NYA_VendorRule* vendor = build_rule->vendors[i];
        if (!vendor) break;

        count = _nya_build_append_flags(build_rule, count, vendor->linker_flags);
    }

    nya_assert(count < NYA_COMMAND_MAX_ARGUMENTS, "Rule '%s' has no room left to null terminate its arguments.", build_rule->name);
    build_rule->command.arguments[count] = nullptr;

    return original_count;
}
