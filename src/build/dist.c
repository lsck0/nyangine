/**
 * @file dist.c
 *
 * Turning what was built into something you can hand to someone: `./build version`,
 * `./build changelog` and `./build dist`.
 *
 * One distribution is one directory under dist/, named after its target, and every one of them has the
 * same shape:
 *
 *   dist/linux/
 *     gnyame                  the entry point
 *     gnyame.desktop          packager specific files: desktop entry, icon, installer, steam library
 *     gnyame.png
 *     install.sh
 *     LICENSE
 *     CHANGELOG.md
 *     data/                   settings.nya and theme.nya, the player's to edit; saves/, which is not
 *     plugins/                one directory per Lua plugin, `example/` to copy
 *
 * There is no assets/ directory: a release binary carries its assets inside it
 * (FLAGS_SHIPPING sets NYA_ASSET_PREFER_BLOB), so shipping them again beside it would ship them twice.
 * The rule is "assets/ when it is not bundled", and today it always is.
 *
 * Beside the directories sit the archives the release publishes and one SHA256SUMS over them, because a
 * package manifest needs a checksum of the file a user will actually download and a directory has none.
 *
 * ## One source of truth
 *
 * This file is it. The shell scripts under packaging/ are gone: they parsed VERSION out of flags.h with sed,
 * which meant three copies of one regex against a header none of them were allowed to change. The
 * version now comes from the macro compiled into this tool, and anything outside the build system that
 * needs it runs `./build version`.
 *
 * The package manifests under packaging/ are templates with @VERSION@, @SHA256_LINUX@,
 * @SHA256_WINDOWS@ and @DATE@ in them, filled in here. A template that is obviously a template beats a
 * real looking file that is silently rewritten by a regex.
 * */
#include "build/build.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * CONSTANTS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/** Where everything below is staged. Wiped and rebuilt, so nothing may be kept here by hand. */
#define DIST_DIRECTORY "./dist"

/** The skeleton every distribution carries: data/ and plugins/, copied verbatim. */
#define DIST_RUNTIME_DIRECTORY "./packaging/runtime"

#define DIST_LICENSE_FILE   "./LICENSE"
#define DIST_CHANGELOG_FILE "./CHANGELOG.md"

/**
 * The checksum program. Not implemented here: SHA-256 is not a primitive the engine has, and adding one
 * to ship a package manifest would be a crypto implementation nobody asked for. coreutils on Linux,
 * coreutils again under MSYS2 on Windows.
 * */
#define DIST_SHA256_PROGRAM "sha256sum"

/**
 * Tokens the package templates carry. One spelling, used by every manifest, so a new channel needs no
 * new substitution rule.
 * */
#define DIST_TOKEN_VERSION        "@VERSION@"
#define DIST_TOKEN_SHA256_LINUX   "@SHA256_LINUX@"
#define DIST_TOKEN_SHA256_WINDOWS "@SHA256_WINDOWS@"
#define DIST_TOKEN_DATE           "@DATE@"

/**
 * Commits and releases one changelog may describe.
 *
 * 388 commits and no tags at the time of writing, so both bounds are roughly an order of magnitude of
 * headroom. Exceeding either is not a silent truncation; it is an assertion, because a changelog that
 * quietly stops part way through history is worse than no changelog.
 * */
#define CHANGELOG_MAX_COMMITS  4096
#define CHANGELOG_MAX_RELEASES 256

/** Field and record separators asked of `git log --pretty`. Neither can occur in a commit subject. */
#define CHANGELOG_FIELD_SEPARATOR  "\x1f"
#define CHANGELOG_RECORD_SEPARATOR "\x1e"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * TYPES
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

typedef enum DistKind DistKind;

enum DistKind {
    /** A directory a player unpacks and runs: an executable plus everything beside it. */
    DIST_KIND_GAME,
    /** A recipe for a package manager. No executable of its own; it names the archive of a game target. */
    DIST_KIND_PACKAGE,
    /** Source, and the only distribution that is: what a plugin author needs in order to write one. */
    DIST_KIND_API,
    /** A target that is planned but not built yet. Stages an empty directory, so the layout is stable. */
    DIST_KIND_PLACEHOLDER,
    DIST_KIND_COUNT,
};

/**
 * One distribution target, described where it is parsed: `./build dist <name>` finds it here, the shell
 * completions are generated from the same table, and `--help` prints these descriptions.
 * */
typedef struct DistTarget {
    NYA_ConstCString name;
    NYA_ConstCString description;
    DistKind         kind;

    /** GAME: what has to be built before there is anything to stage. */
    NYA_BuildRule* rule;

    /** GAME: the file or directory the build leaves in the working directory. */
    NYA_ConstCString payload;

    /**
     * GAME: what the executable is called inside the distribution, or nullptr when `payload` is already
     * a directory with everything in it (both Steam depots are).
     * */
    NYA_ConstCString executable;

    /** PACKAGE and API: the directory under packaging/ copied into the distribution and rendered. */
    NYA_ConstCString templates;

    /** PACKAGE: the target whose archive its manifests link to and checksum. */
    NYA_ConstCString checksums;

    /** The published archive, minus PROJECT_NAME and the version. An empty suffix means "not published". */
    NYA_ConstCString archive_suffix;

    /** Extra files copied in beside the executable, nullptr terminated. */
    NYA_ConstCString extras[5];
} DistTarget;

