#include "nyangine-core/nyangine.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API DECLARATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

NYA_INTERNAL s32  _nya_job_compare(const NYA_Job* a, const NYA_Job* b);
NYA_INTERNAL void _nya_job_scheduler(void* data);

/** What a job's thread runs, since a job is a callback handle and a thread takes a plain function. */
NYA_INTERNAL void _nya_job_run(void* data);

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

    // Ahead of the locks now, because the locks and the scheduler's record are allocated out of it.
    NYA_Arena* allocator = nya_arena_create(.name = "job_system_allocator");

    // Both mutex results used to go straight into the struct unchecked, so a failure here surfaced
    // much later as every lock taken in the frame loop silently doing nothing.
    NYA_Mutex* job_queue_mutex  = nullptr;
    NYA_Mutex* job_active_mutex = nullptr;

    NYA_Error queue_lock = nya_mutex_create(allocator, &job_queue_mutex);
    if (!queue_lock.ok) {
        nya_arena_destroy(allocator);

        return queue_lock;
    }

    NYA_Error active_lock = nya_mutex_create(allocator, &job_active_mutex);
    if (!active_lock.ok) {
        nya_mutex_destroy(job_queue_mutex);
        nya_arena_destroy(allocator);

        return active_lock;
    }

    app->job_system = (NYA_JobSystem){
        .allocator        = allocator,
        .job_queue_mutex  = job_queue_mutex,
        .job_active_mutex = job_active_mutex,
    };

    // The active jobs need no allocation: their records are a fixed pool inside the system, zeroed
    // by the assignment above, because a growable container cannot hold them. See NYA_JobSystem.
    app->job_system.job_queue = nya_heap_create(app->job_system.allocator, NYA_Job, _nya_job_compare);

    // Started last: the scheduler reads everything above, so it must not exist until they do.
    NYA_Error scheduler = nya_thread_spawn(allocator, _nya_job_scheduler, nullptr, "Job Scheduler", &app->job_system.scheduler);
    if (!scheduler.ok) {
        nya_mutex_destroy(job_queue_mutex);
        nya_mutex_destroy(job_active_mutex);
        nya_arena_destroy(allocator);
        app->job_system = (NYA_JobSystem){ 0 };

        return scheduler;
    }

    nya_log_info("Job system initialized.");
    return NYA_OK;
}

void nya_system_job_deinit(void) {
    NYA_App* app = nya_app_get();

    atomic_store_explicit(&app->job_system.scheduler_should_exit, true, memory_order_relaxed);
    nya_thread_join(app->job_system.scheduler);

    /*
     * Whatever was still running when the scheduler stopped, drained here.
     */
    for (u32 slot = 0; slot < _NYA_JOB_MAX_ACTIVE; slot++) {
        if (!app->job_system.job_slot_used[slot]) continue;

        nya_thread_join(app->job_system.job_slots[slot].thread);
        app->job_system.job_slots[slot].thread = nullptr;

        app->job_system.job_slot_used[slot] = false;
        app->job_system.job_active_count--;
    }

    nya_mutex_destroy(app->job_system.job_queue_mutex);
    nya_heap_destroy(app->job_system.job_queue);
    nya_mutex_destroy(app->job_system.job_active_mutex);

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
    nya_mutex_lock(app->job_system.job_queue_mutex);

    NYA_JobHandle handle = ++app->job_system.next_job_handle;
    job.job_handle       = handle;

    nya_heap_push(app->job_system.job_queue, job);
    nya_mutex_unlock(app->job_system.job_queue_mutex);

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

    nya_mutex_lock(job_system->job_active_mutex);
    nya_mutex_lock(job_system->job_queue_mutex);
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
    nya_mutex_unlock(job_system->job_active_mutex);
    nya_mutex_unlock(job_system->job_queue_mutex);

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

void _nya_job_run(void* data) {
    NYA_Job* job = (NYA_Job*)data;

    NYA_JobFn function = nya_callback_get(job->function);

    // what a job returns goes nowhere: a caller asks nya_job_is_done and reads the job's own out_data.
    (void)function(job);
}

void _nya_job_scheduler(void* data) {
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

        nya_mutex_lock(job_system->job_active_mutex);
        nya_mutex_lock(job_system->job_queue_mutex);
        {
            /*
             * Reap first, so a slot freed by this pass can be refilled by the scheduling below
             * rather than standing idle for a whole tick.
             */
            for (u32 slot = 0; slot < _NYA_JOB_MAX_ACTIVE; slot++) {
                if (!job_system->job_slot_used[slot]) continue;
                if (!nya_thread_is_finished(job_system->job_slots[slot].thread)) continue;

                // the flag says the job's function has returned; the thread it ran on is still to be waited for.
                nya_thread_join(job_system->job_slots[slot].thread);
                job_system->job_slots[slot].thread = nullptr;

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
                NYA_Error started = nya_thread_spawn(job_system->allocator, _nya_job_run, job_ptr, "Job", &job_ptr->thread);
                nya_assert(started.ok, "a job's thread could not be started: %s", (NYA_ConstCString)started.message);

                started_jobs[started_count++] = *job_ptr;
            }

            // Read while the locks are still held, so the choice below is made against the state
            // this pass just produced rather than one another thread has since changed.
            busy = job_system->job_active_count > 0 || job_system->job_queue->length > 0;
        }
        // Released in the reverse of the order they were taken.
        nya_mutex_unlock(job_system->job_queue_mutex);
        nya_mutex_unlock(job_system->job_active_mutex);

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
}
