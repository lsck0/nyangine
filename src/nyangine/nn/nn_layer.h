/**
 * @file nn_layer.h
 *
 * Layers and a sequential container: the network on top of nn_tensor.h.
 *
 * ```c
 * NYA_NNSequential* q = nya_nn_sequential_create(arena);
 * nya_nn_sequential_push(q, nya_nn_layer_linear(arena, &rng, 4, 64));
 * nya_nn_sequential_push(q, nya_nn_layer_relu(arena));
 * nya_nn_sequential_push(q, nya_nn_layer_linear(arena, &rng, 64, 2));
 *
 * NYA_NNTensor* values = nya_nn_sequential_forward(q, graph, states);   // [batch, 2]
 * ```
 *
 * Parameters live on the arena the layer was created with, so they survive nya_nn_graph_reset;
 * activations come from the graph and do not. See nn_tensor.h for why those are separate.
 *
 * A layer is a tagged struct rather than a vtable. There are few enough kinds that a switch is
 * shorter than the indirection, and it keeps a layer trivially copyable — which is what
 * nya_nn_sequential_copy_parameters needs for a DQN target network.
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
 *
 * Zero biases rather than random ones: the weights already break the symmetry between units, and a
 * random bias only offsets where each unit starts on its activation, which is not information the
 * network benefits from being given at random.
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
 *
 * A DQN's target network, synchronised. Copying rather than sharing is the entire point of having
 * one: the targets have to stop moving for a while, or the network is chasing a value that changes
 * every time it is updated.
 * */
NYA_API void nya_nn_sequential_copy_parameters(NYA_NNSequential* destination, const NYA_NNSequential* source);

/**
 * Moves `destination` a fraction `tau` towards `source`, parameter by parameter.
 *
 * The soft alternative to a periodic hard copy: instead of the targets jumping every N steps, they
 * drift continuously. Same stabilising effect, no discontinuity. Around 0.005 is typical.
 * */
NYA_API void nya_nn_sequential_soft_update(NYA_NNSequential* destination, const NYA_NNSequential* source, f32 tau);
