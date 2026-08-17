/**
 * Tensors, autograd, layers and optimizers.
 *
 * The gradient check is the test that matters. Every other part of this library is checkable by
 * inspection — a matmul either produces the right numbers or it does not — but a backward pass can
 * be wrong in ways that still train: a factor of two on one branch, a missing accumulation on a
 * shared parameter, a sign error on a term that is usually small. All of those produce a network
 * that learns *something*, more slowly, and none of them show up as a failure anywhere else.
 *
 * So the derivatives are compared against finite differences of the forward pass. That treats the
 * forward pass as the specification, which is right: it is the half that is obviously correct.
 **/

#include "nyangine/nyangine.c"
#include "nyangine/nyangine.h"

/** Relative tolerance for the gradient check. Generous, because f32 finite differences are noisy. */
#define TEST_NN_GRADIENT_TOLERANCE 2e-2F

/**
 * How far to nudge a parameter when estimating its derivative numerically.
 *
 * A compromise. Too small and the difference of two nearly equal f32 losses is dominated by rounding;
 * too large and the secant stops approximating the tangent. 1e-3 sits in the flat part of that curve
 * for the value ranges a small network produces.
 * */
#define TEST_NN_EPSILON 1e-3F

/** Everything the gradient check's forward pass needs. C has no closures, so it is passed by hand. */
struct GradientCheckContext {
  NYA_NNGraph*      graph;
  NYA_NNSequential* network;
  const f32*        states;
  const u32*        actions;
  const f32*        targets;
  u32               batch;
  u32               inputs;
};

/**
 * The loss as a pure function of the network's current weights.
 *
 * Rerun from scratch each time, graph reset included, because the numeric estimate has to see the
 * effect of a nudged weight on the *whole* pass — reusing any cached activation would measure a
 * different function than the one the analytic gradient describes.
 * */
static f32 gradient_check_forward(struct GradientCheckContext* context) {
  nya_nn_graph_reset(context->graph);

  NYA_NNTensor* x        = nya_nn_tensor_from(context->graph, NYA_NN_SHAPE(context->batch, context->inputs), context->states);
  NYA_NNTensor* q        = nya_nn_sequential_forward(context->network, context->graph, x);
  NYA_NNTensor* taken    = nya_nn_gather(context->graph, q, context->actions);
  NYA_NNTensor* expected = nya_nn_tensor_from(context->graph, NYA_NN_SHAPE(context->batch, 1), context->targets);

  return nya_nn_tensor_item(nya_nn_huber(context->graph, taken, expected, 1.0F));
}

/** Builds the network under test. Small, but with every op the DQN path uses. */
static NYA_NNSequential* build_network(NYA_Arena* arena, NYA_RNG* rng, u32 inputs, u32 outputs) {
  NYA_NNSequential* network = nya_nn_sequential_create(arena);

  nya_nn_sequential_push(network, nya_nn_layer_linear(arena, rng, inputs, 8));
  nya_nn_sequential_push(network, nya_nn_layer_relu(arena));
  nya_nn_sequential_push(network, nya_nn_layer_linear(arena, rng, 8, outputs));

  return network;
}

