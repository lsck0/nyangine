/**
 * @file nn_dqn.h
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
 * */
#pragma once

#include "nyangine-std/base/base_arena.h"
#include "nyangine-std/base/base_types.h"
#include "nyangine-std/math/math_random.h"
#include "nyangine-core/nn/nn_layer.h"
#include "nyangine-core/nn/nn_optim.h"
#include "nyangine-core/nn/nn_tensor.h"

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
     * */
    f32 discount;

    /** Adam's learning rate. Zero means 1e-3. */
    f32 learning_rate;

    /** Huber delta for the temporal difference loss. Zero means 1. */
    f32 huber_delta;

    /** Gradients clipped to this before the update. Zero means 10. Negative disables it. */
    f32 gradient_clip;

    // Exploration: epsilon-greedy, epsilon falling from `exploration_start` to `exploration_end` over `exploration_steps` gradient steps.

    /** Zero means 1: start by acting entirely at random. */
    f32 exploration_start;

    /** Zero means 0.05. Never anneal to exactly zero, or the agent stops discovering its own errors. */
    f32 exploration_end;

    /** Zero means 10000. */
    u32 exploration_steps;

    // ── Target network ──

    /**
     * Fraction of the online network mixed into the target each step. Zero means 0.005.
     * */
    f32 target_tau;

    // ── Pacing ──

    /** Gradient steps per second, for nya_nn_dqn_train_for. Zero means 30. */
    f32 train_steps_per_second;

    /** Most gradient steps one nya_nn_dqn_train_for may run, however far behind. Zero means 4. */
    u32 max_steps_per_frame;

    /**
     * Wall clock ceiling on one nya_nn_dqn_train_for, in milliseconds. Zero means no limit.
     * */
    f64 max_step_milliseconds;

    /**
     * Transitions that must exist before training starts. Zero means four batches' worth.
     * */
    u32 learning_starts;

    /** Uppercase hex, as NYA_RNG wants. Null seeds from the system entropy source. */
    NYA_ConstCString rng_seed;

    /**
     * Turns off double Q-learning, using the target network for both the choice and the value.
     * */
    b8 disable_double_q;
};

// FUNCTIONS

/** Builds the agent: two networks, an optimizer, and a replay buffer. */
NYA_API NYA_NNDQN* nya_nn_dqn_create(NYA_Arena* arena, NYA_NNDQNConfig config) __attr_no_discard;

/**
 * Chooses an action for `state`, exploring according to the current epsilon.
 * */
NYA_API u32 nya_nn_dqn_act(NYA_NNDQN* dqn, const f32* state) __attr_no_discard;

/** The greedy action, ignoring exploration entirely. What a trained agent should be judged on. */
NYA_API u32 nya_nn_dqn_act_greedy(NYA_NNDQN* dqn, const f32* state) __attr_no_discard;

/** Every action's estimated value for `state`, written into `out_values`. For a debug overlay. */
NYA_API void nya_nn_dqn_action_values(NYA_NNDQN* dqn, const f32* state, f32* out_values);

/**
 * Records one transition. `next_state` is ignored when `terminal` is true.
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

// ── Inspection ──

NYA_API u64 nya_nn_dqn_train_step_count(const NYA_NNDQN* dqn) __attr_no_discard;
NYA_API u32 nya_nn_dqn_replay_count(const NYA_NNDQN* dqn) __attr_no_discard;
NYA_API f32 nya_nn_dqn_exploration(const NYA_NNDQN* dqn) __attr_no_discard;

/** Mean loss over recent gradient steps. The number to watch to see whether it is learning. */
NYA_API f32 nya_nn_dqn_average_loss(const NYA_NNDQN* dqn) __attr_no_discard;

/** The online network, for saving it or drawing it. */
NYA_API NYA_NNSequential* nya_nn_dqn_network(NYA_NNDQN* dqn) __attr_no_discard;
