/**
 * @file nn_neat.h
 *
 * ```c
 * NYA_NeatNetwork* seed = nya_nn_neat_network_create(arena);
 * nya_nn_neat_network_push_sensor(seed, "x");
 * nya_nn_neat_network_push_sensor(seed, "y");
 * nya_nn_neat_network_push_output(seed, "result");
 *
 * NYA_Neat* neat = nya_nn_neat_create((NYA_NeatConfig){
 *     .seed = seed, .trial_function = xor_trial, .activation_function = nya_nn_neat_sigmoid,
 * });
 * defer nya_nn_neat_destroy(neat);
 *
 * for (u32 generation = 0; generation < 200; generation++) nya_nn_neat_step(neat);
 * NYA_NeatNetwork* best = nya_nn_neat_best(neat);
 * ```
 * */
#pragma once

#include "nyangine/base/base_arena.h"
#include "nyangine/base/base_array.h"
#include "nyangine/base/base_attributes.h"
#include "nyangine/base/base_types.h"
#include "nyangine/math/math_random.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * CONSTANTS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/**
 * The most nodes one genome may grow to.
 * */
#ifndef NYA_NEAT_MAX_NODES
#define NYA_NEAT_MAX_NODES 256
#endif

/** The same, for connections. Reaching it stops add-connection mutations rather than failing. */
#ifndef NYA_NEAT_MAX_CONNECTIONS
#define NYA_NEAT_MAX_CONNECTIONS 2048
#endif

/**
 * Species size at which the champion is copied into the next generation unchanged.
 * */
#ifndef NYA_NEAT_ELITISM_MIN_SPECIES_SIZE
#define NYA_NEAT_ELITISM_MIN_SPECIES_SIZE 5
#endif

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * TYPES
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

typedef struct NYA_NeatNode       NYA_NeatNode;
typedef struct NYA_NeatConnection NYA_NeatConnection;
typedef struct NYA_NeatNetwork    NYA_NeatNetwork;
typedef struct NYA_NeatSpecies    NYA_NeatSpecies;
typedef struct NYA_NeatConfig     NYA_NeatConfig;
typedef struct NYA_Neat           NYA_Neat;
typedef enum NYA_NeatNodeKind     NYA_NeatNodeKind;
typedef enum NYA_NeatPhase        NYA_NeatPhase;
typedef struct NYA_NeatTrace      NYA_NeatTrace;

/** Scores one network. Called once per genome per generation; it is the whole cost of evolving. */
typedef f64 (*NYA_NeatTrialFunction)(NYA_NeatNetwork* network);

/**
 * The five things a generation does, in the order it does them.
 * */
enum NYA_NeatPhase {
    /** Every genome scored by the trial function. The only phase whose cost is the caller's. */
    NYA_NEAT_PHASE_EVALUATE,

    /** Fitness divided by species size, so a large species cannot simply out-vote a small one. */
    NYA_NEAT_PHASE_SHARE,

    /** Stagnant species barred from breeding, and the population narrowed if it has stalled. */
    NYA_NEAT_PHASE_CULL,

    /** Offspring bred in proportion to adjusted fitness, then mutated. */
    NYA_NEAT_PHASE_REPRODUCE,

    /** Children placed against the frozen representatives; the species set for the next generation. */
    NYA_NEAT_PHASE_RESPECIATE,

    NYA_NEAT_PHASE_COUNT,
};

/**
 * What one phase did. Handed to the observer as each phase finishes.
 * */
struct NYA_NeatTrace {
    NYA_NeatPhase phase;

    /** The generation being stepped. Incremented once the step completes. */
    u32 generation;

    /** Species and genomes alive as the phase finished. Watching these is how you see a collapse. */
    u32 species_count;
    u32 population_count;

    /** Best and mean raw fitness known at that moment. Zero before NYA_NEAT_PHASE_EVALUATE has run. */
    f64 fitness_max;
    f64 fitness_average;

    /** Wall time the phase took. Evaluate is normally almost all of it. */
    f64 duration_ms;
};

/**
 * Called as each phase of a generation finishes. Null disables it, which is the default.
 * */
typedef void (*NYA_NeatObserverFunction)(const NYA_Neat* neat, const NYA_NeatTrace* trace, void* user_data);

typedef f64 (*NYA_NeatActivationFunction)(f64 value);

enum NYA_NeatNodeKind {
    /** Always one. Gives the network a constant to lean on, so a node can learn a threshold. */
    NYA_NEAT_NODE_BIAS,

    /** An input. Set by name before each run. */
    NYA_NEAT_NODE_SENSOR,

    /** Added by evolution, never by hand. */
    NYA_NEAT_NODE_HIDDEN,

    /** An output. Read by name after each run. */
    NYA_NEAT_NODE_OUTPUT,

    NYA_NEAT_NODE_KIND_COUNT,
};

struct NYA_NeatNode {
    u32              index;
    NYA_NeatNodeKind kind;

    /**
     * How a sensor or output is addressed. Null for hidden nodes, which are never named.
     * */
    NYA_ConstCString label;

    f64 value;
};

