/**
 * @file nn_neat.h
 *
 * NEAT: NeuroEvolution of Augmenting Topologies.
 *
 * Evolves both the weights *and the shape* of a network, starting from the smallest topology that
 * connects the inputs to the outputs and adding nodes and connections only when they earn their
 * keep. That is what makes it useful without a training set: it needs a fitness function rather
 * than labelled data, so "how well did this play" is enough.
 *
 * Stanley & Miikkulainen, 2002: https://nn.cs.utexas.edu/downloads/papers/stanley.ec02.pdf
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
 *
 * ## Three ideas, and why each is there
 *
 * **Innovation numbers.** Every structural change gets a number, and the same change appearing in
 * two genomes in the same generation gets the *same* number. That is what lets two differently
 * shaped networks be lined up gene by gene, which is what makes crossover between them meaningful
 * rather than destructive.
 *
 * **Speciation.** Genomes are grouped by how similar their genes are, and compete mainly within
 * their group. A new structure is almost always worse before it is better, and without this it is
 * killed off in the generation it appears in.
 *
 * **Explicit fitness sharing.** A genome's fitness is divided by the size of its species, so a
 * species that starts winning cannot grow without bound and squeeze the others out.
 *
 * ## Memory
 *
 * Everything lives in one arena owned by the context, and `nya_nn_neat_destroy` frees the lot.
 * Networks hold **dynamic arrays**, so a network is not memcpy-able — copying the struct copies the
 * array pointers rather than the arrays, and both copies then share genes. Use
 * nya_nn_neat_network_clone, which is what the evolution does internally.
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
 *
 * A ceiling rather than a preference. NEAT adds nodes and never removes them, so an unbounded run
 * grows unbounded genomes — and nya_nn_neat_network_run needs one scratch value per node on the
 * stack, which turns "runs for a long time" into a stack overflow with no warning.
 *
 * It is also a quality control: a sweep on function fitting found the *smaller* topologies
 * generalized better, with the champion of a good run carrying a single hidden node while a bloated
 * one carried six and scored worse. Hitting this cap means structural mutation stops, not that
 * anything fails.
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
 *
 * Elitism costs a species one of its offspring slots, which is a large fraction of a small species'
 * whole chance to explore. Below this it is not worth it; at or above it, losing the best genome
 * found to a bad mutation is the greater risk. Five is the paper's figure.
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
 *
 * Named rather than implicit because a generation is where every NEAT run goes wrong, and "the
 * population collapsed" is not a debuggable statement until you know whether it collapsed during
 * culling or during respeciation. Also the unit the observer reports against.
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
 *
 * A snapshot rather than a pointer into the population: the arenas a generation works in are swapped
 * and reset at the end of the step, so anything the observer keeps has to be a copy of numbers.
 * */
struct NYA_NeatTrace {
    NYA_NeatPhase phase;

    /** The generation being stepped. Not yet incremented — it increments once the step completes. */
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
 *
 * For logging, for a graph, for a test that asserts the population never empties. It runs inside the
 * step, so it must not evolve anything — treat `neat` as read only.
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
     *
     * Borrowed, not copied: the caller's string has to outlive the network. In practice these are
     * literals, which outlive everything.
     * */
    NYA_ConstCString label;

    f64 value;
};

