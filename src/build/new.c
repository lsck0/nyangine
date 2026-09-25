/**
 * @file new.c
 *
 * `./build new <name>`: the project scaffolder. It writes the smallest thing that builds — a source
 * directory, an entry point, and a literate header — so a new nyangine program starts from something
 * that already compiles and runs rather than from an empty file and a memory of how the last one was
 * wired.
 *
 * Three shapes, chosen with --kind, because a nyangine program comes in three:
 *
 *   example   a standalone program under examples/, discovered by its directory. `./build run example
 *             <name>` compiles its main.c on its own and runs it. The place to try something out.
 *   headless  the same standalone shape, but its main brings the engine up with no window or GPU and
 *             ticks a system until it quits — the shape a server, a tool or a test grows from.
 *   app       a hot-reloadable binary on the generic host in src/main.c, under src/<name>/, exporting
 *             the app entry contract (core_app_entry.h) exactly as gnyame-cli does. It needs three
 *             edits to wire into the build, which this command prints rather than makes: an app cannot
 *             be discovered the way an example can, so the build has to be told its name.
 *
 * The files are rendered from the templates below by substituting the name into a placeholder token, so
 * the generated code carries the same literate comments the rest of the engine does. A name has to be a
 * C identifier — it becomes one, in the app_id, the system name and the header guard prefix — and a
 * directory that already exists is left untouched rather than written over.
 * */
#include "build/build.h"

/* CONSTANTS */

/** Where each kind is scaffolded. Examples auto-discover from here; an app lives beside the engine. */
#define NEW_EXAMPLE_ROOT "./examples"
#define NEW_APP_ROOT     "./src"

/**
 * The tokens the templates carry where the name goes, one for each case the generated code needs: the
 * lowercase identifier (directory, filename, app_id, function and variable names) and the uppercase one
 * (the header guard and macro prefix). Rendering replaces the uppercase token first; neither is a
 * substring of the other, so the order only matters for reading, not correctness.
 * */
#define NEW_TOKEN       "nyanew"
#define NEW_TOKEN_UPPER "NYANEW"

/* TYPES */

/** Which of the three shapes to scaffold. */
typedef enum NewKind {
    NEW_KIND_EXAMPLE,
    NEW_KIND_HEADLESS,
    NEW_KIND_APP,
} NewKind;

/* TEMPLATES Each is the exact text of a file, with `nyanew`/`NYANEW` standing in for the name. Written as adjacent line literals so the template reads as the file it becomes. */

/** examples/<name>/nyanew.h — the literate header a plain example gets. */
NYA_INTERNAL NYA_ConstCString NEW_TEMPLATE_EXAMPLE_HEADER =
    "/**\n"
    " * @file nyanew.h\n"
    " *\n"
    " * The nyanew example. Scaffolded by `./build new`, and yours to grow.\n"
    " *\n"
    " * An example is a standalone program under examples/ that the build discovers by its directory:\n"
    " * `./build run example nyanew` compiles main.c on its own — the whole engine in one translation\n"
    " * unit, debug flags and sanitizers on — and runs it. Start here.\n"
    " * */\n"
    "#pragma once\n"
    "\n"
    "/** The line the example prints. The smallest thing worth keeping in a header of its own. */\n"
    "#define NYANEW_GREETING \"Hello from the nyanew example.\"\n";

/** examples/<name>/main.c — a plain example, modelled on examples/hello_world. */
NYA_INTERNAL NYA_ConstCString NEW_TEMPLATE_EXAMPLE_MAIN =
    "/**\n"
    " * @file main.c\n"
    " *\n"
    " * The nyanew example's entry point. See nyanew.h.\n"
    " * */\n"
    "#include \"nyangine-core/nyangine.h\"\n"
    "\n"
    "#include \"nyangine-core/nyangine.c\"\n"
    "\n"
    "#include \"nyanew.h\"\n"
    "\n"
    "s32 main(s32 argc, NYA_CString* argv) {\n"
    "    nya_unused(argc, argv);\n"
    "    nya_backtrace_init();\n"
    "\n"
    "    nya_log_info(\"%s\", NYANEW_GREETING);\n"
    "\n"
    "    nya_backtrace_deinit();\n"
    "    return EXIT_SUCCESS;\n"
    "}\n";

