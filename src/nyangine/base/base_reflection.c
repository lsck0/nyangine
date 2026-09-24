#include "nyangine/nyangine.h"

// PRIVATE API DECLARATION

/**
 * Reads any integer-shaped primitive as a signed 64 bit value.
 * */
NYA_INTERNAL b8 _nya_reflect_read_integer(NYA_Type primitive, const void* instance, OUT s64* out_value);
NYA_INTERNAL b8 _nya_reflect_write_integer(NYA_Type primitive, void* instance, s64 value);

/**
 * A real truncated to an s64, or false when it has no s64: NaN, an infinity, or anything past the range.
 * The cast alone is undefined there, and a document ({"count": 1e300}, or a binary NaN) chooses it.
 * */
NYA_INTERNAL b8 _nya_reflect_real_to_s64(f64 real, OUT s64* out_value);

/** One element of an array or vector, as a value. Shared by both, which differ only in their stride. */
NYA_INTERNAL NYA_Value _nya_reflect_element_to_value(NYA_Arena* arena, const NYA_TypeReflection* element, const void* address, b8 redact);

/** The one walk behind nya_reflect_to_object and its redacting twin; `redact` is the only difference. */
NYA_INTERNAL NYA_Object* _nya_reflect_to_object(NYA_Arena* arena, const NYA_TypeReflection* type, const void* instance, b8 redact)
    __attr_no_discard;

// The check. See nya_reflect_check.

/** Longest rendering of one value a report carries: a type name and a little of its contents. */
#define _NYA_REFLECT_FOUND_MAX 96

/** Longest rendering of what a field wanted. Holds a handful of enum variant names before it elides. */
#define _NYA_REFLECT_EXPECTED_MAX 192

/** What a list that did not fit ends with, so nobody reads a truncated one as the whole set. */
#define _NYA_REFLECT_ELISION ", ..."

/** Appends `.name` (or `name` at the root) to `path`, and answers the new length. Truncates rather than overflowing. */
NYA_INTERNAL u64 _nya_reflect_path_push(OUT char* path, u64 length, NYA_ConstCString name, char separator);

/** What the document holds, in a few words: `string "fast"`, `f32 0.5`, `an object`. */
NYA_INTERNAL void _nya_reflect_describe_value(const NYA_Value* value, OUT char* out, u64 capacity);

/** What the described type wanted there: a primitive's name, an enum's variants, `an object`, `a list of 3`. */
NYA_INTERNAL void _nya_reflect_describe_expected(const NYA_TypeReflection* type, OUT char* out, u64 capacity);

/**
 * `prefix` followed by the names in `names`, comma separated: "one of msaa_samples, fxaa, bloom".
 *
 * `names` is an array of structs whose first member is the name and `stride` is that struct's size,
 * which both NYA_ReflectField and NYA_ReflectVariant satisfy. Passing the array rather than the type
 * is what lets an enum's variants and a struct's fields be listed by the same three lines.
 * */
NYA_INTERNAL void _nya_reflect_describe_names(OUT char* out, u64 capacity, NYA_ConstCString prefix, const void* names, u64 stride, u32 count);

/** One value against one described type, recursing into objects and lists. Returns the problems found. */
NYA_INTERNAL u32 _nya_reflect_check_value(
    const NYA_TypeReflection* type,
    const NYA_Value*          value,
    NYA_ReflectReportFn       report,
    void*                     user_data,
    OUT char*                 path,
    u64                       length
);

/** The struct or union case of the above, walking the document's keys rather than the type's fields. */
NYA_INTERNAL u32 _nya_reflect_check_object(
    const NYA_TypeReflection* type,
    const NYA_Object*         object,
    NYA_ReflectReportFn       report,
    void*                     user_data,
    OUT char*                 path,
    u64                       length
);

// The layout hash. See nya_reflect_layout_hash.

/** Stable names for the kinds, hashed instead of their enum values for the reason NYA_TYPE_NAME_MAP is. */
NYA_INTERNAL const NYA_ConstCString _NYA_REFLECT_KIND_NAME_MAP[NYA_REFLECT_COUNT] = {
    [NYA_REFLECT_PRIMITIVE] = "primitive", [NYA_REFLECT_STRUCT] = "struct", [NYA_REFLECT_UNION] = "union",     [NYA_REFLECT_ENUM] = "enum",
    [NYA_REFLECT_ARRAY] = "array",         [NYA_REFLECT_VECTOR] = "vector", [NYA_REFLECT_POINTER] = "pointer",
};

NYA_INTERNAL u64 _nya_reflect_layout_feed_u64(u64 hash, u64 value);
NYA_INTERNAL u64 _nya_reflect_layout_feed_text(u64 hash, NYA_ConstCString text);
NYA_INTERNAL u64 _nya_reflect_layout_feed_type(u64 hash, const NYA_TypeReflection* type, u32 depth);

// PUBLIC API IMPLEMENTATION

const NYA_ReflectField* nya_reflect_field(const NYA_TypeReflection* type, NYA_ConstCString name) {
    if (type == nullptr || name == nullptr) return nullptr;

    for (u32 i = 0; i < type->field_count; i++) {
        if (nya_string_equals(type->fields[i].name, name)) return &type->fields[i];
    }

    return nullptr;
}

void* nya_reflect_field_pointer(void* instance, const NYA_ReflectField* field) {
    if (instance == nullptr || field == nullptr) return nullptr;

    return (u8*)instance + field->offset;
}

