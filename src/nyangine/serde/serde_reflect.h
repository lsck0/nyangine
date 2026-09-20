/**
 * @file serde_reflect.h
 *
 * A reflected struct straight to and from a file. The low-level pair is
 * nya_reflect_to_object / nya_reflect_from_object in base_reflection.h plus nya_serialize /
 * nya_deserialize in serde.h; this is the one call that is almost always what a caller wants, built
 * on top of both and exporting neither away.
 *
 * ```c
 * NYA_TRY(nya_reflect_save_file(nya_reflect_of(NYA_SettingsGraphics), &graphics, "graphics.nya", NYA_SERDE_PRETTY));
 * NYA_TRY(nya_reflect_load_file(nya_reflect_of(NYA_SettingsGraphics), &graphics, "graphics.nya", NYA_SERDE_NO_CHECKSUM));
 * ```
 *
 * Here rather than beside the reflection tables because the direction of the dependency matters: the
 * base layer knows nothing about formats or files, and a description of a type is useful without
 * either. This is the module that already owns both.
 * */
#pragma once

#include "nyangine/base/base_error.h"
#include "nyangine/base/base_reflection.h"
#include "nyangine/serde/serde_types.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * FUNCTIONS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/**
 * Writes `instance` to `path` as the format the extension names, through `type`'s description.
 *
 * Allocates from a scratch arena of its own and releases it before returning, so nothing of the
 * document outlives the call.
 * */
NYA_API NYA_Error nya_reflect_save_file(const NYA_TypeReflection* type, const void* instance, NYA_ConstCString path, NYA_SerdeFlags flags)
    __attr_no_discard;

/**
 * Reads `path` over `instance`, in place. A field the file does not mention keeps the value it had,
 * which is what lets a file written by an older build load into a struct that has since grown.
 *
 * Every problem in the document is logged as a warning naming the key, what was found and what was
 * expected, and then skipped; see nya_reflect_check. `instance` is only touched once the whole file
 * has parsed, so a syntax error leaves it exactly as it was.
 *
 * Pass NYA_SERDE_NO_CHECKSUM for a file a person is invited to edit: the native format's checksum is
 * over the contents, and an honest edit changes it.
 * */
NYA_API NYA_Error nya_reflect_load_file(const NYA_TypeReflection* type, void* instance, NYA_ConstCString path, NYA_SerdeFlags flags)
    __attr_no_discard;
