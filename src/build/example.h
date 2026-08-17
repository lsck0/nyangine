/**
 * @file example.h
 *
 * The example command: builds one directory under examples/ and runs it.
 *
 * An example is a directory containing a `main.c` that includes the engine and provides its own
 * entry point — `examples/hello_world/main.c` is the shape. Nothing about it is registered anywhere:
 * the directory *is* the registration, so adding an example is adding a folder and no edit to the
 * build system.
 *
 * A command handler rather than a set of NYA_BuildRules, for the same reason test.h is one: which
 * examples exist is only known by listing the directory, and the rule has to be built from what is
 * found. What it produces is still an ordinary rule, so an example compile is reported like any
 * other target.
 *
 * The binary lands at the repo root as `<name>.example`, next to the game's own artifacts, because
 * the Linux rules link with `-Wl,-rpath,$ORIGIN` and the vendored shared objects sit there. Building
 * it inside its own directory would produce something that cannot find libSDL3 at runtime.
 * */
#pragma once

#include "nyangine/nyangine.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * FUNCTIONS AND MACROS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/** Where the examples live, and what a directory must contain to be one. */
#define EXAMPLE_DIRECTORY   "./examples"
#define EXAMPLE_ENTRY_POINT "main.c"

/** Suffix of the built binary, appended to the example's directory name. See the note in this file's header. */
#define EXAMPLE_BINARY_SUFFIX ".example"

/**
 * Builds and runs one example.
 *
 * `./build run example hello_world` compiles `examples/hello_world/main.c` into `hello_world.example`
 * and executes it. With no name, or with one that names no directory, it lists what is available and
 * fails — which is the useful answer to a typo, and the only discovery mechanism a directory based
 * registry can offer.
 * */
void example_runner(NYA_ArgCommand* command);

/**
 * Name of the example at `index`, or nullptr past the last one. Fits NYA_ArgCompletion.choices_fn.
 *
 * Lists the directory on first call and caches the result, because the completion machinery asks for
 * one name at a time and re-listing per index would be quadratic in the number of examples.
 * */
NYA_ConstCString example_completion_name(u32 index);
