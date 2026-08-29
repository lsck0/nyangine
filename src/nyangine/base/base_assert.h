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
 * Always compiled in, shipping included — base_basic.h rejects -DNYA_NO_ASSERT with an #error, so
 * keep side effects out of the condition.
 * */
#define nya_assert(...)             _NYA_ASSERT_ENABLED(__VA_ARGS__)

/**
 * The same assertion, spelled so the reader knows the check is load bearing: for invariants whose
 * failure is a security or data-integrity problem rather than a programming mistake — the alloca
 * bound in base_memory.h and the tamper check in base_integrity.c are the two cases. Identical to
 * nya_assert today; kept separate because it used to be the one form surviving -DNYA_NO_ASSERT, and
 * the distinction stays documentary now that flag is refused outright.
 * */
#define nya_assert_always(...)      _NYA_ASSERT_ENABLED(__VA_ARGS__)

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

/*
 * There is no disabled form. One existed behind #ifdef NYA_NO_ASSERT, but was unreachable dead code
 * — base_basic.h refuses that macro with an #error — and buggy besides: it kept the *condition* in
 * a discarded context but dropped the message and every format argument, so a message-only variable
 * would have gone unused the moment it was switched on. Removed rather than fixed.
 */
