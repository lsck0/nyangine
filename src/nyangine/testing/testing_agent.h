/**
 * @file testing_agent.h
 *
 * A learned action chooser for a played session: DQN or NEAT deciding what the user does next.
 *
 * testing_session.h leaves one seam, NYA_SessionPolicyFn, between "which action this tick" and the
 * rest of the harness. The default policy draws from the seed, which is the fuzzing case. This is the
 * same seam with a network behind it, so the two kinds in nn/ can play the application as a person
 * would: through the input queue, into whatever the game does with it, with the assertions still the
 * oracle and the seed still the replay.
 *
 * Overview:
 *   nya_agent_create / _destroy    an agent, from a kind and a seed
 *   nya_agent_choose               fits a session's policy: senses in, action index out
 *   nya_agent_reward               what the scenario thought of that choice
 *   nya_agent_learn                one round of learning: gradient steps, or a generation
 *   nya_agent_score                the trial in progress, and what a NEAT trial returns
 *   nya_agent_report               one line about where the agent has got to
 *   nya_agent_kind_name / _from_name  the two names a command line has to move between
 *
 * ```c
 * static u32 policy(NYA_Session* session, const f32* senses, u32 sense_count) {
 *     return nya_agent_choose(session->user_data, session, senses, sense_count);
 * }
 *
 * NYA_Agent* agent = nya_agent_create(.kind = NYA_AGENT_KIND_DQN, .sense_count = 8, .action_count = 6);
 *
 * NYA_Session* session = nya_session_create(.seed = seed, .tick_count = 20000, .observe = observe,
 *                                           .policy = policy, .check = check, .user_data = agent);
 * u32 failures = nya_session_run(session);
 * ```
 *
 * ## The two kinds learn at different scales, and the loop is the same either way
 *
 * A DQN learns inside one session: every reward is a transition in its replay buffer and a gradient
 * step, so a long run is a long education. A NEAT population learns across sessions: one genome plays
 * one session, its score is its fitness, and a generation is one session per genome. Both are driven
 * by the same three calls, and which one is behind them is a flag.
 *
 * `trial` is what a NEAT generation calls once per genome, and it is the scenario's own "play one
 * session" function. It is unused for a DQN, which has nothing to breed.
 *
 * ## Replay
 *
 * A failing run is still a seed, with one more thing to say: the environment replays from the
 * session's seed and the agent's decisions replay from the agent's, so a failure is reproduced by
 * running the same command again rather than by keeping the network that found it. Both seeds are
 * printed at the start of a run and by nya_session_fail.
 *
 * That holds only from the start of a run: a network is the sum of everything it has seen, so episode
 * forty cannot be replayed without episodes one to thirty-nine. Replay the run, not the episode.
 * */
#pragma once

#ifdef NYA_TESTING

#include "nyangine/base/base_attributes.h"
#include "nyangine/base/base_types.h"
#include "nyangine/testing/testing_session.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * CONSTANTS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/**
 * Genomes in a NEAT population, when the caller names none.
 *
 * The paper's 150 is a generation of 150 sessions, which is minutes of fast forwarding for one
 * generation and a slow way to learn anything about a game. Twenty-four still speciates and still
 * finds structure, and a generation is a few seconds.
 * */
#ifndef NYA_AGENT_POPULATION_DEFAULT
#define NYA_AGENT_POPULATION_DEFAULT 24
#endif

/**
 * Gradient steps nya_agent_learn runs for a DQN.
 *
 * A DQN also takes one step per reward while it plays, so this is the catch-up at the end of an
 * episode rather than the whole of its training.
 * */
#ifndef NYA_AGENT_LEARN_STEPS
#define NYA_AGENT_LEARN_STEPS 64
#endif

/** Longest label this generates for a sensor or an output: a letter and up to four digits. */
#define NYA_AGENT_LABEL_MAX 8

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * TYPES
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

typedef struct NYA_Agent        NYA_Agent;
typedef struct NYA_AgentOptions NYA_AgentOptions;
typedef enum NYA_AgentKind      NYA_AgentKind;

