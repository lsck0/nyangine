#include "nyangine/nyangine.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API DECLARATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/**
 * Reads any integer-shaped primitive as a signed 64 bit value.
 *
 * Enums and the bool family go through here too: what an enum *is* at runtime is its underlying
 * integer, and treating it as anything else would need a second copy of this switch.
 *
 * Fails rather than truncating for the 128 bit types, which do not fit and have no honest answer.
 * */
NYA_INTERNAL b8 _nya_reflect_read_integer(NYA_Type primitive, const void* instance, OUT s64* out_value);
NYA_INTERNAL b8 _nya_reflect_write_integer(NYA_Type primitive, void* instance, s64 value);

/** The numeric content of a value, however it was spelled. See the note in nya_reflect_write. */
NYA_INTERNAL b8 _nya_reflect_value_to_s64(NYA_Value value, OUT s64* out_value);
NYA_INTERNAL b8 _nya_reflect_value_to_f64(NYA_Value value, OUT f64* out_value);

/** Whether an array of these is text rather than a list of numbers. See nya_reflect_to_object. */
NYA_INTERNAL b8 _nya_reflect_is_char_array(const NYA_TypeReflection* type);

/** One element of an array or vector, as a value. Shared by both, which differ only in their stride. */
NYA_INTERNAL NYA_Value _nya_reflect_element_to_value(NYA_Arena* arena, const NYA_TypeReflection* element, const void* address);

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PUBLIC API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

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
        /*
         * The segment is compared in place rather than copied out.
         *
         * A path is walked once per field per frame by an inspector, so the copy would be the only
         * allocation in an otherwise allocation free lookup — and the comparison is the same length
         * either way.
         */
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

        // Only descend when there is more path left: the last segment names the answer, and its type
        // may perfectly well be a primitive with no fields to walk into.
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

NYA_Value nya_reflect_read(const NYA_TypeReflection* type, const void* instance) {
    NYA_Value none = { .type = NYA_TYPE_NULL };

    if (type == nullptr || instance == nullptr) return none;

    // An enum reads as its underlying integer. Turning it into a name is nya_reflect_to_object's
    // business, because that is a serialisation choice and not what the field holds.
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

        /*
         * A string field is a pointer the struct does not own, and it is copied as that pointer.
         *
         * Whoever serialises the result has to finish before the owner frees it, which is true of
         * every string in an NYA_Object and is why they are arena allocated together.
         */
        case NYA_TYPE_STRING: return (NYA_Value){ .type = NYA_TYPE_STRING, .as_string = *(char* const*)instance };

        default: return none;
    }
}

b8 nya_reflect_write(const NYA_TypeReflection* type, void* instance, NYA_Value value) {
    if (type == nullptr || instance == nullptr) return false;

    if (type->kind == NYA_REFLECT_ENUM) {
        s64 integer = 0;
        if (!_nya_reflect_value_to_s64(value, &integer)) return false;

        return _nya_reflect_write_integer(type->primitive, instance, integer);
    }

    if (type->kind != NYA_REFLECT_PRIMITIVE) return false;

    /*
     * Coerced rather than matched exactly.
     *
     * A number that went through a file comes back as whatever the reader decided it was — 1 is an
     * integer and 1.0 is a float, and neither knows the field it is about to land in. Requiring an
     * exact type match would make a hand edited "scale": 1 fail to load into an f32.
     */
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
            if (!_nya_reflect_value_to_s64(value, &integer)) return false;

            *(char*)instance = (char)integer;
            return true;
        }

        case NYA_TYPE_F32: {
            f64 number = 0.0;
            if (!_nya_reflect_value_to_f64(value, &number)) return false;

            *(f32*)instance = (f32)number;
            return true;
        }

        case NYA_TYPE_F64: {
            f64 number = 0.0;
            if (!_nya_reflect_value_to_f64(value, &number)) return false;

            *(f64*)instance = number;
            return true;
        }

        default: {
            s64 integer = 0;
            if (!_nya_reflect_value_to_s64(value, &integer)) return false;

            return _nya_reflect_write_integer(type->primitive, instance, integer);
        }
    }
}

/*
 * ─────────────────────────────────────────────────────────
 * THE GENERIC CONVERSION
 * ─────────────────────────────────────────────────────────
 */

