#include "gnyame/gnyame.h"

#ifdef NYA_TESTING

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * TYPES
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/** What one episode is keeping track of. Reachable from every action through NYA_Session.user_data. */
typedef struct {
    NYA_Agent* agent;

    /** The stack arrangement as of the last tick, so a change is noticed rather than polled for. */
    char screen[GNY_AGENT_STACK_MAX * NYA_LAYER_ID_MAX];

    u64 screen_since_tick;
    u32 screen_changes;

    /** Times the agent worked its way to the quit item. Counted, then refused; see gny_agent_check. */
    u32 quits_refused;
} GNY_AgentEpisode;

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API DECLARATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/**
 * The episode being played, or null.
 *
 * An event hook is a plain function with no context of its own, and there is one frame loop and so
 * one episode. Asserted on entry rather than left to dereference null.
 * */
NYA_INTERNAL GNY_AgentEpisode* _GNY_AGENT_EPISODE = nullptr;

/** The main window's layer stack, joined, which is this game's whole notion of "which screen". */
NYA_INTERNAL void _gny_agent_screen(OUT char* out, u64 capacity);

/**
 * Takes back the request to quit, at the one point in the frame where it has landed and the loop has
 * not yet read it.
 *
 * Quitting is something a player can do from the title screen, and a session that the thing it is
 * testing can stop ends on its first lucky guess. A screen request is applied at the barrier, which is
 * inside the tick, and nya_app_run tests should_quit at the top of the next frame, before any hook the
 * agent has runs: NYA_EVENT_UPDATING_ENDED is dispatched between those two and is the only place this
 * fits. Refusing it from the session's own check lost about a third of a run.
 * */
NYA_INTERNAL void _gny_agent_on_updating_ended(NYA_Event* event);

/** Whether the top of the stack is a menu, a scene, or nothing the game knows about. */
NYA_INTERNAL b8 _gny_agent_stack_holds(NYA_ConstCString id) __attr_no_discard;

/*
 * The agent's hands. Every one of these is a press through nya_session_*, which dispatches into the
 * queue SDL pushes a real key into: window handling, the input system, then each layer's on_event.
 */
NYA_INTERNAL void _gny_agent_up(NYA_Session* session);
NYA_INTERNAL void _gny_agent_down(NYA_Session* session);
NYA_INTERNAL void _gny_agent_left(NYA_Session* session);
NYA_INTERNAL void _gny_agent_right(NYA_Session* session);
NYA_INTERNAL void _gny_agent_confirm(NYA_Session* session);
NYA_INTERNAL void _gny_agent_confirm_alternative(NYA_Session* session);
NYA_INTERNAL void _gny_agent_cancel(NYA_Session* session);
NYA_INTERNAL void _gny_agent_walk_left(NYA_Session* session);
NYA_INTERNAL void _gny_agent_walk_right(NYA_Session* session);
NYA_INTERNAL void _gny_agent_let_go(NYA_Session* session);
NYA_INTERNAL void _gny_agent_point_and_click(NYA_Session* session);

/** NYA_SessionObserveFn: what a player can see, as numbers. */
NYA_INTERNAL u32 _gny_agent_observe(NYA_Session* session, OUT f32* out_senses, u32 capacity);

/** NYA_SessionPolicyFn: hands the senses to the agent and takes back an action index. */
NYA_INTERNAL u32 _gny_agent_policy(NYA_Session* session, const f32* senses, u32 sense_count);

/** NYA_SessionCheckFn: the invariants, and the reward the agent learns from. */
NYA_INTERNAL void _gny_agent_check(NYA_Session* session);

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * CONSTANTS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/**
 * The whole vocabulary, in the order the agent's outputs are numbered.
 *
 * A name, a weight for the random baseline, and the press. The weights are a player's: most ticks are
 * spent holding a direction or doing nothing, and confirm is rare enough that a menu is read rather
 * than mashed through.
 * */
