#include "nyangine/nyangine.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PUBLIC API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

NYA_NNLayer* nya_nn_layer_linear(NYA_Arena* arena, NYA_RNG* rng, u32 in_features, u32 out_features) {
    nya_assert(arena != nullptr);
    nya_assert(rng != nullptr);
    nya_assert(in_features > 0 && out_features > 0, "a linear layer of %u by %u", in_features, out_features);

    NYA_NNLayer* layer = nya_arena_alloc(arena, sizeof(NYA_NNLayer));

    *layer = (NYA_NNLayer){
        .kind = NYA_NN_LAYER_LINEAR,

        // [in, out] rather than [out, in]: the forward pass is x * W with x as [batch, in], so this
        // orientation makes the matmul a straight row-major walk with no transpose anywhere.
        .weight = nya_nn_tensor_create(arena, NYA_NN_SHAPE(in_features, out_features), true),
        .bias   = nya_nn_tensor_create(arena, NYA_NN_SHAPE(out_features), true),
    };

    nya_nn_tensor_fill_kaiming(layer->weight, rng, in_features);

    return layer;
}

NYA_NNLayer* nya_nn_layer_relu(NYA_Arena* arena) {
    nya_assert(arena != nullptr);

    NYA_NNLayer* layer = nya_arena_alloc(arena, sizeof(NYA_NNLayer));
    *layer             = (NYA_NNLayer){ .kind = NYA_NN_LAYER_RELU };

    return layer;
}

NYA_NNLayer* nya_nn_layer_tanh(NYA_Arena* arena) {
    nya_assert(arena != nullptr);

    NYA_NNLayer* layer = nya_arena_alloc(arena, sizeof(NYA_NNLayer));
    *layer             = (NYA_NNLayer){ .kind = NYA_NN_LAYER_TANH };

    return layer;
}

NYA_NNTensor* nya_nn_layer_forward(NYA_NNLayer* layer, NYA_NNGraph* graph, NYA_NNTensor* input) {
    nya_assert(layer != nullptr);
    nya_assert(graph != nullptr);
    nya_assert(input != nullptr);

    switch (layer->kind) {
        case NYA_NN_LAYER_LINEAR: return nya_nn_bias(graph, nya_nn_matmul(graph, input, layer->weight), layer->bias);
        case NYA_NN_LAYER_RELU:   return nya_nn_relu(graph, input);
        case NYA_NN_LAYER_TANH:   return nya_nn_tanh(graph, input);

        case NYA_NN_LAYER_KIND_COUNT:
        default:                  nya_panic("unknown layer kind %d", (int)layer->kind);
    }
}

NYA_NNSequential* nya_nn_sequential_create(NYA_Arena* arena) {
    nya_assert(arena != nullptr);

    NYA_NNSequential* sequential = nya_arena_alloc(arena, sizeof(NYA_NNSequential));
    *sequential                  = (NYA_NNSequential){ 0 };

    return sequential;
}

void nya_nn_sequential_push(NYA_NNSequential* sequential, NYA_NNLayer* layer) {
    nya_assert(sequential != nullptr);
    nya_assert(layer != nullptr);
    nya_assert(sequential->layer_count < NYA_NN_SEQUENTIAL_MAX_LAYERS, "more than %d layers; raise NYA_NN_SEQUENTIAL_MAX_LAYERS", NYA_NN_SEQUENTIAL_MAX_LAYERS);

    sequential->layers[sequential->layer_count++] = layer;
}

NYA_NNTensor* nya_nn_sequential_forward(NYA_NNSequential* sequential, NYA_NNGraph* graph, NYA_NNTensor* input) {
    nya_assert(sequential != nullptr);

    NYA_NNTensor* activation = input;
    for (u32 i = 0; i < sequential->layer_count; i++) activation = nya_nn_layer_forward(sequential->layers[i], graph, activation);

    return activation;
}

u32 nya_nn_sequential_parameters(NYA_NNSequential* sequential, NYA_NNTensor** out_parameters, u32 capacity) {
    nya_assert(sequential != nullptr);
    nya_assert(out_parameters != nullptr);

    u32 count = 0;

    for (u32 i = 0; i < sequential->layer_count; i++) {
        NYA_NNLayer* layer = sequential->layers[i];

        if (layer->weight != nullptr) {
            nya_assert(count < capacity, "more parameters than the %u the caller made room for", capacity);
            out_parameters[count++] = layer->weight;
        }

        if (layer->bias != nullptr) {
            nya_assert(count < capacity, "more parameters than the %u the caller made room for", capacity);
            out_parameters[count++] = layer->bias;
        }
    }

    return count;
}

void nya_nn_sequential_copy_parameters(NYA_NNSequential* destination, const NYA_NNSequential* source) {
    // A hard copy is soft update with tau = 1, so there is one implementation of the loop and no way
    // for the two to disagree about which direction they copy in.
    nya_nn_sequential_soft_update(destination, source, 1.0F);
}

void nya_nn_sequential_soft_update(NYA_NNSequential* destination, const NYA_NNSequential* source, f32 tau) {
    nya_assert(destination != nullptr);
    nya_assert(source != nullptr);
    nya_assert(destination->layer_count == source->layer_count, "soft update between networks of %u and %u layers", destination->layer_count, source->layer_count);

    for (u32 i = 0; i < destination->layer_count; i++) {
        NYA_NNLayer*       target = destination->layers[i];
        const NYA_NNLayer* online = source->layers[i];

        nya_assert(target->kind == online->kind, "soft update between different layer kinds at index %u", i);

        if (target->weight != nullptr) nya_nn_tensor_lerp_(target->weight, online->weight, tau);
        if (target->bias != nullptr) nya_nn_tensor_lerp_(target->bias, online->bias, tau);
    }
}
