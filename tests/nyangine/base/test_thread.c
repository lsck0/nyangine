/**
 * The arena's half of threading: a record that reports its own end, locks that a null pointer turns
 * off, a semaphore two threads hand work over with, and the main thread's claim.
 *
 * The host's primitives underneath are tests/nyangine/os/test_thread.c's.
 **/

#include "nyangine-core/nyangine.c"
#include "nyangine-core/nyangine.h"

/** Threads the handover test starts. One more than the tokens posted, so one of them waits in vain. */
#define WORKER_COUNT 4

/** How long a worker waits for work before looking at the stop flag again. */
#define WORKER_WAIT_MS 10

/** How long a test waits for something another thread must do before calling it stuck. */
#define PATIENCE_MS 2000

/** What the threads here share, since a thread is given exactly one pointer. */
typedef struct {
    NYA_Semaphore* work;
    atomic u32     taken;
    atomic b8      stopping;

    /** Raised by the thread under test and read by the test, to prove the thread ran at all. */
    atomic b8 ran;
} Shared;

static void raise_the_flag(void* data) {
    Shared* shared = (Shared*)data;

    atomic_store(&shared->ran, true);
}

static void take_work(void* data) {
    Shared* shared = (Shared*)data;

    while (!atomic_load_explicit(&shared->stopping, memory_order_acquire)) {
        if (!nya_semaphore_wait_timeout(shared->work, WORKER_WAIT_MS)) continue;

        (void)atomic_fetch_add(&shared->taken, 1);
    }
}

/** Both waits below are bounded, so a build where the flag never rises fails rather than hangs. */
static b8 wait_until_finished(const NYA_Thread* thread) {
    u64 deadline_ns = nya_clock_get_monotonic_ns() + ((u64)PATIENCE_MS * 1'000'000ULL);

    while (!nya_thread_is_finished(thread) && nya_clock_get_monotonic_ns() < deadline_ns) nya_os_time_sleep_ms(1);

    return nya_thread_is_finished(thread);
}

static b8 wait_until_taken(const Shared* shared, u32 count) {
    u64 deadline_ns = nya_clock_get_monotonic_ns() + ((u64)PATIENCE_MS * 1'000'000ULL);

    while (atomic_load(&shared->taken) < count && nya_clock_get_monotonic_ns() < deadline_ns) nya_os_time_sleep_ms(1);

    return atomic_load(&shared->taken) == count;
}

s32 main(void) {
    NYA_Arena* arena = nya_arena_create(.name = "test_thread");

    // A thread runs, says so, and says it has finished before it is joined.
    {
        Shared shared = { 0 };

        NYA_Thread* thread = nullptr;
        nya_check(nya_thread_spawn(arena, raise_the_flag, &shared, "Flag", &thread).ok, "spawning a thread should succeed");

        nya_check(wait_until_finished(thread), "the thread should report itself finished");
        nya_check(atomic_load(&shared.ran), "the thread should have run its function");

        nya_thread_join(thread);
    }

    // A semaphore hands work to whichever worker is free, and a wait that ran out is not work.
    {
        Shared shared = { 0 };
        nya_check(nya_semaphore_create(arena, 0, &shared.work).ok, "a semaphore should be makeable");

        NYA_Thread* workers[WORKER_COUNT] = { 0 };
        for (u32 i = 0; i < WORKER_COUNT; i++) {
            nya_check(nya_thread_spawn(arena, take_work, &shared, "Worker", &workers[i]).ok, "spawning worker %u should succeed", i);
        }

        for (u32 i = 0; i < WORKER_COUNT - 1; i++) nya_semaphore_post(shared.work);

        nya_check(wait_until_taken(&shared, WORKER_COUNT - 1), "every posted token should be taken exactly once");

        atomic_store_explicit(&shared.stopping, true, memory_order_release);

        // one post each, the way nya_system_http_deinit wakes a pool that is asleep on its timeout.
        for (u32 i = 0; i < WORKER_COUNT; i++) nya_semaphore_post(shared.work);
        for (u32 i = 0; i < WORKER_COUNT; i++) nya_thread_join(workers[i]);

        // one round of posts to hand work over and one to wake the pool, so that is every token there was.
        nya_check(atomic_load(&shared.taken) <= (WORKER_COUNT * 2) - 1, "no worker should have taken a token that was never posted");

        nya_semaphore_destroy(shared.work);
    }

    // A blocking wait ends on a post, and a lock is a lock.
    {
        NYA_Semaphore* semaphore = nullptr;
        nya_check(nya_semaphore_create(arena, 1, &semaphore).ok, "a semaphore should be makeable");

        nya_semaphore_wait(semaphore);
        nya_semaphore_post(semaphore);
        nya_semaphore_wait(semaphore);

        nya_semaphore_destroy(semaphore);

        NYA_Mutex* mutex = nullptr;
        nya_check(nya_mutex_create(arena, &mutex).ok, "a mutex should be makeable");

        nya_mutex_lock(mutex);
        nya_mutex_unlock(mutex);

        nya_mutex_destroy(mutex);
    }

    /* A null lock is no lock at all. What the unthreaded HTTP server runs on: the same call sites, with nothing behind them. It has to be a no-op rather than a crash or nothing in that mode would answer a request at all. */
    {
        nya_mutex_lock(nullptr);
        nya_mutex_unlock(nullptr);
        nya_mutex_destroy(nullptr);
        nya_semaphore_destroy(nullptr);
    }

    /* A thread can be let go of instead of joined. After it has finished, so nothing is still writing into the record when the arena goes: that is the deal nya_thread_abandon documents, and the case it exists for keeps its arena forever. */
    {
        Shared shared = { 0 };

        NYA_Thread* thread = nullptr;
        nya_check(nya_thread_spawn(arena, raise_the_flag, &shared, "Abandoned", &thread).ok, "spawning a thread should succeed");

        nya_check(wait_until_finished(thread), "the thread should report itself finished");

        nya_thread_abandon(thread);
    }

    // The main thread claim answers for the thread that made it and for no other.
    {
        nya_check(nya_thread_main_is_current(), "before any claim every thread is the main one");

        nya_thread_main_claim();
        nya_check(nya_thread_main_is_current(), "the claiming thread is the main one");

        nya_thread_main_only("this test");
    }

    nya_arena_destroy(arena);

    return nya_check_failures() == 0 ? 0 : 1;
}
