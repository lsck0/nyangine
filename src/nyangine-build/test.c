#include "nyangine-build/build.h"

/** An array template needs a plain type name, and `NYA_BuildRule*` is not one. */
typedef NYA_BuildRule* NYA_BuildRulePointer;
nya_derive_array(NYA_BuildRulePointer);
nya_derive_dict(b8);

/* PRIVATE API DECLARATION */

/*
 * The engine a test links when it takes the engine as it is. Its own `#include "nyangine-core/nyangine.c"`
 * then resolves to the shim under TEST_ENGINE_SHIM_DIRECTORY, which includes the headers only.
 */
#define TEST_ENGINE_SOURCE         "./src/nyangine-core/nyangine.c"
#define TEST_ENGINE_OBJECT         OBJECT_DIRECTORY "/nyangine.test" OBJECT_SUFFIX
#define TEST_ENGINE_SHIM_DIRECTORY "./src/nyangine-build/prebuilt_engine"
#define TEST_ENGINE_SHIM           TEST_ENGINE_SHIM_DIRECTORY "/nyangine-core/nyangine.c"

/** Most lines the engine sharing scan reads from one test. */
#define TEST_SCAN_MAX_LINES 4096

/** Longest `_nya_` or `_NYA_` identifier the engine sharing scan looks up. A longer one compiles its own engine. */
#define TEST_SCAN_MAX_NAME 256

/** The headers that decide which internal identifiers a test can name and still share the engine. */
#define TEST_ENGINE_HEADER_DIRECTORY "./src/nyangine-std", "./src/nyangine-core", "./src/nyangine-ui", "./src/nyangine-plugins"

#if OS_WINDOWS
#define TEST_VENDORS NYA_PROJECT_VENDORS_WINDOWS_X86_64
#else
#define TEST_VENDORS NYA_PROJECT_VENDORS_LINUX_X86_64
#endif

/**
 * The llvm tools a coverage run needs after the instrumented binaries have run: one merges the raw
 * profiles into a single profile, the other maps the counters back to source. They ship with the clang
 * the build already uses, so a coverage run either has both or skips with a notice. See coverage_runner.
 * */
#define COVERAGE_PROFDATA_PROGRAM "llvm-profdata"
#define COVERAGE_COV_PROGRAM      "llvm-cov"

/**
 * The response files a coverage run hands the llvm tools. The whole suite is hundreds of binaries and
 * as many raw profiles, well past the arguments one NYA_Command holds, so the lists go in a file each
 * and the command carries a single `@file` the tool expands. Both live under the gitignored directory.
 * */
#define COVERAGE_OBJECTS_RESPONSE  COVERAGE_DIRECTORY "/objects.rsp"
#define COVERAGE_PROFILES_RESPONSE COVERAGE_DIRECTORY "/profiles.rsp"

NYA_INTERNAL b8  _test_collect_sources(NYA_ConstCString path, const NYA_DirectoryEntry* entry, void* user_data);
NYA_INTERNAL b8  _test_collect_headers(NYA_ConstCString path, const NYA_DirectoryEntry* entry, void* user_data);
NYA_INTERNAL s32 _test_compare_paths(const NYA_String* a, const NYA_String* b);

/** Whether a token is an identifier starting `_nya_` or `_NYA_`. */
NYA_INTERNAL b8 _test_token_is_internal_name(const NYA_Lexer* lexer, const NYA_Token* token) __attr_no_discard;

/**
 * Every `_nya_` and `_NYA_` identifier the engine headers name outside comments, mapped to whether a test
 * compiled against the headers alone can use it: false once a header names it on a NYA_INTERNAL line.
 * */
NYA_INTERNAL NYA_Dictᐸb8ᐳ* _test_scan_header_identifiers(void) __attr_no_discard;

/**
 * Whether a test can link the shared engine object. Not when it defines or undefines anything before
 * including the engine, since that changes what the engine compiles to, not when it never includes the
 * engine, and not when it names a `_nya_` or `_NYA_` identifier that `header_identifiers` does not mark
 * usable. Those are static to an engine source or defined in one, so only the unity build reaches them.
 * */