/** examples/<name>/nyanew.h — the header a headless example gets: it declares the one system. */
NYA_INTERNAL NYA_ConstCString NEW_TEMPLATE_HEADLESS_HEADER =
    "/**\n"
    " * @file nyanew.h\n"
    " *\n"
    " * The nyanew headless example: the engine brought up with no window or GPU, ticking a single\n"
    " * system until it asks to quit. Scaffolded by `./build new --kind headless`.\n"
    " *\n"
    " * `./build run example nyanew` compiles main.c on its own and runs it. Headless means it needs no\n"
    " * display, so it is the shape a server, a tool or a test grows from.\n"
    " * */\n"
    "#pragma once\n"
    "\n"
    "#include \"nyangine-std/base/base_basic.h\"\n"
    "#include \"nyangine-std/base/base_types.h\"\n"
    "\n"
    "/** Frames to tick before the heartbeat stops the run. About 20 seconds at the 10 fps cap main sets. */\n"
    "#define NYANEW_MAX_FRAMES 200\n"
    "\n"
    "/**\n"
    " * The one system this program registers: a heartbeat that also decides when the run is done.\n"
    " *\n"
    " * Declared here rather than left static, so it reads as the program's public part the same way\n"
    " * gnyame's callbacks are declared in a header of their own. See main.c.\n"
    " * */\n"
    "void nyanew_heartbeat(f32 delta_time_s);\n";

/** examples/<name>/main.c — a headless example: the engine up, one system ticking, then it exits. */
NYA_INTERNAL NYA_ConstCString NEW_TEMPLATE_HEADLESS_MAIN =
    "/**\n"
    " * @file main.c\n"
    " *\n"
    " * The nyanew headless example's entry point. Brings the engine up headless, ticks the heartbeat\n"
    " * system, and exits on its own. See nyanew.h.\n"
    " * */\n"
    "#include \"nyangine-core/nyangine.h\"\n"
    "\n"
    "#include \"nyangine-core/nyangine.c\"\n"
    "\n"
    "#include \"nyanew.h\"\n"
    "\n"
    "/** How many frames the heartbeat has seen. Standalone, so a plain file-scope counter. */\n"
    "NYA_INTERNAL u32 nyanew_frame_count = 0;\n"
    "\n"
    "void nyanew_heartbeat(f32 delta_time_s) {\n"
    "    nya_unused(delta_time_s);\n"
    "\n"
    "    nyanew_frame_count++;\n"
    "    if (nyanew_frame_count % 10 == 0) nya_log_info(\"nyanew: heartbeat %u/%u.\", nyanew_frame_count, (u32)NYANEW_MAX_FRAMES);\n"
    "    if (nyanew_frame_count >= NYANEW_MAX_FRAMES) nya_app_get()->should_quit = true;\n"
    "}\n"
    "\n"
    "/** Registers the parts before the frame loop starts, the way an app hands the engine its systems. */\n"
    "NYA_INTERNAL void nyanew_parts(void) {\n"
    "    nya_system_register((NYA_SystemEntry){ .name = \"nyanew_heartbeat\", .frame = nya_callback(nyanew_heartbeat) });\n"
    "}\n"
    "\n"
    "s32 main(s32 argc, NYA_CString* argv) {\n"
    "    nya_unused(argc, argv);\n"
    "    nya_backtrace_init();\n"
    "\n"
    "    // Headless: no window and no GPU, so this runs anywhere. The unfocused cap is the frame limiter\n"
    "    // for a run that never has a focused window, keeping the heartbeat at a readable rate.\n"
    "    NYA_EXPECT(\n"
    "        nya_app_init(\n"
    "            .headless                   = true,\n"
    "            .app_id                     = \"nyanew\",\n"
    "            .unfocused_frame_rate_limit = 10,\n"
    "            .parts                      = nyanew_parts\n"
    "        ),\n"
    "        \"while starting nyanew\"\n"
    "    );\n"
    "\n"
    "    nya_app_run();\n"
    "    nya_app_deinit();\n"
    "\n"
    "    nya_backtrace_deinit();\n"
    "    return EXIT_SUCCESS;\n"
    "}\n";

