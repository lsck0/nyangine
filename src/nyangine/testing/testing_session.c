#include "SDL3/SDL_timer.h"

#include "nyangine/nyangine.h"

#ifdef NYA_TESTING

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API DECLARATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/**
 * The session the frame hook is driving, or null.
 *
 * A file-scope pointer because an event hook is a plain function pointer with no context of its own,
 * and because the application it plays is a singleton anyway: there is one frame loop, so there is one
 * session. Asserted on entry, rather than left to produce a null dereference inside a hook.
 * */
NYA_INTERNAL NYA_Session* _NYA_SESSION_PLAYING = nullptr;

/** The clock the app reads while a session runs. Advanced by exactly one tick per frame. */
NYA_INTERNAL u64 _nya_session_clock_ns(void);

/** The NYA_EVENT_FRAME_STARTED hook: advances the clock, observes, chooses, acts, and counts. */
NYA_INTERNAL void _nya_session_on_frame_started(NYA_Event* event);

/** The NYA_EVENT_UPDATING_STARTED hook: counts the fixed steps the application really ran. */
NYA_INTERNAL void _nya_session_on_updating_started(NYA_Event* event);

/** Which window synthetic input is addressed to. The session's, or the first the app has. */
NYA_INTERNAL NYA_WindowHandle _nya_session_window(const NYA_Session* session);

/** Remembers a key as held, or forgets it. Keeps `held` free of duplicates. */
NYA_INTERNAL void _nya_session_hold(NYA_Session* session, NYA_Keycode key, b8 down);

/** Folds one number into the run's digest. See NYA_Session.digest. */
NYA_INTERNAL void _nya_session_digest_fold(NYA_Session* session, u64 value);

/** Blocks until the real monotonic clock reaches `deadline_ns`. A real time session's whole cost. */
NYA_INTERNAL void _nya_session_wait_until(u64 deadline_ns);

/**
 * Lets go of every key and button the agent is holding, and makes the application see it.
 *
 * Pumped rather than only dispatched: a release left in the queue is a release the application has not
 * had yet, and the next thing to run the loop inherits a key that is still down. That is how two runs
 * of one seed came to differ in their first tick.
 * */
NYA_INTERNAL void _nya_session_release_all(NYA_Session* session);

/**
 * Puts the agent's pointer where the session says it is, so tick zero is tick zero.
 *
 * The input system remembers where the pointer was left, and a session observes at the top of the
 * frame, before that frame pumps its events: a placement merely dispatched would reach the input
 * system only after tick zero had already looked at it.
 * */
NYA_INTERNAL void _nya_session_start_from_known_state(NYA_Session* session);

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

NYA_Session* nya_session_create_with_options(NYA_SessionOptions options) {
    NYA_App* app = nya_app_get();

    nya_assert(app->initialized, "a session plays an application; bring one up with nya_app_init first");
    nya_assert(app->options.time_step_ns > 0, "the app has no fixed tick for a session to advance by");
    nya_assert(options.tick_count > 0, "a session of no ticks plays nothing");

    NYA_Arena* allocator = nya_arena_create(.name = "session");

    NYA_Session* session = nya_arena_alloc(allocator, sizeof(NYA_Session));

    *session = (NYA_Session){
        .allocator = allocator,
        .seed      = options.seed,
        .tick_count = options.tick_count,
        .verbose   = options.verbose,

        // the app's own step, so a session ticks at the rate the game does rather than at one this
        // file picked.
        .time_step_ns = app->options.time_step_ns,

        .real_time = options.real_time,

        .observe = options.observe,
        .policy  = options.policy != nullptr ? options.policy : nya_session_policy_random,
        .check   = options.check,

        .window    = options.window,
        .user_data = options.user_data,

        // seeded from the seed, so an empty run still has a digest that says which seed produced it.
        .digest = options.seed,
    };

    return session;
}

void nya_session_destroy(NYA_Session* session) {
    if (session == nullptr) return;

    // released rather than left down, for a session that was created and never run. The application
    // must still be up: a session plays one, so destroying it after the app has gone is out of order.
    _nya_session_release_all(session);

    NYA_Arena* allocator = session->allocator;
    nya_arena_destroy(allocator);
}

/*
 * ─────────────────────────────────────────────────────────
 * ACTIONS
 * ─────────────────────────────────────────────────────────
 */

