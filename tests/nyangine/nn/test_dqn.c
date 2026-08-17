/**
 * The DQN agent, judged on whether it learns to act — not on whether the loss goes down.
 *
 * A falling loss proves nothing here. A Q-network that collapses to predicting the same value for
 * every action has a very low temporal difference loss and a policy no better than random, and that
 * is the *usual* failure mode, not an exotic one. So the tests below measure the return the agent
 * actually collects, against what a random policy collects on the same task.
 *
 * The task is a corridor: the agent starts in the middle of a line and must reach the right end. It
 * is deliberately the smallest thing that is not trivial — the reward is delayed, so a bandit-style
 * agent that only credits the immediately rewarded action cannot solve it, and it requires the
 * discounting and bootstrapping that are the point of Q-learning.
 **/

#include "nyangine/nyangine.c"
#include "nyangine/nyangine.h"

/** Cells in the corridor. Odd, so there is a true middle to start from. */
#define TEST_DQN_CORRIDOR 7

/** Steps before an episode is cut off, so a dithering agent does not run forever. */
#define TEST_DQN_EPISODE_LIMIT 30

/** The corridor as a one-hot position. One-hot rather than a scalar, so nothing is inferable from magnitude. */
static void corridor_state(u32 position, f32* out_state) {
  for (u32 i = 0; i < TEST_DQN_CORRIDOR; i++) out_state[i] = i == position ? 1.0F : 0.0F;
}

/**
 * Runs one episode. `agent` may be null, which plays uniformly at random.
 *
 * Returns the undiscounted return. Reaching the right end is worth 1 and ends the episode; every
 * other step costs a little, so dithering is worse than committing.
 * */
static f32 corridor_episode(NYA_NNDQN* agent, NYA_RNG* rng, b8 training, b8 greedy) {
  f32 state[TEST_DQN_CORRIDOR];
  f32 next_state[TEST_DQN_CORRIDOR];

  u32 position = TEST_DQN_CORRIDOR / 2;
  f32 total    = 0.0F;

  for (u32 step = 0; step < TEST_DQN_EPISODE_LIMIT; step++) {
    corridor_state(position, state);

    u32 action = 0;
    if (agent == nullptr) action = nya_rng_gen_bool(rng, 0.5F) ? 1 : 0;
    else if (greedy) action = nya_nn_dqn_act_greedy(agent, state);
    else action = nya_nn_dqn_act(agent, state);

    // 0 is left, 1 is right. The edges are walls rather than wrap-around, so a wrong commitment is
    // recoverable and the agent has to learn direction rather than parity.
    if (action == 1 && position + 1 < TEST_DQN_CORRIDOR) position++;
    else if (action == 0 && position > 0) position--;

    b8  terminal = position == TEST_DQN_CORRIDOR - 1;
    f32 reward   = terminal ? 1.0F : -0.02F;

    total += reward;

    corridor_state(position, next_state);

    if (agent != nullptr && training) nya_nn_dqn_observe(agent, state, action, reward, next_state, terminal);

    if (terminal) break;
  }

  return total;
}

