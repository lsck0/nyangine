/**
 * @file nn_dqn.h
 *
 * Deep Q-learning: an agent that learns which action is worth taking, from reward alone.
 *
 * ```c
 * NYA_NNDQN* agent = nya_nn_dqn_create(arena, (NYA_NNDQNConfig){
 *     .state_size = 4, .action_count = 2,
 *     .layers = {
 *         { .kind = NYA_NN_LAYER_LINEAR, .units = 64 },
 *         { .kind = NYA_NN_LAYER_RELU },
 *     },
 *     .layer_count = 2,
 * });
 *
 * // per frame
 * u32 action = nya_nn_dqn_act(agent, state);
 * apply(action);
 * nya_nn_dqn_observe(agent, state, action, reward, next_state, episode_over);
 * nya_nn_dqn_train_for(agent, delta_time_s);
 * ```
 *
 * ## Running live
 *
 * `nya_nn_dqn_train_for` is paced in gradient steps per second and takes a delta, exactly as
 * nya_nn_neat_step_for does for NEAT — so a run looks the same on a 60Hz monitor and a 144Hz one,
 * and a frame that stalls does not try to make up the whole gap at once.
 *
 * Acting and training are deliberately separate calls. A game wants to act every frame and train
 * far less often, and the two costs are nothing alike: acting is one forward pass over a single
 * state with no tape, training is a forward and backward pass over a whole batch.
 *
 * ## Why the pieces are here
 *
 * Q-learning on a neural network diverges if implemented literally, and the three things that fix it
 * are all in this file rather than left to the caller:
 *
 * **Replay.** Consecutive frames are almost the same state, and training on them in order means
 * every batch is highly correlated — the network chases the last few seconds and forgets everything
 * else. Transitions go into a ring buffer and batches are drawn uniformly from it.
 *
 * **A target network.** The regression target contains the network's own output, so updating the
 * network moves the target it is being fitted to. A second, slowly synchronised copy supplies the
 * target instead, which turns a moving goalpost into a stationary one for a while.
 *
 * **Double Q-learning.** `max` over a noisy estimate is biased upwards, and that bias compounds
 * through bootstrapping until values run away. Choosing the action with the online network and
 * evaluating it with the target network removes most of it, for the cost of one extra forward pass.
 * See NYA_NNDQNConfig.disable_double_q for the argument against turning it off.
 * */
#pragma once

#include "nyangine/base/base_arena.h"
#include "nyangine/base/base_types.h"
#include "nyangine/math/math_random.h"
#include "nyangine/nn/nn_layer.h"
#include "nyangine/nn/nn_optim.h"
#include "nyangine/nn/nn_tensor.h"

typedef struct NYA_NNDQN       NYA_NNDQN;
typedef struct NYA_NNDQNConfig NYA_NNDQNConfig;
typedef struct NYA_NNDQNLayerConfig NYA_NNDQNLayerConfig;

#ifndef NYA_NN_DQN_MAX_LAYERS
#define NYA_NN_DQN_MAX_LAYERS 16
#endif

struct NYA_NNDQNLayerConfig {
    NYA_NNLayerKind kind;
    u32             units;
};

struct NYA_NNDQNConfig {
    /** Numbers describing one observation. Required. */
    u32 state_size;

    /** How many discrete actions the agent chooses between. Required. */
    u32 action_count;

    /**
     * Hidden layers, in order.
     *
     * A linear layer uses `units`; activations ignore it. The output layer is appended
     * automatically as a final linear layer to `action_count`.
     */
    NYA_NNDQNLayerConfig layers[NYA_NN_DQN_MAX_LAYERS];

    /** How many entries in `layers` are live. Zero means one hidden linear layer + ReLU. */
    u32 layer_count;

    /** Transitions kept for replay. Zero means 10000. */
    u32 replay_capacity;

    /** Transitions per gradient step. Zero means 64. */
    u32 batch_size;

    /**
     * Discount on future reward, in [0, 1). Zero means 0.99.
     *
     * How far ahead the agent looks. At 0.99 a reward is still worth a third of its value a hundred
     * steps later; at 0.9 it is worth almost nothing after fifty. Too high on a task with short
     * episodes makes the value estimate mostly noise about a distant future that never arrives.
     * */
    f32 discount;

    /** Adam's learning rate. Zero means 1e-3. */
    f32 learning_rate;

    /** Huber delta for the temporal difference loss. Zero means 1. */
    f32 huber_delta;

    /** Gradients clipped to this before the update. Zero means 10. Negative disables it. */
    f32 gradient_clip;

    /*
     * ── Exploration ──
     *
     * Epsilon-greedy: act at random with probability epsilon, greedily otherwise. Epsilon falls from
     * `exploration_start` to `exploration_end` over `exploration_steps` gradient steps.
     */

    /** Zero means 1: start by acting entirely at random. */
    f32 exploration_start;

    /** Zero means 0.05. Never anneal to exactly zero, or the agent stops discovering its own errors. */
    f32 exploration_end;

    /** Zero means 10000. */
    u32 exploration_steps;

    /*
     * ── Target network ──
     */

