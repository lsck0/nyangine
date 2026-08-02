#include "nyangine/nyangine.h"

NYA_Object* nya_object_create(NYA_Arena* arena) {
    nya_assert(arena != nullptr);
    return nya_dict_create(arena, NYA_Value);
}

NYA_Object nya_object_create_on_stack(NYA_Arena* arena) {
    nya_assert(arena != nullptr);
    return nya_dict_create_on_stack(arena, NYA_Value);
}

void nya_object_reset(NYA_Object* obj) {
    nya_assert(obj != nullptr);
    nya_dict_clear(obj);
}

void nya_object_destroy(NYA_Object* obj) {
    nya_assert(obj != nullptr);
    nya_dict_destroy(obj);
}

void nya_object_destroy_on_stack(NYA_Object* obj) {
    nya_assert(obj != nullptr);
    nya_dict_destroy_on_stack(*obj);
}

NYA_Value* nya_object_get(const NYA_Object* obj, NYA_CString key) {
    nya_assert(obj != nullptr);
    nya_assert(key != nullptr);
    return nya_dict_get(obj, key);
}

void nya_object_set(NYA_Object* obj, NYA_CString key, NYA_Value value) {
    nya_assert(obj != nullptr);
    nya_assert(key != nullptr);
    nya_dict_set(obj, key, value);
}

void nya_object_remove(NYA_Object* obj, NYA_CString key) {
    nya_assert(obj != nullptr);
    nya_assert(key != nullptr);
    nya_dict_remove(obj, key);
}

NYA_String* nya_s128_to_string(NYA_Arena* arena, s128 value) {
    NYA_String* string = nya_string_create(arena);

    if (value == 0) nya_string_extend(string, "0");

    b8   is_negative = value < 0;
    u128 uvalue      = is_negative ? (u128)(-(value + 1)) + 1 : (u128)value;

    while (uvalue > 0) {
        u8 digit  = (u8)(uvalue % 10);
        uvalue   /= 10;

        nya_string_extend_front_sprintf(string, "%c", '0' + digit);
    }

    if (is_negative) nya_string_extend_front(string, "-");

    return string;
}

NYA_String* nya_u128_to_string(NYA_Arena* arena, u128 value) {
    NYA_String* string = nya_string_create(arena);

    if (value == 0) nya_string_extend(string, "0");

    while (value > 0) {
        u8 digit  = (u8)(value % 10);
        value    /= 10;

        nya_string_extend_front_sprintf(string, "%c", '0' + digit);
    }

    return string;
}