NYA_INTERNAL b8 _test_shares_engine(NYA_ConstCString source, NYA_Dictᐸb8ᐳ* header_identifiers);

/** Appends a nullptr terminated argument list to a rule's command. */
NYA_INTERNAL void _test_append_arguments(NYA_BuildRule* rule, NYA_ConstCString const* arguments);

/**
 * Finds, builds and runs the tests, optionally under coverage instrumentation and its threshold gate.
 * */
NYA_INTERNAL void _test_run_all(NYA_ArgCommand* command, b8 coverage, s64 fail_under, b8 want_html);

/**
 * Merges the raw profiles a coverage run produced, prints the per-file report, and exits non-zero when
 * total line coverage of src/nyangine is below `fail_under`. Writes the HTML listing too when asked.
 * */
NYA_INTERNAL void _test_report_coverage(NYA_ArrayᐸNYA_BuildRulePointerᐳ* run_rules, s64 fail_under, b8 want_html);

/** Whether `program --version` runs and exits cleanly, so a missing llvm tool is a skip, not a crash. */
NYA_INTERNAL b8 _coverage_program_exists(NYA_ConstCString program) __attr_no_discard;

/**
 * Writes `<binary>\n-object <binary>...\nsrc/nyangine` to COVERAGE_OBJECTS_RESPONSE: the instrumented
 * binaries llvm-cov reads the coverage mapping from, then the one source tree the numbers are about.
 * Returns the `@file` argument that expands to it, shared by the report, the export and the listing —
 * a file because the suite is more binaries than one command's argument list holds.
 * */
NYA_INTERNAL NYA_ConstCString _coverage_write_object_response(NYA_ArrayᐸNYA_BuildRulePointerᐳ* run_rules) __attr_no_discard;

/**
 * Writes one raw profile path per line to COVERAGE_PROFILES_RESPONSE and returns the `@file` argument
 * that expands to it, so llvm-profdata merges them all however many tests there are.
 * */
NYA_INTERNAL NYA_ConstCString _coverage_write_profile_response(NYA_ArrayᐸNYA_BuildRulePointerᐳ* run_rules) __attr_no_discard;

/** The total line coverage percent out of `llvm-cov export` JSON, or a negative on a parse failure. */
NYA_INTERNAL f64 _coverage_parse_line_percent(NYA_ConstCString json) __attr_no_discard;

/* PUBLIC API IMPLEMENTATION */

void test_runner(NYA_ArgCommand* command) {
    _test_run_all(command, false, 0, false);
}

void coverage_runner(NYA_ArgCommand* command) {
    nya_assert(command != nullptr);

    NYA_ArgParameter* fail_under = command->parameters[1];
    NYA_ArgParameter* html_flag  = command->parameters[2];
    nya_assert(fail_under != nullptr && nya_string_equals(fail_under->name, "fail-under"));
    nya_assert(html_flag != nullptr && nya_string_equals(html_flag->name, "html"));

    /* llvm-profdata merges the profiles and llvm-cov maps the counters back to source. They ship with the clang the build already uses, but a stripped toolchain can lack them; missing, this skips with a notice rather than failing, the way the CVE hook does. Coverage is a gate CI runs, not something a developer's checkout without the matching llvm tools should trip over. */
    if (!_coverage_program_exists(COVERAGE_PROFDATA_PROGRAM) || !_coverage_program_exists(COVERAGE_COV_PROGRAM)) {
        nya_log_info("Coverage skipped: '%s' and '%s' are not both on PATH. They ship with clang;", COVERAGE_PROFDATA_PROGRAM, COVERAGE_COV_PROGRAM);
        nya_log_info("install the matching llvm tools to measure coverage. CI has them and runs ./build coverage.");
        return;
    }

    _test_run_all(command, true, fail_under->value.as_s64, html_flag->value.as_b8);
}

