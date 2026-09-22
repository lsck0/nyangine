/**
 * @file testing_simulation.h
 *
 * Deterministic simulation testing: one seed drives a whole run, every decision in it comes from
 * hashing that seed with a coordinate rather than from a stateful generator, time is simulated rather
 * than waited on, and faults are injected on purpose. The engine's own assertions are the oracle, so a
 * failing seed is a complete bug report: the same seed replays the same run, step for step.
 *
 * Overview:
 *   nya_simulation_create / _destroy    the run, from a seed and a step count
 *   nya_simulation_action_add           an atomic action the simulator may take
 *   nya_simulation_check_add            an invariant checked after every action
 *   nya_simulation_run                  takes the steps; returns the number of failures
 *   nya_simulation_roll / _below /
 *     _chance / _range_f32 / _pick      the entropy every action draws from
 *   nya_simulation_advance / _now_ns /
 *     _delta_s                          the simulated clock
 *   nya_simulation_fail                 records a failure and prints the replay command
 *   nya_simulation_fill                 well shaped random data for any reflected type
 *
 * ```c
 * NYA_SimulationRun* run = nya_simulation_create(.seed = 0x1234, .step_count = 10000);
 *
 * nya_simulation_action_add(run, "spawn", 40, spawn_something);
 * nya_simulation_action_add(run, "tick", 30, tick_the_world);
 * nya_simulation_fault_add(run, "corrupt_save", 2, corrupt_the_save_file);
 * nya_simulation_check_add(run, "counts agree", counts_agree);
 *
 * u32 failures = nya_simulation_run(run);
 * nya_simulation_destroy(run);
 * ```
 *
 * ## Why a counter hash and not an RNG
 *
 * Every draw is `siphash(seed, step, draw_index)`. A stateful generator would make each draw depend on
 * how many draws came before it anywhere in the program, so adding one call inside one action changes
 * every later decision in the run and a seed from yesterday stops reproducing. Hashing the coordinate
 * keeps a step's decisions local to that step.
 *
 * ## Why actions and not a script
 *
 * A hand written sequence tests the order somebody thought of. The interesting failures are in the
 * orders nobody thought of: a despawn between a body attach and the step that reads it, a reload
 * during a save, a restart between two halves of a write. Composing random sequences out of atomic
 * actions is how those get reached, and the weights are how the shape of the run is steered without
 * naming any particular sequence.
 *
 * ## Faults are actions
 *
 * A fault is registered in the same table as everything else, with a small weight, so it appears in the
 * history, is replayed by the seed, and is reported by name. There is no separate fault schedule to
 * keep in step with the action stream.
 *
 * Rejected: a separate fault injector driven by its own generator. It made a failing run reproducible
 * only if both generators were seeded and stepped identically, which is two things to get right where
 * one will do.
 * */
#pragma once

#ifdef NYA_TESTING

#include "nyangine/base/base_arena.h"
#include "nyangine/base/base_attributes.h"
#include "nyangine/base/base_reflection.h"
#include "nyangine/base/base_types.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * CONSTANTS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/**
 * Actions one run may register. The engine set is around twenty and a game adds a handful; sixty-four
 * is room for both with the weights table still one cache line per field.
 * */
#ifndef NYA_SIMULATION_MAX_ACTIONS
#define NYA_SIMULATION_MAX_ACTIONS 64
#endif

/** Invariants checked after every action. */
#ifndef NYA_SIMULATION_MAX_CHECKS
#define NYA_SIMULATION_MAX_CHECKS 32
#endif

/**
 * Steps kept in the history a failure prints. The last ones are what matters; a run of a million steps
 * would not be read past its tail, and the seed replays the rest.
 * */
#ifndef NYA_SIMULATION_HISTORY_MAX
#define NYA_SIMULATION_HISTORY_MAX 64
#endif

/** Failures reported in full before the rest are only counted. Same reasoning as NYA_CHECK_REPORT_MAX. */
#ifndef NYA_SIMULATION_REPORT_MAX
#define NYA_SIMULATION_REPORT_MAX 8
#endif

/** Longest a name may be, so the summary table lines up without measuring twice. */
#define NYA_SIMULATION_NAME_MAX 48

/**
 * The siphash key the entropy is drawn under.
 *
 * Nothing up my sleeve: the two halves are the ASCII of "nyangine" and "simulate", so anybody can
 * check where they came from. The key only has to be fixed, since the seed is the secret nobody is
 * keeping.
 * */
