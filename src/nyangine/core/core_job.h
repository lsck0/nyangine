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

/*
 * ─────────────────────────────────────────────────────────
 * SYSTEM STRUCT
 * ─────────────────────────────────────────────────────────
 */

struct NYA_JobSystem {
    NYA_Arena* allocator;

    SDL_Mutex*         job_queue_mutex;
    NYA_HeapᐸNYA_Jobᐳ* job_queue;

    SDL_Mutex*          job_active_mutex;
    NYA_ArrayᐸNYA_Jobᐳ* job_active;

    SDL_Thread* scheduler;
    b8          scheduler_should_exit;
};

/*
 * ─────────────────────────────────────────────────────────
 * JOB STRUCTS
 * ─────────────────────────────────────────────────────────
 */

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