NYA_Object* nya_reflect_to_object(NYA_Arena* arena, const NYA_TypeReflection* type, const void* instance) {
    nya_assert(arena != nullptr);

    if (type == nullptr || instance == nullptr) return nullptr;
    if (type->kind != NYA_REFLECT_STRUCT && type->kind != NYA_REFLECT_UNION) return nullptr;

    NYA_Object* object = nya_object_create(arena);

    // A union is written as the one member its tag selects. Without a tag there is no way to know
    // which member is live, so nothing is written rather than something arbitrary. See the header.
    if (type->kind == NYA_REFLECT_UNION && type->tag_field == nullptr) return object;

    for (u32 i = 0; i < type->field_count; i++) {
        const NYA_ReflectField*   field       = &type->fields[i];
        const NYA_TypeReflection* field_type  = field->type;
        const void*               address     = (const u8*)instance + field->offset;

        if (field_type == nullptr) continue;

        switch (field_type->kind) {
            case NYA_REFLECT_PRIMITIVE: {
                NYA_Value value = nya_reflect_read(field_type, address);
                if (value.type == NYA_TYPE_NULL) continue;

                nya_object_set(object, (NYA_CString)field->name, value);
                break;
            }

            /*
             * An enum is written as its variant *name*.
             *
             * Same reasoning nya_settings_to_object gives for writing input action names rather than
             * their indices: a number is not a stable identity, and a game that inserts a variant in
             * the middle of an enum renumbers everything after it. A saved file keyed by number would
             * then load as the wrong thing, silently.
             */
            case NYA_REFLECT_ENUM: {
                s64 raw = 0;
                if (!_nya_reflect_read_integer(field_type->primitive, address, &raw)) continue;

                if (field_type->is_bitflags) {
                    // A set of flags is a list of names, so a flag being added or removed changes
                    // only which names appear rather than the meaning of a number.
                    NYA_ArrayᐸNYA_Valueᐳ* names = nya_array_create(arena, NYA_Value);

                    for (u32 v = 0; v < field_type->variant_count; v++) {
                        const NYA_ReflectVariant* variant = &field_type->variants[v];

                        if (variant->value == 0) continue;
                        if ((raw & variant->value) != variant->value) continue;

                        nya_array_push_back(names, ((NYA_Value){ .type = NYA_TYPE_STRING, .as_string = (NYA_CString)variant->name }));
                    }

                    nya_object_set(object, (NYA_CString)field->name, (NYA_Value){ .type = NYA_TYPE_ARRAY, .as_array = *names });
                    break;
                }

                NYA_ConstCString name = nya_reflect_variant_name(field_type, raw);

                // A value with no name is written as the number, because losing it entirely would be
                // worse than writing something a newer build can still read.
                if (name == nullptr) {
                    nya_object_set(object, (NYA_CString)field->name, (NYA_Value){ .type = NYA_TYPE_S64, .as_s64 = raw });
                    break;
                }

                nya_object_set(object, (NYA_CString)field->name, (NYA_Value){ .type = NYA_TYPE_STRING, .as_string = (NYA_CString)name });
                break;
            }

            case NYA_REFLECT_STRUCT:
            case NYA_REFLECT_UNION: {
                NYA_Object* nested = nya_reflect_to_object(arena, field_type, address);
                if (nested == nullptr) continue;

                nya_object_set(object, (NYA_CString)field->name, (NYA_Value){ .type = NYA_TYPE_OBJECT, .as_object = *nested });
                break;
            }

            case NYA_REFLECT_ARRAY:
            case NYA_REFLECT_VECTOR: {
                // A char array is text, not a list of numbers. `char name[32]` written as thirty two
                // integers is technically complete and useless to read.
                if (_nya_reflect_is_char_array(field_type)) {
                    const char* text = (const char*)address;

                    u64 length = 0;
                    while (length < field_type->element_count && text[length] != '\0') length++;

                    char* copy = nya_arena_alloc(arena, length + 1);
                    nya_memcpy(copy, text, length);
                    copy[length] = '\0';

                    nya_object_set(object, (NYA_CString)field->name, (NYA_Value){ .type = NYA_TYPE_STRING, .as_string = copy });
                    break;
                }

                if (field_type->element == nullptr) continue;

                NYA_ArrayᐸNYA_Valueᐳ* elements = nya_array_create(arena, NYA_Value);

                for (u32 e = 0; e < field_type->element_count; e++) {
                    const void* element_address = (const u8*)address + ((u64)e * field_type->element->size);

                    nya_array_push_back(elements, _nya_reflect_element_to_value(arena, field_type->element, element_address));
                }

                nya_object_set(object, (NYA_CString)field->name, (NYA_Value){ .type = NYA_TYPE_ARRAY, .as_array = *elements });
                break;
            }

            /*
             * Pointers are not followed.
             *
             * A pointer to a struct says nothing about who owns it, whether it is one object or an
             * array, or whether it is still alive. Following one would be guessing, and a wrong guess
             * here is a crash rather than a bad file. A game that wants the pointee described gives
             * the field its own accessor.
             */
            case NYA_REFLECT_POINTER: break;

            case NYA_REFLECT_COUNT:
            default: break;
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

        // Absent means "leave it alone", not "zero it". See the header: that is what lets an older
        // save load into a newer struct without erasing the fields it has never heard of.
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

                // Written as a name, so read as one — falling back to the number for the unnamed case
                // to_object emits, and for a file written by hand.
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
                if (_nya_reflect_is_char_array(field_type)) {
                    if (value->type != NYA_TYPE_STRING || value->as_string == nullptr) break;

                    char* destination = (char*)address;

                    u64 length = strlen(value->as_string);

                    // Truncated to fit, and always terminated: the array is the struct's own storage
                    // and a longer string in the file must not run past it.
                    if (length >= field_type->element_count) length = field_type->element_count - 1;

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

            case NYA_REFLECT_POINTER:
            case NYA_REFLECT_COUNT:
            default: break;
        }
    }

    /*
     * The hook runs last, once every plain field is in place.
     *
     * It exists because some of what a type needs on load is not a value to write but a thing to do —
     * creating a physics body from the shape and size that were just restored, say. See
     * NYA_TypeReflection.on_apply.
     */
    if (type->on_apply != nullptr) return type->on_apply(instance);

    return NYA_OK;
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

b8 _nya_reflect_is_char_array(const NYA_TypeReflection* type) {
    return type->kind == NYA_REFLECT_ARRAY && type->element != nullptr && type->element->kind == NYA_REFLECT_PRIMITIVE &&
           type->element->primitive == NYA_TYPE_CHAR;
}

NYA_Value _nya_reflect_element_to_value(NYA_Arena* arena, const NYA_TypeReflection* element, const void* address) {
    if (element->kind == NYA_REFLECT_STRUCT || element->kind == NYA_REFLECT_UNION) {
        NYA_Object* nested = nya_reflect_to_object(arena, element, address);

        if (nested == nullptr) return (NYA_Value){ .type = NYA_TYPE_NULL };

        return (NYA_Value){ .type = NYA_TYPE_OBJECT, .as_object = *nested };
    }

    return nya_reflect_read(element, address);
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

        // Reinterpreted rather than range checked: a u64 above the signed maximum comes back negative
        // and is written back as the same bits, which round trips even though it does not compare.
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

        default: return false;
    }
}

b8 _nya_reflect_value_to_s64(NYA_Value value, OUT s64* out_value) {
    switch (value.type) {
        case NYA_TYPE_B8:  *out_value = value.as_b8; return true;
        case NYA_TYPE_B16: *out_value = value.as_b16; return true;
        case NYA_TYPE_B32: *out_value = value.as_b32; return true;
        case NYA_TYPE_B64: *out_value = (s64)value.as_b64; return true;

        case NYA_TYPE_U8:  *out_value = value.as_u8; return true;
        case NYA_TYPE_U16: *out_value = value.as_u16; return true;
        case NYA_TYPE_U32: *out_value = value.as_u32; return true;
        case NYA_TYPE_U64: *out_value = (s64)value.as_u64; return true;

        case NYA_TYPE_S8:  *out_value = value.as_s8; return true;
        case NYA_TYPE_S16: *out_value = value.as_s16; return true;
        case NYA_TYPE_S32: *out_value = value.as_s32; return true;
        case NYA_TYPE_S64: *out_value = value.as_s64; return true;

        case NYA_TYPE_CHAR: *out_value = value.as_char; return true;

        // A whole number written with a decimal point is still a whole number. Truncation is
        // deliberate rather than an error, so "count": 3.0 loads.
        case NYA_TYPE_F32: *out_value = (s64)value.as_f32; return true;
        case NYA_TYPE_F64: *out_value = (s64)value.as_f64; return true;

        default: return false;
    }
}

b8 _nya_reflect_value_to_f64(NYA_Value value, OUT f64* out_value) {
    if (value.type == NYA_TYPE_F32) {
        *out_value = (f64)value.as_f32;
        return true;
    }

    if (value.type == NYA_TYPE_F64) {
        *out_value = value.as_f64;
        return true;
    }

    // Integers widen into a float without complaint, which is the case that matters: a hand written
    // 1 has to load into an f32 field.
    s64 integer = 0;
    if (!_nya_reflect_value_to_s64(value, &integer)) return false;

    *out_value = (f64)integer;
    return true;
}