NYA_INTERNAL const struct {
    NYA_ConstCString    name;
    u32                 weight;
    NYA_SessionActionFn run;
} _GNY_AGENT_ACTIONS[GNY_AGENT_ACTIONS] = {
    { "up",          12, _gny_agent_up                  },
    { "down",        12, _gny_agent_down                },
    { "left",         6, _gny_agent_left                },
    { "right",        6, _gny_agent_right               },
    { "confirm",      8, _gny_agent_confirm             },
    { "confirm_alt",  3, _gny_agent_confirm_alternative },
    { "cancel",       6, _gny_agent_cancel              },
    { "walk_left",   10, _gny_agent_walk_left           },
    { "walk_right",  10, _gny_agent_walk_right          },
    { "let_go",       8, _gny_agent_let_go              },
    { "click",        6, _gny_agent_point_and_click     },
    { "idle",        25, nullptr                        },
};

static_assert(nya_carray_length(_GNY_AGENT_ACTIONS) == GNY_AGENT_ACTIONS, "the action table and the agent's output count have to agree");

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PUBLIC API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

u32 gny_agent_play(NYA_Agent* agent, u64 seed, u64 tick_count, b8 verbose) {
    nya_assert(agent != nullptr);

    GNY_AgentEpisode episode = { .agent = agent };
    _gny_agent_screen(episode.screen, sizeof(episode.screen));

    NYA_Session* session = nya_session_create(.seed       = seed,
                                              .tick_count = tick_count,
                                              .observe    = _gny_agent_observe,
                                              .policy     = _gny_agent_policy,
                                              .check      = _gny_agent_check,
                                              .window     = GNY_WINDOW_MAIN,
                                              .verbose    = verbose,
                                              .user_data  = &episode);
    defer nya_session_destroy(session);

    for (u32 i = 0; i < GNY_AGENT_ACTIONS; i++) {
        nya_session_action_add(session, _GNY_AGENT_ACTIONS[i].name, _GNY_AGENT_ACTIONS[i].weight, _GNY_AGENT_ACTIONS[i].run);
    }

    NYA_EventHook refuse_quit = {
        .event_type = NYA_EVENT_UPDATING_ENDED,
        .hook_type  = NYA_EVENT_HOOK_TYPE_IMMEDIATE,
        .fn         = nya_callback(_gny_agent_on_updating_ended),
    };

    nya_assert(_GNY_AGENT_EPISODE == nullptr, "an episode is already playing");

    _GNY_AGENT_EPISODE = &episode;
    nya_event_hook_register(refuse_quit);

    u32 failures = nya_session_run(session);

    nya_event_hook_unregister(refuse_quit);
    _GNY_AGENT_EPISODE = nullptr;

    // read before the terminal reward below, which closes the trial and starts the next one's sum at zero.
    f32 scored = nya_agent_score(agent);

    // the episode is over whatever the score: a terminal reward is what tells a DQN not to bootstrap
    // past the end of a run it will never see the next state of.
    nya_agent_reward(agent, 0.0F, true);

    printf("  scored %.1f, screens reached %u, quit refused %u, left on '%s'\n", (f64)scored, episode.screen_changes, episode.quits_refused, episode.screen);

    return failures;
}

f32 gny_agent_trial(NYA_Agent* agent, void* user_data) {
    nya_assert(agent != nullptr);
    nya_assert(user_data != nullptr);

    GNY_AgentRun* run = user_data;

    /*
     * A genome's seed is the run's, not a fresh one: every genome in a generation is judged on the
     * same session, so the fitness ordering is about the genomes rather than about which of them drew
     * the easier world.
     */
    u32 failures = gny_agent_play(agent, run->seed, run->tick_count, run->verbose);

    // failures are counted by the caller through the same sessions; a genome that found a bug is not
    // thereby a good genome, so it is scored on what it did and nothing else.
    nya_unused(failures);

    return nya_agent_score_best(agent);
}

