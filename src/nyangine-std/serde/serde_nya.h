/**
 * @file serde_nya.h
 *
 * ```
 * nya 2 14018299633108934951
 * {
 *     version: u32 3;
 *     name: string "MyApp";
 *     fullscreen: b8 false;
 *     ratio: f32 0.5625;
 *     missing: null;
 *     tags: string[] ["fast", "small"];
 *     window: object {
 *         width: u32 1920;
 *         height: u32 1080;
 *     };
 *     grid: array[] [s32[] [1, 2], s32[] [3, 4]];
 *     mixed: any[] [s32 1, string "two", b8 true];
 * }
 * ```
 * */
#pragma once

#include "nyangine-std/base/base_error.h"
#include "nyangine-std/base/base_object.h"
#include "nyangine-std/base/base_string.h"
#include "nyangine-std/serde/serde_types.h"

// ───────────────────────────────────── CONSTANTS ─────────────────────────────────────

#define NYA_SERDE_NYA_MAGIC   "nya"
#define NYA_SERDE_NYA_VERSION 2

/** Element type name for an array whose members do not share one type. */
#define NYA_SERDE_NYA_ANY_TYPE "any"

// ───────────────────────────────────── FUNCTIONS AND MACROS ─────────────────────────────────────

NYA_API NYA_String* nya_serde_nya_serialize(NYA_Arena* arena, const NYA_Object* object, NYA_SerdeFlags flags) __attr_no_discard;

NYA_API NYA_Error nya_serde_nya_deserialize(NYA_Arena* arena, const u8* data, u64 size, NYA_SerdeFlags flags, OUT NYA_Object** out_object)
    __attr_no_discard;

/**
 * Checksum of an object tree.
 * */
NYA_API u64 nya_serde_nya_checksum(const NYA_Object* object) __attr_no_discard;
