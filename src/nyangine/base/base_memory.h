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

/*
 * The libc block operations, made safe to call with a count of zero.
 *
 * memcpy, memmove, memcmp and memset all declare their pointer parameters non-null, and that holds
 * even when the count is zero — passing null is undefined behaviour the standard permits a compiler
 * to optimise around, and UBSan flags it outright.
 *
 * That matters here because an empty container in this engine has a null items pointer: an
 * NYA_String of length zero, a freshly created array, a substring that came out empty. Comparing
 * two empty strings or concatenating them therefore reached memcmp/memcpy with null and a count of
 * zero, and since FLAGS_SANITIZE pairs that check with -fno-sanitize-recover=all, it aborted rather
 * than reporting. Every caller guarding its own call sites would be the alternative, which is a
 * guard per call for a condition that is never interesting.
 *
 * Statement expressions so each argument is evaluated exactly once, and so the return value stays
 * what the libc function returns.
 * */
#define nya_memcmp(lhs, rhs, size)                                                                                                                   \
    ({                                                                                                                                               \
        u64 _nya_mem_size = (size);                                                                                                                  \
        _nya_mem_size == 0 ? 0 : memcmp((lhs), (rhs), _nya_mem_size);                                                                                \
    })

#define nya_memcpy(destination, source, size)                                                                                                        \
    ({                                                                                                                                               \
        void* _nya_mem_destination = (destination);                                                                                                  \
        u64   _nya_mem_size        = (size);                                                                                                         \
        if (_nya_mem_size != 0) memcpy(_nya_mem_destination, (source), _nya_mem_size);                                                               \
        _nya_mem_destination;                                                                                                                        \
    })

#define nya_memmove(destination, source, size)                                                                                                       \
    ({                                                                                                                                               \
        void* _nya_mem_destination = (destination);                                                                                                  \
        u64   _nya_mem_size        = (size);                                                                                                         \
        if (_nya_mem_size != 0) memmove(_nya_mem_destination, (source), _nya_mem_size);                                                              \
        _nya_mem_destination;                                                                                                                        \
    })

#define nya_memset(destination, value, size)                                                                                                         \
    ({                                                                                                                                               \
        void* _nya_mem_destination = (destination);                                                                                                  \
        u64   _nya_mem_size        = (size);                                                                                                         \
        if (_nya_mem_size != 0) memset(_nya_mem_destination, (value), _nya_mem_size);                                                                \
        _nya_mem_destination;                                                                                                                        \
    })

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