u32 gny_agent_run(GNY_AgentRun run) {
    nya_assert((u32)run.kind < (u32)NYA_AGENT_KIND_COUNT, "agent kind %u is not a kind", (u32)run.kind);
    nya_assert(nya_window_is_valid(GNY_WINDOW_MAIN), "the agent plays the real application; bring up the window first");

    if (run.episodes == 0) run.episodes = GNY_AGENT_EPISODES_DEFAULT;
    if (run.tick_count == 0) run.tick_count = GNY_AGENT_TICKS_DEFAULT;

    NYA_Agent* agent = nya_agent_create(.kind         = run.kind,
                                        .sense_count  = GNY_AGENT_SENSES,
                                        .action_count = GNY_AGENT_ACTIONS,
                                        .seed         = run.seed,
                                        .population   = run.population,
                                        .trial        = gny_agent_trial,
                                        .user_data    = &run);
    defer nya_agent_destroy(agent);

    printf("AGENT: %s, seed 0x%016llX, %u episodes of %llu ticks\n", nya_agent_kind_name(run.kind), (unsigned long long)run.seed, run.episodes,
           (unsigned long long)run.tick_count);

    u32 failures = 0;

    for (u32 episode = 0; episode < run.episodes; episode++) {
        printf("EPISODE %u/%u\n", episode + 1, run.episodes);

        /*
         * A NEAT generation plays a session per genome from inside nya_agent_learn, so the episode
         * loop hands it the round and plays nothing itself. Everything else plays here and learns
         * afterwards. Same loop, one branch, because a generation is a different number of sessions.
         */
        if (run.kind == NYA_AGENT_KIND_NEAT) {
            nya_agent_learn(agent);
        } else {
            // the episode number is in the seed, so an episode is a different world each time and the
            // whole run still replays from one number.
            failures += gny_agent_play(agent, run.seed + episode, run.tick_count, run.verbose);
            nya_agent_learn(agent);
        }

        nya_agent_report(agent);
    }

    return failures;
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

void _gny_agent_on_updating_ended(NYA_Event* event) {
    nya_unused(event);

    GNY_AgentEpisode* episode = _GNY_AGENT_EPISODE;
    nya_assert(episode != nullptr, "the agent's tick hook ran with no episode playing");

    if (!nya_app_get()->should_quit) return;

    nya_app_get()->should_quit = false;
    episode->quits_refused++;

    // put back where a player starts, so the run carries on from somewhere rather than from whatever
    // the stack was left as.
    gny_screen_request(GNY_SCREEN_MAIN_MENU);
}

void _gny_agent_screen(OUT char* out, u64 capacity) {
    nya_assert(out != nullptr);
    nya_assert(capacity > 0);

    out[0] = '\0';

    NYA_Window* window = nya_window_get(GNY_WINDOW_MAIN);
    if (window == nullptr) return;

    u64 length = 0;

    nya_array_foreach (window->layer_stack, layer) {
        if (length + 1 >= capacity) break;

        s32 written = snprintf(&out[length], capacity - length, "%s%s", length > 0 ? " " : "", layer->id);
        if (written > 0) length += (u64)written;
    }
}

b8 _gny_agent_stack_holds(NYA_ConstCString id) {
    NYA_Window* window = nya_window_get(GNY_WINDOW_MAIN);
    if (window == nullptr) return false;

    nya_array_foreach (window->layer_stack, layer) {
        if (nya_string_equals(layer->id, id)) return true;
    }

    return false;
}

void _gny_agent_up(NYA_Session* session) {
    nya_session_key_tap(session, NYA_KEY_UP);
}

void _gny_agent_down(NYA_Session* session) {
    nya_session_key_tap(session, NYA_KEY_DOWN);
}

void _gny_agent_left(NYA_Session* session) {
    nya_session_key_tap(session, NYA_KEY_LEFT);
}

void _gny_agent_right(NYA_Session* session) {
    nya_session_key_tap(session, NYA_KEY_RIGHT);
}

void _gny_agent_confirm(NYA_Session* session) {
    nya_session_key_tap(session, NYA_KEY_RETURN);
}

void _gny_agent_confirm_alternative(NYA_Session* session) {
    nya_session_key_tap(session, NYA_KEY_SPACE);
}

void _gny_agent_cancel(NYA_Session* session) {
    nya_session_key_tap(session, NYA_KEY_ESCAPE);
}

void _gny_agent_walk_left(NYA_Session* session) {
    // released first: holding both at once is a player leaning on the keyboard, and the scenario is
    // about what a player does on purpose.
    nya_session_key(session, NYA_KEY_D, false);
    nya_session_key(session, NYA_KEY_A, true);
}

void _gny_agent_walk_right(NYA_Session* session) {
    nya_session_key(session, NYA_KEY_A, false);
    nya_session_key(session, NYA_KEY_D, true);
}

void _gny_agent_let_go(NYA_Session* session) {
    nya_session_key(session, NYA_KEY_A, false);
    nya_session_key(session, NYA_KEY_D, false);
}

void _gny_agent_point_and_click(NYA_Session* session) {
    NYA_Window* window = nya_window_get(GNY_WINDOW_MAIN);

    f32 width  = window != nullptr ? (f32)window->screen_width : 1.0F;
    f32 height = window != nullptr ? (f32)window->screen_height : 1.0F;

    nya_session_mouse_move(session, (f32x2){ nya_session_range_f32(session, 0.0F, width), nya_session_range_f32(session, 0.0F, height) });
    nya_session_mouse_click(session, NYA_MOUSE_BUTTON_LEFT);
}

u32 _gny_agent_observe(NYA_Session* session, OUT f32* out_senses, u32 capacity) {
    nya_assert(capacity >= GNY_AGENT_SENSES);

    GNY_AgentEpisode* episode = session->user_data;

    NYA_Window* window = nya_window_get(GNY_WINDOW_MAIN);

    f32 width  = window != nullptr && window->screen_width > 0 ? (f32)window->screen_width : 1.0F;
    f32 height = window != nullptr && window->screen_height > 0 ? (f32)window->screen_height : 1.0F;

    f32x2 pointer = nya_input_mouse_position();

    out_senses[0] = _gny_agent_stack_holds(GNY_LAYER_MAIN_MENU_ID) ? 1.0F : 0.0F;
    out_senses[1] = _gny_agent_stack_holds(GNY_LAYER_PAUSE_MENU_ID) ? 1.0F : 0.0F;
    out_senses[2] = _gny_agent_stack_holds(GNY_LAYER_GAME_ID) ? 1.0F : 0.0F;
    out_senses[3] = _gny_agent_stack_holds(GNY_LAYER_CUBE3D_ID) ? 1.0F : 0.0F;

    // how long this screen has been up, saturating: the difference that matters is between "just
    // arrived" and "been here a while", not between six and seven hundred ticks.
    out_senses[4] = nya_clamp((f32)(session->tick - episode->screen_since_tick) / (f32)GNY_AGENT_STUCK_TICKS, 0.0F, 1.0F);

    out_senses[5] = nya_clamp(pointer.x / width, 0.0F, 1.0F);
    out_senses[6] = nya_clamp(pointer.y / height, 0.0F, 1.0F);
    out_senses[7] = nya_clamp((f32)episode->screen_changes / 32.0F, 0.0F, 1.0F);

    return GNY_AGENT_SENSES;
}

u32 _gny_agent_policy(NYA_Session* session, const f32* senses, u32 sense_count) {
    GNY_AgentEpisode* episode = session->user_data;

    return nya_agent_choose(episode->agent, session, senses, sense_count);
}

void _gny_agent_check(NYA_Session* session) {
    GNY_AgentEpisode* episode = session->user_data;

    NYA_Window* window = nya_window_get(GNY_WINDOW_MAIN);
    if (window == nullptr) return;

    /*
     * ── The oracle ──
     *
     * The engine's own assertions do most of it. These are the ones only the game can state.
     */
    if (window->layer_stack->length == 0) {
        nya_session_fail(session, "the main window has no layers left");
    }

    if (window->layer_stack->length > GNY_AGENT_STACK_MAX) {
        nya_session_fail(session, "the layer stack reached %llu layers: %s", (unsigned long long)window->layer_stack->length, episode->screen);
    }

    nya_array_foreach (window->layer_stack, layer) {
        if (layer->id[0] != '\0') continue;

        nya_session_fail(session, "a layer with no id is on the stack");
        break;
    }

    /*
     * ── The reward ──
     */
    char screen[GNY_AGENT_STACK_MAX * NYA_LAYER_ID_MAX];
    _gny_agent_screen(screen, sizeof(screen));

    f32 reward = 0.0F;

    if (!nya_string_equals(screen, episode->screen)) {
        (void)snprintf(episode->screen, sizeof(episode->screen), "%s", screen);
        episode->screen_since_tick = session->tick;
        episode->screen_changes++;

        reward += GNY_AGENT_REWARD_SCREEN;
    }

    if (_gny_agent_stack_holds(GNY_LAYER_GAME_ID) || _gny_agent_stack_holds(GNY_LAYER_CUBE3D_ID)) reward += GNY_AGENT_REWARD_IN_SCENE;

    if (session->tick - episode->screen_since_tick > GNY_AGENT_STUCK_TICKS) reward -= GNY_AGENT_PENALTY_STUCK;

    nya_agent_reward(episode->agent, reward, false);
}

#endif // NYA_TESTING