#define NYA_SIMULATION_HASH_KEY_LOW  0x6E79616E67696E65ULL
#define NYA_SIMULATION_HASH_KEY_HIGH 0x73696D756C617465ULL

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * TYPES
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

typedef struct NYA_SimulationRun     NYA_SimulationRun;
typedef struct NYA_SimulationAction  NYA_SimulationAction;
typedef struct NYA_SimulationCheck   NYA_SimulationCheck;
typedef struct NYA_SimulationOptions NYA_SimulationOptions;

/** One atomic action. Takes the run, draws whatever it needs from it, and leaves the world consistent. */
typedef void (*NYA_SimulationActionFn)(NYA_SimulationRun* run);

/** One invariant. Calls nya_simulation_fail for anything it does not like. */
typedef void (*NYA_SimulationCheckFn)(NYA_SimulationRun* run);

struct NYA_SimulationAction {
    NYA_ConstCString name;

    /** Relative frequency. Zero never runs, which is how an action is disabled without removing it. */
    u32 weight;

    NYA_SimulationActionFn run;

    /** Whether this action is a deliberate fault. Reported separately, because the mix matters. */
    b8 is_fault;

    /** Times it was taken. The coverage report at the end of a run. */
    u64 taken;
};

struct NYA_SimulationCheck {
    NYA_ConstCString      name;
    NYA_SimulationCheckFn run;
};

/** Everything a run is configured with. Defaults are usable; only the seed usually differs. */
struct NYA_SimulationOptions {
    /** What the whole run is derived from. Printed on failure, and enough to replay it. */
    u64 seed;

    /** Actions to take. */
    u64 step_count;

    /** Nanoseconds the clock advances per simulated tick. Default 16ms, the engine's own step. */
    u64 time_step_ns;

    /** Printed as the run goes. Off by default, since a passing run should say almost nothing. */
    b8 verbose;

    /** The scenario's own state, handed to every action. The harness never looks inside. */
    void* user_data;
};

#define _NYA_SIMULATION_DEFAULT_OPTIONS .step_count = 1000, .time_step_ns = 16000000

struct NYA_SimulationRun {
    /** Owns the run and everything it allocates. Destroyed with it. */
    NYA_Arena* allocator;

    u64 seed;
    u64 step_count;
    u64 time_step_ns;
    b8  verbose;

    /** Steps taken so far. The second coordinate every draw is hashed under. */
    u64 step;

    /**
     * Draws taken during this step, reset at the top of each one. The third coordinate.
     * */
    u64 draw;

    /** Simulated nanoseconds since the run started. Never read off a clock, never waited on. */
    u64 clock_ns;

    NYA_SimulationAction actions[NYA_SIMULATION_MAX_ACTIONS];
    u32                  action_count;

    /** Sum of every weight, so a pick is one modulo and one walk. */
    u64 weight_total;

    NYA_SimulationCheck checks[NYA_SIMULATION_MAX_CHECKS];
    u32                 check_count;

    /** The last actions taken, newest last, as indices into `actions`. A ring; see `history_count`. */
    u16 history[NYA_SIMULATION_HISTORY_MAX];

    /** Actions taken in total. The ring holds the last NYA_SIMULATION_HISTORY_MAX of them. */
    u64 history_count;

    u32 failures;

    void* user_data;
};

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * FUNCTIONS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/*
 * ─────────────────────────────────────────────────────────
 * LIFETIME
 * ─────────────────────────────────────────────────────────
 */

#define nya_simulation_create(...) nya_simulation_create_with_options((NYA_SimulationOptions){ _NYA_SIMULATION_DEFAULT_OPTIONS, __VA_ARGS__ })

NYA_API NYA_SimulationRun* nya_simulation_create_with_options(NYA_SimulationOptions options) __attr_no_discard;

/** Idempotent, and a no-op on null, so a failing run can be torn down from anywhere. */
NYA_API void nya_simulation_destroy(NYA_SimulationRun* run);

/*
 * ─────────────────────────────────────────────────────────
 * ACTIONS AND CHECKS
 * ─────────────────────────────────────────────────────────
 */

/** Registers an action under `name`, with `weight` relative to every other registered action. */
NYA_API void nya_simulation_action_add(NYA_SimulationRun* run, NYA_ConstCString name, u32 weight, NYA_SimulationActionFn action);

