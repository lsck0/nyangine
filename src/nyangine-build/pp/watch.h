/**
 * @file watch.h
 *
 * Writes the code that registers a function's own locals, so that a crash report can say what they
 * held. The function asks for it and says where, and this pass writes the rest.
 *
 * ```c
 * // @watch
 * u32 gny_stone_build(NYA_Vertex3D* out, u32 sides, u32 segments) {
 *     u32 at = 0;
 *     nya_watch(gny_stone_build);   // everything declared above this, parameters included
 *
 *     ...
 * }
 * ```
 *
 * `nya_watch(name)` expands to one macro per annotated function, and each lands in the companion
 * header its own source file includes; base_watch.h holds the ring they write into. The macro opens a
 * frame, records each local by name, by type as it was written, and by address, and ends the frame in a
 * `defer`, which is what makes every early return, and every `NYA_TRY` inside one, unregister on the
 * way out. A pointer that outlived its frame would be a read of somebody else's stack.
 *
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────
 *
 * Why the marker sits below the declarations
 *
 * The registration is code, so it can only take the address of a local that is already in scope where
 * it sits. Put it at the top of the body and it could name the parameters and nothing else; put it
 * where the declarations end and it covers all of them, with the values they were initialised to.
 *
 * What is watched: the parameters, and every declaration at the top level of the body above the marker
 * whose declarator is a plain name — `u32 at`, `const NYA_String* text`, `NYA_Vertex3D* out`. An array,
 * a function pointer and two declarators in one statement are not, because the generated line takes one
 * name and one `sizeof`, and neither is anything declared below the marker. Each of those is written
 * into the companion as a `not watched:` line, and the companion is committed, so an omission turns up
 * in the review of the annotation rather than in a crash report that was missing a value. Anything
 * declared in an inner block is not watched at all: its storage lifetime ends with that block, and the
 * ring holds a pointer for the life of the function.
 *
 * Generated in every build, like every other pass here. A watched frame costs a handful of stores per
 * call and nothing at all to a function that did not ask, and a crash report from a release build is
 * the one that most needs the values; assertions stay on in release for the same reason.
 * */
#pragma once

#include "nyangine-core/nyangine.h"

/* CONSTANTS */

/** The annotation. On its own line in a comment directly above the function definition. */
#define NYA_WATCH_MARKER "@watch"

/** The call site inside the body, which is the macro this pass writes the body of. */
#define NYA_WATCH_CALL "nya_watch"

/** What a function name becomes. Must agree with base_watch.h, which pastes the same prefix. */
#define NYA_WATCH_PREFIX "_nya_watch_"

/** Where the companions land. Committed, like the other passes' output, so a fresh clone builds. */
#define NYA_WATCH_OUTPUT_DIRECTORY "./src/genyarated/watches"

/**
 * Every watched function and every local it registers, one line each, written last.
 *
 * The pass's watermark, for the reason the lambda manifest is one: the companions come and go with the
 * annotations, so there is no fixed set of outputs to compare timestamps against.
 * */
#define NYA_WATCH_OUTPUT_MANIFEST NYA_WATCH_OUTPUT_DIRECTORY "/manifest.txt"

/** What a source spells to include its own companion, minus the file name. Resolved through -I./src. */
#define NYA_WATCH_INCLUDE_PREFIX "genyarated/watches/"

/** Trees scanned, each walked sorted so the output is a function of the tree alone. */
#define NYA_WATCH_TREE_ENGINE   "./src/nyangine-std", "./src/nyangine-core", "./src/nyangine-ui", "./src/nyangine-plugins"
#define NYA_WATCH_TREE_GAME     "./src/gnyame"
#define NYA_WATCH_TREE_EXAMPLES "./examples"
#define NYA_WATCH_TREE_TESTS    "./tests"

/** Generous bounds. Exceeding one fails the build rather than watching half a function. */
#define NYA_WATCH_MAX_FUNCTIONS 128
#define NYA_WATCH_MAX_SOURCES   64
#define NYA_WATCH_MAX_LOCALS    32
#define NYA_WATCH_MAX_SKIPPED   16
#define NYA_WATCH_MAX_NAME      128
#define NYA_WATCH_MAX_PATH      256

/* FUNCTIONS */

/**
 * Walks every tree, reads every annotated function, and writes one companion per source file plus the
 * manifest.
 *
 * Nothing is written until every annotation in every tree has parsed, so a function this pass cannot
 * read leaves the generated tree exactly as it was rather than half rewritten.
 * */
void nya_watch_generate(void);
