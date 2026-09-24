/**
 * @file format.c
 *
 * `./build format`: runs clang-format over the hand-written C under src/, tests/, examples/ and bench/,
 * rewriting each file in place. `--check` reports what is not formatted and fails instead of rewriting,
 * which is the shape a CI gate wants.
 *
 * The style is .clang-format at the repo root, tuned to sit as close to the codebase's hand-formatting as
 * clang-format can reach and, above all, to leave the deliberately dependency-ordered includes of the
 * unity build alone (SortIncludes: Never — reordering an `#include "*.c"` would compile a file before the
 * one it needs). It cannot reproduce the hand style exactly: the manual argument wrapping, the selective
 * alignment and the long lines kept unwrapped are human judgement a mechanical pass still moves. So this
 * is an advisory gate — run it over the lines you change rather than as a tree-wide rewrite — and it is
 * deliberately kept out of `./build check` so a formatting drift never fails the correctness gate.
 *
 * src/genyarated is not formatted here: those files are generated, and asset.c already runs clang-format
 * over the ones that should carry the style as it writes them. Corpora and kept crashes are inputs to
 * parsers, not source, so they are skipped exactly as the linter skips them.
 *
 * clang-format is an optional tool, not a vendored one: a checkout without it still builds and tests, so a
 * missing binary is a skip with a notice saying how to install it, the same contract typos.c keeps for the
 * spell checker. When it is present, `--check` fails on the first unformatted file, which is what CI wants.
 * */
#include "build/build.h"

/* CONSTANTS */

/** The formatter, by name. Found on PATH, or skipped with a notice if it is not installed. */
#define FORMAT_PROGRAM "clang-format"

/** The trees to format. src/genyarated is generated and asset.c formats it; vendors are not ours to touch. */
NYA_INTERNAL const NYA_ConstCString _FORMAT_ROOTS[] = {
    "./src/nyangine", "./src/gnyame", "./src/build", "./examples", "./tests", "./bench",
};

/** The translation-unit roots that sit outside a tree, the same two the linter names by hand. */
NYA_INTERNAL const NYA_ConstCString _FORMAT_FILES[] = { "./src/main.c", "./build.c" };

/* PRIVATE API DECLARATION */

/** Whether `clang-format --version` runs and exits cleanly, which is to say the tool is installed on PATH. */
NYA_INTERNAL b8 _format_program_exists(void);

/** Collects the .c and .h files under a walked tree into an NYA_ArrayᐸNYA_Stringᐳ, skipping corpora and crashes. */
NYA_INTERNAL b8 _format_collect(NYA_ConstCString path, const NYA_DirectoryEntry* entry, void* user_data);

/* PUBLIC API IMPLEMENTATION */

