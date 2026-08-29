/**
 * @file i18n.h
 *
 * Turns every `.json` under `assets/i18n` into `src/generated/strings.h`, and refuses to build a translation that would
 * crash.
 *
 * The generated header is what makes the runtime typesafe. For every key in the base locale it emits
 * an enum entry and an inline function whose parameters are read off the format specifiers in the
 * base string, so this:
 *
 * ```json
 * "hud_score": "%s scored %d points"
 * ```
 *
 * becomes this:
 *
 * ```c
 * static inline NYA_ConstCString nya_string_hud_score(NYA_ConstCString a0, s32 a1);
 * ```
 *
 * A call with the wrong number of arguments, or with an `f32` where the string wants a `%s`, is a
 * compile error at the call site. That is the whole point: the alternative is a printf with a format
 * string chosen at runtime by the player's locale setting, which the compiler cannot check at all and
 * which crashes in whichever language nobody on the team reads.
 *
 * ## What is validated, and why each one is a real bug
 *
 * Every non-base locale is checked against the base:
 *
 * - **A missing key** would fall back to the base language silently, so a half-finished translation
 *   ships looking finished.
 * - **An extra key** is a key that was renamed in the base and not here, so the translation for it is
 *   already dead and nobody would notice.
 * - **A different specifier sequence** is the dangerous one. `"%s scored %d"` translated as
 *   `"%d Punkte für %s"` reads a pointer as an integer and an integer as a pointer — a crash, or
 *   worse, in exactly one language.
 *
 * All three fail the build with the key named. A warning would be read once and then never again.
 *
 * ## Reordering, for languages that need it
 *
 * A translation may reorder its arguments with positional specifiers — `%2$s` before `%1$d` — and
 * the check compares the *sorted* set of specifiers rather than their order. That is the one case
 * where a different order is correct rather than a bug, and it is common enough that refusing it
 * would make the system unusable for German and Japanese alike.
 * */
#pragma once

#include "nyangine/nyangine.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * CONSTANTS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/** Where the locale files live. Every `.json` directly inside is a locale. */
#define NYA_I18N_DIRECTORY "./assets/i18n"

/**
 * The locale every other one is checked against, and the one the header is generated from.
 *
 * English because that is what the keys are written in; the choice matters only in that exactly one
 * locale has to be the schema, and a schema that changes with the build is not one.
 * */
#define NYA_I18N_BASE_LOCALE "en"

/** The generated header. Regenerated whenever a locale file changes; do not edit it. */
#define NYA_I18N_OUTPUT "./src/generated/strings.h"

/** Most keys one locale may hold. Generous: a game's whole script is usually a few hundred lines. */
#define NYA_I18N_MAX_KEYS 1024

/** Most format arguments one string may take. Past four, a string wants a struct rather than a call. */
#define NYA_I18N_MAX_ARGUMENTS 8

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * FUNCTIONS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/**
 * Reads every locale, validates them against the base, and writes NYA_I18N_OUTPUT.
 *
 * Throws — through NYA_EXPECT — when a locale disagrees with the base, naming the locale and the key.
 * A build that produced a header from an inconsistent set would be a build that compiles and then
 * crashes for one language, which is strictly worse than not building.
 * */
void nya_i18n_generate(void);
