#include "build/build.h"

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

        NYA_CString text   = nya_string_to_cstring(arena, line);
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
    if (!nya_file_read(cache_file, cache).ok) return;

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

void hook_convert_perf_data_to_plain(NYA_BuildRule* rule) {
    nya_assert(rule != nullptr);

    /*
     * Nothing recorded yet is not a failure.
     *
     * This runs both after a profiled run and before the viewer opens, and the second of those can be
     * reached with no perf.data at all — `./build perf` on a clean checkout. Asserting there would abort
     * the build system over a missing file the user is about to be told about anyway.
     */
    if (!nya_filesystem_exists("./perf.data")) {
        nya_log_warn("There is no ./perf.data to convert; run './build run debug' first.");
        return;
    }

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

/** An environment variable if it is set to something, otherwise the compiled in default. */
NYA_INTERNAL NYA_ConstCString signing_setting(NYA_ConstCString variable, NYA_ConstCString fallback) {
    NYA_ConstCString value = getenv(variable);
    return (value != nullptr && value[0] != '\0') ? value : fallback;
}

void hook_sign_windows_executable(NYA_BuildRule* rule) {
    nya_assert(rule != nullptr);
    nya_assert(rule->output_file != nullptr, "hook_sign_windows_executable needs an output_file to sign.");

    NYA_ConstCString pfx       = signing_setting(SIGNING_PFX_PATH_ENV, SIGNING_PFX_PATH);
    NYA_ConstCString password  = signing_setting(SIGNING_PFX_PASSWORD_ENV, SIGNING_PFX_PASSWORD);
    NYA_ConstCString timestamp = signing_setting(SIGNING_TIMESTAMP_URL_ENV, SIGNING_TIMESTAMP_URL);

    if (!nya_filesystem_exists(pfx)) {
        nya_log_warn("No signing certificate at '%s', leaving %s unsigned. See the README.", pfx, rule->output_file);
        return;
    }

    // Captured rather than shown: signing narrates its progress and its timestamp server round trip
    // on stdout, which is noise in a build log. Kept so that a failure can still say what went wrong.
#if OS_WINDOWS
    // signtool edits the file in place, so there is nothing to move afterwards.
    NYA_Command command = {
        .flags     = NYA_COMMAND_FLAG_OUTPUT_CAPTURE,
        .arena     = nya_arena_global,
        .program   = "signtool",
        .arguments = {
            "sign",
            "/f", pfx,
            "/p", password,
            "/fd", "SHA256",
            // /tr, not /t: an RFC 3161 countersignature is what keeps already shipped binaries
            // verifying after the certificate behind them expires.
            "/tr", timestamp,
            "/td", "SHA256",
            rule->output_file,
        },
    };
#else
    // osslsigncode refuses to write over its input, so it signs to a temporary beside the binary
    // which then replaces it.
    NYA_String* signed_path_string = nya_string_sprintf(nya_arena_global, "%s.signed", rule->output_file);
    NYA_CString signed_path        = nya_string_to_cstring(nya_arena_global, signed_path_string);

    NYA_Command command = {
        .flags     = NYA_COMMAND_FLAG_OUTPUT_CAPTURE,
        .arena     = nya_arena_global,
        .program   = "osslsigncode",
        .arguments = {
            "sign",
            "-pkcs12", pfx,
            "-pass", password,
            "-ts", timestamp,
            "-h", "sha256",
            "-in", rule->output_file,
            "-out", signed_path,
        },
    };
#endif

    NYA_Error result = nya_command_run(&command);

    if (!result.ok || command.exit_code != 0) {
        // A missing signing tool is the ordinary case on a machine that only builds to run the
        // game, so it cannot fail the build. A release that has to be signed is a CI concern, and
        // CI installs the tool.
        NYA_ConstCString reason = !result.ok ? (NYA_ConstCString)result.message : "the signing tool reported failure";
        nya_log_warn("Could not sign %s: %s. Leaving it unsigned.", rule->output_file, reason);

        // The captured output, which is the only place the actual reason is written.
        if (command.stderr_content != nullptr && command.stderr_content->length > 0) {
            nya_log_warn("%s", nya_string_to_cstring(nya_arena_global, command.stderr_content));
        }
        if (command.stdout_content != nullptr && command.stdout_content->length > 0) {
            nya_log_warn("%s", nya_string_to_cstring(nya_arena_global, command.stdout_content));
        }

#if !OS_WINDOWS
        if (nya_filesystem_exists(signed_path)) NYA_EXPECT(nya_filesystem_delete(signed_path), "while discarding a failed signature");
#endif
        nya_command_destroy(&command);
        return;
    }

#if !OS_WINDOWS
    NYA_EXPECT(nya_filesystem_move(signed_path, rule->output_file), "while replacing the unsigned binary with the signed one");
#endif

    nya_command_destroy(&command);
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

void hook_generate_strings(NYA_BuildRule* rule) {
    nya_unused(rule);

    nya_i18n_generate();
}

void hook_generate_reflection(NYA_BuildRule* rule) {
    nya_unused(rule);

    nya_reflection_generate();
}

void hook_index_assets(NYA_BuildRule* rule) {
    nya_assert(rule != nullptr);

    nya_asset_index();
}

void hook_bundle_assets(NYA_BuildRule* rule) {
    nya_assert(rule != nullptr);

    nya_asset_bundle();
}