/**
 * One connection gene.
 *
 * `enabled` rather than deleting: a disabled gene still carries its innovation number and its
 * weight, so it still lines up during crossover and can be switched back on. Deleting it would lose
 * the history that makes two genomes comparable.
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
     *
     * NEAT networks may be recurrent, so there is no single pass that settles them. Each step
     * propagates values one connection further; a feed forward network needs as many steps as it is
     * deep, and a recurrent one is simply cut off here.
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
     *
     * A fixed representative rather than "any current member": comparing against a moving target
     * lets a species drift arbitrarily far from where it started, one small step at a time.
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
     *
     * Copied into the population, not taken over — the caller's network is untouched and stays
     * theirs to free. The original implementation destroyed it, which made the seed unusable
     * afterwards for no reason the caller could see.
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
     * ── Speciation ──
     *
     * Distance is c1·E/N + c2·D/N + c3·W̄: excess genes, disjoint genes, and the average weight
     * difference of the genes the two genomes share. Two genomes are the same species when that
     * comes out below the threshold.
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
     * ── Structural mutation ──
     *
     * Deliberately rarer than weight mutation, and adding a node rarer still: a new node is two new
     * connections and an immediate fitness drop, so a population that adds them freely never settles
     * long enough to tune anything.
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
     *
     * A rate rather than a per-frame count, so a run looks the same on a 60Hz monitor and a 144Hz
     * one — stepping once per frame would silently evolve at more than twice the speed on the
     * second. Values below one are meaningful: 0.5 is a generation every two seconds, roughly the
     * pace at which a topology change is watchable.
     *
     * Defaults to 10. A headless trainer that wants to go as fast as it can should call
     * nya_nn_neat_step directly in a loop rather than asking this for an enormous rate.
     * */
    f32 generations_per_second;

    /**
     * The most generations one nya_nn_neat_step_for call may run, however far behind it is.
     *
     * A frame that stalls — a breakpoint, a window drag — leaves a large delta, and without a cap
     * the next call tries to make all of it up at once and stalls again. Falling behind is better
     * than compounding the hitch. Defaults to 8.
     * */
    u32 max_steps_per_frame;

    /**
     * Wall time one nya_nn_neat_step_for call may spend, in milliseconds. Zero disables the budget.
     *
     * max_steps_per_frame counts generations, which only bounds the frame while a generation costs
     * what it did when the number was chosen. It does not: evaluate is the population's total
     * connection count times the trial length, and NEAT adds structure without ever being obliged to
     * remove it. On the gnyame demo one generation grew from 6 ms to 59 ms over seventy seconds, so a
     * cap of two generations still allowed 118 ms in a single frame — a sixteen millisecond timestep
     * rendering at five frames a second.
     *
     * Checked between generations rather than inside one. A single generation is indivisible here,
     * so this bounds the overshoot to one generation rather than eliminating it; what it does
     * guarantee is that the cost of a frame stops scaling with how bloated the population has become.
     *
     * Defaults to 0 for callers that drive this themselves, but any interactive caller wants it set.
     * */
    f64 max_step_milliseconds;

    /**
     * Species count the compatibility threshold is retuned to hold. Zero keeps the threshold fixed.
     *
     * A fixed threshold does not survive contact with a population whose genomes drift together: on
     * the gnyame demo the species count fell 22 → 8 → 6 → 1 within thirty seconds and never recovered.
     * One species means fitness sharing has nothing to share between and the whole population walks
     * as one, which is both the end of the diversity NEAT exists to protect and the start of
     * unbounded growth in evaluate cost.
     *
     * The original paper sets the threshold by hand per problem. Every practical implementation since
     * makes it a controller instead, because the value that gives ten species in generation one gives
     * one in generation five hundred. See compatibility_threshold_adjust and _min/_max.
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

    /**
     * Called after each phase of every generation. Null — the default — costs nothing.
     *
     * The alternative would be logging from inside the step, which is either too quiet to diagnose
     * anything or far too loud to leave on. A hook lets the caller decide, and lets a test watch the
     * population without the library knowing what a test is.
     * */
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
 *
 * The reason this exists rather than assigning the struct — a network holds dynamic arrays, so a
 * plain copy leaves two networks mutating the same genes.
 * */
NYA_API NYA_NeatNetwork* nya_nn_neat_network_clone(NYA_Arena* arena, const NYA_NeatNetwork* source) __attr_no_discard;

/** Steps the network `activation_steps` times. Sensors keep their values; everything else updates. */
NYA_API void nya_nn_neat_network_run(NYA_NeatNetwork* network);

