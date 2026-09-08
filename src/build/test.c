#include "build/build.h"

/** An array template needs a plain type name, and `NYA_BuildRule*` is not one. */
typedef NYA_BuildRule* NYA_BuildRulePointer;
nya_derive_array(NYA_BuildRulePointer);

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API DECLARATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

NYA_INTERNAL b8  _test_collect_sources(NYA_ConstCString path, const NYA_DirectoryEntry* entry, void* user_data);
NYA_INTERNAL s32 _test_compare_paths(const NYA_String* a, const NYA_String* b);

/**
 * Finds, builds and runs the tests, optionally under coverage instrumentation.
 *
 * One function for both because the two differ in three places — extra compile flags, an environment
 * variable naming the profile, and whether the binary is deleted afterwards — and everything else,
 * discovery and filtering and the parallel compile, is the same work. Two copies of it would drift.
 * */
NYA_INTERNAL void _test_run_all(NYA_ArgCommand* command, b8 coverage);

/** Merges the raw profiles a coverage run produced and prints the report. */
NYA_INTERNAL void _test_report_coverage(NYA_ArrayᐸNYA_BuildRulePointerᐳ* run_rules);

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PUBLIC API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

void test_runner(NYA_ArgCommand* command) {
    _test_run_all(command, false);
}

void coverage_runner(NYA_ArgCommand* command) {
    _test_run_all(command, true);
}

