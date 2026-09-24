#include "build/build.h"

#if !OS_WINDOWS
#include <ftw.h>
#endif

/* PUBLIC API IMPLEMENTATION */

/* BUILD */

void hook_create_build_directory(NYA_BuildRule* rule) {
    nya_assert(rule != nullptr);
    nya_assert(rule->command.working_directory != nullptr, "hook_create_build_directory needs a working_directory to create.");

    NYA_EXPECT(nya_filesystem_create_directory(rule->command.working_directory), "while creating a vendor build directory");
}

/**
 * Moves input_file to output_file.
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

/**
 * Reads the value of a cache entry by its bare name, whatever type cmake recorded it under.
 *
 * `cmake_cache_value` wants the `KEY:TYPE` a caller already knows. A `-DKEY=VALUE` on a command line
 * carries no type, and cmake picks one itself, so a lookup from the command line has to match on the
 * name up to the colon.
 * */
NYA_INTERNAL NYA_CString cmake_cache_value_untyped(NYA_Arena* arena, const NYA_String* cache, NYA_ConstCString key) {
    NYA_ArrayᐸNYA_Stringᐳ* lines = nya_string_split_lines(arena, cache);

    const u64 key_length = strlen(key);

    nya_array_foreach (lines, line) {
        NYA_CString text = nya_string_to_cstring(arena, line);

        if (strncmp(text, key, key_length) != 0) continue;
        if (text[key_length] != ':') continue; // a longer name that merely starts the same way

        NYA_CString equals = strchr(text + key_length, '=');
        if (equals == nullptr) continue;

        return equals + 1;
    }

    return nullptr;
}

/** Case-insensitive equality. <strings.h> is not on this tool's include line. */
NYA_INTERNAL b8 cmake_same_word(NYA_ConstCString a, NYA_ConstCString b) {
    for (u64 i = 0;; i++) {
        const char left  = (a[i] >= 'a' && a[i] <= 'z') ? (char)(a[i] - ('a' - 'A')) : a[i];
        const char right = (b[i] >= 'a' && b[i] <= 'z') ? (char)(b[i] - ('a' - 'A')) : b[i];

        if (left != right) return false;
        if (left == '\0') return true;
    }
}

/**
 * Whether two cache values mean the same thing.
 *
 * Booleans are the whole reason this is not strcmp: cmake writes back the canonical `ON` or `OFF`
 * whatever spelling it was given, so `-DSDL_TESTS=0` against a cache holding `OFF` is not a change and
 * comparing the text would reconfigure on every build forever.
 * */
NYA_INTERNAL b8 cmake_truth_of(NYA_ConstCString value, OUT b8* out_truth) {
    NYA_ConstCString truths[] = { "ON", "TRUE", "YES", "Y", "1" };
    NYA_ConstCString falses[] = { "OFF", "FALSE", "NO", "N", "0", "IGNORE", "NOTFOUND", "" };

    for (u64 i = 0; i < sizeof(truths) / sizeof(truths[0]); i++) {
        if (!cmake_same_word(value, truths[i])) continue;
        *out_truth = true;

        return true;
    }

    for (u64 i = 0; i < sizeof(falses) / sizeof(falses[0]); i++) {
        if (!cmake_same_word(value, falses[i])) continue;
        *out_truth = false;

        return true;
    }

    return false;
}

NYA_INTERNAL b8 cmake_values_agree(NYA_ConstCString wanted, NYA_ConstCString cached) {
    if (nya_string_equals((NYA_CString)wanted, (NYA_CString)cached)) return true;

    /* A program named rather than located. cmake resolves CMAKE_C_COMPILER=clang against PATH and writes back /usr/sbin/clang, so the text never matches again and the whole of SDL was thrown away and rebuilt for a compiler that had not changed. */
    if (strchr(wanted, '/') == nullptr && cached[0] == '/') {
        NYA_ConstCString last = strrchr(cached, '/');
        if (last != nullptr && nya_string_equals((NYA_CString)(last + 1), (NYA_CString)wanted)) return true;
    }

    b8 wanted_truth = false;
    b8 cached_truth = false;

    if (!cmake_truth_of(wanted, &wanted_truth)) return false;
    if (!cmake_truth_of(cached, &cached_truth)) return false;

    return wanted_truth == cached_truth;
}

