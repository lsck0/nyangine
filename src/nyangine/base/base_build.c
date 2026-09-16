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
NYA_INTERNAL void      _nya_build_report(NYA_BuildRule* build_rule);
NYA_INTERNAL void      _nya_build_finish_parallel(NYA_BuildRule* build_rule, NYA_Error wait_result, NYA_Error* result);

/**
 * Longest the parallel pool sleeps between checks when nothing it runs has finished. Only bounds how
 * late an exit is noticed; output wakes it sooner where the host can wait on pipes.
 * */
#define _NYA_BUILD_POLL_INTERVAL_MS 10

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PUBLIC API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

NYA_Error nya_build(NYA_BuildRule* build_rule) {
    nya_assert(build_rule != nullptr);

    // a new top level build starts a new epoch and nested calls share it, so the memo below lasts one
    // invocation. NYA_BUILD_ALWAYS still runs once per graph walk.
    if (_nya_build_depth == 0) _nya_build_epoch++;

    if (build_rule->last_built_epoch == _nya_build_epoch) return NYA_OK;

    _nya_build_depth++;
    NYA_Error result = _nya_build_dispatch(build_rule);
    _nya_build_depth--;

    // Only successes are remembered. A failure aborts the whole build anyway, and memoizing one
    // would mean a retry silently skipped the rule that failed.
    if (result.ok) build_rule->last_built_epoch = _nya_build_epoch;

    return result;
}

