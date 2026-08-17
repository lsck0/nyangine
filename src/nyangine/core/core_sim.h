/**
 * @file core_sim.h
 *
 * The simulation barrier: what happened this frame, and what should change because of it.
 *
 * Two problems, one mechanism.
 *
 * **Mutating the world while it is being iterated.** A projectile's update killing the entity it
 * hit invalidates whatever the caller was walking, and makes the outcome depend on update order.
 * So mutations are not applied where they are decided — they are queued with nya_sim_defer and
 * applied at a barrier, after every update for the tick has run.
 *
 * **Telling anyone what happened.** A damage number, an achievement, a combat log, a replay stream
 * and a telemetry counter all want the same facts, and none of them belong in the code that does
 * the hitting. So facts are recorded with nya_sim_record and read back at the end of the frame by
 * observers, which know nothing about each other.
 *
 * ```c
 * // in the projectile's update
 * nya_sim_record(GNY_SIM_DAMAGE_DEALT, &(GNY_DamageDealt){ .target = id, .amount = 12 }, sizeof(GNY_DamageDealt));
 * if (health <= 0) nya_sim_defer(gny_kill_entity, &id, sizeof(id));
 * ```
 *
 * Records are cleared at the end of every frame, so an observer sees exactly the frame it is being
 * notified about and nothing accumulates. Anything that must outlive the frame is the observer's to
 * copy somewhere durable.
 *
 * `type` is a plain u32 that the game defines. Core does not interpret it — it has no business
 * knowing what damage is.
 * */
#pragma once

#include "nyangine/base/base_arena.h"
#include "nyangine/base/base_array.h"
#include "nyangine/base/base_attributes.h"
#include "nyangine/base/base_error.h"
#include "nyangine/base/base_types.h"
#include "nyangine/core/core_callback.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * CONSTANTS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

#define NYA_SIM_OBSERVER_MAX 16

/**
 * How many times the command queue may refill while draining before it is called a runaway.
 *
 * A deferred command is allowed to defer more work, which is what makes a chain like death → drop
 * loot → trigger pickup express itself naturally. A cycle would otherwise spin forever, so the
 * drain gives up loudly instead.
 * */
#define NYA_SIM_COMMAND_DRAIN_MAX 64

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * TYPES
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

typedef struct NYA_SimRecord  NYA_SimRecord;
typedef struct NYA_SimCommand NYA_SimCommand;
typedef struct NYA_SimSystem  NYA_SimSystem;

/** Something that happened. `data` is a copy owned by the simulation's frame arena. */
struct NYA_SimRecord {
    /** Game defined. Core never interprets it. */
    u32 type;

    /** Which fixed update tick produced this, so an observer can tell one tick's worth from another. */
    u64 tick;

    void* data;
    u64   size;
};

nya_derive_array(NYA_SimRecord);

/** Applies a queued mutation. `data` is the copy taken at nya_sim_defer time, or null. */
typedef void (*NYA_SimCommandFn)(void* data);

struct NYA_SimCommand {
    NYA_SimCommandFn apply;
    void*            data;
    u64              size;
};

nya_derive_array(NYA_SimCommand);

/**
 * Notified once per frame with everything recorded during it.
 *
 * `records` is empty on a frame where nothing happened; observers are still called, so one that
 * needs to see "nothing happened" can.
 * */
typedef void (*NYA_SimObserverFn)(const NYA_ArrayᐸNYA_SimRecordᐳ* records, void* user_data);

/*
 * ─────────────────────────────────────────────────────────
 * SYSTEM STRUCT
 * ─────────────────────────────────────────────────────────
 */

struct NYA_SimSystem {
    /** Reset at the end of every frame. Owns the record and command payloads. */
    NYA_Arena* allocator;

    /** Fixed update ticks since startup. Advances with the simulation, not with rendered frames. */
    u64 tick;

    NYA_ArrayᐸNYA_SimRecordᐳ*  records;
    NYA_ArrayᐸNYA_SimCommandᐳ* commands;

    struct {
        /**
         * By handle rather than by pointer, so an observer registered by the game survives a reload.
         *
         * The game is a shared library that is closed and reopened, and a raw function pointer into
         * it aims at an unmapped page afterwards — the next end of frame would call it. Callback
         * handles are re-resolved by name in update_callback_pointers, which is the same reason the
         * per entity hooks are stored this way.
         * */
        NYA_CallbackHandle callback;
        void*              user_data;
    } observers[NYA_SIM_OBSERVER_MAX];
    u32 observer_count;

    /** Set while draining, so a command deferring more work appends instead of recursing. */
    b8 draining;
};

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * FUNCTIONS AND MACROS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/*
 * ─────────────────────────────────────────────────────────
 * SYSTEM FUNCTIONS
 * ─────────────────────────────────────────────────────────
 */

NYA_API void nya_system_sim_init(void);
NYA_API void nya_system_sim_deinit(void);

/** Applies every queued command. Called by the frame loop at the end of each fixed update tick. */
NYA_API void nya_system_sim_apply_commands(void);

/** Notifies the observers, then drops the frame's records. Called by the frame loop at frame end. */
NYA_API void nya_system_sim_end_frame(void);

/*
 * ─────────────────────────────────────────────────────────
 * SIMULATION FUNCTIONS
 * ─────────────────────────────────────────────────────────
 */

/**
 * Records that something happened. `data` is copied, so the caller may pass a stack local.
 *
 * Pass null data with size 0 for a fact that is entirely described by its type.
 * */
NYA_API void nya_sim_record(u32 type, const void* data, u64 size);

/**
 * Queues a mutation to apply at the next barrier. `data` is copied.
 *
 * Called from inside a barrier, the new command joins the same drain rather than running
 * immediately, so ordering stays predictable.
 * */
NYA_API void nya_sim_defer(NYA_SimCommandFn apply, const void* data, u64 size);

/** Everything recorded so far this frame. Valid until the end of the frame. */
NYA_API const NYA_ArrayᐸNYA_SimRecordᐳ* nya_sim_records(void) __attr_no_discard;

/** Fixed update ticks since startup. */
NYA_API u64 nya_sim_tick(void) __attr_no_discard;

/** Registers a frame end observer. Errors rather than silently dropping it once full. */
/**
 * Registers an observer, by callback handle so it survives a hot reload.
 *
 * ```c
 * NYA_EXPECT(nya_sim_observer_add(nya_callback(gny_sim_observe), nullptr));
 * ```
 *
 * The function must be exported — not NYA_INTERNAL — because the handle is resolved by name.
 * */
NYA_API NYA_Error nya_sim_observer_add(NYA_CallbackHandle observer, void* user_data) __attr_no_discard;

NYA_API void nya_sim_observer_clear(void);
