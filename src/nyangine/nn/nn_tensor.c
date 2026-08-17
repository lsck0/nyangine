#include "nyangine/nyangine.h"

#include "nyangine/nn/nn_simd.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * TYPES
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

struct NYA_NNGraph {
    /**
     * Where activations live. Reset wholesale, never freed piecewise.
     *
     * Its own arena rather than the caller's, because the whole point is that one call throws away
     * everything a forward pass produced. Sharing the caller's arena would mean a reset also threw
     * away the parameters.
     * */
    NYA_Arena* allocator;

    /** The tape: every recorded tensor, in the order it was produced. */
    NYA_NNTensor* tape[NYA_NN_GRAPH_MAX_TENSORS];
    u32           tape_count;

    /** Nesting depth of nya_nn_graph_grad_begin. Non-zero means ops do not record. */
    u32 no_grad_depth;
};

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API DECLARATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/** Allocates a tensor and its buffers from `arena`. The one place a tensor comes into existence. */
NYA_INTERNAL NYA_NNTensor* _nya_nn_tensor_alloc(NYA_Arena* arena, NYA_NNShape shape, b8 requires_grad) __attr_no_discard;

/**
 * Allocates an op's result against the graph and records it.
 *
 * Requires a gradient exactly when an input does, which is what stops a network run under
 * nya_nn_graph_grad_begin — or one whose inputs are all constants — from allocating gradient buffers
 * nothing will ever read.
 * */
NYA_INTERNAL NYA_NNTensor* _nya_nn_op(NYA_NNGraph* graph, NYA_NNShape shape, NYA_NNOp op, NYA_NNTensor* a, NYA_NNTensor* b) __attr_no_discard;

/** The shape of a tensor, as a shape literal. */
NYA_INTERNAL NYA_NNShape _nya_nn_shape_of(const NYA_NNTensor* tensor) __attr_no_discard;

/** Whether two tensors agree on rank and every dimension. */
NYA_INTERNAL b8 _nya_nn_shape_equals(const NYA_NNTensor* a, const NYA_NNTensor* b) __attr_no_discard;

/** Propagates one tape node's gradient into its inputs. */
NYA_INTERNAL void _nya_nn_backward_node(NYA_NNTensor* tensor);

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PUBLIC API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

NYA_NNGraph* nya_nn_graph_create(NYA_Arena* arena) {
    nya_assert(arena != nullptr);

    NYA_NNGraph* graph = nya_arena_alloc(arena, sizeof(NYA_NNGraph));
    *graph             = (NYA_NNGraph){ 0 };

    // Sized for activations, which are small and short lived: a batch of a few hundred rows through
    // a few layers. It grows if a network needs more, in steps of this rather than of the default.
    graph->allocator = nya_arena_create(.name = "nn_graph", .region_size = nya_mebyte_to_byte(4UL));

    return graph;
}

void nya_nn_graph_reset(NYA_NNGraph* graph) {
    nya_assert(graph != nullptr);

    nya_arena_free_all(graph->allocator);

    graph->tape_count = 0;
}

u32 nya_nn_graph_tensor_count(const NYA_NNGraph* graph) {
    nya_assert(graph != nullptr);

    return graph->tape_count;
}

u64 nya_nn_graph_memory_usage_bytes(const NYA_NNGraph* graph) {
    nya_assert(graph != nullptr);

    return nya_arena_memory_usage_bytes(graph->allocator);
}

void nya_nn_graph_grad_begin(NYA_NNGraph* graph) {
    nya_assert(graph != nullptr);

    graph->no_grad_depth++;
}

void nya_nn_graph_grad_end(NYA_NNGraph* graph) {
    nya_assert(graph != nullptr);
    nya_assert(graph->no_grad_depth > 0, "nya_nn_graph_grad_end without a matching begin");

    graph->no_grad_depth--;
}

NYA_NNTensor* nya_nn_tensor_create(NYA_Arena* arena, NYA_NNShape shape, b8 requires_grad) {
    nya_assert(arena != nullptr);

    return _nya_nn_tensor_alloc(arena, shape, requires_grad);
}

NYA_NNTensor* nya_nn_tensor_zeros(NYA_NNGraph* graph, NYA_NNShape shape) {
    nya_assert(graph != nullptr);

    // No gradient: an input or a target is data, and nothing wants dloss/dinput. A parameter is made
    // with nya_nn_tensor_create against a persistent arena instead.
    return _nya_nn_tensor_alloc(graph->allocator, shape, false);
}

