/**
 * @file i18n.h
 *
 * ```json
 * "hud_score": "%s scored %d points"
 * ```
 *
 * ```c
 * static inline NYA_ConstCString nya_string_hud_score(NYA_ConstCString a0, s32 a1);
 * ```
 * */
#pragma once

#include "nyangine-core/nyangine.h"

/* CONSTANTS */

/** Where the locale files live. Every `.json` directly inside is a locale. */
#define NYA_I18N_DIRECTORY "./assets/i18n"

/**
 * The locale every other one is checked against, and the one the header is generated from.
 * */
#define NYA_I18N_BASE_LOCALE "en"

/** The generated header. Regenerated whenever a locale file changes; do not edit it. */
#define NYA_I18N_OUTPUT "./src/genyarated/strings.h"

/** Most keys one locale may hold. Generous: a game's whole script is usually a few hundred lines. */
#define NYA_I18N_MAX_KEYS 1024

/** Most format arguments one string may take. Past four, a string wants a struct rather than a call. */
#define NYA_I18N_MAX_ARGUMENTS 8

/* FUNCTIONS */

/**
 * Reads every locale, validates them against the base, and writes NYA_I18N_OUTPUT.
 * */
void nya_i18n_generate(void);
