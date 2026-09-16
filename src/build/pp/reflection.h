/**
 * @file reflection.h
 *
 * ```c
 * // @reflect
 * struct GNY_Crate {
 *     f32x3     position;                 // @hint(position)
 *     NYA_Color tint;                     // @hint(color)
 *     u32       kind;                     // @enum(GNY_EntityType)
 *     u64       flags;                    // @flags(GNY_EntityFlag)
 *     b2BodyId  body;                     // @skip
 * };
 * ```
 * */
#pragma once

#include "nyangine/nyangine.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * CONSTANTS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/** The marker. Anywhere inside a comment directly above a type declaration. */
#define NYA_REFLECT_MARKER "@reflect"

/** Where the generated pair lands. Committed, like assets.h, so a fresh clone builds. */
#define NYA_REFLECT_OUTPUT_HEADER "./src/generated/reflection.h"
#define NYA_REFLECT_OUTPUT_SOURCE "./src/generated/reflection.c"

/** Trees walked for annotations, in this order. The game's own types matter as much as the engine's. */
#define NYA_REFLECT_ENGINE_DIRECTORY "./src/nyangine"
#define NYA_REFLECT_GAME_DIRECTORY   "./src/gnyame"

/** Generous bounds. A tree that exceeds one of these fails the build rather than truncating quietly. */
#define NYA_REFLECT_MAX_TYPES    512
#define NYA_REFLECT_MAX_FIELDS   128
#define NYA_REFLECT_MAX_VARIANTS 256
#define NYA_REFLECT_MAX_NAME     128

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * FUNCTIONS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/**
 * Walks both trees, parses every annotated type, and writes the generated pair.
 * */
void nya_reflection_generate(void);
