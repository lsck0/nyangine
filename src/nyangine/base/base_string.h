#pragma once

#include "nyangine/base/base_arena.h"
#include "nyangine/base/base_array.h"
#include "nyangine/base/base_attributes.h"
#include "nyangine/base/base_types.h"

/**
 * use like: nya_log_debug("string: "NYA_FMT_STRING"\n", NYA_FMT_STRING_ARG(str))
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
 * */
NYA_API void                   nya_string_push_back(NYA_String* str, u8 character);
NYA_API void                   nya_string_print(const NYA_String* str);
NYA_API void                   nya_string_remove(NYA_String* str, NYA_ConstCString substr) __attr_overloaded;
NYA_API void                   nya_string_remove(NYA_String* str, const NYA_String* substr) __attr_overloaded;
NYA_API void                   nya_string_replace(NYA_String* str, NYA_ConstCString old, NYA_ConstCString new) __attr_overloaded;
NYA_API void                   nya_string_replace(NYA_String* str, const NYA_String* old, const NYA_String* new) __attr_overloaded;
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

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * UTF-8
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 *
 * NYA_String is bytes: `length`, indexing and every function above work on bytes.
 *
 * These walk a string as characters, for the two places that need it: the text renderer picking
 * glyphs and i18n counting what a translator wrote.
 *
 * Not a Unicode library: no normalisation, no case mapping outside ASCII, no grapheme clustering, no
 * bidirectional text. Each needs real data tables, and a half implementation is worse than none.
 */

/** How many bytes the sequence starting at `cursor` occupies, from its lead byte. Never zero. */
NYA_API u32 nya_utf8_length(NYA_ConstCString cursor) __attr_no_discard;

/**
 * Decodes one sequence into `out_codepoint` and answers how many bytes it consumed. Never zero.
 *
 * ```c
 * for (NYA_ConstCString cursor = text; *cursor != '\0';) {
 *     u32 codepoint = 0;
 *     cursor       += nya_utf8_next(cursor, &codepoint);
 *     ...
 * }
 * ```
 * */
NYA_API u32 nya_utf8_next(NYA_ConstCString cursor, OUT u32* out_codepoint);

/**
 * How many codepoints a NUL terminated string holds, which is not how many bytes it holds.
 * */
NYA_API u64 nya_utf8_count(NYA_ConstCString text) __attr_no_discard;
