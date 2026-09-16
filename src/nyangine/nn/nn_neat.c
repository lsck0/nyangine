#include "nyangine/nyangine.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API DECLARATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

struct NYA_Neat {
    NYA_NeatConfig config;

    /** Owns the context, the config's copy of the seed, and the best network snapshot. */
    NYA_Arena* allocator;

    /*
     * Two generation arenas, used alternately.
     */
    NYA_Arena* generation_allocators[2];
    u32        generation_allocator_index;

    /**
     * Holds nothing but the current best genome, and is reset before each new one is written.
     * */
    NYA_Arena* best_allocator;

    NYA_ArrayᐸNYA_NeatSpeciesᐳ* species;

    /** A pointer, aligned by hand; see nya_nn_neat_create. */
    NYA_RNG* rng;

    u32 generation;

    /** Fractional generations carried between nya_nn_neat_step_for calls. See NYA_NeatConfig. */
    f32 step_accumulator;

    /**
     * The innovations created this generation, cleared at the start of the next.
     * */
    NYA_ArrayᐸNYA_NeatConnectionᐳ* current_innovations;
    u32                            innovation_counter;

    /** A snapshot, not a pointer into the population, which is rebuilt every generation. */
    NYA_NeatNetwork* best;

    f64 fitness_max;
    f64 fitness_average;
    u32 generations_without_improvement;
};

NYA_INTERNAL u32 _nya_nn_neat_push_node(NYA_NeatNetwork* network, NYA_NeatNodeKind kind, NYA_ConstCString label);

/** A uniform double in [min, max]. The one random primitive everything else here is built from. */
NYA_INTERNAL f64 _nya_nn_neat_uniform(NYA_RNG* rng, f64 min, f64 max);

/** A uniform index in [0, count), or 0 when count is zero. */
NYA_INTERNAL u32 _nya_nn_neat_index(NYA_RNG* rng, u32 count);

/**
 * The innovation number for a connection between `in` and `out`.
 * */
NYA_INTERNAL u32 _nya_nn_neat_innovation_for(NYA_Neat* neat, u32 in, u32 out);

NYA_INTERNAL void _nya_nn_neat_mutate_weights(NYA_Neat* neat, NYA_NeatNetwork* network);
NYA_INTERNAL void _nya_nn_neat_mutate_add_connection(NYA_Neat* neat, NYA_NeatNetwork* network);
NYA_INTERNAL void _nya_nn_neat_mutate_add_node(NYA_Neat* neat, NYA_NeatNetwork* network);

/** A child of two parents, allocated from `arena`. Genes come from the fitter parent where they disagree. */
/**
 * Working state for one generation, threaded through the five phases.
 * */
typedef struct _NYA_NeatGeneration {
    /** The arena next generation is built in. The one the current population lives in is the other. */
    NYA_Arena* next_arena;

    /** Offspring, still grouped by the parent species. Produced by reproduce, consumed by respeciate. */
    NYA_ArrayᐸNYA_NeatSpeciesᐳ* next_species;

    /** Offspring regrouped by compatibility. Becomes neat->species. Null until respeciate has run. */
    NYA_ArrayᐸNYA_NeatSpeciesᐳ* respeciated;

    /** The one species stagnation culling is not allowed to take, when it would otherwise take all. */
    NYA_NeatSpecies* spared;

    /** Sum of adjusted fitness over species allowed to breed. The denominator of every share. */
    f64 adjusted_grand_total;

    /** Genomes alive at evaluate time. */
    u32 population_count;

    /** The whole population has stalled, which suspends per-species stagnation culling. */
    b8 population_stagnant;

    /** Start of the phase currently running, for the observer's duration. */
    u64 phase_start_ns;
} _NYA_NeatGeneration;

NYA_INTERNAL void            _nya_nn_neat_phase_evaluate(NYA_Neat* neat, _NYA_NeatGeneration* generation);
NYA_INTERNAL void            _nya_nn_neat_phase_share(NYA_Neat* neat, _NYA_NeatGeneration* generation);
NYA_INTERNAL void            _nya_nn_neat_phase_cull(NYA_Neat* neat, _NYA_NeatGeneration* generation);
NYA_INTERNAL void            _nya_nn_neat_phase_reproduce(NYA_Neat* neat, _NYA_NeatGeneration* generation);
NYA_INTERNAL void            _nya_nn_neat_phase_respeciate(NYA_Neat* neat, _NYA_NeatGeneration* generation);

/** One species' offspring, bred and mutated. The inner half of the reproduce phase. */
NYA_INTERNAL NYA_NeatSpecies _nya_nn_neat_breed_species(NYA_Neat* neat, _NYA_NeatGeneration* generation, NYA_NeatSpecies* species);

/** Whether a species is barred from breeding this generation. The stagnation rule, in one place. */
NYA_INTERNAL b8 _nya_nn_neat_species_is_barred(const NYA_Neat* neat, const _NYA_NeatGeneration* generation, const NYA_NeatSpecies* species) __attr_no_discard;

/** Times the phase that just ended and reports it, whether or not an observer is installed. */
NYA_INTERNAL void _nya_nn_neat_observe(NYA_Neat* neat, _NYA_NeatGeneration* generation, NYA_NeatPhase phase);

NYA_INTERNAL NYA_NeatNetwork* _nya_nn_neat_crossover(NYA_Neat* neat, NYA_Arena* arena, NYA_NeatNetwork* parent1, NYA_NeatNetwork* parent2);

/**
 * Sorts a genome's connections by innovation number.
 * */
NYA_INTERNAL void _nya_nn_neat_sort_connections(NYA_NeatNetwork* network);

/** Compatibility distance: how different two genomes are, in the paper's terms. */
NYA_INTERNAL f64 _nya_nn_neat_distance(const NYA_Neat* neat, const NYA_NeatNetwork* a, const NYA_NeatNetwork* b);

/** Fills anything the caller left at zero with the paper's value. */
NYA_INTERNAL void _nya_nn_neat_apply_config_defaults(NYA_NeatConfig* config);

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PUBLIC API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/*
 * ─────────────────────────────────────────────────────────
 * ACTIVATION
 * ─────────────────────────────────────────────────────────
 */

f64 nya_nn_neat_sigmoid(f64 value) {
    return 1.0 / (1.0 + exp(-4.9 * value));
}

f64 nya_nn_neat_sigmoid_gentle(f64 value) {
    return 1.0 / (1.0 + exp(-value));
}

f64 nya_nn_neat_tanh(f64 value) {
    return tanh(value);
}

f64 nya_nn_neat_relu(f64 value) {
    return value > 0.0 ? value : 0.0;
}

/*
 * ─────────────────────────────────────────────────────────
 * NETWORK
 * ─────────────────────────────────────────────────────────
 */

NYA_NeatNetwork* nya_nn_neat_network_create(NYA_Arena* arena) {
    nya_assert(arena != nullptr);

    NYA_NeatNetwork* network = nya_arena_alloc(arena, sizeof(NYA_NeatNetwork));

    *network = (NYA_NeatNetwork){
        .nodes            = nya_array_create(arena, NYA_NeatNode),
        .connections      = nya_array_create(arena, NYA_NeatConnection),
        .activation_steps = 1,
    };

    return network;
}

