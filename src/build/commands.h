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
 *   dist.c       `./build dist`
 *   example.c    `./build run example`
 *   test.c     `./build run test` and `./build run coverage`
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

