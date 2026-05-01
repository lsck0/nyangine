#pragma once

#include "nyangine/base/base_basic.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * MEMORY OPERATIONS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

#define nya_alloca  __builtin_alloca
#define nya_malloc  malloc
#define nya_realloc realloc
#define nya_calloc  calloc
#define nya_free    free

#define nya_memcmp  memcmp
#define nya_memcpy  memcpy
#define nya_memmove memmove
#define nya_memset  memset

#define nya_is_zeroed(val) (nya_memcmp(&(val), &(typeof(val)){ 0 }, sizeof(val)) == 0)

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * TYPE AND OFFSET MACROS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

#define nya_typeof_field(type, member) typeof(((type*)0)->member)
#define nya_sizeof_field(type, member) sizeof((((type*)0)->member))
#define nya_offsetof(type, member)     __builtin_offsetof(type, member)
#define nya_offsetof_end(type, member) (offsetof(type, member) + nya_sizeof_field(type, member))
#define nya_container_of(ptr, type, member)                                                                                                          \
    _Generic(                                                                                                                                        \
        ptr,                                                                                                                                         \
        const typeof(*(ptr))*: ((const type*)_nya_raw_container_of(ptr, type, member)),                                                              \
        default: ((type*)_nya_raw_container_of(ptr, type, member))                                                                                   \
    )
#define _nya_raw_container_of(ptr, type, member)                                                                                                     \
    ({                                                                                                                                               \
        void* ptr_var = (void*)(ptr);                                                                                                                \
        assert_type_match(*(ptr), ((type*)0)->member);                                                                                               \
        ((type*)(ptr_var - offsetof(type, member)));                                                                                                 \
    })

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * UNIT CONVERSION UTILITIES
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

#define nya_byte_to_kibyte(val) ((val) >> 10)
#define nya_byte_to_mebyte(val) ((val) >> 20)
#define nya_byte_to_gibyte(val) ((val) >> 30)
#define nya_byte_to_tebyte(val) ((val) >> 40)
#define nya_byte_to_kbyte(val)  ((val) / 1'000)
#define nya_byte_to_mbyte(val)  ((val) / 1'000'000)
#define nya_byte_to_gbyte(val)  ((val) / 1'000'000'000)
#define nya_byte_to_tbyte(val)  ((val) / 1'000'000'000'000LL)
#define nya_kibyte_to_byte(val) ((u64)(val) << 10)
#define nya_mebyte_to_byte(val) ((u64)(val) << 20)
#define nya_gibyte_to_byte(val) ((u64)(val) << 30)
#define nya_tebyte_to_byte(val) ((u64)(val) << 40)
#define nya_kbyte_to_byte(val)  ((val) * 1'000)
#define nya_mbyte_to_byte(val)  ((val) * 1'000'000)
#define nya_gbyte_to_byte(val)  ((val) * 1'000'000'000)
#define nya_tbyte_to_byte(val)  ((val) * 1'000'000'000'000LL)