NYA_NeatNetwork* nya_nn_neat_network_clone(NYA_Arena* arena, const NYA_NeatNetwork* source) {
    nya_assert(arena != nullptr);
    nya_assert(source != nullptr);

    NYA_NeatNetwork* clone = nya_nn_neat_network_create(arena);

    clone->activation_function = source->activation_function;
    clone->activation_steps    = source->activation_steps;
    clone->fitness_raw         = source->fitness_raw;
    clone->fitness_adjusted    = source->fitness_adjusted;

    // element by element: assigning the struct would leave both networks sharing one set of genes.
    nya_array_foreach (source->nodes, node) nya_array_push_back(clone->nodes, *node);
    nya_array_foreach (source->connections, connection) nya_array_push_back(clone->connections, *connection);

    return clone;
}

void nya_nn_neat_network_run(NYA_NeatNetwork* network) {
    nya_assert(network != nullptr);
    nya_assert(network->activation_function != nullptr, "a network needs an activation function before it can run");
    nya_assert(network->activation_steps > 0);

    u32 node_count = (u32)network->nodes->length;
    if (node_count == 0) return;

    nya_assert(node_count <= NYA_NEAT_MAX_NODES, "a genome grew past NYA_NEAT_MAX_NODES (%d), which _nya_nn_neat_push_node should prevent", NYA_NEAT_MAX_NODES);

    /* Double buffered, so every node reads the previous step's values. */
    /* A fixed stack array: bounded, unlike an alloca sized by the genome. */
    f64 next[NYA_NEAT_MAX_NODES];

    /* Incoming weight sums in one pass over the connections instead of a rescan per node. */
    f64 sums[NYA_NEAT_MAX_NODES];

    for (u32 step = 0; step < network->activation_steps; step++) {
        for (u32 node_index = 0; node_index < node_count; node_index++) sums[node_index] = 0.0;

        nya_array_foreach (network->connections, connection) {
            if (!connection->enabled) continue;

            /* A gene naming a missing node is skipped. */
            if (connection->in >= node_count || connection->out >= node_count) continue;

            sums[connection->out] += network->nodes->items[connection->in].value * connection->weight;
        }

        for (u32 node_index = 0; node_index < node_count; node_index++) {
            NYA_NeatNode* node = &network->nodes->items[node_index];

            // sensors and the bias are inputs; activating them would overwrite what the caller set.
            if (node->kind == NYA_NEAT_NODE_SENSOR || node->kind == NYA_NEAT_NODE_BIAS) {
                next[node_index] = node->value;
                continue;
            }

            next[node_index] = network->activation_function(sums[node_index]);
        }

        for (u32 node_index = 0; node_index < node_count; node_index++) network->nodes->items[node_index].value = next[node_index];
    }
}

void nya_nn_neat_network_flush(NYA_NeatNetwork* network) {
    nya_assert(network != nullptr);

    nya_array_foreach (network->nodes, node) {
        if (node->kind == NYA_NEAT_NODE_HIDDEN || node->kind == NYA_NEAT_NODE_OUTPUT) node->value = 0.0;
    }
}

void nya_nn_neat_network_push_bias(NYA_NeatNetwork* network, NYA_ConstCString label) {
    nya_assert(network != nullptr);
    nya_assert(label != nullptr);

    _nya_nn_neat_push_node(network, NYA_NEAT_NODE_BIAS, label);
}

void nya_nn_neat_network_push_sensor(NYA_NeatNetwork* network, NYA_ConstCString label) {
    nya_assert(network != nullptr);
    nya_assert(label != nullptr);

    _nya_nn_neat_push_node(network, NYA_NEAT_NODE_SENSOR, label);
}

void nya_nn_neat_network_push_output(NYA_NeatNetwork* network, NYA_ConstCString label) {
    nya_assert(network != nullptr);
    nya_assert(label != nullptr);

    _nya_nn_neat_push_node(network, NYA_NEAT_NODE_OUTPUT, label);
}

void nya_nn_neat_network_set_sensor(NYA_NeatNetwork* network, NYA_ConstCString label, f64 value) {
    nya_assert(network != nullptr);
    nya_assert(label != nullptr);

    nya_array_foreach (network->nodes, node) {
        // the kind is checked as well as the label, or a label shared by a sensor and an output sets whichever
        // comes first.
        if (node->kind != NYA_NEAT_NODE_SENSOR) continue;
        if (node->label == nullptr || strcmp(node->label, label) != 0) continue;

        node->value = value;
        return;
    }

    nya_log_panic("no sensor named '%s'", label);
}

f64 nya_nn_neat_network_get_output(NYA_NeatNetwork* network, NYA_ConstCString label) {
    nya_assert(network != nullptr);
    nya_assert(label != nullptr);

    nya_array_foreach (network->nodes, node) {
        if (node->kind != NYA_NEAT_NODE_OUTPUT) continue;
        if (node->label == nullptr || strcmp(node->label, label) != 0) continue;

        return node->value;
    }

    nya_log_panic("no output named '%s'", label);
}

/*
 * ─────────────────────────────────────────────────────────
 * SAVING AND LOADING
 * ─────────────────────────────────────────────────────────
 */

/* A number read back from a value of any numeric type. */
NYA_INTERNAL b8 _nya_nn_neat_value_u32(const NYA_Value* value, OUT u32* out) {
    if (value == nullptr) return false;

    switch (value->type) {
        case NYA_TYPE_U8:  *out = value->as_u8; return true;
        case NYA_TYPE_U16: *out = value->as_u16; return true;
        case NYA_TYPE_U32: *out = value->as_u32; return true;
        case NYA_TYPE_U64: *out = (u32)value->as_u64; return true;
        case NYA_TYPE_S8:  *out = (u32)value->as_s8; return true;
        case NYA_TYPE_S16: *out = (u32)value->as_s16; return true;
        case NYA_TYPE_S32: *out = (u32)value->as_s32; return true;

        // what the JSON reader produces for whole numbers. negatives are refused, since every u32 here is an index
        // or a count.
        case NYA_TYPE_S64:
            if (value->as_s64 < 0) return false;
            *out = (u32)value->as_s64;
            return true;

        default: return false;
    }
}

NYA_INTERNAL b8 _nya_nn_neat_value_f64(const NYA_Value* value, OUT f64* out) {
    if (value == nullptr) return false;

    switch (value->type) {
        case NYA_TYPE_F32: *out = (f64)value->as_f32; return true;
        case NYA_TYPE_F64: *out = value->as_f64; return true;

        // a whole weight was written without a decimal point and reads back as an integer.
        case NYA_TYPE_S64: *out = (f64)value->as_s64; return true;
        case NYA_TYPE_U64: *out = (f64)value->as_u64; return true;
        case NYA_TYPE_S32: *out = (f64)value->as_s32; return true;
        case NYA_TYPE_U32: *out = (f64)value->as_u32; return true;

        default: return false;
    }
}

