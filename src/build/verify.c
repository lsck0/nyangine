/**
 * @file verify.c
 *
 * `./build verify`: bounded model checking of the engine's most exposed parsers with CBMC
 * (github.com/diffblue/cbmc), the C analogue of the Rust template's kani proofs. For the handful of
 * functions that read fully untrusted input, a proof beats a test: it checks the invariant for every
 * input under a small bound rather than the few a test or a fuzzing session happens to reach.
 *
 * The harnesses live under tests/cbmc, one .c per proof, each a `main` that feeds a target
 * nondeterministic input constrained with __CPROVER_assume and asserts its invariant with
 * __CPROVER_assert. CBMC checks --bounds-check and --pointer-check (no out-of-bounds read or write) on
 * top of whatever the harness asserts, and --unwinding-assertions makes the small unwind bound sound by
 * proving each loop really does exit within it. See the harnesses under tests/cbmc for what each proves.
 *
 * `cbmc` is an optional tool, not a vendored one: a checkout without it still builds and tests. So a
 * missing binary is a skip with a notice saying how to install it, exactly as the CVE hook in sbom.c
 * treats a missing osv-scanner, rather than a hard failure over a tool that was never promised. When it
 * is present, a failed proof fails the command, which is what a CI gate wants.
 * */
#include "build/build.h"

/* CONSTANTS */

/** The model checker, by name. Found on PATH, or skipped with a notice if it is not installed. */
#define VERIFY_PROGRAM "cbmc"

/** Where the proof harnesses live: one .c per proof. */
#define VERIFY_DIRECTORY "./tests/cbmc"

/**
 * How many times CBMC unwinds each loop before it stops.
 *
 * The harnesses bound their inputs so every loop provably finishes inside this many iterations, and
 * --unwinding-assertions turns "provably" into a checked fact: too small a bound is caught rather than
 * silently assumed away. Twelve clears the widest loop any current harness has (base64's padding strip
 * over an eight-byte input, and the CBOR argument's eight bytes) with room to spare.
 * */
#define VERIFY_UNWIND "12"

/* PRIVATE API DECLARATION */

/** Whether `cbmc --version` runs and exits cleanly, which is to say the tool is installed on PATH. */
NYA_INTERNAL b8 _verify_program_exists(void) __attr_no_discard;

/** Collects the harness paths under tests/cbmc: every .c file, so a new proof needs no registration. */
NYA_INTERNAL b8 _verify_collect_harnesses(NYA_ConstCString path, const NYA_DirectoryEntry* entry, void* user_data);

/** Byte order, so the run and its output are the same on every machine rather than the filesystem's. */
NYA_INTERNAL s32 _verify_compare_paths(const NYA_String* a, const NYA_String* b);

/* PUBLIC API IMPLEMENTATION */

void verify_runner(NYA_ArgCommand* command) {
    nya_unused(command);

    // A missing tool is a skip with a notice, never a hard failure: the same contract the CVE hook in
    // sbom.c keeps, so a machine without `cbmc` still runs every other command.
    if (!_verify_program_exists()) {
        nya_log_info("Model checking skipped: '%s' is not installed. Install it with", VERIFY_PROGRAM);
        nya_log_info("  your package manager's cbmc package, or grab a release from github.com/diffblue/cbmc,");
        nya_log_info("then re-run `./build verify`. The harnesses under " VERIFY_DIRECTORY " still compile as C without it.");
        return;
    }

    NYA_Arena* arena = nya_arena_create(.name = "verify_runner");
    defer nya_arena_destroy(arena);

    if (!nya_filesystem_is_directory(VERIFY_DIRECTORY)) {
        nya_log_warn("Nothing to verify: " VERIFY_DIRECTORY " does not exist.");
        return;
    }

    NYA_ArrayᐸNYA_Stringᐳ* harnesses = nya_array_create(arena, NYA_String);
    NYA_EXPECT(nya_filesystem_walk(arena, VERIFY_DIRECTORY, _verify_collect_harnesses, harnesses));
    nya_array_sort(harnesses, _verify_compare_paths);

    if (harnesses->length == 0) {
        nya_log_warn("Nothing to verify: no .c harness under " VERIFY_DIRECTORY ".");
        return;
    }

    nya_log_info("Model checking " FMTu64 " harness%s with %s (--bounds-check --pointer-check --unwind " VERIFY_UNWIND ").",
                 harnesses->length, harnesses->length == 1 ? "" : "es", VERIFY_PROGRAM);

    u32 failed = 0;

    nya_array_foreach (harnesses, harness) {
        NYA_ConstCString source = nya_string_to_cstring(arena, harness);

        nya_log_info("Proving %s", source);

        NYA_Command proof = {
            .flags     = NYA_COMMAND_FLAG_OUTPUT_SHOW,
            .program   = VERIFY_PROGRAM,
            .arena     = arena,
            .arguments = {
                source,

                // The two safety checks the task asks of every harness: no out-of-bounds read or write,
                // and no invalid pointer, proved for every input the harness admits.
                "--bounds-check",
                "--pointer-check",

                // The small bound, and the assertion that the bound is big enough — without it a loop
                // that could run longer would be silently assumed done, and the proof would mean less
                // than it says.
                "--unwind", VERIFY_UNWIND,
                "--unwinding-assertions",
            },
        };

        NYA_Error ran = nya_command_run(&proof);
        if (!ran.ok) nya_log_panic("could not run %s (%s).", VERIFY_PROGRAM, ran.message);

        // A non-zero exit from a checker that did run is a failed proof: a real counterexample, or a
        // loop that outran the unwind bound. CBMC prints the trace above.
        if (proof.exit_code != 0) {
            nya_log_error("Proof failed: %s", source);
            failed++;
        }
    }

    if (failed > 0) nya_log_panic("%u of " FMTu64 " proofs failed; see the traces above.", failed, harnesses->length);

    nya_log_info("Model checking: all " FMTu64 " proofs hold.", harnesses->length);
}

/* PRIVATE API IMPLEMENTATION */

b8 _verify_program_exists(void) {
    // A program missing from PATH still spawns — the forked child fails execvp and _exit(127)s, so
    // nya_command_run returns ok with a 127 exit. Presence is the clean exit, not the spawn; `cbmc`
    // answers --version with 0. This is the probe sbom.c uses for osv-scanner.
    NYA_Command probe = {
        .flags     = NYA_COMMAND_FLAG_OUTPUT_SUPPRESS,
        .program   = VERIFY_PROGRAM,
        .arguments = { "--version" },
    };
    NYA_Error ran = nya_command_run(&probe);
    return ran.ok && probe.exit_code == 0;
}

b8 _verify_collect_harnesses(NYA_ConstCString path, const NYA_DirectoryEntry* entry, void* user_data) {
    NYA_ArrayᐸNYA_Stringᐳ* harnesses = (NYA_ArrayᐸNYA_Stringᐳ*)user_data;

    if (entry->type != NYA_FILE_TYPE_FILE) return true;

    NYA_String* name = nya_string_from(nya_arena_global, path);

    if (!nya_string_ends_with(name, ".c")) return true;

    nya_array_push_back(harnesses, *name);
    return true;
}

s32 _verify_compare_paths(const NYA_String* a, const NYA_String* b) {
    u64 shared     = nya_min(a->length, b->length);
    s32 difference = nya_memcmp(a->items, b->items, shared);
    if (difference != 0) return difference < 0 ? -1 : 1;

    if (a->length == b->length) return 0;
    return a->length < b->length ? -1 : 1;
}
