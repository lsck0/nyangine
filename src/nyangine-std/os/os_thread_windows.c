#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "nyangine-std/os/os_thread.h"

static_assert(sizeof(HANDLE) <= sizeof(u64), "NYA_OsThread must hold a thread HANDLE");
static_assert(sizeof(SRWLOCK) <= sizeof(((NYA_OsMutex*)nullptr)->storage), "NYA_OsMutex must hold an SRWLOCK");
static_assert(alignof(SRWLOCK) <= alignof(u64), "NYA_OsMutex's storage must be aligned like an SRWLOCK");
static_assert(sizeof(HANDLE) <= sizeof(((NYA_OsSemaphore*)nullptr)->storage), "NYA_OsSemaphore must hold a semaphore HANDLE");

// INTERNAL

/**
 * What CreateThread is actually given, since it takes a DWORD WINAPI function and the engine's threads
 * return nothing. Casting the caller's function to that shape instead would be a call through the wrong
 * prototype, which is undefined whatever the calling convention happens to agree on.
 * */
NYA_INTERNAL DWORD WINAPI _nya_os_thread_entry(LPVOID start) {
    NYA_OsThreadStart* started = (NYA_OsThreadStart*)start;

    started->function(started->data);

    return 0;
}

/**
 * An SRWLOCK rather than a CRITICAL_SECTION, which is recursive: the same lock has to behave the same
 * way on both targets, and a plain pthread mutex deadlocks a thread that takes it twice. A lock that
 * forgave that here would hide the mistake until it ran on Linux.
 * */
NYA_INTERNAL SRWLOCK* _nya_os_mutex_handle(NYA_OsMutex* mutex) {
    return (SRWLOCK*)mutex->storage;
}

NYA_INTERNAL HANDLE _nya_os_semaphore_handle(const NYA_OsSemaphore* semaphore) {
    return (HANDLE)(intptr_t)semaphore->storage[0];
}

// THREADS

NYA_OsThreadStatus nya_os_thread_spawn(NYA_OsThreadStart* start, OUT NYA_OsThread* out_thread) {
    // Zero stack size takes the image's 1 MB default; CreateThread not _beginthreadex, since the CRT is a DLL that cleans its own per-thread state on detach.
    HANDLE thread = CreateThread(nullptr, 0, _nya_os_thread_entry, start, 0, nullptr);

    if (thread == nullptr) return NYA_OS_THREAD_FAILED;

    out_thread->handle = (u64)(intptr_t)thread;

    return NYA_OS_THREAD_OK;
}

NYA_OsThreadStatus nya_os_thread_join(NYA_OsThread thread) {
    HANDLE handle = (HANDLE)(intptr_t)thread.handle;

    if (WaitForSingleObject(handle, INFINITE) != WAIT_OBJECT_0) return NYA_OS_THREAD_FAILED;

    // the wait says it has ended; the handle is what still has to go, or the thread object stays.
    if (!CloseHandle(handle)) return NYA_OS_THREAD_FAILED;

    return NYA_OS_THREAD_OK;
}

// Closing the handle is the whole of it: Windows frees a thread's object when its last handle closes, so letting go of the handle detaches the thread.
void nya_os_thread_abandon(NYA_OsThread thread) {
    (void)CloseHandle((HANDLE)(intptr_t)thread.handle);
}

u64 nya_os_thread_id_current(void) {
    return (u64)GetCurrentThreadId();
}

// Nothing: SetThreadDescription is Windows 10 1607 and this build targets older; the name is a debugger label nothing may depend on, so leaving it unset is honest.
void nya_os_thread_name_set(NYA_ConstCString name) {
    (void)name;
}

// MUTEXES

NYA_OsThreadStatus nya_os_mutex_init(OUT NYA_OsMutex* mutex) {
    InitializeSRWLock(_nya_os_mutex_handle(mutex));

    return NYA_OS_THREAD_OK;
}

// An SRWLOCK is one pointer-sized word with nothing of the system's to give back; this exists only to pair with the Linux pthread_mutex_destroy.
void nya_os_mutex_deinit(NYA_OsMutex* mutex) {
    (void)mutex;
}

void nya_os_mutex_lock(NYA_OsMutex* mutex) {
    AcquireSRWLockExclusive(_nya_os_mutex_handle(mutex));
}

void nya_os_mutex_unlock(NYA_OsMutex* mutex) {
    ReleaseSRWLockExclusive(_nya_os_mutex_handle(mutex));
}

// SEMAPHORES

NYA_OsThreadStatus nya_os_semaphore_init(OUT NYA_OsSemaphore* semaphore, u32 initial) {
    // Unnamed, so it is this process's and never collides with another program's; MAXLONG because the count is bounded by what the caller queues.
    HANDLE handle = CreateSemaphoreA(nullptr, (LONG)initial, MAXLONG, nullptr);

    if (handle == nullptr) return NYA_OS_THREAD_FAILED;

    semaphore->storage[0] = (u64)(intptr_t)handle;

    return NYA_OS_THREAD_OK;
}

void nya_os_semaphore_deinit(NYA_OsSemaphore* semaphore) {
    (void)CloseHandle(_nya_os_semaphore_handle(semaphore));

    semaphore->storage[0] = 0;
}

void nya_os_semaphore_post(NYA_OsSemaphore* semaphore) {
    (void)ReleaseSemaphore(_nya_os_semaphore_handle(semaphore), 1, nullptr);
}

// INFINITE (0xFFFFFFFF) is NYA_OS_THREAD_WAIT_FOREVER and a zero timeout answers at once, so the two special values need no branch; a wait is never cut short by a signal.
NYA_OsThreadStatus nya_os_semaphore_wait(NYA_OsSemaphore* semaphore, u32 timeout_ms) {
    DWORD waited = WaitForSingleObject(_nya_os_semaphore_handle(semaphore), (DWORD)timeout_ms);

    if (waited == WAIT_OBJECT_0) return NYA_OS_THREAD_OK;
    if (waited == WAIT_TIMEOUT) return NYA_OS_THREAD_TIMEOUT;

    return NYA_OS_THREAD_FAILED;
}
