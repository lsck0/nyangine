/**
 * @file agent.h
 *
 * gnyame played by a network: what the agent may press, what it can see, and what a good run looks
 * like.
 *
 * The engine owns the harness (testing_session.h) and the learners (testing_agent.h). This is the
 * part only the game knows: that up and down move a menu, that Return is confirm, that being in a
 * scene is worth more than sitting on the title screen, and that a layer stack with nothing on it is
 * a bug. Everything the agent does goes through nya_session_*, which dispatches into the same event
 * queue a keyboard does, so the game cannot tell it apart from a person.
 *
 * Overview:
 *   gny_agent_run      the whole thing: an agent, a number of episodes, and a report
 *   gny_agent_play     one episode, which is one seeded session
 *   gny_agent_trial    the same, shaped as the callback a NEAT generation calls per genome
 *
 * ```c
 * u32 failures = gny_agent_run((GNY_AgentRun){ .kind = NYA_AGENT_KIND_DQN, .seed = seed, .episodes = 20 });
 * ```
 *
 * Compiled only under NYA_TESTING, like the harness it drives: a shipping build has no business
 * carrying a training loop.
 * */
#pragma once

#ifdef NYA_TESTING

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * CONSTANTS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/** Numbers the agent is handed about the application. See gny_agent_observe. */
#define GNY_AGENT_SENSES 8

/** Things the agent may do. See _GNY_AGENT_ACTIONS. */
#define GNY_AGENT_ACTIONS 12

/** Ticks one episode plays when the caller names none. About four simulated minutes at 62.5 Hz. */
#define GNY_AGENT_TICKS_DEFAULT 15000

/** Episodes one run plays when the caller names none. */
#define GNY_AGENT_EPISODES_DEFAULT 8

/**
 * What reaching a different arrangement of the layer stack is worth.
 *
 * The meta-objective: an agent paid for changing the screen learns the menus, which is where a crash
 * nobody looks for lives. Paid once per change rather than per tick, so sitting in a scene is not a
 * way to farm it.
 * */
#define GNY_AGENT_REWARD_SCREEN 2.0F

/** Per tick spent in a scene rather than in a menu. Small: it is a tiebreak, not the objective. */
#define GNY_AGENT_REWARD_IN_SCENE 0.01F

/** Per tick on the same screen once it has been there GNY_AGENT_STUCK_TICKS. Boredom, priced. */
#define GNY_AGENT_PENALTY_STUCK 0.02F

/** How long the same screen stops being exploration and starts being stuck. Ten simulated seconds. */
#define GNY_AGENT_STUCK_TICKS 600

/**
 * Layers the main window's stack may hold at once.
 *
 * Background, a scene, its HUD and a menu is four. Anything past this is a screen change that pushed
 * without popping, which is the bug this bound exists to catch rather than a size to raise.
 * */
#define GNY_AGENT_STACK_MAX 6

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * TYPES
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

typedef struct GNY_AgentRun GNY_AgentRun;

struct GNY_AgentRun {
    NYA_AgentKind kind;

    /** What the run is derived from. Every episode's seed is this one plus the episode number. */
    u64 seed;

    /** Episodes, or NEAT generations. Zero means GNY_AGENT_EPISODES_DEFAULT. */
    u32 episodes;

    /** Ticks per episode. Zero means GNY_AGENT_TICKS_DEFAULT. */
    u64 tick_count;

    /** Genomes per NEAT generation. Zero means the agent's own default. */
    u32 population;

    /** Print every action as it is taken. */
    b8 verbose;
};

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * FUNCTIONS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/**
 * Plays `episodes` sessions with one agent, learning between them, and returns the failures found.
 *
 * The application must already be up, with the main window and its layer stack: this plays a game, it
 * does not build one. A non-zero return is a bug with a seed attached; nya_session_fail has already
 * printed the command that replays it.
 * */
u32 gny_agent_run(GNY_AgentRun run) __attr_no_discard;

/** One episode: one seeded session over the real application, with `agent` choosing. Returns failures. */
u32 gny_agent_play(NYA_Agent* agent, u64 seed, u64 tick_count, b8 verbose) __attr_no_discard;

/** NYA_AgentTrialFn: plays one episode and returns what it scored, for a NEAT generation. */
f32 gny_agent_trial(NYA_Agent* agent, void* user_data);

#endif // NYA_TESTING
