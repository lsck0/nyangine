#include "nyangine/nyangine.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * TYPES
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/** Nothing has failed yet. A real slot index once one has. */
#define _NYA_TASKGROUP_NO_FAILURE (-1)

/**
 * One task's record, allocated with the group and pointed at by the job it rides on.
 *
 * `result` is written only by the pool thread that runs this task, and read only by the wait once that
 * thread has been joined, so it needs no lock of its own.
 * */
typedef struct {
    NYA_TaskFn     function;
    void*          argument;
    NYA_TaskGroup* group;

    NYA_JobHandle handle;
    NYA_Error     result;
} _NYA_Task;

struct NYA_TaskGroup {
    NYA_Arena* arena;

    /** `capacity` records, of which `count` have been spawned. Never moves once allocated. */
    _NYA_Task* tasks;
    u32        capacity;
    u32        count;

    /** Raised by a caller's cancel or by the first task to fail; polled by the tasks. */
    atomic b8 cancelled;

    /**
     * The slot of the first task to fail, or _NYA_TASKGROUP_NO_FAILURE. Claimed once, by compare and
     * exchange, so two tasks failing at once cannot both write it and the winner is the first the pool
     * finished. Its release pairs with the acquire in the wait, publishing that task's `result`.
     * */
    atomic s32 first_failed_slot;
};

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API DECLARATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/** What a task's job runs: calls the task's function and records its outcome on the group. */
NYA_INTERNAL int _nya_taskgroup_run(NYA_Job* job);

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PUBLIC API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

NYA_Error nya_taskgroup_begin(NYA_Arena* arena, u32 max_tasks, OUT NYA_TaskGroup** out_group) {
    nya_assert(arena != nullptr, "a task group needs an arena to live in");
    nya_assert(out_group != nullptr, "a task group has to be handed back somewhere");

    // A group that can hold nothing is a caller's mistake rather than an empty group: it can never
    // spawn, so it would only fail later and further from the cause.
    if (max_tasks == 0) return nya_error(NYA_ERROR_INVALID_ARGUMENT, "a task group needs room for at least one task");

    NYA_TaskGroup* group = nya_arena_alloc(arena, sizeof(NYA_TaskGroup));
    *group               = (NYA_TaskGroup){
                      .arena             = arena,
                      .tasks             = nya_arena_alloc(arena, sizeof(_NYA_Task) * max_tasks),
                      .capacity          = max_tasks,
                      .first_failed_slot = _NYA_TASKGROUP_NO_FAILURE,
    };

    *out_group = group;
    return NYA_OK;
}

NYA_Error nya_taskgroup_spawn(NYA_TaskGroup* group, NYA_TaskFn function, void* arg) {
    nya_assert(group != nullptr, "cannot spawn into a null task group");
    nya_assert(function != nullptr, "a task needs a function to run");

    // The bound. A group that fanned out more than it was opened for would either move its records —
    // which the running tasks hold pointers into — or silently drop one; refusing is neither.
    if (group->count >= group->capacity) {
        return nya_error(NYA_ERROR_OUT_OF_MEMORY, "task group is full at " FMTu32 " tasks", group->capacity);
    }

    _NYA_Task* task = &group->tasks[group->count];
    *task           = (_NYA_Task){
                  .function = function,
                  .argument = arg,
                  .group    = group,
                  .result   = NYA_OK,
    };

    // The job carries the record's address, not a copy: the trampoline writes the result back through
    // it, and the record outlives the job because it lives in the group's arena.
    task->handle = nya_job_submit((NYA_Job){
        .priority = NYA_JOB_PRIORITY_NORMAL,
        .function = nya_callback(_nya_taskgroup_run),
        .in_data  = task,
        .in_size  = sizeof(_NYA_Task),
    });

    group->count++;
    return NYA_OK;
}

void nya_taskgroup_end(NYA_TaskGroup* group, OUT NYA_Error* out_error) {
    nya_assert(group != nullptr, "cannot end a null task group");

    // The join for everything the group spawned. Once this loop is through, no task of the group is
    // still running, which is the promise the whole primitive is built on.
    for (u32 i = 0; i < group->count; i++) nya_job_wait(group->tasks[i].handle);

    if (out_error == nullptr) return;

    // Acquire, to pair with the release the failing task did when it claimed the slot: reading a real
    // index here makes that task's write to its `result` visible below.
    s32 failed = atomic_load_explicit(&group->first_failed_slot, memory_order_acquire);

    *out_error = failed == _NYA_TASKGROUP_NO_FAILURE ? NYA_OK : group->tasks[failed].result;
}

void nya_taskgroup_cancel(NYA_TaskGroup* group) {
    nya_assert(group != nullptr, "cannot cancel a null task group");

    atomic_store_explicit(&group->cancelled, true, memory_order_release);
}

b8 nya_taskgroup_is_cancelled(const NYA_TaskGroup* group) {
    nya_assert(group != nullptr, "cannot ask a null task group whether it is cancelled");

    return atomic_load_explicit(&group->cancelled, memory_order_acquire);
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

int _nya_taskgroup_run(NYA_Job* job) {
    _NYA_Task*     task  = (_NYA_Task*)job->in_data;
    NYA_TaskGroup* group = task->group;

    task->result = task->function(task->argument, group);

    if (!task->result.ok) {
        // First failure wins the slot; a later one leaves it be. The compare and exchange's release
        // publishes the write to `result` just above to whoever reads a real index from the wait.
        s32 expected = _NYA_TASKGROUP_NO_FAILURE;
        (void)atomic_compare_exchange_strong(&group->first_failed_slot, &expected, (s32)(task - group->tasks));

        // Ask the siblings to wind down. A failure is exactly the case the cancellation flag is for.
        atomic_store_explicit(&group->cancelled, true, memory_order_release);
    }

    return 0;
}