const NYA_ReflectField* nya_reflect_path(const NYA_TypeReflection* type, NYA_ConstCString path, void* instance, OUT void** out_instance) {
    if (type == nullptr || path == nullptr) return nullptr;

    const NYA_ReflectField* found   = nullptr;
    const NYA_TypeReflection* walk  = type;
    void*                     cursor = instance;

    for (NYA_ConstCString segment = path; *segment != '\0';) {
        // The segment is compared in place rather than copied out.
        u64 length = 0;
        while (segment[length] != '\0' && segment[length] != '.') length++;

        found = nullptr;

        for (u32 i = 0; i < walk->field_count; i++) {
            const NYA_ReflectField* candidate = &walk->fields[i];

            if (strlen(candidate->name) != length) continue;
            if (nya_memcmp(candidate->name, segment, length) != 0) continue;

            found = candidate;
            break;
        }

        if (found == nullptr) return nullptr;

        if (cursor != nullptr) cursor = (u8*)cursor + found->offset;

        segment += length;
        if (*segment == '.') segment++;

        // Only descend when there is more path left: the last segment names the answer, and its type may be a primitive with no fields to walk into.
        if (*segment != '\0') {
            walk = found->type;

            if (walk == nullptr) return nullptr;
            if (walk->kind != NYA_REFLECT_STRUCT && walk->kind != NYA_REFLECT_UNION) return nullptr;
        }
    }

    if (out_instance != nullptr) *out_instance = cursor;

    return found;
}

NYA_ConstCString nya_reflect_variant_name(const NYA_TypeReflection* type, s64 value) {
    if (type == nullptr || type->kind != NYA_REFLECT_ENUM) return nullptr;

    for (u32 i = 0; i < type->variant_count; i++) {
        if (type->variants[i].value == value) return type->variants[i].name;
    }

    return nullptr;
}

b8 nya_reflect_variant_value(const NYA_TypeReflection* type, NYA_ConstCString name, OUT s64* out_value) {
    if (type == nullptr || type->kind != NYA_REFLECT_ENUM || name == nullptr || out_value == nullptr) return false;

    for (u32 i = 0; i < type->variant_count; i++) {
        if (!nya_string_equals(type->variants[i].name, name)) continue;

        *out_value = type->variants[i].value;
        return true;
    }

    return false;
}

b8 nya_reflect_is_char_array(const NYA_TypeReflection* type) {
    if (type == nullptr) return false;

    return type->kind == NYA_REFLECT_ARRAY && type->element != nullptr && type->element->kind == NYA_REFLECT_PRIMITIVE &&
           type->element->primitive == NYA_TYPE_CHAR;
}

u64 nya_reflect_layout_hash(const NYA_TypeReflection* type) {
    nya_assert(type != nullptr);

    return _nya_reflect_layout_feed_type(NYA_HASH_FNV1A_OFFSET_BASIS, type, 0);
}

b8 nya_reflect_value_to_s64(NYA_Value value, OUT s64* out_value) {
    switch (value.type) {
        case NYA_TYPE_B8:  *out_value = value.as_b8; return true;
        case NYA_TYPE_B16: *out_value = value.as_b16; return true;
        case NYA_TYPE_B32: *out_value = value.as_b32; return true;
        case NYA_TYPE_B64: *out_value = (s64)value.as_b64; return true;

        case NYA_TYPE_U8:  *out_value = value.as_u8; return true;
        case NYA_TYPE_U16: *out_value = value.as_u16; return true;
        case NYA_TYPE_U32: *out_value = value.as_u32; return true;
        case NYA_TYPE_U64: *out_value = (s64)value.as_u64; return true;

        case NYA_TYPE_S8:  *out_value = (s64)value.as_s8; return true; // NOLINT(bugprone-signed-char-misuse): sign extension is the point
        case NYA_TYPE_S16: *out_value = value.as_s16; return true;
        case NYA_TYPE_S32: *out_value = value.as_s32; return true;
        case NYA_TYPE_S64: *out_value = value.as_s64; return true;

        case NYA_TYPE_CHAR: *out_value = (u8)value.as_char; return true;

        // A whole number written with a decimal point is still a whole number; truncation is deliberate, not an error, so "count": 3.0 loads.
        case NYA_TYPE_F32: return _nya_reflect_real_to_s64((f64)value.as_f32, out_value);
        case NYA_TYPE_F64: return _nya_reflect_real_to_s64(value.as_f64, out_value);

        default: return false;
    }
}

b8 nya_reflect_value_to_f64(NYA_Value value, OUT f64* out_value) {
    if (value.type == NYA_TYPE_F32) {
        *out_value = (f64)value.as_f32;
        return true;
    }

    if (value.type == NYA_TYPE_F64) {
        *out_value = value.as_f64;
        return true;
    }

    // Integers widen into a float without complaint, the case that matters: a hand-written 1 has to load into an f32 field.
    s64 integer = 0;
    if (!nya_reflect_value_to_s64(value, &integer)) return false;

    *out_value = (f64)integer;
    return true;
}