void _test_run_all(NYA_ArgCommand* command, b8 coverage, s64 fail_under, b8 want_html) {
    nya_assert(command != nullptr);

    NYA_ArgParameter* test_files = command->parameters[0];
    nya_assert(test_files != nullptr);
    nya_assert(nya_string_equals(test_files->name, "tests"));

    // Walked rather than shelled out to `find`, which on Windows off msys2 resolves to the unrelated find.exe in System32 and would take these arguments as a text search rather than fail.
    NYA_ArrayᐸNYA_Stringᐳ* tests = nya_array_create(nya_arena_global, NYA_String);
    NYA_EXPECT(nya_filesystem_walk(nya_arena_global, "./tests", _test_collect_sources, tests));
    nya_array_sort(tests, _test_compare_paths);

    /* Three phases: compile everything at once, link everything at once, then run the binaries one at a time. The engine is compiled once and linked into every test that takes it as it is. */
    NYA_ArrayᐸNYA_BuildRulePointerᐳ* compile_rules = nya_array_create(nya_arena_global, NYA_BuildRulePointer);
    NYA_ArrayᐸNYA_BuildRulePointerᐳ* link_rules    = nya_array_create(nya_arena_global, NYA_BuildRulePointer);
    NYA_ArrayᐸNYA_BuildRulePointerᐳ* run_rules     = nya_array_create(nya_arena_global, NYA_BuildRulePointer);

    // The raw profiles land here, one per test, and the binaries have to survive the run for llvm-cov to map counters back to source. Recreated each time so a deleted test cannot leave a stale profile behind to be merged into the next report.
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

    u32           tests_sharing_engine = 0;
    NYA_Dictᐸb8ᐳ* header_identifiers   = _test_scan_header_identifiers();

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

        /* A test under tests/nyangine/terminal/ is compiled against the terminal backend instead of the GPU one, and therefore against its own engine: the shared engine object is built once without NYA_TERMINAL, and linking a test that was compiled with it against that object would mix two different render2d implementations in one binary. Everything under src/nyangine/renderer/render2d_terminal.c had no test at all before this, because there was no way to ask for that flavour. */
        const b8 terminal_flavour = strstr(test_cstr, "/terminal/") != nullptr;

        b8 shares_engine      = !terminal_flavour && _test_shares_engine(test_cstr, header_identifiers);
        tests_sharing_engine += shares_engine ? 1 : 0;

        // the baked blob is generated for release builds only, so a test that compiles the engine the way a release does asks for it here. Found when test_asset_blob passed locally and CI had no assets.c.
        NYA_String* source = nya_string_create(nya_arena_global);
        NYA_EXPECT(nya_file_read(test_cstr, source), "while reading '%s'", test_cstr);
        const b8 wants_blob = nya_string_contains(source, "#define NYA_ASSET_PREFER_BLOB");

        NYA_String*    compile_test_name = nya_string_sprintf(nya_arena_global, "compile_test:%s", test_binary);
        NYA_BuildRule* compile_test_rule = nya_arena_alloc(nya_arena_global, sizeof(NYA_BuildRule));
        *compile_test_rule = (NYA_BuildRule){
            .name        = nya_string_to_cstring(nya_arena_global, compile_test_name),
            .policy      = NYA_BUILD_ALWAYS,
            .output_file = test_object,

            .dependencies = { &build_shaders, &index_assets, wants_blob ? &bundle_assets : nullptr, },

            .command = {
                .program   = CC,
                .arguments = {
                    test_cstr,
                    "-c", "-o", test_object,
                    CFLAGS,
                    WARNINGS,
                    INCLUDE_PATHS,
                    // The same plugins the project compiles, so a test can exercise them. Without this a plugin test compiles to an empty file and reports a pass.
                    FLAGS_PLUGINS,
                    // FLAGS_TEST, not FLAGS_DEBUG: it sets NYA_EXECUTION_MODE=4, compiles in nya_expect_crash so a test can survive a deliberate panic, and runs headless so no GPU device is created.
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

        // Searched before the -I paths, so the test's own include of the engine source finds the headers only and the definitions come from the engine object at link time. Also included up front, since a test that includes nyangine.h first would otherwise see the headers outside the shim.
        if (shares_engine) {
            _test_append_arguments(compile_test_rule, (NYA_ConstCString[]){ "-iquote", TEST_ENGINE_SHIM_DIRECTORY, "-include", TEST_ENGINE_SHIM, nullptr });
        }
        if (terminal_flavour) _test_append_arguments(compile_test_rule, (NYA_ConstCString[]){ FLAGS_TERMINAL, nullptr });
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
                    // Where the Steam redistributable sits relative to a test binary. An rpath is an ELF concept; a Windows host would resolve the DLL by search path instead.
                    "-Wl,-rpath,$ORIGIN/../../../vendor/steam/redistributable_bin/linux64",
#endif
                },
            },

            // Exactly what the project links, by naming the same macro. A hand copied list is what previously drifted and made a plugin test compile and then fail to link.
            .vendors          = { TEST_VENDORS, },
            .vendor_flags     = NYA_BUILD_VENDOR_FLAGS_LINK,
            // A test that compiles the engine itself leaves an object as large as the engine's, one per test. The compiler cache keeps its own copy, so nothing is lost by dropping it.
            .post_build_hooks = { &hook_remove_input_file, },
        };

        // the vendor archives are spliced in after every argument, so they still follow the engine object. base_perf.h defines an extern inline function, which C emits once in every object including the header, so the test and the engine each carry an identical copy.
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

            // No dependency on the link rule: the whole batch is linked below, before any of it runs, so a per-rule dependency would just rebuild what is already there. A coverage run keeps its binaries instead: llvm-cov reads the coverage mapping out of the executable, so deleting it leaves counts that cannot be attributed to any line.
            .post_build_hooks = { coverage ? nullptr : &hook_remove_output_file, },
        };

        if (coverage) {
            /* One raw profile per test, named after it. */
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

    nya_log_info("%u of %u tests link the shared engine object.", tests_sharing_engine, (u32)compile_rules->length);

    // First, so the longest compile starts before the short ones queue behind it.
    if (tests_sharing_engine > 0) nya_array_push_front(compile_rules, compile_engine_rule);

    // Zero jobs means one per hardware thread.
    NYA_EXPECT(nya_build_parallel(compile_rules->items, (u32)compile_rules->length, 0));
    NYA_EXPECT(nya_build_parallel(link_rules->items, (u32)link_rules->length, 0));

    nya_array_foreach (run_rules, run_rule) NYA_EXPECT(nya_build(*run_rule));

    if (coverage) _test_report_coverage(run_rules, fail_under, want_html);
}

