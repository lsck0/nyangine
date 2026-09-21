/**
 * @file core_system.h
 *
 * One registry for every system in the process: the engine's subsystems, the game's, and a plugin's.
 * A system is a name, an optional predecessor, a lifetime pair (`init` / `deinit`) and a callback per
 * frame phase. The registry owns the order, runs the phases, and can be changed while the game runs:
 * registered, unregistered, enabled and disabled by name.
 *
 * Phases, in the order the app runs them each frame:
 *   NYA_SYSTEM_PHASE_FRAME    once per frame, before events are handled. Variable delta.
 *   NYA_SYSTEM_PHASE_TICK     the fixed timestep. Runs zero or more times per frame. Fixed delta.
 *   NYA_SYSTEM_PHASE_RENDER   once per frame, drawing. Variable delta.
 *
 * Everything in the module:
 *   nya_system_register                   adds a system, before or after finalize
 *   nya_system_unregister                 removes one by name, idempotent
 *   nya_system_enable / _disable          whether its phase callbacks run; `init`/`deinit` untouched
 *   nya_system_is_enabled                 what those two last set
 *   nya_system_phase_name                 "frame", "tick", "render", for a log or an overlay
 *   nya_system_owner_name                 "engine", "game", or the plugin's own name
 *   nya_system_registry_finalize          sorts by `after` into run order and reports a bad graph
 *   nya_system_registry_run_init          brings every system up in order, unwinding on failure
 *   nya_system_registry_run               runs one phase, in order, skipping disabled systems
 *   nya_system_registry_run_deinit        tears down what came up, in reverse order
 *   nya_system_registry_is_running        whether a phase run is in progress right now
 *   nya_system_registry_report            logs the schedule, one line per phase, at debug level
 *   nya_system_registry_count             how many are registered
 *   nya_system_registry_at                the entry at an index, in run order
 *   nya_system_registry_enabled_at        whether the entry at an index is enabled
 *   nya_system_registry_initialized_at    whether its `init` ran and succeeded
 *   nya_system_registry_runs_phase_at     whether it has work in one phase
 *   nya_system_registry_time_ns_at        what it cost over the last frame, while accounting is on
 *   nya_system_accounting_enable/_disable whether the run loop times each system
 *   nya_system_accounting_is_enabled      what those two last set
 *   nya_system_accounting_frame_end       rolls the measured window; called once per frame by the app
 *   nya_system_owner_count                how many distinct owners are registered
 *   nya_system_owner_stats_at             one owner's system count, frame time and held bytes
 *
 * ```c
 * void gravity_tick(f32 delta_time_s) { ... }
 *
 * nya_system_register((NYA_SystemEntry){ .name = "gravity", .after = "physics2d", .tick = nya_callback(gravity_tick) });
 * NYA_EXPECT(nya_system_registry_finalize());
 *
 * // for an effect: the system stays registered and initialized, it just stops ticking.
 * nya_system_disable("gravity");
 * nya_system_enable("gravity");
 *
 * // and the app drives the frame from the registry:
 * nya_system_registry_run(NYA_SYSTEM_PHASE_TICK, delta_time_s);
 * ```
 *
 * ORDER. Each entry names at most one system it must run `after` and at most one it must run `before`.
 * `nya_system_registry_finalize` resolves those into one total order that every phase iterates,
 * skipping entries without a callback for that phase. Systems no constraint separates keep the order
 * they were registered in, so a list that needs no `after` at all still reads top to bottom.
 *
 * One order rather than one per phase, because two different positions in the frame are two different
 * systems: split them into two entries, as `steam` (which must initialize before the renderer) and
 * `steam_frame` (which must pump after the gamepad) are split in core_app.c.
 *
 * THE BARRIER. Registering, unregistering, enabling and disabling take effect immediately when no
 * phase is running. Called from inside a system's own callback they are queued instead and applied
 * when that run ends, so a run never sees its own list change under it and every system in a frame
 * sees the same schedule. The queue holds NYA_SYSTEM_PENDING_MAX operations per run and is applied in
 * the order it was filled, so a register followed by a disable of the same name still works.
 *
 * FAILURE. A cycle or an `after` naming nothing registered is reported by
 * `nya_system_registry_finalize`, which is where a typo in a registration list surfaces. The same
 * mistake made after finalize has no return channel to report through and asserts instead.
 *
 * HOT RELOAD. An entry holds callback handles, not function pointers, and copies the three names it is
 * given. Both halves are needed for a system registered from the game DLL to survive a code reload:
 * the handles are re-resolved by name against the new image (see core_callback.h), and the names are
 * the registry's own bytes rather than string literals in whichever image registered them. Registering
 * a system therefore reads `.tick = nya_callback(my_tick)`, exactly as a layer's hooks do.
 *
 * It used to hold raw function pointers, and a system registered from a reloaded image went on running
 * the generation that registered it: the old image stays mapped, so it was a reload that did not take
 * rather than a crash, and it was the one thing in the way of systems coming from a plugin.
 *
 * OWNERSHIP. Every entry says who it belongs to: the engine, the game, or a named plugin. It costs
 * one field at registration and buys the questions that are otherwise unanswerable once a plugin can
 * add systems at runtime: which plugin's systems are eating the frame, how much memory each holds,
 * and whose code raised an error. Leave it zero and the system belongs to the engine, which is what
 * core_app.c wants and nothing else does.
 * */