NYA_NNTensor* nya_nn_tensor_from(NYA_NNGraph* graph, NYA_NNShape shape, const f32* values) {
    nya_assert(graph != nullptr);
    nya_assert(values != nullptr);

    NYA_NNTensor* tensor = nya_nn_tensor_zeros(graph, shape);
    nya_memcpy(tensor->data, values, (u64)tensor->count * sizeof(f32));

    return tensor;
}

void nya_nn_tensor_fill(NYA_NNTensor* tensor, f32 value) {
    nya_assert(tensor != nullptr);

    for (u32 i = 0; i < tensor->count; i++) tensor->data[i] = value;
}

void nya_nn_tensor_fill_uniform(NYA_NNTensor* tensor, NYA_RNG* rng, f32 min, f32 max) {
    nya_assert(tensor != nullptr);
    nya_assert(rng != nullptr);

    for (u32 i = 0; i < tensor->count; i++) {
        tensor->data[i] = nya_rng_sample_f32(rng, (NYA_RNGDistribution){ .type = NYA_RNG_DISTRIBUTION_UNIFORM, .uniform = { .min = min, .max = max } });
    }
}

void nya_nn_tensor_fill_kaiming(NYA_NNTensor* tensor, NYA_RNG* rng, u32 fan_in) {
    nya_assert(tensor != nullptr);

    // A zero fan would divide by zero, and a layer with no inputs is a caller bug rather than
    // something to silently produce nonsense for.
    if (fan_in == 0) fan_in = 1;

    f32 bound = sqrtf(6.0F / (f32)fan_in);

    nya_nn_tensor_fill_uniform(tensor, rng, -bound, bound);
}

void nya_nn_tensor_zero_grad(NYA_NNTensor* tensor) {
    nya_assert(tensor != nullptr);

    if (tensor->grad == nullptr) return;

    nya_memset(tensor->grad, 0, (u64)tensor->count * sizeof(f32));
}

void nya_nn_tensor_copy(NYA_NNTensor* destination, const NYA_NNTensor* source) {
    nya_assert(destination != nullptr);
    nya_assert(source != nullptr);
    nya_assert(destination->count == source->count, "nya_nn_tensor_copy between different sizes");

    nya_memcpy(destination->data, source->data, (u64)source->count * sizeof(f32));
}

void nya_nn_tensor_lerp_(NYA_NNTensor* destination, const NYA_NNTensor* source, f32 tau) {
    nya_assert(destination != nullptr);
    nya_assert(source != nullptr);
    nya_assert(destination->count == source->count, "nya_nn_tensor_lerp_ between different sizes");

    for (u32 i = 0; i < destination->count; i++) {
        destination->data[i] = ((1.0F - tau) * destination->data[i]) + (tau * source->data[i]);
    }
}

f32 nya_nn_tensor_at(const NYA_NNTensor* tensor, u32 row, u32 column) {
    nya_assert(tensor != nullptr);
    nya_assert(tensor->rank == 2, "nya_nn_tensor_at on a rank %u tensor", tensor->rank);
    nya_assert(row < tensor->shape[0] && column < tensor->shape[1], "nya_nn_tensor_at out of range");

    return tensor->data[(row * tensor->shape[1]) + column];
}

f32 nya_nn_tensor_item(const NYA_NNTensor* tensor) {
    nya_assert(tensor != nullptr);
    nya_assert(tensor->count == 1, "nya_nn_tensor_item on a tensor of %u elements", tensor->count);

    return tensor->data[0];
}

u32 nya_nn_tensor_argmax_row(const NYA_NNTensor* tensor, u32 row) {
    nya_assert(tensor != nullptr);
    nya_assert(tensor->rank == 2 && row < tensor->shape[0]);

    const f32* values = &tensor->data[row * tensor->shape[1]];

    u32 best = 0;
    for (u32 i = 1; i < tensor->shape[1]; i++) {
        if (values[i] > values[best]) best = i;
    }

    return best;
}

f32 nya_nn_tensor_max_row(const NYA_NNTensor* tensor, u32 row) {
    return nya_nn_tensor_at(tensor, row, nya_nn_tensor_argmax_row(tensor, row));
}

/*
 * ── Ops ──
 */

