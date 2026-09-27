#include "nyangine-build/build.h"

/* The headless-server switch, defined in cli.c, which is compiled after this file in the unity build. When it is set an example is built to ship rather than to run — see the two rules example_runner chooses between. */
extern NYA_ArgParameter server_flag;

/* PRIVATE API DECLARATION */

/**
 * Every directory under EXAMPLE_DIRECTORY that holds an EXAMPLE_ENTRY_POINT, sorted by name.
 * */
NYA_INTERNAL NYA_ArrayᐸNYA_Stringᐳ* _example_discover(NYA_Arena* arena);

NYA_INTERNAL s32 _example_compare(const NYA_String* a, const NYA_String* b);

/** Prints the available examples on stderr. What a missing or misspelled name gets. */
NYA_INTERNAL void _example_list(NYA_Arena* arena);

/** The debug build of one example to `<name>.example` at the root, sanitized, the way `run example` runs it. */
NYA_INTERNAL NYA_BuildRule* _example_build_rule(NYA_Arena* arena, NYA_ConstCString name);

#if !OS_WINDOWS
/** Why a bounded run failed, as the first telling line of its log, or nullptr when it passed. */
NYA_INTERNAL NYA_ConstCString _example_run_failure(NYA_Arena* arena, NYA_ConstCString log_path, s32 exit_code);

/** What `run examples` hands each one: the engine's run budget, and a backstop for a program that ignores it. The exits are timeout(1)'s. */
enum {
    _EXAMPLES_BUDGET_FRAMES   = 300,
    _EXAMPLES_BUDGET_SECONDS  = 10,
    _EXAMPLES_TIMEOUT_SECONDS = 120,
    _EXAMPLES_TIMEOUT_EXIT    = 124,
    _EXAMPLES_KILLED_EXIT     = 128 + 9,
};
#endif

/* PUBLIC API IMPLEMENTATION */