/** One commit, parsed. A subject that is not a conventional commit never becomes one of these. */
typedef struct ChangelogEntry {
    NYA_ConstCString type;
    NYA_ConstCString scope;
    NYA_ConstCString summary;
    b8               breaking;
} ChangelogEntry;

/** One version's worth of entries, as a window into the flat array they were parsed into. */
typedef struct ChangelogRelease {
    NYA_ConstCString version;
    NYA_ConstCString date;
    u32         first;
    u32         count;
} ChangelogRelease;

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API DECLARATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/** Runs `program` with captured output and returns its stdout. Fails loudly, since every caller needs the answer. */
NYA_INTERNAL NYA_String* _dist_capture(NYA_Arena* arena, NYA_ConstCString program, const NYA_ConstCString* arguments);

/** Runs `program`, showing its output. */
NYA_INTERNAL void _dist_run(NYA_ConstCString name, NYA_ConstCString program, const NYA_ConstCString* arguments);

/** `dist/<name>`, created empty. */
NYA_INTERNAL NYA_CString _dist_make_directory(NYA_Arena* arena, NYA_ConstCString name);

/** The lowercase hex SHA-256 of `path`. */
NYA_INTERNAL NYA_CString _dist_sha256(NYA_Arena* arena, NYA_ConstCString path);

/** Copies `source` to `directory`, keeping its basename. */
NYA_INTERNAL void _dist_copy_into(NYA_Arena* arena, NYA_ConstCString source, NYA_ConstCString directory);

/** Copies a template tree, substituting the four tokens in every file it copies. */
NYA_INTERNAL void _dist_render_tree(NYA_Arena* arena, NYA_ConstCString source, NYA_ConstCString destination);

/** Writes CHANGELOG.md, or prints only the newest release's section on stdout. What both callers share. */
NYA_INTERNAL void _changelog_write(NYA_Arena* arena, b8 release_only);

/** Stages one target, and whatever it needs staged first. Idempotent within one run. */
NYA_INTERNAL void _dist_stage(NYA_Arena* arena, const DistTarget* target);

/** Where a target's published archive goes, or nowhere when it has none. */
NYA_INTERNAL NYA_CString _dist_archive_path(NYA_Arena* arena, const DistTarget* target);

/** Archives a staged directory: .tar.gz through tar, .zip through zip, contents at the archive's root. */
NYA_INTERNAL void _dist_archive(NYA_Arena* arena, const DistTarget* target);

/** Writes dist/SHA256SUMS over every archive staged so far. */
NYA_INTERNAL void _dist_write_checksums(NYA_Arena* arena);

/** Every commit in the repository, newest first, grouped by the tag that contains it. */
NYA_INTERNAL u32 _changelog_read(NYA_Arena* arena, ChangelogEntry* entries, ChangelogRelease* releases, u32* out_entry_count);

/** Renders one release as markdown into `out`. */
NYA_INTERNAL void _changelog_render(NYA_Arena* arena, NYA_String* out, const ChangelogEntry* entries, const ChangelogRelease* release);

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * TABLES
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

// clang-format off

/**
 * Every distribution, in the order it is staged. Game targets first, because a package target's
 * manifests carry the checksum of a game target's archive and cannot be rendered before it exists.
 * */
