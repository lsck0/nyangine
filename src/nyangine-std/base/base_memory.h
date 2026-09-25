#pragma once

#include "nyangine-std/base/base_basic.h"

// Named rather than relied on from base.h's ordering: the bounded nya_alloca below uses both, and a header that compiles in only one include order shows editor errors while building fine.
#include "nyangine-std/base/base_assert.h"
#include "nyangine-std/base/base_types.h"

// MEMORY OPERATIONS

/**
 * Largest stack allocation nya_alloca will make. Override with -DNYA_ALLOCA_MAX=<bytes>.
 * */
/* Spelled out rather than via nya_kibyte_to_byte, which this header only defines further down. */
#ifndef NYA_ALLOCA_MAX
#define NYA_ALLOCA_MAX (64ULL * 1024ULL)
#endif

/**
 * alloca with an upper bound, because the failure mode without one has no diagnostic.
 * */
#define nya_alloca(size)                                                                                                                             \
    __builtin_alloca(({                                                                                                                              \
        u64 _nya_alloca_size = (size);                                                                                                               \
        nya_assert_always(                                                                                                                           \
            _nya_alloca_size <= NYA_ALLOCA_MAX,                                                                                                      \
            "stack allocation of " FMTu64 " bytes is past NYA_ALLOCA_MAX (" FMTu64 ")",                                                              \
            _nya_alloca_size,                                                                                                                        \
            (u64)NYA_ALLOCA_MAX                                                                                                                      \
        );                                                                                                                                           \
        _nya_alloca_size;                                                                                                                            \
    }))

#define nya_malloc  malloc
#define nya_realloc realloc
#define nya_calloc  calloc
#define nya_free    free

// The libc block operations, made safe to call with a count of zero.
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

// TYPE AND OFFSET MACROS

#define nya_typeof_field(type, member) typeof(((type*)0)->member)
#define nya_sizeof_field(type, member) sizeof((((type*)0)->member))
#define nya_offsetof(type, member)     __builtin_offsetof(type, member)
#define nya_offsetof_end(type, member) (nya_offsetof(type, member) + nya_sizeof_field(type, member))

// Recovers the enclosing struct from a pointer to one of its members.
#define nya_container_of(ptr, type, member)                                                                                                          \
    _Generic(                                                                                                                                        \
        ptr,                                                                                                                                         \
        const typeof(*(ptr))*: ((const type*)_nya_raw_container_of(ptr, type, member)),                                                              \
        default: ((type*)_nya_raw_container_of(ptr, type, member))                                                                                   \
    )
#define _nya_raw_container_of(ptr, type, member)                                                                                                     \
    ({                                                                                                                                               \
        static_assert(                                                                                                                               \
            __builtin_types_compatible_p(typeof_unqual(*(ptr)), typeof_unqual(((type*)0)->member)),                                                  \
            "nya_container_of: the pointer does not point at that member's type."                                                                    \
        );                                                                                                                                           \
        u8* _nya_container_of_base = (u8*)(void*)(ptr);                                                                                              \
        ((type*)(void*)(_nya_container_of_base - nya_offsetof(type, member)));                                                                       \
    })

// UNIT CONVERSION UTILITIES

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
