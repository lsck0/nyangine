#include "build/build.h"

/* PRIVATE API DECLARATION */

/** Collects the target names under tests/fuzz: "fuzz_serde_json.c" becomes "serde_json". */
NYA_INTERNAL b8 _fuzz_collect_targets(NYA_ConstCString path, const NYA_DirectoryEntry* entry, void* user_data);

/** Every target that exists, sorted, so the listing and the completions are the same order everywhere. */
NYA_INTERNAL NYA_ArrayᐸNYA_Stringᐳ* _fuzz_discover(NYA_Arena* arena) __attr_no_discard;

/** Byte order, so the listing is the same on every machine rather than the filesystem's. */
NYA_INTERNAL s32 _fuzz_compare_names(const NYA_String* a, const NYA_String* b);

/**
 * The AFL++ compiler, or null when there is none.
 *
 * Probed by running it, as the compiler cache is: an optional dependency that is missing is found out
 * rather than assumed, and the answer is cached because a probe is a process launch.
 * */
NYA_INTERNAL NYA_ConstCString _fuzz_compiler_program(void) __attr_no_discard;

/** Whether `program` runs at all. The whole environment probe. */
NYA_INTERNAL b8 _fuzz_program_exists(NYA_ConstCString program) __attr_no_discard;

/* PUBLIC API IMPLEMENTATION */

void fuzz_runner(NYA_ArgCommand* command) {
    nya_assert(command != nullptr);

    NYA_ArgParameter* target_name = command->parameters[0];
    nya_assert(target_name != nullptr);

    NYA_ArrayᐸNYA_Stringᐳ* targets = _fuzz_discover(nya_arena_global);

    /* No target named: say what there is rather than guessing one. A fuzzing session runs until it is stopped, so starting the wrong one costs an afternoon. */
    if (target_name->values_count == 0) {
        nya_log_info("Fuzz targets under " FUZZ_DIRECTORY ":");
        nya_array_foreach (targets, target) nya_log_info("  %s", nya_string_to_cstring(nya_arena_global, target));

        return;
    }

    // one session per invocation: afl-fuzz owns the terminal until it is stopped, and two of them would fight over it and over the findings directory.
    if (target_name->values_count > 1) {
        nya_log_error("Fuzz one target at a time; %u were named.", target_name->values_count);
        return;
    }

    NYA_ConstCString requested = target_name->values[0].as_string;

    b8 known = false;
    nya_array_foreach (targets, target) known = known || nya_string_equals(target, requested);

    if (!known) {
        nya_log_error("There is no fuzz target '%s'. Run `./build run fuzz` to list them.", requested);
        return;
    }

    /* The environment probe, in one place and before any work starts. */
    NYA_ConstCString compiler = _fuzz_compiler_program();

    if (compiler == nullptr || !_fuzz_program_exists(FUZZ_DRIVER_PROGRAM)) {
        nya_log_error("AFL++ is not installed, so there is nothing to fuzz with.");
        nya_log_error("  %s compiles a target with coverage instrumentation.", FUZZ_COMPILER_PROGRAM);
        nya_log_error("  %s drives it.", FUZZ_DRIVER_PROGRAM);
        nya_log_error("  Install it with your package manager's aflplusplus package, or set %s to the compiler's path.", FUZZ_COMPILER_ENV);
        nya_log_error("  The targets still build and replay their corpus without it: ./build run test %s", FUZZ_TARGET_PREFIX);

        return;
    }

    NYA_CString source = nya_string_to_cstring(nya_arena_global, nya_string_sprintf(nya_arena_global, FUZZ_DIRECTORY "/" FUZZ_TARGET_PREFIX "%s.c", requested));
    NYA_CString binary = nya_string_to_cstring(nya_arena_global, nya_string_sprintf(nya_arena_global, FUZZ_OUTPUT_DIRECTORY "/" FUZZ_TARGET_PREFIX "%s", requested));
    NYA_CString corpus = nya_string_to_cstring(nya_arena_global, nya_string_sprintf(nya_arena_global, FUZZ_CORPUS_ROOT "/%s", requested));
    NYA_CString findings = nya_string_to_cstring(nya_arena_global, nya_string_sprintf(nya_arena_global, FUZZ_OUTPUT_DIRECTORY "/%s", requested));

    if (!nya_filesystem_exists(FUZZ_OUTPUT_DIRECTORY)) NYA_EXPECT(nya_filesystem_create_directory(FUZZ_OUTPUT_DIRECTORY), "while creating the fuzzing output directory");

    NYA_BuildRule build_rule = {
        .name        = "build_fuzz_target",
        .policy      = NYA_BUILD_ALWAYS,
        .output_file = binary,

        // the same codegen a test gets: a target is a unity build of the engine and reads the generated strings, assets and reflection tables.
        .dependencies = { &build_shaders, &index_assets, },

        .command = {
            .program   = compiler,
            .arguments = {
                source,
                "-o", binary,
                CFLAGS,
                WARNINGS,
                INCLUDE_PATHS,
                LINKER_FLAGS,
                FLAGS_PLUGINS,
                // the same flags a test is built with: assertions live, headless, NYA_TESTING on. A target that fuzzed a build with different assertions would be fuzzing a different program from the one the suite checks.
                FLAGS_TEST,
                FLAGS_HOST_NATIVE_COMPILE
                FLAGS_HOST_NATIVE_LINK,
            },
        },

        .pre_build_hooks = { &hook_add_version_flag, },
#if OS_WINDOWS
        .vendors         = { NYA_PROJECT_VENDORS_WINDOWS_X86_64, },
#else
        .vendors         = { NYA_PROJECT_VENDORS_LINUX_X86_64, },
#endif
    };

    NYA_EXPECT(nya_build(&build_rule), "while building the fuzz target");

    nya_log_info("Fuzzing %s. Findings land in %s; keep anything it finds under " FUZZ_CRASHES_ROOT "/%s.", requested, findings, requested);

    NYA_BuildRule run_rule = {
        .name    = "run_fuzz",
        .policy  = NYA_BUILD_ALWAYS,
        .command = {
            .program   = FUZZ_DRIVER_PROGRAM,
            .arguments = { "-i", corpus, "-o", findings, "--", binary },

            .environment = {
                /* The shared sanitizer settings plus the two AFL refuses to start without, which is why `./build run fuzz` could not run one: abort_on_error, because the driver watches for a child dying on a signal and asan exiting quietly with a status is a crash it never hears about, and symbolize=0, because resolving a backtrace per crash is far slower than the rest of an iteration. Spelled out here rather than added to SANITIZER_ENVIRONMENT: everything else that runs an instrumented binary wants asan's own symbolized report and not a bare SIGABRT. */
                "ASAN_OPTIONS=suppressions=./.sanitizers/asan.supp:detect_leaks=1:strict_string_checks=1:halt_on_error=1:abort_on_error=1:symbolize=0",
                "LSAN_OPTIONS=suppressions=./.sanitizers/lsan.supp:symbolize=0",
                "TSAN_OPTIONS=suppressions=./.sanitizers/tsan.supp:symbolize=0",
                "UBSAN_OPTIONS=suppressions=./.sanitizers/ubsan.supp:print_stacktrace=1:halt_on_error=1:abort_on_error=1:symbolize=0",

                // AFL refuses to start against an asan build unless it is told the memory limit is deliberate: asan reserves terabytes of address space, which looks like a runaway target to the driver's own accounting.
                "AFL_MAP_SIZE=262144",
                "AFL_I_DONT_CARE_ABOUT_MISSING_CRASHES=1",

                // AFL stops on a CPU governor that is not `performance`, which is every laptop. It is a warning about throughput and not about correctness, and refusing to fuzz at all because the machine might fuzz slowly is worse than fuzzing slowly.
                "AFL_SKIP_CPUFREQ=1",
            },
        },
    };

    NYA_EXPECT(nya_build(&run_rule), "while running afl-fuzz");
}

