/**
 * Structural invariants of every genome, across a real evolutionary run.
 *
 * The trial function is called once per genome per generation, which makes it the place to inspect
 * the whole population. _nya_nn_neat_distance and _nya_nn_neat_crossover both walk two gene lists
 * as a linear merge and are only correct if those lists are sorted by innovation number, so that
 * ordering is an invariant of the data structure rather than an internal detail of one function.
 */

#include "nyangine/nyangine.c"
#include "nyangine/nyangine.h"

static u32 genomes_checked = 0;
static u32 max_nodes_seen = 0;
static u32 max_connections_seen = 0;

static void check_genome(NYA_NeatNetwork* network) {
  genomes_checked++;

  u32 node_count = (u32)network->nodes->length;
  u32 connection_count = (u32)network->connections->length;

  if (node_count > max_nodes_seen) max_nodes_seen = node_count;
  if (connection_count > max_connections_seen) max_connections_seen = connection_count;

  nya_check(node_count <= NYA_NEAT_MAX_NODES, "genome has %u nodes, past the cap of %d", node_count, NYA_NEAT_MAX_NODES);
  nya_check(connection_count <= NYA_NEAT_MAX_CONNECTIONS, "genome has %u connections, past the cap of %d", connection_count, NYA_NEAT_MAX_CONNECTIONS);

  for (u32 i = 0; i < connection_count; i++) {
    const NYA_NeatConnection* connection = &network->connections->items[i];

    // The merge in distance and crossover depends on this and nothing enforces it at the point of
    // use, so a mutation that reuses an innovation number minted earlier in the same generation
    // would break both silently.
    if (i > 0) {
      const NYA_NeatConnection* previous = &network->connections->items[i - 1];
      nya_check(
        previous->innovation_number <= connection->innovation_number,
        "connections are not innovation sorted: [%u] = %u then [%u] = %u",
        i - 1,
        previous->innovation_number,
        i,
        connection->innovation_number
      );
      nya_check(
        previous->innovation_number != connection->innovation_number,
        "the same innovation number %u appears twice in one genome",
        connection->innovation_number
      );
    }

    nya_check(connection->in < node_count, "connection %u comes from node %u, past the %u the genome has", i, connection->in, node_count);
    nya_check(connection->out < node_count, "connection %u goes to node %u, past the %u the genome has", i, connection->out, node_count);
    nya_check(connection->in != connection->out, "connection %u is a self loop on node %u", i, connection->in);

    if (connection->in >= node_count || connection->out >= node_count) continue;

    NYA_NeatNodeKind from = network->nodes->items[connection->in].kind;
    NYA_NeatNodeKind to   = network->nodes->items[connection->out].kind;

    nya_check(from != NYA_NEAT_NODE_OUTPUT, "connection %u leaves an output node", i);
    nya_check(to != NYA_NEAT_NODE_SENSOR, "connection %u feeds a sensor", i);
    nya_check(to != NYA_NEAT_NODE_BIAS, "connection %u feeds the bias", i);

    // A duplicate edge means one genome holds the same structural gene twice, which makes its
    // distance to anything else wrong and double counts the weight in the forward pass.
    for (u32 j = i + 1; j < connection_count; j++) {
      const NYA_NeatConnection* other = &network->connections->items[j];
      nya_check(
        !(other->in == connection->in && other->out == connection->out),
        "the edge %u -> %u appears at both %u and %u",
        connection->in,
        connection->out,
        i,
        j
      );
    }
  }
}

/** XOR, so the run is a real one rather than a random walk, with the invariant check on top. */
static f64 checked_xor_trial(NYA_NeatNetwork* network) {
  check_genome(network);

  const f64 inputs[4][2] = { { 0.0, 0.0 }, { 0.0, 1.0 }, { 1.0, 0.0 }, { 1.0, 1.0 } };
  const f64 expected[4]  = { 0.0, 1.0, 1.0, 0.0 };

  f64 error = 0.0;

  for (u32 i = 0; i < 4; i++) {
    nya_nn_neat_network_flush(network);
    nya_nn_neat_network_set_sensor(network, "x", inputs[i][0]);
    nya_nn_neat_network_set_sensor(network, "y", inputs[i][1]);
    nya_nn_neat_network_run(network);

    f64 output = nya_nn_neat_network_get_output(network, "result");

    // A NaN here poisons fitness, which poisons the sort, which decides who breeds.
    nya_check(isfinite(output), "a genome produced a non-finite output");

    error += fabs(expected[i] - output);
  }

  return 4.0 - error;
}