/** Which learner is behind the policy. `_COUNT` bounds the name table and is not a value. */
enum NYA_AgentKind {
    /** The seeded weighted draw, so "no agent" is an agent and the baseline is one flag away. */
    NYA_AGENT_KIND_RANDOM,

    NYA_AGENT_KIND_DQN,
    NYA_AGENT_KIND_NEAT,

    NYA_AGENT_KIND_COUNT,
};

/**
 * Plays one session with the agent as its policy and returns what it scored.
 *
 * What a NEAT generation calls once per genome. It must create and run a session, so it may not be
 * called from inside one: a generation happens between sessions, never during.
 * */
typedef f32 (*NYA_AgentTrialFn)(NYA_Agent* agent, void* user_data);

struct NYA_AgentOptions {
    NYA_AgentKind kind;

    /** Numbers the scenario's observe writes. Must match, and is asserted against, what it reports. */
    u32 sense_count;

    /** Actions the session registers. The agent chooses an index below this. */
    u32 action_count;

    /**
     * What the agent's own decisions are derived from, separate from the session's seed: one is the
     * environment, the other is the learner. Zero picks a fixed one, so a run with no seed given is
     * still a run that repeats.
     * */
    u64 seed;

    /** Genomes per generation, NEAT only. Zero means NYA_AGENT_POPULATION_DEFAULT. */
    u32 population;

    /** Plays one session, NEAT only. Required for NEAT, ignored otherwise. */
    NYA_AgentTrialFn trial;

    /** Passed to `trial` untouched. */
    void* user_data;
};

#define _NYA_AGENT_DEFAULT_OPTIONS .sense_count = 1, .action_count = 2

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * FUNCTIONS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

#define nya_agent_create(...) nya_agent_create_with_options((NYA_AgentOptions){ _NYA_AGENT_DEFAULT_OPTIONS, __VA_ARGS__ })

NYA_API NYA_Agent* nya_agent_create_with_options(NYA_AgentOptions options) __attr_no_discard;
NYA_API void       nya_agent_destroy(NYA_Agent* agent);

/**
 * Chooses which of the session's actions runs this tick, from what the scenario reported seeing.
 *
 * `session` is taken so a random agent draws from the run's own seeded stream rather than from a
 * generator of its own: the baseline has to replay like everything else.
 * */
NYA_API u32 nya_agent_choose(NYA_Agent* agent, NYA_Session* session, const f32* senses, u32 sense_count);

/**
 * What the scenario thought of the last choice, and whether the episode ended with it.
 *
 * Called once per choice, or not at all if the scenario has nothing to say: an agent given no reward
 * learns nothing and still plays, which is the fuzzing case with a network attached.
 * */
NYA_API void nya_agent_reward(NYA_Agent* agent, f32 reward, b8 terminal);

/**
 * One round of learning: a DQN takes gradient steps on its replay buffer, a NEAT population breeds a
 * generation, trialling every genome through `trial`.
 *
 * Asserts when a session is playing. A generation runs a session per genome, and a session inside a
 * session is a frame loop inside a frame loop.
 * */
NYA_API void nya_agent_learn(NYA_Agent* agent);

/** What the trial in progress has scored, summed from every reward since the last terminal one. */
NYA_API f32 nya_agent_score(const NYA_Agent* agent) __attr_no_discard;

/** The best score any trial has reached, so a run can say whether it is getting anywhere. */
NYA_API f32 nya_agent_score_best(const NYA_Agent* agent) __attr_no_discard;

/** One line about where the agent has got to: generations and species, or steps and exploration. */
NYA_API void nya_agent_report(const NYA_Agent* agent);

/** "random", "dqn" or "neat". Asserts on anything else. */
NYA_API NYA_ConstCString nya_agent_kind_name(NYA_AgentKind kind) __attr_no_discard;

/** The kind called `name`, or NYA_AGENT_KIND_COUNT for a name that is none of them. */
NYA_API NYA_AgentKind nya_agent_kind_from_name(NYA_ConstCString name) __attr_no_discard;

#endif // NYA_TESTING
