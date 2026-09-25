#include "nyangine-core/nyangine.h"

// TYPES

/** One registered parameter and whatever per-parameter state the optimizer keeps for it. */
typedef struct _NYA_NNOptimizerSlot {
    NYA_NNTensor* parameter;

    /** SGD's velocity, or Adam's first moment. Same buffer, different meaning per kind. */
    f32* moment1;

    /** Adam's second moment. Null for SGD. */
    f32* moment2;
} _NYA_NNOptimizerSlot;

struct NYA_NNOptimizer {
    NYA_NNOptimizerKind   kind;
    NYA_NNOptimizerConfig config;

    /** Where the moment buffers come from. Held so nya_nn_optimizer_add can allocate later. */
    NYA_Arena* allocator;

    _NYA_NNOptimizerSlot slots[NYA_NN_OPTIMIZER_MAX_PARAMETERS];
    u32                  slot_count;

    /**
     * Steps taken, for Adam's bias correction.
     * */
    u64 step_count;
};

// PRIVATE API DECLARATION

NYA_INTERNAL NYA_NNOptimizer* _nya_nn_optimizer_create(NYA_Arena* arena, NYA_NNOptimizerKind kind, NYA_NNOptimizerConfig config) __attr_no_discard;

// PUBLIC API IMPLEMENTATION

NYA_NNOptimizer* nya_nn_optimizer_sgd(NYA_Arena* arena, NYA_NNOptimizerConfig config) {
    return _nya_nn_optimizer_create(arena, NYA_NN_OPTIMIZER_SGD, config);
}

NYA_NNOptimizer* nya_nn_optimizer_adam(NYA_Arena* arena, NYA_NNOptimizerConfig config) {
    return _nya_nn_optimizer_create(arena, NYA_NN_OPTIMIZER_ADAM, config);
}

void nya_nn_optimizer_add(NYA_NNOptimizer* optimizer, NYA_NNTensor* parameter) {
    nya_assert(optimizer != nullptr);
    nya_assert(parameter != nullptr);
    nya_assert(parameter->grad != nullptr, "a parameter registered with an optimizer must require a gradient");
    nya_assert(
        optimizer->slot_count < NYA_NN_OPTIMIZER_MAX_PARAMETERS,
        "more than %d parameters; raise NYA_NN_OPTIMIZER_MAX_PARAMETERS",
        NYA_NN_OPTIMIZER_MAX_PARAMETERS
    );

    _NYA_NNOptimizerSlot* slot = &optimizer->slots[optimizer->slot_count++];
    *slot                      = (_NYA_NNOptimizerSlot){ .parameter = parameter };

    u64 bytes = (u64)parameter->count * sizeof(f32);

    slot->moment1 = nya_arena_alloc(optimizer->allocator, bytes);
    nya_memset(slot->moment1, 0, bytes);

    if (optimizer->kind != NYA_NN_OPTIMIZER_ADAM) return;

    slot->moment2 = nya_arena_alloc(optimizer->allocator, bytes);
    nya_memset(slot->moment2, 0, bytes);
}

void nya_nn_optimizer_add_sequential(NYA_NNOptimizer* optimizer, NYA_NNSequential* sequential) {
    nya_assert(optimizer != nullptr);
    nya_assert(sequential != nullptr);

    NYA_NNTensor* parameters[NYA_NN_OPTIMIZER_MAX_PARAMETERS];
    u32           count = nya_nn_sequential_parameters(sequential, parameters, NYA_NN_OPTIMIZER_MAX_PARAMETERS);

    for (u32 i = 0; i < count; i++) nya_nn_optimizer_add(optimizer, parameters[i]);
}

void nya_nn_optimizer_zero_grad(NYA_NNOptimizer* optimizer) {
    nya_assert(optimizer != nullptr);

    for (u32 i = 0; i < optimizer->slot_count; i++) nya_nn_tensor_zero_grad(optimizer->slots[i].parameter);
}

