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

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * UTF-8
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 *
 * NYA_String holds bytes and nothing here changes that. `length` is bytes, indexing is bytes, and
 * every function above operates on bytes — which is right, because that is what a string *is* in
 * memory and what a file holds.
 *
 * What these add is the ability to walk a string as *characters* where that is the question being
 * asked. Two places need it and both are load bearing: the text renderer, which has to know which
 * glyph to draw, and i18n, which has to count the characters a translator wrote rather than the
 * bytes their language happens to need.
 *
 * Deliberately not a full Unicode library. There is no normalisation, no case mapping outside ASCII,
 * no grapheme clustering and no bidirectional algorithm. Each of those is a real feature with a real
 * data table behind it, and pretending otherwise by adding a half-implementation is worse than not
 * having one.
 */

/** How many bytes the sequence starting at `cursor` occupies, from its lead byte. Never zero. */
NYA_API u32 nya_utf8_length(NYA_ConstCString cursor) __attr_no_discard;

/**
 * Decodes one sequence into `out_codepoint` and answers how many bytes it consumed. Never zero.
 *
 * Malformed input decodes as U+FFFD and consumes exactly one byte. That is not a detail: consuming
 * zero spins forever, and consuming the length a truncated lead byte *claimed* reads past the end of
 * the buffer. One byte is the only answer that both makes progress and stays in bounds.
 *
 * Overlong encodings and surrogates are rejected the same way. Both are ways of spelling something
 * that has a shorter or no legal encoding, and accepting them is how a decoder becomes a security
 * problem — a filter checking for a literal NUL never sees the two-byte spelling of one.
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
 *
 * The difference is the whole reason this exists: `"Grüße"` is five characters and seven bytes, and
 * anything that lays text out or truncates it by byte count gets both wrong.
 * */
NYA_API u64 nya_utf8_count(NYA_ConstCString text) __attr_no_discard;
