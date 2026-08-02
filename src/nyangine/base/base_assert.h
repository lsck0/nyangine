// clang-format off
/**
 * @file base_assert.h
 *
 * Assertions. Every failure routes into the central crash sink in base_logging.h, so an assertion
 * is observable by telemetry and a crash window exactly like a panic or a fault is.
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
 *
 * Compiled out by -DNYA_NO_ASSERT. Use nya_assert_always for checks that must survive that.
 * */
#define nya_assert(...)             _nya_assert(__VA_ARGS__)

/**
 * An assertion that -DNYA_NO_ASSERT does not remove.
 *
 * For invariants whose failure is a security or data integrity problem rather than a programming
 * mistake, where silently continuing is worse than dying.
 * */
#define nya_assert_always(...)      _NYA_ASSERT_ENABLED(__VA_ARGS__)

#define nya_assert_type_match(a, b) static_assert(__builtin_types_compatible_p(typeof(a), typeof(b)), "Incompatible types.")
#define nya_unused(...)             ((void)(0, __VA_ARGS__))

// The do/while keeps these safe inside an unbraced if/else, which the bare two-statement form was
// not. __builtin_unreachable() is redundant while asserts are on, since _nya_crash_raise is
// noreturn, but under -DNYA_NO_ASSERT it is the only thing telling the compiler control stops
// here. Without it, every function ending in nya_unreachable() warns under -Wreturn-type.
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

#ifdef NYA_NO_ASSERT
// The condition is kept in a discarded context rather than dropped outright, so that variables it
// mentions do not suddenly become unused and side effects inside it still fail to compile loudly.
#define _nya_assert(...)                     _NYA_ASSERT_DISABLED(__VA_ARGS__)
#define _NYA_ASSERT_DISABLED(...)            _NYA_PICK_ASSERT(__VA_ARGS__, _NYA_NOASSERT3, _NYA_NOASSERT3, _NYA_NOASSERT3, _NYA_NOASSERT3, _NYA_NOASSERT3, _NYA_NOASSERT3, _NYA_NOASSERT3, _NYA_NOASSERT3, _NYA_NOASSERT3, _NYA_NOASSERT3, _NYA_NOASSERT2, _NYA_NOASSERT1)(__VA_ARGS__)
#define _NYA_NOASSERT1(condition)              ((void)sizeof((condition) ? 1 : 0))
#define _NYA_NOASSERT2(condition, message)     ((void)sizeof((condition) ? 1 : 0))
#define _NYA_NOASSERT3(condition, format, ...) ((void)sizeof((condition) ? 1 : 0))
#else
#define _nya_assert(...)                     _NYA_ASSERT_ENABLED(__VA_ARGS__)
#endif // NYA_NO_ASSERT