/**
 * One connection gene.
 * */
struct NYA_NeatConnection {
    u32 in;
    u32 out;
    f64 weight;
    b8  enabled;
    u32 innovation_number;
};

nya_derive_array(NYA_NeatNode);
nya_derive_array(NYA_NeatConnection);

struct NYA_NeatNetwork {
    NYA_ArrayᐸNYA_NeatNodeᐳ*       nodes;
    NYA_ArrayᐸNYA_NeatConnectionᐳ* connections;

    NYA_NeatActivationFunction activation_function;

    /**
     * How many times the network is stepped per run.
     * */
    u32 activation_steps;

    f64 fitness_raw;

    /** fitness_raw divided by the size of the species. See the note on fitness sharing above. */
    f64 fitness_adjusted;
};

nya_derive_array(NYA_NeatNetwork);

struct NYA_NeatSpecies {
    /**
     * The genome new members are compared against, frozen at the start of the generation.
     * */
    NYA_NeatNetwork representative;

    NYA_ArrayᐸNYA_NeatNetworkᐳ* members;

    f64 fitness_max;
    f64 fitness_adjusted_total;
    u32 generations_without_improvement;
};

nya_derive_array(NYA_NeatSpecies);

struct NYA_NeatConfig {
    /**
     * The starting topology: sensors, outputs and optionally a bias, with no connections.
     * */
    NYA_NeatNetwork* seed;

    NYA_NeatTrialFunction      trial_function;
    NYA_NeatActivationFunction activation_function;
    u32                        activation_steps;

    /** Genomes per generation. Defaults to 150, which is what the paper used. */
    u32 population_size;

    /** Hex seed for the RNG, so a run can be reproduced. Null picks a random one. */
    NYA_ConstCString rng_seed;

    /*
     * Speciation
     *
     * Distance is c1·E/N + c2·D/N + c3·W̄: excess genes, disjoint genes, and the average weight
     * difference of shared genes. Two genomes are one species when it is below the threshold.
     */
    f64 compatibility_threshold;
    f64 compatibility_coefficient_excess;
    f64 compatibility_coefficient_disjoint;
    f64 compatibility_coefficient_weight;

    /*
     * ── Weight mutation ──
     */
    f64 weight_range_min;
    f64 weight_range_max;
    f64 mutation_weight_perturbation_chance;
    f64 mutation_weight_perturbation_percent_max;
    f64 mutation_weight_reroll_chance;

    /*
     * Structural mutation
     *
     * Rarer than weight mutation, and adding a node rarer still. A new node is two new connections and an
     * immediate fitness drop, so a population that adds them freely never settles long enough to tune.
     */
    f64 mutation_add_connection_chance;
    f64 mutation_add_node_chance;

    /*
     * ── Reproduction ──
     */
    f64 crossover_chance;

    /** The fraction of a species allowed to breed, best first. The rest are dead ends. */
    f64 crossover_cutoff_percentage;

    /** A gene disabled in either parent has this chance of coming back on in the child. */
    f64 crossover_revive_disabled_chance;

    /*
     * ── Pacing ──
     *
     * Only nya_nn_neat_step_for reads these; nya_nn_neat_step always runs exactly one generation.
     */

    /**
     * Generations per second, for nya_nn_neat_step_for.
     * */
    f32 generations_per_second;

    /**
     * The most generations one nya_nn_neat_step_for call may run, however far behind it is.
     * */
    u32 max_steps_per_frame;

    /**
     * Wall time one nya_nn_neat_step_for call may spend, in milliseconds. Zero disables the budget.
     * */
    f64 max_step_milliseconds;

    /**
     * Species count the compatibility threshold is retuned to hold. Zero keeps the threshold fixed.
     * */
    u32 target_species_count;

    /** How much the threshold moves per generation when the species count is off target. */
    f64 compatibility_threshold_adjust;

    /** Floor and ceiling for the retuned threshold, so the controller cannot run away. */
    f64 compatibility_threshold_min;
    f64 compatibility_threshold_max;

    /** Generations a species may go without improving before it stops being allowed offspring. */
    u32 species_stagnation_threshold;

    /** The same, for the whole population: everything but the best two species is culled. */
    u32 population_stagnation_threshold;

    /*
     * ── Observation ──
     */

    /** Called after each phase of every generation. Null by default, and then free. */
    NYA_NeatObserverFunction observer;

    /** Passed through to `observer` untouched. */
    void* observer_user_data;
};

/** A human readable name for a phase, for a log line or an overlay. */
NYA_API NYA_ConstCString nya_nn_neat_phase_name(NYA_NeatPhase phase) __attr_no_discard;

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * FUNCTIONS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/*
 * ─────────────────────────────────────────────────────────
 * NETWORK
 * ─────────────────────────────────────────────────────────
 */

/** An empty network. Push the sensors and outputs it should have, then hand it over as a seed. */
NYA_API NYA_NeatNetwork* nya_nn_neat_network_create(NYA_Arena* arena) __attr_no_discard;

/**
 * A deep copy: the arrays are duplicated rather than shared.
 * */