NYA_Value nya_reflect_read(const NYA_TypeReflection* type, const void* instance) {
    NYA_Value none = { .type = NYA_TYPE_NULL };

    if (type == nullptr || instance == nullptr) return none;

    // An enum reads as its underlying integer; turning it into a name is nya_reflect_to_object's business, a serialisation choice and not what the field holds.
    if (type->kind == NYA_REFLECT_ENUM) {
        s64 value = 0;
        if (!_nya_reflect_read_integer(type->primitive, instance, &value)) return none;

        return (NYA_Value){ .type = NYA_TYPE_S64, .as_s64 = value };
    }

    if (type->kind != NYA_REFLECT_PRIMITIVE) return none;

    switch (type->primitive) {
        case NYA_TYPE_B8:   return (NYA_Value){ .type = NYA_TYPE_B8, .as_b8 = *(const b8*)instance };
        case NYA_TYPE_B16:  return (NYA_Value){ .type = NYA_TYPE_B16, .as_b16 = *(const b16*)instance };
        case NYA_TYPE_B32:  return (NYA_Value){ .type = NYA_TYPE_B32, .as_b32 = *(const b32*)instance };
        case NYA_TYPE_B64:  return (NYA_Value){ .type = NYA_TYPE_B64, .as_b64 = *(const b64*)instance };

        case NYA_TYPE_U8:   return (NYA_Value){ .type = NYA_TYPE_U8, .as_u8 = *(const u8*)instance };
        case NYA_TYPE_U16:  return (NYA_Value){ .type = NYA_TYPE_U16, .as_u16 = *(const u16*)instance };
        case NYA_TYPE_U32:  return (NYA_Value){ .type = NYA_TYPE_U32, .as_u32 = *(const u32*)instance };
        case NYA_TYPE_U64:  return (NYA_Value){ .type = NYA_TYPE_U64, .as_u64 = *(const u64*)instance };

        case NYA_TYPE_S8:   return (NYA_Value){ .type = NYA_TYPE_S8, .as_s8 = *(const s8*)instance };
        case NYA_TYPE_S16:  return (NYA_Value){ .type = NYA_TYPE_S16, .as_s16 = *(const s16*)instance };
        case NYA_TYPE_S32:  return (NYA_Value){ .type = NYA_TYPE_S32, .as_s32 = *(const s32*)instance };
        case NYA_TYPE_S64:  return (NYA_Value){ .type = NYA_TYPE_S64, .as_s64 = *(const s64*)instance };

        case NYA_TYPE_F32:  return (NYA_Value){ .type = NYA_TYPE_F32, .as_f32 = *(const f32*)instance };
        case NYA_TYPE_F64:  return (NYA_Value){ .type = NYA_TYPE_F64, .as_f64 = *(const f64*)instance };

        case NYA_TYPE_CHAR: return (NYA_Value){ .type = NYA_TYPE_CHAR, .as_char = *(const char*)instance };

        // A string field is a pointer the struct does not own, and it is copied as that pointer.
        case NYA_TYPE_STRING: return (NYA_Value){ .type = NYA_TYPE_STRING, .as_string = *(char* const*)instance };

        default: return none;
    }
}

b8 nya_reflect_write(const NYA_TypeReflection* type, void* instance, NYA_Value value) {
    if (type == nullptr || instance == nullptr) return false;

    if (type->kind == NYA_REFLECT_ENUM) {
        s64 integer = 0;
        if (!nya_reflect_value_to_s64(value, &integer)) return false;

        return _nya_reflect_write_integer(type->primitive, instance, integer);
    }

    if (type->kind != NYA_REFLECT_PRIMITIVE) return false;

    // Coerced rather than matched exactly.
    switch (type->primitive) {
        case NYA_TYPE_STRING: {
            if (value.type != NYA_TYPE_STRING) return false;

            *(char**)instance = value.as_string;
            return true;
        }

        case NYA_TYPE_CHAR: {
            if (value.type == NYA_TYPE_CHAR) {
                *(char*)instance = value.as_char;
                return true;
            }

            s64 integer = 0;
            if (!nya_reflect_value_to_s64(value, &integer)) return false;

            *(char*)instance = (char)integer;
            return true;
        }

        case NYA_TYPE_F32: {
            f64 number = 0.0;
            if (!nya_reflect_value_to_f64(value, &number)) return false;

            *(f32*)instance = (f32)number;
            return true;
        }

        case NYA_TYPE_F64: {
            f64 number = 0.0;
            if (!nya_reflect_value_to_f64(value, &number)) return false;

            *(f64*)instance = number;
            return true;
        }

        default: {
            s64 integer = 0;
            if (!nya_reflect_value_to_s64(value, &integer)) return false;

            return _nya_reflect_write_integer(type->primitive, instance, integer);
        }
    }
}

// THE GENERIC CONVERSION

NYA_Object* nya_reflect_to_object(NYA_Arena* arena, const NYA_TypeReflection* type, const void* instance) {
    return _nya_reflect_to_object(arena, type, instance, false);
}

NYA_Object* nya_reflect_to_object_redacted(NYA_Arena* arena, const NYA_TypeReflection* type, const void* instance) {
    return _nya_reflect_to_object(arena, type, instance, true);
}