void example_runner(NYA_ArgCommand* command) {
    nya_assert(command != nullptr);

    NYA_ArgParameter* example_name = command->parameters[0];
    nya_assert(example_name != nullptr);
    nya_assert(nya_string_equals(example_name->name, "example"));

    NYA_Arena* arena = nya_arena_create(.name = "example_runner");
    defer nya_arena_destroy(arena);

    if (!example_name->was_matched) {
        (void)fprintf(stderr, "Error: no example named.\n");
        _example_list(arena);
        exit(EXIT_FAILURE);
    }

    NYA_CString name = example_name->value.as_string;

    /* The name is a directory component, not a path. */
    if (name[0] == '\0' || nya_string_contains(name, "/") || nya_string_contains(name, "\\") || nya_string_equals(name, "..")) {
        (void)fprintf(stderr, "Error: '%s' is not a valid example name; it must be a single directory name.\n", name);
        _example_list(arena);
        exit(EXIT_FAILURE);
    }

    NYA_String* source = nya_string_sprintf(arena, "%s/%s/%s", EXAMPLE_DIRECTORY, name, EXAMPLE_ENTRY_POINT);
    NYA_CString source_cstr = nya_string_to_cstring(arena, source);

    if (!nya_filesystem_exists(source_cstr)) {
        (void)fprintf(stderr, "Error: no example '%s'; %s does not exist.\n", name, source_cstr);
        _example_list(arena);
        exit(EXIT_FAILURE);
    }

    // At the repo root, so $ORIGIN finds the vendored shared objects. See example.h.
    NYA_String* binary      = nya_string_sprintf(arena, "%s" EXAMPLE_BINARY_SUFFIX, name);
    NYA_CString binary_cstr = nya_string_to_cstring(arena, binary);

    NYA_String* build_name = nya_string_sprintf(arena, "build_example:%s", name);
    NYA_String* run_name   = nya_string_sprintf(arena, "run_example:%s", name);

    // No server target on Windows: the deploy target is a Linux container, and the shipping flags and the vendor subset below are the Linux ones. On a Windows host --server falls back to a normal run.
#if OS_WINDOWS
    b8 server = false;
#else
    b8 server = server_flag.value.as_b8;
#endif

    // A per-example opt-in: a `.headless` file beside its main.c says this example is a no-core server and wants the headless link — NYA_NO_SDL + NYA_SERVER and the server vendor subset, not the full engine graph dead-stripped. The marker rather than a second flag keeps the knowledge with the example that knows it can build that way; every other server example still takes the full-graph path below. Only meaningful under --server, and so never checked on a Windows host, where there is no server target.
    NYA_String* headless_marker = nya_string_sprintf(arena, "%s/%s/.headless", EXAMPLE_DIRECTORY, name);
    b8          headless        = server && nya_filesystem_exists(nya_string_to_cstring(arena, headless_marker));

    NYA_BuildRule* build_example = _example_build_rule(arena, name);

#if !OS_WINDOWS
    /* The --server shipping build of the example, the artifact a container image copies: release rather than debug (optimized, LTO'd, no sanitizers, no hot-reload entry point), the asset blob baked in (NYA_ASSET_PREFER_BLOB, which is why the dependency is bundle_assets rather than the index alone), dead-code collected, Linux-hardened, and -s stripped. It still links the full project vendors, not the server subset — the example compiles the whole engine graph until the NYA_NO_SDL wall lands; see the deploy README. */
    NYA_BuildRule build_server_example = {
        .name        = nya_string_to_cstring(arena, build_name),
        .policy      = NYA_BUILD_ALWAYS,
        .output_file = binary_cstr,

        .command = {
            .program   = CC,
            .arguments = {
                source_cstr,
                "-o", binary_cstr,
                CFLAGS,
                WARNINGS,
                INCLUDE_PATHS,
                FLAGS_PLUGINS,
                LINKER_FLAGS,
                FLAGS_RELEASE,
                FLAGS_RELEASE_LINK,
                FLAGS_RELEASE_LINK_LINUX_X86_64,
                FLAGS_LINUX_X86_64,
                // Strip: the debug info FLAGS_RELEASE keeps for a symbolized crash trace is the largest thing in the binary, and a container ships bytes, not a debugger. Safe here because this example carries no integrity hash to invalidate; see hook_insert_integrity_hash.
                "-s",
            },
        },

        .pre_build_hooks = { &hook_add_version_flag, },
        .vendors         = { NYA_PROJECT_VENDORS_LINUX_X86_64, },
        // bundle_assets, not the index alone: a release reads the web bundle out of the baked blob, so the blob has to have been written. It pulls in the shaders and the index on its own.
        .dependencies    = { &bundle_assets, },
    };

    /* The headless build of a `.headless` example: the same shipping shape as build_server_example but compiled FLAGS_SERVER_HEADLESS (NYA_NO_SDL + NYA_SERVER, so no core or renderer in the graph) and linked against the server vendor subset — no SDL, box2d, box3d, ufbx or shadercross, so ldd on the result names no libSDL3. No FLAGS_PLUGINS (the list would pull the core-bound Lua plugin) and no asset dependency (a headless example carries its own bytes; see examples/headless_server/main.c). */
    NYA_BuildRule build_headless_server_example = {
        .name        = nya_string_to_cstring(arena, build_name),
        .policy      = NYA_BUILD_ALWAYS,
        .output_file = binary_cstr,

        .command = {
            .program   = CC,
            .arguments = {
                source_cstr,
                "-o", binary_cstr,
                CFLAGS,
                WARNINGS,
                INCLUDE_PATHS,
                LINKER_FLAGS,
                FLAGS_SERVER_HEADLESS,
                FLAGS_RELEASE,
                FLAGS_RELEASE_LINK,
                FLAGS_RELEASE_LINK_LINUX_X86_64,
                FLAGS_LINUX_X86_64,
                // Strip, for the same reason and with the same safety as build_server_example: this example carries no integrity hash for the strip to invalidate.
                "-s",
            },
        },

        .pre_build_hooks = { &hook_add_version_flag, },
        .vendors         = { NYA_SERVER_VENDORS_LINUX_X86_64, },
        // No dependencies: nothing here reads an asset, so there is no bundle or shader to build.
    };

    if (server) {
        if (headless) {
            // Built, not run, like the full-graph server below: a server image is made from the binary.
            NYA_EXPECT(nya_build(&build_headless_server_example), "while building headless example '%s' to ship", name);

            nya_log_info("Built %s headless: release, stripped, no SDL and no core linked. Run it with --port 8000; `ldd` names no libSDL3.", binary_cstr);

            return;
        }

        // Built, not run: a server image is made from the binary, and running it here would block the build on a process that only stops on a signal.
        NYA_EXPECT(nya_build(&build_server_example), "while building example '%s' to ship", name);

        nya_log_info("Built %s to ship: release, stripped, its assets baked in. Run it with --address 0.0.0.0 --port 8000.", binary_cstr);

        return;
    }
#endif

    NYA_BuildRule run_example = {
        .name        = nya_string_to_cstring(arena, run_name),
        .policy      = NYA_BUILD_ALWAYS,
        .output_file = binary_cstr,

        .command = {
            // Not bare `binary_cstr`: a program with no separator is looked up on PATH, and this one is in the working directory.
            .program     = nya_string_to_cstring(arena, nya_string_sprintf(arena, "./%s", binary_cstr)),
            .environment = { SANITIZER_ENVIRONMENT, },
        },

        .dependencies = { build_example, },
    };

    NYA_EXPECT(nya_build(&run_example), "while running example '%s'", name);
}