void nya_nn_optimizer_step(NYA_NNOptimizer* optimizer) {
    nya_assert(optimizer != nullptr);

    optimizer->step_count++;

    f32 learning_rate = optimizer->config.learning_rate;

    // Adam's bias correction, computed once per step rather than per element.
    f32 corrected_rate = learning_rate;
    if (optimizer->kind == NYA_NN_OPTIMIZER_ADAM) {
        f64 bias1 = 1.0 - pow((f64)optimizer->config.beta1, (f64)optimizer->step_count);
        f64 bias2 = 1.0 - pow((f64)optimizer->config.beta2, (f64)optimizer->step_count);

        corrected_rate = (f32)((f64)learning_rate * sqrt(bias2) / bias1);
    }

    // Kind and both gradient modifiers are the same for every element, so they are read once here and the inner loop, split by kind, carries no
    // per-element branch on them: a straight-line body over contiguous buffers, which the vectoriser can take. Decay is the classic L2 form (not
    // AdamW), added onto the gradient; the clip is elementwise, not by global norm.
    f32 weight_decay  = optimizer->config.weight_decay;
    f32 gradient_clip = optimizer->config.gradient_clip;
    b8  decays        = weight_decay > 0.0F;
    b8  clips         = gradient_clip > 0.0F;

    for (u32 i = 0; i < optimizer->slot_count; i++) {
        _NYA_NNOptimizerSlot* slot      = &optimizer->slots[i];
        NYA_NNTensor*         parameter = slot->parameter;

        f32* restrict data           = parameter->data;
        const f32* restrict gradient = parameter->grad;
        u32 count                    = parameter->count;

        if (optimizer->kind == NYA_NN_OPTIMIZER_SGD) {
            // With momentum zero this reduces to plain descent, so there is no second path to keep in step with this one.
            f32 momentum          = optimizer->config.momentum;
            f32* restrict moment1 = slot->moment1;

            for (u32 j = 0; j < count; j++) {
                f32 g = gradient[j];
                if (decays) g += weight_decay * data[j];
                if (clips) g = nya_clamp(g, -gradient_clip, gradient_clip);

                moment1[j]  = (momentum * moment1[j]) + g;
                data[j]    -= learning_rate * moment1[j];
            }
        } else if (optimizer->kind == NYA_NN_OPTIMIZER_ADAM) {
            f32 beta1             = optimizer->config.beta1;
            f32 beta2             = optimizer->config.beta2;
            f32 epsilon           = optimizer->config.epsilon;
            f32* restrict moment1 = slot->moment1;
            f32* restrict moment2 = slot->moment2;

            for (u32 j = 0; j < count; j++) {
                f32 g = gradient[j];
                if (decays) g += weight_decay * data[j];
                if (clips) g = nya_clamp(g, -gradient_clip, gradient_clip);

                moment1[j] = (beta1 * moment1[j]) + ((1.0F - beta1) * g);
                moment2[j] = (beta2 * moment2[j]) + ((1.0F - beta2) * g * g);

                // The sqrt of the second moment estimates each parameter's recent gradient size; dividing by it makes the step scale-independent.
                data[j] -= corrected_rate * moment1[j] / (sqrtf(moment2[j]) + epsilon);
            }
        }
    }
}

void nya_nn_optimizer_set_learning_rate(NYA_NNOptimizer* optimizer, f32 learning_rate) {
    nya_assert(optimizer != nullptr);

    optimizer->config.learning_rate = learning_rate;
}

f32 nya_nn_optimizer_get_learning_rate(const NYA_NNOptimizer* optimizer) {
    nya_assert(optimizer != nullptr);

    return optimizer->config.learning_rate;
}

// PRIVATE API IMPLEMENTATION

NYA_NNOptimizer* _nya_nn_optimizer_create(NYA_Arena* arena, NYA_NNOptimizerKind kind, NYA_NNOptimizerConfig config) {
    nya_assert(arena != nullptr);

    // Defaults applied here rather than at every read, so the stored config is the config in force and a caller sees what is happening.
    if (config.learning_rate <= 0.0F) config.learning_rate = 1e-3F;
    if (config.beta1 <= 0.0F) config.beta1 = 0.9F;
    if (config.beta2 <= 0.0F) config.beta2 = 0.999F;
    if (config.epsilon <= 0.0F) config.epsilon = 1e-8F;

    NYA_NNOptimizer* optimizer = nya_arena_alloc(arena, sizeof(NYA_NNOptimizer));
    *optimizer                 = (NYA_NNOptimizer){ .kind = kind, .config = config, .allocator = arena };

    return optimizer;
}
