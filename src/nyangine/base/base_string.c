#include "nyangine/nyangine.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API DECLARATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

NYA_INTERNAL NYA_CString _nya_strstrn(NYA_ConstCString haystack, NYA_ConstCString needle, u64 haystack_len, u64 needle_len);

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PUBLIC API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

b8 nya_string_contains(const NYA_String* str, NYA_ConstCString substr) __attr_overloaded {
    nya_assert(str != nullptr);
    nya_assert(substr != nullptr);

    return _nya_strstrn((NYA_CString)str->items, substr, str->length, strlen(substr)) != nullptr;
}

b8 nya_string_contains(const NYA_String* str, const NYA_String* substr) __attr_overloaded {
    nya_assert(str != nullptr);
    nya_assert(substr != nullptr);

    return _nya_strstrn((NYA_CString)str->items, (NYA_CString)substr->items, str->length, substr->length) != nullptr;
}

b8 nya_string_contains(NYA_ConstCString str, NYA_ConstCString substr) __attr_overloaded {
    nya_assert(str != nullptr);
    nya_assert(substr != nullptr);

    return _nya_strstrn(str, substr, strlen(str), strlen(substr)) != nullptr;
}

b8 nya_string_ends_with(const NYA_String* str, NYA_ConstCString suffix) {
    nya_assert(str != nullptr);
    nya_assert(suffix != nullptr);

    u64 str_length    = str->length;
    u64 suffix_length = strlen(suffix);

    if (str_length < suffix_length) return false;

    return nya_memcmp(str->items + str_length - suffix_length, suffix, suffix_length) == 0;
}

b8 nya_string_equals(NYA_ConstCString str1, NYA_ConstCString str2) __attr_overloaded {
    nya_assert(str1 != nullptr);
    nya_assert(str2 != nullptr);

    u64 str1_length = strlen(str1);
    u64 str2_length = strlen(str2);
    if (str1_length != str2_length) return false;

    return nya_memcmp(str1, str2, str1_length) == 0;
}

b8 nya_string_equals(const NYA_String* str1, NYA_ConstCString str2) __attr_overloaded {
    nya_assert(str1 != nullptr);
    nya_assert(str2 != nullptr);

    u64 str1_length = str1->length;
    u64 str2_length = strlen(str2);
    if (str1_length != str2_length) return false;

    return nya_memcmp(str1->items, str2, str1_length) == 0;
}

b8 nya_string_equals(const NYA_String* str1, const NYA_String* str2) __attr_overloaded {
    nya_assert(str1 != nullptr);
    nya_assert(str2 != nullptr);

    if (str1->length != str2->length) return false;

    return nya_memcmp(str1->items, str2->items, str1->length) == 0;
}

b8 nya_string_is_empty(const NYA_String* str) {
    nya_assert(str != nullptr);

    return str->length == 0;
}

b8 nya_string_starts_with(const NYA_String* str, NYA_ConstCString prefix) __attr_overloaded {
    nya_assert(str != nullptr);
    nya_assert(prefix != nullptr);

    u64 str_length    = str->length;
    u64 prefix_length = strlen(prefix);

    if (str_length < prefix_length) return false;

    return nya_memcmp(str->items, prefix, prefix_length) == 0;
}

b8 nya_string_starts_with(NYA_ConstCString str, NYA_ConstCString prefix) __attr_overloaded {
    nya_assert(str != nullptr);
    nya_assert(prefix != nullptr);

    u64 str_length    = strlen(str);
    u64 prefix_length = strlen(prefix);

    if (str_length < prefix_length) return false;

    return nya_memcmp(str, prefix, prefix_length) == 0;
}

NYA_String* nya_string_clone(NYA_Arena* arena, const NYA_String* str) {
    nya_assert(arena != nullptr);
    nya_assert(str != nullptr);

    NYA_String* result = nya_string_create_with_capacity(arena, str->length);
    if (str->length > 0) { nya_memcpy(result->items, str->items, str->length); }
    result->length = str->length;

    return result;
}

NYA_String* nya_string_concat(NYA_Arena* arena, const NYA_String* str1, const NYA_String* str2) {
    nya_assert(arena != nullptr);
    nya_assert(str1 != nullptr);
    nya_assert(str2 != nullptr);

    NYA_String* result = nya_string_create_with_capacity(arena, str1->length + str2->length);
    nya_memcpy(result->items, str1->items, str1->length);
    nya_memcpy(result->items + str1->length, str2->items, str2->length);
    result->length = str1->length + str2->length;

    return result;
}