#if !OS_WINDOWS
void examples_runner(NYA_ArgCommand* command) {
    nya_unused(command);

    NYA_Arena* arena = nya_arena_create(.name = "examples_runner");
    defer nya_arena_destroy(arena);

    NYA_ArrayᐸNYA_Stringᐳ* examples = _example_discover(arena);
    u32                   count    = (u32)examples->length;
    NYA_BuildRule**       rules    = nya_arena_alloc(arena, count * sizeof(NYA_BuildRule*));

    for (u32 i = 0; i < count; i++) {
        rules[i] = _example_build_rule(arena, nya_string_to_cstring(arena, &examples->items[i]));
        // a binary left by an earlier run would pass for this one's build below.
        (void)nya_filesystem_delete(rules[i]->output_file);
    }

    // All at once, then one at a time for whatever the pool never started after a failure, so a broken example costs its own row and not the rest.
    (void)nya_build_parallel(rules, count, 0);
    for (u32 i = 0; i < count; i++) {
        if (!nya_filesystem_exists(rules[i]->output_file)) (void)nya_build(rules[i]);
    }

    NYA_ConstCString* failures = nya_arena_alloc(arena, count * sizeof(NYA_ConstCString));
    u32               failed   = 0;

    for (u32 i = 0; i < count; i++) {
        NYA_ConstCString binary = rules[i]->output_file;
        NYA_ConstCString log    = nya_string_to_cstring(arena, nya_string_sprintf(arena, "%s.log", binary));

        if (!nya_filesystem_exists(binary)) {
            failures[i] = "did not compile";
        } else {
            nya_log_info("Running %s, output in %s", binary, log);

            // Under script(1), so a terminal program gets the pseudo terminal it would get from a person. timeout, not a wait of ours, kills the group; the example's own session then loses its terminal and hangs up, so a child it re-executed cannot outlive it. Output goes to a file, so nothing left behind can hold a pipe of ours open.
            NYA_ConstCString shell = "exec timeout --kill-after=5 %u script --quiet --return --command ./%s /dev/null < /dev/null > %s 2>&1";
            NYA_Command      run   = {
                .arena       = arena,
                .program     = "sh",
                .arguments   = { "-c", nya_string_to_cstring(arena, nya_string_sprintf(arena, shell, _EXAMPLES_TIMEOUT_SECONDS, binary, log)) },
                .environment = {
                    SANITIZER_ENVIRONMENT,
                    nya_string_to_cstring(arena, nya_string_sprintf(arena, "NYA_EXIT_AFTER_FRAMES=%u", _EXAMPLES_BUDGET_FRAMES)),
                    nya_string_to_cstring(arena, nya_string_sprintf(arena, "NYA_EXIT_AFTER_SECONDS=%u", _EXAMPLES_BUDGET_SECONDS)),
                },
            };
            NYA_EXPECT(nya_command_run(&run), "while running %s", binary);

            failures[i] = _example_run_failure(arena, log, run.exit_code);
        }

        if (failures[i] != nullptr) failed++;
    }

    printf("\n%-20s %-6s %s\n", "example", "result", "first error");
    for (u32 i = 0; i < count; i++) {
        printf("%-20s %-6s %s\n", nya_string_to_cstring(arena, &examples->items[i]), failures[i] == nullptr ? "PASS" : "FAIL", failures[i] == nullptr ? "" : failures[i]);
    }
    printf("\n%u of %u examples passed.\n", count - failed, count);

    if (failed > 0) exit(EXIT_FAILURE);
}
#endif

