#include "build/hooks/hooks.h"
/**/
#include "build/asset/asset.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PUBLIC API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/*
 * ─────────────────────────────────────────────────────────
 * BUILD
 * ─────────────────────────────────────────────────────────
 */

void hook_create_build_directory(NYA_BuildRule* rule) {
    nya_assert(rule != nullptr);
    nya_assert(rule->command.working_directory != nullptr, "hook_create_build_directory needs a working_directory to create.");

    NYA_EXPECT(nya_filesystem_create_directory(rule->command.working_directory), "while creating a vendor build directory");
}

/**
 * Moves input_file to output_file.
 *
 * Exists so rules do not have to shell out to `mv`, which does not exist on Windows. Vendors that
 * build in tree use this to move an archive aside so the other target can build into the same path.
 * */
/** Reads the value of a `KEY:TYPE=VALUE` line out of a CMakeCache.txt. */
NYA_INTERNAL NYA_CString cmake_cache_value(NYA_Arena* arena, const NYA_String* cache, NYA_ConstCString key) {
    NYA_ArrayᐸNYA_Stringᐳ* lines = nya_string_split_lines(arena, cache);

    nya_array_foreach (lines, line) {
        if (!nya_string_starts_with(line, key)) continue;

        NYA_CString text  = nya_string_to_cstring(arena, line);
        NYA_CString equals = strchr(text, '=');
        if (equals == nullptr) continue;

        return equals + 1;
    }

    return nullptr;
}

void hook_invalidate_stale_cmake_cache(NYA_BuildRule* rule) {
    nya_assert(rule != nullptr);

    // The build directory is whatever follows -B, so this works for every cmake rule without each
    // of them having to repeat the path.
    NYA_ConstCString build_directory = nullptr;
    for (u64 i = 0; i + 1 < NYA_COMMAND_MAX_ARGUMENTS; i++) {
        NYA_ConstCString argument = rule->command.arguments[i];
        if (!argument) break;
        if (!nya_string_equals((NYA_CString)argument, "-B")) continue;

        build_directory = rule->command.arguments[i + 1];
        break;
    }

    if (build_directory == nullptr) return;

    NYA_Arena* arena = nya_arena_create();
    defer      nya_arena_destroy(arena);

    NYA_String* cache_path = nya_path_join(arena, build_directory, "CMakeCache.txt");
    NYA_CString cache_file = nya_string_to_cstring(arena, cache_path);

    if (!nya_filesystem_exists(cache_file)) return;

    NYA_String* cache = nya_string_create(arena);
    if (nya_file_read(cache_file, cache).kind != NYA_ERROR_NONE) return;

    // Only the two that make cmake fail before it can tell you anything useful.
    NYA_ConstCString keys[] = { "CMAKE_MAKE_PROGRAM:FILEPATH", "CMAKE_C_COMPILER:FILEPATH" };

    for (u32 i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
        NYA_CString value = cmake_cache_value(arena, cache, keys[i]);
        if (value == nullptr || value[0] != '/') continue; // relative or absent, nothing to verify
        if (nya_filesystem_exists(value)) continue;

        printf("[STALE CACHE] %s records %s = %s, which does not exist here. Reconfiguring.\n", cache_file, keys[i], value);
        NYA_EXPECT(nya_filesystem_delete_recursive(build_directory), "while discarding a stale cmake build directory");
        return;
    }
}

void hook_move_file(NYA_BuildRule* rule) {
    nya_assert(rule != nullptr);
    nya_assert(rule->input_file != nullptr, "hook_move_file needs an input_file to move from.");
    nya_assert(rule->output_file != nullptr, "hook_move_file needs an output_file to move to.");

    NYA_EXPECT(nya_filesystem_move(rule->input_file, rule->output_file), "while moving a vendor artifact");
}

/**
 * Rewrites a relative -DCMAKE_PREFIX_PATH= argument into an absolute one.
 *
 * cmake resolves a relative CMAKE_PREFIX_PATH against the build directory rather than the working
 * directory, so a vendor pointing at another vendor's output with "./vendor/..." silently fails to
 * find it. When that happens during a cross compile the consequences are confusing rather than
 * loud: find_package falls back to the host's copy and glibc headers end up in a mingw compile.
 * */
