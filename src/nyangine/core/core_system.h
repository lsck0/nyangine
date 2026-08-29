/**
 * @file core_system.h
 *
 * A named, ordered place to hang per-frame behaviour, for callers that want more than "call the
 * functions in the order I typed them" without hand rolling a schedule of their own.
 *
 * The engine's own subsystems register here too — see core_app.c's `_nya_app_register_subsystems` —
 * despite being a single, engine-owned, compile-time-fixed list, the exact case that reads like it
 * would rather just be a plain array. It uses this anyway so engine and game code share one
 * mechanism, at the cost core_app.c pays for it: its bring-up cannot use run_init/run_deinit as-is,
 * because it unwinds only as far as it actually got on a failed init, which those two do not do (see
 * nya_system_registry_init_at/deinit_at below, and core_app.c's own comment on why).
 *
 * `after` names another system rather than taking a list of dependencies, on purpose: a system runs
 * after at most one other, which is enough to chain a handful of systems into a sequence and not
 * enough to build the general dependency graphs that make a scheduler hard to reason about. Want
 * three things in order? Register three entries, each `after` the last.
 *
 * ```c
 * nya_system_register((NYA_SystemEntry){ .name = "input", .update = input_update });
 * nya_system_register((NYA_SystemEntry){ .name = "follow", .after = "input", .update = follow_update });
 * NYA_EXPECT(nya_system_registry_finalize());
 * // once per frame:
 * nya_system_registry_run_update(delta_time_s);
 * ```
 *
 * `finalize` is where an `after` naming nothing, or a cycle, is caught — loudly, as an `NYA_Error`.
 * C has no trait system to reject that at compile time the way Bevy's Rust does, so this is a
 * runtime check standing in for one, and it is deliberately strict rather than permissive about it:
 * a mistyped name silently doing nothing would be a system that quietly never runs in the order
 * anyone intended.
 * */
#pragma once

#include "nyangine/base/base_attributes.h"
#include "nyangine/base/base_error.h"
#include "nyangine/base/base_types.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * CONSTANTS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/**
 * How many systems can be registered, ever.
 *
 * A ceiling rather than a growable array, for the reason every fixed pool in this codebase is: a
 * caller that quietly keeps succeeding past the point where the design assumed a handful of systems
 * hides the moment the schedule stopped being something a person could read top to bottom.
 * */
#define NYA_SYSTEM_REGISTRY_MAX 64

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * TYPES
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

typedef NYA_Error (*NYA_SystemInitFn)(void);
typedef void (*NYA_SystemUpdateFn)(f32 delta_time_s);
typedef void (*NYA_SystemDeinitFn)(void);

typedef struct {
    /** Unique. What `after` refers to, and what a debug HUD could list later. */
    NYA_ConstCString name;

    /** Another system's name this one must run after, or nullptr to leave it unconstrained. */
    NYA_ConstCString after;

    NYA_SystemInitFn   init;
    NYA_SystemUpdateFn update;
    NYA_SystemDeinitFn deinit;
} NYA_SystemEntry;

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * FUNCTIONS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/** Appends to the registry. Warns and refuses past NYA_SYSTEM_REGISTRY_MAX rather than growing. */
NYA_API void nya_system_register(NYA_SystemEntry entry);

/**
 * Sorts every registered entry by `after` into the order the run_* functions use, once.
 *
 * Entries with no unmet `after` sort stably by registration order — a chain only ever moves a system
 * later than where it was typed, never earlier, so the registration list stays a readable preview of
 * the schedule. An `after` naming a system that was never registered, or a cycle, fails loudly rather
 * than being dropped or ignored; see the file comment for why that has to be a runtime check here.
 *
 * A second call is a no-op: nothing in this API adds a system after finalizing, so there is nothing
 * for a repeat call to pick up, and every call site that finalizes defensively can do so for free.
 * */
NYA_API NYA_Error nya_system_registry_finalize(void) __attr_no_discard;

/**
 * Runs every registered `init`, skipping a null one, in finalized order.
 *
 * Stops and returns the first error rather than pressing on — mirrors nya_app_init_with_options,
 * which stops the engine's own bring-up the same way. Unlike that bring-up, this does not unwind
 * what already succeeded; the caller decides what a partial start means for it, same as it decides
 * whether run_init runs at all.
 * */
NYA_API NYA_Error nya_system_registry_run_init(void) __attr_no_discard;

/** Runs every registered `update`, skipping a null one, in finalized order. Every call, no early return. */
NYA_API void nya_system_registry_run_update(f32 delta_time_s);

/**
 * Runs every registered `deinit`, skipping a null one, in REVERSE finalized order.
 *
 * Reversed rather than independently specified, for the reason core_app.c's own subsystem table is:
 * a system torn down out of order from how it came up is a bug that only shows up once something it
 * depended on is already gone, and there is no second list here to disagree with the first.
 * */
NYA_API void nya_system_registry_run_deinit(void);

/** How many systems are registered. */
NYA_API u32 nya_system_registry_count(void) __attr_no_discard;

/** The name at `index`, in registration order before finalize and run order after it. */
NYA_API NYA_ConstCString nya_system_registry_name_at(u32 index) __attr_no_discard;

/**
 * The `init`/`deinit` function pointers at `index` (finalized order), nullable like the entry itself.
 *
 * For a caller that cannot use run_init as-is because it needs to unwind only as far as it actually
 * got — core_app.c's own bring-up is the reason these exist: a subsystem failing to come up there
 * tears down exactly the ones that already succeeded, not every registered deinit the way
 * nya_system_registry_run_deinit does. Everything else should prefer the run_* functions; reaching
 * for these to hand-roll a loop is a sign the run_* contract does not fit the caller, not a shortcut.
 * */
NYA_API NYA_SystemInitFn   nya_system_registry_init_at(u32 index) __attr_no_discard;
NYA_API NYA_SystemDeinitFn nya_system_registry_deinit_at(u32 index) __attr_no_discard;

#ifdef NYA_TESTING
/**
 * Returns the registry to its just-linked state: no entries, not finalized.
 *
 * Test-only. Nothing in production ever needs this — the registry has no init because a zeroed
 * array already is one, and the process lives exactly as long as the registry does — but one test
 * binary runs every scenario in this file in a single process, and the failure scenarios need a
 * clean registry to fail *into* rather than one already left finalized by an earlier scenario.
 * */
NYA_INTERNAL void _nya_system_registry_reset_for_test(void);
#endif
