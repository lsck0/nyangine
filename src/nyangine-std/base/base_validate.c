#include "nyangine-core/nyangine.h"

// PRIVATE API DECLARATION

/** Deepest struct nesting the walk descends; a described tree past this is a broken table and is asserted. */
#define _NYA_VALIDATE_DEPTH_MAX 32

/** Validates one struct or union, field by field, recursing into nested ones. `value` is its address. */
NYA_INTERNAL NYA_Error _nya_validate_type(const NYA_TypeReflection* type, const void* value, u32 depth) __attr_no_discard;

/** Applies every rule one field carries, at `address` within its containing value. */
NYA_INTERNAL NYA_Error _nya_validate_field(const NYA_ReflectField* field, const void* address) __attr_no_discard;

/** The field's value as a string view when it is a `char[N]` or a `char*`; false when it is neither. */
NYA_INTERNAL b8 _nya_validate_string(const NYA_TypeReflection* type, const void* address, OUT NYA_ConstCString* out_text, OUT u32* out_length)
    __attr_no_discard;

/** Whether the field reads as empty: an empty string, a null pointer, or all-zero bytes across its storage. */
NYA_INTERNAL b8 _nya_validate_is_empty(const NYA_TypeReflection* type, const void* address) __attr_no_discard;

/** The field's value as a real when it is a number (a primitive or an enum); false for anything else. */
NYA_INTERNAL b8 _nya_validate_number(const NYA_TypeReflection* type, const void* address, OUT f64* out_number) __attr_no_discard;

/** Parses the number in `args[from..to)`, ignoring surrounding spaces, through the engine's own parser. */
NYA_INTERNAL b8 _nya_validate_parse(NYA_ConstCString args, u64 from, u64 to, NYA_Type target, OUT void* out_value) __attr_no_discard;

/** Splits `min,max` out of `args` and parses each as an integer; false when it is not two numbers. */
NYA_INTERNAL b8 _nya_validate_parse_range(NYA_ConstCString args, OUT s64* out_min, OUT s64* out_max) __attr_no_discard;

/** A bounded glob: `*` matches any run, `?` any one byte, everything else is literal. No allocation. */
NYA_INTERNAL b8 _nya_validate_glob(NYA_ConstCString pattern, u32 pattern_length, NYA_ConstCString text, u32 text_length) __attr_no_discard;

// PUBLIC API IMPLEMENTATION

NYA_Error nya_validate(const NYA_TypeReflection* type, const void* value) {
    if (type == nullptr || value == nullptr) return nya_error(NYA_ERROR_INVALID_ARGUMENT, "no type or no value to validate");

    return _nya_validate_type(type, value, 0);
}

// PRIVATE API IMPLEMENTATION

NYA_Error _nya_validate_type(const NYA_TypeReflection* type, const void* value, u32 depth) {
    nya_assert(depth < _NYA_VALIDATE_DEPTH_MAX, "'%s' nests deeper than _NYA_VALIDATE_DEPTH_MAX", type->name);

    // Only a struct or union has fields, and only a field carries a rule; anything else is trivially valid.
    if (type->kind != NYA_REFLECT_STRUCT && type->kind != NYA_REFLECT_UNION) return NYA_OK;

    for (u32 i = 0; i < type->field_count; i++) {
        const NYA_ReflectField*   field      = &type->fields[i];
        const NYA_TypeReflection* field_type = field->type;

        if (field_type == nullptr) continue;

        const void* address = (const u8*)value + field->offset;

        NYA_TRY(_nya_validate_field(field, address));

        // A nested struct or union is validated to its leaves, so a DTO with an object inside it is checked whole.
        if (field_type->kind == NYA_REFLECT_STRUCT || field_type->kind == NYA_REFLECT_UNION) {
            NYA_TRY(_nya_validate_type(field_type, address, depth + 1));
        }
    }

    return NYA_OK;
}

