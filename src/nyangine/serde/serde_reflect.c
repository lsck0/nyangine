#include "nyangine/nyangine.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API DECLARATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/** Turns one finding from nya_reflect_check into a warning. `user_data` is the path being read. */
NYA_INTERNAL void _nya_serde_reflect_report(NYA_ConstCString path, NYA_ConstCString found, NYA_ConstCString expected, void* user_data);

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PUBLIC API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

NYA_Error nya_reflect_save_file(const NYA_TypeReflection* type, const void* instance, NYA_ConstCString path, NYA_SerdeFlags flags) {
    nya_assert(type != nullptr);
    nya_assert(instance != nullptr);
    nya_assert(path != nullptr);

    // On the stack and released here: the document exists only to be written, and handing the caller
    // an arena to hold something they never see would be ceremony for nothing.
    NYA_Arena scratch = nya_arena_create_on_stack(.name = "reflect_save_file");
    defer     nya_arena_destroy_on_stack(&scratch);

    NYA_Object* object = nya_reflect_to_object(&scratch, type, instance);
    if (object == nullptr) return nya_error(NYA_ERROR_INVALID_ARGUMENT, "'%s' is not a struct or union", type->name);

    return nya_serde_save_file(object, path, flags);
}

NYA_Error nya_reflect_load_file(const NYA_TypeReflection* type, void* instance, NYA_ConstCString path, NYA_SerdeFlags flags) {
    nya_assert(type != nullptr);
    nya_assert(instance != nullptr);
    nya_assert(path != nullptr);

    NYA_Arena scratch = nya_arena_create_on_stack(.name = "reflect_load_file");
    defer     nya_arena_destroy_on_stack(&scratch);

    NYA_Object* object = nullptr;
    NYA_TRY(nya_serde_load_file(&scratch, path, flags, &object));

    // Before anything is written, so the warnings describe the file as it was rather than as it
    // survived being applied. Counted only to keep the summary line honest.
    u32 problems = nya_reflect_check(type, object, _nya_serde_reflect_report, (void*)path);

    if (problems > 0) {
        nya_log_warn("'%s' has " FMTu32 " entr%s this build could not use; the rest of the file was loaded.", path, problems,
                     problems == 1 ? "y" : "ies");
    }

    return nya_reflect_from_object(type, instance, object);
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

void _nya_serde_reflect_report(NYA_ConstCString path, NYA_ConstCString found, NYA_ConstCString expected, void* user_data) {
    nya_log_warn("%s: '%s' is %s, expected %s; ignoring it.", (NYA_ConstCString)user_data, path, found, expected);
}
