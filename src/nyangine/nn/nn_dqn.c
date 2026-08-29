#include "nyangine/nyangine.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * CONSTANTS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/** Gradient steps the reported average loss covers. Long enough to be steady, short enough to move. */
#define _NYA_NN_DQN_LOSS_HISTORY 64

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * TYPES
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

struct NYA_NNDQN {
    NYA_NNDQNConfig config;

    NYA_Arena* allocator;
    NYA_RNG*   rng;

    /** The network being trained, and the frozen copy that supplies the regression targets. */
    NYA_NNSequential* online;
    NYA_NNSequential* target;

    NYA_NNOptimizer* optimizer;
    NYA_NNGraph*     graph;

    /*
     * ── Replay ──
     *
     * One flat block per field rather than an array of structs. A batch reads one field across many
     * scattered rows at a time — every state, then every action — so this is the layout that touches
     * fewer cache lines. It also makes the copy into a batch tensor a straight memcpy per row.
     */

    f32* states;       /** capacity * state_size */
    f32* next_states;  /** capacity * state_size */
    f32* rewards;      /** capacity */
    u32* actions;      /** capacity */
    b8*  terminals;    /** capacity */

    /** Where the next transition goes. Wraps: the oldest is overwritten once full. */
    u32 replay_cursor;

    /** Transitions stored, saturating at capacity. Distinguishes "empty slot" from "a zero reward". */
    u32 replay_count;

    /*
     * ── Scratch, reused every gradient step ──
     *
     * Owned rather than allocated per step, which is what keeps a training step free of allocation
     * once it has run once. See the steady state note in nn_tensor.h.
     */

    u32* batch_indices;
    f32* batch_states;
    f32* batch_next_states;
    u32* batch_actions;
    f32* batch_targets;

    /*
     * ── Bookkeeping ──
     */

    u64 train_step_count;

    /** Fractional gradient steps owed to nya_nn_dqn_train_for, carried between frames. */
    f32 train_step_debt;

    f32 loss_history[_NYA_NN_DQN_LOSS_HISTORY];
    u32 loss_cursor;
    u32 loss_count;
};

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API DECLARATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/** Fills anything the caller left at zero with a default. */
NYA_INTERNAL void _nya_nn_dqn_apply_config_defaults(NYA_NNDQNConfig* config);
NYA_INTERNAL void _nya_nn_dqn_build_sequential(NYA_NNDQN* dqn, NYA_NNSequential* sequential);

/** Runs the online network over one state, without recording a tape. Result is [1, action_count]. */
NYA_INTERNAL NYA_NNTensor* _nya_nn_dqn_evaluate(NYA_NNDQN* dqn, const f32* state) __attr_no_discard;

/** A uniform index below `bound`. */
NYA_INTERNAL u32 _nya_nn_dqn_index(NYA_NNDQN* dqn, u32 bound) __attr_no_discard;

/** Builds the batch's regression targets from the replay sample. The Q-learning update itself. */
NYA_INTERNAL void _nya_nn_dqn_build_targets(NYA_NNDQN* dqn);

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PUBLIC API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

