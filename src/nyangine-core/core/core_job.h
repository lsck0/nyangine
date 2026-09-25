#pragma once

#include "nyangine-std/base/base.h"
#include "nyangine-std/base/base_arena.h"
#include "nyangine-std/base/base_array.h"
#include "nyangine-std/base/base_heap.h"
#include "nyangine-std/base/base_thread.h"
#include "nyangine-core/core/core_callback.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * TYPES
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

typedef u64                  NYA_JobHandle;
typedef enum NYA_JobPriority NYA_JobPriority;
typedef struct NYA_JobSystem NYA_JobSystem;
typedef struct NYA_Job       NYA_Job;
nya_derive_heap(NYA_Job);
nya_derive_array(NYA_Job);

typedef int (*NYA_JobFn)(NYA_Job* job);

/** Jobs that can be running at once, which sizes the record pool. */
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
    NYA_Thread*   thread;
};

/*
 * ─────────────────────────────────────────────────────────
 * SYSTEM STRUCT
 * ─────────────────────────────────────────────────────────
 */

struct NYA_JobSystem {
    NYA_Arena* allocator;

    NYA_Mutex*         job_queue_mutex;
    NYA_HeapᐸNYA_Jobᐳ* job_queue;

    NYA_Mutex* job_active_mutex;

    /**
     * Running jobs, in storage that never moves.
     * */
    NYA_Job job_slots[_NYA_JOB_MAX_ACTIVE];

    /** Whether the slot beside it holds a job that has been started and not yet reaped. */
    b8 job_slot_used[_NYA_JOB_MAX_ACTIVE];

    /** How many of the above are used, so the concurrency limit is a compare rather than a scan. */
    u32 job_active_count;

    /**
     * Where the next handle comes from. Guarded by job_queue_mutex, alongside the push it names.
     * */
    NYA_JobHandle next_job_handle;

    NYA_Thread* scheduler;

    /**
     * Told to the scheduler thread by whoever calls nya_system_job_deinit.
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