void hook_absolutize_cmake_prefix_path(NYA_BuildRule* rule) {
    nya_assert(rule != nullptr);

    NYA_ConstCString flag        = "-DCMAKE_PREFIX_PATH=";
    u64              flag_length = strlen(flag);

    char working_directory[4096];
    if (getcwd(working_directory, sizeof(working_directory)) == nullptr) return;

    for (u64 i = 0; i < NYA_COMMAND_MAX_ARGUMENTS; i++) {
        NYA_ConstCString argument = rule->command.arguments[i];
        if (!argument) break;
        if (strncmp(argument, flag, flag_length) != 0) continue;

        NYA_ConstCString value = argument + flag_length;
        if (value[0] == '/') continue; // already absolute
        if (value[0] == '.' && value[1] == '/') value += 2;

        NYA_String* absolute       = nya_string_sprintf(nya_arena_global, "%s%s/%s", flag, working_directory, value);
        rule->command.arguments[i] = nya_string_to_cstring(nya_arena_global, absolute);
    }
}

/**
 * Expands the token %CWD% in any argument to the absolute working directory.
 *
 * Several tools resolve relative paths against something other than where the build was invoked
 * from, cmake being the usual offender, so an argument that has to be absolute can be written with
 * this marker instead of being hardcoded.
 * */
void hook_expand_cwd(NYA_BuildRule* rule) {
    nya_assert(rule != nullptr);

    NYA_ConstCString marker        = "%CWD%";
    u64              marker_length = strlen(marker);

    char working_directory[4096];
    if (getcwd(working_directory, sizeof(working_directory)) == nullptr) return;

    for (u64 i = 0; i < NYA_COMMAND_MAX_ARGUMENTS; i++) {
        NYA_ConstCString argument = rule->command.arguments[i];
        if (!argument) break;

        NYA_ConstCString found = strstr(argument, marker);
        if (!found) continue;

        NYA_String* expanded =
            nya_string_sprintf(nya_arena_global, "%.*s%s%s", (int)(found - argument), argument, working_directory, found + marker_length);
        rule->command.arguments[i] = nya_string_to_cstring(nya_arena_global, expanded);
    }
}

/**
 * Copies input_file to output_file.
 *
 * Where hook_move_file would be wrong: lz4 leaves lib/liblz4.a as a *relative* symlink into its
 * cachedObjs directory, so moving it one level up succeeds and silently leaves a dangling link.
 * Copying follows the link and produces a real archive.
 * */
void hook_copy_file(NYA_BuildRule* rule) {
    nya_assert(rule != nullptr);
    nya_assert(rule->input_file != nullptr, "hook_copy_file needs an input_file to copy from.");
    nya_assert(rule->output_file != nullptr, "hook_copy_file needs an output_file to copy to.");

    NYA_EXPECT(nya_filesystem_copy(rule->input_file, rule->output_file), "while copying a vendor artifact");
}

void hook_add_version_flag_and_git_hash(NYA_BuildRule* rule) {
    nya_assert(rule != nullptr);

    static b8          initialized = false;
    static NYA_CString GIT_HASH_FLAG;
    static NYA_CString VERSION_FLAG;

    if (!initialized) {
        NYA_Command git_hash_command = {
            .arena     = nya_arena_global,
            .flags     = NYA_COMMAND_FLAG_OUTPUT_CAPTURE,
            .program   = "git",
            .arguments = { "rev-parse", "HEAD" },
        };
        NYA_EXPECT(nya_command_run(&git_hash_command));
        nya_assert(git_hash_command.exit_code == 0, "Failed to get git commit hash.");

        nya_string_trim_whitespace(git_hash_command.stdout_content);
        NYA_CString git_hash      = nya_string_to_cstring(nya_arena_global, git_hash_command.stdout_content);
        NYA_String* git_hash_flag = nya_string_sprintf(nya_arena_global, "-DGIT_COMMIT=\"%s\"", git_hash);
        NYA_String* version_flag  = nya_string_sprintf(nya_arena_global, "-DVERSION=\"%s\"", VERSION);
        GIT_HASH_FLAG             = nya_string_to_cstring(nya_arena_global, git_hash_flag);
        VERSION_FLAG              = nya_string_to_cstring(nya_arena_global, version_flag);
        initialized               = true;
    }

    u64 length = 0;
    while (length < NYA_COMMAND_MAX_ARGUMENTS && rule->command.arguments[length] != nullptr) length++;
    nya_assert(length < NYA_COMMAND_MAX_ARGUMENTS - 2, "Not enough space to add version flags.");
    rule->command.arguments[length + 0] = GIT_HASH_FLAG;
    rule->command.arguments[length + 1] = VERSION_FLAG;
}

void hook_remove_output_file(NYA_BuildRule* rule) {
    nya_assert(rule != nullptr);
    nya_assert(rule->output_file);

    NYA_EXPECT(nya_filesystem_delete(rule->output_file));
}