NYA_Object* _nya_reflect_to_object(NYA_Arena* arena, const NYA_TypeReflection* type, const void* instance, b8 redact) {
    nya_assert(arena != nullptr);

    if (type == nullptr || instance == nullptr) return nullptr;
    if (type->kind != NYA_REFLECT_STRUCT && type->kind != NYA_REFLECT_UNION) return nullptr;

    NYA_Object* object = nya_object_create(arena);

    // A union is written as the one member its tag selects; without a tag there is no way to know which is live, so nothing is written rather than something arbitrary. See the header.
    if (type->kind == NYA_REFLECT_UNION && type->tag_field == nullptr) return object;

    for (u32 i = 0; i < type->field_count; i++) {
        const NYA_ReflectField*   field       = &type->fields[i];
        const NYA_TypeReflection* field_type  = field->type;
        const void*               address     = (const u8*)instance + field->offset;

        if (field_type == nullptr) continue;

        /* Before the switch and before the field is read, so what happens next does not depend on the field's kind: a tagged string, struct or array all come out as the same four words. */
        // `@secret` is masked here too, not only `@redact`: a value encrypted at rest still has no place in a log, and this redacting walk is what every logging path goes through.
        if (redact && (field->is_redacted || field->is_secret)) {
            nya_object_add(object, (NYA_CString)field->name, (NYA_Value){ .type = NYA_TYPE_STRING, .as_string = NYA_REFLECT_REDACTED });
            continue;
        }

        switch (field_type->kind) {
            case NYA_REFLECT_PRIMITIVE: {
                NYA_Value value = nya_reflect_read(field_type, address);
                if (value.type == NYA_TYPE_NULL) continue;

                nya_object_add(object, (NYA_CString)field->name, value);
                break;
            }

            // An enum is written as its variant *name*.
            case NYA_REFLECT_ENUM: {
                s64 raw = 0;
                if (!_nya_reflect_read_integer(field_type->primitive, address, &raw)) continue;

                if (field_type->is_bitflags) {
                    // A set of flags is a list of names, so adding or removing a flag changes only which names appear, not the meaning of a number.
                    NYA_ArrayᐸNYA_Valueᐳ* names = nya_array_create(arena, NYA_Value);

                    for (u32 v = 0; v < field_type->variant_count; v++) {
                        const NYA_ReflectVariant* variant = &field_type->variants[v];

                        if (variant->value == 0) continue;
                        if ((raw & variant->value) != variant->value) continue;

                        nya_array_push_back(names, ((NYA_Value){ .type = NYA_TYPE_STRING, .as_string = (NYA_CString)variant->name }));
                    }

                    nya_object_add(object, (NYA_CString)field->name, (NYA_Value){ .type = NYA_TYPE_ARRAY, .as_array = *names });
                    break;
                }

                NYA_ConstCString name = nya_reflect_variant_name(field_type, raw);

                // A value with no name is written as the number, since losing it entirely is worse than writing something a newer build can still read.
                if (name == nullptr) {
                    nya_object_add(object, (NYA_CString)field->name, (NYA_Value){ .type = NYA_TYPE_S64, .as_s64 = raw });
                    break;
                }

                nya_object_add(object, (NYA_CString)field->name, (NYA_Value){ .type = NYA_TYPE_STRING, .as_string = (NYA_CString)name });
                break;
            }

            case NYA_REFLECT_STRUCT:
            case NYA_REFLECT_UNION: {
                NYA_Object* nested = _nya_reflect_to_object(arena, field_type, address, redact);
                if (nested == nullptr) continue;

                nya_object_add(object, (NYA_CString)field->name, (NYA_Value){ .type = NYA_TYPE_OBJECT, .as_object = *nested });
                break;
            }

            case NYA_REFLECT_ARRAY:
            case NYA_REFLECT_VECTOR: {
                // A char array is text, not a list of numbers: `char name[32]` written as thirty-two integers is technically complete and useless to read.
                if (nya_reflect_is_char_array(field_type)) {
                    const char* text = (const char*)address;

                    u64 length = 0;
                    while (length < field_type->element_count && text[length] != '\0') length++;

                    char* copy = nya_arena_alloc(arena, length + 1);
                    nya_memcpy(copy, text, length);
                    copy[length] = '\0';

                    nya_object_add(object, (NYA_CString)field->name, (NYA_Value){ .type = NYA_TYPE_STRING, .as_string = copy });
                    break;
                }

                if (field_type->element == nullptr) continue;

                NYA_ArrayᐸNYA_Valueᐳ* elements = nya_array_create(arena, NYA_Value);

                for (u32 e = 0; e < field_type->element_count; e++) {
                    const void* element_address = (const u8*)address + ((u64)e * field_type->element->size);

                    nya_array_push_back(elements, _nya_reflect_element_to_value(arena, field_type->element, element_address, redact));
                }

                nya_object_add(object, (NYA_CString)field->name, (NYA_Value){ .type = NYA_TYPE_ARRAY, .as_array = *elements });
                break;
            }

            // Pointers are not followed.
            case NYA_REFLECT_POINTER: break;

            case NYA_REFLECT_COUNT:
            default: nya_unreachable();
        }
    }

    return object;
}