void format_runner(NYA_ArgCommand* command) {
    nya_assert(command != nullptr);

    const b8 check = command->parameters[0]->value.as_b8;

    // A missing tool is a skip with a notice, never a hard failure: the same contract typos.c keeps for
    // the spell checker, so a machine without clang-format still runs every other command.
    if (!_format_program_exists()) {
        nya_log_info("Formatting skipped: '%s' is not installed. Install your distribution's clang / llvm", FORMAT_PROGRAM);
        nya_log_info("package (it ships clang-format), then re-run `./build format`.");
        return;
    }

    NYA_Arena* arena = nya_arena_create(.name = "format_runner");
    defer nya_arena_destroy(arena);

    // Every hand-written .c and .h under the trees, plus the two roots that sit on their own. The style file at the repo root does the rest; nothing here decides how a file is formatted, only which ones. Collected on the global arena, as the linter and the bench runner collect theirs, since the walk callback is handed the array and nothing else.
    NYA_ArrayᐸNYA_Stringᐳ* files = nya_array_create(nya_arena_global, NYA_String);
    for (u32 i = 0; i < nya_carray_length(_FORMAT_ROOTS); i++) {
        NYA_EXPECT(nya_filesystem_walk(nya_arena_global, _FORMAT_ROOTS[i], _format_collect, files), "while listing %s", _FORMAT_ROOTS[i]);
    }
    for (u32 i = 0; i < nya_carray_length(_FORMAT_FILES); i++) {
        NYA_String* file = nya_string_from(nya_arena_global, _FORMAT_FILES[i]);
        nya_array_push_back(files, *file);
    }

    nya_log_info("%s " FMTu64 " files under src/, tests/, examples/ and bench/ with %s (see .clang-format).", check ? "Checking" : "Formatting",
                 files->length, FORMAT_PROGRAM);

    // Per file, so `--check` can name each one that drifted rather than dumping every warning line clang-format
    // would print. The 512-argument ceiling on a command rules out one invocation over the whole tree anyway.
    u32 unformatted = 0;
    nya_array_foreach (files, file) {
        NYA_CString path = nya_string_to_cstring(arena, file);

        NYA_Command run = {
            // Suppressed: check mode names the file itself, and write mode says nothing per file. A parse failure still surfaces through the exit code below.
            .flags   = NYA_COMMAND_FLAG_OUTPUT_SUPPRESS,
            .program = FORMAT_PROGRAM,
            .arena   = arena,
        };
        // --dry-run --Werror turns "would change" into a non-zero exit without touching the file; -i rewrites
        // it. Both take --style=file by default, which is the .clang-format at the root.
        if (check) {
            run.arguments[0] = "--dry-run";
            run.arguments[1] = "--Werror";
            run.arguments[2] = path;
        } else {
            run.arguments[0] = "-i";
            run.arguments[1] = path;
        }

        NYA_Error ran = nya_command_run(&run);
        if (!ran.ok) nya_log_panic("could not run %s on %s (%s).", FORMAT_PROGRAM, path, ran.message);

        if (run.exit_code != 0) {
            // In check mode a non-zero exit is the file needing a reformat; in write mode it is clang-format
            // failing to parse the file, which is a real error rather than a style finding.
            if (!check) nya_log_panic("%s failed on %s (exit %d).", FORMAT_PROGRAM, path, run.exit_code);
            nya_log_error("%s is not clang-formatted.", path);
            unformatted++;
        }
    }

    if (check) {
        if (unformatted > 0) {
            nya_log_panic("%u file%s not clang-formatted; see above. Run `./build format`, or format the lines you changed.", unformatted,
                          unformatted == 1 ? " is" : "s are");
        }
        nya_log_info("Format check: every file under src/, tests/, examples/ and bench/ is clang-formatted.");
        return;
    }

    nya_log_info("Formatted " FMTu64 " files. Review the diff before committing: the style cannot match every hand-formatted construct.",
                 files->length);
}

/* PRIVATE API IMPLEMENTATION */

b8 _format_program_exists(void) {
    // The probe typos.c uses for its spell checker: a program missing from PATH still spawns and the child
    // _exit(127)s, so presence is the clean exit, not the spawn. clang-format answers --version with 0.
    NYA_Command probe = {
        .flags     = NYA_COMMAND_FLAG_OUTPUT_SUPPRESS,
        .program   = FORMAT_PROGRAM,
        .arguments = { "--version" },
    };
    NYA_Error ran = nya_command_run(&probe);
    return ran.ok && probe.exit_code == 0;
}

b8 _format_collect(NYA_ConstCString path, const NYA_DirectoryEntry* entry, void* user_data) {
    NYA_ArrayᐸNYA_Stringᐳ* files = (NYA_ArrayᐸNYA_Stringᐳ*)user_data;

    // Corpora and kept crashes are inputs to parsers, not source. Filtered per file, because returning false ends the whole walk, and a directory's callback comes after its children anyway. See _lint_collect.
    if (entry->type != NYA_FILE_TYPE_FILE) return true;
    if (nya_string_contains(path, "/corpus/") || nya_string_contains(path, "/crashes/")) return true;

    NYA_String* file = nya_string_from(nya_arena_global, path);
    if (!nya_string_ends_with(file, ".c") && !nya_string_ends_with(file, ".h")) return true;

    // clang-format takes the path verbatim; nya_filesystem_walk hands it back without the leading "./".
    if (!nya_string_starts_with(file, "./")) nya_string_extend_front(file, "./");

    nya_array_push_back(files, *file);
    return true;
}
