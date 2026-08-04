#include "build/build.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API DECLARATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

NYA_INTERNAL b8  _test_collect_sources(NYA_ConstCString path, const NYA_DirectoryEntry* entry, void* user_data);
NYA_INTERNAL s32 _test_compare_paths(const NYA_String* a, const NYA_String* b);

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PUBLIC API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

void test_runner(NYA_ArgCommand* command) {
    nya_assert(command != nullptr);

    NYA_ArgParameter* test_files = command->parameters[0];
    nya_assert(test_files != nullptr);
    nya_assert(nya_string_equals(test_files->name, "tests"));

    // Walked rather than shelled out to `find`, which on Windows off msys2 resolves to the unrelated
    // find.exe in System32 and would take these arguments as a text search rather than fail.
    NYA_ArrayᐸNYA_Stringᐳ* tests = nya_array_create(nya_arena_global, NYA_String);
    NYA_EXPECT(nya_filesystem_walk(nya_arena_global, "./tests", _test_collect_sources, tests));
    nya_array_sort(tests, _test_compare_paths);

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
        NYA_BuildRule build_test_rule = {
        .name        = nya_string_to_cstring(nya_arena_global, build_test_name),
        .policy      = NYA_BUILD_ALWAYS,
        .output_file = test_binary,

        .command = {
            .program   = CC,
            .arguments = {
                test_cstr,
                "-o", test_binary,
                CFLAGS,
                WARNINGS,
                INCLUDE_PATHS,
                LINKER_FLAGS,
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
        .vendors         = { &vendor_sdl_linux_x86_64, &vendor_sdl_image_linux_x86_64, &vendor_sdl_ttf_linux_x86_64,
                             &vendor_sdl_mixer_linux_x86_64, &vendor_libbacktrace_linux_x86_64, },
    };

        NYA_String* run_test_name = nya_string_sprintf(nya_arena_global, "run_test:%s", test_binary);
        NYA_BuildRule run_test_rule = {
        .name        = nya_string_to_cstring(nya_arena_global, run_test_name),
        .policy      = NYA_BUILD_ALWAYS,
        .output_file = test_binary,

        .command = {
            .program     = test_binary,
            // The shared set from build.h. These four were spelled out here as well, identically,
            // which is one edit away from a test running under different options than a profiled run.
            .environment = { SANITIZER_ENVIRONMENT, },
        },

        .dependencies      = { &build_test_rule, },
        .post_build_hooks  = { &hook_remove_output_file, },
    };

        NYA_EXPECT(nya_build(&run_test_rule));
    }
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
