/**
 * @file nn_optim.h
 *
 * Optimizers: what turns accumulated gradients into a weight update.
 *
 * ```c
 * NYA_NNOptimizer* optimizer = nya_nn_optimizer_adam(arena, (NYA_NNOptimizerConfig){ .learning_rate = 1e-3F });
 * nya_nn_optimizer_add_sequential(optimizer, network);
 *
 * for (;;) {
 *     nya_nn_optimizer_zero_grad(optimizer);
 *
 *     NYA_NNTensor* loss = ...;
 *     nya_nn_backward(graph, loss);
 *
 *     nya_nn_optimizer_step(optimizer);
 *     nya_nn_graph_reset(graph);
 * }
 * ```
 *
 * The order matters and is the same every time: zero, forward, backward, step, reset. Gradients
 * accumulate by design — that is what lets one parameter be used twice in a pass — so a run that
 * forgets to zero them does not fail, it just trains on the sum of every step so far and diverges.
 * */
#pragma once

#include "nyangine/base/base_arena.h"
#include "nyangine/base/base_types.h"
#include "nyangine/nn/nn_layer.h"
#include "nyangine/nn/nn_tensor.h"

typedef struct NYA_NNOptimizer       NYA_NNOptimizer;
typedef struct NYA_NNOptimizerConfig NYA_NNOptimizerConfig;
typedef enum NYA_NNOptimizerKind     NYA_NNOptimizerKind;

/** Parameter tensors one optimizer may own. Two per linear layer. */
#ifndef NYA_NN_OPTIMIZER_MAX_PARAMETERS
#define NYA_NN_OPTIMIZER_MAX_PARAMETERS 64
#endif

enum NYA_NNOptimizerKind {
    /** Plain gradient descent, with optional momentum. */
    NYA_NN_OPTIMIZER_SGD,

    /**
     * Adam: per parameter step sizes from running estimates of the gradient's mean and variance.
     *
     * The default choice for DQN, and not only out of habit. The gradient scale of a value network
     * changes by orders of magnitude over a run as rewards are discovered and targets shift, and a
     * single global learning rate that suits the start is wrong later. Adam normalises each
     * parameter's step by its own recent gradient magnitude, which absorbs most of that.
     * */
    NYA_NN_OPTIMIZER_ADAM,

    NYA_NN_OPTIMIZER_KIND_COUNT,
};

struct NYA_NNOptimizerConfig {
    /** Zero means 1e-3, which is the usual starting point for Adam on a small value network. */
    f32 learning_rate;

    /** SGD only. Zero is plain descent. 0.9 is the usual value when it is used at all. */
    f32 momentum;

    /** Adam's decay for the gradient mean. Zero means 0.9. */
    f32 beta1;

    /** Adam's decay for the gradient variance. Zero means 0.999. */
    f32 beta2;

    /** Added to the denominator so a parameter with no gradient history does not divide by zero. Zero means 1e-8. */
    f32 epsilon;

    /**
     * L2 penalty, applied to the gradient. Zero disables it.
     *
     * Off by default: it is a regulariser for supervised learning on a fixed dataset, and in
     * reinforcement learning it pulls value estimates towards zero, which is a bias the returns did
     * not ask for.
     * */
    f32 weight_decay;

    /**
     * Gradients are clipped to this magnitude before the step. Zero disables it.
     *
     * Worth having on for DQN. A bootstrapped target can occasionally be very wrong, and one huge
     * gradient can undo a long run's worth of learning in a single step. Huber loss bounds the
     * gradient of the loss itself; this bounds what reaches the weights after backprop through the
     * layers, which is not the same thing.
     * */
    f32 gradient_clip;
};

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * FUNCTIONS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

NYA_API NYA_NNOptimizer* nya_nn_optimizer_sgd(NYA_Arena* arena, NYA_NNOptimizerConfig config) __attr_no_discard;
NYA_API NYA_NNOptimizer* nya_nn_optimizer_adam(NYA_Arena* arena, NYA_NNOptimizerConfig config) __attr_no_discard;

/** Registers one parameter. Its state buffers are allocated here, so this cannot be called per step. */
NYA_API void nya_nn_optimizer_add(NYA_NNOptimizer* optimizer, NYA_NNTensor* parameter);

/** Registers every parameter of a network. The normal way to set one up. */
NYA_API void nya_nn_optimizer_add_sequential(NYA_NNOptimizer* optimizer, NYA_NNSequential* sequential);

/** Zeroes every registered parameter's gradient. Call before each backward pass. */
NYA_API void nya_nn_optimizer_zero_grad(NYA_NNOptimizer* optimizer);

/** Applies one update to every registered parameter from its accumulated gradient. */
NYA_API void nya_nn_optimizer_step(NYA_NNOptimizer* optimizer);

/** The learning rate in use, for a schedule that wants to change it between steps. */
NYA_API void nya_nn_optimizer_set_learning_rate(NYA_NNOptimizer* optimizer, f32 learning_rate);
NYA_API f32  nya_nn_optimizer_get_learning_rate(const NYA_NNOptimizer* optimizer) __attr_no_discard;
