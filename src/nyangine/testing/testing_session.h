/**
 * @file testing_session.h
 *
 * A played session: the real application, driven headless with no wall clock wait, by an agent that
 * presses keys and moves the mouse through the same queue a player's hardware pushes into.
 *
 * Overview:
 *   nya_session_create / _destroy    a session, from a seed and a tick count
 *   nya_session_action_add           one thing the agent may do, by name
 *   nya_session_run                  runs the app's own frame loop to the tick count
 *   nya_session_is_playing           whether one is running right now
 *   nya_session_digest               one number standing for everything the run did
 *   nya_session_key / _mouse_* /
 *     _wheel                         what an action calls to act as the user
 *   nya_session_policy_random        the default policy, and the shape a learned one has
 *
 * ```c
 * NYA_Session* session = nya_session_create(.seed = seed, .tick_count = 200000);
 *
 * nya_session_action_add(session, "left",  30, press_left);
 * nya_session_action_add(session, "right", 30, press_right);
 * nya_session_action_add(session, "jump",  10, press_jump);
 * nya_session_action_add(session, "click", 20, click_somewhere);
 * nya_session_action_add(session, "idle",  40, nullptr);
 *
 * u32 failures = nya_session_run(session);
 * nya_session_destroy(session);
 * ```
 *
 * ## Fast forward
 *
 * The frame loop is wall clock driven: it books the real nanoseconds since the last frame as debt and
 * pays it off one fixed tick at a time. A session installs an NYA_AppTimeSource whose clock it
 * advances by exactly one tick per frame and which never sleeps, so the loop books exactly one tick,
 * runs it, and comes straight back. The session then runs at whatever rate the CPU manages, which on
 * a headless build is three to four orders of magnitude past a player, and it runs the same number of
 * ticks whatever the machine.
 *
 * That is also why the tick count and not a duration is what a session takes: "a hundred thousand
 * ticks" is the same run everywhere, and "half an hour" is not.
 *
 * ## Why it is the same run either way
 *
 * A fast-forwarded run is only worth anything if it finds the bugs a real one would, which means it
 * has to be the same run. Three things make it one, and each is load bearing:
 *
 *   - The tick is a fixed timestep. Every system in the tick phase is handed NYA_AppOptions.time_step_ns
 *     whatever the wall clock did, so the clock changes how often a tick happens and never what one does.
 *   - Every draw the agent makes is siphash(seed, tick, draw index) rather than a stateful generator,
 *     so the action at tick N is a function of the seed and N alone.
 *   - The clock advances by exactly one step per frame and the catch-up cap is one, so a frame is a
 *     tick. The agent acts once per frame, which is therefore once per tick, in the same place in the
 *     frame a real player's input lands: dispatched at NYA_EVENT_FRAME_STARTED and handled by the
 *     event pump before the tick runs.
 *
 * `real_time` is the proof rather than a feature: it paces the same session to the wall clock, one
 * step per frame, and leaves everything else alone. Same seed, same digest, and the only difference is
 * how long it took. See tests/nyangine/testing/test_session.c.
 *
 * ## Acting as the user
 *
 * An action injects an NYA_Event through nya_event_dispatch, which is the same queue
 * nya_system_event_drain_sdl_events pushes SDL's events into and the same one the frame loop polls.
 * The event therefore reaches window handling, then the input system, then each layer's on_event, in
 * the order and at the point a real one would. Nothing here calls a game function directly, so what
 * the agent can reach is exactly what a player can reach.
 *
 * Rejected: setting NYA_InputSystem's key table directly. It is two lines shorter and it skips
 * window focus, the text input path, every layer that handles an event before the input system sees
 * it, and the UI's own hit testing, which is most of what a session is trying to exercise.
 *
 * ## The policy seam
 *
 * Which action runs at a tick comes from the session's policy. The default is a weighted draw from
 * the seed, which is the fuzzing case. A learned policy is the same function with a network behind
 * it: it is handed the sense vector the scenario filled and returns an action index, so the nn/ NEAT
 * and DQN code can drive the game without this file knowing they exist. See NYA_SessionPolicyFn.
 * */
#pragma once

#ifdef NYA_TESTING

#include "nyangine/base/base_attributes.h"
#include "nyangine/base/base_types.h"
#include "nyangine/core/core_keys.h"
#include "nyangine/core/core_types.h"
#include "nyangine/testing/testing_simulation.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * CONSTANTS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/** Actions one session may register. A player has a dozen buttons; this is room for four games' worth. */
#ifndef NYA_SESSION_MAX_ACTIONS
#define NYA_SESSION_MAX_ACTIONS 48
#endif

/**
 * Numbers a policy is handed about the world. Enough for a position, a velocity, a few distances and
 * some flags, which is what the robots in gnyame already learn from.
 * */
#ifndef NYA_SESSION_MAX_SENSES
#define NYA_SESSION_MAX_SENSES 32
#endif

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * TYPES
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

typedef struct NYA_Session        NYA_Session;
typedef struct NYA_SessionAction  NYA_SessionAction;
typedef struct NYA_SessionOptions NYA_SessionOptions;

