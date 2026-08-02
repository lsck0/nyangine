/**
 * @file serde_nya.h
 *
 * The native serialization format.
 *
 * Every value carries its type, so a round trip is lossless in a way JSON cannot be: a u8 written
 * is a u8 read back, not a widened integer that has to be narrowed again by hand.
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
 *
 * **Header.** `nya <version> <checksum>`. The magic makes the format identifiable and stops a JSON
 * document being fed to this parser by accident. The checksum covers the object tree, not the
 * bytes, so reformatting or re-indenting a file by hand does not invalidate it.
 *
 * **Values.** `key: <type> <value>;`. The type is one of the names in NYA_TYPE_NAME_MAP. `b8`
 * through `b128` are written as `true` / `false`. `null` is written bare, with no type name.
 *
 * **Arrays.** `key: <element type>[] [a, b, c]`. Every element shares the header's type, so the
 * type is written once rather than per element. An array whose elements are not all the same type
 * uses the element type `any`, and then each element carries its own type name. Arrays nest: the
 * element type may itself be an array. An empty array is `[]` with no element type.
 *
 * **Strings.** Double quoted, with the escapes for quote, backslash, newline, tab, carriage return
 * and nul.
 *
 * **Comments.** Both block comments and line comments, anywhere whitespace is allowed.
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

#define NYA_SERDE_NYA_MAGIC   "nya"
#define NYA_SERDE_NYA_VERSION 2

/** Element type name for an array whose members do not share one type. */
#define NYA_SERDE_NYA_ANY_TYPE "any"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * FUNCTIONS AND MACROS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

NYA_API NYA_String* nya_serde_nya_serialize(NYA_Arena* arena, const NYA_Object* object, NYA_SerdeFlags flags) __attr_no_discard;

NYA_API NYA_Error nya_serde_nya_deserialize(NYA_Arena* arena, const u8* data, u64 size, NYA_SerdeFlags flags, OUT NYA_Object** out_object)
    __attr_no_discard;

/**
 * Checksum of an object tree.
 *
 * Order independent, because a dict has no order to preserve, but unlike a plain XOR of per entry
 * hashes it does not cancel: swapping two keys' values changes the result.
 * */
NYA_API u64 nya_serde_nya_checksum(const NYA_Object* object) __attr_no_discard;