NYA_String* nya_string_from(NYA_Arena* arena, NYA_ConstCString cstr) __attr_overloaded {
    nya_assert(arena != nullptr);
    nya_assert(cstr != nullptr);

    u64 length = strlen(cstr);
    if (length == 0) return nya_string_create(arena);

    NYA_String* result = nya_string_create_with_capacity(arena, length);
    nya_memcpy(result->items, cstr, length);
    result->length = length;

    return result;
}

NYA_String* nya_string_join(NYA_Arena* arena, const NYA_ArrayᐸNYA_Stringᐳ* arr, NYA_ConstCString separator) __attr_overloaded {
    nya_assert(arena != nullptr);
    nya_assert(arr != nullptr);
    nya_assert(separator != nullptr);

    if (arr->length == 0) return nya_string_create(arena);

    u64 separator_length = strlen(separator);
    u64 total_length     = 0;

    for (u64 i = 0; i < arr->length; i++) total_length += arr->items[i].length;
    total_length += (arr->length - 1) * separator_length;

    NYA_String* result = nya_string_create_with_capacity(arena, total_length);
    for (u64 i = 0; i < arr->length; i++) {
        nya_memmove(result->items + result->length, arr->items[i].items, arr->items[i].length);
        result->length += arr->items[i].length;

        if (i < arr->length - 1) {
            nya_memmove(result->items + result->length, separator, separator_length);
            result->length += separator_length;
        }
    }

    return result;
}

NYA_String* nya_string_join(NYA_Arena* arena, const NYA_ArrayᐸNYA_Stringᐳ* arr, const NYA_String* separator) __attr_overloaded {
    nya_assert(arena != nullptr);
    nya_assert(arr != nullptr);
    nya_assert(separator != nullptr);

    NYA_CString separator_cstr = nya_string_to_cstring(arena, separator);

    return nya_string_join(arena, arr, separator_cstr);
}

NYA_String* nya_string_create(NYA_Arena* arena) {
    nya_assert(arena != nullptr);

    return nya_array_create(arena, u8);
}

NYA_String* nya_string_create_with_capacity(NYA_Arena* arena, u64 capacity) {
    nya_assert(arena != nullptr);

    return nya_array_create_with_capacity(arena, u8, capacity);
}

NYA_String nya_string_create_on_stack(NYA_Arena* arena) {
    nya_assert(arena != nullptr);

    return nya_array_create_on_stack(arena, u8);
}

NYA_String nya_string_create_with_capacity_on_stack(NYA_Arena* arena, u64 capacity) {
    nya_assert(arena != nullptr);

    return nya_array_create_with_capacity_on_stack(arena, u8, capacity);
}

NYA_String* nya_string_sprintf(NYA_Arena* arena, NYA_ConstCString fmt, ...) __attr_fmt_printf(2, 3) {
    nya_assert(arena != nullptr);
    nya_assert(fmt != nullptr);

    va_list args;
    va_start(args, fmt);

    u64 length = vsnprintf(nullptr, 0, fmt, args);
    va_end(args);

    NYA_String* result = nya_string_create_with_capacity(arena, length + 1); // +1 for null terminator

    va_start(args, fmt);
    (void)vsnprintf((NYA_CString)result->items, length + 1, fmt, args);
    va_end(args);

    result->length = length;

    return result;
}

NYA_String* nya_string_substring_excld(NYA_Arena* arena, const NYA_String* str, u64 start, u64 end) {
    nya_assert(arena != nullptr);
    nya_assert(str != nullptr);
    nya_assert(start <= end);
    nya_assert(end <= str->length);

    u64 length = end - start;
    if (length == 0) return nya_string_create(arena);

    NYA_String* substr = nya_string_create_with_capacity(arena, length);
    nya_memmove(substr->items, str->items + start, length);
    substr->length = length;

    return substr;
}

NYA_String* nya_string_substring_incld(NYA_Arena* arena, const NYA_String* str, u64 start, u64 end) {
    nya_assert(arena != nullptr);
    nya_assert(str != nullptr);
    nya_assert(start <= end);
    nya_assert(end < str->length);

    return nya_string_substring_excld(arena, str, start, end + 1);
}

