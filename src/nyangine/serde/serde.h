/**
 * @file serde.h
 *
 * Serialization and deserialization of NYA_Object trees.
 *
 * The object model itself, NYA_Object and NYA_Value, lives in base/base_object.h. This module only
 * turns those trees into bytes and back. Two formats are supported:
 *
 * - **nya**, the native format. Types are written explicitly, so a round trip is lossless: a u8
 *   comes back a u8 and not a s64. It carries a checksum and can be obfuscated.
 * - **json**, for talking to anything that is not this engine. JSON has no type annotations, so a
 *   round trip through it is lossy by nature: every integer comes back s64 and every real f64.
 *
 * ```c
 * NYA_String* text = nya_serialize(arena, obj, NYA_SERDE_FORMAT_NYA, NYA_SERDE_PRETTY);
 *
 * NYA_Object* parsed = nullptr;
 * NYA_TRY(nya_deserialize(arena, text->items, text->length, NYA_SERDE_FORMAT_NYA, NYA_SERDE_NONE, &parsed));
 * ```
 *
 * Everything allocates from the arena passed in. Nothing here frees, so destroying the arena
 * releases the parsed tree and every string in it.
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

/** Dispatches to the format's own serializer. */
NYA_API NYA_String* nya_serialize(NYA_Arena* arena, const NYA_Object* object, NYA_SerdeFormat format, NYA_SerdeFlags flags) __attr_no_discard;

/** Dispatches to the format's own parser. */
NYA_API NYA_Error
nya_deserialize(NYA_Arena* arena, const u8* data, u64 size, NYA_SerdeFormat format, NYA_SerdeFlags flags, OUT NYA_Object** out_object)
    __attr_no_discard;

/**
 * Guesses the format from the bytes themselves.
 *
 * The nya format starts with its magic, obfuscated or not, and JSON with `{` or whitespace then
 * `{`. Anything else is reported as NYA_SERDE_FORMAT_COUNT, meaning unknown.
 * */
NYA_API NYA_SerdeFormat nya_serde_detect_format(const u8* data, u64 size) __attr_no_discard;

#include "nyangine/serde/serde_json.h"
#include "nyangine/serde/serde_nya.h"