NYA_NNDQN* nya_nn_dqn_create(NYA_Arena* arena, NYA_NNDQNConfig config) {
    nya_assert(arena != nullptr);
    nya_assert(config.state_size > 0, "a DQN needs a state size");
    nya_assert(config.action_count > 0, "a DQN needs at least one action");

    _nya_nn_dqn_apply_config_defaults(&config);

    NYA_NNDQN* dqn = nya_arena_alloc(arena, sizeof(NYA_NNDQN));
    *dqn           = (NYA_NNDQN){ .config = config, .allocator = arena };

    dqn->rng = nya_rng_create_in(arena, config.rng_seed);

    // Sized for a batch through the hidden stack, twice over — the online pass and the target pass
    // both live on the tape at once during a training step.
    dqn->graph = nya_nn_graph_create(arena);

    /*
     * Two networks of identical architecture, then synchronised.
     *
     * Built independently and copied rather than constructed identically: the copy is the operation
     * that has to be right for the rest of training to mean anything, so it runs once at startup
     * where a mistake is obvious rather than only after the first soft update.
     */
    dqn->online = nya_nn_sequential_create(arena);
    _nya_nn_dqn_build_sequential(dqn, dqn->online);

    dqn->target = nya_nn_sequential_create(arena);
    _nya_nn_dqn_build_sequential(dqn, dqn->target);

    nya_nn_sequential_copy_parameters(dqn->target, dqn->online);

    // The target network is never trained directly, so it is not registered with the optimizer. Only
    // the soft update moves it.
    dqn->optimizer = nya_nn_optimizer_adam(
        arena,
        (NYA_NNOptimizerConfig){ .learning_rate = config.learning_rate, .gradient_clip = config.gradient_clip > 0.0F ? config.gradient_clip : 0.0F }
    );
    nya_nn_optimizer_add_sequential(dqn->optimizer, dqn->online);

    u32 capacity   = config.replay_capacity;
    u32 state_size = config.state_size;
    u32 batch      = config.batch_size;

    dqn->states      = nya_arena_alloc(arena, (u64)capacity * state_size * sizeof(f32));
    dqn->next_states = nya_arena_alloc(arena, (u64)capacity * state_size * sizeof(f32));
    dqn->rewards     = nya_arena_alloc(arena, (u64)capacity * sizeof(f32));
    dqn->actions     = nya_arena_alloc(arena, (u64)capacity * sizeof(u32));
    dqn->terminals   = nya_arena_alloc(arena, (u64)capacity * sizeof(b8));

    dqn->batch_indices     = nya_arena_alloc(arena, (u64)batch * sizeof(u32));
    dqn->batch_states      = nya_arena_alloc(arena, (u64)batch * state_size * sizeof(f32));
    dqn->batch_next_states = nya_arena_alloc(arena, (u64)batch * state_size * sizeof(f32));
    dqn->batch_actions     = nya_arena_alloc(arena, (u64)batch * sizeof(u32));
    dqn->batch_targets     = nya_arena_alloc(arena, (u64)batch * sizeof(f32));

    return dqn;
}

u32 nya_nn_dqn_act(NYA_NNDQN* dqn, const f32* state) {
    nya_assert(dqn != nullptr);
    nya_assert(state != nullptr);

    // Rolled before the network is consulted, so an exploring step costs no forward pass at all —
    // which matters early on, when almost every step explores.
    if (nya_rng_gen_bool(dqn->rng, nya_nn_dqn_exploration(dqn))) return _nya_nn_dqn_index(dqn, dqn->config.action_count);

    return nya_nn_dqn_act_greedy(dqn, state);
}

u32 nya_nn_dqn_act_greedy(NYA_NNDQN* dqn, const f32* state) {
    nya_assert(dqn != nullptr);
    nya_assert(state != nullptr);

    return nya_nn_tensor_argmax_row(_nya_nn_dqn_evaluate(dqn, state), 0);
}

void nya_nn_dqn_action_values(NYA_NNDQN* dqn, const f32* state, f32* out_values) {
    nya_assert(dqn != nullptr);
    nya_assert(out_values != nullptr);

    NYA_NNTensor* values = _nya_nn_dqn_evaluate(dqn, state);
    for (u32 i = 0; i < dqn->config.action_count; i++) out_values[i] = values->data[i];
}

void nya_nn_dqn_observe(NYA_NNDQN* dqn, const f32* state, u32 action, f32 reward, const f32* next_state, b8 terminal) {
    nya_assert(dqn != nullptr);
    nya_assert(state != nullptr);
    nya_assert(action < dqn->config.action_count, "action %u past the %u the agent has", action, dqn->config.action_count);

    u32 state_size = dqn->config.state_size;
    u32 slot       = dqn->replay_cursor;

    nya_memcpy(&dqn->states[(u64)slot * state_size], state, (u64)state_size * sizeof(f32));

    // A terminal transition has no next state to bootstrap from, so what is stored there is never
    // read. Zeroed anyway rather than left as whatever the previous occupant held, so a bug that
    // does read it produces something repeatable instead of stale values from an old episode.
    if (terminal || next_state == nullptr) nya_memset(&dqn->next_states[(u64)slot * state_size], 0, (u64)state_size * sizeof(f32));
    else nya_memcpy(&dqn->next_states[(u64)slot * state_size], next_state, (u64)state_size * sizeof(f32));

    dqn->rewards[slot]   = reward;
    dqn->actions[slot]   = action;
    dqn->terminals[slot] = terminal;

    dqn->replay_cursor = (dqn->replay_cursor + 1) % dqn->config.replay_capacity;
    if (dqn->replay_count < dqn->config.replay_capacity) dqn->replay_count++;
}

