/**
 * @file serde_json.h
 *
 * JSON reading and writing, for talking to things that are not this engine.
 *
 * **Round trips through JSON are lossy, by nature of the format.** JSON has one number type and no
 * type annotations, so the mapping back is a guess:
 *
 * | JSON            | becomes           |
 * |-----------------|-------------------|
 * | `true` `false`  | `NYA_TYPE_B8`     |
 * | `null`          | `NYA_TYPE_NULL`   |
 * | integer literal | `NYA_TYPE_S64`    |
 * | real literal    | `NYA_TYPE_F64`    |
 * | string          | `NYA_TYPE_STRING` |
 * | object          | `NYA_TYPE_OBJECT` |
 * | array           | `NYA_TYPE_ARRAY`  |
 *
 * A u8 written as JSON therefore comes back an s64. Use the nya format when the types matter.
 *
 * Writing is total: every NYA_Value has some JSON representation. The integer widths collapse onto
 * number, the b types onto true/false, and char onto a one character string. Pointer typed values
 * are written as null, since an address means nothing to whoever reads the file.
 * */
#pragma once

#include "nyangine/base/base_error.h"
#include "nyangine/base/base_object.h"
#include "nyangine/base/base_string.h"
#include "nyangine/serde/serde_types.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * CONSTANTS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/** Bounds recursion so a deeply nested or hostile document fails loudly instead of blowing the stack. */
#define NYA_SERDE_JSON_DEPTH_MAX 128

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * FUNCTIONS AND MACROS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/** Only NYA_SERDE_PRETTY is meaningful here; the nya specific flags are ignored. */
NYA_API NYA_String* nya_serde_json_serialize(NYA_Arena* arena, const NYA_Object* object, NYA_SerdeFlags flags) __attr_no_discard;

/** The document's root must be an object; a bare array or scalar is rejected. */
NYA_API NYA_Error nya_serde_json_deserialize(NYA_Arena* arena, const u8* data, u64 size, NYA_SerdeFlags flags, OUT NYA_Object** out_object)
    __attr_no_discard;

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * INTERNAL
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/**
 * The parser behind both JSON and JSONC.
 *
 * `lenient` is what separates them: it skips comments and lets a trailing comma close an object or
 * array. Everything else — the grammar, the escapes, the number handling, the depth limit — is one
 * implementation, so the two dialects cannot drift apart.
 *
 * Call nya_serde_json_deserialize or nya_serde_jsonc_deserialize rather than this.
 * */
NYA_API NYA_Error
_nya_serde_json_deserialize_with(NYA_Arena* arena, const u8* data, u64 size, NYA_SerdeFlags flags, b8 lenient, OUT NYA_Object** out_object)
    __attr_no_discard;

/** Escapes and quotes `text` as a JSON string literal, appending to `out`. */
NYA_API void nya_serde_json_escape(NYA_String* out, NYA_ConstCString text);