#pragma once

#include "nyangine/base/base_attributes.h"
#include "nyangine/base/base_error.h"
#include "nyangine/base/base_types.h"
#include "nyangine/core/core_callback.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * CONSTANTS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/**
 * How many systems can be registered at once. The engine takes about thirty of these and the demo
 * another five, so the rest is headroom for plugins. Refused with a warning past it, never grown.
 * */
#ifndef NYA_SYSTEM_REGISTRY_MAX
#define NYA_SYSTEM_REGISTRY_MAX 64
#endif

/**
 * Registrations, removals and enable flips one phase run may queue before they are applied. Sized for
 * a screen change, which is the worst real case: one layer's worth of systems swapped in a single
 * tick. Refused with a warning past it.
 * */
#ifndef NYA_SYSTEM_PENDING_MAX
#define NYA_SYSTEM_PENDING_MAX 16
#endif

/**
 * Distinct owners the registry accounts for: the engine, the game, and one per loaded plugin. Fourteen
 * plugins is already more than a game ships with; past this the extras are still registered and still
 * run, they just report their time under the last owner that fit.
 * */
#ifndef NYA_SYSTEM_OWNER_MAX
#define NYA_SYSTEM_OWNER_MAX 16
#endif

/**
 * Bytes one name may take, terminator included. The registry keeps its own copy of every name it is
 * given, because the caller's is a string literal in an image a reload replaces.
 *
 * The longest name in the engine is "entity_transforms" at eighteen bytes, so this is twice the worst
 * real case and three of them per entry still fit the registry in six kilobytes. A longer name is a
 * registration site's mistake and asserts rather than truncating, since a silently shortened name is a
 * system nothing can name in its `after`.
 * */
#ifndef NYA_SYSTEM_NAME_MAX
#define NYA_SYSTEM_NAME_MAX 40
#endif

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * TYPES
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

typedef enum NYA_SystemPhase          NYA_SystemPhase;
typedef enum NYA_SystemOwnerKind      NYA_SystemOwnerKind;
typedef struct NYA_SystemOwner        NYA_SystemOwner;
typedef struct NYA_SystemEntry        NYA_SystemEntry;
typedef struct NYA_SystemOwnerStats   NYA_SystemOwnerStats;

/**
 * When in the frame a callback runs. `_COUNT` bounds the arrays keyed by it and is not a value
 * anything may hold.
 * */
enum NYA_SystemPhase {
    /** Once per frame, before events are handled. Wall clock delta. */
    NYA_SYSTEM_PHASE_FRAME,

    /** The fixed timestep. Zero or more times per frame, always with NYA_AppOptions.time_step_ns. */
    NYA_SYSTEM_PHASE_TICK,

    /** Once per frame, drawing. Wall clock delta, the same one NYA_SYSTEM_PHASE_FRAME gets. */
    NYA_SYSTEM_PHASE_RENDER,

    NYA_SYSTEM_PHASE_COUNT,
};

/**
 * Who a system belongs to. The tag a permission model and an error report both key off: "the engine
 * did this" and "the `steam` plugin did this" are different sentences, and only the registration site
 * knows which one applies.
 * */
enum NYA_SystemOwnerKind {
    /** The default, so the engine's own registrations carry no ceremony. */
    NYA_SYSTEM_OWNER_ENGINE,

    /** The game built on top of it. */
    NYA_SYSTEM_OWNER_GAME,

    /** Something loaded beside both, which is the only kind that also needs a name. */
    NYA_SYSTEM_OWNER_PLUGIN,

    NYA_SYSTEM_OWNER_KIND_COUNT,
};

struct NYA_SystemOwner {
    NYA_SystemOwnerKind kind;

    /**
     * Which plugin, when `kind` is NYA_SYSTEM_OWNER_PLUGIN, and null otherwise. Asserted both ways at
     * registration, so a plugin cannot report its time as the engine's by forgetting a field.
     * */
    NYA_ConstCString plugin;
};

/** What one owner's systems cost, summed over the systems belonging to it. See nya_system_owner_stats_at. */
struct NYA_SystemOwnerStats {
    /** "engine", "game", or the plugin's name. */
    NYA_ConstCString name;

    NYA_SystemOwnerKind kind;

    u32 system_count;