/**
 * The same, marked as a deliberate fault so the report separates "what the program did" from "what was
 * done to it". Give these small weights: a run that is mostly faults exercises the recovery path and
 * nothing else.
 * */
NYA_API void nya_simulation_fault_add(NYA_SimulationRun* run, NYA_ConstCString name, u32 weight, NYA_SimulationActionFn fault);

/** Registers an invariant, run after every action. This is the oracle, next to the engine's asserts. */
NYA_API void nya_simulation_check_add(NYA_SimulationRun* run, NYA_ConstCString name, NYA_SimulationCheckFn check);

/*
 * ─────────────────────────────────────────────────────────
 * THE RUN
 * ─────────────────────────────────────────────────────────
 */

/**
 * Takes every step, running one action and then every check. Returns the failures recorded.
 *
 * Nothing here catches a crash: an assertion firing inside an action is the intended outcome of a
 * simulation that found a bug, and the sanitizer report plus the history this prints on the way out is
 * the bug report. `nya_simulation_fail` is for the softer kind, where the run can carry on and find
 * more.
 * */
NYA_API u32 nya_simulation_run(NYA_SimulationRun* run);

/** Prints the seed, the step, the recent history and the command that replays it. */
NYA_API void nya_simulation_report(const NYA_SimulationRun* run);

/** Records a failure and prints it with everything needed to replay the run. */
NYA_API void nya_simulation_fail(NYA_SimulationRun* run, NYA_ConstCString format, ...) __attr_fmt_printf(2, 3);

/*
 * ─────────────────────────────────────────────────────────
 * ENTROPY
 * ─────────────────────────────────────────────────────────
 */

/**
 * The next draw: `siphash(seed, step, draw)`, with `draw` then incremented.
 * */
NYA_API u64 nya_simulation_roll(NYA_SimulationRun* run);

/** A draw below `limit`. Zero for a limit of zero, since there is nothing below it. */
NYA_API u64 nya_simulation_below(NYA_SimulationRun* run, u64 limit);

/** True `percent` of the time, out of a hundred. */
NYA_API b8 nya_simulation_chance(NYA_SimulationRun* run, u32 percent);

/** A draw within [low, high]. `low` when the two are equal or inverted. */
NYA_API f32 nya_simulation_range_f32(NYA_SimulationRun* run, f32 low, f32 high);

/**
 * A draw within [low, high] that is occasionally an edge instead: zero, the bounds themselves, and the
 * values that have historically broken things. Random floats in the middle of a range find very little;
 * the boundaries find everything.
 * */
NYA_API f32 nya_simulation_shaped_f32(NYA_SimulationRun* run, f32 low, f32 high);

/*
 * ─────────────────────────────────────────────────────────
 * TIME
 * ─────────────────────────────────────────────────────────
 */

/** Simulated nanoseconds since the run began. Nothing in a simulation reads a real clock. */
NYA_API u64 nya_simulation_now_ns(const NYA_SimulationRun* run) __attr_no_discard;

/** Moves the simulated clock forward. The only way it ever moves. */
NYA_API void nya_simulation_advance(NYA_SimulationRun* run, u64 nanoseconds);

/** The configured step as seconds, for handing to an `_update(delta_time_s)`. */
NYA_API f32 nya_simulation_delta_s(const NYA_SimulationRun* run) __attr_no_discard;

/*
 * ─────────────────────────────────────────────────────────
 * WELL SHAPED DATA
 * ─────────────────────────────────────────────────────────
 */

/**
 * Fills any reflected type with values that look like what the field is for, driven by the run's
 * entropy so the same seed fills it the same way.
 *
 * ```c
 * NYA_PostBloom bloom = { 0 };
 * nya_simulation_fill(run, nya_reflect_of(NYA_PostBloom), &bloom);
 * ```
 *
 * The field's hint decides the shape: a COLOR gets components in [0, 1], a POSITION world-ish
 * coordinates, an enum one of its declared variants, a bitflags field a subset of its declared bits.
 * Anything without a hint gets a shaped draw for its primitive type, edges included.
 *
 * This is where reflection earns its place in the simulator: the data an action needs comes from the
 * type rather than from a hand written generator per struct, so a field added to a config struct is
 * exercised without anybody editing the simulator. The action *list* is still written by hand, because
 * reflection describes types and not the operations on them; see the report at the top of this file.
 * */
NYA_API void nya_simulation_fill(NYA_SimulationRun* run, const NYA_TypeReflection* type, void* instance);

#endif // NYA_TESTING