NYA_Error nya_reflect_from_object(const NYA_TypeReflection* type, void* instance, const NYA_Object* object) {
    if (type == nullptr || instance == nullptr) return nya_error(NYA_ERROR_INVALID_ARGUMENT, "no type or no instance");
    if (object == nullptr) return nya_error(NYA_ERROR_INVALID_ARGUMENT, "no object to read");
    if (type->kind != NYA_REFLECT_STRUCT && type->kind != NYA_REFLECT_UNION) {
        return nya_error(NYA_ERROR_INVALID_ARGUMENT, "'%s' is not a struct or union", type->name);
    }

    for (u32 i = 0; i < type->field_count; i++) {
        const NYA_ReflectField*   field      = &type->fields[i];
        const NYA_TypeReflection* field_type = field->type;

        if (field_type == nullptr) continue;

        NYA_Value* value = nya_object_get(object, (NYA_CString)field->name);

        // Absent means "leave it alone", not "zero it"; see the header: that is what lets an older save load into a newer struct without erasing fields it never heard of.
        if (value == nullptr) continue;

        void* address = (u8*)instance + field->offset;

        switch (field_type->kind) {
            case NYA_REFLECT_PRIMITIVE: {
                (void)nya_reflect_write(field_type, address, *value);
                break;
            }

            case NYA_REFLECT_ENUM: {
                if (field_type->is_bitflags && value->type == NYA_TYPE_ARRAY) {
                    s64 combined = 0;

                    nya_array_foreach (&value->as_array, element) {
                        if (element->type != NYA_TYPE_STRING || element->as_string == nullptr) continue;

                        s64 flag = 0;
                        if (!nya_reflect_variant_value(field_type, element->as_string, &flag)) continue;

                        combined |= flag;
                    }

                    (void)_nya_reflect_write_integer(field_type->primitive, address, combined);
                    break;
                }

                // Written as a name, so read as one, falling back to the number for unnamed values and hand-written files.
                if (value->type == NYA_TYPE_STRING && value->as_string != nullptr) {
                    s64 named = 0;

                    if (nya_reflect_variant_value(field_type, value->as_string, &named)) {
                        (void)_nya_reflect_write_integer(field_type->primitive, address, named);
                    }

                    break;
                }

                (void)nya_reflect_write(field_type, address, *value);
                break;
            }

            case NYA_REFLECT_STRUCT:
            case NYA_REFLECT_UNION: {
                if (value->type != NYA_TYPE_OBJECT) break;

                NYA_Error nested = nya_reflect_from_object(field_type, address, &value->as_object);
                if (!nested.ok) return nested;

                break;
            }

            case NYA_REFLECT_ARRAY:
            case NYA_REFLECT_VECTOR: {
                if (nya_reflect_is_char_array(field_type)) {
                    if (value->type != NYA_TYPE_STRING || value->as_string == nullptr) break;

                    char* destination = (char*)address;

                    u64 length = strlen(value->as_string);

                    // Truncated to fit and always terminated: the array is the struct's own storage and a longer string in the file must not run past it.
                    if (length >= field_type->element_count) {
                        length = field_type->element_count - 1;

                        // Back off any continuation bytes, since a cut mid-character would store invalid UTF-8 for anything downstream to trip over; same rule as nya_settings_player_name_set.
                        while (length > 0 && ((u8)value->as_string[length] & 0xC0) == 0x80) length--;
                    }

                    nya_memcpy(destination, value->as_string, length);
                    destination[length] = '\0';
                    break;
                }

                if (value->type != NYA_TYPE_ARRAY || field_type->element == nullptr) break;

                u32 index = 0;

                nya_array_foreach (&value->as_array, element) {
                    if (index >= field_type->element_count) break;

                    void* element_address = (u8*)address + ((u64)index * field_type->element->size);

                    if (field_type->element->kind == NYA_REFLECT_STRUCT && element->type == NYA_TYPE_OBJECT) {
                        NYA_Error nested = nya_reflect_from_object(field_type->element, element_address, &element->as_object);
                        if (!nested.ok) return nested;
                    } else {
                        (void)nya_reflect_write(field_type->element, element_address, *element);
                    }

                    index++;
                }

                break;
            }

            case NYA_REFLECT_POINTER: break;

            case NYA_REFLECT_COUNT:
            default: nya_unreachable();
        }
    }

    // The hook runs last, once every plain field is in place.
    if (type->on_apply != nullptr) return type->on_apply(instance);

    return NYA_OK;
}

u32 nya_reflect_check(const NYA_TypeReflection* type, const NYA_Object* object, NYA_ReflectReportFn report, void* user_data) {
    if (type == nullptr || object == nullptr) return 0;
    if (type->kind != NYA_REFLECT_STRUCT && type->kind != NYA_REFLECT_UNION) return 0;

    char path[NYA_REFLECT_PATH_MAX] = { 0 };

    return _nya_reflect_check_object(type, object, report, user_data, path, 0);
}

// PRIVATE API IMPLEMENTATION

u64 _nya_reflect_path_push(OUT char* path, u64 length, NYA_ConstCString name, char separator) {
    nya_assert(path != nullptr);
    nya_assert(length < NYA_REFLECT_PATH_MAX);

    if (length > 0 && length + 1 < NYA_REFLECT_PATH_MAX) {
        path[length] = separator;
        length++;
    }

    u64 name_length = strlen(name);

    // Truncated rather than grown: a report is a message, and a path deep enough to overflow this is already past where a longer string would help.
    if (length + name_length >= NYA_REFLECT_PATH_MAX) name_length = NYA_REFLECT_PATH_MAX - 1 - length;

    nya_memcpy(path + length, name, name_length);
    length       += name_length;
    path[length]  = '\0';

    return length;
}

void _nya_reflect_describe_value(const NYA_Value* value, OUT char* out, u64 capacity) {
    switch (value->type) {
        case NYA_TYPE_NULL:   (void)snprintf(out, capacity, "null"); return;
        case NYA_TYPE_OBJECT: (void)snprintf(out, capacity, "an object"); return;

        case NYA_TYPE_ARRAY:
            (void)snprintf(out, capacity, "a list of " FMTu64, value->as_array.length);
            return;

        // Quoted, so an empty string and a missing one do not read the same in a log line.
        case NYA_TYPE_STRING:
            (void)snprintf(out, capacity, "the text \"%s\"", value->as_string != nullptr ? value->as_string : "");
            return;

        case NYA_TYPE_B8: (void)snprintf(out, capacity, "%s", value->as_b8 ? "true" : "false"); return;

        case NYA_TYPE_F32: (void)snprintf(out, capacity, "the number " FMTf32, (f64)value->as_f32); return;
        case NYA_TYPE_F64: (void)snprintf(out, capacity, "the number " FMTf64, value->as_f64); return;

        default: break;
    }

    s64 integer = 0;
    if (nya_reflect_value_to_s64(*value, &integer)) {
        (void)snprintf(out, capacity, "the number " FMTs64, integer);
        return;
    }

    (void)snprintf(out, capacity, "a %s", NYA_TYPE_NAME_MAP[value->type]);
}