NYA_NNTensor* nya_nn_add(NYA_NNGraph* graph, NYA_NNTensor* a, NYA_NNTensor* b) {
    nya_assert(_nya_nn_shape_equals(a, b), "nya_nn_add on mismatched shapes");

    NYA_NNTensor* out = _nya_nn_op(graph, _nya_nn_shape_of(a), NYA_NN_OP_ADD, a, b);
    nya_nn_simd_add(out->data, a->data, b->data, out->count);

    return out;
}

NYA_NNTensor* nya_nn_sub(NYA_NNGraph* graph, NYA_NNTensor* a, NYA_NNTensor* b) {
    nya_assert(_nya_nn_shape_equals(a, b), "nya_nn_sub on mismatched shapes");

    NYA_NNTensor* out = _nya_nn_op(graph, _nya_nn_shape_of(a), NYA_NN_OP_SUB, a, b);
    nya_nn_simd_sub(out->data, a->data, b->data, out->count);

    return out;
}

NYA_NNTensor* nya_nn_mul(NYA_NNGraph* graph, NYA_NNTensor* a, NYA_NNTensor* b) {
    nya_assert(_nya_nn_shape_equals(a, b), "nya_nn_mul on mismatched shapes");

    NYA_NNTensor* out = _nya_nn_op(graph, _nya_nn_shape_of(a), NYA_NN_OP_MUL, a, b);
    nya_nn_simd_mul(out->data, a->data, b->data, out->count);

    return out;
}

NYA_NNTensor* nya_nn_scale(NYA_NNGraph* graph, NYA_NNTensor* a, f32 scalar) {
    NYA_NNTensor* out = _nya_nn_op(graph, _nya_nn_shape_of(a), NYA_NN_OP_SCALE, a, nullptr);
    out->scalar       = scalar;

    nya_nn_simd_scale(out->data, a->data, scalar, out->count);

    return out;
}

NYA_NNTensor* nya_nn_matmul(NYA_NNGraph* graph, NYA_NNTensor* a, NYA_NNTensor* b) {
    nya_assert(a != nullptr && b != nullptr);
    nya_assert(a->rank == 2 && b->rank == 2, "nya_nn_matmul wants two rank 2 tensors");
    nya_assert(a->shape[1] == b->shape[0], "nya_nn_matmul inner dimensions %u and %u disagree", a->shape[1], b->shape[0]);

    u32 m = a->shape[0];
    u32 k = a->shape[1];
    u32 n = b->shape[1];

    NYA_NNTensor* out = _nya_nn_op(graph, NYA_NN_SHAPE(m, n), NYA_NN_OP_MATMUL, a, b);

    /*
     * i, then k, then j — not the textbook i, j, k.
     *
     * The inner loop walks `b` and `out` along consecutive addresses, so each pass streams two rows
     * rather than striding down a column. Same arithmetic, same result, and several times faster on
     * anything with a cache once the layers are wider than a few dozen units.
     */
    for (u32 i = 0; i < m; i++) {
        f32*       out_row = &out->data[i * n];
        const f32* a_row   = &a->data[i * k];

        for (u32 p = 0; p < k; p++) {
            f32 a_value = a_row[p];
            if (a_value == 0.0F) continue;

            nya_nn_simd_axpy(out_row, a_value, &b->data[p * n], n);
        }
    }

    return out;
}

NYA_NNTensor* nya_nn_bias(NYA_NNGraph* graph, NYA_NNTensor* a, NYA_NNTensor* bias) {
    nya_assert(a != nullptr && bias != nullptr);
    nya_assert(a->rank == 2, "nya_nn_bias wants a rank 2 tensor");
    nya_assert(bias->count == a->shape[1], "nya_nn_bias of %u into rows of %u", bias->count, a->shape[1]);

    NYA_NNTensor* out = _nya_nn_op(graph, _nya_nn_shape_of(a), NYA_NN_OP_BIAS, a, bias);

    u32 rows    = a->shape[0];
    u32 columns = a->shape[1];

    // One row at a time, because the bias vector is re-read per row rather than strided over.
    for (u32 i = 0; i < rows; i++) nya_nn_simd_add(&out->data[i * columns], &a->data[i * columns], bias->data, columns);

    return out;
}

NYA_NNTensor* nya_nn_relu(NYA_NNGraph* graph, NYA_NNTensor* a) {
    NYA_NNTensor* out = _nya_nn_op(graph, _nya_nn_shape_of(a), NYA_NN_OP_RELU, a, nullptr);
    nya_nn_simd_relu(out->data, a->data, out->count);

    return out;
}

