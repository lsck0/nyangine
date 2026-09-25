/**
 * @file core_taskgroup.h
 *
 * A scoped task group — a "nursery" — that fans work out onto the job pool and joins all of it at
 * scope end, carrying the first failure back to the caller.
 *
 * ```
 * nya_taskgroup_begin         opens a group out of an arena, bounded to a task count
 * nya_taskgroup_spawn         queues one task onto the job pool under the group
 * nya_taskgroup_end           joins every spawned task and reports the first error
 *
 * nya_taskgroup_cancel        asks the group's tasks to stop; the running ones poll for it
 * nya_taskgroup_is_cancelled  what a task polls, and what a failing sibling raises on its own
 * ```
 *
 * ```c
 * NYA_TaskGroup* group = nullptr;
 * NYA_TRY(nya_taskgroup_begin(arena, work_count, &group));
 * for (u32 i = 0; i < work_count; i++) NYA_TRY(nya_taskgroup_spawn(group, do_one, &work[i]));
 *
 * NYA_Error failed = NYA_OK;
 * nya_taskgroup_end(group, &failed);
 * NYA_TRY(failed);
 * ```
 *
 * ── what a group is for ──
 *
 * The job pool (core_job.h) hands back a NYA_JobHandle per submission and asks the caller to hold on
 * to every one, wait on each, and notice for itself that a job wrote a failure somewhere. A caller
 * that fans out a dozen jobs ends up hand-managing a dozen handles and has no place to put "did any of
 * them fail". A group is that place: it owns the handles, joins them all in one call, and a task
 * returns a NYA_Error the way any other function does rather than smuggling a status out through
 * `out_data`. Built on the pool — it submits ordinary jobs and never touches a thread itself.
 *
 * ── structured: nothing outlives the wait ──
 *
 * Every task spawned into a group is joined by nya_taskgroup_end, so no task is still running when
 * the wait returns and none can reach into the group — or the arena it and the caller live in — after
 * the caller moves on. That is the whole discipline: a group is opened and waited on in the same
 * scope, and the wait is the join for all of it, the way a thread is joined and never merely dropped
 * (see base_thread.h). Reclaiming the arena without waiting first would leave a task writing into
 * freed memory, exactly as abandoning a live thread and freeing its record would; so the group is
 * waited on, always.
 *
 * ── the first error is the one you get ──
 *
 * Each task writes its own result into its own slot, so the collection races with nothing. The first
 * task to fail — first by the order the pool finishes them — records itself and raises the group's
 * cancellation flag, so a failure in one task both surfaces to the caller and asks the siblings to
 * wind down. nya_taskgroup_end reports that first failure and swallows the rest: one error a caller
 * can NYA_TRY is more use than a pile it has to sift. A task that ignores the flag simply runs to the
 * end; nothing here kills a thread partway.
 *
 * Thread safety: begin, spawn and end belong to the one thread that owns the group, the way the
 * arena calls they make do. cancel and is_cancelled are safe from anywhere, which is what lets a task
 * on a pool thread poll the flag a sibling on another raised. The job pool must be initialized.
 * */
#pragma once

#include "nyangine-std/base/base_arena.h"
#include "nyangine-std/base/base_attributes.h"
#include "nyangine-std/base/base_error.h"
#include "nyangine-std/base/base_types.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * TYPES
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

typedef struct NYA_TaskGroup NYA_TaskGroup;

/**
 * What one task runs. Returns NYA_OK or the failure the group carries back out of the wait.
 *
 * `group` is the task's own group, so it can poll nya_taskgroup_is_cancelled and bow out early when a
 * sibling has already failed or the owner has asked everyone to stop. A task must not spawn into,
 * wait on, or otherwise outlive the group it runs under.
 * */
typedef NYA_Error (*NYA_TaskFn)(void* arg, NYA_TaskGroup* group);

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * FUNCTIONS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/**
 * Opens a group out of `arena`, holding room for `max_tasks` tasks.
 *
 * The group, its task records and its bookkeeping all come from `arena`, which has to outlive the
 * wait. `max_tasks` is the bound: spawning more than that many fails rather than growing. Zero is
 * rejected, since a group that can hold nothing is a caller's mistake, not an empty group.
 * */
NYA_API NYA_Error nya_taskgroup_begin(NYA_Arena* arena, u32 max_tasks, OUT NYA_TaskGroup** out_group) __attr_no_discard;

/**
 * Queues `function` with `arg` onto the job pool under `group`.
 *
 * `arg` has to outlive the wait, since the task reads it on a pool thread. Fails when the group is
 * already holding `max_tasks` tasks. Returns once the task is queued; whether it has started or run is
 * the pool's business until the wait.
 * */
NYA_API NYA_Error nya_taskgroup_spawn(NYA_TaskGroup* group, NYA_TaskFn function, void* arg) __attr_no_discard;

/**
 * Joins every task spawned into `group` and writes the first failure into `out_error`.
 *
 * Blocks until all of the group's tasks have finished. `out_error` is NYA_OK when they all returned
 * it, otherwise the error from the first task to fail. Spent afterwards: the group has joined
 * everything and nothing of it is still running, so its arena may be reclaimed.
 * */
NYA_API void nya_taskgroup_end(NYA_TaskGroup* group, OUT NYA_Error* out_error);

/**
 * Raises `group`'s cancellation flag, asking its tasks to stop.
 *
 * Advisory: it wakes nothing and kills nothing. A task that polls nya_taskgroup_is_cancelled can
 * return early; one that never looks runs to its end. A task's own failure raises this too, so the
 * siblings of a failed task see the same flag a caller would set by hand.
 * */
NYA_API void nya_taskgroup_cancel(NYA_TaskGroup* group);

/** Whether `group` has been cancelled, by a caller or by a task of its own failing. */
NYA_API b8 nya_taskgroup_is_cancelled(const NYA_TaskGroup* group) __attr_no_discard;
