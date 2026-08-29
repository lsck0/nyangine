/**
 * @file base_types.h
 *
 * Fundamental type defines, casting and parsing.
 * */
#pragma once

#include "nyangine/base/base_attributes.h"
#include "nyangine/base/base_basic.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * TYPEDEFS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

typedef void* voidptr;

typedef uint8_t     b8;
typedef uint16_t    b16;
typedef uint32_t    b32;
typedef uint64_t    b64;
typedef __uint128_t b128;
typedef void        u0;
typedef uint8_t     u8;
typedef uint16_t    u16;
typedef uint32_t    u32;
typedef uint64_t    u64;
typedef __uint128_t u128;
typedef int8_t      s8;
typedef int16_t     s16;
typedef int32_t     s32;
typedef int64_t     s64;
typedef __int128_t  s128;
typedef _Float16    f16;
typedef float       f32;
typedef double      f64;
typedef long double f128;

/*
 * Named for their component width, matching the f types above: a c64 is two f64s, not 64 bits.
 *
 * Spelled with the builtin keywords rather than reusing f32/f64/f128, because _Complex is only
 * valid next to a real type *keyword* — `f32 _Complex` does not compile. The assertions keep the
 * two definitions from drifting apart.
 */
typedef float _Complex c32;
typedef double _Complex c64;
typedef long double _Complex c128;

static_assert(sizeof(c32) == 2 * sizeof(f32), "c32 must be exactly two f32 components.");
static_assert(sizeof(c64) == 2 * sizeof(f64), "c64 must be exactly two f64 components.");
static_assert(sizeof(c128) == 2 * sizeof(f128), "c128 must be exactly two f128 components.");

typedef b8*   b8ptr;
typedef b16*  b16ptr;
typedef b32*  b32ptr;
typedef b64*  b64ptr;
typedef b128* b128ptr;
typedef u0*   u0ptr;
typedef u8*   u8ptr;
typedef u16*  u16ptr;
typedef u32*  u32ptr;
typedef u64*  u64ptr;
typedef u128* u128ptr;
typedef s8*   s8ptr;
typedef s16*  s16ptr;
typedef s32*  s32ptr;
typedef s64*  s64ptr;
typedef s128* s128ptr;
typedef f16*  f16ptr;
typedef f32*  f32ptr;
typedef f64*  f64ptr;
typedef f128* f128ptr;

typedef wchar_t      wchar;
typedef wchar*       NYA_WCString;
typedef const wchar* NYA_ConstWCString;

typedef char*       NYA_CString;
typedef const char* NYA_ConstCString;

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * TYPE ENUM AND VALUE UNION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

typedef enum {
    NYA_TYPE_NULL,

    NYA_TYPE_VOID,
    NYA_TYPE_VOID_POINTER,

    NYA_TYPE_B8,
    NYA_TYPE_B16,
    NYA_TYPE_B32,
    NYA_TYPE_B64,
    NYA_TYPE_B128,
    NYA_TYPE_U8,
    NYA_TYPE_U16,
    NYA_TYPE_U32,
    NYA_TYPE_U64,
    NYA_TYPE_U128,
    NYA_TYPE_S8,
    NYA_TYPE_S16,
    NYA_TYPE_S32,
    NYA_TYPE_S64,
    NYA_TYPE_S128,
    NYA_TYPE_F16,
    NYA_TYPE_F32,
    NYA_TYPE_F64,
    NYA_TYPE_F128,

    NYA_TYPE_B8_POINTER,
    NYA_TYPE_B16_POINTER,
    NYA_TYPE_B32_POINTER,
    NYA_TYPE_B64_POINTER,
    NYA_TYPE_B128_POINTER,
    NYA_TYPE_U8_POINTER,
    NYA_TYPE_U16_POINTER,
    NYA_TYPE_U32_POINTER,
    NYA_TYPE_U64_POINTER,
    NYA_TYPE_U128_POINTER,
    NYA_TYPE_S8_POINTER,
    NYA_TYPE_S16_POINTER,
    NYA_TYPE_S32_POINTER,
    NYA_TYPE_S64_POINTER,
    NYA_TYPE_S128_POINTER,
    NYA_TYPE_F16_POINTER,
    NYA_TYPE_F32_POINTER,
    NYA_TYPE_F64_POINTER,
    NYA_TYPE_F128_POINTER,

    NYA_TYPE_CHAR,
    NYA_TYPE_WCHAR,
    NYA_TYPE_STRING,
    NYA_TYPE_WSTRING,

    NYA_TYPE_OBJECT,

    NYA_TYPE_ARRAY,

    NYA_TYPE_COUNT,
} NYA_Type;

