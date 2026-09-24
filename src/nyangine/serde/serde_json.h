/**
 * @file serde_json.h
 * */
#pragma once

#include "nyangine/base/base_error.h"
#include "nyangine/base/base_object.h"
#include "nyangine/base/base_string.h"
#include "nyangine/serde/serde_types.h"

// ───────────────────────────────────── CONSTANTS ─────────────────────────────────────

/** Bounds recursion so a deeply nested or hostile document fails loudly instead of blowing the stack. */
#define NYA_SERDE_JSON_DEPTH_MAX 128

// ───────────────────────────────────── FUNCTIONS AND MACROS ─────────────────────────────────────

/** Only NYA_SERDE_PRETTY is meaningful here; the nya specific flags are ignored. */
NYA_API NYA_String* nya_serde_json_serialize(NYA_Arena* arena, const NYA_Object* object, NYA_SerdeFlags flags) __attr_no_discard;

/** The document's root must be an object; a bare array or scalar is rejected. */
NYA_API NYA_Error nya_serde_json_deserialize(NYA_Arena* arena, const u8* data, u64 size, NYA_SerdeFlags flags, OUT NYA_Object** out_object)
    __attr_no_discard;

// ───────────────────────────────────── INTERNAL ─────────────────────────────────────

/**
 * The parser behind both JSON and JSONC.
 * */
NYA_API NYA_Error
_nya_serde_json_deserialize_with(NYA_Arena* arena, const u8* data, u64 size, NYA_SerdeFlags flags, b8 lenient, OUT NYA_Object** out_object)
    __attr_no_discard;

/** Escapes and quotes `text` as a JSON string literal, appending to `out`. */
NYA_API void nya_serde_json_escape(NYA_String* out, NYA_ConstCString text);