f32 nya_nn_dqn_train_step(NYA_NNDQN* dqn) {
    nya_assert(dqn != nullptr);

    if (dqn->replay_count < dqn->config.learning_starts) return 0.0F;
    if (dqn->replay_count < dqn->config.batch_size) return 0.0F;

    u32 batch      = dqn->config.batch_size;
    u32 state_size = dqn->config.state_size;

    /*
     * Sampled with replacement.
     *
     * A duplicate in a batch is harmless — it weights that transition twice — and rejecting them
     * would need a set membership test per draw for a benefit that does not exist at these sizes.
     */
    for (u32 i = 0; i < batch; i++) {
        u32 index = _nya_nn_dqn_index(dqn, dqn->replay_count);

        dqn->batch_indices[i] = index;
        dqn->batch_actions[i] = dqn->actions[index];

        nya_memcpy(&dqn->batch_states[(u64)i * state_size], &dqn->states[(u64)index * state_size], (u64)state_size * sizeof(f32));
        nya_memcpy(&dqn->batch_next_states[(u64)i * state_size], &dqn->next_states[(u64)index * state_size], (u64)state_size * sizeof(f32));
    }

    nya_nn_graph_reset(dqn->graph);

    _nya_nn_dqn_build_targets(dqn);

    /*
     * The online pass, which is the only part that records a tape.
     *
     * Q(s, a) for the action actually taken — nya_nn_gather is what picks it, and its backward is
     * what confines the gradient to that one action's output. The values of the actions not taken
     * are not evidence about anything and must not move.
     */
    nya_nn_optimizer_zero_grad(dqn->optimizer);

    NYA_NNTensor* states     = nya_nn_tensor_from(dqn->graph, NYA_NN_SHAPE(batch, state_size), dqn->batch_states);
    NYA_NNTensor* all_values = nya_nn_sequential_forward(dqn->online, dqn->graph, states);
    NYA_NNTensor* taken      = nya_nn_gather(dqn->graph, all_values, dqn->batch_actions);
    NYA_NNTensor* targets    = nya_nn_tensor_from(dqn->graph, NYA_NN_SHAPE(batch, 1), dqn->batch_targets);
    NYA_NNTensor* loss       = nya_nn_huber(dqn->graph, taken, targets, dqn->config.huber_delta);

    nya_nn_backward(dqn->graph, loss);
    nya_nn_optimizer_step(dqn->optimizer);

    // Every step, by a small fraction. See NYA_NNDQNConfig.target_tau.
    nya_nn_sequential_soft_update(dqn->target, dqn->online, dqn->config.target_tau);

    f32 loss_value = nya_nn_tensor_item(loss);

    dqn->train_step_count++;

    dqn->loss_history[dqn->loss_cursor] = loss_value;
    dqn->loss_cursor                    = (dqn->loss_cursor + 1) % _NYA_NN_DQN_LOSS_HISTORY;
    if (dqn->loss_count < _NYA_NN_DQN_LOSS_HISTORY) dqn->loss_count++;

    // Reset here rather than at the top of the next step, so nothing the step produced is still
    // reachable once it returns.
    nya_nn_graph_reset(dqn->graph);

    return loss_value;
}

f32 nya_nn_dqn_train_for(NYA_NNDQN* dqn, f32 delta_time_s) {
    nya_assert(dqn != nullptr);

    if (delta_time_s <= 0.0F) return 0.0F;

    /*
     * A rate and a debt, not a per-frame count.
     *
     * Stepping once per frame would train at more than twice the speed on a 144Hz monitor as on a
     * 60Hz one, which makes a run unreproducible for reasons that have nothing to do with the
     * algorithm. Carrying the fraction means a rate below one step per frame works too.
     */
    dqn->train_step_debt += delta_time_s * dqn->config.train_steps_per_second;

    u32 steps = (u32)dqn->train_step_debt;
    if (steps == 0) return 0.0F;

    // A frame that stalled — a breakpoint, a window drag — leaves a large delta, and making all of
    // it up at once stalls the next frame too. Falling behind is better than compounding a hitch.
    if (steps > dqn->config.max_steps_per_frame) {
        steps                = dqn->config.max_steps_per_frame;
        dqn->train_step_debt = 0.0F;
    } else {
        dqn->train_step_debt -= (f32)steps;
    }

    /*
     * The step count bounds how many, this bounds how long. See NYA_NNDQNConfig.max_step_milliseconds.
     *
     * Checked after each step rather than before, so the first one always runs and training never
     * stalls completely on a machine that cannot afford even one step per frame.
     */
    u64 started_ns = nya_clock_get_monotonic_ns();
    u64 budget_ns  = (u64)(dqn->config.max_step_milliseconds * 1'000'000.0);

    f32 loss = 0.0F;
    for (u32 i = 0; i < steps; i++) {
        loss = nya_nn_dqn_train_step(dqn);

        if (budget_ns == 0) continue;
        if (nya_clock_get_monotonic_ns() - started_ns >= budget_ns) {
            // The unrun steps are dropped rather than carried. Carrying them would make every later
            // frame run at the cap until the debt drained, which is the catch-up spiral the cap
            // above already refuses for the same reason.
            dqn->train_step_debt = 0.0F;
            break;
        }
    }

    return loss;
}

