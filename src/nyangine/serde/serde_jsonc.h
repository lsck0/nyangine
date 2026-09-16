/**
 * @file serde_jsonc.h
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
 * */
NYA_API NYA_String* nya_serde_jsonc_serialize(NYA_Arena* arena, const NYA_Object* object, NYA_SerdeFlags flags) __attr_no_discard;

/** Parses JSON, additionally allowing comments and a single trailing comma per object or array. */
NYA_API NYA_Error
nya_serde_jsonc_deserialize(NYA_Arena* arena, const u8* data, u64 size, NYA_SerdeFlags flags, OUT NYA_Object** out_object) __attr_no_discard;
