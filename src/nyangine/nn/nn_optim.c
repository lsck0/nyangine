#include "nyangine/nyangine.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * TYPES
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

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
     *
     * Adam's moment estimates start at zero, so early on they are biased towards it and the first
     * steps would be far too small without correcting for how few samples they average. Counting
     * from one is part of the algorithm, not bookkeeping.
     * */
    u64 step_count;
};

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API DECLARATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

NYA_INTERNAL NYA_NNOptimizer* _nya_nn_optimizer_create(NYA_Arena* arena, NYA_NNOptimizerKind kind, NYA_NNOptimizerConfig config) __attr_no_discard;

/**
 * The gradient for one element, after weight decay and clipping.
 *
 * Shared, so both optimizers see exactly the same gradient and the difference between them is only
 * what they do with it.
 * */
NYA_INTERNAL f32 _nya_nn_optimizer_gradient(const NYA_NNOptimizer* optimizer, const NYA_NNTensor* parameter, u32 index) __attr_no_discard;

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PUBLIC API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

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

    /*
     * Adam's bias correction, computed once per step rather than per element.
     *
     * The textbook form divides each moment by (1 - beta^t) separately. Folding both into the
     * learning rate is algebraically the same and takes two pow calls per step instead of two per
     * parameter element, which on a network of any size is the difference between a rounding error
     * and a measurable cost.
     */
    f32 corrected_rate = learning_rate;
    if (optimizer->kind == NYA_NN_OPTIMIZER_ADAM) {
        f64 bias1 = 1.0 - pow((f64)optimizer->config.beta1, (f64)optimizer->step_count);
        f64 bias2 = 1.0 - pow((f64)optimizer->config.beta2, (f64)optimizer->step_count);

        corrected_rate = (f32)((f64)learning_rate * sqrt(bias2) / bias1);
    }

    for (u32 i = 0; i < optimizer->slot_count; i++) {
        _NYA_NNOptimizerSlot* slot      = &optimizer->slots[i];
        NYA_NNTensor*         parameter = slot->parameter;

        for (u32 j = 0; j < parameter->count; j++) {
            f32 gradient = _nya_nn_optimizer_gradient(optimizer, parameter, j);

            switch (optimizer->kind) {
                case NYA_NN_OPTIMIZER_SGD: {
                    // With momentum zero this reduces to plain descent, so there is no second path
                    // to keep in step with this one.
                    slot->moment1[j] = (optimizer->config.momentum * slot->moment1[j]) + gradient;

                    parameter->data[j] -= learning_rate * slot->moment1[j];
                } break;

                case NYA_NN_OPTIMIZER_ADAM: {
                    f32 beta1 = optimizer->config.beta1;
                    f32 beta2 = optimizer->config.beta2;

                    slot->moment1[j] = (beta1 * slot->moment1[j]) + ((1.0F - beta1) * gradient);
                    slot->moment2[j] = (beta2 * slot->moment2[j]) + ((1.0F - beta2) * gradient * gradient);

                    // The square root of the second moment is a per-parameter estimate of the
                    // gradient's recent magnitude; dividing by it is what makes the step size
                    // independent of how large this particular parameter's gradients happen to be.
                    parameter->data[j] -= corrected_rate * slot->moment1[j] / (sqrtf(slot->moment2[j]) + optimizer->config.epsilon);
                } break;

                case NYA_NN_OPTIMIZER_KIND_COUNT:
                default:                          break;
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

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

NYA_NNOptimizer* _nya_nn_optimizer_create(NYA_Arena* arena, NYA_NNOptimizerKind kind, NYA_NNOptimizerConfig config) {
    nya_assert(arena != nullptr);

    // Defaults applied here rather than at every read, so the stored config is the config in force
    // and a caller inspecting it sees what is actually happening.
    if (config.learning_rate <= 0.0F) config.learning_rate = 1e-3F;
    if (config.beta1 <= 0.0F) config.beta1 = 0.9F;
    if (config.beta2 <= 0.0F) config.beta2 = 0.999F;
    if (config.epsilon <= 0.0F) config.epsilon = 1e-8F;

    NYA_NNOptimizer* optimizer = nya_arena_alloc(arena, sizeof(NYA_NNOptimizer));
    *optimizer                 = (NYA_NNOptimizer){ .kind = kind, .config = config, .allocator = arena };

    return optimizer;
}

f32 _nya_nn_optimizer_gradient(const NYA_NNOptimizer* optimizer, const NYA_NNTensor* parameter, u32 index) {
    f32 gradient = parameter->grad[index];

    // Decay is applied to the gradient rather than to the weight directly, which is the classic L2
    // formulation. It is not AdamW: that decouples the two precisely because this interacts with
    // Adam's per-parameter scaling, and matching the textbook is the less surprising choice here.
    if (optimizer->config.weight_decay > 0.0F) gradient += optimizer->config.weight_decay * parameter->data[index];

    // Elementwise, not by global norm. Cheaper, needs no second pass over every parameter, and for
    // bounding the occasional catastrophic update it is the part that matters.
    if (optimizer->config.gradient_clip > 0.0F) gradient = nya_clamp(gradient, -optimizer->config.gradient_clip, optimizer->config.gradient_clip);

    return gradient;
}
