/**
 * @file nn_tensor.h
 *
 * Tensors and reverse-mode autograd: the arithmetic layer everything else in nn/ is built on.
 *
 * ```c
 * NYA_NNGraph* graph = nya_nn_graph_create(arena);
 *
 * NYA_NNTensor* w = nya_nn_tensor_create(arena, NYA_NN_SHAPE(4, 2), true);
 * nya_nn_tensor_fill_uniform(w, &rng, -0.5F, 0.5F);
 *
 * NYA_NNTensor* x    = nya_nn_tensor_from(graph, NYA_NN_SHAPE(1, 4), (f32[]){ 1, 2, 3, 4 });
 * NYA_NNTensor* y    = nya_nn_relu(graph, nya_nn_matmul(graph, x, w));
 * NYA_NNTensor* loss = nya_nn_mse(graph, y, target);
 *
 * nya_nn_backward(graph, loss);   // w->grad now holds dloss/dw
 * nya_nn_graph_reset(graph);      // activations gone, w and its grad survive
 * ```
 *
 * ## Define by run
 *
 * There is no compiled graph. Every op appends its result to a tape as it executes, and backward
 * walks that tape in reverse. It costs a pointer push per op and it means control flow in C is
 * control flow in the network — which is what makes a DQN's "one branch for the online net, another
 * for the target net" ordinary code rather than a graph construct.
 *
 * ## Two lifetimes, deliberately
 *
 * **Parameters** are created against a long-lived arena and own their gradient buffer. They outlive
 * every forward pass and are what an optimizer updates.
 *
 * **Activations** are created against the graph, which owns a scratch arena that is reset wholesale
 * between steps. A training loop therefore allocates nothing per step after the first, and cannot
 * leak intermediates — the failure mode that makes naive autograd unusable in a game loop.
 *
 * Mixing the two is the one thing to be careful about: an activation must never outlive the
 * nya_nn_graph_reset that frees it, so nothing holds an activation across steps.
 *
 * ## Shapes
 *
 * Row major, up to NYA_NN_TENSOR_MAX_DIMS dimensions, though the ops here are deliberately 2D:
 * `[batch, features]`. That is what a fully connected value network needs and nothing more. A
 * convolution would want real N-dimensional strides, and pretending otherwise now would be a
 * generality that is never exercised and quietly wrong when it finally is.
 * */
#pragma once

#include "nyangine/base/base_arena.h"
#include "nyangine/base/base_types.h"
#include "nyangine/math/math_random.h"

typedef struct NYA_NNTensor NYA_NNTensor;
typedef struct NYA_NNGraph  NYA_NNGraph;
typedef enum NYA_NNOp       NYA_NNOp;

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * CONSTANTS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/** Dimensions a tensor may have. Four covers [batch, channels, height, width] if it ever matters. */
#define NYA_NN_TENSOR_MAX_DIMS 4

/**
 * Tensors one graph may hold before the tape is full.
 *
 * A fixed tape rather than a growing one: the count is a property of the network's shape, not of the
 * data, so a run that exceeds this has a structural bug — a forward pass in a loop without a reset —
 * and failing loudly at a known bound finds that in seconds where a growing array hides it until
 * memory runs out.
 * */
#ifndef NYA_NN_GRAPH_MAX_TENSORS
#define NYA_NN_GRAPH_MAX_TENSORS 4096
#endif

/** Builds a shape literal. `NYA_NN_SHAPE(8, 4)` is a rank 2 shape of 8 rows by 4 columns. */
#define NYA_NN_SHAPE(...) ((NYA_NNShape){ .dims = { __VA_ARGS__ }, .rank = nya_carray_length(((u32[]){ __VA_ARGS__ })) })

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * TYPES
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

typedef struct NYA_NNShape {
    u32 dims[NYA_NN_TENSOR_MAX_DIMS];
    u32 rank;
} NYA_NNShape;

/**
 * How a tensor was produced. A leaf is NYA_NN_OP_NONE; everything else has inputs and a backward.
 *
 * Recorded per tensor rather than as a separate node type, because a tensor and the op that made it
 * are one-to-one — a second object would be a pointer to chase and a lifetime to match.
 * */