__attr_allow_unused static const char* NYA_TYPE_NAME_MAP[NYA_TYPE_COUNT] = {
    [NYA_TYPE_NULL] = "null",

    [NYA_TYPE_VOID]         = "void",
    [NYA_TYPE_VOID_POINTER] = "void*",

    [NYA_TYPE_B8]   = "b8",
    [NYA_TYPE_B16]  = "b16",
    [NYA_TYPE_B32]  = "b32",
    [NYA_TYPE_B64]  = "b64",
    [NYA_TYPE_B128] = "b128",
    [NYA_TYPE_U8]   = "u8",
    [NYA_TYPE_U16]  = "u16",
    [NYA_TYPE_U32]  = "u32",
    [NYA_TYPE_U64]  = "u64",
    [NYA_TYPE_U128] = "u128",
    [NYA_TYPE_S8]   = "s8",
    [NYA_TYPE_S16]  = "s16",
    [NYA_TYPE_S32]  = "s32",
    [NYA_TYPE_S64]  = "s64",
    [NYA_TYPE_S128] = "s128",
    [NYA_TYPE_F16]  = "f16",
    [NYA_TYPE_F32]  = "f32",
    [NYA_TYPE_F64]  = "f64",
    [NYA_TYPE_F128] = "f128",

    [NYA_TYPE_B8_POINTER]   = "b8*",
    [NYA_TYPE_B16_POINTER]  = "b16*",
    [NYA_TYPE_B32_POINTER]  = "b32*",
    [NYA_TYPE_B64_POINTER]  = "b64*",
    [NYA_TYPE_B128_POINTER] = "b128*",
    [NYA_TYPE_U8_POINTER]   = "u8*",
    [NYA_TYPE_U16_POINTER]  = "u16*",
    [NYA_TYPE_U32_POINTER]  = "u32*",
    [NYA_TYPE_U64_POINTER]  = "u64*",
    [NYA_TYPE_U128_POINTER] = "u128*",
    [NYA_TYPE_S8_POINTER]   = "s8*",
    [NYA_TYPE_S16_POINTER]  = "s16*",
    [NYA_TYPE_S32_POINTER]  = "s32*",
    [NYA_TYPE_S64_POINTER]  = "s64*",
    [NYA_TYPE_S128_POINTER] = "s128*",
    [NYA_TYPE_F16_POINTER]  = "f16*",
    [NYA_TYPE_F32_POINTER]  = "f32*",
    [NYA_TYPE_F64_POINTER]  = "f64*",
    [NYA_TYPE_F128_POINTER] = "f128*",

    [NYA_TYPE_CHAR]    = "char",
    [NYA_TYPE_WCHAR]   = "wchar",
    [NYA_TYPE_STRING]  = "string",
    [NYA_TYPE_WSTRING] = "wstring",

    [NYA_TYPE_OBJECT] = "object",

    [NYA_TYPE_ARRAY] = "array",
};

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * LIMITS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

#define U8_MIN   ((u8)0)
#define U8_MAX   ((u8) ~(u8)0)
#define U16_MIN  ((u16)0)
#define U16_MAX  ((u16) ~(u16)0)
#define U32_MIN  ((u32)0)
#define U32_MAX  ((u32) ~(u32)0)
#define U64_MIN  ((u64)0)
#define U64_MAX  ((u64) ~(u64)0)
#define U128_MIN ((u128)0)
#define U128_MAX ((u128) ~(u128)0)

