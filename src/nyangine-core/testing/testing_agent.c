#include "nyangine-core/nyangine.h"

#ifdef NYA_TESTING

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * TYPES
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

struct NYA_Agent {
    NYA_Arena* allocator;

    NYA_AgentKind kind;

    u32 sense_count;
    u32 action_count;

    NYA_AgentTrialFn trial;
    void*            user_data;

    /** Hex, as the two learners and NYA_RNG want their seeds. Derived from NYA_AgentOptions.seed. */
    char rng_seed[17];

    /*
     * ── DQN ──
     */
    NYA_NNDQN* dqn;

    /** The observation the last choice was made from, which a transition needs as its `from`. */
    f32* previous_state;
    u32  previous_action;
    b8   have_previous;

    /**
     * A reward waiting for the observation that follows it.
     *
     * A transition is (state, action, reward, next state), and the next state is what the scenario
     * sees on the tick after the action ran. So a reward is held until the next choice arrives with
     * that observation in hand, rather than stored against a copy of the state it came from, which
     * would teach the network that nothing it does changes anything.
     * */
    f32 pending_reward;
    b8  have_pending;

    /*
     * ── NEAT ──
     */
    NYA_Neat* neat;

    /** The genome being trialled right now, set by the trial function around each call. */
    NYA_NeatNetwork* network;

    /** "s0".."sN" and "a0".."aM", owned here because a network refers to its labels by pointer. */
    char (*sensor_labels)[NYA_AGENT_LABEL_MAX];
    char (*output_labels)[NYA_AGENT_LABEL_MAX];

    /*
     * ── Score ──
     */
    f32 score;
    f32 score_best;
    b8  have_score_best;

    u64 choices;
};

/**
 * The agent a NEAT generation is trialling, or null.
 *
 * NYA_NeatTrialFunction is `f64 (*)(NYA_NeatNetwork*)` with no context of its own, and a generation
 * is a strictly sequential thing inside one nya_agent_learn call, so a file-scope pointer set for the
 * length of that call is the whole of the state it needs. Asserted on entry rather than left to
 * dereference null.
 * */
NYA_INTERNAL NYA_Agent* _NYA_AGENT_EVOLVING = nullptr;

NYA_INTERNAL NYA_ConstCString _NYA_AGENT_KIND_NAMES[NYA_AGENT_KIND_COUNT] = { "random", "dqn", "neat" };

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API DECLARATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/** Builds the label tables and the starting topology: a bias, one sensor per sense, one output per action. */
NYA_INTERNAL NYA_NeatNetwork* _nya_agent_neat_seed(NYA_Agent* agent);

/** NYA_NeatTrialFunction: plays one session with `network` as the policy and returns its score. */
NYA_INTERNAL f64 _nya_agent_neat_trial(NYA_NeatNetwork* network);

/**
 * Runs `network` over `senses` and returns the index of its largest output, ties drawn from the
 * session's own stream.
 *
 * Ties are not a corner case here, they are where a NEAT run starts: the seed topology has no
 * connections at all, so every output is the same number and argmax would press the first action for
 * the whole of the first generation. Drawing among the tied outputs makes a genome with nothing to
 * say behave like the random baseline, and leaves every genome that has grown a connection deciding.
 * */
NYA_INTERNAL u32 _nya_agent_neat_choose(NYA_Agent* agent, NYA_Session* session, const f32* senses) __attr_no_discard;

/** How close two outputs have to be to count as the same preference. */
#define _NYA_AGENT_TIE_EPSILON 1e-9

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PUBLIC API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/*
 * ─────────────────────────────────────────────────────────
 * LIFETIME
 * ─────────────────────────────────────────────────────────
 */

