/**
 * @file core_system.h
 *
 * ```c
 * nya_system_register((NYA_SystemEntry){ .name = "input", .update = input_update });
 * nya_system_register((NYA_SystemEntry){ .name = "follow", .after = "input", .update = follow_update });
 * NYA_EXPECT(nya_system_registry_finalize());
 * // once per frame:
 * nya_system_registry_run_update(delta_time_s);
 * ```
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
 * */
NYA_API NYA_Error nya_system_registry_finalize(void) __attr_no_discard;

/**
 * Runs every registered `init`, skipping a null one, in finalized order.
 * */
NYA_API NYA_Error nya_system_registry_run_init(void) __attr_no_discard;

/** Runs every registered `update`, skipping a null one, in finalized order. Every call, no early return. */
NYA_API void nya_system_registry_run_update(f32 delta_time_s);

/**
 * Runs every registered `deinit`, skipping a null one, in REVERSE finalized order.
 * */
NYA_API void nya_system_registry_run_deinit(void);

/** How many systems are registered. */
NYA_API u32 nya_system_registry_count(void) __attr_no_discard;

/** The name at `index`, in registration order before finalize and run order after it. */
NYA_API NYA_ConstCString nya_system_registry_name_at(u32 index) __attr_no_discard;

/**
 * The `init`/`deinit` function pointers at `index` (finalized order), nullable like the entry itself.
 * */
NYA_API NYA_SystemInitFn   nya_system_registry_init_at(u32 index) __attr_no_discard;
NYA_API NYA_SystemDeinitFn nya_system_registry_deinit_at(u32 index) __attr_no_discard;

#ifdef NYA_TESTING
/**
 * Returns the registry to its just-linked state: no entries, not finalized.
 * */
NYA_INTERNAL void _nya_system_registry_reset_for_test(void);
#endif