    /** How many of those are enabled right now. */
    u32 enabled_count;

    /** What they cost over the last frame. Zero while accounting is off; see nya_system_accounting_enable. */
    u64 time_ns;

    /** What they report holding, summed from each system's `memory_bytes`. Zero for systems without one. */
    u64 memory_bytes;
};

typedef NYA_Error (*NYA_SystemInitFn)(void);
typedef void (*NYA_SystemDeinitFn)(void);

/** One phase's work for one system. The delta is the phase's, see NYA_SystemPhase. */
typedef void (*NYA_SystemPhaseFn)(f32 delta_time_s);

/** What a system reports holding right now, for the per-owner memory line. Cheap: it is polled. */
typedef u64 (*NYA_SystemMemoryFn)(void);

/*
 * Every callback below is an NYA_CallbackHandle taken from nya_callback, never a bare function
 * pointer: see HOT RELOAD above. NYA_CALLBACK_HANDLE_NONE, which is what a zeroed field holds, means
 * the system has nothing to do there.
 */
struct NYA_SystemEntry {
    /**
     * Unique, and the handle everything else here takes. What `after` refers to and what the overlay
     * lists. Copied into the registry at registration, so the caller's string need not outlive the call.
     * */
    NYA_ConstCString name;

    /**
     * Another system's name this one must run after, in every phase, or nullptr to leave it
     * unconstrained.
     * */
    NYA_ConstCString after;

    /**
     * The mirror of `after`, and the one a plugin usually wants: a system that was registered before
     * this one cannot name it in its own `after`, so "run ahead of that" has to be sayable from this
     * side. Both may be set, and then both hold.
     * */
    NYA_ConstCString before;

    /** NYA_SystemInitFn and NYA_SystemDeinitFn, by handle. */
    NYA_CallbackHandle init;
    NYA_CallbackHandle deinit;

    /** NYA_SystemPhaseFn, by handle. None where this system has nothing to do in that phase. */
    NYA_CallbackHandle frame;
    NYA_CallbackHandle tick;
    NYA_CallbackHandle render;

    /**
     * Whether this system is allowed to be unavailable in the mode it was registered for.
     *
     * An optional system whose `init` fails is reported at debug level, left uninitialized (so its
     * `deinit` is skipped too) and stepped over, and bring-up carries on. A mandatory one stops
     * bring-up and unwinds. This is the mandatory-versus-optional dependency split: a dedicated server
     * with no GPU backend is a correct headless run, the same failure in a windowed run is not, and
     * only whoever registers the system knows which run this is.
     * */
    b8 optional;

    /** Zero is the engine. See OWNERSHIP above. */
    NYA_SystemOwner owner;

    /**
     * NYA_SystemMemoryFn, by handle: what this system is holding, asked for the owner's memory line and
     * nowhere else. None where the system holds nothing of its own, which is most of them.
     * */
    NYA_CallbackHandle memory_bytes;
};

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * REGISTRATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/**
 * Adds `entry`, enabled, in the position `after` asks for. Warns and refuses past
 * NYA_SYSTEM_REGISTRY_MAX rather than growing, and asserts on a duplicate name.
 *
 * Legal before and after finalize. The order is resolved at the next barrier, so a batch may be
 * registered in any order as long as every `after` is registered by the time the batch ends.
 * */
NYA_API void nya_system_register(NYA_SystemEntry entry);

/**
 * Removes the system called `name`. The partner of nya_system_register.
 *
 * Idempotent, and a no-op for a name that was never registered. Asserts when the system is running
 * right now, or when another registered system names it in `after`: unregister the dependents first.
 * A system that had been initialized is torn down by its `deinit` before it goes.
 * */
NYA_API void nya_system_unregister(NYA_ConstCString name);

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * ENABLING
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/**
 * Whether the system's phase callbacks run. Its `init` and `deinit` are untouched either way, so a
 * disabled system keeps everything it allocated and picks up exactly where it left off.
 *
 * This is how a game turns an engine system off for an effect (`nya_system_disable("physics2d")` for
 * a freeze frame) and how a plugin replaces one: disable the stock system, register its own after it.
 *
 * Both assert on a name that was never registered, since that is a typo rather than a state.
 * */
NYA_API void nya_system_enable(NYA_ConstCString name);
NYA_API void nya_system_disable(NYA_ConstCString name);

/** What those two last set. False for a name that was never registered. */
NYA_API b8 nya_system_is_enabled(NYA_ConstCString name) __attr_no_discard;

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * RUNNING
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/**
 * Sorts every registered entry by `after` into the order the run functions use.
 *
 * Call it once after the startup registrations. Calling it again re-sorts only if something changed
 * since, so it is cheap to call defensively. Reports a cycle, naming the systems in it, and an
 * `after` that names nothing registered, naming both.
 * */