NYA_Error nya_build_parallel(NYA_BuildRule** build_rules, u32 count, u32 max_jobs) {
    nya_assert(build_rules != nullptr);

    if (count == 0) return NYA_OK;
    if (max_jobs == 0) max_jobs = nya_platform_processor_count();
    if (max_jobs == 0) max_jobs = 1;
    if (max_jobs > count) max_jobs = count;
    if (max_jobs > NYA_BUILD_MAX_PARALLEL_JOBS) max_jobs = NYA_BUILD_MAX_PARALLEL_JOBS;

    // A new epoch, exactly as nya_build starts one, so the shared dependencies below are built once
    // for this whole call rather than once per rule.
    if (_nya_build_depth == 0) _nya_build_epoch++;

    /*
     * Preparation is sequential; only the commands overlap.
     */
    for (u32 i = 0; i < count; i++) {
        NYA_BuildRule* rule = build_rules[i];
        nya_assert(rule != nullptr);

        for (u64 d = 0; d < NYA_BUILD_MAX_DEPENDENCIES; d++) {
            if (!rule->dependencies[d]) break;
            NYA_TRY(nya_build(rule->dependencies[d]));
        }

        for (u64 h = 0; h < NYA_BUILD_MAX_DEPENDENCIES; h++) {
            void (*hook)(NYA_BuildRule* rule) = rule->pre_build_hooks[h];
            if (!hook) break;
            hook(rule);
        }

        rule->parallel_arguments_before_vendors = _nya_build_apply_vendors(rule);

        // Captured rather than streamed. A dozen compilers writing to one terminal interleaves at
        // arbitrary byte boundaries, which turns a single diagnostic into confetti; held per rule, a
        // failure prints as one block.
        rule->command.flags |= NYA_COMMAND_FLAG_OUTPUT_CAPTURE;
        if (rule->command.arena == nullptr) rule->command.arena = nya_arena_global;
    }

    NYA_Error result = NYA_OK;

    /*
     * A pool: a finished rule's slot goes straight to the next rule, so one slow rule holds one slot
     * rather than the whole batch around it. Starting stops at the first failure, reaping does not.
     */
    NYA_BuildRule* running[NYA_BUILD_MAX_PARALLEL_JOBS];
    NYA_Command*   running_commands[NYA_BUILD_MAX_PARALLEL_JOBS];
    u32            running_count = 0;
    u32            next          = 0;

    while (next < count || running_count > 0) {
        while (result.ok && next < count && running_count < max_jobs) {
            NYA_BuildRule* rule = build_rules[next++];

            if (rule->is_metarule) {
                printf("[BUILDING META] %s\n", rule->name);
                continue;
            }

            printf("[BUILDING] %s\n", rule->name);

            NYA_Error spawn_result = nya_command_spawn(&rule->command);
            if (!spawn_result.ok) {
                result = spawn_result;
                break;
            }

            rule->parallel_is_running = true;
            running[running_count++]  = rule;
        }

        // every rule started has been reaped, and either none is left or a failure stopped the starting.
        if (running_count == 0) break;

        // Every running rule is reaped, including after one has already failed: a child left unwaited
        // is a zombie holding a half written output file that a later build would take for finished work.
        u32 finished_count = 0;
        for (u32 slot = 0; slot < running_count;) {
            NYA_BuildRule* rule     = running[slot];
            b8             finished = false;

            NYA_Error wait_result = nya_command_try_wait(&rule->command, &finished);
            if (wait_result.ok && !finished) {
                slot++;
                continue;
            }

            rule->parallel_is_running = false;
            running[slot]             = running[--running_count];
            finished_count++;

            _nya_build_finish_parallel(rule, wait_result, &result);
        }

        if (finished_count > 0) continue;

        for (u32 slot = 0; slot < running_count; slot++) running_commands[slot] = &running[slot]->command;
        nya_command_wait_ready(running_commands, running_count, _NYA_BUILD_POLL_INTERVAL_MS);
    }

    nya_assert(running_count == 0, "nya_build_parallel returned with commands still running.");
    nya_assert(!result.ok || next == count, "nya_build_parallel succeeded without starting every rule.");

    // Undo the vendor splice on every rule, exactly as _nya_build_always does, or a second call
    // would append the same flags again.
    for (u32 i = 0; i < count; i++) build_rules[i]->command.arguments[build_rules[i]->parallel_arguments_before_vendors] = nullptr;

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

/**
 * The newest modification time under `path`, or zero when it cannot be read.
 *
 * Decides whether the build tool is stale. A walk because the tool is compiled from whole trees, and
 * NYA_BUILD_IF_OUTDATED would only watch build.c and miss its headers.
 * */
NYA_INTERNAL b8 _nya_build_newest_callback(NYA_ConstCString path, const NYA_DirectoryEntry* entry, void* user_data) {
    nya_unused(path);

    if (entry->type == NYA_FILE_TYPE_FILE) {
        u64* newest = user_data;
        if (entry->modified_at > *newest) *newest = entry->modified_at;
    }

    return true;
}

NYA_INTERNAL u64 _nya_build_newest_under(NYA_ConstCString path) {
    u64 newest = 0;

    NYA_Arena* arena = nya_arena_create();
    if (arena == nullptr) return 0;

    // failure reads as unknown, which the caller treats as stale, so an unreadable directory rebuilds.
    NYA_Error error = nya_filesystem_walk(arena, path, _nya_build_newest_callback, &newest);

    nya_arena_destroy(arena);

    return error.ok ? newest : 0;
}

void nya_rebuild_yourself(s32* argc, NYA_CString* argv, NYA_Command cmd) {
    NYA_CString marker = "--no-rebuild"; // appended to argv

    /*
     * The whole argument list is scanned, not only the last slot.
     */
    for (s32 i = 1; i < *argc; i++) {
        if (!nya_string_equals(argv[i], marker)) continue;

        if (i == *argc - 1) *argc -= 1;
        return;
    }

    /*
     * Nothing to do when no source is newer than the tool.
     *
     * An unconditional rebuild costs 1.18 s before `./build --help` prints, against 0.012 s without it.
     *
     * Walks the trees FLAGS_BUILD_TOOL compiles: build.c, the build system and the engine base.
     * Deliberately wider than the include set, since extra stats are cheap and a missed dependency is a
     * stale tool.
     */
    u64 tool_modified = 0;

    if (nya_filesystem_last_modified(argv[0], &tool_modified).ok && tool_modified > 0) {
        u64 newest = _nya_build_newest_under("src");

        u64 entry_modified = 0;
        if (nya_filesystem_last_modified("build.c", &entry_modified).ok && entry_modified > newest) newest = entry_modified;

        // Zero means a walk failed, which is read as stale rather than as up to date.
        if (newest > 0 && newest <= tool_modified) return;
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
    /*
     * move rather than copy: vacates the path argv[0] so the compiler can write to it even on
     * Windows, where a running executable is locked against being opened for writing but is
     * allowed to be renamed.
     */
    NYA_EXPECT(nya_filesystem_move(argv[0], backup_path), "while backing up the build executable");

    NYA_Error build_result = nya_build(&rule);
    if (!build_result.ok) {
        /*
         * Restoring overwrites whatever the failed build left behind. nya_filesystem_move on
         * Windows was updated to support this; on POSIX rename() already does.
         */
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

    // post-build hooks only when there is an artifact. After a failed command they would bury the real
    // error under a second one, such as an integrity hook reporting "no such file".
    //
    // After the un-splice above, which is cleanup and runs either way.
    if (!result.ok) return result;

    for (u64 i = 0; i < NYA_BUILD_MAX_DEPENDENCIES; i++) {
        void (*hook)(NYA_BuildRule* rule) = build_rule->post_build_hooks[i];
        if (!hook) break;
        hook(build_rule);
    }

    return result;
}

/**
 * Runs a rule's command and reports what happened.
 * */
NYA_INTERNAL NYA_Error _nya_build_run(NYA_BuildRule* build_rule) {
    if (build_rule->is_metarule) {
        printf("[BUILDING META] %s\n", build_rule->name);
        return NYA_OK;
    }

    printf("[BUILDING] %s\n", build_rule->name);
    printf("[CMD] %s", build_rule->command.program);
    for (u64 i = 0; i < NYA_COMMAND_MAX_ARGUMENTS; i++) {
        NYA_ConstCString arg = build_rule->command.arguments[i];
        if (!arg) break;
        printf(" %s", arg);
    }
    printf("\n");

    NYA_TRY(nya_command_run(&build_rule->command));

    _nya_build_report(build_rule);

    if (build_rule->command.exit_code == 0) return NYA_OK;

    return nya_error(NYA_ERROR_NOT_OK, "build rule '%s' failed with exit code %d", build_rule->name, build_rule->command.exit_code);
}

/**
 * Reports a reaped parallel rule and runs its post build hooks when it passed. The first failure
 * becomes `result`; later ones are still printed.
 * */
NYA_INTERNAL void _nya_build_finish_parallel(NYA_BuildRule* build_rule, NYA_Error wait_result, NYA_Error* result) {
    nya_assert(build_rule != nullptr);
    nya_assert(result != nullptr);
    nya_assert(!build_rule->parallel_is_running);

    if (!wait_result.ok) {
        if (result->ok) *result = wait_result;
        return;
    }

    _nya_build_report(build_rule);

    if (build_rule->command.exit_code != 0) {
        if (result->ok) {
            *result = nya_error(NYA_ERROR_NOT_OK, "build rule '%s' failed with exit code %d", build_rule->name, build_rule->command.exit_code);
        }
        return;
    }

    for (u64 h = 0; h < NYA_BUILD_MAX_DEPENDENCIES; h++) {
        void (*hook)(NYA_BuildRule* rule) = build_rule->post_build_hooks[h];
        if (!hook) break;
        hook(build_rule);
    }

    build_rule->last_built_epoch = _nya_build_epoch;
}

/** Prints how a finished rule went. Shared, so serial and parallel builds report identically. */
NYA_INTERNAL void _nya_build_report(NYA_BuildRule* build_rule) {
    NYA_String         empty_str = { .length = 0, .items = (u8*)"" };
    const NYA_Command* command   = &build_rule->command;

    if (command->exit_code == 0) {
        printf("[OK] %s took " FMTu64 " ms.\n", build_rule->name, command->execution_time_ms);

        // a rule that passed with diagnostics still shows them. compilers and clang-tidy both exit zero
        // on warnings, and swallowing the output made every warning invisible.
        b8 warned = (command->stdout_content != nullptr && nya_string_contains(command->stdout_content, ": warning:")) ||
                    (command->stderr_content != nullptr && nya_string_contains(command->stderr_content, ": warning:"));
        if (!warned) return;

        (void)fflush(stdout);
        (void)fprintf(stderr, "[WARNINGS] %s\n", build_rule->name);
    } else {
        (void)fflush(stdout);
        (void)fprintf(stderr, "[FAILED] %s exit code: %d\n", build_rule->name, command->exit_code);
    }

    const NYA_String* stdout_to_print = command->stdout_content ? command->stdout_content : &empty_str;
    (void)fprintf(stderr, "------- STDOUT -------\n" NYA_FMT_STRING "\n", NYA_FMT_STRING_ARG(stdout_to_print));
    const NYA_String* stderr_to_print = command->stderr_content ? command->stderr_content : &empty_str;
    (void)fprintf(stderr, "------- STDERR -------\n" NYA_FMT_STRING "\n", NYA_FMT_STRING_ARG(stderr_to_print));

    (void)fflush(stderr);
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
