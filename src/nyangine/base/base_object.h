#pragma once

#include "nyangine/base/base_dict.h"
#include "nyangine/base/base_error.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * TYPES
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

typedef struct NYA_Value NYA_Value;
nya_derive_array(NYA_Value);
nya_derive_dict(NYA_Value);

typedef NYA_DictᐸNYA_Valueᐳ NYA_Object;
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

        NYA_ArrayᐸNYA_Valueᐳ as_array;
    };
};

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * FUNCTIONS AND MACROS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

NYA_API NYA_Object* nya_object_create(NYA_Arena* arena);
NYA_API NYA_Object  nya_object_create_on_stack(NYA_Arena* arena);
NYA_API void        nya_object_reset(NYA_Object* obj);
NYA_API void        nya_object_destroy(NYA_Object* obj);
NYA_API void        nya_object_destroy_on_stack(NYA_Object* obj);
NYA_API NYA_Value*  nya_object_get(const NYA_Object* obj, NYA_CString key);
NYA_API void        nya_object_set(NYA_Object* obj, NYA_CString key, NYA_Value value);
NYA_API void        nya_object_remove(NYA_Object* obj, NYA_CString key);

NYA_API NYA_String* nya_s128_to_string(NYA_Arena* arena, s128 value);
NYA_API NYA_String* nya_u128_to_string(NYA_Arena* arena, u128 value);