NYA_ArrayᐸNYA_Stringᐳ* nya_string_split(NYA_Arena* arena, const NYA_String* str, NYA_ConstCString separator) __attr_overloaded {
    nya_assert(arena != nullptr);
    nya_assert(str != nullptr);
    nya_assert(separator != nullptr);
    nya_assert(strlen(separator) > 0);

    if (str->length == 0) return nya_array_create(arena, NYA_String);

    NYA_ArrayᐸNYA_Stringᐳ* result     = nya_array_create(arena, NYA_String);
    u64                    sep_length = strlen(separator);
    u64                    start      = 0;
    u64                    end        = 0;

    while (end < str->length) {
        // bounds check: ensure we have enough bytes left to compare with separator
        if (end + sep_length <= str->length && nya_memcmp(str->items + end, separator, sep_length) == 0) {
            NYA_String* substr = nya_string_substring_excld(arena, str, start, end);
            nya_array_push_back(result, *substr);

            start = end + sep_length;
            end   = start;
        } else {
            end++;
        }
    }

    if (start <= str->length) {
        NYA_String* substr = nya_string_substring_excld(arena, str, start, end);
        nya_array_push_back(result, *substr);
    }

    return result;
}

NYA_ArrayᐸNYA_Stringᐳ* nya_string_split(NYA_Arena* arena, const NYA_String* str, const NYA_String* separator) __attr_overloaded {
    nya_assert(arena != nullptr);
    nya_assert(str != nullptr);
    nya_assert(separator != nullptr);

    NYA_CString separator_cstr = nya_alloca(separator->length + 1);
    nya_memmove(separator_cstr, separator->items, separator->length);
    separator_cstr[separator->length] = '\0';

    return nya_string_split(arena, str, separator_cstr);
}

NYA_ArrayᐸNYA_Stringᐳ* nya_string_split_lines(NYA_Arena* arena, const NYA_String* str) {
    nya_assert(arena != nullptr);
    nya_assert(str != nullptr);

    return nya_string_split(arena, str, "\n");
}

NYA_ArrayᐸNYA_Stringᐳ* nya_string_split_words(NYA_Arena* arena, const NYA_String* str) {
    nya_assert(arena != nullptr);
    nya_assert(str != nullptr);

    NYA_ArrayᐸNYA_Stringᐳ* arr    = nya_array_create(arena, NYA_String);
    NYA_String*            buffer = nya_string_create(arena);

    nya_array_foreach (str, ch) {
        if (isspace(*ch)) {
            if (!nya_string_is_empty(buffer)) {
                nya_array_push_back(arr, *nya_string_clone(arena, buffer));
                nya_string_clear(buffer);
            }
            continue;
        }

        nya_array_push_back(buffer, *ch);
    }

    if (!nya_string_is_empty(buffer)) nya_array_push_back(arr, *buffer);

    return arr;
}

u64 nya_string_count(const NYA_String* str, NYA_ConstCString substr) __attr_overloaded {
    nya_assert(str != nullptr);
    nya_assert(substr != nullptr);

    u64 count  = 0;
    u64 length = strlen(substr);

    if (length == 0 || length > str->length) return 0;

    for (u64 i = 0; i < str->length; i++) {
        if (i + length > str->length) break;
        if (nya_memcmp(str->items + i, substr, length) == 0) {
            count++;
            i += length - 1;
        }
    }

    return count;
}

u64 nya_string_count(const NYA_String* str, const NYA_String* substr) __attr_overloaded {
    nya_assert(str != nullptr);
    nya_assert(substr != nullptr);

    NYA_CString substr_cstr = nya_alloca(substr->length + 1);
    nya_memmove(substr_cstr, substr->items, substr->length);
    substr_cstr[substr->length] = '\0';

    return nya_string_count(str, substr_cstr);
}

void nya_string_clear(NYA_String* str) {
    nya_assert(str != nullptr);

    str->length = 0;
}

void nya_string_extend(NYA_String* str, NYA_ConstCString extension) __attr_overloaded {
    nya_assert(str != nullptr);
    nya_assert(extension != nullptr);

    u64 extension_length = strlen(extension);
    u64 new_length       = str->length + extension_length;

    nya_array_reserve(str, new_length);
    nya_memmove(str->items + str->length, extension, extension_length);
    str->length = new_length;
}

void nya_string_extend(NYA_String* str, const NYA_String* extension) __attr_overloaded {
    nya_assert(str != nullptr);
    nya_assert(extension != nullptr);

    u64 new_length = str->length + extension->length;

    nya_array_reserve(str, new_length);
    nya_memmove(str->items + str->length, extension->items, extension->length);
    str->length = new_length;
}

