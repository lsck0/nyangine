#include "nyangine/nyangine.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API DECLARATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

NYA_INTERNAL s32 _nya_job_compare(const NYA_Job* a, const NYA_Job* b);
NYA_INTERNAL s32 _nya_job_scheduler(void* data);

/*
 * How often the scheduler starts queued work and reaps threads that have finished.
 */

/** Nothing queued and nothing running. Roughly one wakeup a frame at 60Hz. */
#define _NYA_JOB_IDLE_TICK_MS 15

/** Work in flight, so this is the per-batch latency rather than a background cost. */
#define _NYA_JOB_BUSY_TICK_MS 2

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PUBLIC API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/*
 * ─────────────────────────────────────────────────────────
 * SYSTEM FUNCTIONS
 * ─────────────────────────────────────────────────────────
 */

NYA_Error nya_system_job_init(void) {
    NYA_App* app = nya_app_get();

    // Both mutex results used to go straight into the struct unchecked, so an allocation failure
    // here surfaced much later as every SDL_LockMutex in the frame loop silently doing nothing.
    SDL_Mutex* job_queue_mutex = SDL_CreateMutex();
    if (job_queue_mutex == nullptr) return nya_error(NYA_ERROR_OUT_OF_MEMORY, "SDL_CreateMutex() failed for the job queue: %s", SDL_GetError());

    SDL_Mutex* job_active_mutex = SDL_CreateMutex();
    if (job_active_mutex == nullptr) {
        SDL_DestroyMutex(job_queue_mutex);
        return nya_error(NYA_ERROR_OUT_OF_MEMORY, "SDL_CreateMutex() failed for the active job list: %s", SDL_GetError());
    }

    app->job_system = (NYA_JobSystem){
        .allocator        = nya_arena_create(.name = "job_system_allocator"),
        .job_queue_mutex  = job_queue_mutex,
        .job_active_mutex = job_active_mutex,
    };

    // The active jobs need no allocation: their records are a fixed pool inside the system, zeroed
    // by the assignment above, because a growable container cannot hold them. See NYA_JobSystem.
    app->job_system.job_queue = nya_heap_create(app->job_system.allocator, NYA_Job, _nya_job_compare);

    // Started last: the scheduler reads everything above, so it must not exist until they do.
    app->job_system.scheduler = SDL_CreateThread(_nya_job_scheduler, "Job Scheduler", nullptr);
    if (app->job_system.scheduler == nullptr) {
        nya_arena_destroy(app->job_system.allocator);
        SDL_DestroyMutex(job_queue_mutex);
        SDL_DestroyMutex(job_active_mutex);
        app->job_system = (NYA_JobSystem){ 0 };

        return nya_error(NYA_ERROR_NOT_OK, "SDL_CreateThread() failed for the job scheduler: %s", SDL_GetError());
    }

    nya_log_info("Job system initialized.");
    return NYA_OK;
}

void nya_system_job_deinit(void) {
    NYA_App* app = nya_app_get();

    atomic_store_explicit(&app->job_system.scheduler_should_exit, true, memory_order_relaxed);
    SDL_WaitThread(app->job_system.scheduler, nullptr);

    /*
     * Whatever was still running when the scheduler stopped, drained here.
     */
    for (u32 slot = 0; slot < _NYA_JOB_MAX_ACTIVE; slot++) {
        if (!app->job_system.job_slot_used[slot]) continue;

        SDL_WaitThread(app->job_system.job_slots[slot].sdl_thread, nullptr);

        app->job_system.job_slot_used[slot] = false;
        app->job_system.job_active_count--;
    }

    SDL_DestroyMutex(app->job_system.job_queue_mutex);
    nya_heap_destroy(app->job_system.job_queue);
    SDL_DestroyMutex(app->job_system.job_active_mutex);

    nya_arena_destroy(app->job_system.allocator);

    nya_log_info("Job system deinitialized.");
}

/*
 * ─────────────────────────────────────────────────────────
 * JOB FUNCTIONS
 * ─────────────────────────────────────────────────────────
 */

NYA_JobHandle nya_job_submit(NYA_Job job) {
    NYA_App* app = nya_app_get();

    /*
     * The counter, under the same lock as the push it belongs with.
     */
    SDL_LockMutex(app->job_system.job_queue_mutex);

    NYA_JobHandle handle = ++app->job_system.next_job_handle;
    job.job_handle       = handle;

    nya_heap_push(app->job_system.job_queue, job);
    SDL_UnlockMutex(app->job_system.job_queue_mutex);

    return handle;
}

void nya_job_wait(NYA_JobHandle job_handle) {
    // The busy rate, never the idle one: a thread that is waiting on a handle is by definition
    // waiting on work in flight, and polling slower than the scheduler advances it only adds
    // latency on top of the tick that will finish the job.
    while (!nya_job_is_done(job_handle)) SDL_Delay(_NYA_JOB_BUSY_TICK_MS);
}