NYA_INTERNAL b8 _nya_nn_neat_value_b8(const NYA_Value* value, OUT b8* out) {
    if (value == nullptr) return false;

    if (value->type == NYA_TYPE_B8) {
        *out = value->as_b8;
        return true;
    }

    // tolerated so hand-written files may say 1 and 0.
    u32 numeric = 0;
    if (!_nya_nn_neat_value_u32(value, &numeric)) return false;

    *out = numeric != 0;
    return true;
}

NYA_Object* nya_nn_neat_network_to_object(NYA_Arena* arena, const NYA_NeatNetwork* network) {
    nya_assert(arena != nullptr);
    nya_assert(network != nullptr);

    NYA_Object* root = nya_object_create(arena);

    /* A version tag first. */
    nya_object_set(root, "version", (NYA_Value){ .type = NYA_TYPE_U32, .as_u32 = 1 });
    nya_object_set(root, "activation_steps", (NYA_Value){ .type = NYA_TYPE_U32, .as_u32 = network->activation_steps });

    NYA_ArrayᐸNYA_Valueᐳ* nodes = nya_array_create(arena, NYA_Value);

    nya_array_foreach (network->nodes, node) {
        NYA_Object* entry = nya_object_create(arena);

        nya_object_set(entry, "kind", (NYA_Value){ .type = NYA_TYPE_U32, .as_u32 = (u32)node->kind });

        // hidden nodes have no label; omitted, since a null string would round trip as "null".
        if (node->label != nullptr) {
            NYA_String* label = nya_string_sprintf(arena, "%s", node->label);
            nya_object_set(entry, "label", (NYA_Value){ .type = NYA_TYPE_STRING, .as_string = nya_string_to_cstring(arena, label) });
        }

        nya_array_push_back(nodes, ((NYA_Value){ .type = NYA_TYPE_OBJECT, .as_object = *entry }));
    }

    nya_object_set(root, "nodes", (NYA_Value){ .type = NYA_TYPE_ARRAY, .as_array = *nodes });

    NYA_ArrayᐸNYA_Valueᐳ* connections = nya_array_create(arena, NYA_Value);

    nya_array_foreach (network->connections, connection) {
        NYA_Object* entry = nya_object_create(arena);

        nya_object_set(entry, "in", (NYA_Value){ .type = NYA_TYPE_U32, .as_u32 = connection->in });
        nya_object_set(entry, "out", (NYA_Value){ .type = NYA_TYPE_U32, .as_u32 = connection->out });
        nya_object_set(entry, "weight", (NYA_Value){ .type = NYA_TYPE_F64, .as_f64 = connection->weight });
        nya_object_set(entry, "enabled", (NYA_Value){ .type = NYA_TYPE_B8, .as_b8 = connection->enabled });

        // saved: innovation numbers let this genome cross with its population later.
        nya_object_set(entry, "innovation", (NYA_Value){ .type = NYA_TYPE_U32, .as_u32 = connection->innovation_number });

        nya_array_push_back(connections, ((NYA_Value){ .type = NYA_TYPE_OBJECT, .as_object = *entry }));
    }

    nya_object_set(root, "connections", (NYA_Value){ .type = NYA_TYPE_ARRAY, .as_array = *connections });

    return root;
}

NYA_Error nya_nn_neat_network_from_object(
    NYA_Arena* arena, const NYA_Object* object, NYA_NeatActivationFunction activation_function, OUT NYA_NeatNetwork** out_network
) {
    nya_assert(arena != nullptr);
    nya_assert(out_network != nullptr);

    if (object == nullptr) return nya_error(NYA_ERROR_INVALID_ARGUMENT, "no object to read a network from");
    if (activation_function == nullptr) return nya_error(NYA_ERROR_INVALID_ARGUMENT, "a loaded network needs an activation function");

    u32 version = 0;
    if (!_nya_nn_neat_value_u32(nya_object_get(object, "version"), &version)) return nya_error(NYA_ERROR_CORRUPT, "not a saved network: no version");
    if (version != 1) return nya_error(NYA_ERROR_NOT_SUPPORTED, "saved network is version %u, this build reads version 1", version);

    NYA_Value* nodes       = nya_object_get(object, "nodes");
    NYA_Value* connections = nya_object_get(object, "connections");

    if (nodes == nullptr || nodes->type != NYA_TYPE_ARRAY) return nya_error(NYA_ERROR_CORRUPT, "saved network has no node list");
    if (connections == nullptr || connections->type != NYA_TYPE_ARRAY) return nya_error(NYA_ERROR_CORRUPT, "saved network has no connection list");

    if (nodes->as_array.length > NYA_NEAT_MAX_NODES) {
        return nya_error(NYA_ERROR_NOT_SUPPORTED, "saved network has %llu nodes, past NYA_NEAT_MAX_NODES (%d)", (unsigned long long)nodes->as_array.length, NYA_NEAT_MAX_NODES);
    }

    NYA_NeatNetwork* network = nya_nn_neat_network_create(arena);

    network->activation_function = activation_function;
    network->activation_steps    = 1;

    u32 steps = 0;
    if (_nya_nn_neat_value_u32(nya_object_get(object, "activation_steps"), &steps) && steps > 0) network->activation_steps = steps;

    for (u64 i = 0; i < nodes->as_array.length; i++) {
        NYA_Value* entry = &nodes->as_array.items[i];
        if (entry->type != NYA_TYPE_OBJECT) return nya_error(NYA_ERROR_CORRUPT, "node %llu is not an object", (unsigned long long)i);

        u32 kind = 0;
        if (!_nya_nn_neat_value_u32(nya_object_get(&entry->as_object, "kind"), &kind)) return nya_error(NYA_ERROR_CORRUPT, "node %llu has no kind", (unsigned long long)i);
        if (kind >= NYA_NEAT_NODE_KIND_COUNT) return nya_error(NYA_ERROR_CORRUPT, "node %llu has kind %u", (unsigned long long)i, kind);

        NYA_ConstCString label = nullptr;

        NYA_Value* stored_label = nya_object_get(&entry->as_object, "label");
        if (stored_label != nullptr && stored_label->type == NYA_TYPE_STRING && stored_label->as_string != nullptr) {
            // copied: NYA_NeatNode borrows its label, and the source object is the caller's.
            NYA_String* copy = nya_string_sprintf(arena, "%s", stored_label->as_string);
            label            = nya_string_to_cstring(arena, copy);
        }

        _nya_nn_neat_push_node(network, (NYA_NeatNodeKind)kind, label);
    }

    u32 node_count = (u32)network->nodes->length;

    for (u64 i = 0; i < connections->as_array.length; i++) {
        NYA_Value* entry = &connections->as_array.items[i];
        if (entry->type != NYA_TYPE_OBJECT) return nya_error(NYA_ERROR_CORRUPT, "connection %llu is not an object", (unsigned long long)i);

        u32 in         = 0;
        u32 out        = 0;
        u32 innovation = 0;
        f64 weight     = 0.0;
        b8  enabled    = false;

        b8 complete = _nya_nn_neat_value_u32(nya_object_get(&entry->as_object, "in"), &in)
                   && _nya_nn_neat_value_u32(nya_object_get(&entry->as_object, "out"), &out)
                   && _nya_nn_neat_value_u32(nya_object_get(&entry->as_object, "innovation"), &innovation)
                   && _nya_nn_neat_value_f64(nya_object_get(&entry->as_object, "weight"), &weight)
                   && _nya_nn_neat_value_b8(nya_object_get(&entry->as_object, "enabled"), &enabled);

        if (!complete) return nya_error(NYA_ERROR_CORRUPT, "connection %llu is missing or malformed", (unsigned long long)i);

        /* Endpoints are checked against the node list. */
        if (in >= node_count || out >= node_count) {
            return nya_error(NYA_ERROR_CORRUPT, "connection %llu refers to node %u/%u, but there are %u", (unsigned long long)i, in, out, node_count);
        }

        nya_array_push_back(
            network->connections,
            ((NYA_NeatConnection){
                .in                = in,
                .out               = out,
                .weight            = weight,
                .enabled           = enabled,
                .innovation_number = innovation,
            })
        );
    }

    // the distance function relies on sorted genes, and a hand-edited file need not be.
    _nya_nn_neat_sort_connections(network);

    *out_network = network;
    return NYA_OK;
}

