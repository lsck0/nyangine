/**
 * @file nn_layer.h
 *
 * ```c
 * NYA_NNSequential* q = nya_nn_sequential_create(arena);
 * nya_nn_sequential_push(q, nya_nn_layer_linear(arena, &rng, 4, 64));
 * nya_nn_sequential_push(q, nya_nn_layer_relu(arena));
 * nya_nn_sequential_push(q, nya_nn_layer_linear(arena, &rng, 64, 2));
 *
 * NYA_NNTensor* values = nya_nn_sequential_forward(q, graph, states);   // [batch, 2]
 * ```
 * */
#pragma once

#include "nyangine/base/base_arena.h"
#include "nyangine/base/base_types.h"
#include "nyangine/nn/nn_tensor.h"

typedef struct NYA_NNLayer      NYA_NNLayer;
typedef struct NYA_NNSequential NYA_NNSequential;
typedef enum NYA_NNLayerKind    NYA_NNLayerKind;

/** Layers one NYA_NNSequential may hold. A value network is a handful; this is generous. */
#ifndef NYA_NN_SEQUENTIAL_MAX_LAYERS
#define NYA_NN_SEQUENTIAL_MAX_LAYERS 32
#endif

enum NYA_NNLayerKind {
    /** y = xW + b. The only kind with parameters. */
    NYA_NN_LAYER_LINEAR,

    NYA_NN_LAYER_RELU,
    NYA_NN_LAYER_TANH,

    NYA_NN_LAYER_KIND_COUNT,
};

struct NYA_NNLayer {
    NYA_NNLayerKind kind;

    /** [in_features, out_features]. Null for an activation. */
    NYA_NNTensor* weight;

    /** [out_features]. Null for an activation. */
    NYA_NNTensor* bias;
};

struct NYA_NNSequential {
    NYA_NNLayer* layers[NYA_NN_SEQUENTIAL_MAX_LAYERS];
    u32          layer_count;
};

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * FUNCTIONS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/**
 * A fully connected layer, weights Kaiming-initialised and biases zeroed.
 * */
NYA_API NYA_NNLayer* nya_nn_layer_linear(NYA_Arena* arena, NYA_RNG* rng, u32 in_features, u32 out_features) __attr_no_discard;

NYA_API NYA_NNLayer* nya_nn_layer_relu(NYA_Arena* arena) __attr_no_discard;
NYA_API NYA_NNLayer* nya_nn_layer_tanh(NYA_Arena* arena) __attr_no_discard;

/** Runs one layer. `input` is [batch, in_features]. */
NYA_API NYA_NNTensor* nya_nn_layer_forward(NYA_NNLayer* layer, NYA_NNGraph* graph, NYA_NNTensor* input) __attr_no_discard;

NYA_API NYA_NNSequential* nya_nn_sequential_create(NYA_Arena* arena) __attr_no_discard;
NYA_API void              nya_nn_sequential_push(NYA_NNSequential* sequential, NYA_NNLayer* layer);

/** Runs every layer in order. */
NYA_API NYA_NNTensor* nya_nn_sequential_forward(NYA_NNSequential* sequential, NYA_NNGraph* graph, NYA_NNTensor* input) __attr_no_discard;

/** Every parameter tensor, appended to `out_parameters`. Returns how many there were. */
NYA_API u32 nya_nn_sequential_parameters(NYA_NNSequential* sequential, NYA_NNTensor** out_parameters, u32 capacity);

/**
 * Copies every parameter from `source` into `destination`. The two must have the same architecture.
 * */
NYA_API void nya_nn_sequential_copy_parameters(NYA_NNSequential* destination, const NYA_NNSequential* source);

/**
 * Moves `destination` a fraction `tau` towards `source`, parameter by parameter.
 * */
NYA_API void nya_nn_sequential_soft_update(NYA_NNSequential* destination, const NYA_NNSequential* source, f32 tau);
