/**
 * @file serde.h
 *
 * ```c
 * NYA_String* text = nya_serialize(arena, obj, NYA_SERDE_FORMAT_NYA, NYA_SERDE_PRETTY);
 *
 * NYA_Object* parsed = nullptr;
 * NYA_TRY(nya_deserialize(arena, text->items, text->length, NYA_SERDE_FORMAT_NYA, NYA_SERDE_NONE, &parsed));
 * ```
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
 * */
NYA_API NYA_SerdeFormat nya_serde_detect_format(const u8* data, u64 size) __attr_no_discard;

/*
 * ─────────────────────────────────────────────────────────
 * FILES
 * ─────────────────────────────────────────────────────────
 */

/**
 * Writes an object to `path`, picking the format from the extension. Through nya_file_write_atomic, so a
 * crash leaves the previous file whole rather than half of the new one.
 * */
NYA_API NYA_Error nya_serde_save_file(const NYA_Object* object, NYA_ConstCString path, NYA_SerdeFlags flags) __attr_no_discard;

/**
 * Reads an object from `path`, detecting the format from the bytes rather than the extension.
 * */
NYA_API NYA_Error nya_serde_load_file(NYA_Arena* arena, NYA_ConstCString path, NYA_SerdeFlags flags, OUT NYA_Object** out_object) __attr_no_discard;

#include "nyangine/serde/serde_cbor.h"
#include "nyangine/serde/serde_json.h"
#include "nyangine/serde/serde_jsonc.h"
#include "nyangine/serde/serde_nya.h"
#include "nyangine/serde/serde_nya_binary.h"
#include "nyangine/serde/serde_reflect.h"