enum NYA_NNOp {
    /** A leaf: a parameter, an input, or a constant. Backward stops here. */
    NYA_NN_OP_NONE,

    NYA_NN_OP_ADD,
    NYA_NN_OP_SUB,

    /** Elementwise, not matrix. Same shape both sides. */
    NYA_NN_OP_MUL,

    /** Times a constant, held in `scalar`. */
    NYA_NN_OP_SCALE,

    /** [m, k] by [k, n]. The only op here that is not elementwise. */
    NYA_NN_OP_MATMUL,

    /** A row vector [n] added to every row of an [m, n]. Separate from ADD so it broadcasts. */
    NYA_NN_OP_BIAS,

    NYA_NN_OP_RELU,
    NYA_NN_OP_TANH,

    /**
     * One column per row, picked by `indices`. Produces [m, 1].
     *
     * The op that makes DQN expressible: the loss is against Q(s, a) for the action actually taken,
     * which is one entry of each row of the network's output, and the gradient must reach only that
     * entry.
     * */
    NYA_NN_OP_GATHER,

    NYA_NN_OP_SUM,
    NYA_NN_OP_MEAN,

    /** Mean squared error against a target, reduced to a scalar. */
    NYA_NN_OP_MSE,

    /**
     * Huber loss against a target, reduced to a scalar. `scalar` is delta.
     *
     * The usual choice for DQN over plain MSE: bootstrapped targets are noisy and occasionally very
     * wrong, and squared error lets one bad target dominate a batch's gradient. Huber is quadratic
     * near zero and linear past delta, so a large error moves the weights by a bounded amount.
     * */
    NYA_NN_OP_HUBER,

    NYA_NN_OP_COUNT,
};

struct NYA_NNTensor {
    /** Row major, `count` elements. Never null. */
    f32* data;

    /**
     * Accumulated gradient, same length as `data`. Null when the tensor does not require one.
     *
     * Accumulated rather than assigned, which is what makes a tensor used twice in one forward pass
     * come out with the sum of both paths' gradients — the whole reason backward is a reverse sweep
     * over a tape rather than a recursive walk.
     * */
    f32* grad;

    u32 shape[NYA_NN_TENSOR_MAX_DIMS];
    u32 rank;

    /** Product of the shape. Cached because every op needs it and none of them changes the shape. */
    u32 count;

    b8 requires_grad;

    /*
     * ── Autograd ──
     */

    NYA_NNOp      op;
    NYA_NNTensor* inputs[2];

    /** The op's constant, where it has one: the scale factor, Huber's delta. */
    f32 scalar;

    /** NYA_NN_OP_GATHER's column per row. `shape[0]` entries. */
    const u32* indices;
};

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * FUNCTIONS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/**
 * Creates a graph: a scratch arena for activations plus the tape recording them.
 *
 * `arena` owns the graph itself. The activations come from an arena the graph creates inside it,
 * which is what nya_nn_graph_reset empties.
 * */
NYA_API NYA_NNGraph* nya_nn_graph_create(NYA_Arena* arena) __attr_no_discard;

/**
 * Drops every activation and empties the tape. Parameters are untouched, and so are their gradients.
 *
 * Call once per training step, after backward. Nothing produced by an op survives this.
 * */
NYA_API void nya_nn_graph_reset(NYA_NNGraph* graph);

/** Tensors currently on the tape. For a test or a leak hunt, not for a training loop. */
NYA_API u32 nya_nn_graph_tensor_count(const NYA_NNGraph* graph) __attr_no_discard;

/**
 * Bytes the graph's activation arena is currently handing out.
 *
 * The number that says whether a training loop has reached steady state. After the first pass it
 * should be identical from one step to the next: same shapes, same ops, same arena reset in between.
 * A figure that keeps climbing means something is being allocated per step and never released, which
 * inside a frame loop is a leak in everything but name.
 * */
NYA_API u64 nya_nn_graph_memory_usage_bytes(const NYA_NNGraph* graph) __attr_no_discard;

