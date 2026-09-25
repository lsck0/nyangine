/**
 * @file dist.c
 *
 * `./build dist`: turning what was built into something you can hand to someone.
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
 * What that does and does not buy, since it is easy to read as more than it is. Modification is
 * covered: _nya_asset_load_raw_from_blob verifies every entry against a *keyed* hash on first load and
 * calls nya_integrity_fail, so an edited asset stops the game rather than loading. Extraction is not
 * covered at all: the entries are LZ4 compressed, not encrypted, and NYA_ASSET_BLOB_HEADER carries each
 * one's original path and size in plaintext, so listing and unpacking the lot is a short script.
 * Encrypting them is a real change and not this file's to make, so it is written down rather than
 * attempted: monocypher is vendored and linked into every target, so XChaCha20-Poly1305 with a per
 * entry nonce would replace the siphash MAC with an AEAD tag and cost one pass over the bytes. The
 * honest part is the key, which would have to ship inside the binary that reads it. That is
 * obfuscation, not secrecy, and it should be chosen deliberately with that said out loud.
 *
 * Beside the directories sit the archives the release publishes and one SHA256SUMS over them, because a
 * package manifest needs a checksum of the file a user will actually download and a directory has none.
 *
 * ## One source of truth
 *
 * This file and changelog.c beside it. The shell scripts under packaging/ are gone: they parsed VERSION
 * out of flags.h with sed, which meant three copies of one regex against a header none of them were
 * allowed to change. The version now comes from the macro compiled into this tool, and anything outside
 * the build system that needs it runs `./build version`.
 *
 * The package manifests under packaging/ are templates with @VERSION@, @SHA256_LINUX@,
 * @SHA256_WINDOWS@ and @DATE@ in them, filled in here. A template that is obviously a template beats a
 * real looking file that is silently rewritten by a regex.
 * */
#include "nyangine-build/build.h"

/* CONSTANTS */

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

/* TYPES */

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

/* PRIVATE API DECLARATION */

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

/** Stages one target, and whatever it needs staged first. Idempotent within one run. */
NYA_INTERNAL void _dist_stage(NYA_Arena* arena, const DistTarget* target);

/** Where a target's published archive goes, or nowhere when it has none. */
NYA_INTERNAL NYA_CString _dist_archive_path(NYA_Arena* arena, const DistTarget* target);

/** Archives a staged directory: .tar.gz through tar, .zip through zip, contents at the archive's root. */
NYA_INTERNAL void _dist_archive(NYA_Arena* arena, const DistTarget* target);

/** Writes dist/SHA256SUMS over every archive staged so far. */
NYA_INTERNAL void _dist_write_checksums(NYA_Arena* arena);

/* TABLES */

// clang-format off

/**
 * Every distribution, in the order it is staged. Game targets first, because a package target's
 * manifests carry the checksum of a game target's archive and cannot be rendered before it exists.
 * */
