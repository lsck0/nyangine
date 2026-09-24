/**
 * @file typos.c
 *
 * `./build typos`: a spell check over the engine's prose and code with the `typos` tool
 * (github.com/crate-ci/typos), worth its own gate because the codebase carries dense literate
 * comments a mistake would otherwise sit in unnoticed.
 *
 * What it reads and what it lets pass is all in .typos.toml at the repo root: the trees to scan are
 * named here, the generated and vendored paths within them are excluded there, and every deliberate
 * non-word — the engine's coined names, its identifiers, standard domain terms, and the odd piece of
 * test data — is allowlisted there so a clean pass means a real mistake, not a domain word.
 *
 * `typos` is an optional tool, not a vendored one: a checkout without it still builds and tests. So a
 * missing binary is a skip with a notice saying how to install it, exactly as the CVE hook in sbom.c
 * treats a missing osv-scanner, rather than a hard failure over a tool that was never promised. When it
 * is present, a finding fails the command, which is what a CI gate wants.
 * */
#include "build/build.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * CONSTANTS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/** The spell checker, by name. Found on PATH, or skipped with a notice if it is not installed. */
#define TYPOS_PROGRAM "typos"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API DECLARATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/** Whether `typos --version` runs and exits cleanly, which is to say the tool is installed on PATH. */
NYA_INTERNAL b8 _typos_program_exists(void);

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PUBLIC API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

void typos_runner(NYA_ArgCommand* command) {
    nya_unused(command);

    // A missing tool is a skip with a notice, never a hard failure: the same contract the CVE hook in
    // sbom.c keeps, so a machine without `typos` still runs every other command.
    if (!_typos_program_exists()) {
        nya_log_info("Spell check skipped: '%s' is not installed. Install it with", TYPOS_PROGRAM);
        nya_log_info("  cargo install typos-cli");
        nya_log_info("or grab a binary from github.com/crate-ci/typos, then re-run `./build typos`.");
        return;
    }

    NYA_Arena* arena = nya_arena_create(.name = "typos_runner");
    defer nya_arena_destroy(arena);

    nya_log_info("Spell-checking src/, tests/, examples/ and docs/ with %s (see .typos.toml).", TYPOS_PROGRAM);

    // The trees to read. The generated and vendored paths within them, and every deliberate non-word,
    // are handled by .typos.toml, which `typos` discovers by walking up from these paths to the root.
    NYA_Command scan = {
        .flags     = NYA_COMMAND_FLAG_OUTPUT_SHOW,
        .program   = TYPOS_PROGRAM,
        .arena     = arena,
        .arguments = { "src", "tests", "examples", "docs" },
    };

    NYA_Error ran = nya_command_run(&scan);
    if (!ran.ok) nya_log_panic("could not run %s (%s).", TYPOS_PROGRAM, ran.message);

    // A non-zero exit from a checker that did run is a real finding. `typos` prints each one above.
    if (scan.exit_code != 0) {
        nya_log_panic("%s found spelling mistakes; see above. Fix them, or add a deliberate word to .typos.toml.", TYPOS_PROGRAM);
    }
    nya_log_info("Spell check: no typos in src/, tests/, examples/ or docs/.");
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

b8 _typos_program_exists(void) {
    // A program missing from PATH still spawns — the forked child fails execvp and _exit(127)s, so
    // nya_command_run returns ok with a 127 exit. Presence is the clean exit, not the spawn; `typos`
    // answers --version with 0. This is the probe sbom.c uses for osv-scanner.
    NYA_Command probe = {
        .flags     = NYA_COMMAND_FLAG_OUTPUT_SUPPRESS,
        .program   = TYPOS_PROGRAM,
        .arguments = { "--version" },
    };
    NYA_Error ran = nya_command_run(&probe);
    return ran.ok && probe.exit_code == 0;
}
