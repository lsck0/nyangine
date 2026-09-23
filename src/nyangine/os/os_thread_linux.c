#include "nyangine/os/os_thread.h"

// after the engine's own header: base_basic.h asks for POSIX 2008, and these only declare what it
// wants once they have seen that.
#include <pthread.h>
#include <semaphore.h>
#include <sys/prctl.h>

static_assert(sizeof(pthread_t) <= sizeof(u64), "NYA_OsThread must hold a pthread_t");
static_assert(sizeof(pthread_mutex_t) <= sizeof(((NYA_OsMutex*)nullptr)->storage), "NYA_OsMutex must hold a pthread_mutex_t");
static_assert(alignof(pthread_mutex_t) <= alignof(u64), "NYA_OsMutex's storage must be aligned like a pthread_mutex_t");
static_assert(sizeof(sem_t) <= sizeof(((NYA_OsSemaphore*)nullptr)->storage), "NYA_OsSemaphore must hold a sem_t");
static_assert(alignof(sem_t) <= alignof(u64), "NYA_OsSemaphore's storage must be aligned like a sem_t");

/*
 * ─────────────────────────────────────────────────────────
 * INTERNAL
 * ─────────────────────────────────────────────────────────
 */

/**
 * What pthread_create is actually given, since it takes a function returning void* and the engine's
 * threads return nothing. Casting the caller's function to that shape instead would be a call through
 * the wrong prototype, which is undefined and which the sanitized build traps on.
 * */
NYA_INTERNAL void* _nya_os_thread_entry(void* start) {
    NYA_OsThreadStart* started = (NYA_OsThreadStart*)start;

    started->function(started->data);

    return nullptr;
}

/** The two halves of the caller's storage, named once so nothing else casts it. */
NYA_INTERNAL pthread_mutex_t* _nya_os_mutex_handle(NYA_OsMutex* mutex) {
    return (pthread_mutex_t*)mutex->storage;
}

NYA_INTERNAL sem_t* _nya_os_semaphore_handle(NYA_OsSemaphore* semaphore) {
    return (sem_t*)semaphore->storage;
}

/*
 * ─────────────────────────────────────────────────────────
 * THREADS
 * ─────────────────────────────────────────────────────────
 */

NYA_OsThreadStatus nya_os_thread_spawn(NYA_OsThreadStart* start, OUT NYA_OsThread* out_thread) {
    pthread_t thread = 0;

    // the default attributes, which is an 8 MB stack here; a thread that needs a different one has a
    // reason this layer cannot know.
    if (pthread_create(&thread, nullptr, _nya_os_thread_entry, start) != 0) return NYA_OS_THREAD_FAILED;

    out_thread->handle = (u64)(uintptr_t)thread;

    return NYA_OS_THREAD_OK;
}

NYA_OsThreadStatus nya_os_thread_join(NYA_OsThread thread) {
    if (pthread_join((pthread_t)(uintptr_t)thread.handle, nullptr) != 0) return NYA_OS_THREAD_FAILED;

    return NYA_OS_THREAD_OK;
}

void nya_os_thread_abandon(NYA_OsThread thread) {
    (void)pthread_detach((pthread_t)(uintptr_t)thread.handle);
}

u64 nya_os_thread_id_current(void) {
    return (u64)(uintptr_t)pthread_self();
}

/*
 * prctl rather than pthread_setname_np, which is a GNU extension and would need _GNU_SOURCE for the
 * whole engine. Both end up in the same place — the kernel's 16 byte comm field — and prctl names the
 * calling thread, which is the only one this is called for.
 */
void nya_os_thread_name_set(NYA_ConstCString name) {
    char truncated[16] = { 0 };

    (void)snprintf(truncated, sizeof(truncated), "%s", name);
    (void)prctl(PR_SET_NAME, truncated, 0, 0, 0);
}

/*
 * ─────────────────────────────────────────────────────────
 * MUTEXES
 * ─────────────────────────────────────────────────────────
 */

NYA_OsThreadStatus nya_os_mutex_init(OUT NYA_OsMutex* mutex) {
    if (pthread_mutex_init(_nya_os_mutex_handle(mutex), nullptr) != 0) return NYA_OS_THREAD_FAILED;

    return NYA_OS_THREAD_OK;
}

void nya_os_mutex_deinit(NYA_OsMutex* mutex) {
    (void)pthread_mutex_destroy(_nya_os_mutex_handle(mutex));
}

void nya_os_mutex_lock(NYA_OsMutex* mutex) {
    (void)pthread_mutex_lock(_nya_os_mutex_handle(mutex));
}

void nya_os_mutex_unlock(NYA_OsMutex* mutex) {
    (void)pthread_mutex_unlock(_nya_os_mutex_handle(mutex));
}

/*
 * ─────────────────────────────────────────────────────────
 * SEMAPHORES
 * ─────────────────────────────────────────────────────────
 */

NYA_OsThreadStatus nya_os_semaphore_init(OUT NYA_OsSemaphore* semaphore, u32 initial) {
    // zero: shared between the threads of this process and no further, which is what every caller here
    // means and what keeps it out of the file system's namespace.
    if (sem_init(_nya_os_semaphore_handle(semaphore), 0, (unsigned int)initial) != 0) return NYA_OS_THREAD_FAILED;

    return NYA_OS_THREAD_OK;
}

void nya_os_semaphore_deinit(NYA_OsSemaphore* semaphore) {
    (void)sem_destroy(_nya_os_semaphore_handle(semaphore));
}

void nya_os_semaphore_post(NYA_OsSemaphore* semaphore) {
    (void)sem_post(_nya_os_semaphore_handle(semaphore));
}

NYA_OsThreadStatus nya_os_semaphore_wait(NYA_OsSemaphore* semaphore, u32 timeout_ms) {
    sem_t* handle = _nya_os_semaphore_handle(semaphore);

    if (timeout_ms == NYA_OS_THREAD_WAIT_FOREVER) {
        while (sem_wait(handle) != 0) {
            if (errno != EINTR) return NYA_OS_THREAD_FAILED;
        }

        return NYA_OS_THREAD_OK;
    }

    if (timeout_ms == 0) {
        if (sem_trywait(handle) == 0) return NYA_OS_THREAD_OK;

        return errno == EAGAIN ? NYA_OS_THREAD_TIMEOUT : NYA_OS_THREAD_FAILED;
    }

    /*
     * sem_timedwait takes an absolute CLOCK_REALTIME deadline, so the deadline is computed once and
     * waited toward: a signal cuts the wait short and retrying with the same absolute time is what
     * keeps "at least this long" true. sem_clockwait would take the monotonic clock instead and is a
     * GNU extension the engine does not ask its headers for; see the header for why that is bearable.
     */
    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);

    deadline.tv_sec  += (time_t)(timeout_ms / 1'000U);
    deadline.tv_nsec += (long)(timeout_ms % 1'000U) * 1'000'000L;

    if (deadline.tv_nsec >= 1'000'000'000L) {
        deadline.tv_sec  += 1;
        deadline.tv_nsec -= 1'000'000'000L;
    }

    while (sem_timedwait(handle, &deadline) != 0) {
        if (errno == EINTR) continue;

        return errno == ETIMEDOUT ? NYA_OS_THREAD_TIMEOUT : NYA_OS_THREAD_FAILED;
    }

    return NYA_OS_THREAD_OK;
}