NYA_INTERNAL const DistTarget DIST_TARGETS[] = {
// Absent on a Windows host rather than present and failing, exactly as the Linux build commands are: the rules they name do not exist there, and neither does the archive the three Linux package targets checksum. See build.h.
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
    /* The wasm build, and nothing in it yet. The slot is here so the layout does not move when it lands: the same dist/<target>/ shape, the same LICENSE and CHANGELOG.md, the same data/ and plugins/ trees a player edits. What will fill it: the engine compiled to wasm, a canvas/WebGPU rendering backend, and the nyangine UI compiled to HTML, CSS and JS. That is a build target and two backends, not a packaging change, so it is deliberately not started here. Until the rule exists this stages an empty directory rather than failing, because `./build dist` has to keep working meanwhile. */
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

/* PUBLIC API IMPLEMENTATION */

void dist_runner(NYA_ArgCommand* command) {
    nya_assert(command != nullptr);

    NYA_ArgParameter* wanted = command->parameters[0];
    nya_assert(wanted != nullptr);
    nya_assert(nya_string_equals(wanted->name, "targets"));

    NYA_Arena* arena = nya_arena_create(.name = "dist_runner");
    defer nya_arena_destroy(arena);

    /* The values every rendered manifest shares. The date is HEAD's commit date rather than today's: a distribution staged twice from the same commit has to come out the same both times. */
    NYA_String* head_date = build_capture(arena, "git", (const NYA_ConstCString[]){ "log", "-1", "--format=%cs", nullptr });
    nya_string_trim_whitespace(head_date);

    _dist_render_values.version        = VERSION;
    _dist_render_values.date           = nya_string_to_cstring(arena, head_date);
    _dist_render_values.sha256_linux   = "";
    _dist_render_values.sha256_windows = "";

    for (u32 i = 0; i < nya_carray_length(_dist_staged); i++) _dist_staged[i] = false;

    // Everything ships a current changelog, so it is generated before anything is copied rather than whenever someone last remembered to run it.
    _changelog_write(arena, false);

    if (wanted->values_count == 0) {
        // Everything, so dist/ is replaced rather than merged with whatever a previous run left.
        if (nya_filesystem_exists(DIST_DIRECTORY)) NYA_EXPECT(nya_filesystem_delete_recursive(DIST_DIRECTORY), "while clearing " DIST_DIRECTORY);

        for (u32 i = 0; i < nya_carray_length(DIST_TARGETS); i++) _dist_stage(arena, &DIST_TARGETS[i]);

        _dist_write_checksums(arena);

        nya_log_info("Staged every distribution this host can produce under " DIST_DIRECTORY ".");
        return;
    }

    /* Named targets are resolved before any of them is staged, so a typo in the fourth name fails before the first has spent five minutes compiling. */
    // Sized by what the parser can hand over rather than by the number of targets, since naming one twice is legal: staging is idempotent within a run, so it costs nothing and needs no rule.
    const DistTarget* selected[NYA_ARG_MAX_PARAMETERS] = { nullptr };

    for (u32 given = 0; given < wanted->values_count; given++) {
        NYA_CString name = wanted->values[given].as_string;

        const DistTarget* found = nullptr;
        for (u32 i = 0; i < nya_carray_length(DIST_TARGETS); i++) {
            if (nya_string_equals(DIST_TARGETS[i].name, name)) found = &DIST_TARGETS[i];
        }

        if (found == nullptr) {
            // A misspelled target is user input, so it reads like one: the list of what was meant.
            (void)fprintf(stderr, "Error: no distribution target '%s'.\n\nAvailable targets:\n", name);
            for (u32 i = 0; i < nya_carray_length(DIST_TARGETS); i++) {
                (void)fprintf(stderr, "  %-16s %s\n", DIST_TARGETS[i].name, DIST_TARGETS[i].description);
            }
            exit(EXIT_FAILURE);
        }

        nya_assert(given < nya_carray_length(selected), "more targets named than there are targets.");
        selected[given] = found;
    }

    for (u32 given = 0; given < wanted->values_count; given++) _dist_stage(arena, selected[given]);

    _dist_write_checksums(arena);
}

NYA_ConstCString dist_completion_target(u32 index) {
    if (index >= nya_carray_length(DIST_TARGETS)) return nullptr;

    return DIST_TARGETS[index].name;
}

/* PRIVATE API IMPLEMENTATION */

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

    // Staging is a replacement, not a merge: a file left behind by a previous version would be shipped by this one. The existence check is because deleting what is not there answers NOT_FOUND, and a first run on a clean checkout is the normal case, not a failure.
    if (nya_filesystem_exists(path)) NYA_EXPECT(nya_filesystem_delete_recursive(path), "while clearing '%s'", path);
    NYA_EXPECT(nya_filesystem_create_directory(path), "while creating '%s'", path);

    return path;
}

NYA_CString _dist_sha256(NYA_Arena* arena, NYA_ConstCString path) {
    NYA_String* output = build_capture(arena, DIST_SHA256_PROGRAM, (const NYA_ConstCString[]){ path, nullptr });

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

    /* Contents at the root of the archive, no wrapping directory. Every recipe that consumes one then extracts in place, and the tar and zip halves stay symmetrical, which a --transform would not be: only GNU tar has one. */
    if (nya_string_ends_with(nya_string_from(arena, target->archive_suffix), ".zip")) {
        /* zip has no -C, so it runs inside the staged directory and names the archive relative to it: two levels up from dist/<target>/ is the repository root. */
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
        /* Deterministic: entries sorted, ownership zeroed and every timestamp pinned, so the same tree archives to the same bytes and therefore the same checksum on any machine. Without it two runs of this command produce two digests for identical content, and a release that cannot be reproduced cannot be verified. The zip half above is not there yet: zip has no equivalent flag and stores an mtime per entry, so the Windows archives still differ run to run. */
        _dist_run(
            "dist_archive",
            "tar",
            (const NYA_ConstCString[]){
                "-C", staged,
                "--sort=name",
                "--owner=0", "--group=0", "--numeric-owner",
                "--mtime=@0",
                // gzip records the source mtime in its header unless told not to.
                "--use-compress-program", "gzip -n",
                "-cf", archive,
                ".",
                nullptr,
            }
        );
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

    /* A package target's manifests link to and checksum a game target's archive, so that one is staged and archived first. Naming it rather than ordering around it means `./build dist linux-nixos` on its own produces a derivation with a real hash in it. */
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

            // data/ and plugins/, identical in every distribution, so a player who moves between the portable build and a packaged one finds the same two directories in the same shape.
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

            // The example plugin and its manifest are the runtime skeleton's, not a second copy that could drift from the one every player already has.
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
            // The directory and nothing else. Reported, so an empty slot in dist/ is a decision a reader can see rather than a target that silently did nothing.
            nya_log_warn("%s is a reserved slot and is staged empty. %s", target->name, target->description);
            break;
        }

        default: nya_unreachable();
    }

    if (target->archive_suffix == nullptr) return;

    _dist_archive(arena, target);

    /* Recorded as soon as the archive exists, so a package target staged later renders the checksum of the exact file this run produced rather than one left over from a previous one. */
    NYA_CString archive = nya_string_to_cstring(
        arena,
        nya_string_sprintf(arena, DIST_DIRECTORY "/" PROJECT_NAME "." VERSION ".%s", target->archive_suffix)
    );

    if (nya_string_equals(target->name, "linux")) _dist_render_values.sha256_linux = _dist_sha256(arena, archive);
    if (nya_string_equals(target->name, "windows")) _dist_render_values.sha256_windows = _dist_sha256(arena, archive);
}