void _test_run_all(NYA_ArgCommand* command, b8 coverage) {
    nya_assert(command != nullptr);

    NYA_ArgParameter* test_files = command->parameters[0];
    nya_assert(test_files != nullptr);
    nya_assert(nya_string_equals(test_files->name, "tests"));

    // Walked rather than shelled out to `find`, which on Windows off msys2 resolves to the unrelated
    // find.exe in System32 and would take these arguments as a text search rather than fail.
    NYA_ArrayᐸNYA_Stringᐳ* tests = nya_array_create(nya_arena_global, NYA_String);
    NYA_EXPECT(nya_filesystem_walk(nya_arena_global, "./tests", _test_collect_sources, tests));
    nya_array_sort(tests, _test_compare_paths);

    /*
     * Two phases: compile everything at once, then run the binaries one at a time.
     *
     * Compiling is where the time goes — every test is a unity build of the whole engine at -O0 with
     * four sanitizers, and doing them in sequence leaves every core but one idle for the length of
     * the run. Running is cheap by comparison and stays serial on purpose: sanitizer reports go to
     * stderr unbuffered, so concurrent failures would interleave into something unreadable, and a
     * test that binds a port or touches a file would start racing its siblings.
     *
     * The rules are heap allocated because nya_build_parallel holds them across the whole batch,
     * where the serial version only needed them for one iteration.
     */
    NYA_ArrayᐸNYA_BuildRulePointerᐳ* build_rules = nya_array_create(nya_arena_global, NYA_BuildRulePointer);
    NYA_ArrayᐸNYA_BuildRulePointerᐳ* run_rules   = nya_array_create(nya_arena_global, NYA_BuildRulePointer);

    // The raw profiles land here, one per test, and the binaries have to survive the run for
    // llvm-cov to map counters back to source. Recreated each time so a deleted test cannot leave a
    // stale profile behind to be merged into the next report.
    if (coverage) {
        (void)nya_filesystem_delete_recursive(COVERAGE_DIRECTORY);
        NYA_EXPECT(nya_filesystem_create_directory(COVERAGE_DIRECTORY), "while creating the coverage directory");
    }

    nya_array_foreach (tests, original_test) {
        NYA_String* test      = nya_string_clone(nya_arena_global, original_test);
        NYA_CString test_cstr = nya_string_to_cstring(nya_arena_global, test);

        // check if we should run this test, by checking if its name contains any of the requested test names
        b8 should_run = test_files->values_count == 0 ? true : false;
        for (u32 param_index = 0; param_index < test_files->values_count; param_index++) {
            NYA_CString requested_test = test_files->values[param_index].as_string;
            if (nya_string_contains(test, requested_test)) {
                should_run = true;
                break;
            }
        }
        if (!should_run) continue;

        nya_string_strip_suffix(test, ".c");
        NYA_CString test_binary = nya_string_to_cstring(nya_arena_global, test);

        NYA_String* build_test_name = nya_string_sprintf(nya_arena_global, "build_test:%s", test_binary);
        NYA_BuildRule* build_test_rule = nya_arena_alloc(nya_arena_global, sizeof(NYA_BuildRule));
        *build_test_rule = (NYA_BuildRule){
        .name        = nya_string_to_cstring(nya_arena_global, build_test_name),
        .policy      = NYA_BUILD_ALWAYS,
        .output_file = test_binary,

        /*
         * The same codegen the project rules get, and for the same reason.
         *
         * A test compiles the engine from source, so it reads src/generated/strings.h and
         * src/generated/reflection.c exactly as the game does — and without this it read whatever
         * those files happened to hold from the last `./build build`. Editing assets/i18n/en.json or
         * an `@reflect` and running the tests then tested the previous generation of them, which is
         * the one case where a green suite means nothing.
         *
         * Named on every rule rather than run once before the loop because that is what the build
         * system already does with them: nya_build_parallel opens one epoch for the whole batch and
         * builds a shared dependency once inside it, so a hundred test rules naming index_assets
         * generate once. See its comment, and build_example, which is written this way already.
         */
        .dependencies = { &build_shaders, &index_assets, },

        .command = {
            .program   = CC,
            .arguments = {
                test_cstr,
                "-o", test_binary,
                CFLAGS,
                WARNINGS,
                INCLUDE_PATHS,
                LINKER_FLAGS,
                // The same plugins the project compiles, so a test can exercise them. Without this
                // a plugin test compiles to an empty file and reports a pass.
                FLAGS_PLUGINS,
                // FLAGS_TEST, not FLAGS_DEBUG: it sets NYA_EXECUTION_MODE=4, compiles in
                // nya_expect_crash so a test can survive a deliberate panic, and runs headless so
                // no GPU device is created. This rule used to spell out -DNYA_TESTING by hand and
                // get neither of the other two, which is why a test could not run on CI.
                FLAGS_TEST,
                // Built to run here, so the same host flags the build tool uses. See build.h.
                FLAGS_HOST_NATIVE,
#if !OS_WINDOWS
                // Where the Steam redistributable sits relative to a test binary. An rpath is an
                // ELF concept; a Windows host would resolve the DLL by search path instead.
                "-Wl,-rpath,$ORIGIN/../../../vendor/steam/redistributable_bin/linux64",
#endif
            },
        },

        .pre_build_hooks = { &hook_add_version_flag_and_git_hash, },
        // The same set the debug executable links, because a test includes the same engine. Two of
        // them was the drift that made every test fail to compile on a missing SDL_image header.
        // Exactly what the project links, by naming the same macro rather than repeating the list.
        // Spelling it out separately is what previously drifted and made every test fail to compile
        // on a missing SDL_image header; a hand copied list also silently omitted curl and sqlite,
        // so a plugin test would compile and then fail to link.
#if OS_WINDOWS
        .vendors         = { NYA_PROJECT_VENDORS_WINDOWS_X86_64, },
#else
        .vendors         = { NYA_PROJECT_VENDORS_LINUX_X86_64, },
#endif
    };

        NYA_String* run_test_name = nya_string_sprintf(nya_arena_global, "run_test:%s", test_binary);
        NYA_BuildRule* run_test_rule = nya_arena_alloc(nya_arena_global, sizeof(NYA_BuildRule));
        *run_test_rule = (NYA_BuildRule){
        .name        = nya_string_to_cstring(nya_arena_global, run_test_name),
        .policy      = NYA_BUILD_ALWAYS,
        .output_file = test_binary,

        .command = {
            .program     = test_binary,
            // The shared set from build.h. These four were spelled out here as well, identically,
            // which is one edit away from a test running under different options than a profiled run.
            .environment = { SANITIZER_ENVIRONMENT, },
        },

        // No dependency on the build rule any more: the whole batch is compiled below, before any
        // of it runs, so a per-rule dependency would just rebuild what is already there.
        //
        // A coverage run keeps its binaries instead: llvm-cov reads the coverage mapping out of the
        // executable, so deleting it leaves counts that cannot be attributed to any line.
        .post_build_hooks  = { coverage ? nullptr : &hook_remove_output_file, },
    };

        if (coverage) {
            // Appended rather than written into the initializer above, because the argument list is
            // a fixed array and the flags are conditional. Same shape as the libbacktrace splice in
            // build.c.
            u32 count = 0;
            while (count < NYA_COMMAND_MAX_ARGUMENTS && build_test_rule->command.arguments[count] != nullptr) count++;

            NYA_ConstCString extra[]   = { FLAGS_COVERAGE };
            u32              extra_len = (u32)(sizeof(extra) / sizeof(extra[0]));

            nya_assert(count + extra_len < NYA_COMMAND_MAX_ARGUMENTS, "no room to add the coverage flags to '%s'", build_test_rule->name);
            for (u32 i = 0; i < extra_len; i++) build_test_rule->command.arguments[count + i] = extra[i];

            /*
             * One raw profile per test, named after it.
             *
             * The default is `default.profraw` in the working directory, which every test would
             * write in turn — so the merge would see one test's counters and report everything else
             * as dead. Naming them individually is what makes the merge mean anything.
             */
            u32 env_count = 0;
            while (env_count < NYA_COMMAND_MAX_ENV_VARS && run_test_rule->command.environment[env_count] != nullptr) env_count++;

            NYA_String* profile = nya_string_sprintf(
                nya_arena_global,
                "LLVM_PROFILE_FILE=" COVERAGE_DIRECTORY "/%s.profraw",
                nya_string_to_cstring(nya_arena_global, nya_path_basename(nya_arena_global, test_binary))
            );

            nya_assert(env_count + 1 < NYA_COMMAND_MAX_ENV_VARS, "no room for LLVM_PROFILE_FILE on '%s'", run_test_rule->name);
            run_test_rule->command.environment[env_count] = nya_string_to_cstring(nya_arena_global, profile);
        }

        nya_array_push_back(build_rules, build_test_rule);
        nya_array_push_back(run_rules, run_test_rule);
    }

    if (build_rules->length == 0) return;

    // Zero jobs means one per hardware thread.
    NYA_EXPECT(nya_build_parallel(build_rules->items, (u32)build_rules->length, 0));

    nya_array_foreach (run_rules, run_rule) NYA_EXPECT(nya_build(*run_rule));

    if (coverage) _test_report_coverage(run_rules);
}

