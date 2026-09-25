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

#include "nyangine-core/nyangine.h"

/* CONSTANTS */

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
#define NYA_REFLECT_OUTPUT_ENGINE_HEADER "./src/genyarated/reflection_engine.h"
#define NYA_REFLECT_OUTPUT_ENGINE_SOURCE "./src/genyarated/reflection_engine.c"
// The server-safe half of the engine reflections: the builtins and every annotated type that lives in a
// module a headless build compiles (base, math, serde, net, http, db, accounts and their neighbours),
// split out of reflection_engine.c so a NYA_SERVER build has its type descriptions without compiling the
// SDL-bound half. The SDL-bound definitions (core, renderer, ui, physics, debug, replicate) stay in
// reflection_engine.c. See docs/layering-core-split.md, steps 6–7 — this is the reflection table coming
// off the wall for the server types ahead of the full per-component refactor.
#define NYA_REFLECT_OUTPUT_ENGINE_SERVER_SOURCE "./src/genyarated/reflection_engine_server.c"
#define NYA_REFLECT_OUTPUT_HEADER        "./src/genyarated/reflection.h"
#define NYA_REFLECT_OUTPUT_SOURCE        "./src/genyarated/reflection.c"

/** Trees walked for annotations, in this order. The game's own types matter as much as the engine's. The engine is three subprojects now: the stdlib, the core and the ui toolkit. */
#define NYA_REFLECT_ENGINE_STD  "./src/nyangine-std"
#define NYA_REFLECT_ENGINE_CORE "./src/nyangine-core"
#define NYA_REFLECT_ENGINE_UI   "./src/nyangine-ui"
#define NYA_REFLECT_GAME_DIRECTORY "./src/gnyame"

/** Generous bounds. A tree that exceeds one of these fails the build rather than truncating quietly. */
#define NYA_REFLECT_MAX_TYPES    512
#define NYA_REFLECT_MAX_FIELDS   128
#define NYA_REFLECT_MAX_VARIANTS 256
#define NYA_REFLECT_MAX_NAME     128

/* FUNCTIONS */

/**
 * Walks both trees, parses every annotated type, and writes the generated pair.
 * */
void nya_reflection_generate(void);
