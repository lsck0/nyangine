#include "nyangine/base/base_assert.h"
#include "nyangine/base/base_basic.h"
#include "nyangine/base/base_thread.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * STATE
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/** True on exactly one thread, because only that thread ever runs the store. */
NYA_INTERNAL thread_local b8 _nya_thread_is_main = false;

/** Whether anybody has claimed at all. Read from every thread, so it is the atomic one. */
NYA_INTERNAL atomic b8 _nya_thread_main_claimed = false;

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PUBLIC API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

void nya_thread_main_claim(void) {
    _nya_thread_is_main = true;

    // released after the thread local, so a thread that sees the claim also sees its own false.
    atomic_store_explicit(&_nya_thread_main_claimed, true, memory_order_release);
}

b8 nya_thread_main_is_current(void) {
    if (!atomic_load_explicit(&_nya_thread_main_claimed, memory_order_acquire)) return true;

    return _nya_thread_is_main;
}

void nya_thread_main_only(NYA_ConstCString what) {
    nya_assert(what != nullptr);

    nya_assert_always(nya_thread_main_is_current(), "%s is the main thread's, and this is not it", what);
}
