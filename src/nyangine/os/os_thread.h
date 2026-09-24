/**
 * @file os_thread.h
 *
 * A thread, a mutex and a counting semaphore, as the operating system hands them over: the caller owns
 * the storage, a status says whether a call worked, and nothing here allocates or asserts.
 *
 * ```c
 * NYA_OsThreadStart start = { .function = drain, .data = &server };
 * NYA_OsThread      thread;
 *
 * if (nya_os_thread_spawn(&start, &thread) != NYA_OS_THREAD_OK) return false;
 * ...
 * (void)nya_os_thread_join(thread);
 * ```
 *
 * Everything the engine actually waits with — a thread that reports its own end, a lock a null pointer
 * turns off, a spawn that fails with an NYA_Error — is a record and a flag over these and lives once in
 * base/base_thread.h.
 *
 * ── a counting semaphore, not a condition variable ──
 *
 * The two things in the engine that wait, wait for a *count*: the HTTP worker pool sleeps until one more
 * exchange has been queued, and exactly one worker may take it. A counting semaphore is that number, and
 * a post needs no lock held, which is what lets the listener publish an exchange and wake a worker
 * without the queue's lock reaching across the wakeup. A condition variable would need a predicate, a
 * mutex at every wait and the same count spelled out beside it; the one thing it does better — waking
 * every waiter at once — the shutdown path already does by posting once per worker.
 *
 * ── why the caller holds the storage ──
 *
 * A mutex and a semaphore are objects the host wants somewhere in memory, and nothing here may allocate,
 * so each is an opaque block of words the caller places and the platform file static_asserts the host's
 * own type fits. The same reason shapes the spawn: a thread entry point takes one pointer and the host
 * decides its type, so the pair the engine means by "run this on that" — a function and its argument —
 * has to outlive the call somewhere, and that somewhere is the caller's NYA_OsThreadStart.
 * */
#pragma once

#include "nyangine/base/base_attributes.h"
#include "nyangine/base/base_types.h"

// CONSTANTS

/** A wait that only ends when the semaphore is posted. The same spelling nya_os_process_wait takes. */
#define NYA_OS_THREAD_WAIT_FOREVER ((u32)0xFFFFFFFF)

/**
 * Words each primitive's storage holds. Sixty four bytes covers every host this compiles for with room
 * to spare — glibc's pthread_mutex_t is 40 bytes on x86_64 and 48 on aarch64, its sem_t 32, and Windows'
 * CRITICAL_SECTION 40 — and the platform files static_assert what they actually put in there.
 * */
#define _NYA_OS_MUTEX_WORDS     8
#define _NYA_OS_SEMAPHORE_WORDS 8

// TYPES

typedef enum NYA_OsThreadStatus  NYA_OsThreadStatus;
typedef struct NYA_OsThread      NYA_OsThread;
typedef struct NYA_OsThreadStart NYA_OsThreadStart;
typedef struct NYA_OsMutex       NYA_OsMutex;
typedef struct NYA_OsSemaphore   NYA_OsSemaphore;

/** What a thread runs. Its argument is the `data` it was spawned with; what it returns is the caller's business, not the host's. */
typedef void (*NYA_OsThreadFn)(void* data);

/** How a call here ended. One shape for every function, since none of them knows what a failure means. */
enum NYA_OsThreadStatus {
    /** It worked, and whatever it was asked for is in the out parameters. */
    NYA_OS_THREAD_OK,
    /** The wait ran out before the semaphore was posted. Not a failure: nothing happened yet. */
    NYA_OS_THREAD_TIMEOUT,
    /** The operating system refused. Which is for the caller above to make sense of. */
    NYA_OS_THREAD_FAILED,
    NYA_OS_THREAD_STATUS_COUNT,
};

/** A running thread. The pthread_t on Linux, the thread HANDLE cast to this on Windows. */
struct NYA_OsThread {
    u64 handle;
};

/**
 * What a spawned thread is to run, and with what. The caller keeps it alive until that thread has ended,
 * because the host's entry point is handed its address rather than a copy.
 * */
struct NYA_OsThreadStart {
    NYA_OsThreadFn function;
    void*          data;
};

/** A mutex, in the caller's storage. Opaque: only os_thread_*.c may name what is in it. */
struct NYA_OsMutex {
    u64 storage[_NYA_OS_MUTEX_WORDS];
};

/** A counting semaphore, in the caller's storage. Opaque for the same reason. */
struct NYA_OsSemaphore {
    u64 storage[_NYA_OS_SEMAPHORE_WORDS];
};

// FUNCTIONS

