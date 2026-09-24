#include "nyangine/base/base_assert.h"
#include "nyangine/base/base_basic.h"
#include "nyangine/base/base_thread.h"
#include "nyangine/os/os_thread.h"

// TYPES

struct NYA_Thread {
    /** Where the record came from and where nya_thread_join hands it back. */
    NYA_Arena* arena;

    NYA_OsThread os_thread;

    /**
     * What the host's entry point is handed, which is this record and never the caller's function: the
     * name has to be set from inside the thread and the flag below has to be raised there too.
     * */
    NYA_OsThreadStart start;

    NYA_ThreadFn     function;
    void*            data;
    NYA_ConstCString name;

    /** Raised by the thread as it returns, so another thread can ask without waiting. */
    atomic b8 finished;
};

struct NYA_Mutex {
    NYA_Arena*  arena;
    NYA_OsMutex os_mutex;
};

struct NYA_Semaphore {
    NYA_Arena*      arena;
    NYA_OsSemaphore os_semaphore;
};

// STATE

/** True on exactly one thread, because only that thread ever runs the store. */
NYA_INTERNAL thread_local b8 _nya_thread_is_main = false;

/** Whether anybody has claimed at all. Read from every thread, so it is the atomic one. */
NYA_INTERNAL atomic b8 _nya_thread_main_claimed = false;

/** Who claimed, so a guard that fires can say which thread it was rather than only that it was not. */
NYA_INTERNAL atomic u64 _nya_thread_main_id = 0;

// PRIVATE API IMPLEMENTATION

/**
 * Every thread starts here: it names itself, runs what it was spawned for, and raises the flag on its
 * way out. Released, so a thread that sees the flag also sees everything this one wrote before it.
 * */
NYA_INTERNAL void _nya_thread_entry(void* data) {
    NYA_Thread* thread = (NYA_Thread*)data;

    nya_os_thread_name_set(thread->name);

    thread->function(thread->data);

    atomic_store_explicit(&thread->finished, true, memory_order_release);
}

// PUBLIC API IMPLEMENTATION

// THREADS

NYA_Error nya_thread_spawn(NYA_Arena* arena, NYA_ThreadFn function, void* data, NYA_ConstCString name, OUT NYA_Thread** out_thread) {
    nya_assert(arena != nullptr);
    nya_assert(function != nullptr);
    nya_assert(name != nullptr);
    nya_assert(out_thread != nullptr);

    NYA_Thread* thread = nya_arena_alloc(arena, sizeof(NYA_Thread));

    *thread = (NYA_Thread){
        .arena    = arena,
        .function = function,
        .data     = data,
        .name     = name,
    };

    // the record is complete before the thread exists, because the first thing that thread does is read it.
    thread->start = (NYA_OsThreadStart){ .function = _nya_thread_entry, .data = thread };

    if (nya_os_thread_spawn(&thread->start, &thread->os_thread) != NYA_OS_THREAD_OK) {
        nya_arena_free(arena, thread, sizeof(NYA_Thread));

        return nya_error(NYA_ERROR_NOT_OK, "the host refused to start the '%s' thread", name);
    }

    *out_thread = thread;

    return NYA_OK;
}

b8 nya_thread_is_finished(const NYA_Thread* thread) {
    nya_assert(thread != nullptr);

    return atomic_load_explicit(&thread->finished, memory_order_acquire);
}

void nya_thread_join(NYA_Thread* thread) {
    nya_assert(thread != nullptr);

    NYA_OsThreadStatus status = nya_os_thread_join(thread->os_thread);
    nya_assert(status == NYA_OS_THREAD_OK, "the '%s' thread could not be joined, which means this handle was never its own", thread->name);

    nya_arena_free(thread->arena, thread, sizeof(NYA_Thread));
}

void nya_thread_abandon(NYA_Thread* thread) {
    nya_assert(thread != nullptr);

    // the record stays where it is: the thread is still running and still writes its flag into it.
    nya_os_thread_abandon(thread->os_thread);
}

// MUTEXES

