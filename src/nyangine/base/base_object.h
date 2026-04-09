/**
 * @file base_object.h
 *
 * Generic dynamic object type, aka. string value hashmap. Supports all base value types, as well as, nested objects
 * and heterogeneous arrays.
 *
 * The main use for this is to serialize and deserialize data, by converting to and from objects:
 * ```c
 * typedef struct {
 *   NYA_CString name;
 *   u8          age;
 *   u8          gender;
 * } User;
 *
 * NYA_Object* user_to_object(NYA_Arena* arena, const User* user) {
 *   NYA_Object* obj = nya_object_create(arena);
 *   nya_object_set(obj, "name", (NYA_Value){ .type = NYA_TYPE_STRING, .as_string = user->name });
 *   nya_object_set(obj, "age", (NYA_Value){ .type = NYA_TYPE_U8, .as_u8 = user->age });
 *   nya_object_set(obj, "gender", (NYA_Value){ .type = NYA_TYPE_U8, .as_u8 = user->gender });
 *   return obj;
 * }
 *
 * User object_to_user(const NYA_Object* obj) {
 *   User user = { 0 };
 *
 *   NYA_Value* name_value   = nya_object_get(obj, "name");
 *   NYA_Value* age_value    = nya_object_get(obj, "age");
 *   NYA_Value* gender_value = nya_object_get(obj, "gender");
 *
 *   if (name_value && name_value->type == NYA_TYPE_STRING) {
 *     user.name = name_value->as_string;
 *   } else {
 *     // Handle error: name is missing or not a string
 *   }
 *
 *   if (age_value && age_value->type == NYA_TYPE_U8) {
 *     user.age = age_value->as_u8;
 *   } else {
 *     // Handle error: age is missing or not a u8
 *   }
 *
 *   if (gender_value && gender_value->type == NYA_TYPE_U8) {
 *     user.gender = gender_value->as_u8;
 *   } else {
 *     // Handle error: gender is missing or not a u8
 *   }
 *
 *   return user;
 * }
 *
 * s32 main(void) {
 *   NYA_Arena* arena = nya_arena_create();
 *
 *   User user = {
 *     .name   = "Alice",
 *     .age    = 30,
 *     .gender = 1,
 *   };
 *
 *   NYA_Object* obj             = user_to_object(arena, &user);
 *   NYA_String* serialized_user = nya_object_serialize(arena, obj, NYA_OBJECT_SERIALIZATION_FLAG_PRETTY);
 *   // store.. send.. whatever
 *   NYA_Object* deserialized_obj  = nullptr;
 *   NYA_EXPECT(nya_object_deserialize(arena, serialized_user.items, serialized_user.length, NYA_OBJECT_SERIALIZATION_FLAG_NONE, &deserialized_obj));
 *   User        deserialized_user = object_to_user(deserialized_obj);
 *
 *   // ...
 *
 *   nya_arena_destroy(arena);
 *
 *   return 0;
 * }
 * ```
 *
 * Serialization format example:
 * ```
 * 1 9876543210
 * {
 *     version: u32 3;
 *     application: object {
 *         name: string "MyApp";
 *         settings: object {
 *             window: object {
 *                 width: u32 1920;
 *                 height: u32 1080;
 *                 fullscreen: bool false;
 *             };
 *             graphics: object {
 *                 quality: string "high";
 *                 antialiasing: bool true;
 *                 shaders: string[] ["vertex", "fragment", "geometry",];
 *             };
 *         };
 *     };
 *     users: object[] [
 *         {
 *             id: u32 1;
 *             name: string "Alice";
 *             permissions: string[] ["read","write","admin",];
 *             metadata: object {
 *                 created: u64 1704067200;
 *                 last_login: u64 1706659200;
 *             };
 *         },
 *         {
 *             id: u32 2;
 *             name: string "Bob";
 *             permissions: string[] ["read",];
 *             metadata: object {
 *                 created: u64 1704153600;
 *                 last_login: u64 1706572800;
 *             };
 *         },
 *     ];
 * }
 * ```
 *
 * String values are enclosed in double quotes and support the following escape sequences:
 *   \"  \\  \n  \t  \r
 *
 * The serialization format supports C style comments via slash-star blocks.
 **/
#pragma once

#include "nyangine/base/base_dict.h"
#include "nyangine/base/base_error.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * TYPES
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