s32 main(void) {
  setvbuf(stdout, nullptr, _IONBF, 0);

  NYA_Arena* arena = nya_arena_create(.name = "test_neat_invariants");

  NYA_NeatNetwork* seed = nya_nn_neat_network_create(arena);
  nya_nn_neat_network_push_bias(seed, "bias");
  nya_nn_neat_network_push_sensor(seed, "x");
  nya_nn_neat_network_push_sensor(seed, "y");
  nya_nn_neat_network_push_output(seed, "result");

  printf("TEST: genome invariants across 40 generations\n");
  {
    NYA_Neat* neat = nya_nn_neat_create((NYA_NeatConfig){
      .seed            = seed,
      .trial_function  = checked_xor_trial,
      .population_size = 60,
      .rng_seed            = "5EED5EED",
      .activation_function = nya_nn_neat_sigmoid,
    });

    for (u32 generation = 0; generation < 40; generation++) {
      nya_nn_neat_step(neat);
      if (nya_check_failures() > 0) break;
    }

    printf(
      "  %u genomes checked, largest %u nodes / %u connections, %u species\n",
      genomes_checked,
      max_nodes_seen,
      max_connections_seen,
      nya_nn_neat_species_count(neat)
    );

    // The run has to actually grow topology, or the invariants above were checked against a
    // population that never changed and prove nothing.
    nya_check(max_nodes_seen > seed->nodes->length, "no genome ever grew a hidden node; the run is not exercising mutation");
    nya_check(max_connections_seen > 0, "no genome ever grew a connection");

    nya_nn_neat_destroy(neat);
  }
  printf("  %s\n", nya_check_failures() == 0 ? "PASSED" : "FAILED");

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: a genome is at distance zero from itself
  // ─────────────────────────────────────────────────────────────────────────────
  printf("TEST: distance identities\n");
  {
    NYA_Neat* neat = nya_nn_neat_create((NYA_NeatConfig){
      .seed            = seed,
      .trial_function  = checked_xor_trial,
      .population_size = 20,
      .rng_seed            = "D157A9CE",
      .activation_function = nya_nn_neat_sigmoid,
    });

    nya_nn_neat_step(neat);

    NYA_NeatNetwork* best = nya_nn_neat_best(neat);
    nya_check(best != nullptr, "no best genome after a step");

    if (best != nullptr) {
      f64 self = _nya_nn_neat_distance(neat, best, best);
      nya_check(fabs(self) < 1e-9, "a genome is at distance %.9f from itself, expected 0", self);

      NYA_NeatNetwork* clone = nya_nn_neat_network_clone(arena, best);
      f64              copy  = _nya_nn_neat_distance(neat, best, clone);
      nya_check(fabs(copy) < 1e-9, "a genome is at distance %.9f from its own clone, expected 0", copy);

      // Symmetry: the merge walks the two sides differently, so this is not free.
      f64 forward  = _nya_nn_neat_distance(neat, best, clone);
      f64 backward = _nya_nn_neat_distance(neat, clone, best);
      nya_check(fabs(forward - backward) < 1e-9, "distance is not symmetric: %.9f vs %.9f", forward, backward);
    }

    nya_nn_neat_destroy(neat);
  }
  printf("  %s\n", nya_check_failures() == 0 ? "PASSED" : "FAILED");

  nya_arena_destroy(arena);

  printf("%s: test_neat_invariants (" FMTu32 " failures)\n", nya_check_failures() == 0 ? "PASSED" : "FAILED", nya_check_failures());
  return nya_check_failures() == 0 ? 0 : 1;
}