void _nya_reflect_describe_expected(const NYA_TypeReflection* type, OUT char* out, u64 capacity) {
    if (nya_reflect_is_char_array(type)) {
        (void)snprintf(out, capacity, "text of at most " FMTu32 " bytes", type->element_count - 1);
        return;
    }

    switch (type->kind) {
        case NYA_REFLECT_PRIMITIVE:
            if (type->primitive == NYA_TYPE_STRING) {
                (void)snprintf(out, capacity, "text");
                return;
            }

            (void)snprintf(out, capacity, "a %s", NYA_TYPE_NAME_MAP[type->primitive]);
            return;

        case NYA_REFLECT_STRUCT:
        case NYA_REFLECT_UNION:   (void)snprintf(out, capacity, "an object"); return;

        case NYA_REFLECT_ARRAY:
        case NYA_REFLECT_VECTOR:
            (void)snprintf(out, capacity, "a list of at most " FMTu32 " values", type->element_count);
            return;

        case NYA_REFLECT_POINTER: (void)snprintf(out, capacity, "nothing this build can load"); return;

        case NYA_REFLECT_ENUM:    break;

        case NYA_REFLECT_COUNT:
        default:                  nya_unreachable();
    }

    _nya_reflect_describe_names(out, capacity, type->is_bitflags ? "any of " : "one of ", type->variants, sizeof(type->variants[0]), type->variant_count);
}

void _nya_reflect_describe_names(OUT char* out, u64 capacity, NYA_ConstCString prefix, const void* names, u64 stride, u32 count) {
    nya_assert(out != nullptr);
    nya_assert(prefix != nullptr);
    nya_assert(names != nullptr || count == 0);
    nya_assert(stride >= sizeof(NYA_ConstCString), "the name must be the first member of the struct being listed");

    // The elision is held back out of the budget, so a list that does not fit ends on a whole name followed by "..." rather than halfway through a word.
    nya_assert(capacity > sizeof(_NYA_REFLECT_ELISION) + strlen(prefix));
    const u64 budget = capacity - sizeof(_NYA_REFLECT_ELISION) + 1;

    u64 written = (u64)snprintf(out, capacity, "%s", prefix);
    b8  first   = true;

    for (u32 i = 0; i < count; i++) {
        NYA_ConstCString name = *(const NYA_ConstCString*)((const u8*)names + ((u64)i * stride));

        // A generator that could not name something leaves it null rather than emitting an entry that claims to be called "".
        if (name == nullptr) continue;

        s32 added = snprintf(out + written, budget - written, first ? "%s" : ", %s", name);
        if (added < 0) break;

        if (written + (u64)added >= budget) {
            (void)snprintf(out + written, capacity - written, "%s", _NYA_REFLECT_ELISION);
            return;
        }

        written += (u64)added;
        first    = false;
    }
}

u32 _nya_reflect_check_value(
    const NYA_TypeReflection* type,
    const NYA_Value*          value,
    NYA_ReflectReportFn       report,
    void*                     user_data,
    OUT char*                 path,
    u64                       length
) {
    char found[_NYA_REFLECT_FOUND_MAX]       = { 0 };
    char expected[_NYA_REFLECT_EXPECTED_MAX] = { 0 };

    b8 accepted = false;

    if (type->kind == NYA_REFLECT_STRUCT || type->kind == NYA_REFLECT_UNION) {
        if (value->type != NYA_TYPE_OBJECT) {
            _nya_reflect_describe_value(value, found, sizeof(found));
            _nya_reflect_describe_expected(type, expected, sizeof(expected));
            if (report != nullptr) report(path, found, expected, user_data);

            return 1;
        }

        return _nya_reflect_check_object(type, &value->as_object, report, user_data, path, length);
    }

    if (type->kind == NYA_REFLECT_ENUM) {
        if (type->is_bitflags && value->type == NYA_TYPE_ARRAY) {
            u32 problems = 0;
            u32 index    = 0;

            nya_array_foreach (&value->as_array, element) {
                char element_path[NYA_REFLECT_PATH_MAX] = { 0 };
                char index_text[16]                     = { 0 };

                (void)snprintf(index_text, sizeof(index_text), FMTu32, index);
                nya_memcpy(element_path, path, length + 1);

                (void)_nya_reflect_path_push(element_path, length, index_text, '.');

                index++;

                s64 flag = 0;
                if (element->type == NYA_TYPE_STRING && element->as_string != nullptr &&
                    nya_reflect_variant_value(type, element->as_string, &flag)) {
                    continue;
                }

                _nya_reflect_describe_value(element, found, sizeof(found));
                _nya_reflect_describe_expected(type, expected, sizeof(expected));
                if (report != nullptr) report(element_path, found, expected, user_data);

                problems++;
            }

            return problems;
        }

        if (value->type == NYA_TYPE_STRING) {
            s64 named = 0;
            accepted  = value->as_string != nullptr && nya_reflect_variant_value(type, value->as_string, &named);
        } else {
            // The same widening the writer does, so the check cannot be stricter than what follows it.
            u8 scratch[sizeof(u64)] = { 0 };
            accepted                = nya_reflect_write(type, scratch, *value);
        }
    } else if (nya_reflect_is_char_array(type)) {
        accepted = value->type == NYA_TYPE_STRING && value->as_string != nullptr && strlen(value->as_string) < type->element_count;
    } else if (type->kind == NYA_REFLECT_ARRAY || type->kind == NYA_REFLECT_VECTOR) {
        accepted = value->type == NYA_TYPE_ARRAY && value->as_array.length <= type->element_count;
    } else if (type->kind == NYA_REFLECT_POINTER) {
        accepted = false;
    } else {
        // Written into a scratch cell rather than judged by a second copy of the writer's rules: the one that decides is the one that runs.
        u8 scratch[sizeof(u64)] = { 0 };

        nya_assert(type->size <= sizeof(scratch), "'%s' is a primitive wider than the check's scratch cell", type->name);

        accepted = nya_reflect_write(type, scratch, *value);
    }

    if (accepted) {
        if (type->kind != NYA_REFLECT_ARRAY && type->kind != NYA_REFLECT_VECTOR) return 0;
        if (nya_reflect_is_char_array(type) || type->element == nullptr) return 0;

        u32 problems = 0;
        u32 index    = 0;

        nya_array_foreach (&value->as_array, element) {
            char element_path[NYA_REFLECT_PATH_MAX] = { 0 };
            char index_text[16]                     = { 0 };

            (void)snprintf(index_text, sizeof(index_text), FMTu32, index);
            nya_memcpy(element_path, path, length + 1);

            u64 element_length = _nya_reflect_path_push(element_path, length, index_text, '.');

            problems += _nya_reflect_check_value(type->element, element, report, user_data, element_path, element_length);
            index++;
        }

        return problems;
    }

    _nya_reflect_describe_value(value, found, sizeof(found));
    _nya_reflect_describe_expected(type, expected, sizeof(expected));
    if (report != nullptr) report(path, found, expected, user_data);

    return 1;
}

