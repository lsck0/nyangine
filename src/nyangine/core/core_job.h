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
 * Jobs that can be running at once, ever — the pool size for their records.
 *
 * A ceiling, not NYA_AppOptions.max_concurrent_jobs: live threads hold pointers into these records,
 * so the pool can't be resized at runtime. 256 covers every u8 value; costs tens of kilobytes once,
 * in the app singleton.
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
     * A job thread holds the address of its own entry for as long as it runs, so it must never be
     * relocated — this used to be a growable array compacted with nya_array_remove, and a memmove
     * during eviction could hand a live thread somebody else's job or reallocate out from under it.
     * A fixed pool avoids both: a slot is written before its thread starts, freed once it's done, and
     * sized to the ceiling of NYA_AppOptions.max_concurrent_jobs so raising the limit can't outgrow it.
     * */
    NYA_Job job_slots[_NYA_JOB_MAX_ACTIVE];

    /** Whether the slot beside it holds a job that has been started and not yet reaped. */
    b8 job_slot_used[_NYA_JOB_MAX_ACTIVE];

    /** How many of the above are used, so the concurrency limit is a compare rather than a scan. */
    u32 job_active_count;

    /**
     * Where the next handle comes from. Guarded by job_queue_mutex, alongside the push it names.
     *
     * A counter, not a hash of the job as this used to be: identical jobs (e.g. a parallel-for batch)
     * hashed identically and shared one handle, so waiting on any of them returned as soon as the
     * *first* finished — and hashing the struct also read its padding, so it wasn't reliably stable
     * anyway. Left at zero by init so the first handle is one, keeping a zeroed NYA_JobHandle "no job".
     * */
    NYA_JobHandle next_job_handle;

    SDL_Thread* scheduler;

    /**
     * Told to the scheduler thread by whoever calls nya_system_job_deinit.
     *
     * Atomic because it's read in the scheduler's loop and written from another thread. As a plain b8
     * this is a data race: nothing forces a reload, so the compiler may hoist the load into `while
     * (true)`, leaving deinit blocked in SDL_WaitThread forever. It survived only because SDL_Delay is
     * opaque enough to defeat the hoist — luck, not a guarantee — and release builds use -flto.
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