NYA_ConstCString example_completion_name(u32 index) {
    /* Listed once and cached, since completion asks for one name at a time and relisting per index would be quadratic. A static because choices_fn only takes an index, in a short lived process. */
    static NYA_Arena*            arena    = nullptr;
    static NYA_ArrayᐸNYA_Stringᐳ* examples = nullptr;

    if (examples == nullptr) {
        arena    = nya_arena_create(.name = "example_completion");
        examples = _example_discover(arena);
    }

    if (index >= examples->length) return nullptr;

    return nya_string_to_cstring(arena, &examples->items[index]);
}

/* PRIVATE API IMPLEMENTATION */

NYA_ArrayᐸNYA_Stringᐳ* _example_discover(NYA_Arena* arena) {
    NYA_ArrayᐸNYA_Stringᐳ* examples = nya_array_create(arena, NYA_String);

    // a checkout without examples/ is normal, so report nothing found instead of failing.
    if (!nya_filesystem_exists(EXAMPLE_DIRECTORY)) return examples;

    NYA_ArrayᐸNYA_DirectoryEntryᐳ* entries = nullptr;
    NYA_Error listed = nya_filesystem_list(arena, EXAMPLE_DIRECTORY, &entries);
    if (!listed.ok) return examples;

    nya_array_foreach (entries, entry) {
        if (entry->type != NYA_FILE_TYPE_DIRECTORY) continue;

        NYA_CString name = nya_string_to_cstring(arena, entry->name);

        NYA_String* entry_point = nya_string_sprintf(arena, "%s/%s/%s", EXAMPLE_DIRECTORY, name, EXAMPLE_ENTRY_POINT);
        if (!nya_filesystem_exists(nya_string_to_cstring(arena, entry_point))) continue;

        nya_array_push_back(examples, *nya_string_clone(arena, entry->name));
    }

    // Byte order, so the listing and the completions read the same on every machine rather than in whatever order the filesystem happened to return.
    nya_array_sort(examples, _example_compare);

    return examples;
}

s32 _example_compare(const NYA_String* a, const NYA_String* b) {
    u64 shared     = nya_min(a->length, b->length);
    s32 difference = nya_memcmp(a->items, b->items, shared);
    if (difference != 0) return difference < 0 ? -1 : 1;

    if (a->length == b->length) return 0;
    return a->length < b->length ? -1 : 1;
}

void _example_list(NYA_Arena* arena) {
    NYA_ArrayᐸNYA_Stringᐳ* examples = _example_discover(arena);

    if (examples->length == 0) {
        (void)fprintf(stderr, "No examples found under %s.\n", EXAMPLE_DIRECTORY);
        return;
    }

    (void)fprintf(stderr, "Available examples:\n");
    nya_array_foreach (examples, example) (void)fprintf(stderr, "  %s\n", nya_string_to_cstring(arena, example));
}