// THREADS

/**
 * Starts `start->function` on a thread of its own and returns as soon as it is running.
 *
 * The thread has to be joined or abandoned, on both targets: neither host reclaims a thread's last
 * resources while something could still be waiting for it.
 * */
NYA_API NYA_OsThreadStatus nya_os_thread_spawn(NYA_OsThreadStart* start, OUT NYA_OsThread* out_thread) __attr_no_discard;

/**
 * Waits for `thread` to end and releases it. The handle is spent afterwards.
 *
 * FAILED when the host refused, which is what joining the calling thread or a thread that was already
 * joined or abandoned gets. A handle that never came from a spawn here is not a question either host
 * will answer — Linux reads the descriptor it is given — so it is the one thing a caller may not do.
 * */
NYA_API NYA_OsThreadStatus nya_os_thread_join(NYA_OsThread thread) __attr_no_discard;

/**
 * Stops waiting for `thread` and lets the host reclaim it whenever it ends. The handle is spent too.
 *
 * For the one case where waiting is not an option: a shutdown deadline has passed and the thread is
 * still inside somebody's handler. Killing it there would be worse than the problem — it may hold a
 * lock, or be halfway through a write — so it is let go of instead, and whatever it still writes into
 * has to outlive it.
 * */
NYA_API void nya_os_thread_abandon(NYA_OsThread thread);

/**
 * A number no other thread running right now shares: pthread_self on Linux, GetCurrentThreadId on Windows.
 *
 * Only for telling threads apart — in a log line, a trace row, a per-thread table. It is reused once a
 * thread has ended, means nothing across processes, and is not the number Linux shows in `top -H`: the
 * kernel's tid needs a GNU extension the engine does not ask its headers for.
 * */
NYA_API u64 nya_os_thread_id_current(void) __attr_no_discard;

/**
 * Labels the calling thread for a debugger, `top -H` and a trace.
 *
 * Best effort and deliberately not fallible: Linux truncates to 15 bytes, and Windows does nothing at
 * all, because its call is Windows 10 1607 and a wide string and this build targets older than that.
 * Nothing may depend on the name, which is why it is set from inside the thread and never read back.
 * */
NYA_API void nya_os_thread_name_set(NYA_ConstCString name);

// MUTEXES

/**
 * Prepares `mutex` for use. Not recursive: a thread that locks one twice deadlocks against itself.
 *
 * FAILED leaves it untouched and unusable, which is the whole reason this is checked rather than
 * assumed — a mutex nobody noticed was missing makes every lock over it a no-op.
 * */
NYA_API NYA_OsThreadStatus nya_os_mutex_init(OUT NYA_OsMutex* mutex) __attr_no_discard;

/** Releases what the host holds for `mutex`. Locking it afterwards is undefined; the storage is the caller's again. */
NYA_API void nya_os_mutex_deinit(NYA_OsMutex* mutex);

/** Takes `mutex`, waiting for as long as that takes. */
NYA_API void nya_os_mutex_lock(NYA_OsMutex* mutex);

/** Gives `mutex` back. The thread that took it is the one that may. */
NYA_API void nya_os_mutex_unlock(NYA_OsMutex* mutex);

// SEMAPHORES

/** Prepares `semaphore` with `initial` tokens in it. Shared between the threads of one process and no further. */
NYA_API NYA_OsThreadStatus nya_os_semaphore_init(OUT NYA_OsSemaphore* semaphore, u32 initial) __attr_no_discard;

/** Releases what the host holds for `semaphore`. Nothing may be waiting on it. */
NYA_API void nya_os_semaphore_deinit(NYA_OsSemaphore* semaphore);

/** Adds one token, waking one waiter if any are waiting. Never blocks, and needs no lock held. */
NYA_API void nya_os_semaphore_post(NYA_OsSemaphore* semaphore);

/**
 * Takes one token, waiting up to `timeout_ms` for one to appear. Zero asks and answers at once,
 * NYA_OS_THREAD_WAIT_FOREVER waits until there is one.
 *
 * TIMEOUT when the wait ran out, which is the ordinary answer for a worker that had nothing to do. A
 * signal never shortens the wait: it is retried until the deadline, so "at least this long" holds. The
 * deadline is the wall clock's on Linux, so a clock that is set during a wait moves it; the callers
 * here use the timeout as a wakeup bound and not as a promise, which is what makes that acceptable.
 * */
NYA_API NYA_OsThreadStatus nya_os_semaphore_wait(NYA_OsSemaphore* semaphore, u32 timeout_ms) __attr_no_discard;
