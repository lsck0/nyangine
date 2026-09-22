/**
 * @file commands.h
 *
 * The commands that are code rather than a build rule, declared together because they are consumed
 * together: cli.c is the only caller of any of them, and each fits a `void (*)(NYA_ArgCommand*)`.
 *
 * A command that can be expressed as a dependency graph is an NYA_BuildRule and lives with the rules
 * it names. A command that has to discover something on disk, filter it, or write files itself gets a
 * handler, one .c file per handler beside this header:
 *
 *   bench.c      `./build run bench`
 *   changelog.c  `./build version` and `./build changelog`
 *   check.c      `./build check`
 *   lint.c       the rules `./build check` runs before clang-tidy
 *   dist.c       `./build dist`
 *   example.c    `./build run example`
 *   fuzz.c       `./build run fuzz`
 *   agent.c      `./build run agent`
 *   simulation.c `./build run simulation`
 *   test.c       `./build run test` and `./build run coverage`
 * */
#pragma once

#include "nyangine/nyangine.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * CONSTANTS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/** Where the examples live, and what a directory must contain to be one. */
#define EXAMPLE_DIRECTORY   "./examples"
#define EXAMPLE_ENTRY_POINT "main.c"

/** Suffix of the built binary, appended to the example's directory name. */
#define EXAMPLE_BINARY_SUFFIX ".example" HOST_EXECUTABLE_SUFFIX

/** Where the fuzz targets, the corpora and the kept crashes live. */
#define FUZZ_DIRECTORY     "./tests/fuzz"
#define FUZZ_CORPUS_ROOT   FUZZ_DIRECTORY "/corpus"
#define FUZZ_CRASHES_ROOT  FUZZ_DIRECTORY "/crashes"
#define FUZZ_TARGET_PREFIX "fuzz_"

/** Where an AFL session writes its queue, its crashes and its stats. Not committed; see the workflow. */
#define FUZZ_OUTPUT_DIRECTORY "./.fuzz"

/**
 * The AFL++ compiler and driver, by name.
 *
 * Optional dependencies: a machine without them still builds and runs every target as a replay of the
 * committed corpus, which is what `./build run test` does. `./build run fuzz` is the only thing that
 * needs the real fuzzer, and it says what to install rather than failing partway through.
 * */
#define FUZZ_COMPILER_PROGRAM "afl-clang-fast"
#define FUZZ_DRIVER_PROGRAM   "afl-fuzz"

/** How the environment names the AFL++ compiler when it is not on PATH under its usual name. */
#define FUZZ_COMPILER_ENV "NYA_AFL_CC"

/**
 * The deterministic simulation runner, which is also an ordinary test.
 *
 * One binary for both: `./build run test` replays the committed regression seeds through it, and
 * `./build run simulation` runs it on one seed for as long as asked. A separate runner would be a
 * second thing to keep in step with the action set.
 * */
#define SIMULATION_SOURCE "./tests/gnyame/test_simulation.c"
#define SIMULATION_BINARY "./tests/gnyame/test_simulation"

/**
 * The agent runner, which is also an ordinary test, for the same reason the simulation is.
 *
 * `./build run test` plays a short seeded run with each kind through it, which is what proves the
 * three of them can drive the application at all. `./build run agent` trains one of them for as long
 * as asked.
 * */
#define AGENT_SOURCE "./tests/gnyame/test_agent.c"
#define AGENT_BINARY "./tests/gnyame/test_agent"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * FUNCTIONS AND MACROS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/**
 * Builds and runs the benchmarks under bench/, optionally filtered by substring.
 * */
void bench_runner(NYA_ArgCommand* command);

/**
 * Runs clang-tidy over the translation units, optionally filtered.
 * */
void check_runner(NYA_ArgCommand* command);

/**
 * The project's own rules, run by `./build check` before clang-tidy: banned calls, the module order, verb pairs,
 * callers for every NYA_API, and .clangd against the build's flags. Prints each finding and returns how many.
 * */
u32 lint_run(void);

/**
 * Builds and runs the tests, optionally filtered.
 * */
void test_runner(NYA_ArgCommand* command);

/**
 * The same, with the tests compiled under source based coverage instrumentation.
 * */
void coverage_runner(NYA_ArgCommand* command);

/**
 * Builds and runs one example.
 * */
void example_runner(NYA_ArgCommand* command);

/**
 * Name of the example at `index`, or nullptr past the last one. Fits NYA_ArgCompletion.choices_fn.
 * */
NYA_ConstCString example_completion_name(u32 index);

/**
 * Stages one distribution target, or every one this host can produce, under dist/. See dist.c.
 * */
void dist_runner(NYA_ArgCommand* command);

/**
 * Name of the distribution target at `index`, or nullptr past the last one. Fits NYA_ArgCompletion.choices_fn.
 * */
NYA_ConstCString dist_completion_target(u32 index);

/**
 * Writes CHANGELOG.md from the conventional commits in the history, or prints one release's notes.
 * */
void changelog_runner(NYA_ArgCommand* command);

/**
 * Prints VERSION on stdout and nothing else. The one thing a shell script may parse.
 * */
void version_runner(NYA_ArgCommand* command);

/**
 * Runs `program` with its output captured and hands back its stdout.
 *
 * For the commands above that need an answer rather than an effect: the version, a commit date, a
 * checksum. A non-zero exit prints what the tool wrote and ends the process, because there is nothing
 * sensible to return, and carrying on with an empty string renders a manifest with an empty version.
 * A rule that only needs to *run* something is an NYA_BuildRule and reports through nya_build.
 * */
NYA_String* build_capture(NYA_Arena* arena, NYA_ConstCString program, const NYA_ConstCString* arguments);

/**
 * Builds one fuzz target under AFL++ instrumentation and runs afl-fuzz against its committed corpus.
 *
 * With no target named it lists the ones that exist and stops. Without AFL++ installed it says which
 * programs are missing and what they are for, and stops before building anything.
 * */
void fuzz_runner(NYA_ArgCommand* command);

/** Every fuzz target name under tests/fuzz, for the completions and the usage line. */
NYA_ConstCString fuzz_completion_target_name(u32 index);

/**
 * Builds and runs one deterministic simulation. With no seed given it draws one and prints it, which
 * is what a scheduled run does; with a seed it replays exactly, which is what a failure report says.
 * */
void simulation_runner(NYA_ArgCommand* command);

/**
 * Builds and runs one training run: a DQN or a NEAT population playing gnyame as a user would,
 * headless and far faster than real time, with the assertions as the oracle.
 * */
void agent_runner(NYA_ArgCommand* command);

/** The agent kinds, for the completions and the usage line. */
NYA_ConstCString agent_completion_kind(u32 index);