/** src/<name>/nyanew.h — the header an app gets: it declares the system its DLL registers. */
NYA_INTERNAL NYA_ConstCString NEW_TEMPLATE_APP_HEADER =
    "/**\n"
    " * @file nyanew.h\n"
    " *\n"
    " * The nyanew app: a hot-reloadable binary on the nyangine host, scaffolded by\n"
    " * `./build new --kind app` from the gnyame-cli pattern. It exports the app entry contract\n"
    " * (core_app_entry.h) and nothing else, so the generic host in src/main.c loads it exactly as it\n"
    " * loads gnyame — a binary named `nyanew.debug` finds `nyanew.debug.so` beside it and reloads it.\n"
    " *\n"
    " * Wiring it into the build is three edits, each modelled on gnyame-cli; `./build new` printed them.\n"
    " * */\n"
    "#pragma once\n"
    "\n"
    "#include \"nyangine-std/base/base_basic.h\"\n"
    "#include \"nyangine-std/base/base_types.h\"\n"
    "\n"
    "/** Frames to tick before the heartbeat stops the run. About 20 seconds at the 10 fps cap init sets. */\n"
    "#define NYANEW_MAX_FRAMES 200\n"
    "\n"
    "/**\n"
    " * The one system this app registers: a heartbeat that also decides when the run is done.\n"
    " *\n"
    " * Declared here, not left static: a system holds its callback by name and the host re-resolves it\n"
    " * with dlsym against the reloaded image (core_system.h), so a static callback would vanish on the\n"
    " * first code reload. gnyame declares its callbacks in a header for the same reason.\n"
    " * */\n"
    "void nyanew_heartbeat(f32 delta_time_s);\n";

/** src/<name>/nyanew.c — an app, modelled on src/gnyame_cli/gnyame_cli.c. */
NYA_INTERNAL NYA_ConstCString NEW_TEMPLATE_APP_SOURCE =
    "/**\n"
    " * @file nyanew.c\n"
    " *\n"
    " * A hot-reloadable app on the nyangine host, kept deliberately tiny: it brings the engine up\n"
    " * headless, ticks a heartbeat system for a while, and exits. It exports the app entry contract\n"
    " * (core_app_entry.h) and nothing else, so the host in src/main.c loads and reloads it exactly as\n"
    " * it does gnyame. Build and run it with `./build run nyanew` once it is wired in; see nyanew.h.\n"
    " * */\n"
    "#include \"nyangine-core/nyangine.h\"\n"
    "\n"
    "#include \"nyanew.h\"\n"
    "\n"
    "// A DLL global, so a code reload zeroes it and the heartbeat count simply restarts — the honest\n"
    "// demonstration that reloaded state lives in the engine world, not here.\n"
    "NYA_INTERNAL u32 nyanew_frame_count = 0;\n"
    "\n"
    "void nyanew_heartbeat(f32 delta_time_s) {\n"
    "    nya_unused(delta_time_s);\n"
    "\n"
    "    nyanew_frame_count++;\n"
    "    if (nyanew_frame_count % 10 == 0) nya_log_info(\"nyanew: heartbeat %u/%u.\", nyanew_frame_count, (u32)NYANEW_MAX_FRAMES);\n"
    "    if (nyanew_frame_count >= NYANEW_MAX_FRAMES) nya_app_get()->should_quit = true;\n"
    "}\n"
    "\n"
    "/** Registered by name so the handle survives a code reload, exactly as gnyame's parts are. */\n"
    "NYA_INTERNAL void nyanew_parts(void) {\n"
    "    nya_system_register((NYA_SystemEntry){ .name = \"nyanew_heartbeat\", .frame = nya_callback(nyanew_heartbeat) });\n"
    "}\n"
    "\n"
    "b8 nya_app_entry_init(s32 argc, NYA_CString* argv) {\n"
    "    nya_unused(argc, argv);\n"
    "\n"
    "    nya_log_info(\"nyanew up: a hot-reloadable app sharing the nyangine host.\");\n"
    "\n"
    "    // Headless: no window and no GPU, so this runs anywhere. The unfocused cap is the frame limiter\n"
    "    // for a run that never has a focused window, keeping the heartbeat at a readable rate.\n"
    "    NYA_EXPECT(\n"
    "        nya_app_init(\n"
    "            .headless                   = true,\n"
    "            .app_id                     = \"nyanew\",\n"
    "            .unfocused_frame_rate_limit = 10,\n"
    "            .parts                      = nyanew_parts\n"
    "        ),\n"
    "        \"while starting nyanew\"\n"
    "    );\n"
    "\n"
    "    return true;\n"
    "}\n"
    "\n"
    "void nya_app_entry_run(void) { nya_app_run(); }\n"
    "\n"
    "void nya_app_entry_deinit(void) { nya_app_deinit(); }\n";

/* PRIVATE API DECLARATION */

