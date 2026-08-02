#include "nyangine/nyangine.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API DECLARATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

NYA_INTERNAL s32 _nya_job_compare(const NYA_Job* a, const NYA_Job* b);
NYA_INTERNAL s32 _nya_job_scheduler(void* data);

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

    app->job_system.job_queue  = nya_heap_create(app->job_system.allocator, NYA_Job, _nya_job_compare);
    app->job_system.job_active = nya_array_create(app->job_system.allocator, NYA_Job);

    // Started last: the scheduler reads everything above, so it must not exist until they do.
    app->job_system.scheduler = SDL_CreateThread(_nya_job_scheduler, "Job Scheduler", nullptr);
    if (app->job_system.scheduler == nullptr) {
        nya_arena_destroy(app->job_system.allocator);
        SDL_DestroyMutex(job_queue_mutex);
        SDL_DestroyMutex(job_active_mutex);
        app->job_system = (NYA_JobSystem){ 0 };

        return nya_error(NYA_ERROR_NOT_OK, "SDL_CreateThread() failed for the job scheduler: %s", SDL_GetError());
    }

    nya_info("Job system initialized.");
    return NYA_OK;
}

void nya_system_job_deinit(void) {
    NYA_App* app = nya_app_get();

    app->job_system.scheduler_should_exit = true;
    SDL_WaitThread(app->job_system.scheduler, nullptr);

    SDL_DestroyMutex(app->job_system.job_queue_mutex);
    nya_heap_destroy(app->job_system.job_queue);
    SDL_DestroyMutex(app->job_system.job_active_mutex);
    nya_array_destroy(app->job_system.job_active);

    nya_arena_destroy(app->job_system.allocator);

    nya_info("Job system deinitialized.");
}

/*
 * ─────────────────────────────────────────────────────────
 * JOB FUNCTIONS
 * ─────────────────────────────────────────────────────────
 */

NYA_JobHandle nya_job_submit(NYA_Job job) {
    NYA_App* app = nya_app_get();

    NYA_JobHandle handle = nya_hash_fnv1a(&job, sizeof(NYA_Job));
    job.job_handle       = handle;

    SDL_LockMutex(app->job_system.job_queue_mutex);
    nya_heap_push(app->job_system.job_queue, job);
    SDL_UnlockMutex(app->job_system.job_queue_mutex);

    return handle;
}

void nya_job_wait(NYA_JobHandle job_handle) {
    while (!nya_job_is_done(job_handle)) sleep(1);
}

b8 nya_job_is_done(NYA_JobHandle job_handle) {
    NYA_JobSystem* job_system = &nya_app_get()->job_system;

    b8 is_done = true;

    SDL_LockMutex(job_system->job_active_mutex);
    SDL_LockMutex(job_system->job_queue_mutex);
    {
        nya_array_foreach (job_system->job_active, active_job) {
            if (active_job->job_handle == job_handle) {
                is_done = SDL_GetThreadState(active_job->sdl_thread) == SDL_THREAD_COMPLETE;
                break;
            }
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

    while (!job_system->scheduler_should_exit) {
        SDL_LockMutex(job_system->job_active_mutex);
        SDL_LockMutex(job_system->job_queue_mutex);
        {
            NYA_ArrayᐸNYA_Jobᐳ finished_jobs = nya_array_create_on_stack(job_system->allocator, NYA_Job);

            // evict finished jobs
            nya_array_foreach (job_system->job_active, active_job) {
                if (SDL_GetThreadState(active_job->sdl_thread) == SDL_THREAD_ALIVE) continue;

                nya_array_push_back(&finished_jobs, *active_job);
            }
            nya_array_foreach (&finished_jobs, finished_job) {
                nya_event_dispatch((NYA_Event){
          .type = NYA_EVENT_JOB_COMPLETED,
          .as_job_event = {
            .job = *finished_job,
          },
        });
                nya_array_for (job_system->job_active, job_index) {
                    if (job_system->job_active->items[job_index].job_handle == finished_job->job_handle) {
                        nya_array_remove(job_system->job_active, job_index);
                        break;
                    }
                }
            }

            // schedule new jobs
            while (job_system->job_queue->length > 0 && job_system->job_active->length < app->options.max_concurrent_jobs) {
                NYA_Job job = nya_heap_pop(job_system->job_queue);
                nya_array_push_back(job_system->job_active, job);
                NYA_Job* job_ptr = nya_array_last(job_system->job_active);

                NYA_JobFn   function = nya_callback_get(job.function);
                SDL_Thread* thread   = SDL_CreateThread((int (*)(void*))function, nullptr, job_ptr);
                nya_assert(thread != nullptr);

                job.sdl_thread      = thread;
                job_ptr->sdl_thread = thread;

                nya_event_dispatch((NYA_Event){
          .type = NYA_EVENT_JOB_STARTED,
          .as_job_event = {
            .job = job,
          },
        });
            }

            nya_array_destroy_on_stack(&finished_jobs);
        }
        SDL_UnlockMutex(job_system->job_active_mutex);
        SDL_UnlockMutex(job_system->job_queue_mutex);

        sleep(1);
    }

    return 0;
}
