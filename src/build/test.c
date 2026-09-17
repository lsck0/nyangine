#include "build/build.h"

/** An array template needs a plain type name, and `NYA_BuildRule*` is not one. */
typedef NYA_BuildRule* NYA_BuildRulePointer;
nya_derive_array(NYA_BuildRulePointer);

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API DECLARATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/*
 * The engine a test links when it takes the engine as it is. Its own `#include "nyangine/nyangine.c"`
 * then resolves to the shim under TEST_ENGINE_SHIM_DIRECTORY, which includes the headers only.
 */
#define TEST_ENGINE_SOURCE         "./src/nyangine/nyangine.c"
#define TEST_ENGINE_OBJECT         OBJECT_DIRECTORY "/nyangine.test" OBJECT_SUFFIX
#define TEST_ENGINE_SHIM_DIRECTORY "./src/build/prebuilt_engine"
#define TEST_ENGINE_SHIM           TEST_ENGINE_SHIM_DIRECTORY "/nyangine/nyangine.c"

/** Most lines the engine sharing scan reads from one test. */
#define TEST_SCAN_MAX_LINES 4096

#if OS_WINDOWS
#define TEST_VENDORS NYA_PROJECT_VENDORS_WINDOWS_X86_64
#else
#define TEST_VENDORS NYA_PROJECT_VENDORS_LINUX_X86_64
#endif

NYA_INTERNAL b8  _test_collect_sources(NYA_ConstCString path, const NYA_DirectoryEntry* entry, void* user_data);
NYA_INTERNAL s32 _test_compare_paths(const NYA_String* a, const NYA_String* b);

/**
 * Whether a test can link the shared engine object. Not when it defines or undefines anything before
 * including the engine, since that changes what the engine compiles to, not when it never includes the
 * engine, and not when it names an internal `_nya_` or `_NYA_` identifier, which may be static to an
 * engine source and so only reachable from inside the unity build.
 * */
NYA_INTERNAL b8 _test_shares_engine(NYA_ConstCString source);

/** Appends a nullptr terminated argument list to a rule's command. */
NYA_INTERNAL void _test_append_arguments(NYA_BuildRule* rule, NYA_ConstCString const* arguments);