void hook_invalidate_stale_cmake_cache(NYA_BuildRule* rule) {
    nya_assert(rule != nullptr);

    // The build directory is whatever follows -B, so this works for every cmake rule without each of them having to repeat the path.
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

    /* And the case that actually bites: an option in the recipe that disagrees with the one the cache was built from. cmake applies a changed -D on a reconfigure for most variables, but not for all of them, and not for anything a CMakeLists only reads the first time through. Turning SDL_RENDER on is exactly that kind of change: it had to be found by a test opening a window that had never been able to open. A wipe is the only answer that is right for every variable, and it costs a full rebuild of one vendor on the builds where somebody actually edited an option. */
    for (u64 i = 0; i < NYA_COMMAND_MAX_ARGUMENTS; i++) {
        NYA_ConstCString argument = rule->command.arguments[i];
        if (argument == nullptr) break;
        if (strncmp(argument, "-D", 2) != 0) continue;

        NYA_ConstCString equals = strchr(argument + 2, '=');
        if (equals == nullptr) continue; // -DFOO with no value defines nothing cmake caches

        // The name is what lies between the -D and the first =, minus any :TYPE the recipe spelled out.
        NYA_CString name  = nya_arena_alloc(arena, (u64)(equals - argument) - 1);
        const u64   count = (u64)(equals - (argument + 2));
        nya_memcpy(name, argument + 2, count);
        name[count] = '\0';

        NYA_CString colon = strchr(name, ':');
        if (colon != nullptr) *colon = '\0';

        /* Only the last -D for a name decides, because that is what cmake does with it. The windows vendors set CMAKE_C_FLAGS_RELEASE twice, once in vendor_common.h and again in the mingw toolchain that adds -D__INTRINSIC_DEFINED___cpuidex to it, so comparing the earlier one against the cache reported a change on every build that ever reached this hook. */
        b8        overridden = false;
        const u64 name_length = strlen(name);

        for (u64 j = i + 1; j < NYA_COMMAND_MAX_ARGUMENTS; j++) {
            NYA_ConstCString later = rule->command.arguments[j];
            if (later == nullptr) break;
            if (strncmp(later, "-D", 2) != 0) continue;
            if (strncmp(later + 2, name, name_length) != 0) continue;

            // The same name, and not merely one that starts with it.
            const char after = later[2 + name_length];
            if (after != '=' && after != ':') continue;

            overridden = true;
            break;
        }

        if (overridden) continue;

        NYA_CString cached = cmake_cache_value_untyped(arena, cache, name);
        if (cached == nullptr) continue; // not in the cache: cmake will put it there, nothing is stale

        if (cmake_values_agree(equals + 1, cached)) continue;

        printf("[STALE CACHE] %s records %s = %s, and this build wants %s. Reconfiguring from scratch.\n", cache_file, name, cached, equals + 1);
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
 * */
void hook_copy_file(NYA_BuildRule* rule) {
    nya_assert(rule != nullptr);
    nya_assert(rule->input_file != nullptr, "hook_copy_file needs an input_file to copy from.");
    nya_assert(rule->output_file != nullptr, "hook_copy_file needs an output_file to copy to.");

    NYA_EXPECT(nya_filesystem_copy(rule->input_file, rule->output_file), "while copying a vendor artifact");
}

void hook_create_output_directory(NYA_BuildRule* rule) {
    nya_assert(rule != nullptr);
    nya_assert(rule->output_file != nullptr, "hook_create_output_directory needs an output_file.");

    NYA_String* directory = nya_path_dirname(nya_arena_global, rule->output_file);
    NYA_EXPECT(nya_filesystem_create_directory(nya_string_to_cstring(nya_arena_global, directory)), "while creating the directory for '%s'", rule->output_file);
}

/** The launcher COMPILER_CACHE_ENV asks for, or nullptr for none. Resolved once, since the probe spawns a process. */
NYA_INTERNAL NYA_ConstCString _hook_compiler_cache_program(void) {
    static b8               resolved = false;
    static NYA_ConstCString program  = nullptr;

    if (resolved) return program;
    resolved = true;

    NYA_ConstCString requested = getenv(COMPILER_CACHE_ENV);
    if (requested != nullptr) {
        b8 disabled = requested[0] == '\0' || nya_string_equals(requested, "0") || nya_string_equals(requested, "off");
        program     = disabled ? nullptr : requested;
    } else {
        // the cache is optional, so a missing one is found out by running it rather than reported.
        NYA_Command probe = {
            .flags     = NYA_COMMAND_FLAG_OUTPUT_SUPPRESS,
            .program   = COMPILER_CACHE_PROGRAM,
            .arguments = { "--version" },
        };
        NYA_Error probed = nya_command_run(&probe);
        if (probed.ok && probe.exit_code == 0) program = COMPILER_CACHE_PROGRAM;
    }

    if (program != nullptr) nya_log_info("Compiling through %s.", program);

    return program;
}

void hook_use_compiler_cache(NYA_BuildRule* rule) {
    nya_assert(rule != nullptr);
    nya_assert(rule->command.program != nullptr);

    NYA_ConstCString launcher = _hook_compiler_cache_program();
    if (launcher == nullptr) return;

    // a rule built a second time is already launched through it.
    if (nya_string_equals(rule->command.program, launcher)) return;

    u32 count = 0;
    while (count < NYA_COMMAND_MAX_ARGUMENTS && rule->command.arguments[count] != nullptr) count++;
    nya_assert(count + 1 < NYA_COMMAND_MAX_ARGUMENTS, "No room to launch '%s' through the compiler cache.", rule->name);

    for (u32 i = count; i > 0; i--) rule->command.arguments[i] = rule->command.arguments[i - 1];
    rule->command.arguments[0] = rule->command.program;
    rule->command.program      = launcher;

    nya_assert(rule->command.arguments[count + 1] == nullptr);
}

void hook_add_version_flag(NYA_BuildRule* rule) {
    nya_assert(rule != nullptr);

    static NYA_CString VERSION_FLAG = nullptr;
    if (VERSION_FLAG == nullptr) VERSION_FLAG = nya_string_to_cstring(nya_arena_global, nya_string_sprintf(nya_arena_global, "-DVERSION=\"%s\"", VERSION));

    u64 length = 0;
    while (length < NYA_COMMAND_MAX_ARGUMENTS && rule->command.arguments[length] != nullptr) length++;
    nya_assert(length < NYA_COMMAND_MAX_ARGUMENTS - 1, "Not enough space to add the version flag.");
    rule->command.arguments[length] = VERSION_FLAG;
}

/**
 * Runs a git command, capturing it. False when git is missing or the repository refuses the question;
 * `out_exit_code` then says nothing. `out_stdout` is trimmed and may be empty.
 * */
NYA_INTERNAL b8 git_run(NYA_ConstCString const* arguments, OUT s32* out_exit_code, OUT NYA_String** out_stdout) {
    NYA_Command command = {
        .arena   = nya_arena_global,
        .flags   = NYA_COMMAND_FLAG_OUTPUT_CAPTURE,
        .program = "git",
    };
    for (u32 i = 0; i < NYA_COMMAND_MAX_ARGUMENTS && arguments[i] != nullptr; i++) command.arguments[i] = arguments[i];

    NYA_Error result = nya_command_run(&command);
    if (!result.ok) return false;

    *out_exit_code = command.exit_code;

    NYA_String* text = command.stdout_content != nullptr ? command.stdout_content : nya_string_create(nya_arena_global);
    nya_string_trim_whitespace(text);
    *out_stdout = text;

    return true;
}

void hook_add_build_info_flag(NYA_BuildRule* rule) {
    nya_assert(rule != nullptr);

    /* Resolved once per build tool run, since each of these spawns a process. The commit hash used to stay off because ccache's direct mode hashes the command line. That worry does not survive a look at when it actually changes: every artifact is one unity translation unit, so a new commit means changed sources and a cache miss regardless, and going back to a commit that was built before produces the same hash and hits again. Only the `-dirty` suffix flips often, and it flips exactly when the tree changed. A build *timestamp* is the flag that would genuinely destroy the cache, because it differs on every invocation, so it is not injected. base_crash.h reads the executable's own modification time instead, which is when the linker wrote it, which is the build time. */
    static b8          resolved         = false;
    static NYA_CString BUILD_INFO_FLAG  = nullptr;

    if (!resolved) {
        resolved = true;

        s32         exit_code = 0;
        NYA_String* commit    = nullptr;
        b8          answered  = git_run((NYA_ConstCString[]){ "rev-parse", "--short=12", "HEAD", nullptr }, &exit_code, &commit);

        if (answered && exit_code == 0 && commit->length > 0) {
            // `--quiet` prints nothing and exits 1 when the working tree differs from HEAD, which is what marks the build dirty. An unavailable answer is read as clean rather than as dirty, so a shallow or grafted checkout does not permanently label itself modified.
            NYA_String*      ignored = nullptr;
            NYA_ConstCString dirty   = "";
            if (git_run((NYA_ConstCString[]){ "diff", "--quiet", "HEAD", nullptr }, &exit_code, &ignored) && exit_code != 0) dirty = "-dirty";

            NYA_String* flag = nya_string_sprintf(nya_arena_global, "-DNYA_BUILD_COMMIT=\"" NYA_FMT_STRING "%s\"", NYA_FMT_STRING_ARG(commit), dirty);
            BUILD_INFO_FLAG  = nya_string_to_cstring(nya_arena_global, flag);
        } else {
            // A source tarball or an exported tree has no git. base_basic.h's default says "unknown".
            nya_log_warn("No git commit available; the build will not know which revision it came from.");
        }
    }

    if (BUILD_INFO_FLAG == nullptr) return;

    u64 length = 0;
    while (length < NYA_COMMAND_MAX_ARGUMENTS && rule->command.arguments[length] != nullptr) length++;
    nya_assert(length < NYA_COMMAND_MAX_ARGUMENTS - 1, "Not enough space to add the build info flag.");
    rule->command.arguments[length] = BUILD_INFO_FLAG;
}

void hook_add_version_resource_flags(NYA_BuildRule* rule) {
    nya_assert(rule != nullptr);

    static const NYA_ConstCString PARTS[] = { "MAJOR", "MINOR", "PATCH" };

    u32 length = 0;
    while (length < NYA_COMMAND_MAX_ARGUMENTS && rule->command.arguments[length] != nullptr) length++;
    nya_assert(length + 3 < NYA_COMMAND_MAX_ARGUMENTS, "Not enough space to add version resource flags.");

    // a suffix like -rc1 only shows in the version strings, the numeric fields have no room for it.
    NYA_ConstCString cursor = VERSION;
    for (u32 i = 0; i < 3; i++) {
        char* end = nullptr;
        u64   part = strtoull(cursor, &end, 10);
        nya_assert(end != cursor && (i == 2 || *end == '.'), "VERSION '%s' does not start with major.minor.patch.", VERSION);
        nya_assert(part <= U16_MAX, "VERSION '%s' has a part over 65535.", VERSION);

        rule->command.arguments[length + i] = nya_string_to_cstring(nya_arena_global, nya_string_sprintf(nya_arena_global, "-DNYA_RC_VERSION_%s=" FMTu64, PARTS[i], part));
        cursor                              = end + 1;
    }
}

void hook_remove_output_file(NYA_BuildRule* rule) {
    nya_assert(rule != nullptr);
    nya_assert(rule->output_file);

    // A file that is not there is already the state this asks for, which is what lets the hook run before a rule as well as after one: the first build of an archive has nothing to delete.
    if (!nya_filesystem_exists(rule->output_file)) return;

    NYA_EXPECT(nya_filesystem_delete(rule->output_file));
}

void hook_remove_input_file(NYA_BuildRule* rule) {
    nya_assert(rule != nullptr);
    nya_assert(rule->input_file);

    NYA_EXPECT(nya_filesystem_delete(rule->input_file));
}

#if !OS_WINDOWS
NYA_INTERNAL NYA_ConstCString _hook_relativize_root = nullptr;

NYA_INTERNAL s32 _hook_relativize_symlink(NYA_ConstCString path, const struct stat* status, s32 type, struct FTW* position) {
    nya_unused(status, position);
    if (type != FTW_SL) return 0;

    char target[4096];
    ssize_t length = readlink(path, target, sizeof(target) - 1);
    if (length <= 0 || target[0] != '/') return 0;
    target[length] = '\0';

    // one ".." per directory between the link and the root, then the target as the root sees it.
    NYA_String* relative = nya_string_create(nya_arena_global);
    for (NYA_ConstCString cursor = path + strlen(_hook_relativize_root) + 1; (cursor = strchr(cursor, '/')) != nullptr; cursor++) {
        nya_string_extend(relative, "../");
    }
    nya_string_extend(relative, target + 1);

    if (unlink(path) != 0 || symlink(nya_string_to_cstring(nya_arena_global, relative), path) != 0) return -1;
    return 0;
}

void hook_relativize_symlinks(NYA_BuildRule* rule) {
    nya_assert(rule != nullptr);
    nya_assert(rule->command.working_directory != nullptr, "hook_relativize_symlinks walks the rule's working_directory.");

    _hook_relativize_root = rule->command.working_directory;

    s32 walked = nftw(_hook_relativize_root, _hook_relativize_symlink, 64, FTW_PHYS);
    nya_assert(walked == 0, "Could not relativize the symlinks under '%s': %s", _hook_relativize_root, strerror(errno));
}

void hook_build_steamrt_vendors(NYA_BuildRule* rule) {
    nya_unused(rule);

    NYA_EXPECT(nya_vendor_steamrt_build(), "while building the Steam Runtime vendors");
}
#endif

void hook_convert_perf_data_to_plain(NYA_BuildRule* rule) {
    nya_assert(rule != nullptr);

    /* Nothing recorded yet is not a failure. */
    if (!nya_filesystem_exists("./perf.data")) {
        nya_log_warn("There is no ./perf.data to convert; run './build run profile' first.");
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

#if !OS_WINDOWS
void hook_verify_hardening(NYA_BuildRule* rule) {
    nya_assert(rule != nullptr);
    nya_assert(rule->output_file != nullptr, "hook_verify_hardening needs an output_file to inspect.");

    // One readelf pass over the dynamic section (-d), the program headers (-l) and the dynamic symbol table (--dyn-syms); -W keeps it from truncating wide lines. Every mitigation is read out of this text, so a toolchain that silently dropped one trips an assert here instead of shipping soft.
    NYA_Command readelf = {
        .arena     = nya_arena_global,
        .flags     = NYA_COMMAND_FLAG_OUTPUT_CAPTURE,
        .program   = "readelf",
        .arguments = { "-W", "-d", "-l", "--dyn-syms", rule->output_file },
    };
    NYA_EXPECT(nya_command_run(&readelf), "while running readelf over '%s' to verify its hardening", rule->output_file);
    nya_assert_always(readelf.exit_code == 0, "readelf could not read '%s' to verify its hardening.", rule->output_file);
    nya_assert_always(readelf.stdout_content != nullptr && readelf.stdout_content->length > 0, "readelf produced no output for '%s'.", rule->output_file);

    NYA_String* elf = readelf.stdout_content;

    // Full RELRO is -z relro (the GNU_RELRO segment) plus -z now (immediate binding). readelf prints BIND_NOW in DT_FLAGS and NOW in DT_FLAGS_1; either proves -z now took. Without it the GOT stays writable and RELRO is only partial.
    nya_assert_always(nya_string_contains(elf, "GNU_RELRO"),
                      "%s has no GNU_RELRO segment: -Wl,-z,relro did not take.", rule->output_file);
    nya_assert_always(nya_string_contains(elf, "BIND_NOW") || nya_string_contains(elf, "NOW"),
                      "%s has no BIND_NOW / FLAGS_1 NOW: -Wl,-z,now did not take, so RELRO is only partial.", rule->output_file);

    // NX stack: a PT_GNU_STACK segment that is not executable. readelf writes segment flags as e.g. RW, or RWE when executable; an executable segment is exactly what -z noexecstack forbids on the stack.
    nya_assert_always(nya_string_contains(elf, "GNU_STACK"),
                      "%s has no GNU_STACK segment to mark non-executable.", rule->output_file);
    nya_assert_always(!nya_string_contains(elf, "RWE"),
                      "%s has a writable-executable (RWE) segment: the stack is not NX. -Wl,-z,noexecstack did not take.", rule->output_file);

    // Stack protector and _FORTIFY_SOURCE leave their runtime helpers as undefined dynamic symbols, which is proof the codegen flags reached the object rather than only the command line: __stack_chk_fail for -fstack-protector-strong, and the __*_chk wrappers for the level-3 _FORTIFY_SOURCE. Several fortify wrappers are checked because which ones appear depends on the source; at least one of these ubiquitous calls is fortified in any real build.
    nya_assert_always(nya_string_contains(elf, "__stack_chk_fail"),
                      "%s references no __stack_chk_fail: -fstack-protector-strong produced no canaries.", rule->output_file);
    b8 has_fortify = nya_string_contains(elf, "__memcpy_chk") || nya_string_contains(elf, "__memset_chk") ||
                     nya_string_contains(elf, "__memmove_chk") || nya_string_contains(elf, "__snprintf_chk") ||
                     nya_string_contains(elf, "__vsnprintf_chk") || nya_string_contains(elf, "__sprintf_chk") ||
                     nya_string_contains(elf, "__printf_chk") || nya_string_contains(elf, "__fprintf_chk");
    nya_assert_always(has_fortify,
                      "%s references no __*_chk fortify wrapper: _FORTIFY_SOURCE produced no checked calls.", rule->output_file);

    nya_log_info("Hardening verified on %s: full RELRO + BIND_NOW, NX stack, and stack-protector + _FORTIFY_SOURCE symbols all present.", rule->output_file);

    nya_command_destroy(&readelf);
}
#endif

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

    // Captured rather than shown: signing narrates its progress and its timestamp server round trip on stdout, which is noise in a build log. Kept so that a failure can still say what went wrong.
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
            // /tr, not /t: an RFC 3161 countersignature is what keeps already shipped binaries verifying after the certificate behind them expires.
            "/tr", timestamp,
            "/td", "SHA256",
            rule->output_file,
        },
    };
#else
    // osslsigncode refuses to write over its input, so it signs to a temporary beside the binary which then replaces it.
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
        // A missing signing tool is the ordinary case on a machine that only builds to run the game, so it cannot fail the build. A release that has to be signed is a CI concern, and CI installs the tool.
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

/* ASSET */

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

void hook_generate_cheatsheet(NYA_BuildRule* rule) {
    nya_unused(rule);

    nya_cheatsheet_generate();
}

void hook_generate_lua_bindings(NYA_BuildRule* rule) {
    nya_unused(rule);

    nya_luabind_generate();
}

void hook_generate_lambdas(NYA_BuildRule* rule) {
    nya_unused(rule);

    nya_lambda_generate();
}

void hook_generate_watches(NYA_BuildRule* rule) {
    nya_unused(rule);

    nya_watch_generate();
}

void hook_index_assets(NYA_BuildRule* rule) {
    nya_assert(rule != nullptr);

    nya_asset_index();
}

void hook_bundle_assets(NYA_BuildRule* rule) {
    nya_assert(rule != nullptr);

    nya_asset_bundle();
}

void hook_stamp_output_file(NYA_BuildRule* rule) {
    nya_assert(rule != nullptr);

    if (rule->output_file == nullptr) return;

    /* The content is the point of the file, not its bytes: what the next invocation compares is the modification time, and a write is the portable way to set one. The recipe's own name goes in so a stamp found by hand says which rule left it. */
    NYA_Error written = nya_file_write(rule->output_file, rule->name);
    if (!written.ok) nya_log_warn("could not stamp '%s' for rule '%s'; it will run again", rule->output_file, rule->name);
}

void hook_assemble_docs(NYA_BuildRule* rule) {
    nya_unused(rule);

    NYA_ConstCString site = "./site";

    /* A fresh tree every time: a page deleted from docs/ must not linger in the deployed site, and a doxygen run left over from a previous build must not shadow this one. */
    if (nya_filesystem_exists(site)) {
        NYA_EXPECT(nya_filesystem_delete_recursive(site), "while clearing the staged docs site");
    }

    /* The hand-written GitBook tree, and with it the cheatsheet that the generate_cheatsheet dependency wrote into docs/ just before this hook ran. doxygen writes its HTML into ./site/doxygen once this hook returns (its OUTPUT_DIRECTORY in docs/doxygen.config), so the three tiers — prose, cheatsheet, generated reference — share one directory and their relative links resolve wherever the tree is deployed. */
    NYA_EXPECT(nya_filesystem_copy_recursive("./docs", site), "while staging the GitBook prose");

    /* The staged tree is its own deploy root, so its .gitbook.yaml points at itself rather than at ./docs the way the committed one at the repository root does. */
    NYA_EXPECT(nya_file_write("./site/.gitbook.yaml",
                              "root: ./\n"
                              "\n"
                              "structure:\n"
                              "  readme: README.md\n"
                              "  summary: SUMMARY.md\n"
                              "\n"
                              "redirects:\n"
                              "  cheatsheet: CHEATSHEET.md\n"),
               "while writing the staged .gitbook.yaml");

    nya_log_info("Staged the docs site under %s; doxygen HTML lands in %s/doxygen next.", site, site);
}