u32 _nya_reflect_check_object(
    const NYA_TypeReflection* type,
    const NYA_Object*         object,
    NYA_ReflectReportFn       report,
    void*                     user_data,
    OUT char*                 path,
    u64                       length
) {
    u32 problems = 0;

    nya_dict_foreach_key (object, key_slot) {
        NYA_ConstCString key = *key_slot;

        char child_path[NYA_REFLECT_PATH_MAX] = { 0 };
        nya_memcpy(child_path, path, length + 1);

        u64 child_length = _nya_reflect_path_push(child_path, length, key, '.');

        const NYA_ReflectField* field = nya_reflect_field(type, key);
        NYA_Value*              value = nya_object_get(object, (NYA_CString)key);

        if (value == nullptr) continue;

        // Reported rather than ignored: an unknown key is usually a typo or a renamed setting, both invisible to whoever wrote the file if nothing says so.
        if (field == nullptr || field->type == nullptr) {
            char found[_NYA_REFLECT_FOUND_MAX]       = { 0 };
            char expected[_NYA_REFLECT_EXPECTED_MAX] = { 0 };

            (void)snprintf(found, sizeof(found), "not a key this build knows");

            // The keys themselves rather than the type's name: whoever reads this is looking at a file, not the source, and "one of msaa_samples, fxaa, bloom" is the answer to what they should have written.
            _nya_reflect_describe_names(expected, sizeof(expected), "one of ", type->fields, sizeof(type->fields[0]), type->field_count);

            if (report != nullptr) report(child_path, found, expected, user_data);

            problems++;
            continue;
        }

        problems += _nya_reflect_check_value(field->type, value, report, user_data, child_path, child_length);
    }

    return problems;
}

NYA_Value _nya_reflect_element_to_value(NYA_Arena* arena, const NYA_TypeReflection* element, const void* address, b8 redact) {
    if (element->kind == NYA_REFLECT_STRUCT || element->kind == NYA_REFLECT_UNION) {
        NYA_Object* nested = _nya_reflect_to_object(arena, element, address, redact);

        if (nested == nullptr) return (NYA_Value){ .type = NYA_TYPE_NULL };

        return (NYA_Value){ .type = NYA_TYPE_OBJECT, .as_object = *nested };
    }

    return nya_reflect_read(element, address);
}

b8 _nya_reflect_real_to_s64(f64 real, OUT s64* out_value) {
    // -2^63 and 2^63, both exact as an f64. Written as comparisons that a NaN fails, so it is refused too.
    const f64 lowest = (f64)S64_MIN;
    if (!(real >= lowest && real < -lowest)) return false;

    *out_value = (s64)real;
    return true;
}

b8 _nya_reflect_read_integer(NYA_Type primitive, const void* instance, OUT s64* out_value) {
    switch (primitive) {
        case NYA_TYPE_B8:  *out_value = (s64) * (const b8*)instance; return true;
        case NYA_TYPE_B16: *out_value = (s64) * (const b16*)instance; return true;
        case NYA_TYPE_B32: *out_value = (s64) * (const b32*)instance; return true;
        case NYA_TYPE_B64: *out_value = (s64) * (const b64*)instance; return true;

        case NYA_TYPE_U8:  *out_value = (s64) * (const u8*)instance; return true;
        case NYA_TYPE_U16: *out_value = (s64) * (const u16*)instance; return true;
        case NYA_TYPE_U32: *out_value = (s64) * (const u32*)instance; return true;

        // Reinterpreted rather than range checked: a u64 above the signed maximum comes back negative and is written back as the same bits, which round-trips even though it does not compare.
        case NYA_TYPE_U64: *out_value = (s64) * (const u64*)instance; return true;

        case NYA_TYPE_S8:  *out_value = (s64) * (const s8*)instance; return true;
        case NYA_TYPE_S16: *out_value = (s64) * (const s16*)instance; return true;
        case NYA_TYPE_S32: *out_value = (s64) * (const s32*)instance; return true;
        case NYA_TYPE_S64: *out_value = *(const s64*)instance; return true;

        case NYA_TYPE_CHAR: *out_value = (s64) * (const char*)instance; return true;

        default: return false;
    }
}