int main(void) {
  NYA_Arena* arena = nya_arena_create(.name = "test_dqn");
  defer      nya_arena_destroy(arena);

  NYA_RNG rng = nya_rng_create(.seed = "44514E5F74657374");

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: replay, exploration schedule and the training gate
  // ─────────────────────────────────────────────────────────────────────────────
  {
    NYA_NNDQN* agent = nya_nn_dqn_create(
      arena,
      (NYA_NNDQNConfig){
        .state_size        = 2,
        .action_count      = 2,
        .layer_count       = 2,
        .layers = {
          { .kind = NYA_NN_LAYER_LINEAR, .units = 32 },
          { .kind = NYA_NN_LAYER_RELU,   .units = 0 },
        },
        .replay_capacity   = 8,
        .batch_size        = 4,
        .learning_starts   = 6,
        .exploration_steps = 100,
        .rng_seed          = "44514E5F63666731",
      }
    );

    nya_assert(nya_nn_dqn_replay_count(agent) == 0, "a new agent has no replay");
    nya_assert(fabsf(nya_nn_dqn_exploration(agent) - 1.0F) < 1e-5F, "exploration starts at one");

    // Below learning_starts, training must decline rather than train on a handful of transitions.
    for (u32 i = 0; i < 5; i++) nya_nn_dqn_observe(agent, (f32[]){ 0.0F, 1.0F }, 0, 0.5F, (f32[]){ 1.0F, 0.0F }, false);

    nya_assert(nya_nn_dqn_replay_count(agent) == 5, "replay count after five observations");
    nya_assert(nya_nn_dqn_train_step(agent) == 0.0F, "training must not start below learning_starts");
    nya_assert(nya_nn_dqn_train_step_count(agent) == 0, "a declined training step must not be counted");

    // The ring must overwrite rather than grow past capacity.
    for (u32 i = 0; i < 20; i++) nya_nn_dqn_observe(agent, (f32[]){ 1.0F, 0.0F }, 1, -0.5F, (f32[]){ 0.0F, 1.0F }, false);

    nya_assert(nya_nn_dqn_replay_count(agent) == 8, "replay saturates at capacity, got %u", nya_nn_dqn_replay_count(agent));

    nya_assert(nya_nn_dqn_train_step(agent) != 0.0F, "training must start once the replay is deep enough");
    nya_assert(nya_nn_dqn_train_step_count(agent) == 1, "a completed training step must be counted");

    printf("  PASSED: replay and training gate\n");
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: pacing matches the configured rate, like NEAT's step_for
  // ─────────────────────────────────────────────────────────────────────────────
  {
    NYA_NNDQN* agent = nya_nn_dqn_create(
      arena,
      (NYA_NNDQNConfig){
        .state_size            = 2,
        .action_count          = 2,
        .layer_count           = 2,
        .layers = {
          { .kind = NYA_NN_LAYER_LINEAR, .units = 32 },
          { .kind = NYA_NN_LAYER_RELU,   .units = 0 },
        },
        .replay_capacity       = 64,
        .batch_size            = 8,
        .learning_starts       = 8,
        .train_steps_per_second = 10.0F,
        .max_steps_per_frame   = 4,
        .rng_seed              = "44514E5F63666732",
      }
    );

    for (u32 i = 0; i < 32; i++) nya_nn_dqn_observe(agent, (f32[]){ 0.0F, 1.0F }, i % 2, 0.1F, (f32[]){ 1.0F, 0.0F }, false);

    // One second at ten steps a second, delivered as sixty frames, is ten steps — not sixty.
    for (u32 frame = 0; frame < 60; frame++) (void)nya_nn_dqn_train_for(agent, 1.0F / 60.0F);

    u64 steps = nya_nn_dqn_train_step_count(agent);
    nya_assert(steps >= 9 && steps <= 11, "one second at 10 steps/s ran %llu steps", (unsigned long long)steps);

    // A stalled frame must not be made up all at once.
    u64 before = nya_nn_dqn_train_step_count(agent);
    (void)nya_nn_dqn_train_for(agent, 10.0F);

    u64 caught_up = nya_nn_dqn_train_step_count(agent) - before;
    nya_assert(caught_up <= 4, "a ten second stall ran %llu steps, past max_steps_per_frame", (unsigned long long)caught_up);

    printf("  PASSED: pacing (%llu steps in one simulated second)\n", (unsigned long long)steps);
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: the agent learns to walk the corridor
  // ─────────────────────────────────────────────────────────────────────────────
  {
    NYA_NNDQN* agent = nya_nn_dqn_create(
      arena,
      (NYA_NNDQNConfig){
        .state_size   = TEST_DQN_CORRIDOR,
        .action_count = 2,
        .layer_count  = 2,
        .layers = {
          { .kind = NYA_NN_LAYER_LINEAR, .units = 32 },
          { .kind = NYA_NN_LAYER_RELU,   .units = 0 },
        },

        .replay_capacity  = 2000,
        .batch_size       = 32,
        .learning_starts  = 200,
        .learning_rate    = 2e-3F,
        .discount         = 0.95F,

        // Annealed quickly: the task is small, and an agent still exploring half the time cannot be
        // judged on the return it collects.
        .exploration_steps = 1500,
        .exploration_end   = 0.02F,

        .target_tau = 0.02F,
        .rng_seed   = "44514E5F636F7272",
      }
    );

    // A random baseline on the same task, so "learned something" is measured rather than assumed.
    f32 random_return = 0.0F;
    for (u32 episode = 0; episode < 200; episode++) random_return += corridor_episode(nullptr, &rng, false, false);
    random_return /= 200.0F;

    for (u32 episode = 0; episode < 600; episode++) {
      (void)corridor_episode(agent, &rng, true, false);

      // Trained between episodes rather than inside the step loop, which keeps the environment and
      // the optimizer independent — and is how nya_nn_dqn_train_for would be used from a game loop.
      for (u32 i = 0; i < 10; i++) (void)nya_nn_dqn_train_step(agent);
    }

    f32 learned_return = 0.0F;
    for (u32 episode = 0; episode < 100; episode++) learned_return += corridor_episode(agent, &rng, false, true);
    learned_return /= 100.0F;

    printf("  random %.3f, learned %.3f, loss %.4f, epsilon %.3f\n",
           (f64)random_return, (f64)learned_return, (f64)nya_nn_dqn_average_loss(agent), (f64)nya_nn_dqn_exploration(agent));

    /*
     * The optimum is three steps right from the middle: 1.0 - 2 * 0.02 = 0.96.
     *
     * Asserted well below that, because the point is to catch a policy that did not learn, not to
     * pin the exact optimum — and a run that reaches the end at all, reliably, has learned the thing
     * the task tests. A random walk on this corridor averages well under half of it.
     */
    nya_assert(learned_return > 0.8F, "the agent returned %f, expected better than 0.8", (f64)learned_return);
    nya_assert(learned_return > random_return + 0.5F, "the agent (%f) did not clearly beat random (%f)", (f64)learned_return, (f64)random_return);

    printf("  PASSED: corridor solved\n");
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: acting is allocation free once warm
  // ─────────────────────────────────────────────────────────────────────────────
  {
    /*
     * The property that decides whether this can run inside a frame. Acting happens every frame; if
     * it grew memory each time it would be unusable however fast it was.
     */
    NYA_NNDQN* agent = nya_nn_dqn_create(
      arena,
      (NYA_NNDQNConfig){
        .state_size   = TEST_DQN_CORRIDOR,
        .action_count = 2,
        .layer_count  = 2,
        .layers = {
          { .kind = NYA_NN_LAYER_LINEAR, .units = 32 },
          { .kind = NYA_NN_LAYER_RELU,   .units = 0 },
        },
        .rng_seed = "44514E5F616C6C6F"
      }
    );

    f32 state[TEST_DQN_CORRIDOR];
    corridor_state(3, state);

    for (u32 i = 0; i < 32; i++) (void)nya_nn_dqn_act_greedy(agent, state);

    u64 settled = nya_arena_memory_usage_bytes(arena);
    for (u32 i = 0; i < 256; i++) (void)nya_nn_dqn_act_greedy(agent, state);

    nya_assert(nya_arena_memory_usage_bytes(arena) == settled, "acting grew the arena from %llu to %llu bytes",
               (unsigned long long)settled, (unsigned long long)nya_arena_memory_usage_bytes(arena));

    printf("  PASSED: acting is allocation free\n");
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: plain DQN also learns, and overestimates more than double DQN
  // ─────────────────────────────────────────────────────────────────────────────
  {
    /*
     * The disable_double_q branch existed and nothing ran it, which for a flag that changes the
     * learning rule is a whole algorithm going untested.
     *
     * It has to still solve the task — it is a valid algorithm, just a biased one — so the assertion
     * is that it learns, not that it matches. Whether it overestimates more is measured and printed
     * rather than asserted: the bias is real and well documented, but on a task this small and this
     * deterministic it is not large enough to be a reliable test signal, and asserting on it would
     * be asserting on noise.
     */
    NYA_NNDQNConfig config = {
      .state_size   = TEST_DQN_CORRIDOR,
      .action_count = 2,
      .layer_count  = 2,
      .layers = {
        { .kind = NYA_NN_LAYER_LINEAR, .units = 32 },
        { .kind = NYA_NN_LAYER_RELU,   .units = 0 },
      },

      .replay_capacity   = 2000,
      .batch_size        = 32,
      .learning_starts   = 200,
      .learning_rate     = 2e-3F,
      .discount          = 0.95F,
      .exploration_steps = 1500,
      .exploration_end   = 0.02F,
      .target_tau        = 0.02F,

      .rng_seed         = "44514E5F73696E67",
      .disable_double_q = true,
    };

    NYA_NNDQN* agent = nya_nn_dqn_create(arena, config);

    for (u32 episode = 0; episode < 600; episode++) {
      (void)corridor_episode(agent, &rng, true, false);
      for (u32 i = 0; i < 10; i++) (void)nya_nn_dqn_train_step(agent);
    }

    f32 learned_return = 0.0F;
    for (u32 episode = 0; episode < 100; episode++) learned_return += corridor_episode(agent, &rng, false, true);
    learned_return /= 100.0F;

    // The value it assigns the start state, against what the task can actually pay from there.
    f32 values[2];
    f32 start[TEST_DQN_CORRIDOR];
    corridor_state(TEST_DQN_CORRIDOR / 2, start);
    nya_nn_dqn_action_values(agent, start, values);

    printf("  plain DQN return %.3f, start value %.3f\n", (f64)learned_return, (f64)nya_max(values[0], values[1]));

    nya_assert(learned_return > 0.8F, "plain DQN returned %f, expected better than 0.8", (f64)learned_return);

    printf("  PASSED: plain DQN branch\n");
  }

  printf("PASSED: test_dqn\n");
  return 0;
}
