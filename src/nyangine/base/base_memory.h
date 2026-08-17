#pragma once

#include "nyangine/base/base_basic.h"

// Named rather than relied on from base.h's ordering: the bounded nya_alloca below uses both, and a
// header that only compiles in one include order reports errors in the editor while building fine.
#include "nyangine/base/base_assert.h"
#include "nyangine/base/base_types.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * MEMORY OPERATIONS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/**
 * Largest stack allocation nya_alloca will make. Override with -DNYA_ALLOCA_MAX=<bytes>.
 *
 * 64KiB against the 8MiB a main thread usually gets, and well inside the smaller stacks a spawned
 * thread is given. Generous for the things this is used for — a path, a separator, a substring —
 * and small enough that several live at once still leave room for the frames around them.
 * */
/* Spelled out rather than via nya_kibyte_to_byte, which this header only defines further down. */
#ifndef NYA_ALLOCA_MAX
#define NYA_ALLOCA_MAX (64ULL * 1024ULL)
#endif

/**
 * alloca with an upper bound, because the failure mode without one has no diagnostic.
 *
 * Every caller here sizes the allocation from data — a path length, a separator length — so an
 * unbounded version overflows the stack on a long enough input. That does not report anything: it
 * is a SIGSEGV at whatever address the next frame would have touched, usually nowhere near the call
 * that caused it, and it is a stack clash rather than a crash if the guard page is jumped over.
 *
 * nya_assert_always rather than nya_assert, to mark the bound as load bearing. This is the case
 * base_assert.h describes: the failure is a memory safety problem rather than a mistake in the
 * caller's logic, and dying with a message beats continuing. The two macros are the same thing —
 * assertions cannot be compiled out at all — so this is a note to whoever edits it next, not a
 * guarantee the build system provides.
 *
 * A macro rather than a helper function so the assertion reports the *caller's* file and line. Out
 * of a function it would name this header every time, which is the one thing the message has to get
 * right — the whole problem with an unbounded alloca is that the crash points nowhere useful.
 *
 * The statement expression covers only the size check, and __builtin_alloca is applied to its
 * result from outside. That keeps the allocation in the caller's frame: alloca *inside* a statement
 * expression risks the block's stack being restored when the expression ends, freeing the memory
 * before the caller ever uses it. Evaluating `size` once is the other reason it is not a plain
 * comma expression.
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
#define nya_offsetof_end(type, member) (nya_offsetof(type, member) + nya_sizeof_field(type, member))

/*
 * Recovers the enclosing struct from a pointer to one of its members.
 *
 * Did not compile, and could not have: the type check below was spelled `assert_type_match`, without
 * the nya_ prefix the macro actually has, so expanding this was an implicit function declaration
 * followed by a hard error. Nothing expanded it — tests/nyangine/base/test_memory.c has a case for
 * it that says exactly this and then asserts `true` instead.
 *
 * The comparison uses typeof_unqual because the const branch of the _Generic below hands it a
 * `const T` while the member is a plain `T`, and __builtin_types_compatible_p does not ignore
 * top-level qualifiers — so the const path would have failed the check it was there to pass.
 *
 * The subtraction goes through u8* rather than void*: arithmetic on void* is a GNU extension, and
 * this header is included by code compiled with -Wno-gnu, which would have hidden it rather than
 * making it portable.
 */
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