void nya_string_extend_front(NYA_String* str, NYA_ConstCString extension) __attr_overloaded {
    nya_assert(str != nullptr);
    nya_assert(extension != nullptr);

    u64 extension_length = strlen(extension);
    u64 new_length       = str->length + extension_length;

    nya_array_reserve(str, new_length);
    nya_memmove(str->items + extension_length, str->items, str->length);
    nya_memmove(str->items, extension, extension_length);
    str->length = new_length;
}

void nya_string_extend_front(NYA_String* str, const NYA_String* extension) __attr_overloaded {
    nya_assert(str != nullptr);
    nya_assert(extension != nullptr);

    u64 new_length = str->length + extension->length;

    nya_array_reserve(str, new_length);
    nya_memmove(str->items + extension->length, str->items, str->length);
    nya_memmove(str->items, extension->items, extension->length);
    str->length = new_length;
}

void nya_string_extend_front_sprintf(NYA_String* str, NYA_ConstCString fmt, ...) __attr_fmt_printf(2, 3) {
    nya_assert(str != nullptr);
    nya_assert(fmt != nullptr);

    va_list args;
    va_start(args, fmt);
    u64 length = vsnprintf(nullptr, 0, fmt, args);
    va_end(args);

    nya_array_reserve(str, str->length + length);

    // shift existing content to make room for new formatted string at the front
    nya_memmove(str->items + length, str->items, str->length);

    va_start(args, fmt);
    (void)vsnprintf((NYA_CString)str->items, length + 1, fmt, args);
    va_end(args);

    str->length += length;
}

void nya_string_extend_sprintf(NYA_String* str, NYA_ConstCString fmt, ...) __attr_fmt_printf(2, 3) {
    nya_assert(str != nullptr);
    nya_assert(fmt != nullptr);

    va_list args;
    va_start(args, fmt);
    u64 length = vsnprintf(nullptr, 0, fmt, args);
    va_end(args);

    nya_array_reserve(str, str->length + length + 1);

    va_start(args, fmt);
    (void)vsnprintf((NYA_CString)(str->items + str->length), length + 1, fmt, args);
    va_end(args);

    str->length += length;
}

void nya_string_destroy(NYA_String* str) {
    nya_array_destroy(str);
}

void nya_string_destroy_on_stack(NYA_String* str) {
    nya_array_destroy_on_stack(str);
}

void nya_string_push_back(NYA_String* str, u8 character) {
    nya_assert(str != nullptr);
    nya_array_push_back(str, character);
}

void nya_string_print(const NYA_String* str) {
    nya_assert(str != nullptr);

    printf(NYA_FMT_STRING, NYA_FMT_STRING_ARG(str));
}

void nya_string_println(const NYA_String* str) {
    nya_assert(str != nullptr);

    printf(NYA_FMT_STRING "\n", NYA_FMT_STRING_ARG(str));
}

void nya_string_remove(NYA_String* str, NYA_ConstCString substr) __attr_overloaded {
    nya_assert(str != nullptr);
    nya_assert(substr != nullptr);

    u64 length = strlen(substr);
    if (length == 0 || length > str->length) return;

    for (u64 i = 0; i + length <= str->length;) {
        if (nya_memcmp(str->items + i, substr, length) == 0) {
            nya_memmove(str->items + i, str->items + i + length, str->length - i - length);
            str->length -= length;
            // don't increment i - check same position again for consecutive matches
        } else {
            i++;
        }
    }
}

void nya_string_remove(NYA_String* str, const NYA_String* substr) __attr_overloaded {
    nya_assert(str != nullptr);
    nya_assert(substr != nullptr);

    if (substr->length == 0 || substr->length > str->length) return;

    for (u64 i = 0; i + substr->length <= str->length;) {
        if (nya_memcmp(str->items + i, substr->items, substr->length) == 0) {
            nya_memmove(str->items + i, str->items + i + substr->length, str->length - i - substr->length);
            str->length -= substr->length;
            // don't increment i - check same position again for consecutive matches
        } else {
            i++;
        }
    }
}

void nya_string_replace(NYA_String* str, NYA_ConstCString old, NYA_ConstCString new) __attr_overloaded {
    nya_assert(str != nullptr);
    nya_assert(old != nullptr);
    nya_assert(new != nullptr);

    u64 old_length = strlen(old);
    u64 new_length = strlen(new);

    if (old_length == 0 || old_length > str->length) return;

    for (u64 i = 0; i + old_length <= str->length;) {
        if (nya_memcmp(str->items + i, old, old_length) == 0) {
            if (new_length > old_length) { nya_array_reserve(str, str->length + (new_length - old_length)); }

            nya_memmove(str->items + i + new_length, str->items + i + old_length, str->length - i - old_length);
            nya_memmove(str->items + i, new, new_length);

            // use signed arithmetic for length change
            str->length  = str->length - old_length + new_length;
            i           += new_length;

            // handle case where new is empty (effectively a remove)
            if (new_length == 0) continue;
        } else {
            i++;
        }
    }
}

