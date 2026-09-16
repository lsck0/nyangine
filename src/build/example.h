/**
 * @file example.h
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
#define EXAMPLE_BINARY_SUFFIX ".example" HOST_EXECUTABLE_SUFFIX

/**
 * Builds and runs one example.
 * */
void example_runner(NYA_ArgCommand* command);

/**
 * Name of the example at `index`, or nullptr past the last one. Fits NYA_ArgCompletion.choices_fn.
 * */
NYA_ConstCString example_completion_name(u32 index);