NYA_Agent* nya_agent_create_with_options(NYA_AgentOptions options) {
    nya_assert((u32)options.kind < (u32)NYA_AGENT_KIND_COUNT, "agent kind %u is not a kind", (u32)options.kind);
    nya_assert(options.sense_count > 0, "an agent with nothing to look at cannot learn anything");
    nya_assert(options.action_count > 1, "an agent choosing between fewer than two actions is not choosing");
    nya_assert(options.kind != NYA_AGENT_KIND_NEAT || options.trial != nullptr, "a NEAT agent needs a trial that plays one session");

    NYA_Arena* allocator = nya_arena_create(.name = "agent");

    NYA_Agent* agent = nya_arena_alloc(allocator, sizeof(NYA_Agent));

    *agent = (NYA_Agent){
        .allocator    = allocator,
        .kind         = options.kind,
        .sense_count  = options.sense_count,
        .action_count = options.action_count,
        .trial        = options.trial,
        .user_data    = options.user_data,
    };

    // A fixed seed rather than the system entropy source when none is given: an agent that cannot be
    // asked to do the same thing twice cannot hand back a bug report.
    u64 seed = options.seed != 0 ? options.seed : 0xA6E7A6E7A6E7A6E7ULL;
    (void)snprintf(agent->rng_seed, sizeof(agent->rng_seed), "%016llX", (unsigned long long)seed);

    switch (agent->kind) {
        case NYA_AGENT_KIND_RANDOM: break;

        case NYA_AGENT_KIND_DQN: {
            agent->previous_state = nya_arena_alloc(allocator, sizeof(f32) * agent->sense_count);

            /*
             * Two small hidden layers, the same shape the drones in gnyame learn to fly with. A menu
             * is not a harder function than a thrust vector, and a bigger network is a slower session
             * for no reason anyone has measured.
             */
            agent->dqn = nya_nn_dqn_create(allocator, (NYA_NNDQNConfig){
                .state_size   = agent->sense_count,
                .action_count = agent->action_count,
                .layers       = {
                    { .kind = NYA_NN_LAYER_LINEAR, .units = 32 },
                    { .kind = NYA_NN_LAYER_RELU },
                    { .kind = NYA_NN_LAYER_LINEAR, .units = 32 },
                    { .kind = NYA_NN_LAYER_RELU },
                },
                .layer_count       = 4,
                .replay_capacity   = 8192,
                .batch_size        = 32,
                .discount          = 0.95F,
                .exploration_steps = 4000,
                .learning_starts   = 256,
                .rng_seed          = agent->rng_seed,
            });
        } break;

        case NYA_AGENT_KIND_NEAT: {
            agent->neat = nya_nn_neat_create((NYA_NeatConfig){
                .seed                = _nya_agent_neat_seed(agent),
                .trial_function      = _nya_agent_neat_trial,
                .activation_function = nya_nn_neat_tanh,
                .population_size     = options.population != 0 ? options.population : NYA_AGENT_POPULATION_DEFAULT,
                .rng_seed            = agent->rng_seed,

                // a seed with no connections has nothing to tune until mutation adds some, so they are
                // added more often. The same reasoning as gnyame's drones.
                .mutation_add_connection_chance = 0.2,
                .target_species_count           = 6,
            });
        } break;

        case NYA_AGENT_KIND_COUNT:
        default: nya_unreachable();
    }

    return agent;
}

void nya_agent_destroy(NYA_Agent* agent) {
    if (agent == nullptr) return;

    if (agent->neat != nullptr) nya_nn_neat_destroy(agent->neat);

    // the DQN, its replay buffer and the label tables all came out of this one arena.
    nya_arena_destroy(agent->allocator);
}

/*
 * ─────────────────────────────────────────────────────────
 * PLAYING
 * ─────────────────────────────────────────────────────────
 */

u32 nya_agent_choose(NYA_Agent* agent, NYA_Session* session, const f32* senses, u32 sense_count) {
    nya_assert(agent != nullptr);
    nya_assert(session != nullptr);
    nya_assert(senses != nullptr);
    nya_assert(
        sense_count == agent->sense_count,
        "the scenario reported " FMTu32 " senses to an agent built for " FMTu32,
        sense_count,
        agent->sense_count
    );

    u32 action = 0;

    switch (agent->kind) {
        // drawn from the session's own stream rather than from a generator of its own, so the baseline
        // replays from the seed like everything else does.
        case NYA_AGENT_KIND_RANDOM: action = (u32)nya_session_below(session, agent->action_count); break;

        case NYA_AGENT_KIND_DQN: {
            // the observation the held reward was waiting for; see `pending_reward`.
            if (agent->have_pending && agent->have_previous) {
                nya_nn_dqn_observe(agent->dqn, agent->previous_state, agent->previous_action, agent->pending_reward, senses, false);
                (void)nya_nn_dqn_train_step(agent->dqn);
            }

            agent->have_pending = false;

            // epsilon-greedy, from the DQN's seeded RNG: exploring is most of what finds a crash, and
            // an agent that only ever plays its best move stops looking.
            action = nya_nn_dqn_act(agent->dqn, senses);

            nya_memcpy(agent->previous_state, senses, sizeof(f32) * agent->sense_count);
            agent->previous_action = action;
            agent->have_previous   = true;
        } break;

        case NYA_AGENT_KIND_NEAT: action = _nya_agent_neat_choose(agent, session, senses); break;

        case NYA_AGENT_KIND_COUNT:
        default: nya_unreachable();
    }

    nya_assert(action < agent->action_count, "the agent chose action " FMTu32 " of " FMTu32, action, agent->action_count);

    agent->choices++;

    return action;
}