NYA_INTERNAL const DistTarget DIST_TARGETS[] = {
// Absent on a Windows host rather than present and failing, exactly as the Linux build commands are:
// the rules they name do not exist there, and neither does the archive the three Linux package
// targets checksum. See build.h.
#if !OS_WINDOWS
    {
        .name           = "linux",
        .description    = "The portable Linux build: executable, desktop entry, icon and per user installer.",
        .kind           = DIST_KIND_GAME,
        .rule           = &build_project_linux_x86_64,
        .payload        = LINUX_X86_64_BINARY,
        .executable     = PROJECT_NAME,
        .archive_suffix = "linux-x86_64.tar.gz",
        .extras         = { "./packaging/linux/" PROJECT_NAME ".desktop", "./packaging/linux/" PROJECT_NAME ".png", "./packaging/linux/install.sh", },
    },
    {
        .name           = "steam-linux",
        .description    = "The Steam Linux Runtime depot, libsteam_api.so included. Downloads the sniper SDK sysroot once.",
        .kind           = DIST_KIND_GAME,
        .rule           = &build_project_steam_linux_x86_64,
        .payload        = STEAM_LINUX_X86_64_DIRECTORY,
        .archive_suffix = "steam-linux-x86_64.tar.gz",
    },
#endif
    {
        .name           = "windows",
        .description    = "The portable Windows build: one executable, no installer, unzip and run.",
        .kind           = DIST_KIND_GAME,
        .rule           = &build_project_windows_x86_64,
        .payload        = WINDOWS_X86_64_BINARY,
        .executable     = PROJECT_NAME ".exe",
        .archive_suffix = "windows-x86_64.zip",
    },
    {
        .name           = "steam-windows",
        .description    = "The Steam Windows depot, steam_api64.dll included.",
        .kind           = DIST_KIND_GAME,
        .rule           = &build_project_steam_windows_x86_64,
        .payload        = STEAM_WINDOWS_X86_64_DIRECTORY,
        .archive_suffix = "steam-windows-x86_64.zip",
    },
    /*
     * The wasm build, and nothing in it yet. The slot is here so the layout does not move when it
     * lands: the same dist/<target>/ shape, the same LICENSE and CHANGELOG.md, the same data/ and
     * plugins/ trees a player edits.
     *
     * What will fill it: the engine compiled to wasm, a canvas/WebGPU rendering backend, and the
     * nyangine UI compiled to HTML, CSS and JS. That is a build target and two backends, not a
     * packaging change, so it is deliberately not started here. Until the rule exists this stages an
     * empty directory rather than failing, because `./build dist` has to keep working meanwhile.
     */
    {
        .name        = "web",
        .description = "The wasm build. Empty until the target exists; see the note in dist.c.",
        .kind        = DIST_KIND_PLACEHOLDER,
    },
    {
        .name           = "lua-api",
        .description    = "What a plugin author needs: the nya.lua stub, an annotated manifest and a working example.",
        .kind           = DIST_KIND_API,
        .templates      = "./packaging/lua-api",
        .archive_suffix = "lua-api.tar.gz",
    },
#if !OS_WINDOWS
    {
        .name        = "linux-pacman",
        .description = "PKGBUILD and .SRCINFO, for makepkg and for the AUR.",
        .kind        = DIST_KIND_PACKAGE,
        .templates   = "./packaging/aur/" PROJECT_NAME "-bin",
        .checksums   = "linux",
    },
    {
        .name        = "linux-nixos",
        .description = "A nix derivation and a flake, for nix profile install and for nixpkgs.",
        .kind        = DIST_KIND_PACKAGE,
        .templates   = "./packaging/nix",
        .checksums   = "linux",
    },
    {
        .name        = "linux-flatpak",
        .description = "The Flathub manifest and its metainfo, for every distribution without a native package.",
        .kind        = DIST_KIND_PACKAGE,
        .templates   = "./packaging/flatpak",
        .checksums   = "linux",
    },
#endif
    {
        .name        = "windows-winget",
        .description = "The three winget manifests, for microsoft/winget-pkgs.",
        .kind        = DIST_KIND_PACKAGE,
        .templates   = "./packaging/windows/winget",
        .checksums   = "windows",
    },
    {
        .name        = "windows-scoop",
        .description = "The scoop manifest, for a bucket. Its autoupdate section picks up later releases on its own.",
        .kind        = DIST_KIND_PACKAGE,
        .templates   = "./packaging/windows/scoop",
        .checksums   = "windows",
    },
};

/**
 * Which conventional commit types appear in the changelog, under what heading, in this order.
 *
 * `chore` and `style` are deliberately absent: a changelog is what changed for someone who uses the
 * thing, and neither ever is. A subject that is not a conventional commit at all is skipped too; the
 * pre-MVP history this repository still carries is exactly that.
 * */
NYA_INTERNAL const struct { NYA_ConstCString type; NYA_ConstCString heading; } CHANGELOG_SECTIONS[] = {
    { "feat",     "Features"                },
    { "fix",      "Bug Fixes"               },
    { "perf",     "Performance"             },
    { "tune",     "Tuning"                  },
    { "refactor", "Refactors"               },
    { "docs",     "Documentation"           },
    { "test",     "Tests"                   },
    { "bench",    "Benchmarks"              },
    { "build",    "Build System"            },
    { "ci",       "Continuous Integration"  },
    { "revert",   "Reverts"                 },
};

// clang-format on

/**
 * Filled once per run by dist_runner, because every manifest wants the same four values and computing a
 * checksum twice is a chance for two manifests to disagree about the same file.
 * */
NYA_INTERNAL struct {
    NYA_ConstCString version;
    NYA_ConstCString date;
    NYA_ConstCString sha256_linux;
    NYA_ConstCString sha256_windows;
} _dist_render_values = { 0 };

/** Which targets have already been staged this run, indexed like DIST_TARGETS. */
NYA_INTERNAL b8 _dist_staged[nya_carray_length(DIST_TARGETS)] = { 0 };

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PUBLIC API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

void version_runner(NYA_ArgCommand* command) {
    nya_unused(command);

    // Bare, on stdout, no label: this exists so `version="$(./build version)"` is correct, which means
    // nothing else may ever be printed here.
    printf("%s\n", VERSION);
}