u64 nya_nn_dqn_train_step_count(const NYA_NNDQN* dqn) {
    nya_assert(dqn != nullptr);

    return dqn->train_step_count;
}

u32 nya_nn_dqn_replay_count(const NYA_NNDQN* dqn) {
    nya_assert(dqn != nullptr);

    return dqn->replay_count;
}

f32 nya_nn_dqn_exploration(const NYA_NNDQN* dqn) {
    nya_assert(dqn != nullptr);

    // Linear in gradient steps, not in frames or transitions: it is training that makes the greedy
    // action worth trusting, so that is what exploration should decay against.
    f32 progress = dqn->config.exploration_steps > 0 ? (f32)dqn->train_step_count / (f32)dqn->config.exploration_steps : 1.0F;
    progress     = nya_clamp(progress, 0.0F, 1.0F);

    return dqn->config.exploration_start + ((dqn->config.exploration_end - dqn->config.exploration_start) * progress);
}

f32 nya_nn_dqn_average_loss(const NYA_NNDQN* dqn) {
    nya_assert(dqn != nullptr);

    if (dqn->loss_count == 0) return 0.0F;

    f32 total = 0.0F;
    for (u32 i = 0; i < dqn->loss_count; i++) total += dqn->loss_history[i];

    return total / (f32)dqn->loss_count;
}