#define S8_MAX   ((s8)(U8_MAX >> 1))
#define S8_MIN   ((s8)(-S8_MAX - 1))
#define S16_MAX  ((s16)(U16_MAX >> 1))
#define S16_MIN  ((s16)(-S16_MAX - 1))
#define S32_MAX  ((s32)(U32_MAX >> 1))
#define S32_MIN  ((s32)(-S32_MAX - 1))
#define S64_MAX  ((s64)(U64_MAX >> 1))
#define S64_MIN  ((s64)(-S64_MAX - 1))
#define S128_MAX ((s128)(U128_MAX >> 1))
#define S128_MIN ((s128)(-S128_MAX - 1))

#define F16_MIN  (-65504.0F)
#define F16_MAX  (65504.0F)
#define F32_MIN  (-__FLT_MAX__)
#define F32_MAX  (__FLT_MAX__)
#define F64_MIN  (-__DBL_MAX__)
#define F64_MAX  (__DBL_MAX__)
#define F128_MIN (-__LDBL_MAX__)
#define F128_MAX (__LDBL_MAX__)

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * FORMATTING
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

// use like: nya_log_debug("number: "FMTu64, number);
#define FMTb8   "%" PRIu8
#define FMTb16  "%" PRIu16
#define FMTb32  "%" PRIu32
#define FMTb64  "%" PRIu64
#define FMTu8   "%" PRIu8
#define FMTu16  "%" PRIu16
#define FMTu32  "%" PRIu32
#define FMTu64  "%" PRIu64
#define FMTs8   "%" PRId8
#define FMTs16  "%" PRId16
#define FMTs32  "%" PRId32
#define FMTs64  "%" PRId64
#define FMTf16  "%e"
#define FMTf32  "%f"
#define FMTf64  "%lf"
#define FMTf128 "%Le"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * SAFE CASTING
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/*
 * A negative value is rejected on its own, before the range comparison.
 *
 * `val >= U32_MIN && val <= U32_MAX` cannot see a negative: the usual arithmetic conversions turn
 * the signed operand unsigned before either comparison runs, so -1 becomes the largest value of the
 * target type and satisfies both halves. nya_cast_to_u32(-1) therefore asserted nothing and handed
 * back 4294967295, which is the exact accident this macro exists to prevent. Only u8 and u16
 * escaped it, and only because they promote to int first.
 *
 * The value is also bound to a temporary, so an argument with a side effect happens once rather
 * than once per mention.
 * */

/** True when `val` is a signed type holding a value below zero. Always false for unsigned types. */
#define _nya_value_is_negative(val) (((typeof(val))-1 < (typeof(val))0) && ((val) < (typeof(val))0))


#define nya_cast_to_u8(val)                                                                                                              \
    ({                                                                                                                                        \
        __auto_type _nya_cast_value = (val);                                                                                                 \
        nya_assert(!_nya_value_is_negative(_nya_cast_value), "Cannot cast a negative value to u8.");                                        \
        nya_assert((u128)_nya_cast_value <= (u128)U8_MAX, "Cannot cast to u8.");                                                            \
        (u8)_nya_cast_value;                                                                                                                 \
    })

#define nya_cast_to_u16(val)                                                                                                              \
    ({                                                                                                                                        \
        __auto_type _nya_cast_value = (val);                                                                                                 \
        nya_assert(!_nya_value_is_negative(_nya_cast_value), "Cannot cast a negative value to u16.");                                        \
        nya_assert((u128)_nya_cast_value <= (u128)U16_MAX, "Cannot cast to u16.");                                                            \
        (u16)_nya_cast_value;                                                                                                                 \
    })