void nya_string_replace(NYA_String* str, const NYA_String* old, const NYA_String* new) __attr_overloaded {
    nya_assert(str != nullptr);
    nya_assert(old != nullptr);
    nya_assert(new != nullptr);

    if (old->length == 0 || old->length > str->length) return;

    for (u64 i = 0; i + old->length <= str->length;) {
        if (nya_memcmp(str->items + i, old->items, old->length) == 0) {
            if (new->length > old->length) { nya_array_reserve(str, str->length + (new->length - old->length)); }

            nya_memmove(str->items + i + new->length, str->items + i + old->length, str->length - i - old->length);
            nya_memmove(str->items + i, new->items, new->length);

            // use signed arithmetic for length change
            str->length  = str->length - old->length + new->length;
            i           += new->length;

            // handle case where new is empty (effectively a remove)
            if (new->length == 0) continue;
        } else {
            i++;
        }
    }
}

void nya_string_reserve(NYA_String* str, u64 capacity) {
    nya_assert(str != nullptr);

    nya_array_reserve(str, capacity);
}

void nya_string_reverse(NYA_String* str) {
    nya_assert(str != nullptr);

    nya_array_reverse(str);
}

void nya_string_shrink_to_fit(NYA_String* str) {
    nya_assert(str != nullptr);

    nya_array_shrink_to_fit(str);
}

s32 nya_string_sscanf(NYA_String* str, NYA_ConstCString fmt, ...) __attr_fmt_scanf(2, 3) {
    nya_assert(str != nullptr);
    nya_assert(fmt != nullptr);

    va_list args;
    va_start(args, fmt);

    s32 ret = vsscanf((NYA_ConstCString)str->items, fmt, args);

    va_end(args);
    return ret;
}

void nya_string_strip_prefix(NYA_String* str, NYA_ConstCString prefix) {
    nya_assert(str != nullptr);
    nya_assert(prefix != nullptr);

    u64 prefix_length = strlen(prefix);

    if (nya_memcmp(str->items, prefix, prefix_length) == 0) {
        nya_memmove(str->items, str->items + prefix_length, str->length - prefix_length);
        str->length -= prefix_length;
    }
}

void nya_string_strip_suffix(NYA_String* str, NYA_ConstCString suffix) {
    nya_assert(str != nullptr);
    nya_assert(suffix != nullptr);

    u64 suffix_length = strlen(suffix);
    if (suffix_length > str->length) return;

    if (nya_memcmp(str->items + str->length - suffix_length, suffix, suffix_length) == 0) { str->length -= suffix_length; }
}

NYA_CString nya_string_to_cstring(NYA_Arena* arena, const NYA_String* str) {
    nya_assert(str != nullptr);

    NYA_CString cstr = nya_arena_alloc(arena, str->length + 1);
    if (str->length > 0) { nya_memmove(cstr, str->items, str->length); }
    cstr[str->length] = '\0';

    return cstr;
}

void nya_string_to_lower(NYA_String* str) {
    nya_assert(str != nullptr);

    for (u64 i = 0; i < str->length; i++) str->items[i] = tolower(str->items[i]);
}

void nya_string_to_upper(NYA_String* str) {
    nya_assert(str != nullptr);

    for (u64 i = 0; i < str->length; i++) str->items[i] = toupper(str->items[i]);
}

void nya_string_trim_whitespace(NYA_String* str) {
    nya_assert(str != nullptr);

    u64 start = 0;
    u64 end   = str->length;

    while (start < str->length && isspace(str->items[start])) start++;
    while (end > start && isspace(str->items[end - 1])) end--;

    nya_memmove(str->items, str->items + start, end - start);
    str->length = end - start;
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

static NYA_CString _nya_strstrn(NYA_ConstCString haystack, NYA_ConstCString needle, u64 haystack_len, u64 needle_len) {
    if (needle_len == 0 || needle_len > haystack_len) return nullptr;

    for (u64 i = 0; i <= haystack_len - needle_len; i++) {
        if (nya_memcmp(haystack + i, needle, needle_len) == 0) return (NYA_CString)(haystack + i);
    }

    return nullptr;
}
