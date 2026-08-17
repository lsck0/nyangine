/**
 * NEAT: the network primitives, and whether evolution actually works.
 *
 * The second half is the one that matters. Every part of NEAT can be individually plausible and the
 * whole still fail to learn — a wrong innovation number, a dropped gene in crossover, a distance
 * function that collapses every genome into one species, all of them produce a population that
 * evolves *something* and never gets better. So the acceptance test is behavioural: solve XOR.
 *
 * XOR is the standard one because it is not linearly separable. A network with no hidden nodes
 * cannot do it at all, so a run that succeeds has necessarily grown its own topology — which is the
 * entire point of the algorithm and the thing a weights-only optimiser cannot fake.
 *
 * The RNG is seeded fixed, so a failure here is a real regression rather than an unlucky run.
 **/

#include "nyangine/nyangine.c"
#include "nyangine/nyangine.h"

/** Fitness in [0, 4]: four cases, one point each, minus the absolute error on each. */
static f64 xor_trial(NYA_NeatNetwork* network) {
  const f64 inputs[4][2]  = { { 0.0, 0.0 }, { 0.0, 1.0 }, { 1.0, 0.0 }, { 1.0, 1.0 } };
  const f64 expected[4]   = { 0.0, 1.0, 1.0, 0.0 };

  f64 error = 0.0;

  for (u32 i = 0; i < 4; i++) {
    // Flushed between cases: the network may be recurrent, and without this the answer to one case
    // depends on which case ran before it.
    nya_nn_neat_network_flush(network);

    nya_nn_neat_network_set_sensor(network, "x", inputs[i][0]);
    nya_nn_neat_network_set_sensor(network, "y", inputs[i][1]);
    nya_nn_neat_network_run(network);

    error += fabs(expected[i] - nya_nn_neat_network_get_output(network, "result"));
  }

  return 4.0 - error;
}

/** The seed topology: a bias, two sensors, one output, and no connections at all. */
/** What the observer test accumulates. */
struct ObserverLog {
  u32 calls;
  u32 phase_sequence_broken;
  u32 empty_population_seen;
  u32 generations;
};

/** Records one phase. At file scope because C has no nested functions. */
static void record_phase(const NYA_Neat* neat, const NYA_NeatTrace* trace, void* user_data) {
  nya_unused(neat);

  struct ObserverLog* observed = user_data;

  if (trace->phase != observed->calls % NYA_NEAT_PHASE_COUNT) observed->phase_sequence_broken++;
  if (trace->population_count == 0) observed->empty_population_seen++;
  if (trace->phase == NYA_NEAT_PHASE_RESPECIATE) observed->generations++;

  observed->calls++;
}

static NYA_NeatNetwork* xor_seed(NYA_Arena* arena) {
  NYA_NeatNetwork* seed = nya_nn_neat_network_create(arena);

  // The bias is not optional for XOR. Without a constant to lean on, a node cannot learn a
  // threshold, and the population plateaus around 3 of 4 — which looks like slow progress rather
  // than a missing input. The original example omitted it.
  nya_nn_neat_network_push_bias(seed, "bias");
  nya_nn_neat_network_push_sensor(seed, "x");
  nya_nn_neat_network_push_sensor(seed, "y");
  nya_nn_neat_network_push_output(seed, "result");

  return seed;
}

