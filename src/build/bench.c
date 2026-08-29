#include "build/build.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * INTERNALS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/** The same shape as _test_collect_sources, and for the same reasons. See test.c. */
NYA_INTERNAL b8 _bench_collect_sources(NYA_ConstCString path, const NYA_DirectoryEntry* entry, void* user_data) {
    NYA_ArrayᐸNYA_Stringᐳ* sources = (NYA_ArrayᐸNYA_Stringᐳ*)user_data;

    if (entry->type != NYA_FILE_TYPE_FILE) return true;

    NYA_String* file = nya_string_from(nya_arena_global, path);
    if (!nya_string_ends_with(file, ".c")) return true;

    // The rules below use these paths verbatim as input and output files, and nya_path_join has
    // normalised the leading "./" away.
    if (!nya_string_starts_with(file, "./")) nya_string_extend_front(file, "./");

    nya_array_push_back(sources, *file);
    return true;
}

/** Byte order, so the run order is the same on every machine rather than the filesystem's. */
NYA_INTERNAL s32 _bench_compare_paths(const NYA_String* a, const NYA_String* b) {
    u64 shared     = nya_min(a->length, b->length);
    s32 difference = nya_memcmp(a->items, b->items, shared);
    if (difference != 0) return difference < 0 ? -1 : 1;

    if (a->length == b->length) return 0;
    return a->length < b->length ? -1 : 1;
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PUBLIC API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

void bench_runner(NYA_ArgCommand* command) {
    nya_assert(command != nullptr);

    NYA_ArgParameter* filters = command->parameters[0];

    if (!nya_filesystem_is_directory("./bench")) {
        nya_log_info("No bench/ directory; nothing to run.");
        return;
    }

    NYA_ArrayᐸNYA_Stringᐳ* sources = nya_array_create(nya_arena_global, NYA_String);
    NYA_EXPECT(nya_filesystem_walk(nya_arena_global, "./bench", _bench_collect_sources, sources));
    nya_array_sort(sources, _bench_compare_paths);

    nya_array_foreach (sources, original) {
        NYA_String* source      = nya_string_clone(nya_arena_global, original);
        NYA_CString source_cstr = nya_string_to_cstring(nya_arena_global, source);

        b8 should_run = filters == nullptr || filters->values_count == 0;
        for (u32 i = 0; filters != nullptr && i < filters->values_count; i++) {
            if (nya_string_contains(source, filters->values[i].as_string)) {
                should_run = true;
                break;
            }
        }
        if (!should_run) continue;

        nya_string_strip_suffix(source, ".c");
        NYA_CString binary = nya_string_to_cstring(nya_arena_global, source);

        NYA_String*    build_name = nya_string_sprintf(nya_arena_global, "build_bench:%s", binary);
        NYA_BuildRule* build_rule = nya_arena_alloc(nya_arena_global, sizeof(NYA_BuildRule));

        *build_rule = (NYA_BuildRule){
            .name        = nya_string_to_cstring(nya_arena_global, build_name),
            .policy      = NYA_BUILD_ALWAYS,
            .output_file = binary,

            .command = {
                .program   = CC,
                .arguments = {
                    source_cstr,
                    "-o", binary,
                    CFLAGS,
                    WARNINGS,
                    INCLUDE_PATHS,
                    LINKER_FLAGS,
                    FLAGS_PLUGINS,
                    // FLAGS_BENCH, not FLAGS_TEST: optimised, headless, and no NYA_TESTING.
                    FLAGS_BENCH,
                    // FLAGS_HOST_NATIVE_BENCH, not FLAGS_HOST_NATIVE: the latter bundles FLAGS_SANITIZE,
                    // and a sanitized benchmark measures the sanitizer. That is the whole point of this
                    // command, and using the wrong macro here silently produced sanitized numbers.
                    FLAGS_HOST_NATIVE_BENCH,
                },
            },

            .pre_build_hooks = { &hook_add_version_flag_and_git_hash, },
#if OS_WINDOWS
            .vendors         = { NYA_PROJECT_VENDORS_WINDOWS_X86_64, },
#else
            .vendors         = { NYA_PROJECT_VENDORS_LINUX_X86_64, },
#endif
        };

        NYA_String*    run_name = nya_string_sprintf(nya_arena_global, "run_bench:%s", binary);
        NYA_BuildRule* run_rule = nya_arena_alloc(nya_arena_global, sizeof(NYA_BuildRule));

        *run_rule = (NYA_BuildRule){
            .name    = nya_string_to_cstring(nya_arena_global, run_name),
            .policy  = NYA_BUILD_ALWAYS,
            .command = { .program = binary },

            // Removed after running, like a test binary: it is a build artifact and the repo already
            // ignores the pattern.
            .post_build_hooks = { &hook_remove_output_file, },
            .output_file      = binary,
        };

        /*
         * Built and run one at a time, deliberately.
         *
         * The test runner compiles in parallel because a test's result does not depend on timing. A
         * benchmark's does: two running at once contend for cache, memory bandwidth and cores, and both
         * report numbers that mean nothing.
         */
        NYA_EXPECT(nya_build(build_rule));
        NYA_EXPECT(nya_build(run_rule));
    }
}