#define nya_cast_to_u32(val)                                                                                                              \
    ({                                                                                                                                        \
        __auto_type _nya_cast_value = (val);                                                                                                 \
        nya_assert(!_nya_value_is_negative(_nya_cast_value), "Cannot cast a negative value to u32.");                                        \
        nya_assert((u128)_nya_cast_value <= (u128)U32_MAX, "Cannot cast to u32.");                                                            \
        (u32)_nya_cast_value;                                                                                                                 \
    })

#define nya_cast_to_u64(val)                                                                                                              \
    ({                                                                                                                                        \
        __auto_type _nya_cast_value = (val);                                                                                                 \
        nya_assert(!_nya_value_is_negative(_nya_cast_value), "Cannot cast a negative value to u64.");                                        \
        nya_assert((u128)_nya_cast_value <= (u128)U64_MAX, "Cannot cast to u64.");                                                            \
        (u64)_nya_cast_value;                                                                                                                 \
    })

#define nya_cast_to_u128(val)                                                                                                              \
    ({                                                                                                                                        \
        __auto_type _nya_cast_value = (val);                                                                                                 \
        nya_assert(!_nya_value_is_negative(_nya_cast_value), "Cannot cast a negative value to u128.");                                        \
        nya_assert((u128)_nya_cast_value <= (u128)U128_MAX, "Cannot cast to u128.");                                                            \
        (u128)_nya_cast_value;                                                                                                                 \
    })

#define nya_cast_to_s8(val)                                                                                                                          \
    ({                                                                                                                                               \
        nya_assert((val) >= S8_MIN && (val) <= S8_MAX, "Cannot cast to s8.");                                                                        \
        (s8)(val);                                                                                                                                   \
    })

#define nya_cast_to_s16(val)                                                                                                                         \
    ({                                                                                                                                               \
        nya_assert((val) >= S16_MIN && (val) <= S16_MAX, "Cannot cast to s16.");                                                                     \
        (s16)(val);                                                                                                                                  \
    })

#define nya_cast_to_s32(val)                                                                                                                         \
    ({                                                                                                                                               \
        nya_assert((val) >= S32_MIN && (val) <= S32_MAX, "Cannot cast to s32.");                                                                     \
        (s32)(val);                                                                                                                                  \
    })

#define nya_cast_to_s64(val)                                                                                                                         \
    ({                                                                                                                                               \
        nya_assert((val) >= S64_MIN && (val) <= S64_MAX, "Cannot cast to s64.");                                                                     \
        (s64)(val);                                                                                                                                  \
    })

#define nya_cast_to_s128(val)                                                                                                                        \
    ({                                                                                                                                               \
        nya_assert((val) >= S128_MIN && (val) <= S128_MAX, "Cannot cast to s128.");                                                                  \
        (s128)(val);                                                                                                                                 \
    })

#define nya_cast_to_f16(val)                                                                                                                         \
    ({                                                                                                                                               \
        nya_assert((val) >= F16_MIN && (val) <= F16_MAX, "Cannot cast to f16.");                                                                     \
        (f16)(val);                                                                                                                                  \
    })

#define nya_cast_to_f32(val)                                                                                                                         \
    ({                                                                                                                                               \
        nya_assert((val) >= F32_MIN && (val) <= F32_MAX, "Cannot cast to f32.");                                                                     \
        (f32)(val);                                                                                                                                  \
    })

#define nya_cast_to_f64(val)                                                                                                                         \
    ({                                                                                                                                               \
        nya_assert((val) >= F64_MIN && (val) <= F64_MAX, "Cannot cast to f64.");                                                                     \
        (f64)(val);                                                                                                                                  \
    })

#define nya_cast_to_f128(val)                                                                                                                        \
    ({                                                                                                                                               \
        nya_assert((val) >= F128_MIN && (val) <= F128_MAX, "Cannot cast to f128.");                                                                  \
        (f128)(val);                                                                                                                                 \
    })

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PARSING
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

NYA_API b8 nya_type_parse(NYA_Type target, const u8* data, u64 length, OUT void* out_value);
NYA_API b8 nya_type_name_parse(const u8* data, u64 length, OUT NYA_Type* out_type, OUT NYA_ConstCString* out_type_name);
