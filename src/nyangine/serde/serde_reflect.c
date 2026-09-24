#include "nyangine/nyangine.h"

// ───────────────────────────────────── PRIVATE API DECLARATION ─────────────────────────────────────

/** Turns one finding from nya_reflect_check into a warning. `user_data` is the path being read. */
NYA_INTERNAL void _nya_serde_reflect_report(NYA_ConstCString path, NYA_ConstCString found, NYA_ConstCString expected, void* user_data);

/** nya_reflect_save_file and its sealed twin, with `secret` null for the plain one. */
NYA_INTERNAL NYA_Error
_nya_serde_reflect_save(const NYA_TypeReflection* type, const void* instance, NYA_ConstCString path, NYA_SerdeFlags flags, const NYA_SerdeSecret* secret);

/** nya_reflect_load_file and its sealed twin, likewise. */
NYA_INTERNAL NYA_Error
_nya_serde_reflect_load(const NYA_TypeReflection* type, void* instance, NYA_ConstCString path, NYA_SerdeFlags flags, const NYA_SerdeSecret* secret);

/**
 * Walks `object` beside `type` and encrypts every `@secret` field in place, recursing through plain
 * nested structs and arrays of them to reach any deeper. Refuses, rather than writing plaintext, when
 * a `@secret` field is met and `secret` is null.
 * */
NYA_INTERNAL NYA_Error _nya_serde_reflect_seal(NYA_Arena* arena, const NYA_TypeReflection* type, NYA_Object* object, const NYA_SerdeSecret* secret);

/** The inverse of _nya_serde_reflect_seal: decrypts every `@secret` field in place, or fails closed. */
NYA_INTERNAL NYA_Error _nya_serde_reflect_unseal(NYA_Arena* arena, const NYA_TypeReflection* type, NYA_Object* object, const NYA_SerdeSecret* secret);

/** Seals one field's value: its binary `.nya` encoding through `secret->seal`, the result stored as a string. */
NYA_INTERNAL NYA_Error _nya_serde_reflect_seal_value(NYA_Arena* arena, NYA_ConstCString field, NYA_Value* value, const NYA_SerdeSecret* secret);

/** Opens one sealed field: `secret->unseal`, then the binary `.nya` decode back into the value. */
NYA_INTERNAL NYA_Error _nya_serde_reflect_unseal_value(NYA_Arena* arena, NYA_ConstCString field, NYA_Value* value, const NYA_SerdeSecret* secret);

// ───────────────────────────────────── PUBLIC API IMPLEMENTATION ─────────────────────────────────────

NYA_Error nya_reflect_save_file(const NYA_TypeReflection* type, const void* instance, NYA_ConstCString path, NYA_SerdeFlags flags) {
    return _nya_serde_reflect_save(type, instance, path, flags, nullptr);
}

NYA_Error nya_reflect_load_file(const NYA_TypeReflection* type, void* instance, NYA_ConstCString path, NYA_SerdeFlags flags) {
    return _nya_serde_reflect_load(type, instance, path, flags, nullptr);
}

NYA_Error
nya_reflect_save_file_secret(const NYA_TypeReflection* type, const void* instance, NYA_ConstCString path, NYA_SerdeFlags flags, NYA_SerdeSecret secret) {
    return _nya_serde_reflect_save(type, instance, path, flags, &secret);
}

NYA_Error nya_reflect_load_file_secret(const NYA_TypeReflection* type, void* instance, NYA_ConstCString path, NYA_SerdeFlags flags, NYA_SerdeSecret secret) {
    return _nya_serde_reflect_load(type, instance, path, flags, &secret);
}

// ───────────────────────────────────── PRIVATE API IMPLEMENTATION ─────────────────────────────────────