void nya_session_action_add(NYA_Session* session, NYA_ConstCString name, u32 weight, NYA_SessionActionFn action) {
    nya_assert(session != nullptr);
    nya_assert(name != nullptr && name[0] != '\0');
    nya_assert(session->tick == 0, "actions are registered before the session starts, so the mix is fixed for the seed");

    nya_assert(session->action_count < NYA_SESSION_MAX_ACTIONS, "more than %d actions; raise NYA_SESSION_MAX_ACTIONS", NYA_SESSION_MAX_ACTIONS);
    nya_assert(nya_session_action_find(session, name) == NYA_SESSION_MAX_ACTIONS, "'%s' is registered twice", name);

    u64 weight_before = session->weight_total;

    session->actions[session->action_count++] = (NYA_SessionAction){ .name = name, .weight = weight, .run = action };
    session->weight_total                    += weight;

    nya_assert(session->weight_total == weight_before + weight, "the weight total drifted from the table");
}

u32 nya_session_action_find(const NYA_Session* session, NYA_ConstCString name) {
    nya_assert(session != nullptr);
    nya_assert(name != nullptr);

    for (u32 i = 0; i < session->action_count; i++) {
        if (nya_string_equals(session->actions[i].name, name)) return i;
    }

    return NYA_SESSION_MAX_ACTIONS;
}

/*
 * ─────────────────────────────────────────────────────────
 * THE SESSION
 * ─────────────────────────────────────────────────────────
 */

u32 nya_session_run(NYA_Session* session) {
    nya_assert(session != nullptr);
    nya_assert(session->action_count > 0, "a session with no actions would sit still");
    nya_assert(_NYA_SESSION_PLAYING == nullptr, "a session is already playing this application");

    NYA_App* app = nya_app_get();

    // seeded from the real clock: the loop has already written timestamps from it, and a simulated
    // clock starting at zero would make every difference against those wrap.
    session->clock_ns   = nya_clock_get_monotonic_ns();
    session->started_ns = session->clock_ns;

    printf("SESSION: seed 0x%016llX, %llu ticks, %u actions, %s policy, %s\n", (unsigned long long)session->seed,
           (unsigned long long)session->tick_count, session->action_count,
           session->policy == nya_session_policy_random ? "random" : "supplied", session->real_time ? "real time" : "fast forward");

    NYA_AppTimeSource previous = nya_app_time_source();

    _NYA_SESSION_PLAYING = session;

    // before the clock is installed, so none of it is charged to the run.
    _nya_session_start_from_known_state(session);

    /*
     * The catch-up cap is one either way, so a frame is a tick and the agent's one action per frame is
     * one action per tick. What the two modes differ in is only where the loop reads time: a simulated
     * clock the session steps itself and never sleeps against, or the real one with the frame paced to
     * it. Everything downstream of the tick is identical, which is what makes the digests comparable.
     */
    nya_app_time_source_set((NYA_AppTimeSource){
        .now_ns = session->real_time ? nullptr : _nya_session_clock_ns,

        // exactly one: the clock advances by one step per frame, so one tick is all there ever is to
        // pay, and a cap of one turns a mistake about that into a stall instead of a burst.
        .catch_up_ticks_max = 1,
        .never_sleep        = !session->real_time,
    });

    NYA_EventHook hook = {
        .event_type = NYA_EVENT_FRAME_STARTED,
        .hook_type  = NYA_EVENT_HOOK_TYPE_IMMEDIATE,
        .fn         = nya_callback(_nya_session_on_frame_started),
    };

    NYA_EventHook tick_hook = {
        .event_type = NYA_EVENT_UPDATING_STARTED,
        .hook_type  = NYA_EVENT_HOOK_TYPE_IMMEDIATE,
        .fn         = nya_callback(_nya_session_on_updating_started),
    };

    nya_event_hook_register(hook);
    nya_event_hook_register(tick_hook);

    b8 quitting_before = app->should_quit;

    // cleared so a session runs even when something asked the app to quit earlier; restored below.
    app->should_quit = false;

    nya_app_run();

    session->elapsed_ns = nya_clock_get_monotonic_ns() - session->started_ns;

    // before the run ends, not when the session is freed: a run that stopped mid-jump would otherwise
    // hand the application a key that is still down, and whatever runs the loop next inherits it.
    _nya_session_release_all(session);

    nya_event_hook_unregister(hook);
    nya_event_hook_unregister(tick_hook);
    nya_app_time_source_set(previous);

    /*
     * The assumption the whole harness rests on, checked rather than trusted: the agent acted once per
     * frame, so a frame has to have been a tick. See NYA_Session.ticks_run.
     */
    nya_assert(
        session->ticks_run == session->tick,
        "the agent acted on %llu frames but the application ran %llu fixed steps",
        (unsigned long long)session->tick,
        (unsigned long long)session->ticks_run
    );

    _NYA_SESSION_PLAYING = nullptr;
    app->should_quit     = quitting_before;

    /*
     * Simulated against real, because "how much faster than a player" is the number the whole facility
     * exists for, and a run that is not faster is a run with the clock wired wrong.
     */
    u64 simulated_ns = session->tick * session->time_step_ns;

    printf("  %llu ticks played, %.1f simulated seconds in %.1f real, %.0fx\n", (unsigned long long)session->tick, nya_time_ns_to_s(simulated_ns),
           nya_time_ns_to_s(session->elapsed_ns), session->elapsed_ns > 0 ? (f64)simulated_ns / (f64)session->elapsed_ns : 0.0);
    printf("  digest 0x%016llX\n", (unsigned long long)session->digest);

    /*
     * The mix, so an action the policy never chose is visible rather than counted as covered.
     */
    for (u32 i = 0; i < session->action_count; i++) {
        if (session->actions[i].taken == 0) printf("  NEVER TAKEN: %s\n", session->actions[i].name);
    }

    return session->failures;
}