void _test_report_coverage(NYA_ArrayᐸNYA_BuildRulePointerᐳ* run_rules, s64 fail_under, b8 want_html) {
    nya_assert(run_rules != nullptr);

    /* Merge the raw profiles into one, print the human report, read the total off a captured summary, then gate on it. Merge and the report are ordinary build rules, so a failure in either is reported the way any other command's is. The gate reads a second, machine-summary run rather than scraping the printed table, so the number it acts on is llvm-cov's own and no column has to be guessed at. */
    NYA_ConstCString profiles_response = _coverage_write_profile_response(run_rules);
    NYA_ConstCString objects_response  = _coverage_write_object_response(run_rules);

    NYA_BuildRule merge = {
        .name    = "coverage_merge",
        .policy  = NYA_BUILD_ALWAYS,
        .command = {
            .program   = COVERAGE_PROFDATA_PROGRAM,
            .arguments = { "merge", "-sparse", "-o", COVERAGE_PROFILE_DATA, profiles_response },
        },
    };
    NYA_EXPECT(nya_build(&merge), "while merging the coverage profiles");

    // The human report: the per-file and total line and region table, printed straight through.
    NYA_BuildRule report = {
        .name    = "coverage_report",
        .policy  = NYA_BUILD_ALWAYS,
        .command = {
            .program   = COVERAGE_COV_PROGRAM,
            .arguments = { "report", "-instr-profile=" COVERAGE_PROFILE_DATA, objects_response },
        },
    };
    NYA_EXPECT(nya_build(&report), "while generating the coverage report");

    /* The number the gate reads. `export -summary-only` keeps the per-file and total figures and drops the per-line detail, as JSON, so the total is unambiguous rather than a column counted off a table. */
    NYA_String* summary      = build_capture(nya_arena_global, COVERAGE_COV_PROGRAM,
                                             (const NYA_ConstCString[]){ "export", "-summary-only", "-instr-profile=" COVERAGE_PROFILE_DATA, objects_response, nullptr });
    f64         line_percent = _coverage_parse_line_percent(nya_string_to_cstring(nya_arena_global, summary));
    if (line_percent < 0.0) nya_log_panic("Could not read total line coverage from the llvm-cov export summary.");

    // An annotated HTML tree, when asked. Recreated each time so a deleted source cannot leave a stale page behind, and dropped under the gitignored coverage directory rather than committed.
    if (want_html) {
        (void)nya_filesystem_delete_recursive(COVERAGE_HTML_DIRECTORY);

        NYA_BuildRule show = {
            .name    = "coverage_html",
            .policy  = NYA_BUILD_ALWAYS,
            .command = {
                .program   = COVERAGE_COV_PROGRAM,
                .arguments = { "show", "-format=html", "-output-dir=" COVERAGE_HTML_DIRECTORY, "-instr-profile=" COVERAGE_PROFILE_DATA, objects_response },
            },
        };
        NYA_EXPECT(nya_build(&show), "while writing the HTML coverage listing");

        nya_log_info("Annotated HTML coverage written to " COVERAGE_HTML_DIRECTORY "/index.html");
    }

    nya_log_info("Coverage profile written to " COVERAGE_PROFILE_DATA);
    nya_log_info("Total line coverage of src/nyangine: %.2f%% (floor %lld%%, raise it with --fail-under).", line_percent, (long long)fail_under);

    if (line_percent < (f64)fail_under) {
        nya_log_panic("Total line coverage %.2f%% is below the --fail-under floor of %lld%%.", line_percent, (long long)fail_under);
    }
}