void changelog_runner(NYA_ArgCommand* command) {
    nya_assert(command != nullptr);

    NYA_ArgParameter* release_only = command->parameters[0];
    nya_assert(release_only != nullptr);
    nya_assert(nya_string_equals(release_only->name, "release"));

    NYA_Arena* arena = nya_arena_create(.name = "changelog_runner");
    defer nya_arena_destroy(arena);

    _changelog_write(arena, release_only->value.as_b8);
}

void _changelog_write(NYA_Arena* arena, b8 release_only) {
    nya_assert(arena != nullptr);

    ChangelogEntry*   entries  = nya_arena_alloc(arena, CHANGELOG_MAX_COMMITS * sizeof(ChangelogEntry));
    ChangelogRelease* releases = nya_arena_alloc(arena, CHANGELOG_MAX_RELEASES * sizeof(ChangelogRelease));

    u32 entry_count   = 0;
    u32 release_count = _changelog_read(arena, entries, releases, &entry_count);

    NYA_String* out = nya_string_create(arena);

    /*
     * --release is the release notes for the newest version, on stdout. Same parser, same renderer,
     * same wording as the file: the alternative was a second generator living in a workflow, which is
     * how release notes and a changelog end up describing the same tag differently.
     */
    if (release_only) {
        if (release_count == 0) {
            nya_log_warn("No commits to describe.");
            return;
        }

        _changelog_render(arena, out, entries, &releases[0]);
        nya_string_print(out);
        return;
    }

    nya_string_extend(
        out,
        "# Changelog\n"
        "\n"
        "Generated by `./build changelog` from the commit history. Never edited by hand: an entry here\n"
        "is a conventional commit subject, so fixing the wording means amending the commit.\n"
    );

    for (u32 i = 0; i < release_count; i++) {
        nya_string_extend(out, "\n");
        _changelog_render(arena, out, entries, &releases[i]);
    }

    NYA_EXPECT(nya_file_write(DIST_CHANGELOG_FILE, out), "while writing " DIST_CHANGELOG_FILE);

    nya_log_info("Wrote " DIST_CHANGELOG_FILE ": " FMTu32 " releases, " FMTu32 " entries.", release_count, entry_count);
}

void dist_runner(NYA_ArgCommand* command) {
    nya_assert(command != nullptr);

    NYA_ArgParameter* wanted = command->parameters[0];
    nya_assert(wanted != nullptr);
    nya_assert(nya_string_equals(wanted->name, "target"));

    NYA_Arena* arena = nya_arena_create(.name = "dist_runner");
    defer nya_arena_destroy(arena);

    /*
     * The values every rendered manifest shares. The date is HEAD's commit date rather than today's:
     * a distribution staged twice from the same commit has to come out the same both times.
     */
    NYA_String* head_date = _dist_capture(arena, "git", (const NYA_ConstCString[]){ "log", "-1", "--format=%cs", nullptr });
    nya_string_trim_whitespace(head_date);

    _dist_render_values.version        = VERSION;
    _dist_render_values.date           = nya_string_to_cstring(arena, head_date);
    _dist_render_values.sha256_linux   = "";
    _dist_render_values.sha256_windows = "";

    for (u32 i = 0; i < nya_carray_length(_dist_staged); i++) _dist_staged[i] = false;

    // Everything ships a current changelog, so it is generated before anything is copied rather than
    // whenever someone last remembered to run it.
    _changelog_write(arena, false);

    if (!wanted->was_matched) {
        if (nya_filesystem_exists(DIST_DIRECTORY)) NYA_EXPECT(nya_filesystem_delete_recursive(DIST_DIRECTORY), "while clearing " DIST_DIRECTORY);

        for (u32 i = 0; i < nya_carray_length(DIST_TARGETS); i++) _dist_stage(arena, &DIST_TARGETS[i]);

        _dist_write_checksums(arena);

        nya_log_info("Staged every distribution this host can produce under " DIST_DIRECTORY ".");
        return;
    }

    for (u32 i = 0; i < nya_carray_length(DIST_TARGETS); i++) {
        if (!nya_string_equals(DIST_TARGETS[i].name, wanted->value.as_string)) continue;

        _dist_stage(arena, &DIST_TARGETS[i]);
        _dist_write_checksums(arena);
        return;
    }

    // A misspelled target is user input, so it reads like one: the list of what was meant, not a panic.
    (void)fprintf(stderr, "Error: no distribution target '%s'.\n\nAvailable targets:\n", wanted->value.as_string);
    for (u32 i = 0; i < nya_carray_length(DIST_TARGETS); i++) {
        (void)fprintf(stderr, "  %-16s %s\n", DIST_TARGETS[i].name, DIST_TARGETS[i].description);
    }
    exit(EXIT_FAILURE);
}