b8 _nya_reflect_write_integer(NYA_Type primitive, void* instance, s64 value) {
    switch (primitive) {
        case NYA_TYPE_B8:  *(b8*)instance = value != 0; return true;
        case NYA_TYPE_B16: *(b16*)instance = value != 0; return true;
        case NYA_TYPE_B32: *(b32*)instance = value != 0; return true;
        case NYA_TYPE_B64: *(b64*)instance = value != 0; return true;

        case NYA_TYPE_U8:  *(u8*)instance = (u8)value; return true;
        case NYA_TYPE_U16: *(u16*)instance = (u16)value; return true;
        case NYA_TYPE_U32: *(u32*)instance = (u32)value; return true;
        case NYA_TYPE_U64: *(u64*)instance = (u64)value; return true;

        case NYA_TYPE_S8:  *(s8*)instance = (s8)value; return true;
        case NYA_TYPE_S16: *(s16*)instance = (s16)value; return true;
        case NYA_TYPE_S32: *(s32*)instance = (s32)value; return true;
        case NYA_TYPE_S64: *(s64*)instance = value; return true;

        case NYA_TYPE_CHAR: *(char*)instance = (char)value; return true;

        default:            return false;
    }
}

// THE LAYOUT HASH

/** Eight bytes, least significant first, whatever the host's order. */
u64 _nya_reflect_layout_feed_u64(u64 hash, u64 value) {
    u8 bytes[sizeof(u64)];
    for (u32 i = 0; i < sizeof(u64); i++) bytes[i] = (u8)(value >> (i * 8));

    return nya_hash_fnv1a_continue(hash, bytes, sizeof(bytes));
}

/** Length first, so "ab" then "c" and "a" then "bc" do not hash alike. */
u64 _nya_reflect_layout_feed_text(u64 hash, NYA_ConstCString text) {
    NYA_ConstCString present = text != nullptr ? text : "";
    u64              length  = strlen(present);

    hash = _nya_reflect_layout_feed_u64(hash, length);
    return nya_hash_fnv1a_continue(hash, present, length);
}

u64 _nya_reflect_layout_feed_type(u64 hash, const NYA_TypeReflection* type, u32 depth) {
    nya_assert(type != nullptr);
    nya_assert(type->kind >= 0 && type->kind < NYA_REFLECT_COUNT, "a reflection table holds a kind outside the enum");
    nya_assert(type->primitive >= 0 && type->primitive < NYA_TYPE_COUNT, "a reflection table holds a primitive outside the enum");
    nya_assert(depth < NYA_REFLECT_LAYOUT_DEPTH_MAX, "'%s' nests deeper than NYA_REFLECT_LAYOUT_DEPTH_MAX", type->name);

    hash = _nya_reflect_layout_feed_text(hash, _NYA_REFLECT_KIND_NAME_MAP[type->kind]);
    hash = _nya_reflect_layout_feed_u64(hash, type->size);
    hash = _nya_reflect_layout_feed_u64(hash, type->alignment);

    switch (type->kind) {
        case NYA_REFLECT_PRIMITIVE: return _nya_reflect_layout_feed_text(hash, NYA_TYPE_NAME_MAP[type->primitive]);

        case NYA_REFLECT_ENUM:      {
            hash = _nya_reflect_layout_feed_text(hash, NYA_TYPE_NAME_MAP[type->primitive]);
            hash = _nya_reflect_layout_feed_u64(hash, type->is_bitflags ? 1 : 0);
            hash = _nya_reflect_layout_feed_u64(hash, type->variant_count);

            for (u32 i = 0; i < type->variant_count; i++) {
                hash = _nya_reflect_layout_feed_text(hash, type->variants[i].name);
                hash = _nya_reflect_layout_feed_u64(hash, (u64)type->variants[i].value);
            }
            return hash;
        }

        case NYA_REFLECT_STRUCT:
        case NYA_REFLECT_UNION:  {
            hash = _nya_reflect_layout_feed_u64(hash, type->field_count);

            for (u32 i = 0; i < type->field_count; i++) {
                const NYA_ReflectField* field = &type->fields[i];

                hash = _nya_reflect_layout_feed_text(hash, field->name);
                hash = _nya_reflect_layout_feed_u64(hash, field->offset);
                hash = _nya_reflect_layout_feed_u64(hash, field->has_tag_value ? 1 : 0);
                hash = _nya_reflect_layout_feed_u64(hash, (u64)field->tag_value);
                hash = _nya_reflect_layout_feed_type(hash, field->type, depth + 1);
            }

            // The name, and an empty one for none: an untagged union and one tagged by a field called "" cannot both exist, since a field always has a name.
            return _nya_reflect_layout_feed_text(hash, type->tag_field != nullptr ? type->tag_field->name : "");
        }

        case NYA_REFLECT_ARRAY:
        case NYA_REFLECT_VECTOR: {
            hash = _nya_reflect_layout_feed_u64(hash, type->element_count);
            return _nya_reflect_layout_feed_type(hash, type->element, depth + 1);
        }

        case NYA_REFLECT_POINTER: {
            if (type->element == nullptr) return _nya_reflect_layout_feed_text(hash, "void");

            hash = _nya_reflect_layout_feed_text(hash, _NYA_REFLECT_KIND_NAME_MAP[type->element->kind]);
            return _nya_reflect_layout_feed_text(hash, NYA_TYPE_NAME_MAP[type->element->primitive]);
        }

        default: nya_unreachable();
    }
}
