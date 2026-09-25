/**
 * The neural-net training hot paths: the optimizer step, once per batch over every parameter.
 **/

#include "nyangine-core/nyangine.c"
#include "nyangine-core/nyangine.h"

#define ROWS    256
#define COLUMNS 256
#define COUNT   (ROWS * COLUMNS)

static u32 lcg = 0x1234567u;

static f32 next_f32(void) {
    lcg = (u32)((((u64)lcg * 1664525ull) + 1013904223ull) & 0xFFFFFFFFull);
    return ((f32)(lcg >> 8) / 16777216.0F) * 2.0F - 1.0F;
}

s32 main(void) {
    NYA_Arena* arena = nya_arena_create(.name = "bench_nn");

    // One fat weight matrix stands in for a layer's parameters; the optimizer walks it element by element every step.
    NYA_NNTensor* param_sgd  = nya_nn_tensor_create(arena, NYA_NN_SHAPE(ROWS, COLUMNS), true);
    NYA_NNTensor* param_adam = nya_nn_tensor_create(arena, NYA_NN_SHAPE(ROWS, COLUMNS), true);
    NYA_NNTensor* param_clip = nya_nn_tensor_create(arena, NYA_NN_SHAPE(ROWS, COLUMNS), true);

    for (u32 i = 0; i < param_sgd->count; i++) {
        // Held away from zero so Adam's second moment never collapses into the denormal range and skews the timing.
        f32 g                = next_f32();
        g                   += g >= 0.0F ? 0.5F : -0.5F;
        f32 w                = next_f32();
        param_sgd->data[i]   = w;
        param_sgd->grad[i]   = g;
        param_adam->data[i]  = w;
        param_adam->grad[i]  = g;
        param_clip->data[i]  = w;
        param_clip->grad[i]  = g;
    }

    NYA_NNOptimizer* sgd  = nya_nn_optimizer_sgd(arena, (NYA_NNOptimizerConfig){ .learning_rate = 1e-3F, .momentum = 0.9F });
    NYA_NNOptimizer* adam = nya_nn_optimizer_adam(arena, (NYA_NNOptimizerConfig){ .learning_rate = 1e-3F });
    NYA_NNOptimizer* clip =
        nya_nn_optimizer_adam(arena, (NYA_NNOptimizerConfig){ .learning_rate = 1e-3F, .gradient_clip = 1.0F, .weight_decay = 1e-4F });

    nya_nn_optimizer_add(sgd, param_sgd);
    nya_nn_optimizer_add(adam, param_adam);
    nya_nn_optimizer_add(clip, param_clip);

    nya_bench_begin("optimizer step (65536 parameters per iteration)");

    nya_bench("sgd + momentum", COUNT, {
        nya_nn_optimizer_step(sgd);
        nya_bench_keep(param_sgd->data[0]);
    });

    nya_bench("adam", COUNT, {
        nya_nn_optimizer_step(adam);
        nya_bench_keep(param_adam->data[0]);
    });

    // The same Adam step with decay and clipping on: the branches the hot loop guards on loop-invariant flags.
    nya_bench("adam + clip + decay", COUNT, {
        nya_nn_optimizer_step(clip);
        nya_bench_keep(param_clip->data[0]);
    });

    return nya_bench_end();
}
