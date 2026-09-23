/**
 * @file base_thread.h
 *
 * Which thread the program calls its own, so a module that is only safe on it can say so and be
 * caught rather than believed.
 *
 * ```
 * nya_thread_main_claim       this thread is the program's. Called by the app, and by http's init
 * nya_thread_main_is_current  whether the caller is that thread
 * nya_thread_main_only        asserts it, naming what the caller was trying to reach
 * ```
 *
 * ```c
 * void nya_system_register(NYA_SystemEntry entry) {
 *     nya_thread_main_only("the system registry");
 *     ...
 * }
 * ```
 *
 * Until something claims, every thread answers yes: a program with one thread must not pay for a rule
 * it cannot break, and a guard that fires in a program that never started a second thread would only
 * teach people to delete guards. After a claim exactly one thread answers yes and every other one
 * trips the assertion the first time it reaches guarded code, which is the point: a handler that
 * promised to stay off program state fails loudly the first time it runs rather than corrupting
 * something once a month.
 *
 * Thread safety: all three. The claim is published once and read from everywhere.
 * */
#pragma once

#include "nyangine/base/base_attributes.h"
#include "nyangine/base/base_types.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * FUNCTIONS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/**
 * Names the calling thread as the program's main one.
 *
 * Called from nya_app_init_with_options, and from nya_system_http_init for a program that serves
 * without a frame loop. Calling it twice from the same thread is a no-op; calling it from a second
 * thread moves the claim, which no caller should want and none does.
 * */
NYA_API void nya_thread_main_claim(void);

/** Whether this is that thread, or nothing has claimed yet. */
NYA_API b8 nya_thread_main_is_current(void) __attr_no_discard;

/**
 * Crashes when it is not, naming `what` the caller was reaching for.
 *
 * The enforcement half of a "main thread only" note in a header: the note says which, this makes the
 * note true. Kept in release, like every other assertion here.
 * */
NYA_API void nya_thread_main_only(NYA_ConstCString what);
