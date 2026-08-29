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
 *
 * A poll rather than a wait on a condition, because a job is a bare SDL thread and there is no
 * signal when one exits — SDL_GetThreadState has to be asked.
 *
 * Two rates rather than one, because the two situations want opposite things. With nothing queued
 * and nothing running there is nobody to be responsive to, so the scheduler idles at a rate that
 * costs a handful of wakeups a second. With work in flight the tick is the latency of every batch
 * after the first: a queue longer than the concurrency limit advances one tick at a time, so a
 * batch of sixty-four against a limit of four pays sixteen of them.
 *
 * Both were POSIX `sleep(1)` — one *second*, not one millisecond. That made the same sixty-four job
 * batch take sixteen seconds, and made tests/nyangine/core/test_job.c seventeen times slower than
 * anything else in the suite.
 *
 * SDL_Delay rather than sleep for a second reason: `sleep` is POSIX and does not exist on the
 * Windows host this tree also builds tests for.
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
     *
     * Those threads hold a pointer into job_slots and may still call back into this system, so
     * letting them outlive the mutexes and the arena destroyed below is a use-after-free waiting for
     * an unlucky shutdown. Nothing else can reap them either — the scheduler that would have is
     * already joined.
     *
     * No mutex around this: the scheduler is gone, so this is the only thread touching the pool.
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
     *
     * This used to be nya_hash_fnv1a over the job struct, which gives two submissions of the same
     * function with the same arguments the *same* handle — and that is the common case, not a corner
     * one: it is exactly what submitting a batch of identical work produces. nya_job_is_done can
     * only answer for the first match it finds, so waiting on any handle in such a batch returned as
     * soon as one of them finished, and the caller carried on while the rest were still running.
     *
     * It was invisible while the scheduler ticked once a second, because everything had long since
     * finished by the time anything got around to waiting. Making the scheduler quick is what
     * exposed it — tests/nyangine/core/test_job.c caught 29 of 32 jobs done.
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
        /*
         * Holding a slot means not done — the thread state is deliberately not consulted.
         *
         * A job becomes done when the scheduler *reaps* it, which joins its thread while holding
         * job_active_mutex. A caller that observes "done" through this lock therefore also observes
         * everything the job wrote, because the join and the unlock both happen before the caller's
         * lock. Answering from SDL_THREAD_COMPLETE instead returned as soon as the thread stopped
         * running and before any join, so reading a job's out_data afterwards was a race with no
         * synchronisation behind it — which is exactly what tests/nyangine/core/test_job.c does.
         *
         * Costs at most one scheduler tick of extra latency, and buys a guarantee the API could not
         * otherwise make.
         */
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
     *
     * At function scope rather than per iteration, and plain arrays rather than nya_array against
     * job_system->allocator. Two reasons. The arena is shared with nya_job_submit, which pushes the
     * queue onto it from whatever thread submits, so touching it is only safe under job_queue_mutex
     * — and the whole point here is to be outside that lock. And a scheduler tick is two
     * milliseconds, so an allocate-and-free per pass is a cost with nothing to show for it.
     *
     * _NYA_JOB_MAX_ACTIVE bounds both: a pass cannot reap more than the pool holds, and cannot start
     * more than it has slots for. Together that is about thirty two kibibytes of this thread's stack,
     * once.
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
             *
             * SDL_WaitThread rather than leaving the thread at COMPLETE: it is what reclaims the
             * thread's resources, which nothing used to do, and it makes releasing the slot
             * unambiguous. Once it returns the thread is genuinely gone, so the record it was
             * holding a pointer to can be handed to the next job. It cannot block here — the state
             * was just checked as not ALIVE.
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
             *
             * max_concurrent_jobs is a u8, so today it cannot exceed the pool. If it is ever
             * widened, this costs concurrency rather than overrunning the slots.
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
                 *
                 * sdl_thread is filled in after the thread exists, which is safe because every
                 * reader of it takes the active mutex this block already holds — so nobody can
                 * observe the record half filled.
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
         *
         * nya_event_dispatch takes event_queue_mutex and runs every immediate hook while still
         * holding it, so dispatching from inside the critical section above established the order
         * job_active -> job_queue -> event_queue on this thread. Any other thread that dispatches an
         * event whose hook calls nya_job_submit establishes event_queue -> job_queue, and the two
         * together are a deadlock: the scheduler waits on event_queue holding job_queue while the
         * other thread waits on job_queue holding event_queue.
         *
         * It survived only because nothing in the tree calls nya_job_submit yet, and because SDL
         * mutexes are recursive — which covers a JOB_COMPLETED handler resubmitting on this thread,
         * and covers nothing else.
         *
         * The cost is that work submitted from a JOB_COMPLETED handler is now picked up on the next
         * tick rather than the same one, since the scheduling loop above has already run by the time
         * the handler sees the event. That is two milliseconds, against a deadlock.
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
