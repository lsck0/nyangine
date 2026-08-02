#pragma once

#include "nyangine/base/base_arena.h"
#include "nyangine/base/base_error.h"
#include "nyangine/base/base_string.h"
#include "nyangine/base/base_types.h"
#include "nyangine/platform/filesystem/filesystem.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * FILE DESCRIPTOR FUNCTIONS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/*
 * Whole-content helpers over an open NYA_File.
 *
 * These took a raw `s32 fd` before, which quietly made them POSIX only. NYA_File wraps a
 * descriptor or a Windows HANDLE, so the same code works on both.
 */

NYA_API NYA_Error nya_file_read_string(NYA_File* file, OUT NYA_String* out_content) __attr_no_discard;
NYA_API NYA_Error nya_file_write_string(NYA_File* file, const NYA_String* content) __attr_overloaded __attr_no_discard;
NYA_API NYA_Error nya_file_write_string(NYA_File* file, NYA_ConstCString content) __attr_overloaded __attr_no_discard;
NYA_API NYA_Error nya_file_append_string(NYA_File* file, const NYA_String* content) __attr_overloaded __attr_no_discard;
NYA_API NYA_Error nya_file_append_string(NYA_File* file, NYA_ConstCString content) __attr_overloaded __attr_no_discard;

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * FILE FUNCTIONS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

NYA_API NYA_Error nya_file_read(const char* path, OUT NYA_String* out_content) __attr_overloaded __attr_no_discard;
NYA_API NYA_Error nya_file_read(const NYA_String* path, OUT NYA_String* out_content) __attr_overloaded __attr_no_discard;
NYA_API NYA_Error nya_file_write(const char* path, const NYA_String* content) __attr_overloaded __attr_no_discard;
NYA_API NYA_Error nya_file_write(const NYA_String* path, const NYA_String* content) __attr_overloaded __attr_no_discard;
NYA_API NYA_Error nya_file_write(const char* path, NYA_ConstCString content) __attr_overloaded __attr_no_discard;
NYA_API NYA_Error nya_file_write(const NYA_String* path, NYA_ConstCString content) __attr_overloaded __attr_no_discard;
NYA_API NYA_Error nya_file_append(const char* path, const NYA_String* content) __attr_overloaded __attr_no_discard;
NYA_API NYA_Error nya_file_append(const NYA_String* path, const NYA_String* content) __attr_overloaded __attr_no_discard;
NYA_API NYA_Error nya_file_append(const char* path, NYA_ConstCString content) __attr_overloaded __attr_no_discard;
NYA_API NYA_Error nya_file_append(const NYA_String* path, NYA_ConstCString content) __attr_overloaded __attr_no_discard;