/**
 * Zeroes the hidden and output nodes, leaving sensors and bias alone.
 *
 * Needed between unrelated inputs: without it a recurrent network carries state from the previous
 * trial into the next one, and the same input gives a different answer depending on what came
 * before.
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
 * A genome round trips through NYA_Object, so serde already knows how to write it and it lands in the
 * same shape a query row or an HTTP body does — which means an evolved network can go over the curl
 * plugin or into a sqlite column with no extra code.
 *
 * **The native .nya format by default, JSON when the path ends in .json.** A custom binary format
 * would be smaller, and smaller is not what matters for a few kilobytes of genes; being able to open
 * a saved network and read which connection carries which weight is worth far more. .nya gives that
 * *and* is lossless — every value carries its type, so a u32 returns a u32 — *and* carries a
 * checksum. JSON is there for interop with things outside the engine, and costs the exact types.
 *
 * **The activation function is not saved.** It is a function pointer, and nothing sensible can be
 * written for it — so loading takes one as an argument. Handing back a network that cannot run until
 * someone remembers to set a field would be the worse alternative.
 *
 * Node *values* are not saved either. They are the state of one evaluation, not part of the genome,
 * and a loaded network flushes to zero like a fresh one.
 */

/**
 * Converts a genome into a plain object: topology, weights, innovation numbers and labels.
 *
 * Everything is allocated from `arena`. Useful on its own for embedding a network inside a larger
 * save file rather than writing it to a path of its own.
 * */
NYA_API NYA_Object* nya_nn_neat_network_to_object(NYA_Arena* arena, const NYA_NeatNetwork* network) __attr_no_discard;

/**
 * Rebuilds a genome from an object produced by nya_nn_neat_network_to_object.
 *
 * `activation_function` is supplied rather than restored, for the reason above. Labels are copied
 * into `arena`, so the loaded network does not borrow from the object it came from and the object
 * may be discarded immediately.
 *
 * Fails rather than asserting on a malformed object: a save file is data from outside the program,
 * and a corrupt one should be an error the caller can report, not a crash.
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
 *
 * Steeper than the textbook sigmoid on purpose — it saturates sooner, which makes it easier for
 * evolution to find weights that behave like a switch. Right for **classification**, where the
 * answer is one of a few discrete things; wrong for fitting a smooth curve, where that same
 * saturation throws away most of the output range. See nya_nn_neat_sigmoid_gentle.
 * */
NYA_API f64 nya_nn_neat_sigmoid(f64 value) __attr_no_discard;

/**
 * The plain logistic sigmoid: 1 / (1 + e^-x).
 *
 * What **regression** wants — approximating a smooth function rather than deciding between two
 * answers. The steepened one above saturates so hard that most of its input range maps to nearly 0
 * or nearly 1, which leaves very little of the curve available for representing values in between.
 *
 * Measured on fitting x^y over a grid: swapping to this cut the held-out mean absolute error from
 * 0.118 to 0.081, and together with a denser training grid from 0.118 to 0.028.
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
 *
 * Owns one arena; nya_nn_neat_destroy frees all of it. The seed is copied, so it remains the
 * caller's.
 * */
NYA_API NYA_Neat* nya_nn_neat_create(NYA_NeatConfig config) __attr_no_discard;
NYA_API void      nya_nn_neat_destroy(NYA_Neat* neat);

/** One generation: evaluate, speciate, cull, reproduce, mutate. */
NYA_API void nya_nn_neat_step(NYA_Neat* neat);

/**
 * Runs however many generations `delta_time_s` is worth, at the configured rate.
 *
 * The call a game loop makes. Fractional generations are carried between calls rather than
 * truncated, so a rate below the frame rate still advances instead of rounding to zero every frame
 * and never moving.
 *
 * Returns how many generations actually ran, which is zero on most frames at a slow rate.
 * */
NYA_API u32 nya_nn_neat_step_for(NYA_Neat* neat, f32 delta_time_s);

/** The fittest genome of the last evaluated generation. Null before the first nya_nn_neat_step. */
NYA_API NYA_NeatNetwork* nya_nn_neat_best(NYA_Neat* neat) __attr_no_discard;

NYA_API u32 nya_nn_neat_generation(const NYA_Neat* neat) __attr_no_discard;
NYA_API u32 nya_nn_neat_species_count(const NYA_Neat* neat) __attr_no_discard;
NYA_API f64 nya_nn_neat_fitness_max(const NYA_Neat* neat) __attr_no_discard;
NYA_API f64 nya_nn_neat_fitness_average(const NYA_Neat* neat) __attr_no_discard;