NYA_API NYA_NeatNetwork* nya_nn_neat_network_clone(NYA_Arena* arena, const NYA_NeatNetwork* source) __attr_no_discard;

/** Steps the network `activation_steps` times. Sensors keep their values; everything else updates. */
NYA_API void nya_nn_neat_network_run(NYA_NeatNetwork* network);

/**
 * Zeroes the hidden and output nodes, leaving sensors and bias alone.
 * */
NYA_API void nya_nn_neat_network_flush(NYA_NeatNetwork* network);

NYA_API void nya_nn_neat_network_push_bias(NYA_NeatNetwork* network, NYA_ConstCString label);
NYA_API void nya_nn_neat_network_push_sensor(NYA_NeatNetwork* network, NYA_ConstCString label);
NYA_API void nya_nn_neat_network_push_output(NYA_NeatNetwork* network, NYA_ConstCString label);

/** Sets a sensor by name. Asserts when no sensor has that label, which is a wiring mistake. */
NYA_API void nya_nn_neat_network_set_sensor(NYA_NeatNetwork* network, NYA_ConstCString label, f64 value);

/** Reads an output by name. Asserts when no output has that label. */
NYA_API f64 nya_nn_neat_network_get_output(NYA_NeatNetwork* network, NYA_ConstCString label) __attr_no_discard;

/*
 * ─────────────────────────────────────────────────────────
 * SAVING AND LOADING
 * ─────────────────────────────────────────────────────────
 */

/*
 * A genome round trips through NYA_Object, so serde writes it and it can go over the curl plugin or
 * into a sqlite column with no extra code.
 */

/**
 * Converts a genome into a plain object: topology, weights, innovation numbers and labels.
 * */
NYA_API NYA_Object* nya_nn_neat_network_to_object(NYA_Arena* arena, const NYA_NeatNetwork* network) __attr_no_discard;

/**
 * Rebuilds a genome from an object produced by nya_nn_neat_network_to_object.
 * */
NYA_API NYA_Error nya_nn_neat_network_from_object(
    NYA_Arena* arena, const NYA_Object* object, NYA_NeatActivationFunction activation_function, OUT NYA_NeatNetwork** out_network
) __attr_no_discard;

/** Writes a genome to `path`. Native format, or JSON if the path ends in .json. Creates or overwrites. */
NYA_API NYA_Error nya_nn_neat_network_save(const NYA_NeatNetwork* network, NYA_ConstCString path) __attr_no_discard;

/** Reads a genome written by nya_nn_neat_network_save. See the note above on the activation function. */
NYA_API NYA_Error nya_nn_neat_network_load(
    NYA_Arena* arena, NYA_ConstCString path, NYA_NeatActivationFunction activation_function, OUT NYA_NeatNetwork** out_network
) __attr_no_discard;

/*
 * ─────────────────────────────────────────────────────────
 * ACTIVATION
 * ─────────────────────────────────────────────────────────
 */

/**
 * The steepened sigmoid the paper uses: 1 / (1 + e^(-4.9x)).
 * */
NYA_API f64 nya_nn_neat_sigmoid(f64 value) __attr_no_discard;

/**
 * The plain logistic sigmoid: 1 / (1 + e^-x).
 * */
NYA_API f64 nya_nn_neat_sigmoid_gentle(f64 value) __attr_no_discard;

/** Hyperbolic tangent, for when an output should be able to go negative. */
NYA_API f64 nya_nn_neat_tanh(f64 value) __attr_no_discard;

/** Rectified linear. Cheap, unbounded above; watch for outputs running away. */
NYA_API f64 nya_nn_neat_relu(f64 value) __attr_no_discard;

/*
 * ─────────────────────────────────────────────────────────
 * EVOLUTION
 * ─────────────────────────────────────────────────────────
 */

/**
 * Creates a population seeded from `config.seed`, with everything unset filled in from the paper.
 * */
NYA_API NYA_Neat* nya_nn_neat_create(NYA_NeatConfig config) __attr_no_discard;
NYA_API void      nya_nn_neat_destroy(NYA_Neat* neat);

/** One generation: evaluate, speciate, cull, reproduce, mutate. */
NYA_API void nya_nn_neat_step(NYA_Neat* neat);

/**
 * Runs however many generations `delta_time_s` is worth, at the configured rate.
 * */
NYA_API u32 nya_nn_neat_step_for(NYA_Neat* neat, f32 delta_time_s);

/** The fittest genome of the last evaluated generation. Null before the first nya_nn_neat_step. */
NYA_API NYA_NeatNetwork* nya_nn_neat_best(NYA_Neat* neat) __attr_no_discard;

NYA_API u32 nya_nn_neat_generation(const NYA_Neat* neat) __attr_no_discard;
NYA_API u32 nya_nn_neat_species_count(const NYA_Neat* neat) __attr_no_discard;
NYA_API f64 nya_nn_neat_fitness_max(const NYA_Neat* neat) __attr_no_discard;
NYA_API f64 nya_nn_neat_fitness_average(const NYA_Neat* neat) __attr_no_discard;
