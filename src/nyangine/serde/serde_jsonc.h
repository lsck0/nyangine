/**
 * @file serde_jsonc.h
 *
 * JSON with comments, for files a person edits.
 *
 * Two things strict JSON does not allow, and both exist for the same reason — a configuration file
 * is written by hand and wants to explain itself:
 *
 * | allowed                   | example                     |
 * |---------------------------|-----------------------------|
 * | line comments             | `{ "fov": 90 } // degrees`  |
 * | block comments            | a slash-star comment inline |
 * | one trailing comma        | `{ "a": 1, }`               |
 *
 * Nothing else is relaxed. Unquoted keys, single quotes and NaN are still errors, because the point
 * is a file a strict parser can *almost* read rather than a different language.
 *
 * **Reading only.** nya_serde_jsonc_serialize writes ordinary JSON: no comments, no trailing comma.
 * Comments are what the person who wrote the file put there, and a serializer has nothing to say —
 * inventing them would also mean every consumer needed a JSONC parser. Round tripping a commented
 * file through this therefore loses the comments, which is the honest outcome and not a bug.
 *
 * The parser is shared with serde_json.h, differing only by a leniency flag, so the two dialects
 * cannot drift apart in their handling of escapes, numbers or nesting depth.
 * */
#pragma once

#include "nyangine/base/base_error.h"
#include "nyangine/base/base_object.h"
#include "nyangine/base/base_string.h"
#include "nyangine/serde/serde_types.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * FUNCTIONS AND MACROS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/**
 * Writes ordinary JSON. Identical output to nya_serde_json_serialize, including under NYA_SERDE_PRETTY.
 *
 * Present so that a caller holding NYA_SERDE_FORMAT_JSONC can serialize without special casing it,
 * not because there is a second way to write the format.
 * */
NYA_API NYA_String* nya_serde_jsonc_serialize(NYA_Arena* arena, const NYA_Object* object, NYA_SerdeFlags flags) __attr_no_discard;

/** Parses JSON, additionally allowing comments and a single trailing comma per object or array. */
NYA_API NYA_Error
nya_serde_jsonc_deserialize(NYA_Arena* arena, const u8* data, u64 size, NYA_SerdeFlags flags, OUT NYA_Object** out_object) __attr_no_discard;