b8 nya_job_is_done(NYA_JobHandle job_handle) {
    NYA_JobSystem* job_system = &nya_app_get()->job_system;

    b8 is_done = true;

    SDL_LockMutex(job_system->job_active_mutex);
    SDL_LockMutex(job_system->job_queue_mutex);
    {
        /* Holding a slot means not done; the thread state is not consulted. */
        for (u32 slot = 0; slot < _NYA_JOB_MAX_ACTIVE; slot++) {
            if (!job_system->job_slot_used[slot]) continue;
            if (job_system->job_slots[slot].job_handle != job_handle) continue;

            is_done = false;
            break;
        }

        nya_array_foreach (job_system->job_queue, queued_job) {
            if (queued_job->job_handle == job_handle) {
                is_done = false;
                break;
            }
        };
    }
    SDL_UnlockMutex(job_system->job_active_mutex);
    SDL_UnlockMutex(job_system->job_queue_mutex);

    return is_done;
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

s32 _nya_job_compare(const NYA_Job* a, const NYA_Job* b) {
    return (s32)a->priority - (s32)b->priority;
}

NYA_INTERNAL s32 _nya_job_scheduler(void* data) {
    nya_unused(data);

    NYA_App*       app        = nya_app_get();
    NYA_JobSystem* job_system = &app->job_system;

    b8 busy = false;

    /*
     * What this pass reaped and what it started, to be announced once the locks are gone.
     */
    NYA_Job finished_jobs[_NYA_JOB_MAX_ACTIVE];
    NYA_Job started_jobs[_NYA_JOB_MAX_ACTIVE];

    while (!atomic_load_explicit(&job_system->scheduler_should_exit, memory_order_relaxed)) {
        u32 finished_count = 0;
        u32 started_count  = 0;

        SDL_LockMutex(job_system->job_active_mutex);
        SDL_LockMutex(job_system->job_queue_mutex);
        {
            /*
             * Reap first, so a slot freed by this pass can be refilled by the scheduling below
             * rather than standing idle for a whole tick.
             */
            for (u32 slot = 0; slot < _NYA_JOB_MAX_ACTIVE; slot++) {
                if (!job_system->job_slot_used[slot]) continue;
                if (SDL_GetThreadState(job_system->job_slots[slot].sdl_thread) == SDL_THREAD_ALIVE) continue;

                SDL_WaitThread(job_system->job_slots[slot].sdl_thread, nullptr);

                finished_jobs[finished_count++] = job_system->job_slots[slot];

                job_system->job_slot_used[slot] = false;
                job_system->job_active_count--;
            }

            /*
             * Clamped to the pool as well as to the configured limit.
             */
            u32 limit = nya_min((u32)app->options.max_concurrent_jobs, (u32)_NYA_JOB_MAX_ACTIVE);

            while (job_system->job_queue->length > 0 && job_system->job_active_count < limit) {
                u32 slot = 0;
                while (slot < _NYA_JOB_MAX_ACTIVE && job_system->job_slot_used[slot]) slot++;
                nya_assert(slot < _NYA_JOB_MAX_ACTIVE, "the active count says there is room but every slot is taken");

                NYA_Job* job_ptr = &job_system->job_slots[slot];
                *job_ptr         = nya_heap_pop(job_system->job_queue);

                job_system->job_slot_used[slot] = true;
                job_system->job_active_count++;

                /*
                 * The thread is handed the address of its slot, which is the whole reason the pool
                 * exists: it keeps dereferencing this record while later jobs are scheduled and
                 * earlier ones are reaped, and neither may move it.
                 */
                NYA_JobFn   function = nya_callback_get(job_ptr->function);
                SDL_Thread* thread   = SDL_CreateThread((int (*)(void*))function, nullptr, job_ptr);
                nya_assert(thread != nullptr, "SDL_CreateThread() failed for a job: %s", SDL_GetError());

                job_ptr->sdl_thread = thread;

                started_jobs[started_count++] = *job_ptr;
            }

            // Read while the locks are still held, so the choice below is made against the state
            // this pass just produced rather than one another thread has since changed.
            busy = job_system->job_active_count > 0 || job_system->job_queue->length > 0;
        }
        // Released in the reverse of the order they were taken.
        SDL_UnlockMutex(job_system->job_queue_mutex);
        SDL_UnlockMutex(job_system->job_active_mutex);

        /*
         * Announced with both locks released, which is the whole reason this is collected first.
         */
        for (u32 i = 0; i < finished_count; i++) {
            nya_event_dispatch((NYA_Event){
                .type         = NYA_EVENT_JOB_COMPLETED,
                .as_job_event = { .job = finished_jobs[i] },
            });
        }

        for (u32 i = 0; i < started_count; i++) {
            nya_event_dispatch((NYA_Event){
                .type         = NYA_EVENT_JOB_STARTED,
                .as_job_event = { .job = started_jobs[i] },
            });
        }

        SDL_Delay(busy ? _NYA_JOB_BUSY_TICK_MS : _NYA_JOB_IDLE_TICK_MS);
    }

    return 0;
}