u64 nya_session_digest(const NYA_Session* session) {
    nya_assert(session != nullptr);

    return session->digest;
}

void nya_session_fail(NYA_Session* session, NYA_ConstCString format, ...) {
    nya_assert(session != nullptr);
    nya_assert(format != nullptr);

    printf("  FAIL (seed 0x%016llX tick %llu, doing '%s'): ", (unsigned long long)session->seed, (unsigned long long)session->tick,
           session->chosen < session->action_count ? session->actions[session->chosen].name : "nothing");

    va_list arguments;
    va_start(arguments, format);
    (void)vprintf(format, arguments);
    va_end(arguments);

    printf("\n  REPLAY: ./build run simulation --session --seed 0x%016llX --steps %llu\n", (unsigned long long)session->seed,
           (unsigned long long)session->tick_count);

    session->failures++;
}

u32 nya_session_policy_random(NYA_Session* session, const f32* senses, u32 sense_count) {
    // a random policy is blind on purpose: it is the baseline a learned one has to beat, and the
    // fuzzing case where nobody is looking at anything.
    nya_unused(senses, sense_count);

    nya_assert(session->weight_total > 0, "every registered action has a weight of zero");

    u64 roll = nya_session_below(session, session->weight_total);

    for (u32 i = 0; i < session->action_count; i++) {
        if (roll < session->actions[i].weight) return i;
        roll -= session->actions[i].weight;
    }

    // unreachable: the roll is below the sum of the weights, so the walk consumes it before the end.
    nya_assert_always(false, "the weight walk ran off the end of the action table");
    __builtin_unreachable();
}

/*
 * ─────────────────────────────────────────────────────────
 * ACTING AS THE USER
 * ─────────────────────────────────────────────────────────
 */

void nya_session_key(NYA_Session* session, NYA_Keycode key, b8 down) {
    nya_assert(session != nullptr);

    _nya_session_hold(session, key, down);

    // dispatched, not written into the input system: this is the queue SDL's own events land in, so
    // the synthetic press goes through window handling, the input system and every layer in turn.
    nya_event_dispatch((NYA_Event){
        .type = down ? NYA_EVENT_KEY_DOWN : NYA_EVENT_KEY_UP,
        .as_key_event = {
            .window  = _nya_session_window(session),
            .is_down = down,
            .key     = key,
        },
    });
}

void nya_session_key_tap(NYA_Session* session, NYA_Keycode key) {
    nya_session_key(session, key, true);
    nya_session_key(session, key, false);
}

void nya_session_mouse_move(NYA_Session* session, f32x2 point) {
    nya_assert(session != nullptr);

    f32x2 from = session->cursor;
    session->cursor = point;

    nya_event_dispatch((NYA_Event){
        .type = NYA_EVENT_MOUSE_MOVED,
        .as_mouse_moved_event = {
            .window  = _nya_session_window(session),
            .x       = point.x,
            .y       = point.y,
            .delta_x = point.x - from.x,
            .delta_y = point.y - from.y,
        },
    });
}