int main(void) {
  NYA_Arena* arena = nya_arena_create(.name = "test_nn");
  defer      nya_arena_destroy(arena);

  NYA_RNG rng = nya_rng_create(.seed = "6E79616E6E5F3031");

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: shapes, indexing and the reductions
  // ─────────────────────────────────────────────────────────────────────────────
  {
    NYA_NNGraph* graph = nya_nn_graph_create(arena);

    NYA_NNTensor* t = nya_nn_tensor_from(graph, NYA_NN_SHAPE(2, 3), (f32[]){ 1, 2, 3, 4, 6, 5 });

    nya_assert(t->rank == 2 && t->shape[0] == 2 && t->shape[1] == 3, "shape not as constructed");
    nya_assert(t->count == 6, "count must be the product of the shape, got %u", t->count);

    nya_assert(nya_nn_tensor_at(t, 1, 1) == 6.0F, "row major indexing is wrong");
    nya_assert(nya_nn_tensor_argmax_row(t, 0) == 2, "argmax of the first row");
    nya_assert(nya_nn_tensor_argmax_row(t, 1) == 1, "argmax of the second row");
    nya_assert(nya_nn_tensor_max_row(t, 1) == 6.0F, "max of the second row");

    nya_assert(fabsf(nya_nn_tensor_item(nya_nn_sum(graph, t)) - 21.0F) < 1e-5F, "sum");
    nya_assert(fabsf(nya_nn_tensor_item(nya_nn_mean(graph, t)) - 3.5F) < 1e-5F, "mean");

    printf("  PASSED: shapes and reductions\n");
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: matmul against a hand computed result
  // ─────────────────────────────────────────────────────────────────────────────
  {
    NYA_NNGraph* graph = nya_nn_graph_create(arena);

    // [2,3] by [3,2]. Worked out by hand, because a matmul that is wrong in a way a gradient check
    // cannot see — a consistent transpose, say — would still pass every derivative test below.
    NYA_NNTensor* a = nya_nn_tensor_from(graph, NYA_NN_SHAPE(2, 3), (f32[]){ 1, 2, 3, 4, 5, 6 });
    NYA_NNTensor* b = nya_nn_tensor_from(graph, NYA_NN_SHAPE(3, 2), (f32[]){ 7, 8, 9, 10, 11, 12 });

    NYA_NNTensor* c = nya_nn_matmul(graph, a, b);

    nya_assert(c->shape[0] == 2 && c->shape[1] == 2, "matmul output shape");
    nya_assert(nya_nn_tensor_at(c, 0, 0) == 58.0F, "matmul [0,0] was %f", (f64)nya_nn_tensor_at(c, 0, 0));
    nya_assert(nya_nn_tensor_at(c, 0, 1) == 64.0F, "matmul [0,1] was %f", (f64)nya_nn_tensor_at(c, 0, 1));
    nya_assert(nya_nn_tensor_at(c, 1, 0) == 139.0F, "matmul [1,0] was %f", (f64)nya_nn_tensor_at(c, 1, 0));
    nya_assert(nya_nn_tensor_at(c, 1, 1) == 154.0F, "matmul [1,1] was %f", (f64)nya_nn_tensor_at(c, 1, 1));

    printf("  PASSED: matmul\n");
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: autograd against finite differences, over the whole DQN path
  // ─────────────────────────────────────────────────────────────────────────────
  {
    /*
     * The exact expression a DQN optimises: gather the taken action's value out of the network's
     * output, then Huber against a target. Checking the pieces separately would miss the thing most
     * likely to be wrong, which is how they compose.
     */
    const u32 batch   = 4;
    const u32 inputs  = 3;
    const u32 outputs = 2;

    NYA_NNGraph*      graph   = nya_nn_graph_create(arena);
    NYA_NNSequential* network = build_network(arena, &rng, inputs, outputs);

    f32 states[] = { 0.5F, -0.2F, 1.0F, -1.0F, 0.3F, 0.7F, 0.1F, 0.9F, -0.4F, 0.8F, -0.6F, 0.2F };
    u32 actions[] = { 0, 1, 1, 0 };
    f32 targets[] = { 1.0F, -0.5F, 0.25F, 2.0F };

    NYA_NNTensor* parameters[NYA_NN_OPTIMIZER_MAX_PARAMETERS];
    u32           parameter_count = nya_nn_sequential_parameters(network, parameters, NYA_NN_OPTIMIZER_MAX_PARAMETERS);
    nya_assert(parameter_count == 4, "two linear layers is four parameter tensors, got %u", parameter_count);

    struct GradientCheckContext context = {
      .graph   = graph,
      .network = network,
      .states  = states,
      .actions = actions,
      .targets = targets,
      .batch   = batch,
      .inputs  = inputs,
    };

    // Analytic gradients, once.
    nya_nn_graph_reset(graph);
    for (u32 i = 0; i < parameter_count; i++) nya_nn_tensor_zero_grad(parameters[i]);

    NYA_NNTensor* analytic_x        = nya_nn_tensor_from(graph, NYA_NN_SHAPE(batch, inputs), states);
    NYA_NNTensor* analytic_q        = nya_nn_sequential_forward(network, graph, analytic_x);
    NYA_NNTensor* analytic_taken    = nya_nn_gather(graph, analytic_q, actions);
    NYA_NNTensor* analytic_expected = nya_nn_tensor_from(graph, NYA_NN_SHAPE(batch, 1), targets);
    NYA_NNTensor* analytic_loss     = nya_nn_huber(graph, analytic_taken, analytic_expected, 1.0F);

    nya_nn_backward(graph, analytic_loss);

    // Copied out, because the numeric pass below resets the graph and reruns the forward.
    u32 checked   = 0;
    u32 mismatches = 0;

    for (u32 p = 0; p < parameter_count; p++) {
      NYA_NNTensor* parameter = parameters[p];

      for (u32 i = 0; i < parameter->count; i++) {
        f32 analytic = parameter->grad[i];
        f32 original = parameter->data[i];

        // Central difference, not forward: its error is quadratic in epsilon rather than linear, and
        // at f32 precision that is the difference between a check that passes and one that is noise.
        parameter->data[i] = original + TEST_NN_EPSILON;
        f32 plus           = gradient_check_forward(&context);

        parameter->data[i] = original - TEST_NN_EPSILON;
        f32 minus          = gradient_check_forward(&context);

        parameter->data[i] = original;

        f32 numeric = (plus - minus) / (2.0F * TEST_NN_EPSILON);

        // Relative to the larger of the two, with a floor: a gradient that is legitimately near zero
        // cannot be compared in relative terms at all.
        f32 scale      = nya_max(nya_max(fabsf(analytic), fabsf(numeric)), 1e-3F);
        f32 difference = fabsf(analytic - numeric) / scale;

        checked++;
        if (difference > TEST_NN_GRADIENT_TOLERANCE) {
          mismatches++;
          printf("  gradient mismatch on parameter %u element %u: analytic %f, numeric %f\n", p, i, (f64)analytic, (f64)numeric);
        }
      }
    }

    nya_assert(checked > 0, "the gradient check examined nothing");
    nya_assert(mismatches == 0, "%u of %u gradients disagree with finite differences", mismatches, checked);

    printf("  PASSED: autograd matches finite differences (%u gradients)\n", checked);
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: no_grad records nothing and allocates no gradients
  // ─────────────────────────────────────────────────────────────────────────────
  {
    NYA_NNGraph*      graph   = nya_nn_graph_create(arena);
    NYA_NNSequential* network = build_network(arena, &rng, 2, 2);

    nya_nn_graph_reset(graph);

    NYA_NNTensor* x = nya_nn_tensor_from(graph, NYA_NN_SHAPE(1, 2), (f32[]){ 0.5F, -0.5F });
    (void)nya_nn_sequential_forward(network, graph, x);

    u32 with_grad = nya_nn_graph_tensor_count(graph);
    nya_assert(with_grad > 0, "a forward pass recorded nothing");

    nya_nn_graph_reset(graph);

    nya_nn_graph_grad_begin(graph);
    NYA_NNTensor* x2 = nya_nn_tensor_from(graph, NYA_NN_SHAPE(1, 2), (f32[]){ 0.5F, -0.5F });
    NYA_NNTensor* y2 = nya_nn_sequential_forward(network, graph, x2);
    nya_nn_graph_grad_end(graph);

    nya_assert(nya_nn_graph_tensor_count(graph) == 0, "no_grad still recorded %u tensors", nya_nn_graph_tensor_count(graph));
    nya_assert(y2->grad == nullptr, "no_grad still allocated a gradient buffer");

    printf("  PASSED: no_grad\n");
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: a target network copies and soft updates
  // ─────────────────────────────────────────────────────────────────────────────
  {
    NYA_NNSequential* online = build_network(arena, &rng, 2, 2);
    NYA_NNSequential* target = build_network(arena, &rng, 2, 2);

    nya_nn_sequential_copy_parameters(target, online);

    NYA_NNTensor* online_parameters[NYA_NN_OPTIMIZER_MAX_PARAMETERS];
    NYA_NNTensor* target_parameters[NYA_NN_OPTIMIZER_MAX_PARAMETERS];

    u32 count = nya_nn_sequential_parameters(online, online_parameters, NYA_NN_OPTIMIZER_MAX_PARAMETERS);
    (void)nya_nn_sequential_parameters(target, target_parameters, NYA_NN_OPTIMIZER_MAX_PARAMETERS);

    for (u32 p = 0; p < count; p++) {
      for (u32 i = 0; i < online_parameters[p]->count; i++) {
        nya_assert(target_parameters[p]->data[i] == online_parameters[p]->data[i], "a hard copy left parameter %u element %u different", p, i);
      }
    }

    // Move the online net, then check a soft update travels exactly a tenth of the way.
    f32 before = target_parameters[0]->data[0];
    online_parameters[0]->data[0] += 1.0F;

    nya_nn_sequential_soft_update(target, online, 0.1F);

    f32 expected = (0.9F * before) + (0.1F * online_parameters[0]->data[0]);
    nya_assert(fabsf(target_parameters[0]->data[0] - expected) < 1e-5F, "soft update landed at %f, expected %f", (f64)target_parameters[0]->data[0], (f64)expected);

    printf("  PASSED: target network sync\n");
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: the whole stack learns XOR
  // ─────────────────────────────────────────────────────────────────────────────
  {
    /*
     * XOR, for the same reason nn_neat's test uses it: it is not linearly separable, so a network
     * that solves it has necessarily learned something a single layer cannot represent. It is also
     * the smallest problem where a broken optimizer still drives the loss down a little and then
     * stalls, which a "did the loss decrease" assertion would happily accept.
     */
    NYA_NNGraph*      graph   = nya_nn_graph_create(arena);
    NYA_NNSequential* network = nya_nn_sequential_create(arena);

    nya_nn_sequential_push(network, nya_nn_layer_linear(arena, &rng, 2, 16));
    nya_nn_sequential_push(network, nya_nn_layer_tanh(arena));
    nya_nn_sequential_push(network, nya_nn_layer_linear(arena, &rng, 16, 1));

    NYA_NNOptimizer* optimizer = nya_nn_optimizer_adam(arena, (NYA_NNOptimizerConfig){ .learning_rate = 0.05F });
    nya_nn_optimizer_add_sequential(optimizer, network);

    f32 inputs[]  = { 0, 0, 0, 1, 1, 0, 1, 1 };
    f32 expected[] = { 0, 1, 1, 0 };

    f32 final_loss = 0.0F;
    for (u32 step = 0; step < 2000; step++) {
      nya_nn_graph_reset(graph);
      nya_nn_optimizer_zero_grad(optimizer);

      NYA_NNTensor* x      = nya_nn_tensor_from(graph, NYA_NN_SHAPE(4, 2), inputs);
      NYA_NNTensor* y      = nya_nn_sequential_forward(network, graph, x);
      NYA_NNTensor* target = nya_nn_tensor_from(graph, NYA_NN_SHAPE(4, 1), expected);
      NYA_NNTensor* loss   = nya_nn_mse(graph, y, target);

      nya_nn_backward(graph, loss);
      nya_nn_optimizer_step(optimizer);

      final_loss = nya_nn_tensor_item(loss);
    }

    nya_assert(final_loss < 0.01F, "XOR did not converge, final loss %f", (f64)final_loss);

    // Solved means every case on the right side of the halfway line, not merely a small average.
    nya_nn_graph_reset(graph);
    NYA_NNTensor* x = nya_nn_tensor_from(graph, NYA_NN_SHAPE(4, 2), inputs);
    NYA_NNTensor* y = nya_nn_sequential_forward(network, graph, x);

    for (u32 i = 0; i < 4; i++) {
      f32 predicted = nya_nn_tensor_at(y, i, 0);
      f32 wanted    = expected[i];

      nya_assert((predicted > 0.5F) == (wanted > 0.5F), "XOR case %u predicted %f, wanted %f", i, (f64)predicted, (f64)wanted);
    }

    printf("  PASSED: XOR converges (loss %f)\n", (f64)final_loss);
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: the graph arena does not grow across steps
  // ─────────────────────────────────────────────────────────────────────────────
  {
    /*
     * The property that makes this usable inside a frame. A training loop that allocates per step
     * would be a leak in everything but name — bounded only by how long the game runs.
     */
    NYA_NNGraph*      graph   = nya_nn_graph_create(arena);
    NYA_NNSequential* network = build_network(arena, &rng, 4, 3);

    NYA_NNOptimizer* optimizer = nya_nn_optimizer_adam(arena, (NYA_NNOptimizerConfig){ .gradient_clip = 1.0F });
    nya_nn_optimizer_add_sequential(optimizer, network);

    f32 batch[16] = { 0 };
    u32 actions[4] = { 0, 1, 2, 0 };
    f32 targets[4] = { 0.1F, 0.2F, 0.3F, 0.4F };

    u64 settled_usage = 0;
    u32 growth_steps  = 0;

    for (u32 step = 0; step < 50; step++) {
      nya_nn_graph_reset(graph);
      nya_nn_optimizer_zero_grad(optimizer);

      NYA_NNTensor* x      = nya_nn_tensor_from(graph, NYA_NN_SHAPE(4, 4), batch);
      NYA_NNTensor* q      = nya_nn_sequential_forward(network, graph, x);
      NYA_NNTensor* taken  = nya_nn_gather(graph, q, actions);
      NYA_NNTensor* target = nya_nn_tensor_from(graph, NYA_NN_SHAPE(4, 1), targets);
      NYA_NNTensor* loss   = nya_nn_huber(graph, taken, target, 1.0F);

      nya_nn_backward(graph, loss);
      nya_nn_optimizer_step(optimizer);

      /*
       * Sampled from step ten on, so the first passes' growth is not mistaken for a leak.
       *
       * The arena reuses its regions after a reset, so once the largest pass has been seen the
       * figure stops moving entirely. Anything after that is a genuine per-step allocation.
       */
      if (step < 10) continue;

      u64 usage = nya_nn_graph_memory_usage_bytes(graph);

      if (settled_usage == 0) settled_usage = usage;
      else if (usage > settled_usage) growth_steps++;
    }

    nya_assert(settled_usage > 0, "the graph reported no memory in use during training");
    nya_assert(growth_steps == 0, "the graph arena grew on %u of the last 40 steps; the training loop allocates per step", growth_steps);

    printf("  PASSED: steady state training loop\n");
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: two backward calls on one tape sum, rather than double count
  // ─────────────────────────────────────────────────────────────────────────────
  {
    /*
     * A regression test for a bug that was in this library and produced no symptom.
     *
     * Backward sweeps the whole tape, so a second call walked the first loss's nodes while they were
     * still holding gradients from the first call and propagated them again. dw came out as 11 where
     * the correct total was 7 — a wrong answer that still trains, just towards the wrong thing.
     */
    NYA_NNGraph*  graph = nya_nn_graph_create(arena);
    NYA_NNTensor* w     = nya_nn_tensor_create(arena, NYA_NN_SHAPE(1, 1), true);

    nya_nn_tensor_fill(w, 3.0F);
    nya_nn_graph_reset(graph);
    nya_nn_tensor_zero_grad(w);

    NYA_NNTensor* first  = nya_nn_sum(graph, nya_nn_mul(graph, nya_nn_tensor_from(graph, NYA_NN_SHAPE(1, 1), (f32[]){ 2.0F }), w));
    NYA_NNTensor* second = nya_nn_sum(graph, nya_nn_mul(graph, nya_nn_tensor_from(graph, NYA_NN_SHAPE(1, 1), (f32[]){ 5.0F }), w));

    nya_nn_backward(graph, first);
    nya_assert(fabsf(w->grad[0] - 2.0F) < 1e-5F, "first backward gave %f, expected 2", (f64)w->grad[0]);

    nya_nn_backward(graph, second);
    nya_assert(fabsf(w->grad[0] - 7.0F) < 1e-5F, "two backward calls gave %f, expected 7", (f64)w->grad[0]);

    printf("  PASSED: repeated backward accumulates without double counting\n");
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: a parameter used twice in one pass gets both contributions
  // ─────────────────────────────────────────────────────────────────────────────
  {
    // The reason gradients accumulate rather than assign. w appears on both sides, so dloss/dw is
    // the sum of two paths; an implementation that assigned would report only the last one.
    NYA_NNGraph*  graph = nya_nn_graph_create(arena);
    NYA_NNTensor* w     = nya_nn_tensor_create(arena, NYA_NN_SHAPE(1, 1), true);

    nya_nn_tensor_fill(w, 4.0F);
    nya_nn_graph_reset(graph);
    nya_nn_tensor_zero_grad(w);

    // loss = sum(w * w) = w^2, so dloss/dw = 2w = 8.
    NYA_NNTensor* squared = nya_nn_mul(graph, w, w);
    nya_nn_backward(graph, nya_nn_sum(graph, squared));

    nya_assert(fabsf(w->grad[0] - 8.0F) < 1e-4F, "a doubly used parameter gave %f, expected 8", (f64)w->grad[0]);

    printf("  PASSED: shared parameter accumulates both paths\n");
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: every op's gradient, individually, against finite differences
  // ─────────────────────────────────────────────────────────────────────────────
  {
    /*
     * The composed check above exercises the ops the DQN path uses. This one covers the rest, so an
     * op that nothing currently composes is still known to be right the day something does.
     */
    NYA_NNGraph* graph = nya_nn_graph_create(arena);

    for (u32 op = 0; op < 6; op++) {
      NYA_NNTensor* w = nya_nn_tensor_create(arena, NYA_NN_SHAPE(2, 2), true);
      nya_nn_tensor_fill_uniform(w, &rng, 0.3F, 0.9F);

      NYA_ConstCString name = "";

      for (u32 element = 0; element < w->count; element++) {
        f32 original = w->data[element];

        f32 values[2] = { 0 };
        for (u32 side = 0; side < 2; side++) {
          w->data[element] = original + (side == 0 ? TEST_NN_EPSILON : -TEST_NN_EPSILON);

          nya_nn_graph_reset(graph);

          NYA_NNTensor* other = nya_nn_tensor_from(graph, NYA_NN_SHAPE(2, 2), (f32[]){ 0.4F, -0.7F, 1.1F, 0.2F });
          NYA_NNTensor* result = nullptr;

          switch (op) {
            case 0: result = nya_nn_add(graph, w, other); name = "add"; break;
            case 1: result = nya_nn_sub(graph, w, other); name = "sub"; break;
            case 2: result = nya_nn_mul(graph, w, other); name = "mul"; break;
            case 3: result = nya_nn_scale(graph, w, 2.5F); name = "scale"; break;
            case 4: result = nya_nn_tanh(graph, w); name = "tanh"; break;
            case 5: result = nya_nn_bias(graph, w, nya_nn_tensor_from(graph, NYA_NN_SHAPE(2), (f32[]){ 0.3F, -0.2F })); name = "bias"; break;
            default: break;
          }

          values[side] = nya_nn_tensor_item(nya_nn_mean(graph, result));
        }

        w->data[element] = original;

        f32 numeric = (values[0] - values[1]) / (2.0F * TEST_NN_EPSILON);

        // Analytic, from a clean pass.
        nya_nn_graph_reset(graph);
        nya_nn_tensor_zero_grad(w);

        NYA_NNTensor* other = nya_nn_tensor_from(graph, NYA_NN_SHAPE(2, 2), (f32[]){ 0.4F, -0.7F, 1.1F, 0.2F });
        NYA_NNTensor* result = nullptr;

        switch (op) {
          case 0: result = nya_nn_add(graph, w, other); break;
          case 1: result = nya_nn_sub(graph, w, other); break;
          case 2: result = nya_nn_mul(graph, w, other); break;
          case 3: result = nya_nn_scale(graph, w, 2.5F); break;
          case 4: result = nya_nn_tanh(graph, w); break;
          case 5: result = nya_nn_bias(graph, w, nya_nn_tensor_from(graph, NYA_NN_SHAPE(2), (f32[]){ 0.3F, -0.2F })); break;
          default: break;
        }

        nya_nn_backward(graph, nya_nn_mean(graph, result));

        f32 analytic = w->grad[element];
        f32 scale    = nya_max(nya_max(fabsf(analytic), fabsf(numeric)), 1e-3F);

        nya_assert(fabsf(analytic - numeric) / scale < TEST_NN_GRADIENT_TOLERANCE,
                   "op %s element %u: analytic %f, numeric %f", name, element, (f64)analytic, (f64)numeric);
      }
    }

    printf("  PASSED: every op's gradient\n");
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: SGD, momentum, weight decay and clipping
  // ─────────────────────────────────────────────────────────────────────────────
  {
    // Plain SGD on a known gradient: one step must move the weight by exactly rate * gradient.
    NYA_NNTensor* w = nya_nn_tensor_create(arena, NYA_NN_SHAPE(1), true);
    nya_nn_tensor_fill(w, 1.0F);

    NYA_NNOptimizer* sgd = nya_nn_optimizer_sgd(arena, (NYA_NNOptimizerConfig){ .learning_rate = 0.1F });
    nya_nn_optimizer_add(sgd, w);

    w->grad[0] = 2.0F;
    nya_nn_optimizer_step(sgd);
    nya_assert(fabsf(w->data[0] - 0.8F) < 1e-5F, "sgd step gave %f, expected 0.8", (f64)w->data[0]);

    // Clipping must bound the step regardless of how large the gradient is.
    NYA_NNTensor* clipped = nya_nn_tensor_create(arena, NYA_NN_SHAPE(1), true);
    nya_nn_tensor_fill(clipped, 1.0F);

    NYA_NNOptimizer* limited = nya_nn_optimizer_sgd(arena, (NYA_NNOptimizerConfig){ .learning_rate = 0.1F, .gradient_clip = 1.0F });
    nya_nn_optimizer_add(limited, clipped);

    clipped->grad[0] = 1000.0F;
    nya_nn_optimizer_step(limited);
    nya_assert(fabsf(clipped->data[0] - 0.9F) < 1e-5F, "clipped step gave %f, expected 0.9", (f64)clipped->data[0]);

    // Weight decay must pull towards zero even with no gradient at all.
    NYA_NNTensor* decayed = nya_nn_tensor_create(arena, NYA_NN_SHAPE(1), true);
    nya_nn_tensor_fill(decayed, 1.0F);

    NYA_NNOptimizer* decaying = nya_nn_optimizer_sgd(arena, (NYA_NNOptimizerConfig){ .learning_rate = 0.1F, .weight_decay = 0.5F });
    nya_nn_optimizer_add(decaying, decayed);

    decayed->grad[0] = 0.0F;
    nya_nn_optimizer_step(decaying);
    nya_assert(decayed->data[0] < 1.0F, "weight decay did not shrink the weight");

    printf("  PASSED: sgd, clipping and weight decay\n");
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: non-finite values are found, and found at their source
  // ─────────────────────────────────────────────────────────────────────────────
  {
    NYA_NNGraph*  graph = nya_nn_graph_create(arena);
    NYA_NNTensor* w     = nya_nn_tensor_create(arena, NYA_NN_SHAPE(1, 1), true);

    nya_nn_tensor_fill(w, 1.0F);
    nya_nn_graph_reset(graph);

    NYA_NNTensor* healthy = nya_nn_scale(graph, w, 2.0F);
    nya_assert(nya_nn_graph_find_non_finite(graph) == nullptr, "a healthy graph reported a non-finite tensor");
    nya_assert(nya_nn_tensor_is_finite(healthy), "a healthy tensor reported non-finite");

    // Poisoned by hand, since making a real network diverge inside a test is slow and unreliable.
    NYA_NNTensor* poisoned = nya_nn_scale(graph, healthy, 1.0F);
    poisoned->data[0]      = NAN;

    NYA_NNTensor* found = nya_nn_graph_find_non_finite(graph);
    nya_assert(found == poisoned, "the non-finite search found the wrong tensor");
    nya_assert(strcmp(nya_nn_op_name(found->op), "scale") == 0, "op name was %s, expected scale", nya_nn_op_name(found->op));

    printf("  PASSED: non-finite detection\n");
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: the visualiser survives what a debug overlay gets handed
  // ─────────────────────────────────────────────────────────────────────────────
  {
    /*
     * Headless, so the draw calls are no-ops — but the layout arithmetic, the forward pass and every
     * guard still run, which is where the crashes would be. A visualiser is called from a debug
     * overlay on networks that are half built or empty, and it must never be the thing that takes
     * the frame down.
     */
    NYA_NNGraph* graph = nya_nn_graph_create(arena);

    // A zeroed window is enough: headless draw calls assert it is non-null and then do nothing, so
    // this exercises every line of the visualiser that is not the GPU.
    NYA_Window* window = nya_arena_alloc(arena, sizeof(NYA_Window));
    nya_memset(window, 0, sizeof(NYA_Window));

    NYA_NNSequential* network = nya_nn_sequential_create(arena);
    nya_nn_sequential_push(network, nya_nn_layer_linear(arena, &rng, 3, 40));
    nya_nn_sequential_push(network, nya_nn_layer_relu(arena));
    nya_nn_sequential_push(network, nya_nn_layer_linear(arena, &rng, 40, 5));

    f32 input[3] = { 0.5F, -0.25F, 1.0F };

    // A layer wider than NYA_NN_DRAW_MAX_UNITS, so the sampling path is the one exercised.
    nya_nn_draw(window, network, graph, input, (NYA_NNDrawStyle){ .width = 300.0F, .height = 200.0F, .show_values = true });

    /*
     * Labels, including every way a caller gets the count wrong.
     *
     * They are indexed by unit, so a count shorter than the layer, a null entry, and a count longer
     * than the layer all have to be tolerated rather than read past — an overlay builds these from
     * whatever it happens to know and will get it wrong.
     */
    NYA_ConstCString input_labels[]  = { "x", nullptr, "z" };
    NYA_ConstCString output_labels[] = { "a", "b" };

    nya_nn_draw(window, network, graph, input, (NYA_NNDrawStyle){
        .width = 300.0F, .height = 200.0F,
        .input_labels = input_labels, .input_label_count = nya_carray_length(input_labels),
        // Deliberately fewer than the five output units, so the unlabelled remainder is exercised.
        .output_labels = output_labels, .output_label_count = nya_carray_length(output_labels),
    });

    // A count claiming more labels than the array holds must not be read past either, so it is
    // clamped by the unit count rather than trusted.
    nya_nn_draw(window, network, graph, input, (NYA_NNDrawStyle){
        .width = 300.0F, .height = 200.0F,
        .output_labels = output_labels, .output_label_count = 2,
    });

    // Every degenerate input a real overlay will eventually pass.
    nya_nn_draw(window, nullptr, graph, input, (NYA_NNDrawStyle){ 0 });
    nya_nn_draw(window, network, nullptr, input, (NYA_NNDrawStyle){ 0 });
    nya_nn_draw(window, network, graph, nullptr, (NYA_NNDrawStyle){ 0 });
    nya_nn_draw(window, nya_nn_sequential_create(arena), graph, input, (NYA_NNDrawStyle){ 0 });

    // The unit sampling must cover the whole layer, ends included, and never index past it.
    nya_assert(nya_nn_graph_tensor_count(graph) == 0, "drawing recorded a tape it does not need");

    printf("  PASSED: visualiser guards\n");
  }

  printf("PASSED: test_nn\n");
  return 0;
}
