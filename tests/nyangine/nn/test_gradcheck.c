/**
 * Finite-difference gradient check over every op in the autograd graph.
 *
 * For each op, builds loss = f(params), runs backward, and compares each parameter's analytic
 * gradient against (f(x+h) - f(x-h)) / 2h. A wrong backward rule shows up immediately and nothing
 * else does.
 */

#include "nyangine/nyangine.c"
#include "nyangine/nyangine.h"


/* Each case builds a scalar loss out of the two parameters it is handed. */
typedef NYA_NNTensor* (*BuildFn)(NYA_NNGraph* graph, NYA_NNTensor* a, NYA_NNTensor* b);

static NYA_NNTensor* build_add(NYA_NNGraph* g, NYA_NNTensor* a, NYA_NNTensor* b) {
  return nya_nn_sum(g, nya_nn_add(g, a, b));
}
static NYA_NNTensor* build_sub(NYA_NNGraph* g, NYA_NNTensor* a, NYA_NNTensor* b) {
  return nya_nn_sum(g, nya_nn_sub(g, a, b));
}
static NYA_NNTensor* build_mul(NYA_NNGraph* g, NYA_NNTensor* a, NYA_NNTensor* b) {
  return nya_nn_sum(g, nya_nn_mul(g, a, b));
}
static NYA_NNTensor* build_scale(NYA_NNGraph* g, NYA_NNTensor* a, NYA_NNTensor* b) {
  nya_unused(b);
  return nya_nn_sum(g, nya_nn_scale(g, a, 2.5F));
}
static NYA_NNTensor* build_relu(NYA_NNGraph* g, NYA_NNTensor* a, NYA_NNTensor* b) {
  return nya_nn_sum(g, nya_nn_relu(g, nya_nn_add(g, a, b)));
}
static NYA_NNTensor* build_tanh(NYA_NNGraph* g, NYA_NNTensor* a, NYA_NNTensor* b) {
  return nya_nn_sum(g, nya_nn_tanh(g, nya_nn_mul(g, a, b)));
}
static NYA_NNTensor* build_mean(NYA_NNGraph* g, NYA_NNTensor* a, NYA_NNTensor* b) {
  return nya_nn_mean(g, nya_nn_mul(g, a, b));
}
static NYA_NNTensor* build_mse(NYA_NNGraph* g, NYA_NNTensor* a, NYA_NNTensor* b) {
  return nya_nn_mse(g, a, b);
}
static NYA_NNTensor* build_huber_quadratic(NYA_NNGraph* g, NYA_NNTensor* a, NYA_NNTensor* b) {
  return nya_nn_huber(g, a, b, 10.0F);
}
static NYA_NNTensor* build_huber_linear(NYA_NNGraph* g, NYA_NNTensor* a, NYA_NNTensor* b) {
  return nya_nn_huber(g, a, b, 0.05F);
}
static NYA_NNTensor* build_chain(NYA_NNGraph* g, NYA_NNTensor* a, NYA_NNTensor* b) {
  // tanh(a*b) + a, summed: exercises a value feeding two consumers, which is where an accumulate
  // written as an assignment would show.
  NYA_NNTensor* product = nya_nn_mul(g, a, b);
  NYA_NNTensor* squashed = nya_nn_tanh(g, product);
  return nya_nn_sum(g, nya_nn_add(g, squashed, a));
}

/* 2x3 · 3x2 -> 2x2, then summed. `a` is the 2x3, `b` the 3x2. */
static NYA_NNTensor* build_matmul(NYA_NNGraph* g, NYA_NNTensor* a, NYA_NNTensor* b) {
  return nya_nn_sum(g, nya_nn_matmul(g, a, b));
}

/* 2x3 with a 3-wide bias. */
static NYA_NNTensor* build_bias(NYA_NNGraph* g, NYA_NNTensor* a, NYA_NNTensor* b) {
  return nya_nn_sum(g, nya_nn_tanh(g, nya_nn_bias(g, a, b)));
}

static const u32 GATHER_INDICES[] = { 2, 0 };

/* 2x3, one column picked per row. */
static NYA_NNTensor* build_gather(NYA_NNGraph* g, NYA_NNTensor* a, NYA_NNTensor* b) {
  nya_unused(b);
  return nya_nn_sum(g, nya_nn_gather(g, a, GATHER_INDICES));
}

/** Runs the forward pass alone and returns the loss value, leaving no gradient behind. */
static f32 evaluate(NYA_NNGraph* graph, BuildFn build, NYA_NNTensor* a, NYA_NNTensor* b) {
  nya_nn_graph_reset(graph);
  return nya_nn_tensor_item(build(graph, a, b));
}