NYA_API NYA_Error nya_system_registry_finalize(void) __attr_no_discard;

/**
 * Runs every `init` in order, timing each one into the debug log.
 *
 * On a mandatory system's failure, tears down everything that had come up, in reverse, and returns
 * that error: a failed bring-up leaves nothing half built. An optional system's failure is logged at
 * debug level and skipped, and bring-up continues. See NYA_SystemEntry.optional.
 * */
NYA_API NYA_Error nya_system_registry_run_init(void) __attr_no_discard;

/**
 * Runs `phase` for every enabled system that has a callback for it, in finalized order. Every call,
 * no early return.
 *
 * Applies whatever the run queued at the barrier on the way out; see THE BARRIER above. Asserts on
 * reentrancy: a phase may not be run from inside a phase.
 * */
NYA_API void nya_system_registry_run(NYA_SystemPhase phase, f32 delta_time_s);

/**
 * Runs `deinit` in reverse finalized order, for every system whose `init` ran and succeeded. The
 * partner of nya_system_registry_run_init, and safe to call when only part of the registry came up.
 * */
NYA_API void nya_system_registry_run_deinit(void);

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * INTROSPECTION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/** "frame", "tick" or "render". Asserts on anything else. */
NYA_API NYA_ConstCString nya_system_phase_name(NYA_SystemPhase phase) __attr_no_discard;

/** "engine", "game", or the plugin's own name. The key every per-owner number is grouped by. */
NYA_API NYA_ConstCString nya_system_owner_name(NYA_SystemOwner owner) __attr_no_discard;

/** Whether a phase run is in progress, which is what makes a mutation queue instead of apply. */
NYA_API b8 nya_system_registry_is_running(void) __attr_no_discard;

/**
 * Logs the schedule at debug level, one line per phase, in run order, with a disabled system marked.
 *
 * The answer to "why did my system run after that one", which is otherwise only readable by working
 * the `after` and `before` edges out by hand. Call it after the registrations are in.
 * */
NYA_API void nya_system_registry_report(void);

/** How many systems are registered. */
NYA_API u32 nya_system_registry_count(void) __attr_no_discard;

/**
 * The entry at `index`, in registration order before finalize and run order after it. Never null:
 * `index` is asserted against the count.
 * */
NYA_API const NYA_SystemEntry* nya_system_registry_at(u32 index) __attr_no_discard;

/** Whether the entry at `index` is enabled, and whether its `init` ran and succeeded. */
NYA_API b8 nya_system_registry_enabled_at(u32 index) __attr_no_discard;
NYA_API b8 nya_system_registry_initialized_at(u32 index) __attr_no_discard;

/**
 * Whether the entry at `index` has work in `phase`, which is what the overlay's phase column reads.
 * Here rather than at the call site so nothing outside this module resolves a callback handle itself.
 * */
NYA_API b8 nya_system_registry_runs_phase_at(u32 index, NYA_SystemPhase phase) __attr_no_discard;

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * ACCOUNTING
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 *
 * Off by default, because the run loop is otherwise two loads and a call per system and a clock read
 * per system would be most of it. On, every phase run times each system it calls and the totals are
 * grouped by owner, which is what answers "which plugin is costing the frame".
 *
 * Measured against the real monotonic clock, not the app's time source. A simulation run installs a
 * clock that advances one tick per frame (see core_app.h and testing_session.h), and a profiler
 * reading it would report that every system takes exactly the same simulated time, which is true and
 * useless. What this answers is what a system costs the machine, and that is wall clock.
 */

NYA_API void nya_system_accounting_enable(void);
NYA_API void nya_system_accounting_disable(void);
NYA_API b8   nya_system_accounting_is_enabled(void) __attr_no_discard;

/**
 * Publishes what was measured since the last call as "the last frame" and starts a new window. Called
 * once per frame by the app, at the end of the frame; a no-op while accounting is off.
 *
 * Here rather than inside a phase run because the tick phase runs a variable number of times per frame
 * and the render phase may be skipped: no single run is the frame boundary, so the app has to say.
 * */
NYA_API void nya_system_accounting_frame_end(void);

/** What the system at `index` cost over the last frame, summed across every phase it ran in. */
NYA_API u64 nya_system_registry_time_ns_at(u32 index) __attr_no_discard;

/** How many distinct owners have a system registered, at most NYA_SYSTEM_OWNER_MAX. */
NYA_API u32 nya_system_owner_count(void) __attr_no_discard;

/** One owner's totals. Walks the registry, so read it once per frame rather than once per row. */
NYA_API NYA_SystemOwnerStats nya_system_owner_stats_at(u32 index) __attr_no_discard;

#ifdef NYA_TESTING
/**
 * Returns the registry to its just-linked state: no entries, nothing pending, not finalized.
 * */
NYA_INTERNAL void _nya_system_registry_reset_for_test(void);
#endif
