/**
 * Starting a thread, joining it, letting go of one, and the two things threads wait on each other with.
 *
 * The primitives only: the record in an arena, the finished flag and the errors that base_thread.h
 * builds over them are covered by tests/nyangine/base/test_thread.c.
 **/

#include "nyangine-core/nyangine.c"
#include "nyangine-core/nyangine.h"

/** Threads started at once by the contention test. Past the core count on purpose, so some of them wait. */
#define THREAD_COUNT 8

/** Increments each of those threads makes. Enough that an unsynchronized total would be visibly wrong. */
#define INCREMENTS_PER_THREAD 2000

/** How long a wait that is meant to run out is given. Short, because the whole test waits it out twice. */
#define TIMEOUT_MS 20

/** What the threads in this file share, since a thread here is given exactly one pointer. */
typedef struct {
    NYA_OsMutex     mutex;
    NYA_OsSemaphore semaphore;

    /** Guarded by the mutex above, and deliberately not atomic: it is what proves the mutex works. */
    u64 counter;

    atomic u64 started;
    atomic u64 id_seen;
} Shared;

static void count_under_the_mutex(void* data) {
    Shared* shared = (Shared*)data;

    (void)atomic_fetch_add(&shared->started, 1);

    for (u32 i = 0; i < INCREMENTS_PER_THREAD; i++) {
        nya_os_mutex_lock(&shared->mutex);
        shared->counter++;
        nya_os_mutex_unlock(&shared->mutex);
    }
}

static void report_its_own_id(void* data) {
    Shared* shared = (Shared*)data;

    nya_os_thread_name_set("os_thread_test");

    atomic_store(&shared->id_seen, nya_os_thread_id_current());
}

static void post_once(void* data) {
    Shared* shared = (Shared*)data;

    nya_os_semaphore_post(&shared->semaphore);
}

s32 main(void) {
    // A thread runs what it was given, with what it was given, and joining it waits for that.
    {
        Shared shared = { 0 };

        nya_check(nya_os_thread_id_current() != 0, "this thread should have an id");
        nya_check(nya_os_thread_id_current() == nya_os_thread_id_current(), "the id should not change between calls");

        NYA_OsThreadStart start  = { .function = report_its_own_id, .data = &shared };
        NYA_OsThread      thread = { 0 };

        nya_check(nya_os_thread_spawn(&start, &thread) == NYA_OS_THREAD_OK, "spawning a thread should succeed");
        nya_check(nya_os_thread_join(thread) == NYA_OS_THREAD_OK, "joining it should succeed");

        u64 seen = atomic_load(&shared.id_seen);
        nya_check(seen != 0, "the thread should have reported an id");
        nya_check(seen != nya_os_thread_id_current(), "the thread's id should not be this thread's");
    }

    // A mutex serializes what would otherwise be a lost update, under real contention.
    {
        Shared shared = { 0 };
        nya_check(nya_os_mutex_init(&shared.mutex) == NYA_OS_THREAD_OK, "a mutex should be makeable");

        NYA_OsThreadStart start = { .function = count_under_the_mutex, .data = &shared };

        NYA_OsThread threads[THREAD_COUNT] = { 0 };
        for (u32 i = 0; i < THREAD_COUNT; i++) {
            nya_check(nya_os_thread_spawn(&start, &threads[i]) == NYA_OS_THREAD_OK, "spawning thread %u should succeed", i);
        }

        for (u32 i = 0; i < THREAD_COUNT; i++) nya_check(nya_os_thread_join(threads[i]) == NYA_OS_THREAD_OK, "joining thread %u should succeed", i);

        nya_check(atomic_load(&shared.started) == THREAD_COUNT, "every thread should have run");
        nya_check(shared.counter == (u64)THREAD_COUNT * INCREMENTS_PER_THREAD, "the count should be exact, got " FMTu64, shared.counter);

        nya_os_mutex_deinit(&shared.mutex);
    }

    // A semaphore hands out exactly the tokens it was given, and waiting for one that never comes ends.
    {
        Shared shared = { 0 };
        nya_check(nya_os_semaphore_init(&shared.semaphore, 2) == NYA_OS_THREAD_OK, "a semaphore should be makeable");

        nya_check(nya_os_semaphore_wait(&shared.semaphore, 0) == NYA_OS_THREAD_OK, "the first token should be there at once");
        nya_check(nya_os_semaphore_wait(&shared.semaphore, 0) == NYA_OS_THREAD_OK, "and so should the second");
        nya_check(nya_os_semaphore_wait(&shared.semaphore, 0) == NYA_OS_THREAD_TIMEOUT, "a third should not be");

        u64 before = nya_os_time_monotonic_ns();
        nya_check(nya_os_semaphore_wait(&shared.semaphore, TIMEOUT_MS) == NYA_OS_THREAD_TIMEOUT, "an empty semaphore should time out");

        // only that it waited at all: how much longer than asked is the scheduler's business.
        u64 waited_ms = (nya_os_time_monotonic_ns() - before) / 1'000'000ULL;
        nya_check(waited_ms + 1 >= TIMEOUT_MS, "the wait should have lasted the timeout, took " FMTu64 " ms", waited_ms);

        nya_os_semaphore_post(&shared.semaphore);
        nya_check(nya_os_semaphore_wait(&shared.semaphore, TIMEOUT_MS) == NYA_OS_THREAD_OK, "a posted token should be taken");

        nya_os_semaphore_deinit(&shared.semaphore);
    }

    // A wait with no timeout ends when another thread posts, which is what the worker pools do.
    {
        Shared shared = { 0 };
        nya_check(nya_os_semaphore_init(&shared.semaphore, 0) == NYA_OS_THREAD_OK, "a semaphore should be makeable");

        NYA_OsThreadStart start  = { .function = post_once, .data = &shared };
        NYA_OsThread      thread = { 0 };
        nya_check(nya_os_thread_spawn(&start, &thread) == NYA_OS_THREAD_OK, "spawning the poster should succeed");

        nya_check(nya_os_semaphore_wait(&shared.semaphore, NYA_OS_THREAD_WAIT_FOREVER) == NYA_OS_THREAD_OK, "the post should end the wait");

        nya_check(nya_os_thread_join(thread) == NYA_OS_THREAD_OK, "joining the poster should succeed");
        nya_os_semaphore_deinit(&shared.semaphore);
    }

    /* A thread can be let go of instead of joined. Abandoned after its post has been taken, so this process does not end while it is still running: what is under test is that letting go of a thread is allowed and leaks nothing, not what happens to a program that walks away from live work. */
    {
        Shared shared = { 0 };
        nya_check(nya_os_semaphore_init(&shared.semaphore, 0) == NYA_OS_THREAD_OK, "a semaphore should be makeable");

        NYA_OsThreadStart start  = { .function = post_once, .data = &shared };
        NYA_OsThread      thread = { 0 };
        nya_check(nya_os_thread_spawn(&start, &thread) == NYA_OS_THREAD_OK, "spawning the poster should succeed");

        nya_check(nya_os_semaphore_wait(&shared.semaphore, NYA_OS_THREAD_WAIT_FOREVER) == NYA_OS_THREAD_OK, "the post should end the wait");

        nya_os_thread_abandon(thread);
        nya_os_semaphore_deinit(&shared.semaphore);
    }

    return nya_check_failures() == 0 ? 0 : 1;
}