NYA_BuildRule* _example_build_rule(NYA_Arena* arena, NYA_ConstCString name) {
    NYA_CString source_cstr = nya_string_to_cstring(arena, nya_string_sprintf(arena, "%s/%s/%s", EXAMPLE_DIRECTORY, name, EXAMPLE_ENTRY_POINT));
    // At the repo root, so $ORIGIN finds the vendored shared objects. See example.h.
    NYA_CString binary_cstr = nya_string_to_cstring(arena, nya_string_sprintf(arena, "%s" EXAMPLE_BINARY_SUFFIX, name));

    NYA_BuildRule* rule = nya_arena_alloc(arena, sizeof(NYA_BuildRule));
    *rule = (NYA_BuildRule){
        .name        = nya_string_to_cstring(arena, nya_string_sprintf(arena, "build_example:%s", name)),
        .policy      = NYA_BUILD_ALWAYS,
        .output_file = binary_cstr,

        .command = {
            .program   = CC,
            .arguments = {
                source_cstr,
                "-o", binary_cstr,
                CFLAGS,
                WARNINGS,
                INCLUDE_PATHS,
                // The same plugins the project compiles. Without them an example that touches curl or sqlite compiles the plugin away to nothing and appears to do nothing.
                FLAGS_PLUGINS,
                LINKER_FLAGS,
                // FLAGS_DEBUG rather than FLAGS_DEVELOPER: examples are documentation and checks, so unoptimized with assertions. The hot reload entry point is in src/main.c, and examples bring their own main.
                FLAGS_DEBUG,
                // Built to run on this machine right now, so the host set: sanitizers included, which is what makes an example that leaks or overruns fail loudly rather than pass.
                FLAGS_HOST_NATIVE,
            },
        },

        .pre_build_hooks = { &hook_add_version_flag, },
        // Exactly what the project links, by naming the same macro. A hand copied list here is the drift that made every test fail to compile on a missing SDL_image header.
#if OS_WINDOWS
        .vendors         = { NYA_PROJECT_VENDORS_WINDOWS_X86_64, },
#else
        .vendors         = { NYA_PROJECT_VENDORS_LINUX_X86_64, },
#endif
        // An example may draw, so the shaders and the asset index have to exist. The same two the game's own debug DLL depends on, and both are cached, so this is nearly free.
        .dependencies    = { &build_shaders, &index_assets, },
    };

    return rule;
}

#if !OS_WINDOWS
NYA_ConstCString _example_run_failure(NYA_Arena* arena, NYA_ConstCString log_path, s32 exit_code) {
    // [PREVENTED ASSERTION FAILED] is a test catching its own assert, and does not match the first.
    static NYA_ConstCString MARKERS[] = { "[ASSERTION FAILED]", "[PANIC]", "[ERROR THROWN]", "AddressSanitizer", "LeakSanitizer", "runtime error:" };

    NYA_String* output = nya_string_create(arena);
    if (!nya_file_read(log_path, output).ok) return "wrote no log";

    NYA_ConstCString        last  = "";
    NYA_ArrayᐸNYA_Stringᐳ* lines = nya_string_split_lines(arena, output);

    nya_array_foreach (lines, line) {
        // the pseudo terminal ends every line in \r\n.
        nya_string_trim_whitespace(line);
        if (line->length > 0) last = nya_string_to_cstring(arena, line);

        for (u32 m = 0; m < sizeof(MARKERS) / sizeof(MARKERS[0]); m++) {
            if (nya_string_contains(line, MARKERS[m])) return last;
        }
    }

    if (exit_code == _EXAMPLES_TIMEOUT_EXIT || exit_code == _EXAMPLES_KILLED_EXIT) {
        return nya_string_to_cstring(arena, nya_string_sprintf(arena, "timed out after %us: %s", _EXAMPLES_TIMEOUT_SECONDS, last));
    }
    if (exit_code != 0) return nya_string_to_cstring(arena, nya_string_sprintf(arena, "exit code %d: %s", exit_code, last));

    return nullptr;
}
#endif