/** One thing the agent may do. Injects input; it must not reach into the game directly. */
typedef void (*NYA_SessionActionFn)(NYA_Session* session);

/**
 * Fills `out_senses` with what the agent can perceive, and returns how many it wrote.
 *
 * The scenario's job, because only the game knows what a player can see. Null means a blind session,
 * which is what a random policy needs and what fuzzing uses.
 * */
typedef u32 (*NYA_SessionObserveFn)(NYA_Session* session, OUT f32* out_senses, u32 capacity);

/**
 * Chooses which registered action runs this tick, given what was observed.
 *
 * This is the whole seam between a fuzzing session and a learned one. nya_session_policy_random draws
 * from the seed; a NEAT network or a DQN reads `senses`, runs forward, and returns the index of its
 * highest output. Returning an index past `session->action_count` is a programmer error and asserted,
 * because an agent that picks an action that does not exist is a bug in the mapping, not an input to
 * handle.
 * */
typedef u32 (*NYA_SessionPolicyFn)(NYA_Session* session, const f32* senses, u32 sense_count);

/** An invariant checked at the end of every tick, alongside the engine's own assertions. */
typedef void (*NYA_SessionCheckFn)(NYA_Session* session);

struct NYA_SessionAction {
    NYA_ConstCString name;

    /** Relative frequency, for the random policy. A learned policy ignores it. */
    u32 weight;

    /** Null is a legal action: doing nothing for a tick is something a player does constantly. */
    NYA_SessionActionFn run;

    u64 taken;
};

struct NYA_SessionOptions {
    /** What the whole session is derived from. Enough to replay it exactly. */
    u64 seed;

    /** Ticks to play. Not seconds: a tick count is the same run on every machine. */
    u64 tick_count;

    /** Fills the sense vector. Null for a blind session. */
    NYA_SessionObserveFn observe;

    /** Chooses an action. Null means nya_session_policy_random. */
    NYA_SessionPolicyFn policy;

    /** Checked at the end of every tick. Null for none. */
    NYA_SessionCheckFn check;

    /** Which window synthetic input is addressed to. Zero means the first one the app has. */
    NYA_WindowHandle window;

    /** Printed as it plays. Off by default. */
    b8 verbose;

    /**
     * Plays at the rate a person would, one tick per wall clock step, instead of as fast as the CPU
     * allows.
     *
     * Only a test wants this: it is how a fast-forwarded run is shown to be the same run as a real
     * time one, by playing the same seed both ways and comparing nya_session_digest. Nothing else
     * changes, so the comparison is of the clock and of nothing else.
     * */
    b8 real_time;

    /** The scenario's own state, reachable from every action. The harness never looks inside. */
    void* user_data;
};

#define _NYA_SESSION_DEFAULT_OPTIONS .tick_count = 10000

struct NYA_Session {
    NYA_Arena* allocator;

    u64 seed;
    u64 tick_count;
    b8  verbose;
    b8  real_time;

    /** Ticks played so far. The coordinate every draw is hashed under. */
    u64 tick;

    /**
     * Fixed steps the application actually ran, counted independently of `tick`.
     *
     * A session acts once per frame and assumes a frame is a tick, which is what the clock and the
     * catch-up cap are arranged to guarantee. The two are counted separately and compared at the end
     * of the run so the assumption is checked rather than trusted: a run where they disagree acted on
     * frames that ran no tick, which is a different run from the one the seed names.
     * */
    u64 ticks_run;

    /** Draws taken during this tick, reset at the top of each. */
    u64 draw;

    /**
     * The simulated clock the app reads, in nanoseconds. Advanced by exactly one tick per frame.
     *
     * Seeded from the real clock when the session installs itself, so every difference the frame loop
     * takes against a timestamp written before the session started stays positive.
     * */
    u64 clock_ns;

    /** Nanoseconds one tick advances the clock by. Read from the app's own step at create time. */
    u64 time_step_ns;

    /** Real monotonic nanoseconds at the first tick, which a real time run paces each tick against. */
    u64 started_ns;

    /** Real monotonic nanoseconds the run took, so a caller can say how much faster than real it was. */
    u64 elapsed_ns;

    /**
     * Everything the run did, folded into one number: per tick, the tick index, the action chosen and
     * every sense the scenario reported.
     *
     * The comparable result of a session. Two runs of one seed agree on it or the run was not
     * deterministic, and that is the whole claim fast forwarding rests on. Senses are folded as their
     * bit patterns, so a float that differs in its last place is a different digest rather than a
     * rounding question.
     * */
    u64 digest;

    NYA_SessionAction actions[NYA_SESSION_MAX_ACTIONS];
    u32               action_count;
    u64               weight_total;

    NYA_SessionObserveFn observe;
    NYA_SessionPolicyFn  policy;
    NYA_SessionCheckFn   check;

    /** What the last observation saw. Kept so a failure can print what the agent was looking at. */
    f32 senses[NYA_SESSION_MAX_SENSES];
    u32 sense_count;

    /** The action the policy chose this tick, as an index. */
    u32 chosen;