NYA_Error nya_nn_neat_network_save(const NYA_NeatNetwork* network, NYA_ConstCString path) {
    nya_assert(network != nullptr);
    nya_assert(path != nullptr);

    // a scratch arena for the object.
    NYA_Arena scratch = nya_arena_create_on_stack(.name = "neat_save");
    defer     nya_arena_destroy_on_stack(&scratch);

    // nya_serde_save_file picks the format and writes, so every caller agrees.
    return nya_serde_save_file(nya_nn_neat_network_to_object(&scratch, network), path, NYA_SERDE_PRETTY);
}

NYA_Error nya_nn_neat_network_load(
    NYA_Arena* arena, NYA_ConstCString path, NYA_NeatActivationFunction activation_function, OUT NYA_NeatNetwork** out_network
) {
    nya_assert(arena != nullptr);
    nya_assert(path != nullptr);
    nya_assert(out_network != nullptr);

    NYA_Arena scratch = nya_arena_create_on_stack(.name = "neat_load");
    defer     nya_arena_destroy_on_stack(&scratch);

    /* The object is parsed into scratch; the network is built into the caller's arena. */
    NYA_Object* object = nullptr;
    NYA_TRY(nya_serde_load_file(&scratch, path, NYA_SERDE_NONE, &object));

    return nya_nn_neat_network_from_object(arena, object, activation_function, out_network);
}

/*
 * ─────────────────────────────────────────────────────────
 * EVOLUTION
 * ─────────────────────────────────────────────────────────
 */

NYA_Neat* nya_nn_neat_create(NYA_NeatConfig config) {
    nya_assert(config.seed != nullptr, "NEAT needs a seed network to start from");
    nya_assert(config.trial_function != nullptr, "NEAT needs a trial function to score genomes with");
    nya_assert(config.activation_function != nullptr, "NEAT needs an activation function");

    _nya_nn_neat_apply_config_defaults(&config);

    /* Region sizes stated explicitly. */
    const u64 neat_region_size = nya_mebyte_to_byte(4UL);

    NYA_Arena* allocator = nya_arena_create(.name = "neat_allocator", .region_size = neat_region_size);

    NYA_Neat* neat = nya_arena_alloc(allocator, sizeof(NYA_Neat));
    *neat          = (NYA_Neat){
                 .config                = config,
                 .allocator             = allocator,
                 .generation_allocators = { nya_arena_create(.name = "neat_generation_a", .region_size = neat_region_size),
                                            nya_arena_create(.name = "neat_generation_b", .region_size = neat_region_size) },
                     };

    /* The RNG, over-allocated and aligned up. */
    neat->rng = nya_rng_create_in(allocator, config.rng_seed);

    neat->current_innovations = nya_array_create(allocator, NYA_NeatConnection);

    // small: it holds one genome at a time. see NYA_Neat.best_allocator.
    neat->best_allocator = nya_arena_create(.name = "neat_best", .region_size = nya_mebyte_to_byte(1UL));

    NYA_Arena* generation = neat->generation_allocators[0];
    neat->species         = nya_array_create(generation, NYA_NeatSpecies);

    /* One species holding the whole population, all copies of the seed. */
    NYA_NeatSpecies initial = {
        .members = nya_array_create(generation, NYA_NeatNetwork),
    };

    for (u32 i = 0; i < config.population_size; i++) {
        NYA_NeatNetwork* member = nya_nn_neat_network_clone(generation, config.seed);

        member->activation_function = config.activation_function;
        member->activation_steps    = config.activation_steps;

        nya_array_push_back(initial.members, *member);
    }

    initial.representative = initial.members->items[0];
    nya_array_push_back(neat->species, initial);

    return neat;
}

void nya_nn_neat_destroy(NYA_Neat* neat) {
    if (neat == nullptr) return;

    nya_arena_destroy(neat->generation_allocators[0]);
    nya_arena_destroy(neat->generation_allocators[1]);
    nya_arena_destroy(neat->best_allocator);

    // last, because the context lives in it.
    nya_arena_destroy(neat->allocator);
}

u32 nya_nn_neat_generation(const NYA_Neat* neat) {
    nya_assert(neat != nullptr);
    return neat->generation;
}

u32 nya_nn_neat_species_count(const NYA_Neat* neat) {
    nya_assert(neat != nullptr);
    return (u32)neat->species->length;
}

f64 nya_nn_neat_fitness_max(const NYA_Neat* neat) {
    nya_assert(neat != nullptr);
    return neat->fitness_max;
}

f64 nya_nn_neat_fitness_average(const NYA_Neat* neat) {
    nya_assert(neat != nullptr);
    return neat->fitness_average;
}