/**
 * Stops ops recording to the tape until nya_nn_graph_grad_end.
 *
 * Inference does not need a backward pass, and building a tape for one is pure cost — for DQN it is
 * also most of the work, since acting greedily and evaluating the target network happen far more
 * often than a gradient step. Tensors made inside still come from the graph's arena, so a reset
 * still frees them.
 * */
NYA_API void nya_nn_graph_grad_begin(NYA_NNGraph* graph);
NYA_API void nya_nn_graph_grad_end(NYA_NNGraph* graph);

/*
 * ── Creation ──
 */

/** A tensor of zeros against `arena`. Use for parameters, which outlive the graph. */
NYA_API NYA_NNTensor* nya_nn_tensor_create(NYA_Arena* arena, NYA_NNShape shape, b8 requires_grad) __attr_no_discard;

/** A tensor of zeros against the graph, freed by the next reset. Use for inputs and targets. */
NYA_API NYA_NNTensor* nya_nn_tensor_zeros(NYA_NNGraph* graph, NYA_NNShape shape) __attr_no_discard;

/** The same, filled from `values`, which is copied. `values` must hold the shape's element count. */
NYA_API NYA_NNTensor* nya_nn_tensor_from(NYA_NNGraph* graph, NYA_NNShape shape, const f32* values) __attr_no_discard;

/*
 * ── Leaves ──
 */

NYA_API void nya_nn_tensor_fill(NYA_NNTensor* tensor, f32 value);
NYA_API void nya_nn_tensor_fill_uniform(NYA_NNTensor* tensor, NYA_RNG* rng, f32 min, f32 max);

/**
 * Fills with the Kaiming uniform initialisation for a `fan_in` wide layer.
 *
 * Bounds of +/- sqrt(6 / fan_in). Sized so the variance of a layer's output matches the variance of
 * its input under ReLU, which is what stops the signal from either vanishing or exploding as layers
 * are stacked. A network initialised uniformly on a fixed range instead trains, slowly, and gets
 * worse the deeper it is.
 * */
NYA_API void nya_nn_tensor_fill_kaiming(NYA_NNTensor* tensor, NYA_RNG* rng, u32 fan_in);

/** Zeroes the gradient. Nothing else clears it, since gradients accumulate by design. */
NYA_API void nya_nn_tensor_zero_grad(NYA_NNTensor* tensor);

/** Copies `source`'s data into `destination`. Shapes must match; gradients are not copied. */
NYA_API void nya_nn_tensor_copy(NYA_NNTensor* destination, const NYA_NNTensor* source);

/**
 * destination = (1 - tau) * destination + tau * source, elementwise.
 *
 * Polyak averaging, for pulling a DQN target network slowly towards the online one. A hard copy
 * every N steps works too and is a special case of this with tau = 1.
 * */
NYA_API void nya_nn_tensor_lerp_(NYA_NNTensor* destination, const NYA_NNTensor* source, f32 tau);

/** Element at [row, column] of a rank 2 tensor. */
NYA_API f32 nya_nn_tensor_at(const NYA_NNTensor* tensor, u32 row, u32 column) __attr_no_discard;

/** The single element of a one-element tensor, which is what every loss reduces to. */
NYA_API f32 nya_nn_tensor_item(const NYA_NNTensor* tensor) __attr_no_discard;

/** Index of the largest element in `row`. Greedy action selection, for DQN. */
NYA_API u32 nya_nn_tensor_argmax_row(const NYA_NNTensor* tensor, u32 row) __attr_no_discard;

/** Largest element in `row`. */
NYA_API f32 nya_nn_tensor_max_row(const NYA_NNTensor* tensor, u32 row) __attr_no_discard;

/*
 * ── Ops ──
 *
 * Each records onto the graph's tape and returns a tensor the graph owns.
 */