    NYA_WindowHandle window;

    /** Where the synthetic cursor is, so a move is relative to somewhere rather than to nothing. */
    f32x2 cursor;

    /** Keys currently held by the agent, so a session never leaves one stuck down. */
    NYA_Keycode held[NYA_SESSION_MAX_ACTIONS];
    u32         held_count;

    /** The same for the mouse, as one bit per button. */
    u32 held_buttons;

    u32 failures;

    void* user_data;
};

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * FUNCTIONS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/*
 * ─────────────────────────────────────────────────────────
 * LIFETIME
 * ─────────────────────────────────────────────────────────
 */

#define nya_session_create(...) nya_session_create_with_options((NYA_SessionOptions){ _NYA_SESSION_DEFAULT_OPTIONS, __VA_ARGS__ })

/** The app must already be initialized: a session plays the application, it does not build one. */
NYA_API NYA_Session* nya_session_create_with_options(NYA_SessionOptions options) __attr_no_discard;

/** Releases every key the agent still holds and frees the session. A no-op on null. */
NYA_API void nya_session_destroy(NYA_Session* session);

/*
 * ─────────────────────────────────────────────────────────
 * ACTIONS
 * ─────────────────────────────────────────────────────────
 */

/** Registers one thing the agent may do. A null `action` is a legal registration meaning "do nothing". */
NYA_API void nya_session_action_add(NYA_Session* session, NYA_ConstCString name, u32 weight, NYA_SessionActionFn action);

/** The index of the action called `name`, or NYA_SESSION_MAX_ACTIONS. For a policy that maps by name. */
NYA_API u32 nya_session_action_find(const NYA_Session* session, NYA_ConstCString name) __attr_no_discard;

/*
 * ─────────────────────────────────────────────────────────
 * THE SESSION
 * ─────────────────────────────────────────────────────────
 */

/**
 * Installs the simulated clock, runs the application's own frame loop for `tick_count` ticks, and puts
 * the clock back. Returns the failures recorded.
 *
 * The loop is nya_app_run's, unmodified: the session hooks NYA_EVENT_FRAME_STARTED to advance its
 * clock, observe, choose and act, and asks the app to quit once the count is reached. Nothing here
 * re-implements a frame, so a session exercises the real order rather than one invented beside it.
 * */
NYA_API u32 nya_session_run(NYA_Session* session);

/**
 * Whether a session is driving the application's frame loop right now.
 *
 * There is one frame loop, so there is one session. Anything that would run a frame of its own, a
 * round of learning that plays a session per genome above all, asks this first rather than finding
 * out from the assertion inside nya_session_run.
 * */
NYA_API b8 nya_session_is_playing(void) __attr_no_discard;

/** Records a failure and prints the seed, tick and the replay command. */
NYA_API void nya_session_fail(NYA_Session* session, NYA_ConstCString format, ...) __attr_fmt_printf(2, 3);

/**
 * One number standing for everything the run did; see NYA_Session.digest.
 *
 * Two runs of the same seed must agree on it whatever the machine and whatever the clock, which is
 * what makes a failing seed a bug report and a fast-forwarded run worth running.
 * */
NYA_API u64 nya_session_digest(const NYA_Session* session) __attr_no_discard;

/** The default policy: a weighted draw from the seed. The fuzzing case, and the baseline a learned one beats. */
NYA_API u32 nya_session_policy_random(NYA_Session* session, const f32* senses, u32 sense_count);

/*
 * ─────────────────────────────────────────────────────────
 * ACTING AS THE USER
 * ─────────────────────────────────────────────────────────
 */

/** Presses a key, or releases it. Held keys are tracked, so a session never leaves one down. */
NYA_API void nya_session_key(NYA_Session* session, NYA_Keycode key, b8 down);

/** Presses and releases a key within the same tick. What a menu keypress is. */
NYA_API void nya_session_key_tap(NYA_Session* session, NYA_Keycode key);

/** Moves the synthetic cursor to a window point and dispatches the move. */
NYA_API void nya_session_mouse_move(NYA_Session* session, f32x2 point);

/** Presses or releases a mouse button where the cursor is. */
NYA_API void nya_session_mouse_button(NYA_Session* session, u8 button, b8 down);

/** Presses and releases a mouse button where the cursor is. */
NYA_API void nya_session_mouse_click(NYA_Session* session, u8 button);

/** Turns the wheel. */
NYA_API void nya_session_wheel(NYA_Session* session, f32 amount);

/*
 * ─────────────────────────────────────────────────────────
 * ENTROPY
 * ─────────────────────────────────────────────────────────
 */

/** The next draw, hashed from (seed, tick, draw). Same construction and same reasoning as a simulation's. */
NYA_API u64 nya_session_roll(NYA_Session* session);
NYA_API u64 nya_session_below(NYA_Session* session, u64 limit);
NYA_API b8  nya_session_chance(NYA_Session* session, u32 percent);
NYA_API f32 nya_session_range_f32(NYA_Session* session, f32 low, f32 high);

#endif // NYA_TESTING
