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

/**
 * Where the generated files land. Committed, like assets.h, so a fresh clone builds.
 *
 * Two pairs rather than one, split along the same line the source tree is: the engine's types are
 * compiled into the engine and the game's into the game. A single table would have to live in the
 * game, and then no engine module could name its own type's description, which is exactly what
 * core_scene.c and core_settings.c do.
 * */
#define NYA_REFLECT_OUTPUT_ENGINE_HEADER "./src/generated/reflection_engine.h"
#define NYA_REFLECT_OUTPUT_ENGINE_SOURCE "./src/generated/reflection_engine.c"
#define NYA_REFLECT_OUTPUT_HEADER        "./src/generated/reflection.h"
#define NYA_REFLECT_OUTPUT_SOURCE        "./src/generated/reflection.c"

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