static void gradcheck(
  NYA_ConstCString name, NYA_Arena* arena, NYA_NNGraph* graph, BuildFn build, NYA_NNShape shape_a, NYA_NNShape shape_b, f32 tolerance
) {
  // Fixed seed so a failure reproduces. The seed grammar is uppercase hex only.
  NYA_RNG rng = nya_rng_create_with_options((NYA_RNGOptions){ .seed = "C0FFEE" });

  NYA_NNTensor* a = nya_nn_tensor_create(arena, shape_a, true);
  NYA_NNTensor* b = nya_nn_tensor_create(arena, shape_b, true);

  // Away from zero, so ReLU and Huber are not evaluated exactly on their kinks where the analytic
  // and numeric derivatives legitimately disagree.
  nya_nn_tensor_fill_uniform(a, &rng, 0.3F, 1.2F);
  nya_nn_tensor_fill_uniform(b, &rng, 0.3F, 1.2F);

  nya_nn_tensor_zero_grad(a);
  nya_nn_tensor_zero_grad(b);

  nya_nn_graph_reset(graph);
  NYA_NNTensor* loss = build(graph, a, b);
  nya_nn_backward(graph, loss);

  NYA_NNTensor* parameters[] = { a, b };
  const char*   labels[]     = { "a", "b" };

  for (u64 p = 0; p < nya_carray_length(parameters); p++) {
    NYA_NNTensor* parameter = parameters[p];

    for (u32 i = 0; i < parameter->count; i++) {
      f32 original = parameter->data[i];
      f32 step     = 1e-2F;

      parameter->data[i] = original + step;
      f32 up             = evaluate(graph, build, a, b);

      parameter->data[i] = original - step;
      f32 down           = evaluate(graph, build, a, b);

      parameter->data[i] = original;

      f32 numeric  = (up - down) / (2.0F * step);
      f32 analytic = parameter->grad[i];

      f32 scale    = fmaxf(fmaxf(fabsf(numeric), fabsf(analytic)), 1.0F);
      f32 relative = fabsf(numeric - analytic) / scale;

      nya_check(
        relative <= tolerance,
        "%s: d/d%s[%u] analytic %.6f, numeric %.6f (rel %.4f)",
        name,
        labels[p],
        i,
        (f64)analytic,
        (f64)numeric,
        (f64)relative
      );
    }
  }
}

s32 main(void) {
  setvbuf(stdout, nullptr, _IONBF, 0);

  NYA_Arena*   arena = nya_arena_create(.name = "test_gradcheck");
  NYA_NNGraph* graph = nya_nn_graph_create(arena);

  NYA_NNShape vector = NYA_NN_SHAPE(6);
  NYA_NNShape rows   = NYA_NN_SHAPE(2, 3);

  printf("TEST: elementwise ops\n");
  gradcheck("add", arena, graph, build_add, vector, vector, 1e-2F);
  gradcheck("sub", arena, graph, build_sub, vector, vector, 1e-2F);
  gradcheck("mul", arena, graph, build_mul, vector, vector, 1e-2F);
  gradcheck("scale", arena, graph, build_scale, vector, vector, 1e-2F);
  printf("  done\n");

  printf("TEST: activations\n");
  gradcheck("relu", arena, graph, build_relu, vector, vector, 1e-2F);
  gradcheck("tanh", arena, graph, build_tanh, vector, vector, 2e-2F);
  printf("  done\n");

  printf("TEST: reductions and losses\n");
  gradcheck("mean", arena, graph, build_mean, vector, vector, 1e-2F);
  gradcheck("mse", arena, graph, build_mse, vector, vector, 1e-2F);
  gradcheck("huber-quadratic", arena, graph, build_huber_quadratic, vector, vector, 1e-2F);
  gradcheck("huber-linear", arena, graph, build_huber_linear, vector, vector, 1e-2F);
  printf("  done\n");

  printf("TEST: matmul, bias, gather\n");
  gradcheck("matmul", arena, graph, build_matmul, rows, NYA_NN_SHAPE(3, 2), 1e-2F);
  gradcheck("bias", arena, graph, build_bias, rows, NYA_NN_SHAPE(3), 2e-2F);
  gradcheck("gather", arena, graph, build_gather, rows, rows, 1e-2F);
  printf("  done\n");

  printf("TEST: a value with two consumers\n");
  gradcheck("chain", arena, graph, build_chain, vector, vector, 2e-2F);
  printf("  done\n");

  nya_arena_destroy(arena);

  printf("%s: test_gradcheck (" FMTu32 " failures)\n", nya_check_failures() == 0 ? "PASSED" : "FAILED", nya_check_failures());
  return nya_check_failures() == 0 ? 0 : 1;
}
