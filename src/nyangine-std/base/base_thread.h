/**
 * @file base_thread.h
 *
 * Threads the engine can start, wait for and lock against, and the one thread the program calls its own.
 *
 * ```
 * nya_thread_spawn            starts a function on a thread of its own, out of an arena
 * nya_thread_is_finished      whether it has returned, without waiting for it
 * nya_thread_join             waits for it and hands the record back
 * nya_thread_abandon          stops waiting for it, for a deadline that has passed
 *
 * nya_mutex_create            a lock, in an arena; lock and unlock take a null one as "no lock"
 * nya_semaphore_create        a counting semaphore, with post and a bounded wait
 *
 * nya_thread_main_claim       this thread is the program's. Called by the app, and by http's init
 * nya_thread_main_is_current  whether the caller is that thread
 * nya_thread_main_only        asserts it, naming what the caller was trying to reach
 * ```
 *
 * ```c
 * NYA_Thread* worker = nullptr;
 * NYA_TRY(nya_thread_spawn(arena, drain, &server, "HTTP Worker", &worker));
 * ...
 * nya_thread_join(worker);
 * ```
 *
 * os/os_thread.h is the host's half of this and takes no arena, reports no NYA_Error and asserts
 * nothing. What is added here is what the rest of the engine needs and that layer may not have: a
 * record in an arena instead of storage the caller has to place, an error instead of a status, the
 * assertions, and the flag a thread raises as it returns — which is what lets a scheduler ask "is this
 * one done" without blocking on it, since neither host offers that question about a thread.
 *
 * ── a null mutex is no mutex ──
 *
 * nya_mutex_lock and nya_mutex_unlock do nothing at all when handed null. That is not defensiveness: a
 * server that runs its whole exchange on the ticking thread has no second thread to race with, and the
 * same code answers a request in that mode and under a worker pool. Null is how it says "there is
 * nothing here to lock", instead of paying for a lock nobody contends or growing a branch per call site.
 *
 * ── what a thread costs to forget ──
 *
 * A spawned thread is joined or abandoned, never dropped: both hosts hold a thread's last resources
 * until one of the two happens. Joining hands its record back to the arena it came from, so a system
 * that starts one thread per job does not grow an arena per job. Abandoning deliberately does not: the
 * thread is still running and still writes its own finished flag, so the record outlives the wait.
 *
 * Thread safety: nya_thread_is_finished, the mutex and the semaphore calls, and the three main-thread
 * ones are safe from anywhere. Spawning, joining and destroying reach into an arena, so they belong to
 * whoever owns it.
 * */
#pragma once

#include "nyangine-std/base/base_arena.h"
#include "nyangine-std/base/base_attributes.h"
#include "nyangine-std/base/base_error.h"
#include "nyangine-std/base/base_types.h"

// TYPES

typedef struct NYA_Thread    NYA_Thread;
typedef struct NYA_Mutex     NYA_Mutex;
typedef struct NYA_Semaphore NYA_Semaphore;

/** What a thread runs. Whatever it means by finishing is its own; nothing here reads a return value. */
typedef void (*NYA_ThreadFn)(void* data);

// FUNCTIONS

// THREADS

/**
 * Starts `function` on a thread of its own with `data`, out of `arena`.
 *
 * `name` labels the thread for a debugger and has to outlive it, which every caller satisfies by
 * passing a literal. `data` has to outlive it too, and so does the arena: the record the thread writes
 * its finished flag into lives there.
 * */
NYA_API NYA_Error nya_thread_spawn(NYA_Arena* arena, NYA_ThreadFn function, void* data, NYA_ConstCString name, OUT NYA_Thread** out_thread)
    __attr_no_discard;

/**
 * Whether `thread`'s function has returned. Asks and answers at once, and never waits.
 *
 * True here does not mean the thread is gone — it still has to be joined — only that nothing of the
 * caller's is running on it any more. The flag is raised after the function returns and read with the
 * matching acquire, so whatever that thread wrote before returning is visible once this says true.
 * */