NYA_Error
_nya_serde_reflect_save(const NYA_TypeReflection* type, const void* instance, NYA_ConstCString path, NYA_SerdeFlags flags, const NYA_SerdeSecret* secret) {
    nya_assert(type != nullptr);
    nya_assert(instance != nullptr);
    nya_assert(path != nullptr);

    // On the stack and released here: the document exists only to be written, so the caller need not hold an arena for it.
    NYA_Arena scratch = nya_arena_create_on_stack(.name = "reflect_save_file");
    defer     nya_arena_destroy_on_stack(&scratch);

    NYA_Object* object = nya_reflect_to_object(&scratch, type, instance);
    if (object == nullptr) return nya_error(NYA_ERROR_INVALID_ARGUMENT, "'%s' is not a struct or union", type->name);

    // Before the write: seals each `@secret` field's plaintext into a string; a `@secret` field with no cipher is refused here, so nothing reaches the file.
    NYA_TRY(_nya_serde_reflect_seal(&scratch, type, object, secret));

    return nya_serde_save_file(object, path, flags);
}

NYA_Error
_nya_serde_reflect_load(const NYA_TypeReflection* type, void* instance, NYA_ConstCString path, NYA_SerdeFlags flags, const NYA_SerdeSecret* secret) {
    nya_assert(type != nullptr);
    nya_assert(instance != nullptr);
    nya_assert(path != nullptr);

    NYA_Arena scratch = nya_arena_create_on_stack(.name = "reflect_load_file");
    defer     nya_arena_destroy_on_stack(&scratch);

    NYA_Object* object = nullptr;
    NYA_TRY(nya_serde_load_file(&scratch, path, flags, &object));

    // Before the check and before anything is applied: unsealing here means a wrong key or tampered value stops the load rather than reading a secret wrong.
    NYA_TRY(_nya_serde_reflect_unseal(&scratch, type, object, secret));

    // Before anything is written, so the warnings describe the file as it was; counted only to keep the summary line honest.
    u32 problems = nya_reflect_check(type, object, _nya_serde_reflect_report, (void*)path);

    if (problems > 0) {
        nya_log_warn("'%s' has " FMTu32 " entr%s this build could not use; the rest of the file was loaded.", path, problems,
                     problems == 1 ? "y" : "ies");
    }

    return nya_reflect_from_object(type, instance, object);
}

NYA_Error _nya_serde_reflect_seal(NYA_Arena* arena, const NYA_TypeReflection* type, NYA_Object* object, const NYA_SerdeSecret* secret) {
    nya_assert(type != nullptr);
    nya_assert(object != nullptr);

    for (u32 i = 0; i < type->field_count; i++) {
        const NYA_ReflectField*   field      = &type->fields[i];
        const NYA_TypeReflection* field_type = field->type;

        if (field_type == nullptr) continue;

        NYA_Value* value = nya_object_get(object, (NYA_CString)field->name);
        if (value == nullptr) continue;

        // A `@secret` field is sealed whole, whatever its shape, so a secret struct is not walked into.
        if (field->is_secret) {
            if (secret == nullptr || secret->seal == nullptr) {
                return nya_error(NYA_ERROR_PERMISSION_DENIED, "'%s.%s' is @secret and no key was given; refusing to write it in the clear",
                                 type->name, field->name);
            }

            NYA_TRY(_nya_serde_reflect_seal_value(arena, field->name, value, secret));
            continue;
        }

        // A plain nested struct or union may hold a `@secret` field of its own.
        if ((field_type->kind == NYA_REFLECT_STRUCT || field_type->kind == NYA_REFLECT_UNION) && value->type == NYA_TYPE_OBJECT) {
            NYA_TRY(_nya_serde_reflect_seal(arena, field_type, &value->as_object, secret));
            continue;
        }

        // As may each element of a plain array of them.
        if ((field_type->kind == NYA_REFLECT_ARRAY || field_type->kind == NYA_REFLECT_VECTOR) && field_type->element != nullptr &&
            (field_type->element->kind == NYA_REFLECT_STRUCT || field_type->element->kind == NYA_REFLECT_UNION) && value->type == NYA_TYPE_ARRAY) {
            nya_array_foreach (&value->as_array, element) {
                if (element->type != NYA_TYPE_OBJECT) continue;
                NYA_TRY(_nya_serde_reflect_seal(arena, field_type->element, &element->as_object, secret));
            }
        }
    }

    return NYA_OK;
}

