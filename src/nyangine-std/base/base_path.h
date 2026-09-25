/**
 * @file base_path.h
 * */
#pragma once

#include "nyangine-std/base/base_arena.h"
#include "nyangine-std/base/base_attributes.h"
#include "nyangine-std/base/base_string.h"
#include "nyangine-std/base/base_types.h"

// CONSTANTS

/** What the host OS uses natively. Everything here emits '/' regardless. */
#if OS_WINDOWS
#define NYA_PATH_SEPARATOR_NATIVE '\\'
#else
#define NYA_PATH_SEPARATOR_NATIVE '/'
#endif

#define NYA_PATH_SEPARATOR '/'

// FUNCTIONS AND MACROS

/**
 * Joins two path segments with exactly one separator, however many the inputs had.
 * */
NYA_API NYA_String* nya_path_join(NYA_Arena* arena, NYA_ConstCString head, NYA_ConstCString tail) __attr_no_discard;

/** Everything after the last separator. "a/b/c.txt" gives "c.txt". */
NYA_API NYA_String* nya_path_basename(NYA_Arena* arena, NYA_ConstCString path) __attr_no_discard;

/** Everything before the last separator. "a/b/c.txt" gives "a/b". No separator gives ".". */
NYA_API NYA_String* nya_path_dirname(NYA_Arena* arena, NYA_ConstCString path) __attr_no_discard;

/**
 * Extension including the dot, or empty when there is none. "a/b/c.tar.gz" gives ".gz".
 *
 * A leading dot on the basename is a hidden file, not an extension, so ".bashrc" gives "".
 * */
NYA_API NYA_String* nya_path_extension(NYA_Arena* arena, NYA_ConstCString path) __attr_no_discard;

/** Basename without the extension. "a/b/c.txt" gives "c". */
NYA_API NYA_String* nya_path_stem(NYA_Arena* arena, NYA_ConstCString path) __attr_no_discard;

/**
 * Collapses separators, resolves "." and "..", and converts '\\' to '/'.
 * */
NYA_API NYA_String* nya_path_normalize(NYA_Arena* arena, NYA_ConstCString path) __attr_no_discard;

/** True for "/..." on POSIX, and for "C:\\..." or "\\\\server" on Windows. */
NYA_API b8 nya_path_is_absolute(NYA_ConstCString path) __attr_no_discard;

/** Replaces the extension, adding one if absent. `extension` may be given with or without the dot. */
NYA_API NYA_String* nya_path_with_extension(NYA_Arena* arena, NYA_ConstCString path, NYA_ConstCString extension) __attr_no_discard;