typedef enum NYA_ObjectSerializationFlags NYA_ObjectSerializationFlags;
typedef struct NYA_Value                  NYA_Value;
nya_derive_dict(NYA_Value);
typedef NYA_CString_NYA_Value_Dict NYA_Object;
nya_derive_array(NYA_Value);
nya_derive_array(NYA_Object);

struct NYA_Value {
    NYA_Type type;

    union {
        b8   as_b8;
        b16  as_b16;
        b32  as_b32;
        b64  as_b64;
        b128 as_b128;
        u8   as_u8;
        u16  as_u16;
        u32  as_u32;
        u64  as_u64;
        u128 as_u128;
        s8   as_s8;
        s16  as_s16;
        s32  as_s32;
        s64  as_s64;
        s128 as_s128;
        f16  as_f16;
        f32  as_f32;
        f64  as_f64;
        f128 as_f128;

        b8ptr   as_b8ptr;
        b16ptr  as_b16ptr;
        b32ptr  as_b32ptr;
        b64ptr  as_b64ptr;
        b128ptr as_b128ptr;
        u8ptr   as_u8ptr;
        u16ptr  as_u16ptr;
        u32ptr  as_u32ptr;
        u64ptr  as_u64ptr;
        u128ptr as_u128ptr;
        s8ptr   as_s8ptr;
        s16ptr  as_s16ptr;
        s32ptr  as_s32ptr;
        s64ptr  as_s64ptr;
        s128ptr as_s128ptr;
        f16ptr  as_f16ptr;
        f32ptr  as_f32ptr;
        f64ptr  as_f64ptr;
        f128ptr as_f128ptr;

        char   as_char;
        wchar  as_wchar;
        char*  as_string;
        wchar* as_wstring;

        NYA_Object as_object;

        NYA_ValueArray as_array;
    };
};

enum NYA_ObjectSerializationFlags {
    NYA_OBJECT_SERIALIZATION_FLAG_NONE = 0,

    /**
     * Base64-encode the serialized data.
     * Conflicts with NYA_OBJECT_SERIALIZATION_FLAG_PRETTY.
     * */
    NYA_OBJECT_SERIALIZATION_FLAG_MINIFY = 1 << 0,

    /**
     * Pretty-print the serialized data with indentation and newlines.
     * Conflicts with NYA_OBJECT_SERIALIZATION_FLAG_MINIFY.
     * */
    NYA_OBJECT_SERIALIZATION_FLAG_PRETTY = 1 << 1,

    /**
     * Skip CRC validation when deserializing.
     * Use for when the data is allowed to be externally modifiable.
     * */
    NYA_OBJECT_SERIALIZATION_FLAG_NO_CRC_VALIDATION = 1 << 2,

    /* internal */

    _NYA_OBJECT_SERIALIZATION_FLAG_NO_TYPE_SPECIFIER = 1 << 3,
};

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * FUNCTIONS AND MACROS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

// clang-format off
NYA_API NYA_EXTERN NYA_Object* nya_object_create(NYA_Arena* arena);
NYA_API NYA_EXTERN NYA_Object  nya_object_create_on_stack(NYA_Arena* arena);
NYA_API NYA_EXTERN void        nya_object_reset(NYA_Object* obj);
NYA_API NYA_EXTERN void        nya_object_destroy(NYA_Object* obj);
NYA_API NYA_EXTERN void        nya_object_destroy_on_stack(NYA_Object* obj);
NYA_API NYA_EXTERN NYA_Value*  nya_object_get(const NYA_Object* obj, NYA_CString key);
NYA_API NYA_EXTERN void        nya_object_set(NYA_Object* obj, NYA_CString key, NYA_Value value);
NYA_API NYA_EXTERN void        nya_object_remove(NYA_Object* obj, NYA_CString key);
NYA_API NYA_EXTERN NYA_String* nya_object_serialize(NYA_Arena* arena, const NYA_Object* obj, NYA_ObjectSerializationFlags flags);
NYA_API NYA_EXTERN NYA_Result nya_object_deserialize(NYA_Arena* arena, const u8* data, u64 size, NYA_ObjectSerializationFlags flags, OUT NYA_Object** out_obj);
// clang-format on

NYA_API NYA_EXTERN NYA_String* nya_s128_to_string(NYA_Arena* arena, s128 value);
NYA_API NYA_EXTERN NYA_String* nya_u128_to_string(NYA_Arena* arena, u128 value);