NYA_Error _nya_serde_reflect_unseal(NYA_Arena* arena, const NYA_TypeReflection* type, NYA_Object* object, const NYA_SerdeSecret* secret) {
    nya_assert(type != nullptr);
    nya_assert(object != nullptr);

    for (u32 i = 0; i < type->field_count; i++) {
        const NYA_ReflectField*   field      = &type->fields[i];
        const NYA_TypeReflection* field_type = field->type;

        if (field_type == nullptr) continue;

        NYA_Value* value = nya_object_get(object, (NYA_CString)field->name);
        if (value == nullptr) continue;

        if (field->is_secret) {
            if (secret == nullptr || secret->unseal == nullptr) {
                return nya_error(NYA_ERROR_PERMISSION_DENIED, "'%s.%s' is @secret and no key was given to open it", type->name, field->name);
            }

            NYA_TRY(_nya_serde_reflect_unseal_value(arena, field->name, value, secret));
            continue;
        }

        if ((field_type->kind == NYA_REFLECT_STRUCT || field_type->kind == NYA_REFLECT_UNION) && value->type == NYA_TYPE_OBJECT) {
            NYA_TRY(_nya_serde_reflect_unseal(arena, field_type, &value->as_object, secret));
            continue;
        }

        if ((field_type->kind == NYA_REFLECT_ARRAY || field_type->kind == NYA_REFLECT_VECTOR) && field_type->element != nullptr &&
            (field_type->element->kind == NYA_REFLECT_STRUCT || field_type->element->kind == NYA_REFLECT_UNION) && value->type == NYA_TYPE_ARRAY) {
            nya_array_foreach (&value->as_array, element) {
                if (element->type != NYA_TYPE_OBJECT) continue;
                NYA_TRY(_nya_serde_reflect_unseal(arena, field_type->element, &element->as_object, secret));
            }
        }
    }

    return NYA_OK;
}

NYA_Error _nya_serde_reflect_seal_value(NYA_Arena* arena, NYA_ConstCString field, NYA_Value* value, const NYA_SerdeSecret* secret) {
    // The value is wrapped and encoded to the binary `.nya` form first, so a secret of any shape is the same handful of bytes to the cipher.
    NYA_Object* wrapper = nya_object_create(arena);
    nya_object_add(wrapper, "v", *value);

    NYA_String* plaintext = nullptr;
    NYA_TRY(nya_serde_nya_binary_encode(arena, wrapper, nullptr, &plaintext));

    NYA_String* text = nullptr;
    NYA_TRY(secret->seal(secret->user, arena, field, plaintext->items, plaintext->length, &text));

    *value = (NYA_Value){ .type = NYA_TYPE_STRING, .as_string = (NYA_CString)text->items };

    return NYA_OK;
}

NYA_Error _nya_serde_reflect_unseal_value(NYA_Arena* arena, NYA_ConstCString field, NYA_Value* value, const NYA_SerdeSecret* secret) {
    if (value->type != NYA_TYPE_STRING || value->as_string == nullptr) {
        return nya_error(NYA_ERROR_PARSE, "'%s' is @secret but the document holds no sealed string there", field);
    }

    NYA_String* plaintext = nullptr;
    NYA_TRY(secret->unseal(secret->user, arena, field, value->as_string, strlen(value->as_string), &plaintext));

    NYA_Object* wrapper = nullptr;
    NYA_TRY(nya_serde_nya_binary_decode(arena, plaintext->items, plaintext->length, nullptr, &wrapper));

    NYA_Value* inner = nya_object_get(wrapper, "v");
    if (inner == nullptr) return nya_error(NYA_ERROR_PARSE, "the sealed value for '%s' opened to nothing", field);

    *value = *inner;

    return NYA_OK;
}

void _nya_serde_reflect_report(NYA_ConstCString path, NYA_ConstCString found, NYA_ConstCString expected, void* user_data) {
    nya_log_warn("%s: '%s' is %s, expected %s; ignoring it.", (NYA_ConstCString)user_data, path, found, expected);
}