s32 main(void) {
  NYA_Arena* arena = nya_arena_create(.name = "test_neat");
  defer      nya_arena_destroy(arena);

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: a network runs, and sensors reach the output through a connection
  // ─────────────────────────────────────────────────────────────────────────────
  {
    NYA_NeatNetwork* network = nya_nn_neat_network_create(arena);
    nya_nn_neat_network_push_sensor(network, "in");
    nya_nn_neat_network_push_output(network, "out");

    network->activation_function = nya_nn_neat_sigmoid;
    network->activation_steps    = 2;

    nya_assert(network->nodes->length == 2);
    nya_assert(network->connections->length == 0);

    // Unconnected: the output cannot see the sensor, so it is whatever the activation of zero is.
    nya_nn_neat_network_set_sensor(network, "in", 1.0);
    nya_nn_neat_network_run(network);
    f64 unconnected = nya_nn_neat_network_get_output(network, "out");
    nya_assert(fabs(unconnected - nya_nn_neat_sigmoid(0.0)) < 0.0001, "got %f", unconnected);

    // Connected with a large positive weight, the output should saturate high.
    nya_array_push_back(network->connections, ((NYA_NeatConnection){ .in = 0, .out = 1, .weight = 5.0, .enabled = true }));

    nya_nn_neat_network_flush(network);
    nya_nn_neat_network_set_sensor(network, "in", 1.0);
    nya_nn_neat_network_run(network);
    nya_assert(nya_nn_neat_network_get_output(network, "out") > 0.9, "a strong connection should drive the output high");

    // A disabled gene must be invisible to the run, not merely weighted less.
    network->connections->items[0].enabled = false;
    nya_nn_neat_network_flush(network);
    nya_nn_neat_network_set_sensor(network, "in", 1.0);
    nya_nn_neat_network_run(network);
    nya_assert(fabs(nya_nn_neat_network_get_output(network, "out") - nya_nn_neat_sigmoid(0.0)) < 0.0001, "a disabled gene must not conduct");
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: a gene naming a node the genome does not have is ignored
  // ─────────────────────────────────────────────────────────────────────────────
  {
    /*
     * Reachable from a loaded genome, or from a crossover against one with fewer nodes.
     *
     * The evaluation used to compare every connection's `out` against each node index in turn, so an
     * index past the end simply never matched and the gene fell out for free — while `in` was read
     * straight out of the node array and was an out of bounds read nobody noticed. Gathering the
     * sums by indexing on `out` makes that a *write* past the end of a stack array, so both ends are
     * now bounds checked. This is the test for that, since no evolution run produces such a gene.
     */
    NYA_NeatNetwork* network = nya_nn_neat_network_create(arena);
    nya_nn_neat_network_push_sensor(network, "in");
    nya_nn_neat_network_push_output(network, "out");

    network->activation_function = nya_nn_neat_sigmoid;
    network->activation_steps    = 2;

    u32 node_count = (u32)network->nodes->length;

    // A real gene, so the network has something legitimate to compute alongside the bad ones.
    nya_array_push_back(network->connections, ((NYA_NeatConnection){ .in = 0, .out = 1, .weight = 5.0, .enabled = true }));

    nya_nn_neat_network_flush(network);
    nya_nn_neat_network_set_sensor(network, "in", 1.0);
    nya_nn_neat_network_run(network);
    f64 clean = nya_nn_neat_network_get_output(network, "out");

    // `out` past the end is the write; `in` past the end is the read. Both far enough out that a
    // missing check lands well outside the array rather than in its slack.
    nya_array_push_back(network->connections, ((NYA_NeatConnection){ .in = 0, .out = node_count + 64, .weight = 9.0, .enabled = true }));
    nya_array_push_back(network->connections, ((NYA_NeatConnection){ .in = node_count + 64, .out = 1, .weight = 9.0, .enabled = true }));

    // Right at the boundary too, which is where an off-by-one in the guard would hide.
    nya_array_push_back(network->connections, ((NYA_NeatConnection){ .in = 0, .out = node_count, .weight = 9.0, .enabled = true }));
    nya_array_push_back(network->connections, ((NYA_NeatConnection){ .in = node_count, .out = 1, .weight = 9.0, .enabled = true }));

    nya_nn_neat_network_flush(network);
    nya_nn_neat_network_set_sensor(network, "in", 1.0);
    nya_nn_neat_network_run(network);

    // Not merely "did not crash": the bad genes must contribute nothing, so the output has to be
    // exactly what it was without them. Under the sanitizers the run itself is the other half of
    // this test — an unguarded version would abort before reaching here.
    f64 with_dangling = nya_nn_neat_network_get_output(network, "out");
    nya_assert(with_dangling == clean, "a dangling gene changed the result: %.17g against %.17g", with_dangling, clean);
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: a bias holds its value, and flush leaves it alone
  // ─────────────────────────────────────────────────────────────────────────────
  {
    NYA_NeatNetwork* network = nya_nn_neat_network_create(arena);
    nya_nn_neat_network_push_bias(network, "bias");
    nya_nn_neat_network_push_output(network, "out");

    network->activation_function = nya_nn_neat_sigmoid;

    nya_assert(fabs(network->nodes->items[0].value - 1.0) < 0.0001, "a bias starts at one");

    nya_nn_neat_network_run(network);
    nya_nn_neat_network_flush(network);

    // Flushing clears hidden and output nodes; a bias is an input and must survive it, or the
    // network loses its constant after the first trial.
    nya_assert(fabs(network->nodes->items[0].value - 1.0) < 0.0001, "flush must not clear the bias");
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: cloning is deep, which is what makes a population possible
  // ─────────────────────────────────────────────────────────────────────────────
  {
    NYA_NeatNetwork* original = nya_nn_neat_network_create(arena);
    nya_nn_neat_network_push_sensor(original, "in");
    nya_nn_neat_network_push_output(original, "out");
    nya_array_push_back(original->connections, ((NYA_NeatConnection){ .in = 0, .out = 1, .weight = 1.0, .enabled = true }));

    NYA_NeatNetwork* clone = nya_nn_neat_network_clone(arena, original);

    nya_assert(clone->nodes != original->nodes, "a clone must not share the node array");
    nya_assert(clone->connections != original->connections, "a clone must not share the connection array");

    // The decisive check: mutating one must not touch the other. A shallow copy passes every
    // structural assertion above and fails this one, and in a population it would mean every genome
    // silently mutating every other.
    clone->connections->items[0].weight = 99.0;
    nya_assert(fabs(original->connections->items[0].weight - 1.0) < 0.0001, "a clone must own its genes");
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: evolution solves XOR
  // ─────────────────────────────────────────────────────────────────────────────
  {
    /*
     * Several seeds, and a majority must solve — not one seed that must.
     *
     * NEAT is stochastic, and a single pinned seed tests whether that one run happens to work rather
     * than whether the algorithm does. This test was pinned that way and it lied twice over: it
     * passed while the compatibility distance was miscounting excess genes, and then failed the
     * moment that was fixed, purely because the corrected metric sent that one run down a different
     * path. Measured across seeds the fix is plainly an improvement; measured on one it looked like a
     * regression.
     *
     * A majority rather than all of them, because an unlucky run is a real property of the algorithm
     * and not a defect. Failing every seed is the signal worth catching.
     */
    NYA_ConstCString seeds[] = { "6E79616E67696E65", "1234567890ABCDEF", "FEDCBA0987654321" };

    const u32 generation_budget = 250;

    u32 solved      = 0;
    u32 hidden_seen = 0;

    for (u32 run = 0; run < nya_carray_length(seeds); run++) {
      NYA_Neat* neat = nya_nn_neat_create((NYA_NeatConfig){
        .seed                = xor_seed(arena),
        .trial_function      = xor_trial,
        .activation_function = nya_nn_neat_sigmoid,
        .activation_steps    = 4,
        .population_size     = 150,
        .rng_seed            = seeds[run],

        .weight_range_min = -2.0,
        .weight_range_max = 2.0,

        .mutation_add_connection_chance = 0.15,
        .mutation_add_node_chance       = 0.04,
      });
      defer nya_nn_neat_destroy(neat);

      f64 best = 0.0;
      for (u32 generation = 0; generation < generation_budget; generation++) {
        nya_nn_neat_step(neat);
        best = nya_nn_neat_fitness_max(neat);

        // 3.9 of 4 means every case is within 0.025 of right, which is solved rather than "leaning
        // the right way".
        if (best >= 3.9) break;
      }

      nya_info("NEAT seed %u: generation %u, %u species, best %.4f", run, nya_nn_neat_generation(neat), nya_nn_neat_species_count(neat), best);

      if (best < 3.9) continue;

      solved++;

      // XOR is not linearly separable, so no arrangement of weights on a bias-plus-two-inputs
      // network can solve it. A champion scoring 3.9 with no hidden node would mean the fitness
      // function is lying, not that NEAT found something clever.
      NYA_NeatNetwork* champion = nya_nn_neat_best(neat);
      nya_assert(champion != nullptr, "a solved run must have a best network");

      u32 hidden = 0;
      nya_array_foreach (champion->nodes, node) {
        if (node->kind == NYA_NEAT_NODE_HIDDEN) hidden++;
      }
      if (hidden > 0) hidden_seen++;

      // The champion survives being re-run, because it is snapshotted into the context arena rather
      // than pointing into the population that is rebuilt every generation.
      nya_assert(fabs(xor_trial(champion) - best) < 0.5, "the reported best should reproduce when re-run");
    }

    nya_assert(solved >= 2, "NEAT solved XOR on only %u of %llu seeds", solved, (unsigned long long)nya_carray_length(seeds));
    nya_assert(hidden_seen == solved, "every solved run must have grown a hidden node, since XOR needs one");
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: the observer sees every phase of every generation, in order
  {
    struct ObserverLog log = { 0 };

    /*
     * The hook exists so a caller can watch a run without the library deciding what is worth
     * logging, and this asserts the two things a watcher depends on: that the five phases arrive in
     * declaration order, and that the population is never reported empty. The second is the
     * regression guard — every historical NEAT failure here was a generation that quietly bred
     * nothing, and before the hook the only symptom was a fitness that stopped moving.
     */
    NYA_Neat* neat = nya_nn_neat_create((NYA_NeatConfig){
      .seed                = xor_seed(arena),
      .trial_function      = xor_trial,
      .activation_function = nya_nn_neat_sigmoid,
      .activation_steps    = 4,
      .population_size     = 30,
      .rng_seed            = "0B5E12EE12345678",

      .observer           = record_phase,
      .observer_user_data = &log,
    });
    defer nya_nn_neat_destroy(neat);

    for (u32 i = 0; i < 20; i++) nya_nn_neat_step(neat);

    nya_assert(log.calls == 20 * NYA_NEAT_PHASE_COUNT, "the observer must fire once per phase per generation, got %u", log.calls);
    nya_assert(log.phase_sequence_broken == 0, "phases must arrive in order, %u out of place", log.phase_sequence_broken);
    nya_assert(log.empty_population_seen == 0, "the population must never be reported empty");
    nya_assert(log.generations == 20, "one respeciate per generation, got %u", log.generations);
  }

  // TEST: a genome survives a round trip through a file
  // ─────────────────────────────────────────────────────────────────────────────
  {
#define TEST_NEAT_PATH "./tests/nyangine/nn/test_neat_genome.json"

    // Built by hand rather than evolved, so the assertions below name exact values — an evolved
    // genome would only let this check that two opaque things match.
    NYA_NeatNetwork* original = nya_nn_neat_network_create(arena);
    nya_nn_neat_network_push_bias(original, "bias");
    nya_nn_neat_network_push_sensor(original, "x");
    nya_nn_neat_network_push_output(original, "out");
    _nya_nn_neat_push_node(original, NYA_NEAT_NODE_HIDDEN, nullptr);

    original->activation_function = nya_nn_neat_sigmoid;
    original->activation_steps    = 3;

    nya_array_push_back(original->connections, ((NYA_NeatConnection){ .in = 1, .out = 3, .weight = 1.5, .enabled = true, .innovation_number = 7 }));
    nya_array_push_back(original->connections, ((NYA_NeatConnection){ .in = 3, .out = 2, .weight = -0.25, .enabled = true, .innovation_number = 9 }));

    // A disabled gene, because it still carries an innovation number and a weight and must come back
    // disabled rather than being dropped or revived.
    nya_array_push_back(original->connections, ((NYA_NeatConnection){ .in = 0, .out = 2, .weight = 0.75, .enabled = false, .innovation_number = 11 }));

    /*
     * Both formats, because they take different paths through serde and the default is the one that
     * is easiest to leave untested — the extension chooses, so a test naming only .json exercises
     * the interop path and never the native one.
     */
    NYA_ConstCString paths[] = { "./tests/nyangine/nn/test_neat_genome.nya", TEST_NEAT_PATH };

    for (u32 format = 0; format < nya_carray_length(paths); format++) {
    NYA_ConstCString path = paths[format];

    NYA_EXPECT(nya_nn_neat_network_save(original, path), "while saving a genome");
    defer (void)nya_filesystem_delete(path);

    NYA_NeatNetwork* loaded = nullptr;
    NYA_EXPECT(nya_nn_neat_network_load(arena, path, nya_nn_neat_sigmoid, &loaded), "while loading a genome");

    nya_assert(loaded->nodes->length == original->nodes->length, "node count changed: %llu vs %llu", (unsigned long long)loaded->nodes->length, (unsigned long long)original->nodes->length);
    nya_assert(loaded->connections->length == original->connections->length, "connection count changed");
    nya_assert(loaded->activation_steps == 3, "activation steps were not restored");

    // Kinds and labels, including the hidden node's absent one.
    nya_assert(loaded->nodes->items[0].kind == NYA_NEAT_NODE_BIAS);
    nya_assert(strcmp(loaded->nodes->items[0].label, "bias") == 0);
    nya_assert(loaded->nodes->items[1].kind == NYA_NEAT_NODE_SENSOR);
    nya_assert(loaded->nodes->items[2].kind == NYA_NEAT_NODE_OUTPUT);
    nya_assert(loaded->nodes->items[3].kind == NYA_NEAT_NODE_HIDDEN);
    nya_assert(loaded->nodes->items[3].label == nullptr, "a hidden node has no label and must not gain one");

    // Genes come back sorted by innovation number, which is the invariant the distance function
    // relies on — so they are checked by number rather than by position.
    for (u32 i = 0; i < 3; i++) {
      u32 innovation = loaded->connections->items[i].innovation_number;

      const NYA_NeatConnection* source = nullptr;
      nya_array_foreach (original->connections, candidate) {
        if (candidate->innovation_number == innovation) source = candidate;
      }

      nya_assert(source != nullptr, "innovation %u appeared from nowhere", innovation);
      nya_assert(loaded->connections->items[i].in == source->in, "gene %u changed its input", innovation);
      nya_assert(loaded->connections->items[i].out == source->out, "gene %u changed its output", innovation);
      nya_assert(fabs(loaded->connections->items[i].weight - source->weight) < 1e-9, "gene %u changed weight", innovation);
      nya_assert(loaded->connections->items[i].enabled == source->enabled, "gene %u changed enabled", innovation);
    }

    // ── The check that matters: it computes the same thing ──
    for (u32 i = 0; i < 4; i++) {
      f64 input = (f64)i * 0.3;

      nya_nn_neat_network_flush(original);
      nya_nn_neat_network_set_sensor(original, "x", input);
      nya_nn_neat_network_run(original);
      f64 expected = nya_nn_neat_network_get_output(original, "out");

      nya_nn_neat_network_flush(loaded);
      nya_nn_neat_network_set_sensor(loaded, "x", input);
      nya_nn_neat_network_run(loaded);
      f64 actual = nya_nn_neat_network_get_output(loaded, "out");

      // Bit for bit, not approximately: nothing in the round trip is lossy, so a difference means a
      // weight or a topology changed rather than that floating point drifted.
      nya_assert(expected == actual, "a loaded network must compute what it did before: %f vs %f", expected, actual);
    }
    }

    // ── Corrupt input is an error, not a crash ──
    {
      NYA_NeatNetwork* rejected = nullptr;

      NYA_Object* empty = nya_object_create(arena);
      nya_assert(!nya_nn_neat_network_from_object(arena, empty, nya_nn_neat_sigmoid, &rejected).ok, "an object with no version must be refused");

      // A connection pointing at a node that does not exist would be an out of bounds read on every
      // evaluation, which is exactly what a save file from elsewhere might contain.
      NYA_Object* bad = nya_nn_neat_network_to_object(arena, original);
      NYA_Value*  connections = nya_object_get(bad, "connections");
      nya_object_set(&connections->as_array.items[0].as_object, "out", (NYA_Value){ .type = NYA_TYPE_U32, .as_u32 = 999 });

      nya_assert(!nya_nn_neat_network_from_object(arena, bad, nya_nn_neat_sigmoid, &rejected).ok, "an out of range endpoint must be refused");
    }

    // A network needs an activation function to run, and one cannot be stored, so omitting it fails
    // rather than producing something that asserts the first time it is used.
    {
      NYA_NeatNetwork* rejected = nullptr;
      NYA_Object*      valid    = nya_nn_neat_network_to_object(arena, original);
      nya_assert(!nya_nn_neat_network_from_object(arena, valid, nullptr, &rejected).ok, "a null activation function must be refused");
    }

#undef TEST_NEAT_PATH
  }

  printf("PASSED: test_neat\n");
  return 0;
}
