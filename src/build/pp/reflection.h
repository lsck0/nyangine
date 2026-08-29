/**
 * @file reflection.h
 *
 * Turns `@reflect` annotations in the engine and game headers into src/generated/reflection.{h,c}.
 *
 * ## The annotation
 *
 * A comment carrying `@reflect` immediately above a type, and optional per field comments:
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
 *
 * The comment may say anything else it likes around the annotation, so an existing doc comment gains
 * `@reflect` on a line of its own rather than being replaced.
 *
 * ## Why the parser can be this small
 *
 * It never parses C. It finds an annotation, then reads the one declaration under it, and it emits
 * every quantity it cannot trivially know as **source text for the compiler to evaluate**:
 *
 * | quantity        | emitted as                        |
 * |-----------------|-----------------------------------|
 * | field offset    | `nya_offsetof(GNY_Crate, position)` |
 * | type size       | `sizeof(GNY_Crate)`                 |
 * | array length    | the bracket text, verbatim          |
 * | enum value      | `(s64)(GNY_ENTITY_BOX)`             |
 *
 * So padding, alignment, the width a compiler chose for an enum, and the value of
 * `1ULL << 3` are all answered by the compiler that is already compiling the header. The generator
 * has no ABI model and cannot be wrong about a target it never ran on. See base_reflection.h.
 *
 * ## What it does not handle, and says so
 *
 * - **Bitfields**: `nya_offsetof` does not apply, so there is no honest offset. Annotate with `@skip`.
 * - **A field whose type is neither a builtin nor itself annotated** gets a null type and is skipped
 *   at runtime. The build prints it, because the usual cause is a forgotten `@reflect`.
 * - **Anonymous nested structs**: unnamed, so nothing can reference them. Give it a name or `@skip`.
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
 *
 * Throws — through NYA_EXPECT — on a malformed annotation, naming the file and line. A generator that
 * skipped what it could not read would produce a header that compiles and describes the wrong thing.
 * */
void nya_reflection_generate(void);