NYA_NNTensor* nya_nn_tanh(NYA_NNGraph* graph, NYA_NNTensor* a) {
    NYA_NNTensor* out = _nya_nn_op(graph, _nya_nn_shape_of(a), NYA_NN_OP_TANH, a, nullptr);
    for (u32 i = 0; i < out->count; i++) out->data[i] = tanhf(a->data[i]);

    return out;
}

NYA_NNTensor* nya_nn_gather(NYA_NNGraph* graph, NYA_NNTensor* a, const u32* indices) {
    nya_assert(a != nullptr && indices != nullptr);
    nya_assert(a->rank == 2, "nya_nn_gather wants a rank 2 tensor");

    u32 rows    = a->shape[0];
    u32 columns = a->shape[1];

    NYA_NNTensor* out = _nya_nn_op(graph, NYA_NN_SHAPE(rows, 1), NYA_NN_OP_GATHER, a, nullptr);
    out->indices      = indices;

    for (u32 i = 0; i < rows; i++) {
        nya_assert(indices[i] < columns, "nya_nn_gather index %u past %u columns", indices[i], columns);
        out->data[i] = a->data[(i * columns) + indices[i]];
    }

    return out;
}

NYA_NNTensor* nya_nn_sum(NYA_NNGraph* graph, NYA_NNTensor* a) {
    NYA_NNTensor* out = _nya_nn_op(graph, NYA_NN_SHAPE(1), NYA_NN_OP_SUM, a, nullptr);

    out->data[0] = nya_nn_simd_sum(a->data, a->count);

    return out;
}

NYA_NNTensor* nya_nn_mean(NYA_NNGraph* graph, NYA_NNTensor* a) {
    NYA_NNTensor* out = _nya_nn_op(graph, NYA_NN_SHAPE(1), NYA_NN_OP_MEAN, a, nullptr);

    f32 total    = nya_nn_simd_sum(a->data, a->count);
    out->data[0] = a->count > 0 ? total / (f32)a->count : 0.0F;

    return out;
}

NYA_NNTensor* nya_nn_mse(NYA_NNGraph* graph, NYA_NNTensor* prediction, NYA_NNTensor* target) {
    nya_assert(_nya_nn_shape_equals(prediction, target), "nya_nn_mse on mismatched shapes");

    NYA_NNTensor* out = _nya_nn_op(graph, NYA_NN_SHAPE(1), NYA_NN_OP_MSE, prediction, target);

    f32 total = nya_nn_simd_sum_squared_difference(prediction->data, target->data, prediction->count);

    out->data[0] = prediction->count > 0 ? total / (f32)prediction->count : 0.0F;

    return out;
}

NYA_NNTensor* nya_nn_huber(NYA_NNGraph* graph, NYA_NNTensor* prediction, NYA_NNTensor* target, f32 delta) {
    nya_assert(_nya_nn_shape_equals(prediction, target), "nya_nn_huber on mismatched shapes");

    // A non-positive delta makes the loss linear everywhere with a zero-width quadratic region,
    // which is not a useful configuration and is always a mistake rather than an intent.
    if (delta <= 0.0F) delta = 1.0F;

    NYA_NNTensor* out = _nya_nn_op(graph, NYA_NN_SHAPE(1), NYA_NN_OP_HUBER, prediction, target);
    out->scalar       = delta;

    f32 total = 0.0F;
    for (u32 i = 0; i < prediction->count; i++) {
        f32 difference = prediction->data[i] - target->data[i];
        f32 magnitude  = fabsf(difference);

        // Quadratic within delta, linear past it, and the two agree in value and slope at the
        // boundary — which is what makes the loss smooth rather than merely continuous.
        total += magnitude <= delta ? 0.5F * difference * difference : delta * (magnitude - (0.5F * delta));
    }

    out->data[0] = prediction->count > 0 ? total / (f32)prediction->count : 0.0F;

    return out;
}