NYA_ConstCString dist_completion_target(u32 index) {
    if (index >= nya_carray_length(DIST_TARGETS)) return nullptr;

    return DIST_TARGETS[index].name;
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

NYA_String* _dist_capture(NYA_Arena* arena, NYA_ConstCString program, const NYA_ConstCString* arguments) {
    NYA_Command command = {
        .flags   = NYA_COMMAND_FLAG_OUTPUT_CAPTURE,
        .program = program,
        .arena   = arena,
    };

    u32 count = 0;
    while (arguments[count] != nullptr) {
        nya_assert(count < NYA_COMMAND_MAX_ARGUMENTS, "'%s' was given more arguments than a command can hold.", program);
        command.arguments[count] = arguments[count];
        count++;
    }

    NYA_EXPECT(nya_command_run(&command), "while running %s", program);

    if (command.exit_code != 0) {
        (void)fprintf(stderr, "Error: %s exited %d.\n", program, command.exit_code);
        if (command.stderr_content != nullptr) (void)fprintf(stderr, NYA_FMT_STRING "\n", NYA_FMT_STRING_ARG(command.stderr_content));
        exit(EXIT_FAILURE);
    }

    return command.stdout_content != nullptr ? command.stdout_content : nya_string_create(arena);
}

void _dist_run(NYA_ConstCString name, NYA_ConstCString program, const NYA_ConstCString* arguments) {
    NYA_BuildRule rule = {
        .name    = name,
        .policy  = NYA_BUILD_ALWAYS,
        .command = { .program = program },
    };

    u32 count = 0;
    while (arguments[count] != nullptr) {
        nya_assert(count < NYA_COMMAND_MAX_ARGUMENTS, "'%s' was given more arguments than a command can hold.", program);
        rule.command.arguments[count] = arguments[count];
        count++;
    }

    NYA_EXPECT(nya_build(&rule), "while running %s", name);
}

NYA_CString _dist_make_directory(NYA_Arena* arena, NYA_ConstCString name) {
    NYA_CString path = nya_string_to_cstring(arena, nya_string_sprintf(arena, DIST_DIRECTORY "/%s", name));

    // Staging is a replacement, not a merge: a file left behind by a previous version would be shipped
    // by this one. The existence check is because deleting what is not there answers NOT_FOUND, and a
    // first run on a clean checkout is the normal case, not a failure.
    if (nya_filesystem_exists(path)) NYA_EXPECT(nya_filesystem_delete_recursive(path), "while clearing '%s'", path);
    NYA_EXPECT(nya_filesystem_create_directory(path), "while creating '%s'", path);

    return path;
}

NYA_CString _dist_sha256(NYA_Arena* arena, NYA_ConstCString path) {
    NYA_String* output = _dist_capture(arena, DIST_SHA256_PROGRAM, (const NYA_ConstCString[]){ path, nullptr });

    // "<64 hex> <two spaces or a space and a mode char> <path>", so the digest is the first word.
    NYA_ArrayᐸNYA_Stringᐳ* words = nya_string_split_words(arena, output);
    nya_assert(words->length > 0, "%s printed nothing for '%s'.", DIST_SHA256_PROGRAM, path);

    NYA_CString digest = nya_string_to_cstring(arena, &words->items[0]);
    nya_assert(strlen(digest) == 64, "%s printed a %zu character digest for '%s'.", DIST_SHA256_PROGRAM, strlen(digest), path);

    return digest;
}

void _dist_copy_into(NYA_Arena* arena, NYA_ConstCString source, NYA_ConstCString directory) {
    NYA_String* base        = nya_path_basename(arena, source);
    NYA_CString destination = nya_string_to_cstring(arena, nya_string_sprintf(arena, "%s/" NYA_FMT_STRING, directory, NYA_FMT_STRING_ARG(base)));

    NYA_EXPECT(nya_filesystem_copy_recursive(source, destination), "while copying '%s' into '%s'", source, directory);
}

/** Substitutes the four tokens in `text` in place. */
NYA_INTERNAL void _dist_substitute(NYA_String* text) {
    nya_string_replace(text, DIST_TOKEN_SHA256_LINUX, _dist_render_values.sha256_linux);
    nya_string_replace(text, DIST_TOKEN_SHA256_WINDOWS, _dist_render_values.sha256_windows);
    nya_string_replace(text, DIST_TOKEN_DATE, _dist_render_values.date);
    // Last, because the two checksum tokens above start with the same @ and a partial match would
    // leave a manifest with a version where a digest belongs.
    nya_string_replace(text, DIST_TOKEN_VERSION, _dist_render_values.version);
}

void _dist_render_tree(NYA_Arena* arena, NYA_ConstCString source, NYA_ConstCString destination) {
    NYA_ArrayᐸNYA_DirectoryEntryᐳ* entries = nullptr;
    NYA_EXPECT(nya_filesystem_list(arena, source, &entries), "while reading the templates in '%s'", source);

    NYA_EXPECT(nya_filesystem_create_directory(destination), "while creating '%s'", destination);

    nya_array_foreach (entries, entry) {
        NYA_CString name = nya_string_to_cstring(arena, entry->name);
        NYA_CString from = nya_string_to_cstring(arena, nya_string_sprintf(arena, "%s/%s", source, name));
        NYA_CString to   = nya_string_to_cstring(arena, nya_string_sprintf(arena, "%s/%s", destination, name));

        if (entry->type == NYA_FILE_TYPE_DIRECTORY) {
            _dist_render_tree(arena, from, to);
            continue;
        }

        NYA_String contents = nya_string_create_on_stack(arena);
        NYA_EXPECT(nya_file_read(from, &contents), "while reading the template '%s'", from);

        _dist_substitute(&contents);

        NYA_EXPECT(nya_file_write(to, &contents), "while writing '%s'", to);
    }
}

NYA_CString _dist_archive_path(NYA_Arena* arena, const DistTarget* target) {
    nya_assert(target->archive_suffix != nullptr);

    return nya_string_to_cstring(arena, nya_string_sprintf(arena, DIST_DIRECTORY "/" PROJECT_NAME "." VERSION ".%s", target->archive_suffix));
}

void _dist_archive(NYA_Arena* arena, const DistTarget* target) {
    NYA_CString archive = _dist_archive_path(arena, target);
    NYA_CString staged  = nya_string_to_cstring(arena, nya_string_sprintf(arena, DIST_DIRECTORY "/%s", target->name));

    /*
     * Contents at the root of the archive, no wrapping directory. Every recipe that consumes one then
     * extracts in place, and the tar and zip halves stay symmetrical, which a --transform would not be:
     * only GNU tar has one.
     */
    if (nya_string_ends_with(nya_string_from(arena, target->archive_suffix), ".zip")) {
        /*
         * zip has no -C, so it runs inside the staged directory and names the archive relative to it:
         * two levels up from dist/<target>/ is the repository root.
         */
        NYA_CString from_staged = nya_string_to_cstring(
            arena,
            nya_string_sprintf(arena, "../../" DIST_DIRECTORY "/" PROJECT_NAME "." VERSION ".%s", target->archive_suffix)
        );

        NYA_BuildRule rule = {
            .name   = "dist_archive",
            .policy = NYA_BUILD_ALWAYS,

            .command = {
                .working_directory = staged,
                .program           = "zip",
                // -X drops the extra attribute records, so the same tree zips to the same bytes twice.
                .arguments         = { "-q", "-X", "-r", from_staged, ".", nullptr },
            },
        };

        NYA_EXPECT(nya_build(&rule), "while archiving '%s'", target->name);
    } else {
        _dist_run("dist_archive", "tar", (const NYA_ConstCString[]){ "-C", staged, "-czf", archive, ".", nullptr });
    }

    nya_log_info("Archived %s -> %s", target->name, archive);
}

/**
 * Writes dist/SHA256SUMS over every archive that exists, last, so nothing can change between hashing
 * and publishing. No ./ prefix on a name: scoop's autoupdate looks entries up in this file verbatim.
 * */
NYA_INTERNAL void _dist_write_checksums(NYA_Arena* arena) {
    NYA_String* sums = nya_string_create(arena);

    for (u32 i = 0; i < nya_carray_length(DIST_TARGETS); i++) {
        if (DIST_TARGETS[i].archive_suffix == nullptr) continue;

        NYA_CString archive = _dist_archive_path(arena, &DIST_TARGETS[i]);
        if (!nya_filesystem_exists(archive)) continue;

        NYA_String* name = nya_path_basename(arena, archive);
        nya_string_extend_sprintf(sums, "%s  " NYA_FMT_STRING "\n", _dist_sha256(arena, archive), NYA_FMT_STRING_ARG(name));
    }

    if (nya_string_is_empty(sums)) return;

    NYA_EXPECT(nya_file_write(DIST_DIRECTORY "/SHA256SUMS", sums), "while writing " DIST_DIRECTORY "/SHA256SUMS");
}

void _dist_stage(NYA_Arena* arena, const DistTarget* target) {
    u32 index = (u32)(target - DIST_TARGETS);
    nya_assert(index < nya_carray_length(DIST_TARGETS), "DistTarget is not one of DIST_TARGETS.");

    if (_dist_staged[index]) return;
    _dist_staged[index] = true;

    nya_log_info("Staging %s: %s", target->name, target->description);

    /*
     * A package target's manifests link to and checksum a game target's archive, so that one is staged
     * and archived first. Naming it rather than ordering around it means `./build dist linux-nixos`
     * on its own produces a derivation with a real hash in it.
     */
    if (target->checksums != nullptr) {
        for (u32 i = 0; i < nya_carray_length(DIST_TARGETS); i++) {
            if (!nya_string_equals(DIST_TARGETS[i].name, target->checksums)) continue;
            _dist_stage(arena, &DIST_TARGETS[i]);
        }
    }

    NYA_CString directory = _dist_make_directory(arena, target->name);

    static_assert(DIST_KIND_COUNT == 4, "Unhandled DistKind enum value.");
    switch (target->kind) {
        case DIST_KIND_GAME: {
            NYA_EXPECT(nya_build(target->rule), "while building the %s distribution", target->name);

            if (target->executable != nullptr) {
                NYA_CString destination = nya_string_to_cstring(arena, nya_string_sprintf(arena, "%s/%s", directory, target->executable));
                NYA_EXPECT(nya_filesystem_copy(target->payload, destination), "while copying '%s'", target->payload);
            } else {
                // A depot: the executable and the library Valve ships beside it are already together.
                NYA_ArrayᐸNYA_DirectoryEntryᐳ* produced = nullptr;
                NYA_EXPECT(nya_filesystem_list(arena, target->payload, &produced), "while reading the '%s' depot", target->name);

                nya_array_foreach (produced, entry) {
                    NYA_CString name = nya_string_to_cstring(arena, entry->name);
                    NYA_CString from = nya_string_to_cstring(arena, nya_string_sprintf(arena, "%s/%s", target->payload, name));
                    _dist_copy_into(arena, from, directory);
                }
            }

            for (u32 i = 0; i < nya_carray_length(target->extras) && target->extras[i] != nullptr; i++) {
                _dist_copy_into(arena, target->extras[i], directory);
            }

            _dist_copy_into(arena, DIST_LICENSE_FILE, directory);
            _dist_copy_into(arena, DIST_CHANGELOG_FILE, directory);

            // data/ and plugins/, identical in every distribution, so a player who moves between the
            // portable build and a packaged one finds the same two directories in the same shape.
            NYA_ArrayᐸNYA_DirectoryEntryᐳ* runtime = nullptr;
            NYA_EXPECT(nya_filesystem_list(arena, DIST_RUNTIME_DIRECTORY, &runtime), "while reading " DIST_RUNTIME_DIRECTORY);

            nya_array_foreach (runtime, entry) {
                NYA_CString name = nya_string_to_cstring(arena, entry->name);
                NYA_CString from = nya_string_to_cstring(arena, nya_string_sprintf(arena, DIST_RUNTIME_DIRECTORY "/%s", name));
                _dist_copy_into(arena, from, directory);
            }

            break;
        }

        case DIST_KIND_API: {
            _dist_render_tree(arena, target->templates, directory);

            // The example plugin and its manifest are the runtime skeleton's, not a second copy that
            // could drift from the one every player already has.
            _dist_copy_into(arena, DIST_RUNTIME_DIRECTORY "/plugins/example", directory);
            _dist_copy_into(arena, DIST_RUNTIME_DIRECTORY "/plugins/example/manifest.nya", directory);
            _dist_copy_into(arena, DIST_LICENSE_FILE, directory);
            _dist_copy_into(arena, DIST_CHANGELOG_FILE, directory);
            break;
        }

        case DIST_KIND_PACKAGE: {
            _dist_render_tree(arena, target->templates, directory);
            break;
        }

        case DIST_KIND_PLACEHOLDER: {
            // The directory and nothing else. Reported, so an empty slot in dist/ is a decision a
            // reader can see rather than a target that silently did nothing.
            nya_log_warn("%s is a reserved slot and is staged empty. %s", target->name, target->description);
            break;
        }

        default: nya_unreachable();
    }

    if (target->archive_suffix == nullptr) return;

    _dist_archive(arena, target);

    /*
     * Recorded as soon as the archive exists, so a package target staged later renders the checksum of
     * the exact file this run produced rather than one left over from a previous one.
     */
    NYA_CString archive = nya_string_to_cstring(
        arena,
        nya_string_sprintf(arena, DIST_DIRECTORY "/" PROJECT_NAME "." VERSION ".%s", target->archive_suffix)
    );

    if (nya_string_equals(target->name, "linux")) _dist_render_values.sha256_linux = _dist_sha256(arena, archive);
    if (nya_string_equals(target->name, "windows")) _dist_render_values.sha256_windows = _dist_sha256(arena, archive);
}

u32 _changelog_read(NYA_Arena* arena, ChangelogEntry* entries, ChangelogRelease* releases, u32* out_entry_count) {
    nya_assert(entries != nullptr);
    nya_assert(releases != nullptr);
    nya_assert(out_entry_count != nullptr);

    /*
     * One walk of the history, newest first, with each commit's ref names in front of it. A tag opens a
     * new section; everything above the first tag is the unreleased one. Two separators no commit
     * subject can contain, rather than a delimiter a subject might legitimately use.
     */
    NYA_String* log = _dist_capture(
        arena,
        "git",
        (const NYA_ConstCString[]){
            "log",
            "--no-merges",
            "--decorate=short",
            "--pretty=format:%D" CHANGELOG_FIELD_SEPARATOR "%cs" CHANGELOG_FIELD_SEPARATOR "%s" CHANGELOG_RECORD_SEPARATOR,
            nullptr,
        }
    );

    NYA_ArrayᐸNYA_Stringᐳ* records = nya_string_split(arena, log, CHANGELOG_RECORD_SEPARATOR);

    u32 entry_count   = 0;
    u32 release_count = 0;

    nya_array_foreach (records, record) {
        NYA_ArrayᐸNYA_Stringᐳ* fields = nya_string_split(arena, record, CHANGELOG_FIELD_SEPARATOR);
        if (fields->length < 3) continue;

        NYA_String* refs    = &fields->items[0];
        NYA_String* date    = &fields->items[1];
        NYA_String* subject = &fields->items[2];

        nya_string_trim_whitespace(refs);
        nya_string_trim_whitespace(date);
        nya_string_trim_whitespace(subject);

        if (subject->length == 0) continue;

        /*
         * A tag on this commit closes the section above it and opens its own. "tag: v" rather than
         * "tag: ", so a non-version tag does not split the changelog into a section nobody released.
         */
        NYA_CString version = nullptr;
        if (nya_string_contains(refs, "tag: v")) {
            NYA_ArrayᐸNYA_Stringᐳ* names = nya_string_split(arena, refs, ", ");
            nya_array_foreach (names, name) {
                if (!nya_string_starts_with(name, "tag: v")) continue;

                nya_string_strip_prefix(name, "tag: v");
                version = nya_string_to_cstring(arena, name);
                break;
            }
        }

        if (version != nullptr || release_count == 0) {
            nya_assert(release_count < CHANGELOG_MAX_RELEASES, "More than %d releases in the history.", CHANGELOG_MAX_RELEASES);

            releases[release_count] = (ChangelogRelease){
                .version = version != nullptr ? version : "Unreleased",
                .date    = version != nullptr ? nya_string_to_cstring(arena, date) : "",
                .first   = entry_count,
                .count   = 0,
            };
            release_count++;
        }

        /*
         * `type(scope)!: summary`. Anything that is not that shape is not a conventional commit and is
         * left out rather than guessed at; see CHANGELOG_SECTIONS.
         */
        NYA_ArrayᐸNYA_Stringᐳ* halves = nya_string_split(arena, subject, ": ");
        if (halves->length < 2) continue;

        NYA_String* prefix = &halves->items[0];
        if (prefix->length == 0) continue;

        b8 breaking = prefix->items[prefix->length - 1] == '!';
        if (breaking) prefix->length--;

        NYA_CString            scope  = "";
        NYA_ArrayᐸNYA_Stringᐳ* pieces = nya_string_split(arena, prefix, "(");
        if (pieces->length == 2) {
            nya_string_strip_suffix(&pieces->items[1], ")");
            scope = nya_string_to_cstring(arena, &pieces->items[1]);
        }

        NYA_CString type = nya_string_to_cstring(arena, &pieces->items[0]);

        // Everything after the first ": ", which a summary containing one of its own would have split.
        NYA_String* summary = nya_string_create(arena);
        for (u64 i = 1; i < halves->length; i++) {
            if (i > 1) nya_string_extend(summary, ": ");
            nya_string_extend(summary, &halves->items[i]);
        }

        nya_assert(entry_count < CHANGELOG_MAX_COMMITS, "More than %d commits in the history.", CHANGELOG_MAX_COMMITS);

        entries[entry_count] = (ChangelogEntry){
            .type     = type,
            .scope    = scope,
            .summary  = nya_string_to_cstring(arena, summary),
            .breaking = breaking,
        };
        entry_count++;
        releases[release_count - 1].count++;
    }

    *out_entry_count = entry_count;

    return release_count;
}

/** Appends one entry as a list item, scope in bold when it has one. */
NYA_INTERNAL void _changelog_render_entry(NYA_String* out, const ChangelogEntry* entry) {
    if (entry->scope[0] != '\0') nya_string_extend_sprintf(out, "- **%s**: %s\n", entry->scope, entry->summary);
    else nya_string_extend_sprintf(out, "- %s\n", entry->summary);
}

void _changelog_render(NYA_Arena* arena, NYA_String* out, const ChangelogEntry* entries, const ChangelogRelease* release) {
    nya_unused(arena);

    if (release->date[0] != '\0') nya_string_extend_sprintf(out, "## %s - %s\n", release->version, release->date);
    else nya_string_extend_sprintf(out, "## %s\n", release->version);

    u32 end = release->first + release->count;

    /*
     * Breaking changes first and on their own, whatever type they carry. Someone reading a release note
     * is looking for what will stop working, and it has to be above the feature that caused it.
     */
    b8 any_breaking = false;
    for (u32 i = release->first; i < end; i++) {
        if (!entries[i].breaking) continue;

        if (!any_breaking) nya_string_extend(out, "\n### Breaking Changes\n\n");
        any_breaking = true;

        _changelog_render_entry(out, &entries[i]);
    }

    for (u32 section = 0; section < nya_carray_length(CHANGELOG_SECTIONS); section++) {
        b8 any = false;

        for (u32 i = release->first; i < end; i++) {
            if (entries[i].breaking) continue; // already listed above
            if (!nya_string_equals(entries[i].type, CHANGELOG_SECTIONS[section].type)) continue;

            if (!any) nya_string_extend_sprintf(out, "\n### %s\n\n", CHANGELOG_SECTIONS[section].heading);
            any = true;

            _changelog_render_entry(out, &entries[i]);
        }
    }

    if (!any_breaking && release->count == 0) nya_string_extend(out, "\nNothing worth reporting.\n");
}