NYA_API NYA_NNTensor* nya_nn_add(NYA_NNGraph* graph, NYA_NNTensor* a, NYA_NNTensor* b) __attr_no_discard;
NYA_API NYA_NNTensor* nya_nn_sub(NYA_NNGraph* graph, NYA_NNTensor* a, NYA_NNTensor* b) __attr_no_discard;
NYA_API NYA_NNTensor* nya_nn_mul(NYA_NNGraph* graph, NYA_NNTensor* a, NYA_NNTensor* b) __attr_no_discard;
NYA_API NYA_NNTensor* nya_nn_scale(NYA_NNGraph* graph, NYA_NNTensor* a, f32 scalar) __attr_no_discard;

/** [m, k] by [k, n] to [m, n]. */
NYA_API NYA_NNTensor* nya_nn_matmul(NYA_NNGraph* graph, NYA_NNTensor* a, NYA_NNTensor* b) __attr_no_discard;

/** Adds a [n] row vector to every row of an [m, n]. */
NYA_API NYA_NNTensor* nya_nn_bias(NYA_NNGraph* graph, NYA_NNTensor* a, NYA_NNTensor* bias) __attr_no_discard;

NYA_API NYA_NNTensor* nya_nn_relu(NYA_NNGraph* graph, NYA_NNTensor* a) __attr_no_discard;
NYA_API NYA_NNTensor* nya_nn_tanh(NYA_NNGraph* graph, NYA_NNTensor* a) __attr_no_discard;

/**
 * Picks column `indices[row]` from each row, producing [m, 1].
 *
 * `indices` is borrowed, not copied, and is read again during backward — so it must outlive the
 * backward pass. A batch of actions held by the caller for the length of a step is the intended
 * shape of that.
 * */
NYA_API NYA_NNTensor* nya_nn_gather(NYA_NNGraph* graph, NYA_NNTensor* a, const u32* indices) __attr_no_discard;

NYA_API NYA_NNTensor* nya_nn_sum(NYA_NNGraph* graph, NYA_NNTensor* a) __attr_no_discard;
NYA_API NYA_NNTensor* nya_nn_mean(NYA_NNGraph* graph, NYA_NNTensor* a) __attr_no_discard;

/** Mean over all elements of (prediction - target)^2. */
NYA_API NYA_NNTensor* nya_nn_mse(NYA_NNGraph* graph, NYA_NNTensor* prediction, NYA_NNTensor* target) __attr_no_discard;

/** Huber loss with the given delta. See NYA_NN_OP_HUBER for why DQN wants this over MSE. */
NYA_API NYA_NNTensor* nya_nn_huber(NYA_NNGraph* graph, NYA_NNTensor* prediction, NYA_NNTensor* target, f32 delta) __attr_no_discard;

/*
 * ── Backward ──
 */

/** The op's name, for an error message or a graph dump. */
NYA_API NYA_ConstCString nya_nn_op_name(NYA_NNOp op) __attr_no_discard;

/** Whether every element of `tensor`, and of its gradient if it has one, is a finite number. */
NYA_API b8 nya_nn_tensor_is_finite(const NYA_NNTensor* tensor) __attr_no_discard;

/**
 * The first tensor on the tape holding a NaN or an infinity, or null when all of them are finite.
 *
 * The single most useful thing to call when training stops working. A network that goes non-finite
 * produces no error and no crash — the loss becomes NaN, every gradient becomes NaN, every weight
 * becomes NaN, and the only symptom is that the agent stops doing anything sensible several thousand
 * steps after the actual cause. Walking the tape finds the *first* op that produced one, which is
 * where the cause is.
 *
 * Ordinary causes: a learning rate high enough to diverge, a reward that is itself infinite, a
 * discount of exactly 1 on a task with no terminal state, or an exploding gradient with clipping off.
 * */
NYA_API NYA_NNTensor* nya_nn_graph_find_non_finite(const NYA_NNGraph* graph) __attr_no_discard;

/**
 * Walks the tape backwards from `loss`, accumulating into every gradient that requires one.
 *
 * `loss` must be a single element. Gradients *accumulate*, so zero them between steps — normally by
 * calling nya_nn_optimizer_zero_grad, which does it for every parameter it owns.
 * */
NYA_API void nya_nn_backward(NYA_NNGraph* graph, NYA_NNTensor* loss);
