/**
 * @file cheatsheet.h
 *
 * The quick reference in docs/CHEATSHEET.md, read out of the public headers rather than written by
 * hand. Every line in it is the declaration as the header spells it, so a renamed parameter or a
 * changed return type shows up on the next build instead of rotting in a document nobody rereads.
 *
 * Only what a caller can reach is listed: `NYA_API` declarations, `nya_`/`NYA_` macros, and the
 * struct, enum and callback types. Anything spelled `_nya_`/`_NYA_` or `NYA_INTERNAL` is private
 * and left out.
 * */
#pragma once

#include "nyangine/nyangine.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * CONSTANTS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/** Where the public headers live. Every `.h` under it is scanned. */
#define NYA_CHEATSHEET_DIRECTORY "./src/nyangine"

/** The generated reference. Regenerated whenever a header changes; do not edit it. */
#define NYA_CHEATSHEET_OUTPUT "./docs/CHEATSHEET.md"

/**
 * Longest source line the scanner will hold. The tree's widest line is a formatted macro table at
 * about 150 columns, and clang-format is configured for 150; 1024 leaves room for the joined
 * multi-line declarations, which are the longest thing this builds.
 * */
#define NYA_CHEATSHEET_MAX_LINE 1024

/**
 * Longest single entry, which is a whole struct body flattened onto one line. The widest in the
 * tree is `NYA_Entity` at roughly 2 KiB of field text.
 * */
#define NYA_CHEATSHEET_MAX_ENTRY 8192

/**
 * A doc comment is only carried over when its first sentence fits, untouched, in this many
 * characters. Truncating prose would make the cheatsheet say something the header does not, so a
 * long comment is left out entirely and the reader is sent to the header.
 * */
#define NYA_CHEATSHEET_MAX_SUMMARY 110

/** Macro names already emitted for the current header, so the two arms of an #if do not both list. */
#define NYA_CHEATSHEET_MAX_SEEN 512

/**
 * An object macro is shown with its value only when the value is at most this long. Past it the
 * body is a formatted expression or a table rather than a constant, and pasting it beside the name
 * would bury the name. The widest real constant in the tree is a colour literal at 44 characters.
 * */
#define NYA_CHEATSHEET_MAX_MACRO_VALUE 48

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * FUNCTIONS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/**
 * Reads every public header and writes NYA_CHEATSHEET_OUTPUT.
 * */
void nya_cheatsheet_generate(void);