/**
 * Finds, builds and runs the tests, optionally under coverage instrumentation.
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
     * Three phases: compile everything at once, link everything at once, then run the binaries one at a
     * time. The engine is compiled once and linked into every test that takes it as it is.
     */
    NYA_ArrayᐸNYA_BuildRulePointerᐳ* compile_rules = nya_array_create(nya_arena_global, NYA_BuildRulePointer);
    NYA_ArrayᐸNYA_BuildRulePointerᐳ* link_rules    = nya_array_create(nya_arena_global, NYA_BuildRulePointer);
    NYA_ArrayᐸNYA_BuildRulePointerᐳ* run_rules     = nya_array_create(nya_arena_global, NYA_BuildRulePointer);

    // The raw profiles land here, one per test, and the binaries have to survive the run for
    // llvm-cov to map counters back to source. Recreated each time so a deleted test cannot leave a
    // stale profile behind to be merged into the next report.
    if (coverage) {
        (void)nya_filesystem_delete_recursive(COVERAGE_DIRECTORY);
        NYA_EXPECT(nya_filesystem_create_directory(COVERAGE_DIRECTORY), "while creating the coverage directory");
    }

    NYA_BuildRule* compile_engine_rule = nya_arena_alloc(nya_arena_global, sizeof(NYA_BuildRule));
    *compile_engine_rule = (NYA_BuildRule){
        .name        = "compile_test_engine",
        .policy      = NYA_BUILD_ALWAYS,
        .output_file = TEST_ENGINE_OBJECT,

        // The same codegen the project rules get, and for the same reason.
        .dependencies = { &build_shaders, &index_assets, },

        .command = {
            .program   = CC,
            .arguments = {
                TEST_ENGINE_SOURCE,
                "-c", "-o", TEST_ENGINE_OBJECT,
                CFLAGS,
                WARNINGS,
                INCLUDE_PATHS,
                FLAGS_PLUGINS,
                FLAGS_TEST,
                FLAGS_HOST_NATIVE_COMPILE
            },
        },

        .pre_build_hooks = { &hook_add_version_flag, &hook_create_output_directory, &hook_use_compiler_cache, },
        .vendors         = { TEST_VENDORS, },
        .vendor_flags    = NYA_BUILD_VENDOR_FLAGS_COMPILE,
    };
    if (coverage) _test_append_arguments(compile_engine_rule, (NYA_ConstCString[]){ FLAGS_COVERAGE, nullptr });

    b8 any_test_shares_engine = false;

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

        // the collected path starts with "./", which the object directory replaces.
        nya_assert(nya_string_starts_with(test, "./"));
        NYA_CString test_stem   = nya_string_to_cstring(nya_arena_global, test) + 2;
        NYA_CString test_object = nya_string_to_cstring(nya_arena_global, nya_string_sprintf(nya_arena_global, OBJECT_DIRECTORY "/%s" OBJECT_SUFFIX, test_stem));

        nya_string_extend(test, HOST_EXECUTABLE_SUFFIX);
        NYA_CString test_binary = nya_string_to_cstring(nya_arena_global, test);

        b8 shares_engine        = _test_shares_engine(test_cstr);
        any_test_shares_engine |= shares_engine;

        NYA_String*    compile_test_name = nya_string_sprintf(nya_arena_global, "compile_test:%s", test_binary);
        NYA_BuildRule* compile_test_rule = nya_arena_alloc(nya_arena_global, sizeof(NYA_BuildRule));
        *compile_test_rule = (NYA_BuildRule){
            .name        = nya_string_to_cstring(nya_arena_global, compile_test_name),
            .policy      = NYA_BUILD_ALWAYS,
            .output_file = test_object,

            .dependencies = { &build_shaders, &index_assets, },

            .command = {
                .program   = CC,
                .arguments = {
                    test_cstr,
                    "-c", "-o", test_object,
                    CFLAGS,
                    WARNINGS,
                    INCLUDE_PATHS,
                    // The same plugins the project compiles, so a test can exercise them. Without this
                    // a plugin test compiles to an empty file and reports a pass.
                    FLAGS_PLUGINS,
                    // FLAGS_TEST, not FLAGS_DEBUG: it sets NYA_EXECUTION_MODE=4, compiles in
                    // nya_expect_crash so a test can survive a deliberate panic, and runs headless so
                    // no GPU device is created.
                    FLAGS_TEST,
                    // Built to run here, so the same host flags the build tool uses. See build.h.
                    FLAGS_HOST_NATIVE_COMPILE
                },
            },

            .pre_build_hooks = { &hook_add_version_flag, &hook_create_output_directory, &hook_use_compiler_cache, },
            // The same set the debug executable links, because a test includes the same engine.
            .vendors         = { TEST_VENDORS, },
            .vendor_flags    = NYA_BUILD_VENDOR_FLAGS_COMPILE,
        };

        // Searched before the -I paths, so the test's own include of the engine source finds the headers
        // only and the definitions come from the engine object at link time. Also included up front, since
        // a test that includes nyangine.h first would otherwise see the headers outside the shim.
        if (shares_engine) {
            _test_append_arguments(compile_test_rule, (NYA_ConstCString[]){ "-iquote", TEST_ENGINE_SHIM_DIRECTORY, "-include", TEST_ENGINE_SHIM, nullptr });
        }
        if (coverage) _test_append_arguments(compile_test_rule, (NYA_ConstCString[]){ FLAGS_COVERAGE, nullptr });

        NYA_String*    link_test_name = nya_string_sprintf(nya_arena_global, "link_test:%s", test_binary);
        NYA_BuildRule* link_test_rule = nya_arena_alloc(nya_arena_global, sizeof(NYA_BuildRule));
        *link_test_rule = (NYA_BuildRule){
            .name        = nya_string_to_cstring(nya_arena_global, link_test_name),
            .policy      = NYA_BUILD_ALWAYS,
            .input_file  = test_object,
            .output_file = test_binary,

            .command = {
                .program   = CC,
                .arguments = {
                    test_object,
                    "-o", test_binary,
                    CFLAGS,
                    FLAGS_TEST,
                    FLAGS_HOST_NATIVE_LINK,
                    LINKER_FLAGS,
#if !OS_WINDOWS
                    // Where the Steam redistributable sits relative to a test binary. An rpath is an
                    // ELF concept; a Windows host would resolve the DLL by search path instead.
                    "-Wl,-rpath,$ORIGIN/../../../vendor/steam/redistributable_bin/linux64",
#endif
                },
            },

            // Exactly what the project links, by naming the same macro. A hand copied list is what
            // previously drifted and made a plugin test compile and then fail to link.
            .vendors          = { TEST_VENDORS, },
            .vendor_flags     = NYA_BUILD_VENDOR_FLAGS_LINK,
            // A test that compiles the engine itself leaves an object as large as the engine's, one per
            // test. The compiler cache keeps its own copy, so nothing is lost by dropping it.
            .post_build_hooks = { &hook_remove_input_file, },
        };

        // the vendor archives are spliced in after every argument, so they still follow the engine object.
        // base_perf.h defines an extern inline function, which C emits once in every object including the
        // header, so the test and the engine each carry an identical copy.
        if (shares_engine) _test_append_arguments(link_test_rule, (NYA_ConstCString[]){ TEST_ENGINE_OBJECT, nullptr });
        if (coverage) _test_append_arguments(link_test_rule, (NYA_ConstCString[]){ FLAGS_COVERAGE, nullptr });

        NYA_String*    run_test_name = nya_string_sprintf(nya_arena_global, "run_test:%s", test_binary);
        NYA_BuildRule* run_test_rule = nya_arena_alloc(nya_arena_global, sizeof(NYA_BuildRule));
        *run_test_rule = (NYA_BuildRule){
            .name        = nya_string_to_cstring(nya_arena_global, run_test_name),
            .policy      = NYA_BUILD_ALWAYS,
            .output_file = test_binary,

            .command = {
                .program     = test_binary,
                // The shared set from build.h, so a test runs under the same options as a profiled run.
                .environment = { SANITIZER_ENVIRONMENT, },
            },

            // No dependency on the link rule: the whole batch is linked below, before any of it runs,
            // so a per-rule dependency would just rebuild what is already there.
            //
            // A coverage run keeps its binaries instead: llvm-cov reads the coverage mapping out of the
            // executable, so deleting it leaves counts that cannot be attributed to any line.
            .post_build_hooks = { coverage ? nullptr : &hook_remove_output_file, },
        };

        if (coverage) {
            /*
             * One raw profile per test, named after it.
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

        nya_array_push_back(compile_rules, compile_test_rule);
        nya_array_push_back(link_rules, link_test_rule);
        nya_array_push_back(run_rules, run_test_rule);
    }

    if (compile_rules->length == 0) return;

    // First, so the longest compile starts before the short ones queue behind it.
    if (any_test_shares_engine) nya_array_push_front(compile_rules, compile_engine_rule);

    // Zero jobs means one per hardware thread.
    NYA_EXPECT(nya_build_parallel(compile_rules->items, (u32)compile_rules->length, 0));
    NYA_EXPECT(nya_build_parallel(link_rules->items, (u32)link_rules->length, 0));

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

NYA_INTERNAL b8 _test_shares_engine(NYA_ConstCString source) {
    nya_assert(source != nullptr);

    NYA_String* text = nya_string_create(nya_arena_global);
    NYA_EXPECT(nya_file_read(source, text), "while reading '%s'", source);

    // Coarse on purpose: a false match only costs that test its own engine compile.
    if (nya_string_contains(text, "_nya_") || nya_string_contains(text, "_NYA_")) return false;

    NYA_ArrayᐸNYA_Stringᐳ* lines = nya_string_split_lines(nya_arena_global, text);
    nya_assert(lines->length <= TEST_SCAN_MAX_LINES, "'%s' is longer than the engine sharing scan reads", source);

    nya_array_foreach (lines, line) {
        nya_string_trim_whitespace(line);
        if (!nya_string_starts_with(line, "#")) continue;

        // the directive word, with any space the preprocessor allows after the hash skipped.
        NYA_CString directive = nya_string_to_cstring(nya_arena_global, line) + 1;
        while (*directive == ' ' || *directive == '\t') directive++;

        if (nya_string_starts_with(directive, "include \"nyangine/nyangine.c\"")) return true;
        if (nya_string_starts_with(directive, "define") || nya_string_starts_with(directive, "undef")) return false;
    }

    return false;
}

NYA_INTERNAL void _test_append_arguments(NYA_BuildRule* rule, NYA_ConstCString const* arguments) {
    nya_assert(rule != nullptr);
    nya_assert(arguments != nullptr);

    u32 count = 0;
    while (count < NYA_COMMAND_MAX_ARGUMENTS && rule->command.arguments[count] != nullptr) count++;

    for (u32 i = 0; arguments[i] != nullptr; i++) {
        nya_assert(count + 1 < NYA_COMMAND_MAX_ARGUMENTS, "no room to add '%s' to '%s'", arguments[i], rule->name);
        rule->command.arguments[count++] = arguments[i];
    }

    nya_assert(rule->command.arguments[count] == nullptr);
}