// we just need to move everything into the right place
void hook_bundle_project(NYA_BuildRule* rule) {
    nya_assert(rule != nullptr);

    NYA_ConstCString dist_path    = "./dist/";
    NYA_ConstCString linux_path   = "./dist/" PROJECT_NAME "." VERSION ".linux-x86_64/";
    NYA_ConstCString windows_path = "./dist/" PROJECT_NAME "." VERSION ".windows-x86_64/";

    NYA_Command clean_dist_command = {
        .program   = "rm",
        .arguments = { "-rf", dist_path },
    };
    NYA_EXPECT(nya_command_run(&clean_dist_command));
    nya_assert(clean_dist_command.exit_code == 0, "Failed to clean dist directory.");

    NYA_Command create_dirs_command = {
    .program   = "mkdir",
    .arguments = { "-p", linux_path, windows_path, },
  };
    NYA_EXPECT(nya_command_run(&create_dirs_command));
    nya_assert(create_dirs_command.exit_code == 0, "Failed to create dist directories.");

    // LINUX

    NYA_Command copy_linux_binary_command = {
    .program   = "cp",
    .arguments = { LINUX_X86_64_BINARY, linux_path, },
  };
    NYA_EXPECT(nya_command_run(&copy_linux_binary_command));
    nya_assert(copy_linux_binary_command.exit_code == 0, "Failed to copy linux binary.");

    NYA_Command copy_steam_sdk_linux_command = {
    .program   = "cp",
    .arguments = { "./vendor/steam/redistributable_bin/linux64/libsteam_api.so", linux_path, },
  };
    NYA_EXPECT(nya_command_run(&copy_steam_sdk_linux_command));
    nya_assert(copy_steam_sdk_linux_command.exit_code == 0, "Failed to copy steam sdk for linux.");

    NYA_Command zip_linux_command = {
        .program           = "zip",
        .arguments         = { "-r", "../" PROJECT_NAME "." VERSION ".linux-x86_64.zip", "." },
        .working_directory = linux_path,
    };
    NYA_EXPECT(nya_command_run(&zip_linux_command));
    nya_assert(zip_linux_command.exit_code == 0, "Failed to create linux zip.");

    // WINDOWS

    NYA_Command copy_windows_binary_command = {
    .program   = "cp",
    .arguments = { WINDOWS_X86_64_BINARY, windows_path, },
  };
    NYA_EXPECT(nya_command_run(&copy_windows_binary_command));
    nya_assert(copy_windows_binary_command.exit_code == 0, "Failed to copy windows binary.");

    NYA_Command copy_steam_sdk_windows_command = {
    .program   = "cp",
    .arguments = { "./vendor/steam/redistributable_bin/win64/steam_api64.dll", windows_path, },
  };
    NYA_EXPECT(nya_command_run(&copy_steam_sdk_windows_command));
    nya_assert(copy_steam_sdk_windows_command.exit_code == 0, "Failed to copy steam sdk for windows.");

    NYA_Command zip_windows_command = {
        .program           = "zip",
        .arguments         = { "-r", "../" PROJECT_NAME "." VERSION ".windows-x86_64.zip", "." },
        .working_directory = windows_path,
    };
    NYA_EXPECT(nya_command_run(&zip_windows_command));
    nya_assert(zip_windows_command.exit_code == 0, "Failed to create windows zip.");
}

void hook_convert_perf_data_to_plain(NYA_BuildRule* rule) {
    nya_assert(rule != nullptr);

    NYA_Command convert_command = {
        .arena     = nya_arena_global,
        .flags     = NYA_COMMAND_FLAG_OUTPUT_CAPTURE,
        .program   = "perf",
        .arguments = { "script", "-i", "./perf.data" },
    };
    NYA_EXPECT(nya_command_run(&convert_command));
    nya_assert(convert_command.exit_code == 0, "Failed to convert perf data to plain text.");

    NYA_EXPECT(nya_file_write("./perf.data.txt", convert_command.stdout_content));
}

void hook_insert_integrity_hash(NYA_BuildRule* rule) {
    nya_assert(rule != nullptr);
    nya_assert(rule->output_file != nullptr, "Output file must be specified to insert integrity hash.");

    u64 integrity_hash = 0;
    NYA_EXPECT(nya_integrity_patch(rule->output_file, &integrity_hash), "while inserting the integrity hash into '%s'", rule->output_file);
}

/*
 * ─────────────────────────────────────────────────────────
 * ASSET
 * ─────────────────────────────────────────────────────────
 */

void hook_compile_shaders(NYA_BuildRule* rule) {
    nya_assert(rule != nullptr);

    nya_asset_compile_shaders();
}

void hook_index_assets(NYA_BuildRule* rule) {
    nya_assert(rule != nullptr);

    nya_asset_index();
}

void hook_bundle_assets(NYA_BuildRule* rule) {
    nya_assert(rule != nullptr);

    nya_asset_bundle();
}
