#pragma once

#include "nyangine/base/base_arena.h"
#include "nyangine/base/base_array.h"
#include "nyangine/base/base_attributes.h"
#include "nyangine/base/base_types.h"

/**
 * use like: nya_debug("string: "NYA_FMT_STRING"\n", NYA_FMT_STRING_ARG(str))
 * */
#define NYA_FMT_STRING          "%.*s"
#define NYA_FMT_STRING_ARG(str) (s32)((str)->length), ((str)->items)

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * TYPES
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

typedef NYA_Arrayᐸu8ᐳ NYA_String;
nya_derive_array(NYA_String);
nya_derive_array(NYA_CString);
nya_derive_array(NYA_ConstCString);

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * FUNCTIONS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

NYA_API b8                     nya_string_contains(const NYA_String* str, NYA_ConstCString substr) __attr_overloaded;
NYA_API b8                     nya_string_contains(const NYA_String* str, const NYA_String* substr) __attr_overloaded;
NYA_API b8                     nya_string_contains(NYA_ConstCString str, NYA_ConstCString substr) __attr_overloaded;
NYA_API NYA_String*            nya_string_create(NYA_Arena* arena);
NYA_API NYA_String*            nya_string_create_with_capacity(NYA_Arena* arena, u64 capacity);
NYA_API NYA_String             nya_string_create_on_stack(NYA_Arena* arena);
NYA_API NYA_String             nya_string_create_with_capacity_on_stack(NYA_Arena* arena, u64 capacity);
NYA_API b8                     nya_string_ends_with(const NYA_String* str, NYA_ConstCString suffix);
NYA_API b8                     nya_string_equals(NYA_ConstCString str1, NYA_ConstCString str2) __attr_overloaded;
NYA_API b8                     nya_string_equals(const NYA_String* str1, NYA_ConstCString str2) __attr_overloaded;
NYA_API b8                     nya_string_equals(const NYA_String* str1, const NYA_String* str2) __attr_overloaded;
NYA_API b8                     nya_string_is_empty(const NYA_String* str);
NYA_API b8                     nya_string_starts_with(const NYA_String* str, NYA_ConstCString prefix) __attr_overloaded;
NYA_API b8                     nya_string_starts_with(NYA_ConstCString str, NYA_ConstCString prefix) __attr_overloaded;
NYA_API NYA_String*            nya_string_clone(NYA_Arena* arena, const NYA_String* str);
NYA_API NYA_String*            nya_string_concat(NYA_Arena* arena, const NYA_String* str1, const NYA_String* str2);
NYA_API NYA_String*            nya_string_from(NYA_Arena* arena, NYA_ConstCString cstr) __attr_overloaded;
NYA_API NYA_String*            nya_string_join(NYA_Arena* arena, const NYA_ArrayᐸNYA_Stringᐳ* arr, NYA_ConstCString separator) __attr_overloaded;
NYA_API NYA_String*            nya_string_join(NYA_Arena* arena, const NYA_ArrayᐸNYA_Stringᐳ* arr, const NYA_String* separator) __attr_overloaded;
NYA_API NYA_String*            nya_string_sprintf(NYA_Arena* arena, NYA_ConstCString fmt, ...) __attr_fmt_printf(2, 3);
NYA_API NYA_String*            nya_string_substring_excld(NYA_Arena* arena, const NYA_String* str, u64 start, u64 end);
NYA_API NYA_String*            nya_string_substring_incld(NYA_Arena* arena, const NYA_String* str, u64 start, u64 end);
NYA_API NYA_ArrayᐸNYA_Stringᐳ* nya_string_split(NYA_Arena* arena, const NYA_String* str, NYA_ConstCString separator) __attr_overloaded;
NYA_API NYA_ArrayᐸNYA_Stringᐳ* nya_string_split(NYA_Arena* arena, const NYA_String* str, const NYA_String* separator) __attr_overloaded;
NYA_API NYA_ArrayᐸNYA_Stringᐳ* nya_string_split_lines(NYA_Arena* arena, const NYA_String* str);
NYA_API NYA_ArrayᐸNYA_Stringᐳ* nya_string_split_words(NYA_Arena* arena, const NYA_String* str);
NYA_API u64                    nya_string_count(const NYA_String* str, NYA_ConstCString substr) __attr_overloaded;
NYA_API u64                    nya_string_count(const NYA_String* str, const NYA_String* substr) __attr_overloaded;
NYA_API void                   nya_string_clear(NYA_String* str);
NYA_API void                   nya_string_destroy(NYA_String* str);
NYA_API void                   nya_string_destroy_on_stack(NYA_String* str);
NYA_API void                   nya_string_extend(NYA_String* str, NYA_ConstCString extension) __attr_overloaded;
NYA_API void                   nya_string_extend(NYA_String* str, const NYA_String* extension) __attr_overloaded;
NYA_API void                   nya_string_extend_front(NYA_String* str, NYA_ConstCString extension) __attr_overloaded;
NYA_API void                   nya_string_extend_front(NYA_String* str, const NYA_String* extension) __attr_overloaded;
NYA_API void                   nya_string_extend_front_sprintf(NYA_String* str, NYA_ConstCString fmt, ...) __attr_fmt_printf(2, 3);
NYA_API void                   nya_string_extend_sprintf(NYA_String* str, NYA_ConstCString fmt, ...) __attr_fmt_printf(2, 3);
/**
 * Appends one byte.
 *
 * NYA_String is an NYA_Arrayᐸu8ᐳ, so nya_array_push_back would also work; this exists so character
 * at a time building reads like the rest of the string API, and so callers stop reaching for
 * nya_string_extend with a two character buffer to append one character.
 * */
NYA_API void                   nya_string_push_back(NYA_String* str, u8 character);
NYA_API void                   nya_string_print(const NYA_String* str);
NYA_API void                   nya_string_println(const NYA_String* str);
NYA_API void                   nya_string_remove(NYA_String* str, NYA_ConstCString substr) __attr_overloaded;
NYA_API void                   nya_string_remove(NYA_String* str, NYA_String* substr) __attr_overloaded;
NYA_API void                   nya_string_replace(NYA_String* str, NYA_ConstCString old, NYA_ConstCString new) __attr_overloaded;
NYA_API void                   nya_string_replace(NYA_String* str, NYA_String* old, const NYA_String* new) __attr_overloaded;
NYA_API void                   nya_string_reserve(NYA_String* str, u64 capacity);
NYA_API void                   nya_string_reverse(NYA_String* str);
NYA_API void                   nya_string_shrink_to_fit(NYA_String* str);
NYA_API s32                    nya_string_sscanf(NYA_String* str, NYA_ConstCString fmt, ...) __attr_fmt_scanf(2, 3);
NYA_API void                   nya_string_strip_prefix(NYA_String* str, NYA_ConstCString prefix);
NYA_API void                   nya_string_strip_suffix(NYA_String* str, NYA_ConstCString suffix);
NYA_API NYA_CString            nya_string_to_cstring(NYA_Arena* arena, const NYA_String* str);
NYA_API void                   nya_string_to_lower(NYA_String* str);
NYA_API void                   nya_string_to_upper(NYA_String* str);
NYA_API void                   nya_string_trim_whitespace(NYA_String* str);