void nya_session_mouse_button(NYA_Session* session, u8 button, b8 down) {
    nya_assert(session != nullptr);
    nya_assert(button < NYA_MOUSE_BUTTON_COUNT, "there is no mouse button %u", button);

    // tracked for the same reason held keys are: a run that ends mid-drag leaves the application
    // holding a button nobody is pressing, and whatever runs next inherits it.
    if (down) {
        session->held_buttons |= 1U << button;
    } else {
        session->held_buttons &= ~(1U << button);
    }

    nya_event_dispatch((NYA_Event){
        .type = down ? NYA_EVENT_MOUSE_BUTTON_DOWN : NYA_EVENT_MOUSE_BUTTON_UP,
        .as_mouse_button_event = {
            .window  = _nya_session_window(session),
            .is_down = down,
            .button  = button,
            .clicks  = 1,
            .x       = session->cursor.x,
            .y       = session->cursor.y,
        },
    });
}

void nya_session_mouse_click(NYA_Session* session, u8 button) {
    nya_session_mouse_button(session, button, true);
    nya_session_mouse_button(session, button, false);
}

void nya_session_wheel(NYA_Session* session, f32 amount) {
    nya_assert(session != nullptr);

    nya_event_dispatch((NYA_Event){
        .type = NYA_EVENT_MOUSE_WHEEL_MOVED,
        .as_mouse_wheel_event = {
            .window           = _nya_session_window(session),
            .amount_y         = amount,
            .integer_amount_y = (s32)amount,
            .mouse_x          = session->cursor.x,
            .mouse_y          = session->cursor.y,
        },
    });
}

/*
 * ─────────────────────────────────────────────────────────
 * ENTROPY
 * ─────────────────────────────────────────────────────────
 */

u64 nya_session_roll(NYA_Session* session) {
    nya_assert(session != nullptr);

    u64 coordinate[3] = { session->seed, session->tick, session->draw };
    session->draw++;

    return nya_siphash(coordinate, sizeof(coordinate), NYA_SIMULATION_HASH_KEY_LOW, NYA_SIMULATION_HASH_KEY_HIGH);
}

u64 nya_session_below(NYA_Session* session, u64 limit) {
    if (limit == 0) return 0;

    return nya_session_roll(session) % limit;
}

b8 nya_session_chance(NYA_Session* session, u32 percent) {
    nya_assert(percent <= 100, "a chance is a percentage, got %u", percent);

    return nya_session_below(session, 100) < percent;
}

