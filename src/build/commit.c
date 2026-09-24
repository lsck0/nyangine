/**
 * @file commit.c
 *
 * `./build commit-check [<file>|<range>]`: the second entry point onto the commit-message linter, so
 * CI can hold a pull request's commits to the same rules the `commit-msg` hook holds a local commit to.
 *
 * There is no second copy of the rules here. The judgement lives in hooks/commit-msg-lint.sh — a
 * POSIX shell script the hook also runs — and this command only decides what to feed it: an argument
 * that names a file on disk is a message file, anything else is a git revision range, and no argument
 * at all is the HEAD commit. The script is the one core; the hook and this command are its two callers.
 *
 * Shell, not C, for the rules themselves: a `commit-msg` hook has to run on a bare checkout with no
 * build tool compiled yet, and reimplementing the same regexes in C would be the drift the shared core
 * exists to prevent. This runner is the thin bridge that lets `./build` and CI reach it.
 * */
#include "build/build.h"

/* CONSTANTS */

/** The shared core, relative to the repository root the build tool runs from. */
#define COMMIT_LINTER "hooks/commit-msg-lint.sh"

/** With no argument, the one commit at HEAD. A range so the script's range mode reads exactly it. */
#define COMMIT_DEFAULT_RANGE "HEAD~1..HEAD"

/* PUBLIC API IMPLEMENTATION */

void commit_check_runner(NYA_ArgCommand* command) {
    nya_assert(command != nullptr);

    NYA_ArgParameter* target = command->parameters[0];
    nya_assert(target != nullptr);
    nya_assert(nya_string_equals(target->name, "target"));

    NYA_Arena* arena = nya_arena_create(.name = "commit_check_runner");
    defer nya_arena_destroy(arena);

    // The linter has to be here to run: a checkout that removed hooks/ has nothing to enforce, and
    // saying so plainly beats a shell error about a missing script.
    if (!nya_filesystem_exists(COMMIT_LINTER)) {
        nya_log_panic("%s is missing; the commit-message linter cannot run.", COMMIT_LINTER);
    }

    // What to lint. An argument that names an existing file is a message file, passed with -f;
    // anything else is a git revision range, passed with -r; nothing given is the HEAD commit.
    NYA_ConstCString flag;
    NYA_ConstCString what;
    if (target->values_count == 0) {
        flag = "-r";
        what = COMMIT_DEFAULT_RANGE;
    } else {
        what = target->values[0].as_string;
        flag = nya_filesystem_exists(what) ? "-f" : "-r";
    }

    // Run through `sh` rather than the script directly, so enforcement does not hinge on the execute
    // bit surviving a checkout or an unpack. Its output is the user's answer, shown as it comes.
    NYA_Command lint = {
        .flags     = NYA_COMMAND_FLAG_OUTPUT_SHOW,
        .program   = "sh",
        .arena     = arena,
        .arguments = { COMMIT_LINTER, flag, what },
    };

    NYA_Error ran = nya_command_run(&lint);
    if (!ran.ok) nya_log_panic("could not run %s (%s).", COMMIT_LINTER, ran.message);

    // A non-zero exit is a real finding: the script has already printed which commit and which rule.
    if (lint.exit_code != 0) {
        nya_log_panic("commit-message check failed; see above. Fix the message, or amend the commit.");
    }

    nya_log_info("Commit-message check passed for %s.", what);
}
