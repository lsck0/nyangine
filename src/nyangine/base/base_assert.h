// clang-format off
/**
 * @file base_assert.h
 * */
#pragma once

#include "nyangine/base/base_basic.h"
#include "nyangine/base/base_logging.h"

/**
 * Usage:
 *
 * ```c
 * nya_assert(condition)
 * nya_assert(condition, message)
 * nya_assert(condition, format, ...) with a max. of 10 format arguments
 * ```
 * */
#define nya_assert(...)             _NYA_ASSERT_ENABLED(__VA_ARGS__)

/**
 * The same assertion, spelled to mark checks that guard security or data integrity rather than
 * catch a programming mistake: the alloca bound in base_memory.h and the tamper check in
 * base_integrity.c. Identical to nya_assert.
 * */
#define nya_assert_always(...)      _NYA_ASSERT_ENABLED(__VA_ARGS__)

/*
 * The comparison assertions, nya_assert_eq and its five siblings, are in base_watch.h: they print what
 * each side held, with the same formatter the watched locals of a crash report are printed by. They
 * cannot live here, because base_array.h includes this header and they need NYA_String.
 */

#define nya_assert_type_match(a, b) static_assert(__builtin_types_compatible_p(typeof(a), typeof(b)), "Incompatible types.")
#define nya_unused(...)             ((void)(0, __VA_ARGS__))

// do/while keeps these safe inside an unbraced if/else. __builtin_unreachable() is redundant while
// asserts are on (_nya_crash_raise is noreturn), but without it -Wreturn-type warns on every
// function ending in nya_unreachable().
#define nya_todo()                  do { nya_assert(0, "Todo"); __builtin_unreachable(); } while (0)
#define nya_unimplemented()         do { nya_assert(0, "Unimplemented"); __builtin_unreachable(); } while (0)
#define nya_unreachable()           do { nya_assert(0, "Unreachable"); __builtin_unreachable(); } while (0)

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * INTERNALS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

#define _NYA_ASSERT_ENABLED(...)  _NYA_PICK_ASSERT(__VA_ARGS__, _NYA_ASSERT3, _NYA_ASSERT3, _NYA_ASSERT3, _NYA_ASSERT3, _NYA_ASSERT3, _NYA_ASSERT3, _NYA_ASSERT3, _NYA_ASSERT3, _NYA_ASSERT3, _NYA_ASSERT3, _NYA_ASSERT2, _NYA_ASSERT1)(__VA_ARGS__)
#define _NYA_PICK_ASSERT(_1, _2, _3, _4, _5, _6, _7, _8, _9, _10, _11, _12, NAME, ...) NAME

#define _NYA_ASSERT1(condition)              do { if (!(condition)) { _nya_crash_raise(NYA_CRASH_SOURCE_ASSERT, __FUNCTION__, __FILE__, __LINE__, 0, "%s", #condition); } } while (0)
#define _NYA_ASSERT2(condition, message)     do { if (!(condition)) { _nya_crash_raise(NYA_CRASH_SOURCE_ASSERT, __FUNCTION__, __FILE__, __LINE__, 0, "%s, %s", #condition, message); } } while (0)
#define _NYA_ASSERT3(condition, format, ...) do { if (!(condition)) { _nya_crash_raise(NYA_CRASH_SOURCE_ASSERT, __FUNCTION__, __FILE__, __LINE__, 0, "%s, " format, #condition, __VA_ARGS__); } } while (0)

/* There is no disabled form. base_basic.h refuses NYA_NO_ASSERT with an #error. */