NYA_ConstCString nya_nn_op_name(NYA_NNOp op) {
    switch (op) {
        case NYA_NN_OP_NONE:   return "leaf";
        case NYA_NN_OP_ADD:    return "add";
        case NYA_NN_OP_SUB:    return "sub";
        case NYA_NN_OP_MUL:    return "mul";
        case NYA_NN_OP_SCALE:  return "scale";
        case NYA_NN_OP_MATMUL: return "matmul";
        case NYA_NN_OP_BIAS:   return "bias";
        case NYA_NN_OP_RELU:   return "relu";
        case NYA_NN_OP_TANH:   return "tanh";
        case NYA_NN_OP_GATHER: return "gather";
        case NYA_NN_OP_SUM:    return "sum";
        case NYA_NN_OP_MEAN:   return "mean";
        case NYA_NN_OP_MSE:    return "mse";
        case NYA_NN_OP_HUBER:  return "huber";

        case NYA_NN_OP_COUNT:
        default:               return "unknown";
    }
}

b8 nya_nn_tensor_is_finite(const NYA_NNTensor* tensor) {
    nya_assert(tensor != nullptr);

    for (u32 i = 0; i < tensor->count; i++) {
        if (!isfinite(tensor->data[i])) return false;
    }

    if (tensor->grad == nullptr) return true;

    for (u32 i = 0; i < tensor->count; i++) {
        if (!isfinite(tensor->grad[i])) return false;
    }

    return true;
}

NYA_NNTensor* nya_nn_graph_find_non_finite(const NYA_NNGraph* graph) {
    nya_assert(graph != nullptr);

    // Forward order, so the tensor returned is the earliest offender rather than merely an offender.
    // Every later one is a consequence of it, and reporting a consequence sends the search downstream
    // of the cause.
    for (u32 i = 0; i < graph->tape_count; i++) {
        if (!nya_nn_tensor_is_finite(graph->tape[i])) return graph->tape[i];
    }

    return nullptr;
}

