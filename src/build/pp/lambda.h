/**
 * @file lambda.h
 *
 * Hoists a function body written at a call site out to file scope, so a callback can be written where
 * it is handed over instead of three hundred lines away.
 *
 * ```c
 * nya_sim_defer(nya_lambda(gny_locale_apply, void, (void* data), {
 *     u32 index = *(u32*)data;
 *     nya_log_info("%s", _GNY_LOCALES[index].locale);
 * }), &index, sizeof(index));
 * ```
 *
 * The tag names the function: everything after it is read by this pass and dropped by the macro, which
 * expands to `_nya_lambda_<tag>` and nothing else. See base_lambda.h for the macro and for what a
 * lambda may and may not touch.
 *
 * The bodies of one source file land in one companion header, which that file includes itself. That is
 * what keeps the body honest: it is compiled inside the file it was written in, sees exactly what is in
 * scope at the include line, and a file that forgot the include is named by this pass rather than by a
 * confusing diagnostic. Each function carries a `#line` back to the source, so a compiler error over a
 * body points at the line somebody typed.
 * */
#pragma once

#include "nyangine-core/nyangine.h"

/* CONSTANTS */

/** The macro whose call sites this pass reads. Matched as a whole identifier, outside comments and literals. */
#define NYA_LAMBDA_MARKER "nya_lambda"

/** What a tag becomes. Must agree with base_lambda.h, which pastes the same prefix onto the same tag. */
#define NYA_LAMBDA_PREFIX "_nya_lambda_"

/**
 * Where the companions land. Committed, like the other passes' output, so a fresh clone builds before
 * anything has run.
 * */
#define NYA_LAMBDA_OUTPUT_DIRECTORY "./src/genyarated/lambdas"

/**
 * Every lambda in the tree, one line each, written last.
 *
 * It is the pass's watermark: the companions come and go with the call sites, so there is no fixed set
 * of output files for nya_pp_is_current to compare against, and a directory's timestamp says nothing
 * about a file rewritten inside it. It is also the one place to look a tag up.
 * */
#define NYA_LAMBDA_OUTPUT_MANIFEST NYA_LAMBDA_OUTPUT_DIRECTORY "/manifest.txt"

/** What a source spells to include its own companion, minus the file name. Resolved through -I./src. */
#define NYA_LAMBDA_INCLUDE_PREFIX "genyarated/lambdas/"

/** Trees scanned for call sites. Each is walked sorted, so the output is a function of the tree alone. */
#define NYA_LAMBDA_TREE_ENGINE   "./src/nyangine-std", "./src/nyangine-core", "./src/nyangine-ui"
#define NYA_LAMBDA_TREE_GAME     "./src/gnyame"
#define NYA_LAMBDA_TREE_EXAMPLES "./examples"
#define NYA_LAMBDA_TREE_TESTS    "./tests"

/** Generous bounds. Exceeding one fails the build rather than truncating a body quietly. */
#define NYA_LAMBDA_MAX_LAMBDAS    256
#define NYA_LAMBDA_MAX_SOURCES    64
#define NYA_LAMBDA_MAX_NAME       128
#define NYA_LAMBDA_MAX_PARAMETERS 256
#define NYA_LAMBDA_MAX_BODY       8192
#define NYA_LAMBDA_MAX_PATH       256

/* FUNCTIONS */

/**
 * Walks every tree, reads every call site, and writes one companion per source file plus the manifest.
 *
 * Nothing is written until every call site in every tree has parsed, so a half written body leaves the
 * generated tree as it was rather than replacing a companion with a broken one.
 * */
void nya_lambda_generate(void);
