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
 * Always compiled in, in every execution mode, shipping included. base_basic.h rejects
 * -DNYA_NO_ASSERT with an #error, so there is no configuration in which this costs nothing — write
 * the condition accordingly and keep side effects out of it.
 * */
#define nya_assert(...)             _NYA_ASSERT_ENABLED(__VA_ARGS__)

/**
 * The same assertion, spelled so the reader knows the check is load bearing.
 *
 * For invariants whose failure is a security or data integrity problem rather than a programming
 * mistake, where silently continuing is worse than dying: the alloca bound in base_memory.h and the
 * tamper check in base_integrity.c are the two cases.
 *
 * Identical to nya_assert today, and deliberately kept rather than folded into it. It used to be
 * the one form that survived -DNYA_NO_ASSERT; that flag is now refused outright, which makes the
 * distinction documentary. The documentation is the point — these are the call sites where removing
 * the check would be a vulnerability rather than a lost diagnostic, and that is worth saying at the
 * call site even when nothing enforces it.
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

/*
 * There is no disabled form.
 *
 * There was one, behind #ifdef NYA_NO_ASSERT, and it could never be reached: base_basic.h refuses
 * that macro with an #error, so the branch had not been compiled since the day that check was
 * added. It also did not do what its comment claimed — it kept the *condition* in a discarded
 * context but dropped the message and every format argument, so a variable mentioned only in the
 * message would have become unused the moment anyone did switch it on.
 *
 * Dead either way, and the kind of dead that is worse than absent: it documented a build mode the
 * engine does not have, in the file a reader checks to find out whether an assertion is guaranteed.
 */