void nya_agent_reward(NYA_Agent* agent, f32 reward, b8 terminal) {
    nya_assert(agent != nullptr);

    agent->score += reward;

    if (agent->kind == NYA_AGENT_KIND_DQN) {
        // Held for the next observation, which is the transition's `next state`; see `pending_reward`.
        // One gradient step per reward keeps the learning inside the session rather than in a burst
        // after it, which is what makes a long fast-forwarded run an education.
        agent->pending_reward = reward;
        agent->have_pending   = true;

        if (terminal && agent->have_previous) {
            // A terminal transition does not bootstrap from its next state, so there is nothing to
            // wait for and the state it came from stands in for a state that is never read.
            nya_nn_dqn_observe(agent->dqn, agent->previous_state, agent->previous_action, reward, agent->previous_state, true);
            (void)nya_nn_dqn_train_step(agent->dqn);

            agent->have_pending = false;
        }
    }

    if (!terminal) return;

    if (!agent->have_score_best || agent->score > agent->score_best) {
        agent->score_best      = agent->score;
        agent->have_score_best = true;
    }

    agent->score         = 0.0F;
    agent->have_previous = false;
}

void nya_agent_learn(NYA_Agent* agent) {
    nya_assert(agent != nullptr);
    nya_assert(!nya_session_is_playing(), "a round of learning runs between sessions, never inside one");

    switch (agent->kind) {
        case NYA_AGENT_KIND_RANDOM: break;

        case NYA_AGENT_KIND_DQN: {
            for (u32 i = 0; i < NYA_AGENT_LEARN_STEPS; i++) (void)nya_nn_dqn_train_step(agent->dqn);
        } break;

        case NYA_AGENT_KIND_NEAT: {
            nya_assert(_NYA_AGENT_EVOLVING == nullptr, "a NEAT generation is already running");

            _NYA_AGENT_EVOLVING = agent;
            nya_nn_neat_step(agent->neat);
            _NYA_AGENT_EVOLVING = nullptr;

            // the best genome so far is what the next round of trials starts from, and what a caller
            // that wants to watch the agent play would run.
            agent->network = nya_nn_neat_best(agent->neat);
        } break;

        case NYA_AGENT_KIND_COUNT:
        default: nya_unreachable();
    }
}

/*
 * ─────────────────────────────────────────────────────────
 * INTROSPECTION
 * ─────────────────────────────────────────────────────────
 */

f32 nya_agent_score(const NYA_Agent* agent) {
    nya_assert(agent != nullptr);
    return agent->score;
}

f32 nya_agent_score_best(const NYA_Agent* agent) {
    nya_assert(agent != nullptr);
    return agent->score_best;
}

void nya_agent_report(const NYA_Agent* agent) {
    nya_assert(agent != nullptr);

    switch (agent->kind) {
        case NYA_AGENT_KIND_RANDOM: {
            printf("  agent random: %llu choices, best score %.1f\n", (unsigned long long)agent->choices, (f64)agent->score_best);
        } break;

        case NYA_AGENT_KIND_DQN: {
            printf("  agent dqn: %llu choices, %llu gradient steps, exploration %.3f, loss %.4f, best score %.1f\n",
                   (unsigned long long)agent->choices, (unsigned long long)nya_nn_dqn_train_step_count(agent->dqn),
                   (f64)nya_nn_dqn_exploration(agent->dqn), (f64)nya_nn_dqn_average_loss(agent->dqn), (f64)agent->score_best);
        } break;

        case NYA_AGENT_KIND_NEAT: {
            printf("  agent neat: %llu choices, generation %u, %u species, fitness max %.1f average %.1f, best score %.1f\n",
                   (unsigned long long)agent->choices, nya_nn_neat_generation(agent->neat), nya_nn_neat_species_count(agent->neat),
                   nya_nn_neat_fitness_max(agent->neat), nya_nn_neat_fitness_average(agent->neat), (f64)agent->score_best);
        } break;

        case NYA_AGENT_KIND_COUNT:
        default: nya_unreachable();
    }
}

NYA_ConstCString nya_agent_kind_name(NYA_AgentKind kind) {
    nya_assert((u32)kind < (u32)NYA_AGENT_KIND_COUNT, "agent kind %u is not a kind", (u32)kind);

    return _NYA_AGENT_KIND_NAMES[kind];
}