NYA_NNSequential* nya_nn_dqn_network(NYA_NNDQN* dqn) {
    nya_assert(dqn != nullptr);

    return dqn->online;
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

void _nya_nn_dqn_apply_config_defaults(NYA_NNDQNConfig* config) {
    if (config->replay_capacity == 0) config->replay_capacity = 10000;
    if (config->batch_size == 0) config->batch_size = 64;
    if (config->discount <= 0.0F) config->discount = 0.99F;
    if (config->learning_rate <= 0.0F) config->learning_rate = 1e-3F;
    if (config->huber_delta <= 0.0F) config->huber_delta = 1.0F;
    if (config->target_tau <= 0.0F) config->target_tau = 0.005F;
    if (config->train_steps_per_second <= 0.0F) config->train_steps_per_second = 30.0F;
    if (config->max_steps_per_frame == 0) config->max_steps_per_frame = 4;

    if (config->exploration_start <= 0.0F) config->exploration_start = 1.0F;
    if (config->exploration_end <= 0.0F) config->exploration_end = 0.05F;
    if (config->exploration_steps == 0) config->exploration_steps = 10000;

    // Negative means the caller explicitly wants none, which is not the same as leaving it at zero.
    if (config->gradient_clip == 0.0F) config->gradient_clip = 10.0F;
    else if (config->gradient_clip < 0.0F) config->gradient_clip = 0.0F;

    if (config->layer_count == 0) {
        config->layer_count = 2;
        config->layers[0]   = (NYA_NNDQNLayerConfig){ .kind = NYA_NN_LAYER_LINEAR, .units = 64 };
        config->layers[1]   = (NYA_NNDQNLayerConfig){ .kind = NYA_NN_LAYER_RELU, .units = 0 };
    }

    if (config->layer_count > NYA_NN_DQN_MAX_LAYERS) config->layer_count = NYA_NN_DQN_MAX_LAYERS;

    // Four batches, so the first gradient step sees a sample that is at least somewhat decorrelated.
    if (config->learning_starts == 0) config->learning_starts = config->batch_size * 4;
    if (config->learning_starts > config->replay_capacity) config->learning_starts = config->replay_capacity;

    nya_assert(config->batch_size <= config->replay_capacity, "a batch of %u cannot be drawn from a replay of %u", config->batch_size, config->replay_capacity);
}

void _nya_nn_dqn_build_sequential(NYA_NNDQN* dqn, NYA_NNSequential* sequential) {
    u32 in_features = dqn->config.state_size;

    for (u32 i = 0; i < dqn->config.layer_count; i++) {
        NYA_NNDQNLayerConfig layer = dqn->config.layers[i];

        switch (layer.kind) {
            case NYA_NN_LAYER_LINEAR: {
                u32 units = layer.units > 0 ? layer.units : 64;
                nya_nn_sequential_push(sequential, nya_nn_layer_linear(dqn->allocator, dqn->rng, in_features, units));
                in_features = units;
                break;
            }

            case NYA_NN_LAYER_RELU:
                nya_nn_sequential_push(sequential, nya_nn_layer_relu(dqn->allocator));
                break;

            case NYA_NN_LAYER_TANH:
                nya_nn_sequential_push(sequential, nya_nn_layer_tanh(dqn->allocator));
                break;

            default:
                nya_log_panic("unsupported DQN layer kind %d", (int)layer.kind);
        }
    }

    nya_nn_sequential_push(sequential, nya_nn_layer_linear(dqn->allocator, dqn->rng, in_features, dqn->config.action_count));
}

NYA_NNTensor* _nya_nn_dqn_evaluate(NYA_NNDQN* dqn, const f32* state) {
    /*
     * Acting does not need a backward pass, so it records nothing.
     *
     * Without this every call would push five tensors onto a tape nobody reads and allocate a
     * gradient buffer for each — and acting happens every frame where training happens rarely, so it
     * is the call that would dominate.
     */
    nya_nn_graph_reset(dqn->graph);
    nya_nn_graph_grad_begin(dqn->graph);

    NYA_NNTensor* input  = nya_nn_tensor_from(dqn->graph, NYA_NN_SHAPE(1, dqn->config.state_size), state);
    NYA_NNTensor* values = nya_nn_sequential_forward(dqn->online, dqn->graph, input);

    nya_nn_graph_grad_end(dqn->graph);

    // Still live: the caller reads it before anything resets the graph again. Every entry point that
    // returns one of these consumes it immediately, which is why this is safe and why it is not
    // exposed outside this file.
    return values;
}

u32 _nya_nn_dqn_index(NYA_NNDQN* dqn, u32 bound) {
    if (bound <= 1) return 0;

    return nya_rng_sample_u32(dqn->rng, (NYA_RNGDistribution){ .type = NYA_RNG_DISTRIBUTION_UNIFORM, .uniform = { .min = 0.0F, .max = (f32)bound } }) % bound;
}

void _nya_nn_dqn_build_targets(NYA_NNDQN* dqn) {
    u32 batch        = dqn->config.batch_size;
    u32 state_size   = dqn->config.state_size;
    u32 action_count = dqn->config.action_count;

    /*
     * The whole target computation is outside the tape.
     *
     * A target is a number, not a function of the weights being trained. Letting a gradient flow
     * back through it would be optimising the target to match the prediction as much as the other
     * way round, and the pair converges happily to something meaningless. This is the "detach" that
     * every DQN implementation has and that is silently catastrophic to omit.
     */
    nya_nn_graph_grad_begin(dqn->graph);

    NYA_NNTensor* next_states = nya_nn_tensor_from(dqn->graph, NYA_NN_SHAPE(batch, state_size), dqn->batch_next_states);

    NYA_NNTensor* target_values = nya_nn_sequential_forward(dqn->target, dqn->graph, next_states);

    // Only needed for double Q-learning, which chooses with the online network and evaluates with
    // the target one. Plain DQN does both with the target network and skips this pass entirely.
    NYA_NNTensor* online_values = dqn->config.disable_double_q ? nullptr : nya_nn_sequential_forward(dqn->online, dqn->graph, next_states);

    for (u32 i = 0; i < batch; i++) {
        u32 index  = dqn->batch_indices[i];
        f32 reward = dqn->rewards[index];

        // A terminal transition has no future: its value is exactly the reward. Bootstrapping past
        // the end of an episode is the other classic way to make values diverge.
        if (dqn->terminals[index]) {
            dqn->batch_targets[i] = reward;
            continue;
        }

        f32 next_value = 0.0F;

        if (online_values != nullptr) {
            // Chosen by the online network, valued by the target network. The two disagree about
            // which action is best exactly where the estimate is noisy, and that disagreement is
            // what cancels the upward bias of taking a max over noise.
            u32 best = nya_nn_tensor_argmax_row(online_values, i);
            next_value = nya_nn_tensor_at(target_values, i, best);
        } else {
            next_value = nya_nn_tensor_max_row(target_values, i);
        }

        nya_unused(action_count);

        dqn->batch_targets[i] = reward + (dqn->config.discount * next_value);
    }

    nya_nn_graph_grad_end(dqn->graph);
}