NYA_Error _nya_validate_field(const NYA_ReflectField* field, const void* address) {
    const NYA_TypeReflection* type = field->type;

    for (u32 i = 0; i < field->attribute_count; i++) {
        const NYA_ReflectAttribute* attribute = &field->attributes[i];
        NYA_ConstCString            args      = attribute->args;

        if (nya_string_equals(attribute->name, "required")) {
            if (_nya_validate_is_empty(type, address)) return nya_error(NYA_ERROR_INVALID_ARGUMENT, "field '%s' is required", field->name);
            continue;
        }

        if (nya_string_equals(attribute->name, "min")) {
            f64 number = 0.0;
            f64 bound  = 0.0;

            // A malformed argument or a field that is not a number: the rule does not apply, so it is skipped rather than failed.
            if (args == nullptr || !_nya_validate_parse(args, 0, strlen(args), NYA_TYPE_F64, &bound)) continue;
            if (!_nya_validate_number(type, address, &number)) continue;

            if (number < bound) return nya_error(NYA_ERROR_INVALID_ARGUMENT, "field '%s' is below the minimum of %s", field->name, args);
            continue;
        }

        if (nya_string_equals(attribute->name, "max")) {
            f64 number = 0.0;
            f64 bound  = 0.0;

            if (args == nullptr || !_nya_validate_parse(args, 0, strlen(args), NYA_TYPE_F64, &bound)) continue;
            if (!_nya_validate_number(type, address, &number)) continue;

            if (number > bound) return nya_error(NYA_ERROR_INVALID_ARGUMENT, "field '%s' is above the maximum of %s", field->name, args);
            continue;
        }

        if (nya_string_equals(attribute->name, "len")) {
            s64 min = 0;
            s64 max = 0;

            NYA_ConstCString text   = nullptr;
            u32              length = 0;

            if (!_nya_validate_parse_range(args, &min, &max)) continue;
            if (!_nya_validate_string(type, address, &text, &length)) continue;

            if ((s64)length < min || (s64)length > max)
                return nya_error(NYA_ERROR_INVALID_ARGUMENT, "field '%s' has length " FMTu32 ", outside [%s]", field->name, length, args);
            continue;
        }

        if (nya_string_equals(attribute->name, "email")) {
            NYA_ConstCString text   = nullptr;
            u32              length = 0;

            if (!_nya_validate_string(type, address, &text, &length)) continue;
            if (text == nullptr || !nya_email_is_valid((const u8*)text, length))
                return nya_error(NYA_ERROR_INVALID_ARGUMENT, "field '%s' is not a valid email address", field->name);
            continue;
        }

        if (nya_string_equals(attribute->name, "pattern")) {
            NYA_ConstCString text   = nullptr;
            u32              length = 0;

            if (args == nullptr || !_nya_validate_string(type, address, &text, &length)) continue;
            if (text == nullptr || !_nya_validate_glob(args, (u32)strlen(args), text, length))
                return nya_error(NYA_ERROR_INVALID_ARGUMENT, "field '%s' does not match the pattern '%s'", field->name, args);
            continue;
        }

        // Any other annotation belongs to another consumer and is not validation's to judge.
    }

    return NYA_OK;
}

b8 _nya_validate_string(const NYA_TypeReflection* type, const void* address, OUT NYA_ConstCString* out_text, OUT u32* out_length) {
    if (nya_reflect_is_char_array(type)) {
        const char* text = (const char*)address;

        u32 length = 0;
        while (length < type->element_count && text[length] != '\0') length++;

        *out_text   = text;
        *out_length = length;
        return true;
    }

    if (type->kind == NYA_REFLECT_PRIMITIVE && type->primitive == NYA_TYPE_STRING) {
        const char* text = *(char* const*)address;

        *out_text   = text;
        *out_length = text != nullptr ? (u32)strlen(text) : 0;
        return true;
    }

    return false;
}

b8 _nya_validate_is_empty(const NYA_TypeReflection* type, const void* address) {
    NYA_ConstCString text   = nullptr;
    u32              length = 0;
    if (_nya_validate_string(type, address, &text, &length)) return length == 0;

    // Everything else is empty when its bytes are all zero: a zero number, a null pointer and an all-zero struct alike.
    const u8* bytes = (const u8*)address;
    for (u64 i = 0; i < type->size; i++) {
        if (bytes[i] != 0) return false;
    }

    return true;
}

b8 _nya_validate_number(const NYA_TypeReflection* type, const void* address, OUT f64* out_number) {
    if (type->kind != NYA_REFLECT_PRIMITIVE && type->kind != NYA_REFLECT_ENUM) return false;

    NYA_Value value = nya_reflect_read(type, address);
    return nya_reflect_value_to_f64(value, out_number);
}

b8 _nya_validate_parse(NYA_ConstCString args, u64 from, u64 to, NYA_Type target, OUT void* out_value) {
    while (from < to && (args[from] == ' ' || args[from] == '\t')) from++;
    while (to > from && (args[to - 1] == ' ' || args[to - 1] == '\t')) to--;

    if (from >= to) return false;

    return nya_type_parse(target, (const u8*)args + from, to - from, out_value);
}

b8 _nya_validate_parse_range(NYA_ConstCString args, OUT s64* out_min, OUT s64* out_max) {
    if (args == nullptr) return false;

    u64 length = strlen(args);

    u64 comma = 0;
    while (comma < length && args[comma] != ',') comma++;
    if (comma >= length) return false; // @len needs two arguments; one is not a range.

    if (!_nya_validate_parse(args, 0, comma, NYA_TYPE_S64, out_min)) return false;
    if (!_nya_validate_parse(args, comma + 1, length, NYA_TYPE_S64, out_max)) return false;

    return true;
}

b8 _nya_validate_glob(NYA_ConstCString pattern, u32 pattern_length, NYA_ConstCString text, u32 text_length) {
    u32 p     = 0;
    u32 t     = 0;
    u32 star  = pattern_length; // where the last '*' sat, or the sentinel for none seen yet.
    u32 match = 0;              // how much of the text that '*' has consumed so far.

    while (t < text_length) {
        if (p < pattern_length && (pattern[p] == '?' || pattern[p] == text[t])) {
            p++;
            t++;
        } else if (p < pattern_length && pattern[p] == '*') {
            star  = p;
            match = t;
            p++;
        } else if (star != pattern_length) {
            // Backtrack: let the last '*' swallow one more byte and try the rest of the pattern again.
            p = star + 1;
            match++;
            t = match;
        } else {
            return false;
        }
    }

    // A run of trailing stars matches the empty tail.
    while (p < pattern_length && pattern[p] == '*') p++;

    return p == pattern_length;
}