NYA_Error nya_mutex_create(NYA_Arena* arena, OUT NYA_Mutex** out_mutex) {
    nya_assert(arena != nullptr);
    nya_assert(out_mutex != nullptr);

    NYA_Mutex* mutex = nya_arena_alloc(arena, sizeof(NYA_Mutex));

    mutex->arena = arena;

    if (nya_os_mutex_init(&mutex->os_mutex) != NYA_OS_THREAD_OK) {
        nya_arena_free(arena, mutex, sizeof(NYA_Mutex));

        return nya_error(NYA_ERROR_OUT_OF_MEMORY, "the host refused to make a mutex");
    }

    *out_mutex = mutex;

    return NYA_OK;
}

void nya_mutex_destroy(NYA_Mutex* mutex) {
    if (mutex == nullptr) return;

    nya_os_mutex_deinit(&mutex->os_mutex);
    nya_arena_free(mutex->arena, mutex, sizeof(NYA_Mutex));
}

void nya_mutex_lock(NYA_Mutex* mutex) {
    if (mutex == nullptr) return;

    nya_os_mutex_lock(&mutex->os_mutex);
}

void nya_mutex_unlock(NYA_Mutex* mutex) {
    if (mutex == nullptr) return;

    nya_os_mutex_unlock(&mutex->os_mutex);
}

// SEMAPHORES

NYA_Error nya_semaphore_create(NYA_Arena* arena, u32 initial, OUT NYA_Semaphore** out_semaphore) {
    nya_assert(arena != nullptr);
    nya_assert(out_semaphore != nullptr);

    NYA_Semaphore* semaphore = nya_arena_alloc(arena, sizeof(NYA_Semaphore));

    semaphore->arena = arena;

    if (nya_os_semaphore_init(&semaphore->os_semaphore, initial) != NYA_OS_THREAD_OK) {
        nya_arena_free(arena, semaphore, sizeof(NYA_Semaphore));

        return nya_error(NYA_ERROR_OUT_OF_MEMORY, "the host refused to make a semaphore");
    }

    *out_semaphore = semaphore;

    return NYA_OK;
}

void nya_semaphore_destroy(NYA_Semaphore* semaphore) {
    if (semaphore == nullptr) return;

    nya_os_semaphore_deinit(&semaphore->os_semaphore);
    nya_arena_free(semaphore->arena, semaphore, sizeof(NYA_Semaphore));
}

void nya_semaphore_post(NYA_Semaphore* semaphore) {
    nya_assert(semaphore != nullptr);

    nya_os_semaphore_post(&semaphore->os_semaphore);
}

void nya_semaphore_wait(NYA_Semaphore* semaphore) {
    nya_assert(semaphore != nullptr);

    NYA_OsThreadStatus status = nya_os_semaphore_wait(&semaphore->os_semaphore, NYA_OS_THREAD_WAIT_FOREVER);
    nya_assert(status == NYA_OS_THREAD_OK, "waiting on a semaphore failed, which means it was destroyed while this thread was in it");
}

b8 nya_semaphore_wait_timeout(NYA_Semaphore* semaphore, u32 timeout_ms) {
    nya_assert(semaphore != nullptr);

    return nya_os_semaphore_wait(&semaphore->os_semaphore, timeout_ms) == NYA_OS_THREAD_OK;
}

// THE MAIN THREAD

void nya_thread_main_claim(void) {
    _nya_thread_is_main = true;

    atomic_store_explicit(&_nya_thread_main_id, nya_os_thread_id_current(), memory_order_relaxed);

    // released after the thread local, so a thread that sees the claim also sees its own false.
    atomic_store_explicit(&_nya_thread_main_claimed, true, memory_order_release);
}

b8 nya_thread_main_is_current(void) {
    if (!atomic_load_explicit(&_nya_thread_main_claimed, memory_order_acquire)) return true;

    return _nya_thread_is_main;
}

void nya_thread_main_only(NYA_ConstCString what) {
    nya_assert(what != nullptr);

    nya_assert_always(
        nya_thread_main_is_current(),
        "%s is the main thread's, and this is not it: thread " FMTu64 ", main is " FMTu64,
        what,
        nya_os_thread_id_current(),
        atomic_load_explicit(&_nya_thread_main_id, memory_order_relaxed)
    );
}