/** Whether `name` is a C identifier: a letter or underscore, then letters, digits or underscores. */
NYA_INTERNAL b8 _new_name_is_identifier(NYA_ConstCString name);

/** Maps a --kind string to a NewKind, returning whether it was one of the three. */
NYA_INTERNAL b8 _new_kind_parse(NYA_ConstCString text, OUT NewKind* out_kind);

/** `name` with every ASCII lowercase letter uppercased, for the header guard and macro prefix. */
NYA_INTERNAL NYA_String* _new_uppercase(NYA_Arena* arena, NYA_ConstCString name);

/** A template with the name tokens substituted, ready to write to disk. */
NYA_INTERNAL NYA_String* _new_render(NYA_Arena* arena, NYA_ConstCString template, NYA_ConstCString name, NYA_ConstCString name_upper);

/** Creates `directory` and writes `contents` to `directory/filename`, panicking on an IO failure. */
NYA_INTERNAL void _new_write_file(NYA_Arena* arena, NYA_ConstCString directory, NYA_ConstCString filename, const NYA_String* contents);

/* PUBLIC API IMPLEMENTATION */

void new_runner(NYA_ArgCommand* command) {
    nya_assert(command != nullptr);

    NYA_ArgParameter* name_param = command->parameters[0];
    NYA_ArgParameter* kind_param = command->parameters[1];
    nya_assert(name_param != nullptr);
    nya_assert(kind_param != nullptr);

    NYA_Arena* arena = nya_arena_create(.name = "new_runner");
    defer nya_arena_destroy(arena);

    // The name. Its own error rather than a panic, because a bad name is user input, not a broken build.
    if (!name_param->was_matched) {
        (void)fprintf(stderr, "Error: no name given. Try: ./build new my_app --kind example\n");
        exit(EXIT_FAILURE);
    }
    NYA_CString name = name_param->value.as_string;

    if (!_new_name_is_identifier(name)) {
        (void)fprintf(stderr, "Error: '%s' is not a valid name.\n", name);
        (void)fprintf(stderr, "It becomes a C identifier, so use letters, digits and underscores, starting with a letter or underscore.\n");
        exit(EXIT_FAILURE);
    }

    NewKind kind;
    if (!_new_kind_parse(kind_param->value.as_string, &kind)) {
        (void)fprintf(stderr, "Error: unknown kind '%s'. One of: app, example, headless.\n", kind_param->value.as_string);
        exit(EXIT_FAILURE);
    }

    NYA_String* name_upper = _new_uppercase(arena, name);
    NYA_CString upper      = nya_string_to_cstring(arena, name_upper);

    // The directory the kind lives in, and the two files it gets. An app's source is named after it (the build compiles that translation unit); an example's is always main.c, which is what discovery looks for.
    NYA_ConstCString root           = (kind == NEW_KIND_APP) ? NEW_APP_ROOT : NEW_EXAMPLE_ROOT;
    NYA_String*      directory      = nya_string_sprintf(arena, "%s/%s", root, name);
    NYA_CString      directory_cstr = nya_string_to_cstring(arena, directory);

    // A directory that already exists is left as it is: scaffolding over an existing project would overwrite files it did not write.
    if (nya_filesystem_exists(directory_cstr)) {
        (void)fprintf(stderr, "Error: %s already exists; not overwriting it. Pick another name or remove it first.\n", directory_cstr);
        exit(EXIT_FAILURE);
    }

    NYA_ConstCString header_template = nullptr;
    NYA_ConstCString source_template = nullptr;
    NYA_ConstCString source_filename = nullptr;
    switch (kind) {
        case NEW_KIND_EXAMPLE:
            header_template = NEW_TEMPLATE_EXAMPLE_HEADER;
            source_template = NEW_TEMPLATE_EXAMPLE_MAIN;
            source_filename = EXAMPLE_ENTRY_POINT; // main.c
            break;
        case NEW_KIND_HEADLESS:
            header_template = NEW_TEMPLATE_HEADLESS_HEADER;
            source_template = NEW_TEMPLATE_HEADLESS_MAIN;
            source_filename = EXAMPLE_ENTRY_POINT; // main.c
            break;
        case NEW_KIND_APP:
            header_template = NEW_TEMPLATE_APP_HEADER;
            source_template = NEW_TEMPLATE_APP_SOURCE;
            source_filename = nya_string_to_cstring(arena, nya_string_sprintf(arena, "%s.c", name));
            break;
        default: nya_unreachable(); // _new_kind_parse only ever yields one of the three above.
    }

    NYA_CString header_filename = nya_string_to_cstring(arena, nya_string_sprintf(arena, "%s.h", name));

    _new_write_file(arena, directory_cstr, source_filename, _new_render(arena, source_template, name, upper));
    _new_write_file(arena, directory_cstr, header_filename, _new_render(arena, header_template, name, upper));

    nya_log_info("Scaffolded %s.", directory_cstr);

    // Next steps: for an example the build discovers it, so one line; for an app the build has to be told its name, so the three edits that do it, each pointing at where gnyame-cli already does the same.
    if (kind == NEW_KIND_APP) {
        nya_log_info("An app is not auto-discovered. Wire it in like gnyame-cli, three edits:");
        nya_log_info("  1. src/build/flags.h: an APP_%s_NAME / APP_%s_DLL_SOURCE pair and the debug binary/dll names,", upper, upper);
        nya_log_info("     beside APP_GNYAME_CLI_NAME (see the APPS section).");
        nya_log_info("  2. src/build/on_linux/build_linux.h: the host, compile-dll, build-dll and metarule rules,");
        nya_log_info("     modelled on build_gnyame_cli_debug_* .");
        nya_log_info("  3. src/build/cli.c: a `run %s` and a `build %s` subcommand naming those rules.", name, name);
        nya_log_info("Then build and run it with: ./build run %s", name);
    } else {
        nya_log_info("Build and run it with: ./build run example %s", name);
    }
}

