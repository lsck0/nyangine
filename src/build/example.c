#include "build/build.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API DECLARATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/**
 * Every directory under EXAMPLE_DIRECTORY that holds an EXAMPLE_ENTRY_POINT, sorted by name.
 *
 * A directory without a `main.c` is not an example — it is a shared asset folder, an editor's
 * scratch directory, or a half-started one — and offering it as a choice would only produce a
 * compile error naming a file that was never there.
 * */
NYA_INTERNAL NYA_ArrayᐸNYA_Stringᐳ* _example_discover(NYA_Arena* arena);

NYA_INTERNAL s32 _example_compare(const NYA_String* a, const NYA_String* b);

/** Prints the available examples on stderr. What a missing or misspelled name gets. */
NYA_INTERNAL void _example_list(NYA_Arena* arena);

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PUBLIC API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

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

    /*
     * The name is a directory component, not a path.
     *
     * It is pasted into both a source path and an output filename, so a name containing a separator
     * would compile `examples/../src/main.c` into a binary written wherever the rest of the string
     * pointed. Rejected rather than sanitised: there is no example whose name legitimately contains
     * one, so the only thing sanitising would do is silently accept a typo.
     */
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

    NYA_BuildRule build_example = {
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
                // The same plugins the project compiles. Without them an example that touches curl
                // or sqlite compiles the plugin away to nothing and appears to do nothing.
                FLAGS_PLUGINS,
                LINKER_FLAGS,
                // FLAGS_DEBUG rather than FLAGS_DEVELOPER: an example is read as documentation and
                // run to check something works, so unoptimized and assertion-heavy is the right
                // trade. It does not select the hot reload entry point — that lives in src/main.c,
                // and an example brings its own main.
                FLAGS_DEBUG,
                // Built to run on this machine right now, so the host set: sanitizers included, which
                // is what makes an example that leaks or overruns fail loudly rather than pass.
                FLAGS_HOST_NATIVE,
            },
        },

        .pre_build_hooks = { &hook_add_version_flag_and_git_hash, },
        // Exactly what the project links, by naming the same macro. A hand copied list here is the
        // drift that made every test fail to compile on a missing SDL_image header.
#if OS_WINDOWS
        .vendors         = { NYA_PROJECT_VENDORS_WINDOWS_X86_64, },
#else
        .vendors         = { NYA_PROJECT_VENDORS_LINUX_X86_64, },
#endif
        // An example may draw, so the shaders and the asset index have to exist. The same two the
        // game's own debug DLL depends on, and both are cached, so this is nearly free.
        .dependencies    = { &build_shaders, &index_assets, },
    };

    NYA_BuildRule run_example = {
        .name        = nya_string_to_cstring(arena, run_name),
        .policy      = NYA_BUILD_ALWAYS,
        .output_file = binary_cstr,

        .command = {
            // Not bare `binary_cstr`: a program with no separator is looked up on PATH, and this one
            // is in the working directory.
            .program     = nya_string_to_cstring(arena, nya_string_sprintf(arena, "./%s", binary_cstr)),
            .environment = { SANITIZER_ENVIRONMENT, },
        },

        .dependencies = { &build_example, },
    };

    NYA_EXPECT(nya_build(&run_example), "while running example '%s'", name);
}

NYA_ConstCString example_completion_name(u32 index) {
    /*
     * Listed once and cached, because the completion machinery asks for one name at a time and a
     * fresh directory listing per index would be quadratic. A file scope static rather than a
     * parameter: choices_fn takes only an index, and this runs in a short lived process that exits
     * right after writing the script.
     */
    static NYA_Arena*            arena    = nullptr;
    static NYA_ArrayᐸNYA_Stringᐳ* examples = nullptr;

    if (examples == nullptr) {
        arena    = nya_arena_create(.name = "example_completion");
        examples = _example_discover(arena);
    }

    if (index >= examples->length) return nullptr;

    return nya_string_to_cstring(arena, &examples->items[index]);
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

NYA_ArrayᐸNYA_Stringᐳ* _example_discover(NYA_Arena* arena) {
    NYA_ArrayᐸNYA_Stringᐳ* examples = nya_array_create(arena, NYA_String);

    // Missing rather than empty is a normal state — a checkout without examples/ is not broken — so
    // this reports nothing found rather than failing the command that asked.
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

    // Byte order, so the listing and the completions read the same on every machine rather than in
    // whatever order the filesystem happened to return.
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