NYA_API b8 nya_thread_is_finished(const NYA_Thread* thread) __attr_no_discard;

/**
 * Waits for `thread` to end and hands its record back to the arena it came from.
 *
 * The pointer is spent afterwards. Everything the thread wrote is visible to the joining thread once
 * this returns, which is the ordering nothing else here provides.
 * */
NYA_API void nya_thread_join(NYA_Thread* thread);

/**
 * Stops waiting for `thread`, leaving it to run and its record where it is.
 *
 * For a deadline that has passed with the thread still inside somebody's code, which is the one case
 * where waiting is not an option and killing it would be worse: see nya_system_http_deinit, which
 * closes the sockets and leaks the arena on purpose rather than free memory a live thread is writing.
 * */
NYA_API void nya_thread_abandon(NYA_Thread* thread);

// MUTEXES

/** A lock out of `arena`, unlocked. */
NYA_API NYA_Error nya_mutex_create(NYA_Arena* arena, OUT NYA_Mutex** out_mutex) __attr_no_discard;

/** Gives the lock back to its arena. Nothing may hold it. */
NYA_API void nya_mutex_destroy(NYA_Mutex* mutex);

/** Takes `mutex`, waiting for as long as that takes. Null is no lock; see the note at the top. */
NYA_API void nya_mutex_lock(NYA_Mutex* mutex);

/** Gives `mutex` back. The thread that took it is the one that may, and null is no lock here too. */
NYA_API void nya_mutex_unlock(NYA_Mutex* mutex);

// SEMAPHORES

/** A counting semaphore out of `arena`, holding `initial` tokens. */
NYA_API NYA_Error nya_semaphore_create(NYA_Arena* arena, u32 initial, OUT NYA_Semaphore** out_semaphore) __attr_no_discard;

/** Gives the semaphore back to its arena. Nothing may be waiting on it. */
NYA_API void nya_semaphore_destroy(NYA_Semaphore* semaphore);

/** Adds one token, waking one waiter if any are waiting. Never blocks, and needs no lock held. */
NYA_API void nya_semaphore_post(NYA_Semaphore* semaphore);

/** Takes one token, waiting for as long as that takes. */
NYA_API void nya_semaphore_wait(NYA_Semaphore* semaphore);

/**
 * Takes one token if one arrives within `timeout_ms`. False when the wait ran out, which is the
 * ordinary answer for a worker that had nothing to do and now gets to look around.
 * */
NYA_API b8 nya_semaphore_wait_timeout(NYA_Semaphore* semaphore, u32 timeout_ms) __attr_no_discard;

// THE MAIN THREAD

/**
 * Names the calling thread as the program's main one.
 *
 * Called from nya_app_init_with_options, and from nya_system_http_init for a program that serves
 * without a frame loop. Calling it twice from the same thread is a no-op; calling it from a second
 * thread moves the claim, which no caller should want and none does.
 *
 * Until something claims, every thread answers yes: a program with one thread must not pay for a rule
 * it cannot break, and a guard that fires in a program that never started a second thread would only
 * teach people to delete guards. After a claim exactly one thread answers yes and every other one
 * trips the assertion the first time it reaches guarded code, which is the point: a handler that
 * promised to stay off program state fails loudly the first time it runs rather than corrupting
 * something once a month.
 * */
NYA_API void nya_thread_main_claim(void);

/** Whether this is that thread, or nothing has claimed yet. */
NYA_API b8 nya_thread_main_is_current(void) __attr_no_discard;

/**
 * Crashes when it is not, naming `what` the caller was reaching for and which thread reached.
 *
 * The enforcement half of a "main thread only" note in a header: the note says which, this makes the
 * note true. Kept in release, like every other assertion here.
 * */
NYA_API void nya_thread_main_only(NYA_ConstCString what);