NYA_NeatNetwork* nya_nn_neat_best(NYA_Neat* neat) {
    nya_assert(neat != nullptr);
    return neat->best;
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

void _nya_nn_neat_apply_config_defaults(NYA_NeatConfig* config) {
    /* Zero means unset, and the defaults below are the paper's. */
    if (config->population_size == 0) config->population_size = 150;
    if (config->activation_steps == 0) config->activation_steps = 3;

    if (config->compatibility_threshold == 0.0) config->compatibility_threshold = 3.0;
    if (config->compatibility_coefficient_excess == 0.0) config->compatibility_coefficient_excess = 1.0;
    if (config->compatibility_coefficient_disjoint == 0.0) config->compatibility_coefficient_disjoint = 1.0;
    if (config->compatibility_coefficient_weight == 0.0) config->compatibility_coefficient_weight = 0.4;

    if (config->weight_range_min == 0.0 && config->weight_range_max == 0.0) {
        config->weight_range_min = -1.0;
        config->weight_range_max = 1.0;
    }

    if (config->mutation_weight_perturbation_chance == 0.0) config->mutation_weight_perturbation_chance = 0.8;
    if (config->mutation_weight_perturbation_percent_max == 0.0) config->mutation_weight_perturbation_percent_max = 0.5;
    if (config->mutation_weight_reroll_chance == 0.0) config->mutation_weight_reroll_chance = 0.1;

    if (config->mutation_add_connection_chance == 0.0) config->mutation_add_connection_chance = 0.05;
    if (config->mutation_add_node_chance == 0.0) config->mutation_add_node_chance = 0.03;

    if (config->crossover_chance == 0.0) config->crossover_chance = 0.75;
    if (config->crossover_cutoff_percentage == 0.0) config->crossover_cutoff_percentage = 0.5;
    if (config->crossover_revive_disabled_chance == 0.0) config->crossover_revive_disabled_chance = 0.25;

    /* Ten generations a second, one per call. */
    if (config->generations_per_second <= 0.0F) config->generations_per_second = 10.0F;
    if (config->max_steps_per_frame == 0) config->max_steps_per_frame = 1;

    /*
     * The threshold controller is off unless a target species count is named; its bounds are filled in when it
     * is. Otherwise compatibility_threshold never moves.
     */
    if (config->compatibility_threshold_adjust == 0.0) config->compatibility_threshold_adjust = 0.3;
    if (config->compatibility_threshold_min == 0.0) config->compatibility_threshold_min = 0.5;
    if (config->compatibility_threshold_max == 0.0) config->compatibility_threshold_max = 20.0;

    if (config->species_stagnation_threshold == 0) config->species_stagnation_threshold = 15;
    if (config->population_stagnation_threshold == 0) config->population_stagnation_threshold = 20;
}

f64 _nya_nn_neat_uniform(NYA_RNG* rng, f64 min, f64 max) {
    return nya_rng_sample_f64(rng, (NYA_RNGDistribution){ .type = NYA_RNG_DISTRIBUTION_UNIFORM, .uniform = { .min = min, .max = max } });
}

u32 _nya_nn_neat_index(NYA_RNG* rng, u32 count) {
    if (count == 0) return 0;

    // the open upper bound: sampling [0, count] and rounding would underweight the last element and sometimes
    // pick one past the end.
    u32 index = (u32)_nya_nn_neat_uniform(rng, 0.0, (f64)count);
    return index >= count ? count - 1 : index;
}

u32 _nya_nn_neat_push_node(NYA_NeatNetwork* network, NYA_NeatNodeKind kind, NYA_ConstCString label) {
    u32 index = (u32)network->nodes->length;

    // mutations may do nothing, so this refuses quietly. callers check the count against the cap.
    if (index >= NYA_NEAT_MAX_NODES) return index;

    nya_array_push_back(
        network->nodes,
        ((NYA_NeatNode){
            .index = index,
            .kind  = kind,
            .label = label,
            // the bias node has a fixed value.
            .value = kind == NYA_NEAT_NODE_BIAS ? 1.0 : 0.0,
        })
    );

    return index;
}

u32 _nya_nn_neat_innovation_for(NYA_Neat* neat, u32 in, u32 out) {
    nya_array_foreach (neat->current_innovations, innovation) {
        if (innovation->in == in && innovation->out == out) return innovation->innovation_number;
    }

    /* A fresh number from the counter, not from the list length. */
    u32 number = neat->innovation_counter++;

    nya_array_push_back(neat->current_innovations, ((NYA_NeatConnection){ .in = in, .out = out, .innovation_number = number }));

    return number;
}

void _nya_nn_neat_mutate_weights(NYA_Neat* neat, NYA_NeatNetwork* network) {
    nya_array_foreach (network->connections, connection) {
        if (nya_rng_gen_bool(neat->rng, (f32)neat->config.mutation_weight_reroll_chance)) {
            connection->weight = _nya_nn_neat_uniform(neat->rng, neat->config.weight_range_min, neat->config.weight_range_max);
            continue;
        }

        // perturbation is a chance: most weights are left alone each generation so a good one is not walked away
        // from.
        if (!nya_rng_gen_bool(neat->rng, (f32)neat->config.mutation_weight_perturbation_chance)) continue;

        f64 max_change      = neat->config.mutation_weight_perturbation_percent_max;
        connection->weight += _nya_nn_neat_uniform(neat->rng, -max_change, max_change);
    }
}

void _nya_nn_neat_mutate_add_connection(NYA_Neat* neat, NYA_NeatNetwork* network) {
    u32 node_count = (u32)network->nodes->length;
    if (node_count < 2) return;

    if (network->connections->length >= NYA_NEAT_MAX_CONNECTIONS) return;

    for (u32 attempt = 0; attempt < 32; attempt++) {
        /* Indices over the nodes. */
        u32 in_index  = _nya_nn_neat_index(neat->rng, node_count);
        u32 out_index = _nya_nn_neat_index(neat->rng, node_count);

        NYA_NeatNode* in  = &network->nodes->items[in_index];
        NYA_NeatNode* out = &network->nodes->items[out_index];

        // no self loops, nothing feeds a sensor or the bias, and outputs feed nothing.
        if (in_index == out_index) continue;
        if (in->kind == NYA_NEAT_NODE_OUTPUT) continue;
        if (out->kind == NYA_NEAT_NODE_SENSOR || out->kind == NYA_NEAT_NODE_BIAS) continue;

        b8 exists = false;
        nya_array_foreach (network->connections, connection) {
            if (connection->in == in_index && connection->out == out_index) {
                exists = true;
                break;
            }
        }
        if (exists) continue;

        nya_array_push_back(
            network->connections,
            ((NYA_NeatConnection){
                .in                = in_index,
                .out               = out_index,
                .weight            = _nya_nn_neat_uniform(neat->rng, neat->config.weight_range_min, neat->config.weight_range_max),
                .enabled           = true,
                .innovation_number = _nya_nn_neat_innovation_for(neat, in_index, out_index),
            })
        );

        return;
    }

    // every attempt collided, normal for a small fully connected genome.
}

void _nya_nn_neat_mutate_add_node(NYA_Neat* neat, NYA_NeatNetwork* network) {
    if (network->connections->length == 0) return;

    // a split costs one node and two connections, so both ceilings need room, or the genome keeps a disabled
    // gene with no replacement.
    if (network->nodes->length >= NYA_NEAT_MAX_NODES) return;
    if (network->connections->length + 2 > NYA_NEAT_MAX_CONNECTIONS) return;

    u32                 target_index = _nya_nn_neat_index(neat->rng, (u32)network->connections->length);
    NYA_NeatConnection* target       = &network->connections->items[target_index];

    if (!target->enabled) return;

    // the old connection is disabled but kept, so its innovation number still lines up in crossover.
    target->enabled = false;

    u32 in     = target->in;
    u32 out    = target->out;
    f64 weight = target->weight;

    u32 new_node_index = _nya_nn_neat_push_node(network, NYA_NEAT_NODE_HIDDEN, nullptr);

    /* Weight one into the new node, the old weight out of it. */
    nya_array_push_back(
        network->connections,
        ((NYA_NeatConnection){
            .in                = in,
            .out               = new_node_index,
            .weight            = 1.0,
            .enabled           = true,
            .innovation_number = _nya_nn_neat_innovation_for(neat, in, new_node_index),
        })
    );

    nya_array_push_back(
        network->connections,
        ((NYA_NeatConnection){
            .in                = new_node_index,
            .out               = out,
            .weight            = weight,
            .enabled           = true,
            .innovation_number = _nya_nn_neat_innovation_for(neat, new_node_index, out),
        })
    );
}

NYA_NeatNetwork* _nya_nn_neat_crossover(NYA_Neat* neat, NYA_Arena* arena, NYA_NeatNetwork* parent1, NYA_NeatNetwork* parent2) {
    NYA_NeatNetwork* fitter = parent1->fitness_raw >= parent2->fitness_raw ? parent1 : parent2;
    NYA_NeatNetwork* other  = fitter == parent1 ? parent2 : parent1;

    // topology comes from the fitter parent, so gene indices refer to nodes it has.
    NYA_NeatNetwork* child = nya_nn_neat_network_create(arena);

    child->activation_function = fitter->activation_function;
    child->activation_steps    = fitter->activation_steps;

    nya_array_foreach (fitter->nodes, node) nya_array_push_back(child->nodes, *node);

    /* A linear merge over two innovation-sorted gene lists, as in _nya_nn_neat_distance. */
    u32 other_index = 0;
    u32 other_count = other->connections->length;

    nya_array_foreach (fitter->connections, gene) {
        NYA_NeatConnection inherited = *gene;

        // genes only the other parent has, which the child does not take.
        while (other_index < other_count && other->connections->items[other_index].innovation_number < gene->innovation_number) other_index++;

        // a matching gene comes from either parent at random.
        if (other_index < other_count && other->connections->items[other_index].innovation_number == gene->innovation_number) {
            if (nya_rng_gen_bool(neat->rng, 0.5F)) inherited = other->connections->items[other_index];
            other_index++;
        }

        // a gene disabled in its parent may come back on, or an early switch-off would be permanent.
        if (!inherited.enabled && nya_rng_gen_bool(neat->rng, (f32)neat->config.crossover_revive_disabled_chance)) inherited.enabled = true;

        nya_array_push_back(child->connections, inherited);
    }

    return child;
}

NYA_INTERNAL int _nya_nn_neat_compare_connections(const void* left, const void* right) {
    u32 a = ((const NYA_NeatConnection*)left)->innovation_number;
    u32 b = ((const NYA_NeatConnection*)right)->innovation_number;

    // not a subtraction: these are unsigned and would wrap.
    return a < b ? -1 : (a > b ? 1 : 0);
}

NYA_INTERNAL int _nya_nn_neat_compare_networks_by_fitness(const void* left, const void* right) {
    f64 a = ((const NYA_NeatNetwork*)left)->fitness_raw;
    f64 b = ((const NYA_NeatNetwork*)right)->fitness_raw;

    // descending, so a cutoff keeps the best.
    return a > b ? -1 : (a < b ? 1 : 0);
}

NYA_INTERNAL int _nya_nn_neat_compare_species_by_fitness(const void* left, const void* right) {
    f64 a = ((const NYA_NeatSpecies*)left)->fitness_max;
    f64 b = ((const NYA_NeatSpecies*)right)->fitness_max;

    return a > b ? -1 : (a < b ? 1 : 0);
}

void _nya_nn_neat_sort_connections(NYA_NeatNetwork* network) {
    if (network->connections->length < 2) return;

    qsort(network->connections->items, network->connections->length, sizeof(NYA_NeatConnection), _nya_nn_neat_compare_connections);
}

f64 _nya_nn_neat_distance(const NYA_Neat* neat, const NYA_NeatNetwork* a, const NYA_NeatNetwork* b) {
    f64 c1 = neat->config.compatibility_coefficient_excess;
    f64 c2 = neat->config.compatibility_coefficient_disjoint;
    f64 c3 = neat->config.compatibility_coefficient_weight;

    /* A linear merge over two innovation-sorted gene lists. */
    u32 a_count = (u32)a->connections->length;
    u32 b_count = (u32)b->connections->length;

    f64 matching      = 0.0;
    f64 disjoint      = 0.0;
    f64 excess        = 0.0;
    f64 weight_deltas = 0.0;

    u32 ai = 0;
    u32 bi = 0;

    while (ai < a_count && bi < b_count) {
        const NYA_NeatConnection* ga = &a->connections->items[ai];
        const NYA_NeatConnection* gb = &b->connections->items[bi];

        if (ga->innovation_number == gb->innovation_number) {
            matching      += 1.0;
            weight_deltas += fabs(ga->weight - gb->weight);
            ai++;
            bi++;
            continue;
        }

        /* Whichever side is behind holds a gene the other lacks, which inside the loop is disjoint, not excess. */
        disjoint += 1.0;

        if (ga->innovation_number < gb->innovation_number) ai++;
        else bi++;
    }

    // what is left is past the other list's end, so it is excess.
    excess += (f64)(a_count - ai) + (f64)(b_count - bi);

    /* N is the longer genome's size, or one for small genomes. */
    f64 n = (f64)nya_max(a_count, b_count);
    if (n < 20.0) n = 1.0;

    // no shared genes: the average weight difference would be NaN, and NaN compares false against every
    // threshold, putting unrelated genomes in one species.
    f64 average_weight_delta = matching > 0.0 ? weight_deltas / matching : 0.0;

    return ((c1 * excess) / n) + ((c2 * disjoint) / n) + (c3 * average_weight_delta);
}

/*
 * ─────────────────────────────────────────────────────────
 * THE GENERATION
 * ─────────────────────────────────────────────────────────
 */

u32 nya_nn_neat_step_for(NYA_Neat* neat, f32 delta_time_s) {
    nya_assert(neat != nullptr);

    // accumulated, or a rate below the frame rate rounds to zero every frame.
    neat->step_accumulator += delta_time_s * neat->config.generations_per_second;

    u32 steps = (u32)neat->step_accumulator;
    if (steps == 0) return 0;

    if (steps > neat->config.max_steps_per_frame) {
        steps = neat->config.max_steps_per_frame;

        // the backlog is dropped, or every call after a stall would run at the cap until it drained.
        neat->step_accumulator = 0.0F;
    } else {
        neat->step_accumulator -= (f32)steps;
    }

    /*
     * The count limits how many generations, not how long they take. Evaluation cost grows with the population's
     * connections; see max_step_milliseconds.
     */
    u64 started_ns  = nya_clock_get_monotonic_ns();
    u64 budget_ns   = (u64)(neat->config.max_step_milliseconds * 1'000'000.0);
    u32 steps_taken = 0;

    for (u32 i = 0; i < steps; i++) {
        nya_nn_neat_step(neat);
        steps_taken++;

        if (budget_ns == 0) continue;
        if (nya_clock_get_monotonic_ns() - started_ns >= budget_ns) break;
    }

    return steps_taken;
}

NYA_ConstCString nya_nn_neat_phase_name(NYA_NeatPhase phase) {
    switch (phase) {
        case NYA_NEAT_PHASE_EVALUATE:   return "evaluate";
        case NYA_NEAT_PHASE_SHARE:      return "share";
        case NYA_NEAT_PHASE_CULL:       return "cull";
        case NYA_NEAT_PHASE_REPRODUCE:  return "reproduce";
        case NYA_NEAT_PHASE_RESPECIATE: return "respeciate";

        case NYA_NEAT_PHASE_COUNT:
        default:                        return "unknown";
    }
}

void nya_nn_neat_step(NYA_Neat* neat) {
    nya_assert(neat != nullptr);

    /* A generation is five phases over shared working state. */
    _NYA_NeatGeneration generation = {
        // into the other arena, so the population being read stays valid until the swap.
        .next_arena = neat->generation_allocators[(neat->generation_allocator_index + 1) % 2],

        // seeded now, or the first phase's duration would be measured from zero.
        .phase_start_ns = nya_clock_get_monotonic_ns(),
    };

    _nya_nn_neat_phase_evaluate(neat, &generation);
    _nya_nn_neat_observe(neat, &generation, NYA_NEAT_PHASE_EVALUATE);

    _nya_nn_neat_phase_share(neat, &generation);
    _nya_nn_neat_observe(neat, &generation, NYA_NEAT_PHASE_SHARE);

    _nya_nn_neat_phase_cull(neat, &generation);
    _nya_nn_neat_observe(neat, &generation, NYA_NEAT_PHASE_CULL);

    _nya_nn_neat_phase_reproduce(neat, &generation);
    _nya_nn_neat_observe(neat, &generation, NYA_NEAT_PHASE_REPRODUCE);

    _nya_nn_neat_phase_respeciate(neat, &generation);

    neat->species = generation.respeciated;

    // the old population's arena is unreferenced now, so it is reset.
    neat->generation_allocator_index = (neat->generation_allocator_index + 1) % 2;
    nya_arena_free_all(neat->generation_allocators[(neat->generation_allocator_index + 1) % 2]);

    // reported before the counter moves, so the trace names the generation that ran.
    _nya_nn_neat_observe(neat, &generation, NYA_NEAT_PHASE_RESPECIATE);

    neat->generation++;
}

void _nya_nn_neat_observe(NYA_Neat* neat, _NYA_NeatGeneration* generation, NYA_NeatPhase phase) {
    u64 now_ns              = nya_clock_get_monotonic_ns();
    f64 duration_ms         = nya_time_ns_to_ms(now_ns - generation->phase_start_ns);
    generation->phase_start_ns = now_ns;

    if (neat->config.observer == nullptr) return;

    // counted from the current species list, whichever phase that is.
    NYA_ArrayᐸNYA_NeatSpeciesᐳ* current = generation->respeciated != nullptr ? generation->respeciated : neat->species;

    u32 population_count = 0;
    nya_array_foreach (current, species) population_count += (u32)species->members->length;

    NYA_NeatTrace trace = {
        .phase            = phase,
        .generation       = neat->generation,
        .species_count    = (u32)current->length,
        .population_count = population_count,
        .fitness_max      = neat->fitness_max,
        .fitness_average  = neat->fitness_average,
        .duration_ms      = duration_ms,
    };

    neat->config.observer(neat, &trace, neat->config.observer_user_data);
}

void _nya_nn_neat_phase_evaluate(NYA_Neat* neat, _NYA_NeatGeneration* generation) {
    /* The only call to the trial function, once per genome, and normally almost all of a generation's cost. */
    f64 fitness_total = 0.0;
    f64 fitness_max   = -INFINITY;

    NYA_NeatNetwork* fittest = nullptr;

    nya_array_foreach (neat->species, species) {
        nya_array_foreach (species->members, member) {
            nya_nn_neat_network_flush(member);
            member->fitness_raw = neat->config.trial_function(member);

            fitness_total += member->fitness_raw;

            if (member->fitness_raw > fitness_max) {
                fitness_max = member->fitness_raw;
                fittest     = member;
            }
        }
    }

    nya_array_foreach (neat->species, species) generation->population_count += (u32)species->members->length;

    neat->fitness_average = generation->population_count > 0 ? fitness_total / (f64)generation->population_count : 0.0;

    // against the best ever, so one worse generation is noise, not stagnation.
    if (fittest != nullptr && (neat->generation == 0 || fitness_max > neat->fitness_max)) {
        neat->fitness_max                     = fitness_max;
        neat->generations_without_improvement = 0;

        /* Snapshotted: the population arena is about to reset and the caller may keep this pointer. */
        nya_arena_free_all(neat->best_allocator);

        neat->best = nya_nn_neat_network_clone(neat->best_allocator, fittest);
    } else {
        neat->generations_without_improvement++;
    }
}

void _nya_nn_neat_phase_share(NYA_Neat* neat, _NYA_NeatGeneration* generation) {
    nya_unused(generation);

    /*
     * Fitness divided by species size, so a large species cannot out-vote a small one. This protects a new
     * topology while it is tuned.
     */
    nya_array_foreach (neat->species, species) {
        f64 species_max    = -INFINITY;
        f64 adjusted_total = 0.0;

        u32 member_count = (u32)species->members->length;

        nya_array_foreach (species->members, member) {
            member->fitness_adjusted  = member_count > 0 ? member->fitness_raw / (f64)member_count : 0.0;
            adjusted_total           += member->fitness_adjusted;
            species_max               = nya_max(species_max, member->fitness_raw);
        }

        if (species_max > species->fitness_max) {
            species->fitness_max                     = species_max;
            species->generations_without_improvement = 0;
        } else {
            species->generations_without_improvement++;
        }

        species->fitness_adjusted_total = adjusted_total;
    }
}

void _nya_nn_neat_phase_cull(NYA_Neat* neat, _NYA_NeatGeneration* generation) {
    NYA_NeatConfig* config = &neat->config;

    /*
     * A species that has not improved for a long time gets no offspring. When the whole population stalls, all
     * but the two best species go, narrowing to the most promising lines.
     */
    generation->population_stagnant = neat->generations_without_improvement > config->population_stagnation_threshold;

    if (generation->population_stagnant && neat->species->length > 2) {
        // the two best by peak fitness, not size, so a small promising species survives.
        qsort(neat->species->items, neat->species->length, sizeof(NYA_NeatSpecies), _nya_nn_neat_compare_species_by_fitness);

        neat->species->length                 = 2;
        neat->generations_without_improvement = 0;
    }

    /* Stagnation culling, always sparing the last species. */
    u32 survivors = 0;
    nya_array_foreach (neat->species, species) {
        if (_nya_nn_neat_species_is_barred(neat, generation, species)) continue;

        survivors++;
    }

    if (survivors == 0) {
        nya_array_foreach (neat->species, species) {
            if (generation->spared == nullptr || species->fitness_max > generation->spared->fitness_max) generation->spared = species;
        }
    }

    nya_array_foreach (neat->species, species) {
        // barred rather than removed, which would invalidate the loop; a zero allocation has the same effect.
        if (_nya_nn_neat_species_is_barred(neat, generation, species)) continue;

        generation->adjusted_grand_total += species->fitness_adjusted_total;
    }
}

void _nya_nn_neat_phase_reproduce(NYA_Neat* neat, _NYA_NeatGeneration* generation) {
    generation->next_species = nya_array_create(generation->next_arena, NYA_NeatSpecies);

    // a new generation gets new innovation numbers for the same structural change.
    nya_array_clear(neat->current_innovations);

    nya_array_foreach (neat->species, species) {
        if (species->members->length == 0) continue;
        if (_nya_nn_neat_species_is_barred(neat, generation, species)) continue;

        nya_array_push_back(generation->next_species, _nya_nn_neat_breed_species(neat, generation, species));
    }
}

NYA_NeatSpecies _nya_nn_neat_breed_species(NYA_Neat* neat, _NYA_NeatGeneration* generation, NYA_NeatSpecies* species) {
    NYA_NeatConfig* config = &neat->config;

    /* Offspring in proportion to the species' share of adjusted fitness, at least one. */
    f64 share = generation->adjusted_grand_total > 0.0
                    ? species->fitness_adjusted_total / generation->adjusted_grand_total
                    : 1.0 / (f64)neat->species->length;

    /* Clamped before the cast, since fitness may be negative. */
    share = nya_clamp(share, 0.0, 1.0);

    u32 offspring_count = (u32)((share * (f64)config->population_size) + 0.5);
    if (offspring_count == 0) offspring_count = 1;

    // best first, so the cutoff below keeps the top of the species.
    qsort(species->members->items, species->members->length, sizeof(NYA_NeatNetwork), _nya_nn_neat_compare_networks_by_fitness);

    NYA_NeatSpecies offspring_species = {
        .members                         = nya_array_create(generation->next_arena, NYA_NeatNetwork),
        .fitness_max                     = species->fitness_max,
        .generations_without_improvement = species->generations_without_improvement,

        // the representative is from the previous generation, so members are measured against where the species
        // was, not where it drifts.
        .representative                  = species->representative,
    };

    // elitism, for species large enough to spare the slot, so the best genome cannot be lost to a mutation.
    if (species->members->length >= NYA_NEAT_ELITISM_MIN_SPECIES_SIZE) {
        NYA_NeatNetwork* champion = nya_nn_neat_network_clone(generation->next_arena, &species->members->items[0]);
        nya_array_push_back(offspring_species.members, *champion);
    }

    u32 breeding_count = (u32)((f64)species->members->length * config->crossover_cutoff_percentage);
    if (breeding_count == 0) breeding_count = 1;

    while (offspring_species.members->length < offspring_count) {
        NYA_NeatNetwork* parent1 = &species->members->items[_nya_nn_neat_index(neat->rng, breeding_count)];
        NYA_NeatNetwork* child   = nullptr;

        if (breeding_count > 1 && nya_rng_gen_bool(neat->rng, (f32)config->crossover_chance)) {
            NYA_NeatNetwork* parent2 = &species->members->items[_nya_nn_neat_index(neat->rng, breeding_count)];
            child                    = _nya_nn_neat_crossover(neat, generation->next_arena, parent1, parent2);
        } else {
            child = nya_nn_neat_network_clone(generation->next_arena, parent1);
        }

        // fitness belongs to the parent; the child has not been scored.
        child->fitness_raw      = 0.0;
        child->fitness_adjusted = 0.0;

        _nya_nn_neat_mutate_weights(neat, child);
        if (nya_rng_gen_bool(neat->rng, (f32)config->mutation_add_connection_chance)) _nya_nn_neat_mutate_add_connection(neat, child);
        if (nya_rng_gen_bool(neat->rng, (f32)config->mutation_add_node_chance)) _nya_nn_neat_mutate_add_node(neat, child);

        // restores the sort order distance and crossover rely on; a mutation can reuse an innovation number minted
        // earlier this generation.
        _nya_nn_neat_sort_connections(child);

        nya_array_push_back(offspring_species.members, *child);
    }

    return offspring_species;
}

void _nya_nn_neat_phase_respeciate(NYA_Neat* neat, _NYA_NeatGeneration* generation) {
    NYA_NeatConfig* config = &neat->config;

    /*
     * Children are placed against the frozen representatives, so one that mutated away founds or joins another
     * species.
     */
    generation->respeciated = nya_array_create(generation->next_arena, NYA_NeatSpecies);

    nya_array_foreach (generation->next_species, species) {
        nya_array_foreach (species->members, member) {
            b8 placed = false;

            nya_array_foreach (generation->respeciated, candidate) {
                if (_nya_nn_neat_distance(neat, member, &candidate->representative) >= config->compatibility_threshold) continue;

                nya_array_push_back(candidate->members, *member);
                placed = true;
                break;
            }

            if (placed) continue;

            NYA_NeatSpecies fresh = {
                .members                         = nya_array_create(generation->next_arena, NYA_NeatNetwork),
                .representative                  = *member,
                .fitness_max                     = species->fitness_max,
                .generations_without_improvement = species->generations_without_improvement,
            };
            nya_array_push_back(fresh.members, *member);
            nya_array_push_back(generation->respeciated, fresh);
        }
    }

    /* Extinction guard. */
    if (generation->respeciated->length == 0 && neat->best != nullptr) {
        NYA_NeatSpecies fallback = { .members = nya_array_create(generation->next_arena, NYA_NeatNetwork) };

        for (u32 i = 0; i < config->population_size; i++) {
            NYA_NeatNetwork* revived = nya_nn_neat_network_clone(generation->next_arena, neat->best);

            revived->fitness_raw      = 0.0;
            revived->fitness_adjusted = 0.0;

            // not the first, so the line is not just the champion repeated.
            if (i > 0) _nya_nn_neat_mutate_weights(neat, revived);

            nya_array_push_back(fallback.members, *revived);
        }

        fallback.representative = fallback.members->items[0];
        nya_array_push_back(generation->respeciated, fallback);

        nya_log_warn("every NEAT species went extinct in generation %u; reseeded from the best genome", neat->generation);
    }

    /* The compatibility threshold is retuned against this generation's species count. */
    if (config->target_species_count == 0) return;

    /* Proportional to the relative error. */
    u32 count = (u32)generation->respeciated->length;
    f64 error = ((f64)count - (f64)config->target_species_count) / (f64)config->target_species_count;

    config->compatibility_threshold += config->compatibility_threshold_adjust * error;

    config->compatibility_threshold = nya_clamp(
        config->compatibility_threshold,
        config->compatibility_threshold_min,
        config->compatibility_threshold_max
    );
}

b8 _nya_nn_neat_species_is_barred(const NYA_Neat* neat, const _NYA_NeatGeneration* generation, const NYA_NeatSpecies* species) {
    // the three conditions for breeding, in one place, since reproduce and the totals must agree on them.
    if (species->generations_without_improvement <= neat->config.species_stagnation_threshold) return false;
    if (generation->population_stagnant) return false;
    if (species == generation->spared) return false;

    return true;
}