void _test_report_coverage(NYA_ArrayᐸNYA_BuildRulePointerᐳ* run_rules) {
    /*
     * Merge, then report. Both are ordinary build rules so a failure is reported the way any other
     * command's is, rather than as a silent absence of output.
     */
    NYA_BuildRule merge = {
        .name    = "coverage_merge",
        .policy  = NYA_BUILD_ALWAYS,
        .command = {
            .program   = "llvm-profdata",
            .arguments = { "merge", "-sparse", "-o", COVERAGE_PROFILE_DATA },
        },
    };

    u32 merge_count = 0;
    while (merge_count < NYA_COMMAND_MAX_ARGUMENTS && merge.command.arguments[merge_count] != nullptr) merge_count++;

    NYA_BuildRule report = {
        .name    = "coverage_report",
        .policy  = NYA_BUILD_ALWAYS,
        .command = {
            .program   = "llvm-cov",
            .arguments = { "report", "-instr-profile=" COVERAGE_PROFILE_DATA },
        },
    };

    u32 report_count = 0;
    while (report_count < NYA_COMMAND_MAX_ARGUMENTS && report.command.arguments[report_count] != nullptr) report_count++;

    /*
     * Every test contributes a profile and a binary.
     *
     * llvm-cov wants one executable as its positional argument and the rest as -object, because a
     * unity build compiles the whole engine into each test — the union across all of them is the
     * coverage, and any single binary alone would report most of the tree as dead.
     */
    b8 first_object = true;

    nya_array_foreach (run_rules, run_rule) {
        NYA_ConstCString binary = (*run_rule)->output_file;

        NYA_String* profile = nya_string_sprintf(
            nya_arena_global,
            COVERAGE_DIRECTORY "/%s.profraw",
            nya_string_to_cstring(nya_arena_global, nya_path_basename(nya_arena_global, binary))
        );

        nya_assert(merge_count < NYA_COMMAND_MAX_ARGUMENTS, "too many tests to merge in one command");
        merge.command.arguments[merge_count++] = nya_string_to_cstring(nya_arena_global, profile);

        nya_assert(report_count + 2 < NYA_COMMAND_MAX_ARGUMENTS, "too many tests to report on in one command");
        if (!first_object) report.command.arguments[report_count++] = "-object";
        report.command.arguments[report_count++] = binary;

        first_object = false;
    }

    // Restricted to the engine: the tests themselves are instrumented too, and counting a test file
    // as covered by its own execution would inflate every number here toward a hundred percent.
    nya_assert(report_count < NYA_COMMAND_MAX_ARGUMENTS, "no room for the source filter");
    report.command.arguments[report_count++] = "src/nyangine";

    NYA_EXPECT(nya_build(&merge), "while merging the coverage profiles");
    NYA_EXPECT(nya_build(&report), "while generating the coverage report");

    nya_log_info("Coverage profile written to " COVERAGE_PROFILE_DATA);
    nya_log_info("For an annotated listing: llvm-cov show -instr-profile=" COVERAGE_PROFILE_DATA " <one of the test binaries> <source file>");
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

NYA_INTERNAL b8 _test_collect_sources(NYA_ConstCString path, const NYA_DirectoryEntry* entry, void* user_data) {
    NYA_ArrayᐸNYA_Stringᐳ* sources = (NYA_ArrayᐸNYA_Stringᐳ*)user_data;

    if (entry->type != NYA_FILE_TYPE_FILE) return true;

    NYA_String* file = nya_string_from(nya_arena_global, path);
    if (!nya_string_ends_with(file, ".c")) return true;

    // nya_path_join normalises away the leading "./", which the build rules above expect to be
    // there since they use these paths verbatim as input and output files.
    if (!nya_string_starts_with(file, "./")) nya_string_extend_front(file, "./");

    nya_array_push_back(sources, *file);
    return true;
}

/** Byte order, so the run order is the same on every machine rather than the filesystem's. */
NYA_INTERNAL s32 _test_compare_paths(const NYA_String* a, const NYA_String* b) {
    u64 shared     = nya_min(a->length, b->length);
    s32 difference = nya_memcmp(a->items, b->items, shared);
    if (difference != 0) return difference < 0 ? -1 : 1;

    if (a->length == b->length) return 0;
    return a->length < b->length ? -1 : 1;
}