f32 nya_session_range_f32(NYA_Session* session, f32 low, f32 high) {
    if (!(high > low)) return low;

    f32 unit = (f32)(nya_session_roll(session) & 0xFFFFFFULL) / (f32)0x1000000ULL;

    return low + ((high - low) * unit);
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

u64 _nya_session_clock_ns(void) {
    nya_assert(_NYA_SESSION_PLAYING != nullptr, "the session clock is installed with no session playing");

    return _NYA_SESSION_PLAYING->clock_ns;
}

void _nya_session_on_frame_started(NYA_Event* event) {
    nya_unused(event);

    NYA_Session* session = _NYA_SESSION_PLAYING;
    nya_assert(session != nullptr, "the session frame hook ran with no session playing");

    if (session->tick >= session->tick_count) {
        nya_app_get()->should_quit = true;
        return;
    }

    if (session->real_time) {
        /*
         * Paced against the frame the loop last started rather than against a schedule laid out from
         * the first tick: what has to hold is that this frame owes exactly one tick, and measuring
         * from the loop's own last frame is what makes that true however long the frame before took.
         * An absolute schedule drifts, and a frame that owes nothing is a frame the agent acted in
         * for no tick at all.
         */
        _nya_session_wait_until(nya_app_get()->frame_stats.prev_frame_time_ns + session->time_step_ns);
    } else {
        // the whole fast forward, in one line: the loop books exactly one tick of debt per frame, pays
        // it and comes straight back, so the session runs at whatever rate the CPU manages.
        session->clock_ns += session->time_step_ns;
    }

    // reset per tick, so a draw's coordinate is (seed, tick, index within the tick) and adding a draw
    // inside one action cannot shift every later tick's decisions.
    session->draw = 0;

    session->sense_count = 0;
    if (session->observe != nullptr) {
        session->sense_count = session->observe(session, session->senses, NYA_SESSION_MAX_SENSES);

        nya_assert(session->sense_count <= NYA_SESSION_MAX_SENSES, "an observation wrote %u senses past the %d it was given",
                   session->sense_count, NYA_SESSION_MAX_SENSES);
    }

    session->chosen = session->policy(session, session->senses, session->sense_count);

    // an agent that picks an action that does not exist is a bug in whatever maps its outputs, not an
    // input to be handled.
    nya_assert(session->chosen < session->action_count, "the policy chose action %u of %u", session->chosen, session->action_count);

    NYA_SessionAction* action = &session->actions[session->chosen];

    if (session->verbose) printf("  tick %llu: %s\n", (unsigned long long)session->tick, action->name);

    /*
     * Folded before the action runs: what is being recorded is the decision, which is the thing two
     * runs of one seed have to agree on, together with the world the decision was made from.
     */
    _nya_session_digest_fold(session, session->tick);
    _nya_session_digest_fold(session, session->chosen);

    for (u32 i = 0; i < session->sense_count; i++) {
        u32 bits = 0;
        nya_memcpy(&bits, &session->senses[i], sizeof(bits));
        _nya_session_digest_fold(session, bits);
    }

    action->taken++;

    // null is a legal action: a player idles most ticks.
    if (action->run != nullptr) action->run(session);

    if (session->check != nullptr) session->check(session);

    session->tick++;
}

void _nya_session_on_updating_started(NYA_Event* event) {
    nya_unused(event);

    NYA_Session* session = _NYA_SESSION_PLAYING;
    nya_assert(session != nullptr, "the session tick hook ran with no session playing");

    session->ticks_run++;
}

NYA_WindowHandle _nya_session_window(const NYA_Session* session) {
    if (session->window.generation != 0) return session->window;

    for (u32 slot = 0; slot < NYA_WINDOW_MAX; slot++) {
        NYA_Window* window = nya_window_at_slot(slot);
        if (window != nullptr) return window->handle;
    }

    // a headless session with no window is legal; the event still reaches the input system and the
    // layers, which is where most of what an agent does is decided.
    return NYA_WINDOW_HANDLE_NONE;
}

void _nya_session_hold(NYA_Session* session, NYA_Keycode key, b8 down) {
    for (u32 i = 0; i < session->held_count; i++) {
        if (session->held[i] != key) continue;

        // already held: a repeat press is a real thing, so only a release changes the table.
        if (down) return;

        session->held[i] = session->held[session->held_count - 1];
        session->held_count--;
        return;
    }

    if (!down) return;

    // bounded by the action table: an agent cannot hold more keys than it has actions to press them
    // with, and a session that reached this would be leaking presses.
    nya_assert(session->held_count < NYA_SESSION_MAX_ACTIONS, "the session is holding more keys than it has actions");

    session->held[session->held_count++] = key;
}

void _nya_session_digest_fold(NYA_Session* session, u64 value) {
    nya_assert(session != nullptr);

    // Order matters and has to: two runs that took the same actions in a different order are two
    // different runs. The previous digest is hashed in with the value, so it is a chain rather than a
    // sum. The same siphash the draws use, so the facility carries one hash and not two.
    u64 coordinate[2] = { session->digest, value };

    session->digest = nya_siphash(coordinate, sizeof(coordinate), NYA_SIMULATION_HASH_KEY_LOW, NYA_SIMULATION_HASH_KEY_HIGH);
}

void _nya_session_release_all(NYA_Session* session) {
    nya_assert(session != nullptr);

    if (session->held_count == 0 && session->held_buttons == 0) return;

    while (session->held_count > 0) nya_session_key(session, session->held[session->held_count - 1], false);

    for (u8 button = 0; button < NYA_MOUSE_BUTTON_COUNT; button++) {
        if ((session->held_buttons & (1U << button)) != 0) nya_session_mouse_button(session, button, false);
    }

    nya_app_events_pump();
}

void _nya_session_start_from_known_state(NYA_Session* session) {
    nya_assert(session != nullptr);

    _nya_session_release_all(session);

    nya_session_mouse_move(session, session->cursor);

    // through the loop's own routing, so the placement takes the path a real move takes.
    nya_app_events_pump();
}

void _nya_session_wait_until(u64 deadline_ns) {
    u64 now = nya_clock_get_monotonic_ns();
    if (now >= deadline_ns) return;

    // SDL's delay rather than a spin: a real time session is idle by definition, and the point of it
    // is to be slow in the same way a player is, not to burn a core proving it.
    SDL_DelayNS(deadline_ns - now);
}

#endif // NYA_TESTING