NYA_ConstCString new_completion_kind(u32 index) {
    static const NYA_ConstCString kinds[] = { "app", "example", "headless" };
    if (index >= nya_carray_length(kinds)) return nullptr;
    return kinds[index];
}

/* PRIVATE API IMPLEMENTATION */

b8 _new_name_is_identifier(NYA_ConstCString name) {
    nya_assert(name != nullptr);

    u8 first = (u8)name[0];
    b8 head  = (first >= 'a' && first <= 'z') || (first >= 'A' && first <= 'Z') || first == '_';
    if (!head) return false; // also catches the empty string, whose first byte is '\0'.

    for (u32 i = 1; name[i] != '\0'; i++) {
        u8 c    = (u8)name[i];
        b8 body = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
        if (!body) return false; // a path separator, a dot, a space: none of them belong in an identifier.
    }
    return true;
}

b8 _new_kind_parse(NYA_ConstCString text, NewKind* out_kind) {
    nya_assert(text != nullptr);
    nya_assert(out_kind != nullptr);

    if (nya_string_equals(text, "example")) { *out_kind = NEW_KIND_EXAMPLE; return true; }
    if (nya_string_equals(text, "headless")) { *out_kind = NEW_KIND_HEADLESS; return true; }
    if (nya_string_equals(text, "app")) { *out_kind = NEW_KIND_APP; return true; }
    return false;
}

NYA_String* _new_uppercase(NYA_Arena* arena, NYA_ConstCString name) {
    nya_assert(name != nullptr);

    NYA_String* out = nya_string_create(arena);
    for (u32 i = 0; name[i] != '\0'; i++) {
        u8 c = (u8)name[i];
        if (c >= 'a' && c <= 'z') c = (u8)(c - 'a' + 'A');
        nya_string_push_back(out, c);
    }
    return out;
}

NYA_String* _new_render(NYA_Arena* arena, NYA_ConstCString template, NYA_ConstCString name, NYA_ConstCString name_upper) {
    nya_assert(template != nullptr);
    nya_assert(name != nullptr);
    nya_assert(name_upper != nullptr);

    NYA_String* out = nya_string_create(arena);
    nya_string_extend(out, template);
    // Uppercase first: the two tokens are disjoint, so this is only for reading, not correctness.
    nya_string_replace(out, NEW_TOKEN_UPPER, name_upper);
    nya_string_replace(out, NEW_TOKEN, name);
    return out;
}

void _new_write_file(NYA_Arena* arena, NYA_ConstCString directory, NYA_ConstCString filename, const NYA_String* contents) {
    nya_assert(directory != nullptr);
    nya_assert(filename != nullptr);
    nya_assert(contents != nullptr);

    NYA_EXPECT(nya_filesystem_create_directory(directory), "while creating %s", directory);

    NYA_String* path = nya_string_sprintf(arena, "%s/%s", directory, filename);
    NYA_EXPECT(nya_file_write(nya_string_to_cstring(arena, path), contents), "while writing %s", nya_string_to_cstring(arena, path));
}