NYA_ConstCString fuzz_completion_target_name(u32 index) {
    /* Listed once and cached, since completion asks for one name at a time. Same arrangement as example_completion_name, and for the same reason. */
    static NYA_Arena*             arena   = nullptr;
    static NYA_ArrayᐸNYA_Stringᐳ* targets = nullptr;

    if (targets == nullptr) {
        arena   = nya_arena_create(.name = "fuzz_completion");
        targets = _fuzz_discover(arena);
    }

    if (index >= targets->length) return nullptr;

    return nya_string_to_cstring(arena, &targets->items[index]);
}

/* PRIVATE API IMPLEMENTATION */

b8 _fuzz_collect_targets(NYA_ConstCString path, const NYA_DirectoryEntry* entry, void* user_data) {
    NYA_ArrayᐸNYA_Stringᐳ* targets = (NYA_ArrayᐸNYA_Stringᐳ*)user_data;

    if (entry->type != NYA_FILE_TYPE_FILE) return true;

    // the walk gives the path; the name is its last component, which is what carries the prefix.
    NYA_String* name = nya_path_basename(nya_arena_global, path);

    if (!nya_string_starts_with(name, FUZZ_TARGET_PREFIX)) return true;
    if (!nya_string_ends_with(name, ".c")) return true;

    nya_string_strip_prefix(name, FUZZ_TARGET_PREFIX);
    nya_string_strip_suffix(name, ".c");

    nya_array_push_back(targets, *name);
    return true;
}

NYA_ArrayᐸNYA_Stringᐳ* _fuzz_discover(NYA_Arena* arena) {
    NYA_ArrayᐸNYA_Stringᐳ* targets = nya_array_create(arena, NYA_String);

    if (!nya_filesystem_is_directory(FUZZ_DIRECTORY)) return targets;

    NYA_EXPECT(nya_filesystem_walk(arena, FUZZ_DIRECTORY, _fuzz_collect_targets, targets));
    nya_array_sort(targets, _fuzz_compare_names);

    return targets;
}

s32 _fuzz_compare_names(const NYA_String* a, const NYA_String* b) {
    u64 shared     = nya_min(a->length, b->length);
    s32 difference = nya_memcmp(a->items, b->items, shared);
    if (difference != 0) return difference < 0 ? -1 : 1;

    if (a->length == b->length) return 0;
    return a->length < b->length ? -1 : 1;
}

b8 _fuzz_program_exists(NYA_ConstCString program) {
    NYA_Command probe = {
        .flags     = NYA_COMMAND_FLAG_OUTPUT_SUPPRESS,
        .program   = program,
        .arguments = { "--version" },
    };

    NYA_Error probed = nya_command_run(&probe);

    // afl-fuzz answers --version with a usage banner and a non-zero code depending on the build, so running at all is the test rather than the exit code.
    return probed.ok;
}

NYA_ConstCString _fuzz_compiler_program(void) {
    static b8               resolved = false;
    static NYA_ConstCString program  = nullptr;

    if (resolved) return program;
    resolved = true;

    NYA_ConstCString requested = getenv(FUZZ_COMPILER_ENV);
    if (requested != nullptr && requested[0] != '\0') {
        program = _fuzz_program_exists(requested) ? requested : nullptr;

        if (program == nullptr) nya_log_error("%s names '%s', which does not run.", FUZZ_COMPILER_ENV, requested);
        return program;
    }

    if (_fuzz_program_exists(FUZZ_COMPILER_PROGRAM)) program = FUZZ_COMPILER_PROGRAM;

    return program;
}
