/**
 * @file changelog.c
 *
 * `./build version` and `./build changelog`.
 *
 * The version is the macro compiled into this tool, printed bare so a shell can read it. Everything
 * outside the build system that needs it, the packaging scripts and the release workflow, asks for it
 * here rather than keeping its own regex over flags.h.
 *
 * The changelog is one walk of the history, newest first, with each commit's ref names in front of it.
 * A tag opens a section, a conventional commit subject becomes an entry under the heading its type maps
 * to, and anything that is not a conventional commit is left out rather than guessed at.
 *
 * One generator, two outputs. `./build changelog` writes CHANGELOG.md, which ships in every
 * distribution; `./build changelog --release` prints the newest section alone, which is what CD
 * publishes as release notes. Those were two generators before, in two places, free to describe the
 * same tag differently.
 * */
#include "build/build.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * CONSTANTS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/** Where the generated changelog goes. dist.c reads it back and ships a copy in every distribution. */
#define CHANGELOG_FILE "./CHANGELOG.md"

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

/*
 * ───────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API DECLARATION
 * ───────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/** Writes CHANGELOG.md, or prints only the newest release's section on stdout. What both callers share. */
NYA_INTERNAL void _changelog_write(NYA_Arena* arena, b8 release_only);

/** Every commit in the repository, newest first, grouped by the tag that contains it. */
NYA_INTERNAL /*
 * ───────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API IMPLEMENTATION
 * ───────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

u32 _changelog_read(NYA_Arena* arena, ChangelogEntry* entries, ChangelogRelease* releases, u32* out_entry_count);

/** Renders one release as markdown into `out`. */
NYA_INTERNAL void _changelog_render(NYA_Arena* arena, NYA_String* out, const ChangelogEntry* entries, const ChangelogRelease* release);

/*
 * ───────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PUBLIC API IMPLEMENTATION
 * ───────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

void version_runner(NYA_ArgCommand* command) {
    nya_unused(command);

    // Bare, on stdout, no label: this exists so `version="$(./build version)"` is correct, which means
    // nothing else may ever be printed here.
    printf("%s\n", VERSION);
}

NYA_String* build_capture(NYA_Arena* arena, NYA_ConstCString program, const NYA_ConstCString* arguments) {
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

    // Every caller needs the answer, so there is nothing sensible to return on a failure. The tool's
    // own words first, then out: a build system that carried on with an empty string here would
    // render a manifest with an empty version in it.
    if (command.exit_code != 0) {
        (void)fprintf(stderr, "Error: %s exited %d.\n", program, command.exit_code);
        if (command.stderr_content != nullptr) (void)fprintf(stderr, NYA_FMT_STRING "\n", NYA_FMT_STRING_ARG(command.stderr_content));
        exit(EXIT_FAILURE);
    }

    return command.stdout_content != nullptr ? command.stdout_content : nya_string_create(arena);
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

    NYA_EXPECT(nya_file_write(CHANGELOG_FILE, out), "while writing " CHANGELOG_FILE);

    nya_log_info("Wrote " CHANGELOG_FILE ": " FMTu32 " releases, " FMTu32 " entries.", release_count, entry_count);
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

u32 _changelog_read(NYA_Arena* arena, ChangelogEntry* entries, ChangelogRelease* releases, u32* out_entry_count) {
    nya_assert(entries != nullptr);
    nya_assert(releases != nullptr);
    nya_assert(out_entry_count != nullptr);

    /*
     * One walk of the history, newest first, with each commit's ref names in front of it. A tag opens a
     * new section; everything above the first tag is the unreleased one. Two separators no commit
     * subject can contain, rather than a delimiter a subject might legitimately use.
     */
    NYA_String* log = build_capture(
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
