#pragma once

#include "SDL3/SDL_mutex.h"
#include "SDL3/SDL_thread.h"

#include "nyangine/base/base.h"
#include "nyangine/base/base_arena.h"
#include "nyangine/base/base_array.h"
#include "nyangine/base/base_heap.h"
#include "nyangine/core/core_callback.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * TYPES
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

typedef u64                  NYA_JobHandle;
typedef enum NYA_JobPriority NYA_JobPriority;
typedef struct NYA_JobSystem NYA_JobSystem;
typedef struct NYA_Job       NYA_Job;
typedef SDL_Thread*          SDL_ThreadPtr;
nya_derive_heap(NYA_Job);
nya_derive_array(NYA_Job);
nya_derive_array(SDL_ThreadPtr);

typedef int (*NYA_JobFn)(NYA_Job* job);

/**
 * Jobs that can be running at once, ever — the size of the pool their records live in.
 *
 * A ceiling rather than the configured limit: NYA_AppOptions.max_concurrent_jobs can be changed
 * while the app runs, and the pool cannot be resized without moving records that live threads hold
 * pointers into. 256 covers every value a u8 field can take, and the scheduler clamps to it anyway
 * so a wider field later degrades to a lower limit rather than to memory corruption.
 *
 * Costs sizeof(NYA_Job) × 256 in the app singleton, which is tens of kilobytes once.
 * */
#define _NYA_JOB_MAX_ACTIVE 256

/*
 * ─────────────────────────────────────────────────────────
 * JOB STRUCTS
 * ─────────────────────────────────────────────────────────
 */

/* Ahead of the system struct, which embeds a pool of NYA_Job by value and so needs it complete. */

enum NYA_JobPriority {
    NYA_JOB_PRIORITY_LOW,
    NYA_JOB_PRIORITY_NORMAL,
    NYA_JOB_PRIORITY_HIGH,
    NYA_JOB_PRIORITY_COUNT,
};

struct NYA_Job {
    NYA_JobPriority    priority;
    NYA_CallbackHandle function;
    void*              in_data;
    u64                in_size;
    void**             out_data;
    u64*               out_size;

    /* set by the system */
    NYA_JobHandle job_handle;
    SDL_Thread*   sdl_thread;
};

/*
 * ─────────────────────────────────────────────────────────
 * SYSTEM STRUCT
 * ─────────────────────────────────────────────────────────
 */

struct NYA_JobSystem {
    NYA_Arena* allocator;

    SDL_Mutex*         job_queue_mutex;
    NYA_HeapᐸNYA_Jobᐳ* job_queue;

    SDL_Mutex* job_active_mutex;

    /**
     * Running jobs, in storage that never moves.
     *
     * A job thread is handed the address of its own entry and dereferences it for as long as it
     * runs, so an entry has to outlive the thread and must never be relocated. This used to be a
     * growable array compacted with nya_array_remove on every eviction — a memmove that shifted
     * every still-running job's record down by one and handed live threads somebody else's job,
     * and that could also reallocate out from under them. A fixed pool has neither problem: a slot
     * is written before its thread starts and released only once that thread is no longer running.
     *
     * Sized to the ceiling of NYA_AppOptions.max_concurrent_jobs rather than to its current value,
     * so raising the limit at runtime cannot outgrow it. The scheduler clamps to this regardless.
     * */
    NYA_Job job_slots[_NYA_JOB_MAX_ACTIVE];

    /** Whether the slot beside it holds a job that has been started and not yet reaped. */
    b8 job_slot_used[_NYA_JOB_MAX_ACTIVE];

    /** How many of the above are used, so the concurrency limit is a compare rather than a scan. */
    u32 job_active_count;

    /**
     * Where the next handle comes from. Guarded by job_queue_mutex, alongside the push it names.
     *
     * A counter rather than a hash of the job, which is what this used to be. Two jobs with the same
     * function and the same arguments hash identically, so a batch of identical work — the normal
     * case, and exactly what a parallel for loop submits — all shared one handle. Waiting on any of
     * them then returned as soon as the *first* finished, because that is all nya_job_is_done can
     * see. Hashing the struct also read its padding, so the handle was not reliably stable anyway.
     *
     * Left at zero by init so the first handle is one, which keeps a zeroed NYA_JobHandle meaning
     * "no job" rather than naming the first one submitted.
     * */
    NYA_JobHandle next_job_handle;

    SDL_Thread* scheduler;

    /**
     * Told to the scheduler thread by whoever calls nya_system_job_deinit.
     *
     * Atomic because it is read in the scheduler's loop condition and written from another thread.
     * As a plain b8 that is a data race, and not a harmless one: nothing in the loop body forces a
     * reload, so the compiler may hoist the load and turn the condition into `while (true)`, leaving
     * deinit blocked in SDL_WaitThread forever. It survived only because SDL_Delay is opaque enough
     * to defeat the hoist, which is luck rather than a guarantee — and release builds are built with
     * -flto, where more of the program is visible at once.
     * */
    atomic b8 scheduler_should_exit;
};

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * FUNCTIONS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/*
 * ─────────────────────────────────────────────────────────
 * SYSTEM FUNCTIONS
 * ─────────────────────────────────────────────────────────
 */

NYA_API NYA_Error nya_system_job_init(void) __attr_no_discard;
NYA_API void nya_system_job_deinit(void);

/*
 * ─────────────────────────────────────────────────────────
 * JOB FUNCTIONS
 * ─────────────────────────────────────────────────────────
 */

NYA_API NYA_JobHandle nya_job_submit(NYA_Job job);
NYA_API void          nya_job_wait(NYA_JobHandle job_handle);
NYA_API b8            nya_job_is_done(NYA_JobHandle job_handle);