NYA_AgentKind nya_agent_kind_from_name(NYA_ConstCString name) {
    nya_assert(name != nullptr);

    for (u32 i = 0; i < (u32)NYA_AGENT_KIND_COUNT; i++) {
        if (nya_string_equals(_NYA_AGENT_KIND_NAMES[i], name)) return (NYA_AgentKind)i;
    }

    // rejected by default: a misspelled kind is a caller's mistake to report, not a silent fallback to
    // the one kind that learns nothing.
    return NYA_AGENT_KIND_COUNT;
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

NYA_NeatNetwork* _nya_agent_neat_seed(NYA_Agent* agent) {
    nya_assert(agent != nullptr);

    // owned here: a network keeps the pointer it was handed, so the labels have to outlive every
    // genome bred from this seed, which is the whole run.
    agent->sensor_labels = nya_arena_alloc(agent->allocator, sizeof(char[NYA_AGENT_LABEL_MAX]) * agent->sense_count);
    agent->output_labels = nya_arena_alloc(agent->allocator, sizeof(char[NYA_AGENT_LABEL_MAX]) * agent->action_count);

    NYA_NeatNetwork* seed = nya_nn_neat_network_create(agent->allocator);

    nya_nn_neat_network_push_bias(seed, "bias");

    for (u32 i = 0; i < agent->sense_count; i++) {
        (void)snprintf(agent->sensor_labels[i], NYA_AGENT_LABEL_MAX, "s" FMTu32, i);
        nya_nn_neat_network_push_sensor(seed, agent->sensor_labels[i]);
    }

    for (u32 i = 0; i < agent->action_count; i++) {
        (void)snprintf(agent->output_labels[i], NYA_AGENT_LABEL_MAX, "a" FMTu32, i);
        nya_nn_neat_network_push_output(seed, agent->output_labels[i]);
    }

    return seed;
}

f64 _nya_agent_neat_trial(NYA_NeatNetwork* network) {
    NYA_Agent* agent = _NYA_AGENT_EVOLVING;

    nya_assert(agent != nullptr, "a NEAT trial ran with no agent evolving");
    nya_assert(agent->trial != nullptr, "a NEAT agent has no trial");

    NYA_NeatNetwork* previous = agent->network;

    agent->network = network;
    f32 score      = agent->trial(agent, agent->user_data);
    agent->network = previous;

    return (f64)score;
}

u32 _nya_agent_neat_choose(NYA_Agent* agent, NYA_Session* session, const f32* senses) {
    nya_assert(agent != nullptr);
    nya_assert(session != nullptr);

    // Before the first generation there is no genome yet and no trial has started: there is nothing to
    // ask, so the run's own stream answers.
    if (agent->network == nullptr) return (u32)nya_session_below(session, agent->action_count);

    // flushed so one tick's decision does not carry the last one's activations, which is what makes a
    // recurrent genome's output a function of this tick's senses and its own memory rather than of
    // when in the session it was asked.
    nya_nn_neat_network_flush(agent->network);

    for (u32 i = 0; i < agent->sense_count; i++) nya_nn_neat_network_set_sensor(agent->network, agent->sensor_labels[i], (f64)senses[i]);

    nya_nn_neat_network_run(agent->network);

    f64 best_value = nya_nn_neat_network_get_output(agent->network, agent->output_labels[0]);

    for (u32 i = 1; i < agent->action_count; i++) {
        f64 value = nya_nn_neat_network_get_output(agent->network, agent->output_labels[i]);
        if (value > best_value) best_value = value;
    }

    // Counted first and then drawn from, rather than kept as a list: the tied set is at most the
    // action count and counting it twice is cheaper than a buffer sized for the worst case.
    u32 tied = 0;
    for (u32 i = 0; i < agent->action_count; i++) {
        if (nya_nn_neat_network_get_output(agent->network, agent->output_labels[i]) >= best_value - _NYA_AGENT_TIE_EPSILON) tied++;
    }

    nya_assert(tied > 0, "no output reached the largest output");

    u32 wanted = (u32)nya_session_below(session, tied);

    for (u32 i = 0; i < agent->action_count; i++) {
        if (nya_nn_neat_network_get_output(agent->network, agent->output_labels[i]) < best_value - _NYA_AGENT_TIE_EPSILON) continue;
        if (wanted == 0) return i;

        wanted--;
    }

    // unreachable: the walk above passes `tied` outputs and `wanted` was drawn below that.
    nya_assert_always(false, "the tie walk ran off the end of the output table");
    __builtin_unreachable();
}

#endif // NYA_TESTING