/* PRIVATE API IMPLEMENTATION */

NYA_INTERNAL b8 _test_collect_sources(NYA_ConstCString path, const NYA_DirectoryEntry* entry, void* user_data) {
    NYA_ArrayᐸNYA_Stringᐳ* sources = (NYA_ArrayᐸNYA_Stringᐳ*)user_data;

    if (entry->type != NYA_FILE_TYPE_FILE) return true;

    NYA_String* file = nya_string_from(nya_arena_global, path);
    if (!nya_string_ends_with(file, ".c")) return true;

    // tests/cbmc holds CBMC harnesses that only build under the `cbmc` model checker (`./build verify`): they use `__CPROVER_*` builtins plain clang cannot resolve. Skip them here so `./build run test` does not fail.
    if (nya_string_contains(nya_string_to_cstring(nya_arena_global, file), "/cbmc/")) return true;

    // nya_path_join normalises away the leading "./", which the build rules above expect to be there since they use these paths verbatim as input and output files.
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

NYA_INTERNAL b8 _test_collect_headers(NYA_ConstCString path, const NYA_DirectoryEntry* entry, void* user_data) {
    NYA_ArrayᐸNYA_Stringᐳ* headers = (NYA_ArrayᐸNYA_Stringᐳ*)user_data;

    if (entry->type != NYA_FILE_TYPE_FILE) return true;

    NYA_String* file = nya_string_from(nya_arena_global, path);
    if (nya_string_ends_with(file, ".h")) nya_array_push_back(headers, *file);

    return true;
}

NYA_INTERNAL b8 _test_token_is_internal_name(const NYA_Lexer* lexer, const NYA_Token* token) {
    nya_assert(lexer != nullptr);
    nya_assert(token != nullptr);

    if (token->type != NYA_TOKEN_IDENT || token->length <= 5) return false;

    NYA_ConstCString spelling = lexer->source + token->source_location;
    return nya_memcmp(spelling, "_nya_", 5) == 0 || nya_memcmp(spelling, "_NYA_", 5) == 0;
}

NYA_INTERNAL NYA_Dictᐸb8ᐳ* _test_scan_header_identifiers(void) {
    NYA_ArrayᐸNYA_Stringᐳ* headers = nya_array_create(nya_arena_global, NYA_String);
    // The engine is three subprojects now; scan each so a test may name an internal from any of them.
    NYA_ConstCString roots[] = { TEST_ENGINE_HEADER_DIRECTORY };
    for (u32 root = 0; root < nya_carray_length(roots); root++) {
        NYA_EXPECT(nya_filesystem_walk(nya_arena_global, roots[root], _test_collect_headers, headers));
    }

    NYA_Dictᐸb8ᐳ* identifiers = nya_dict_create(nya_arena_global, b8);

    nya_array_foreach (headers, header) {
        NYA_CString path = nya_string_to_cstring(nya_arena_global, header);
        NYA_String* text = nya_string_create(nya_arena_global);
        NYA_EXPECT(nya_file_read(path, text), "while reading '%s'", path);

        NYA_Lexer lexer = nya_lexer_create(nya_string_to_cstring(nya_arena_global, text), NYA_LEXER_UTF8_IDENTS);
        nya_lexer_run(&lexer);
        defer nya_lexer_destroy(&lexer);

        // a declaration keeps its storage class on the line that names it.
        u32 internal_line = 0;

        nya_array_foreach (lexer.tokens, token) {
            if (token->type != NYA_TOKEN_IDENT) continue;

            NYA_ConstCString spelling = lexer.source + token->source_location;
            if (token->length == strlen("NYA_INTERNAL") && nya_memcmp(spelling, "NYA_INTERNAL", token->length) == 0) {
                internal_line = token->line_number;
                continue;
            }

            if (!_test_token_is_internal_name(&lexer, token)) continue;

            NYA_CString name = nya_arena_alloc(nya_arena_global, token->length + 1);
            nya_memcpy(name, spelling, token->length);
            name[token->length] = '\0';

            b8  usable = token->line_number != internal_line;
            b8* known  = nya_dict_get(identifiers, name);

            if (known == nullptr) {
                nya_dict_add(identifiers, name, usable);
            } else {
                *known = *known && usable;
            }
        }
    }

    return identifiers;
}

NYA_INTERNAL b8 _test_shares_engine(NYA_ConstCString source, NYA_Dictᐸb8ᐳ* header_identifiers) {
    nya_assert(source != nullptr);
    nya_assert(header_identifiers != nullptr);

    NYA_String* text = nya_string_create(nya_arena_global);
    NYA_EXPECT(nya_file_read(source, text), "while reading '%s'", source);

    {
        NYA_Lexer lexer = nya_lexer_create(nya_string_to_cstring(nya_arena_global, text), NYA_LEXER_UTF8_IDENTS);
        nya_lexer_run(&lexer);
        defer nya_lexer_destroy(&lexer);

        nya_array_foreach (lexer.tokens, token) {
            if (!_test_token_is_internal_name(&lexer, token)) continue;
            if (token->length >= TEST_SCAN_MAX_NAME) return false;

            char name[TEST_SCAN_MAX_NAME];
            nya_memcpy(name, lexer.source + token->source_location, token->length);
            name[token->length] = '\0';

            b8* usable = nya_dict_get(header_identifiers, name);
            if (usable == nullptr || !*usable) return false;
        }
    }

    NYA_ArrayᐸNYA_Stringᐳ* lines = nya_string_split_lines(nya_arena_global, text);
    nya_assert(lines->length <= TEST_SCAN_MAX_LINES, "'%s' is longer than the engine sharing scan reads", source);

    nya_array_foreach (lines, line) {
        nya_string_trim_whitespace(line);
        if (!nya_string_starts_with(line, "#")) continue;

        // the directive word, with any space the preprocessor allows after the hash skipped.
        NYA_CString directive = nya_string_to_cstring(nya_arena_global, line) + 1;
        while (*directive == ' ' || *directive == '\t') directive++;

        if (nya_string_starts_with(directive, "include \"nyangine-core/nyangine.c\"")) return true;
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

NYA_INTERNAL b8 _coverage_program_exists(NYA_ConstCString program) {
    nya_assert(program != nullptr);

    // A program missing from PATH still spawns: the forked child fails execvp and _exit(127)s, so nya_command_run returns ok with a non-zero exit. Presence is the clean exit, not the spawn. Both llvm tools answer --version with 0, which is what this checks. Same shape as sbom.c's probe.
    NYA_Command probe = {
        .flags     = NYA_COMMAND_FLAG_OUTPUT_SUPPRESS,
        .program   = program,
        .arguments = { "--version" },
    };
    NYA_Error ran = nya_command_run(&probe);
    return ran.ok && probe.exit_code == 0;
}

NYA_INTERNAL NYA_ConstCString _coverage_write_object_response(NYA_ArrayᐸNYA_BuildRulePointerᐳ* run_rules) {
    nya_assert(run_rules != nullptr);

    // One token per line, which is how the llvm tools split a response file. The first binary is positional; every later one is introduced by -object, which is how llvm-cov takes coverage from more than one binary at once.
    NYA_String* body         = nya_string_create(nya_arena_global);
    b8          first_object = true;
    nya_array_foreach (run_rules, run_rule) {
        if (!first_object) nya_string_extend(body, "-object ");
        nya_string_extend(body, (*run_rule)->output_file);
        nya_string_extend(body, "\n");
        first_object = false;
    }

    // Restricted to the engine: the tests themselves are instrumented too, and counting a test file as covered by its own execution would drag every number here toward a hundred percent.
    nya_string_extend(body, "src/nyangine\n");

    NYA_EXPECT(nya_file_write(COVERAGE_OBJECTS_RESPONSE, body), "while writing the coverage object list");
    return "@" COVERAGE_OBJECTS_RESPONSE;
}

NYA_INTERNAL NYA_ConstCString _coverage_write_profile_response(NYA_ArrayᐸNYA_BuildRulePointerᐳ* run_rules) {
    nya_assert(run_rules != nullptr);

    // Every test contributes one raw profile, named after its binary. One path per line, the same response-file shape llvm-profdata reads its inputs from.
    NYA_String* body = nya_string_create(nya_arena_global);
    nya_array_foreach (run_rules, run_rule) {
        NYA_String* profile = nya_string_sprintf(
            nya_arena_global,
            COVERAGE_DIRECTORY "/%s.profraw\n",
            nya_string_to_cstring(nya_arena_global, nya_path_basename(nya_arena_global, (*run_rule)->output_file))
        );
        nya_string_extend(body, nya_string_to_cstring(nya_arena_global, profile));
    }

    NYA_EXPECT(nya_file_write(COVERAGE_PROFILES_RESPONSE, body), "while writing the coverage profile list");
    return "@" COVERAGE_PROFILES_RESPONSE;
}

NYA_INTERNAL f64 _coverage_parse_line_percent(NYA_ConstCString json) {
    nya_assert(json != nullptr);

    // `export` ends the object with a "totals" summary, and the per-file summaries with the same shape come before it, so searching forward from "totals" for its "lines" percent lands on the total and never on one file. A hand walk rather than a full parse: one number out of a known-good shape.
    NYA_ConstCString totals = strstr(json, "\"totals\"");
    if (totals == nullptr) return -1.0;

    NYA_ConstCString lines = strstr(totals, "\"lines\"");
    if (lines == nullptr) return -1.0;

    NYA_ConstCString percent = strstr(lines, "\"percent\"");
    if (percent == nullptr) return -1.0;

    // Step past `"percent"` and the colon and space that separate it from its value.
    percent += strlen("\"percent\"");
    while (*percent == ':' || *percent == ' ') percent++;

    char* end   = nullptr;
    f64   value = strtod(percent, &end);
    if (end == percent) return -1.0; // nothing numeric where the value should have been.

    return value;
}