void nya_nn_backward(NYA_NNGraph* graph, NYA_NNTensor* loss) {
    nya_assert(graph != nullptr);
    nya_assert(loss != nullptr);
    nya_assert(loss->count == 1, "nya_nn_backward wants a single element loss, got %u", loss->count);
    nya_assert(loss->grad != nullptr, "nya_nn_backward on a loss with no gradient; was it built under grad_begin?");

    /*
     * Every activation gradient on the tape is cleared before the sweep.
     *
     * Backward may be called more than once against one tape — two loss terms, or an auxiliary loss
     * — and each call must compute the derivative of *its own* loss. Without this, the first call
     * leaves every node it touched holding a gradient, and the second call's reverse sweep walks
     * those same nodes and propagates the stale values a second time.
     *
     * Measured, not theorised: with w used by two independent losses on one tape, dw came out as 11
     * where the correct total is 7. The first loss's contribution of 2 was counted, then re-counted
     * with its own leftover gradient compounding it.
     *
     * Only tape tensors are cleared. Parameters are not on the tape, so their gradients survive and
     * still accumulate across calls — which is exactly the distinction that makes two backward calls
     * sum to dtotal/dparam.
     */
    for (u32 i = 0; i < graph->tape_count; i++) nya_nn_tensor_zero_grad(graph->tape[i]);

    // After the clear, or it would be cleared too. dloss/dloss.
    loss->grad[0] = 1.0F;

    /*
     * Reverse tape order, which is a valid topological order by construction.
     *
     * A tensor is appended only after its inputs exist, so every consumer sits later in the tape than
     * everything it consumes. Walking backwards therefore visits a node only once every gradient
     * flowing into it has already been accumulated — no ordering pass, no visited set, no recursion
     * to blow a stack on a deep network.
     */
    for (u32 i = graph->tape_count; i > 0; i--) {
        NYA_NNTensor* tensor = graph->tape[i - 1];

        if (tensor->op == NYA_NN_OP_NONE) continue;
        if (tensor->grad == nullptr) continue;

        _nya_nn_backward_node(tensor);
    }
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

NYA_NNTensor* _nya_nn_tensor_alloc(NYA_Arena* arena, NYA_NNShape shape, b8 requires_grad) {
    nya_assert(shape.rank > 0 && shape.rank <= NYA_NN_TENSOR_MAX_DIMS, "a tensor needs 1 to %d dimensions, got %u", NYA_NN_TENSOR_MAX_DIMS, shape.rank);

    u32 count = 1;
    for (u32 i = 0; i < shape.rank; i++) {
        nya_assert(shape.dims[i] > 0, "dimension %u of a tensor is zero", i);
        count *= shape.dims[i];
    }

    NYA_NNTensor* tensor = nya_arena_alloc(arena, sizeof(NYA_NNTensor));
    *tensor              = (NYA_NNTensor){ .rank = shape.rank, .count = count, .requires_grad = requires_grad };

    for (u32 i = 0; i < shape.rank; i++) tensor->shape[i] = shape.dims[i];

    // Zeroed, because matmul accumulates into its output and every other op would otherwise start
    // from whatever the arena last held.
    tensor->data = nya_arena_alloc(arena, (u64)count * sizeof(f32));
    nya_memset(tensor->data, 0, (u64)count * sizeof(f32));

    if (requires_grad) {
        tensor->grad = nya_arena_alloc(arena, (u64)count * sizeof(f32));
        nya_memset(tensor->grad, 0, (u64)count * sizeof(f32));
    }

    return tensor;
}

NYA_NNTensor* _nya_nn_op(NYA_NNGraph* graph, NYA_NNShape shape, NYA_NNOp op, NYA_NNTensor* a, NYA_NNTensor* b) {
    nya_assert(graph != nullptr);
    nya_assert(a != nullptr);

    /*
     * A result needs a gradient exactly when one of its inputs does, and not when grad is off.
     *
     * Without the input check, every activation in a network whose inputs are all constants would
     * carry a gradient buffer that backward writes and nothing reads. With it, a forward pass under
     * grad_begin allocates half as much and records nothing.
     */
    b8 requires_grad = graph->no_grad_depth == 0 && ((a != nullptr && a->requires_grad) || (b != nullptr && b->requires_grad));

    NYA_NNTensor* out = _nya_nn_tensor_alloc(graph->allocator, shape, requires_grad);

    if (!requires_grad) return out;

    out->op        = op;
    out->inputs[0] = a;
    out->inputs[1] = b;

    nya_assert(
        graph->tape_count < NYA_NN_GRAPH_MAX_TENSORS,
        "the autograd tape is full at %d tensors; a forward pass is running without a "
        "nya_nn_graph_reset, or NYA_NN_GRAPH_MAX_TENSORS is too small for this network",
        NYA_NN_GRAPH_MAX_TENSORS
    );

    graph->tape[graph->tape_count++] = out;

    return out;
}

NYA_NNShape _nya_nn_shape_of(const NYA_NNTensor* tensor) {
    nya_assert(tensor != nullptr);

    NYA_NNShape shape = { .rank = tensor->rank };
    for (u32 i = 0; i < tensor->rank; i++) shape.dims[i] = tensor->shape[i];

    return shape;
}

b8 _nya_nn_shape_equals(const NYA_NNTensor* a, const NYA_NNTensor* b) {
    if (a == nullptr || b == nullptr) return false;
    if (a->rank != b->rank) return false;

    for (u32 i = 0; i < a->rank; i++) {
        if (a->shape[i] != b->shape[i]) return false;
    }

    return true;
}

void _nya_nn_backward_node(NYA_NNTensor* tensor) {
    NYA_NNTensor* a = tensor->inputs[0];
    NYA_NNTensor* b = tensor->inputs[1];

    // Written once here rather than at every branch: an input that wants no gradient is skipped, and
    // every rule below is free to assume the buffer exists.
    b8 grad_a = a != nullptr && a->grad != nullptr;
    b8 grad_b = b != nullptr && b->grad != nullptr;

    switch (tensor->op) {
        case NYA_NN_OP_ADD: {
            if (grad_a) nya_nn_simd_axpy(a->grad, 1.0F, tensor->grad, tensor->count);
            if (grad_b) nya_nn_simd_axpy(b->grad, 1.0F, tensor->grad, tensor->count);
        } break;

        case NYA_NN_OP_SUB: {
            if (grad_a) nya_nn_simd_axpy(a->grad, 1.0F, tensor->grad, tensor->count);
            if (grad_b) nya_nn_simd_axpy(b->grad, -1.0F, tensor->grad, tensor->count);
        } break;

        case NYA_NN_OP_MUL: {
            // Each side's gradient is scaled by the *other* side's value, which is why both forward
            // values have to still be alive here — the graph arena is not reset until after backward.
            for (u32 i = 0; i < tensor->count; i++) {
                if (grad_a) a->grad[i] += tensor->grad[i] * b->data[i];
                if (grad_b) b->grad[i] += tensor->grad[i] * a->data[i];
            }
        } break;

        case NYA_NN_OP_SCALE: {
            if (grad_a) nya_nn_simd_axpy(a->grad, tensor->scalar, tensor->grad, tensor->count);
        } break;

        case NYA_NN_OP_MATMUL: {
            u32 m = a->shape[0];
            u32 k = a->shape[1];
            u32 n = b->shape[1];

            // dA = dOut * B^T, dB = A^T * dOut. Written as loops over the same memory order as the
            // forward pass rather than by materialising a transpose, which would allocate during
            // backward — the one place that must not, since the graph arena is being read not grown.
            for (u32 i = 0; i < m; i++) {
                const f32* out_row = &tensor->grad[i * n];

                for (u32 p = 0; p < k; p++) {
                    const f32* b_row = &b->data[p * n];

                    if (grad_a) a->grad[(i * k) + p] += nya_nn_simd_dot(out_row, b_row, n);
                    if (grad_b) nya_nn_simd_axpy(&b->grad[p * n], a->data[(i * k) + p], out_row, n);
                }
            }
        } break;

        case NYA_NN_OP_BIAS: {
            u32 rows    = tensor->shape[0];
            u32 columns = tensor->shape[1];

            for (u32 i = 0; i < rows; i++) {
                const f32* row = &tensor->grad[i * columns];

                if (grad_a) nya_nn_simd_axpy(&a->grad[i * columns], 1.0F, row, columns);

                // Summed down the batch: the same bias element contributed to every row, so its
                // gradient is the total of what came back from all of them. Accumulating whole rows
                // into b->grad is that same sum, one row at a time instead of one element at a time.
                if (grad_b) nya_nn_simd_axpy(b->grad, 1.0F, row, columns);
            }
        } break;

        case NYA_NN_OP_RELU: {
            if (!grad_a) break;

            // Zero exactly at zero. The derivative is undefined there and either choice is defensible;
            // this one keeps a dead unit dead rather than reviving it on a boundary case.
            nya_nn_simd_relu_backward(a->grad, a->data, tensor->grad, tensor->count);
        } break;

        case NYA_NN_OP_TANH: {
            if (!grad_a) break;

            // 1 - tanh(x)^2, computed from the *output* rather than recomputing tanh of the input.
            for (u32 i = 0; i < tensor->count; i++) a->grad[i] += tensor->grad[i] * (1.0F - (tensor->data[i] * tensor->data[i]));
        } break;

        case NYA_NN_OP_GATHER: {
            if (!grad_a) break;

            u32 rows    = a->shape[0];
            u32 columns = a->shape[1];

            // Only the selected column of each row receives anything. Every other action's value is
            // untouched by this loss, which is exactly the semantics DQN needs.
            for (u32 i = 0; i < rows; i++) a->grad[(i * columns) + tensor->indices[i]] += tensor->grad[i];
        } break;

        case NYA_NN_OP_SUM: {
            if (!grad_a) break;

            for (u32 i = 0; i < a->count; i++) a->grad[i] += tensor->grad[0];
        } break;

        case NYA_NN_OP_MEAN: {
            if (!grad_a) break;

            f32 scale = a->count > 0 ? 1.0F / (f32)a->count : 0.0F;
            for (u32 i = 0; i < a->count; i++) a->grad[i] += tensor->grad[0] * scale;
        } break;

        case NYA_NN_OP_MSE: {
            f32 scale = a->count > 0 ? 2.0F / (f32)a->count : 0.0F;

            for (u32 i = 0; i < a->count; i++) {
                f32 gradient = tensor->grad[0] * scale * (a->data[i] - b->data[i]);

                if (grad_a) a->grad[i] += gradient;

                // The target usually requires no gradient, but if it was produced by the network —
                // as a bootstrapped target is before it is detached — the sign is simply flipped.
                if (grad_b) b->grad[i] -= gradient;
            }
        } break;

        case NYA_NN_OP_HUBER: {
            f32 scale = a->count > 0 ? 1.0F / (f32)a->count : 0.0F;
            f32 delta = tensor->scalar;

            for (u32 i = 0; i < a->count; i++) {
                f32 difference = a->data[i] - b->data[i];

                // The clamp *is* the point: past delta the gradient stops growing with the error, so
                // one catastrophic target cannot dominate the batch.
                f32 slope = nya_clamp(difference, -delta, delta);

                if (grad_a) a->grad[i] += tensor->grad[0] * scale * slope;
                if (grad_b) b->grad[i] -= tensor->grad[0] * scale * slope;
            }
        } break;

        case NYA_NN_OP_NONE:
        case NYA_NN_OP_COUNT:
        default:               break;
    }
}