    /**
     * Fraction of the online network mixed into the target each step. Zero means 0.005.
     *
     * A soft update every step rather than a hard copy every N. Both work; this one has no
     * discontinuity, so the loss does not jump every time the targets are replaced.
     * */
    f32 target_tau;

    /*
     * ── Pacing ──
     */

    /** Gradient steps per second, for nya_nn_dqn_train_for. Zero means 30. */
    f32 train_steps_per_second;

    /** Most gradient steps one nya_nn_dqn_train_for may run, however far behind. Zero means 4. */
    u32 max_steps_per_frame;

    /**
     * Wall clock ceiling on one nya_nn_dqn_train_for, in milliseconds. Zero means no limit.
     *
     * The same distinction NYA_NeatConfig.max_step_milliseconds draws, and for the same reason: a
     * step count is a budget on how many, not on how long. One gradient step costs a forward and a
     * backward pass over `batch_size` rows through the whole hidden stack, so its price scales with
     * the network — and a count that was reasonable for a small net silently becomes a frame killer
     * for a larger one.
     *
     * Measured on the gnyame demo, which asks for 800 steps per second with a cap of twelve: at
     * 62.5 FPS that is 12.8 steps owed per frame, so it ran the full twelve every single frame and
     * spent 3.7 ms of a 16 ms timestep on training alone, every frame, forever.
     *
     * Checked between steps, so the first one always runs. That bounds the overshoot to a single
     * step rather than eliminating it, exactly as NEAT does — a budget that can decline to make any
     * progress is a budget that stops the agent learning on a slow machine.
     * */
    f64 max_step_milliseconds;

    /**
     * Transitions that must exist before training starts. Zero means four batches' worth.
     *
     * Training on the first few transitions means training on a batch that is mostly the same
     * moment repeated, which is the correlation replay exists to remove.
     * */
    u32 learning_starts;

    /** Uppercase hex, as NYA_RNG wants. Null seeds from the system entropy source. */
    NYA_ConstCString rng_seed;

    /**
     * Turns off double Q-learning, using the target network for both the choice and the value.
     *
     * Here to be measured against, not because it is ever the better default. Plain DQN
     * systematically overestimates — `max` over noisy estimates is biased upward, and bootstrapping
     * feeds that bias back in — and on anything with stochastic reward it will eventually run away.
     * */
    b8 disable_double_q;
};

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * FUNCTIONS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/** Builds the agent: two networks, an optimizer, and a replay buffer. */
NYA_API NYA_NNDQN* nya_nn_dqn_create(NYA_Arena* arena, NYA_NNDQNConfig config) __attr_no_discard;

/**
 * Chooses an action for `state`, exploring according to the current epsilon.
 *
 * `state` holds `state_size` values. One forward pass over a single row, with no tape recorded, so
 * this is cheap enough to call every frame.
 * */
NYA_API u32 nya_nn_dqn_act(NYA_NNDQN* dqn, const f32* state) __attr_no_discard;

/** The greedy action, ignoring exploration entirely. What a trained agent should be judged on. */
NYA_API u32 nya_nn_dqn_act_greedy(NYA_NNDQN* dqn, const f32* state) __attr_no_discard;

/** Every action's estimated value for `state`, written into `out_values`. For a debug overlay. */
NYA_API void nya_nn_dqn_action_values(NYA_NNDQN* dqn, const f32* state, f32* out_values);

/**
 * Records one transition. `next_state` is ignored when `terminal` is true.
 *
 * `terminal` must mean the episode genuinely ended, not that it was cut off at a time limit — a
 * terminal transition tells the agent there is no future reward at all, and saying that about a run
 * that was merely interrupted teaches it that the cutoff is a cliff.
 * */
NYA_API void nya_nn_dqn_observe(NYA_NNDQN* dqn, const f32* state, u32 action, f32 reward, const f32* next_state, b8 terminal);

/**
 * One gradient step. Returns the loss, or zero when there is not enough replay to train on yet.
 *
 * For a trainer in control of its own loop. A game loop wants nya_nn_dqn_train_for instead.
 * */
NYA_API f32 nya_nn_dqn_train_step(NYA_NNDQN* dqn);

/**
 * Runs however many gradient steps `delta_time_s` has earned, at the configured rate.
 *
 * The counterpart to nya_nn_neat_step_for. Returns the last loss seen, or zero if it did not train.
 * */
NYA_API f32 nya_nn_dqn_train_for(NYA_NNDQN* dqn, f32 delta_time_s);

/*
 * ── Inspection ──
 */

NYA_API u64 nya_nn_dqn_train_step_count(const NYA_NNDQN* dqn) __attr_no_discard;
NYA_API u32 nya_nn_dqn_replay_count(const NYA_NNDQN* dqn) __attr_no_discard;
NYA_API f32 nya_nn_dqn_exploration(const NYA_NNDQN* dqn) __attr_no_discard;

/** Mean loss over recent gradient steps. The number to watch to see whether it is learning. */
NYA_API f32 nya_nn_dqn_average_loss(const NYA_NNDQN* dqn) __attr_no_discard;

/** The online network, for saving it or drawing it. */
NYA_API NYA_NNSequential* nya_nn_dqn_network(NYA_NNDQN* dqn) __attr_no_discard;
