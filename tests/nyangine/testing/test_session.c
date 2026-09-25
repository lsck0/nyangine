/**
 * Fast forward: a played session run as fast as the CPU allows is the same run as one paced to the
 * wall clock.
 *
 * The whole value of fast forwarding rests on that. A run that covers a thousand times the ground a
 * player does is worth nothing if the ground is different ground, so the same seed is played twice at
 * two rates and the two digests have to be the same number. The agent moves a position through
 * nothing but synthetic key presses, so what the digest covers is a real path from the event queue,
 * through window handling and the input system, into a system in the tick phase.
 **/

#include "nyangine-core/nyangine.c"
#include "nyangine-core/nyangine.h"

#include "SDL3/SDL_init.h"

#define WINDOW_WIDTH  1280
#define WINDOW_HEIGHT 720

/** Ticks a fast-forwarded run plays. Long enough for the agent's path to be a path and not a step. */
#define FAST_TICKS 4000

/**
 * Ticks the real time comparison plays.
 *
 * It costs a wall clock second per sixty-two of them, so this is the largest number that keeps the
 * test under three seconds while still being long enough for the two runs to disagree if they were
 * ever going to. The fast-forwarded run above is the one that covers ground.
 * */
#define REAL_TICKS 120

/**
 * How much of the speed-up the test insists on seeing.
 *
 * Measured at about 825x on the dev machine, in the debug build, under four sanitizers and with other
 * work running beside it. Twenty is a floor that a loaded machine cannot fail and a loop still reading
 * the wall clock cannot reach: what this catches is the simulated clock not being installed at all.
 * */
#define MINIMUM_SPEED_UP 20.0

/** Units per second the agent's position moves while it holds a key. */
#define PLAYER_SPEED 240.0F

/** How far from the origin the position may get before the check calls it a bug. */
#define PLAYER_BOUND 100000.0F

/** Moved only by what the input system reports, so it is a pure function of the agent's presses. */
static f32x2 player_position = { 0 };

/** Ticks the movement system ran, to prove a run of N ticks really ran the tick phase N times. */
static u64 player_ticks = 0;

// NOLINTNEXTLINE(misc-use-internal-linkage): registered by name, see core_system.h
void player_tick(f32 delta_time_s) {
    f32x2 direction = { 0 };

    if (nya_input_key_pressed(NYA_KEY_A)) direction.x -= 1.0F;
    if (nya_input_key_pressed(NYA_KEY_D)) direction.x += 1.0F;
    if (nya_input_key_pressed(NYA_KEY_W)) direction.y -= 1.0F;
    if (nya_input_key_pressed(NYA_KEY_S)) direction.y += 1.0F;

    player_position += direction * (PLAYER_SPEED * delta_time_s);
    player_ticks++;
}

/* What the agent may do. Each is a press through the same queue SDL pushes a real one into. */

static void hold_left(NYA_Session* session) {
    nya_session_key(session, NYA_KEY_D, false);
    nya_session_key(session, NYA_KEY_A, true);
}

static void hold_right(NYA_Session* session) {
    nya_session_key(session, NYA_KEY_A, false);
    nya_session_key(session, NYA_KEY_D, true);
}

static void hold_up(NYA_Session* session) {
    nya_session_key(session, NYA_KEY_S, false);
    nya_session_key(session, NYA_KEY_W, true);
}

static void hold_down(NYA_Session* session) {
    nya_session_key(session, NYA_KEY_W, false);
    nya_session_key(session, NYA_KEY_S, true);
}

static void release_all(NYA_Session* session) {
    nya_session_key(session, NYA_KEY_A, false);
    nya_session_key(session, NYA_KEY_D, false);
    nya_session_key(session, NYA_KEY_W, false);
    nya_session_key(session, NYA_KEY_S, false);
}

static void point_somewhere(NYA_Session* session) {
    nya_session_mouse_move(session, (f32x2){ nya_session_range_f32(session, 0.0F, (f32)WINDOW_WIDTH), nya_session_range_f32(session, 0.0F, (f32)WINDOW_HEIGHT) });
    nya_session_mouse_click(session, NYA_MOUSE_BUTTON_LEFT);
}

/** A turn of the wheel, up or down at even odds, through the same event a real wheel sends. */
static void scroll(NYA_Session* session) {
    nya_session_wheel(session, nya_session_chance(session, 50) ? 1.0F : -1.0F);
}

/** What the agent perceives, and what the digest therefore covers. */
static u32 observe(NYA_Session* session, OUT f32* out_senses, u32 capacity) {
    nya_unused(session);
    nya_assert(capacity >= 4);

    f32x2 pointer = nya_input_mouse_position();

    out_senses[0] = player_position.x;
    out_senses[1] = player_position.y;
    out_senses[2] = pointer.x;
    out_senses[3] = pointer.y;

    return 4;
}

/** The oracle, beside the engine's own assertions: the agent cannot leave the world. */
static void check(NYA_Session* session) {
    if (fabsf(player_position.x) <= PLAYER_BOUND && fabsf(player_position.y) <= PLAYER_BOUND) return;

    nya_session_fail(session, "the agent reached (%.1f, %.1f), which is outside the world", (f64)player_position.x, (f64)player_position.y);
}

/** One seeded run from a clean world. Returns the digest; the failures are asserted here. */
static u64 play(u64 seed, u64 ticks, b8 real_time) {
    player_position = (f32x2){ 0 };
    player_ticks    = 0;

    NYA_Session* session = nya_session_create(.seed = seed, .tick_count = ticks, .real_time = real_time, .observe = observe, .check = check);
    defer        nya_session_destroy(session);

    nya_session_action_add(session, "left", 20, hold_left);
    nya_session_action_add(session, "right", 20, hold_right);
    nya_session_action_add(session, "up", 15, hold_up);
    nya_session_action_add(session, "down", 15, hold_down);
    nya_session_action_add(session, "release", 10, release_all);
    nya_session_action_add(session, "point", 10, point_somewhere);
    nya_session_action_add(session, "scroll", 5, scroll);
    nya_session_action_add(session, "idle", 40, nullptr);

    u32 failures = nya_session_run(session);
    nya_assert(failures == 0, "seed 0x%016llX failed %u checks", (unsigned long long)seed, failures);

    nya_assert(session->tick == ticks, "a session of %llu ticks played %llu", (unsigned long long)ticks, (unsigned long long)session->tick);
    nya_assert(player_ticks == ticks, "the tick phase ran %llu times for %llu ticks", (unsigned long long)player_ticks, (unsigned long long)ticks);

    return nya_session_digest(session);
}

s32 main(void) {
    // no display, ever: a session is a headless run of the real application.
    SDL_SetHintWithPriority(SDL_HINT_VIDEO_DRIVER, "offscreen", SDL_HINT_OVERRIDE);

    // settings and saves load from the data directory, so a scratch one keeps the player's own out of this and keeps two runs of this test from seeing each other.
    NYA_Arena*  scratch   = nya_arena_create(.name = "test_session_scratch");
    NYA_String* temp_root = nullptr;
    NYA_EXPECT(nya_filesystem_temp_directory(scratch, &temp_root));

    NYA_CString data_home = nya_string_to_cstring(scratch, nya_path_join(scratch, nya_string_to_cstring(scratch, temp_root), "nyangine-test-session"));
    (void)nya_filesystem_delete_recursive(data_home);

    nya_assert(nya_host_environment_add("XDG_DATA_HOME", data_home));
    nya_assert(nya_host_environment_add("APPDATA", data_home));

    defer nya_arena_destroy(scratch);
    defer (void)nya_filesystem_delete_recursive(data_home);

    NYA_EXPECT(nya_app_init(.headless = true, .app_id = "nyangine-test-session"));
    defer nya_app_deinit();

    // the window synthetic input is addressed to. Headless, so nothing is drawn into it; what it is for is that window handling and the UI's hit testing see the same events a real run does.
    NYA_WindowHandle window = nya_window_create("session", WINDOW_WIDTH, WINDOW_HEIGHT, NYA_WINDOW_NONE);
    nya_assert(nya_window_is_valid(window));

    // text delivery is per window and off until a field asks for it, which is what the UI's typing relies on.
    nya_assert(!nya_input_text_active(), "text is delivered before anything asked for it");
    nya_input_text_begin(window);
    nya_assert(nya_input_text_active(), "a window that asked for text gets it");

    // an IME's edit arrives as the composition and the run it has selected, which the text field underlines.
    nya_event_dispatch((NYA_Event){ .type = NYA_EVENT_TEXT_EDITING, .as_text_editing_event = { .window = window, .text = "nihon", .start = 2, .length = 3 } });
    nya_app_events_pump();

    s32 composition_start = 0, composition_length = 0;
    nya_input_text_composition_range(&composition_start, &composition_length);
    nya_assert(strcmp(nya_input_text_composition(), "nihon") == 0, "the composition arrived, got '%s'", nya_input_text_composition());
    nya_assert(composition_start == 2 && composition_length == 3, "with its selected run, got %d and %d", composition_start, composition_length);

    nya_input_text_end();
    nya_input_text_composition_range(&composition_start, &composition_length);
    nya_assert(composition_start == 0 && composition_length == 0, "and letting go of text clears it");
    nya_assert(!nya_input_text_active(), "and stops getting it when it lets go");

    // the agent's only effect on the world, in the tick phase, ordered where a game's systems go.
    nya_system_register((NYA_SystemEntry){ .name   = "test_player",
                                           .after  = "layers",
                                           .before = "tween_tick",
                                           .tick   = nya_callback(player_tick),
                                           .owner  = { .kind = NYA_SYSTEM_OWNER_GAME } });
    NYA_EXPECT(nya_system_registry_finalize());

    // TEST: a chance is a percentage at its ends too, and the wheel reaches input
    {
        NYA_Session* session = nya_session_create(.seed = 7, .tick_count = 1);
        defer        nya_session_destroy(session);

        b8 zero_came_up = false, hundred_missed = false;
        for (u32 i = 0; i < 1000; i++) {
            zero_came_up   |= nya_session_chance(session, 0);
            hundred_missed |= !nya_session_chance(session, 100);
        }
        nya_assert(!zero_came_up && !hundred_missed, "a chance of 0 came up or one of 100 missed in a thousand draws");

        // dispatched events wait for the frame to pump them, as they do in a played run.
        nya_session_wheel(session, 3.0F);
        nya_app_events_pump();
        nya_assert(nya_input_mouse_wheel_scroll().y == 3.0F, "the wheel turned 3 and input saw %f", (f64)nya_input_mouse_wheel_scroll().y);
    }

    // TEST: one seed, played twice, is one run. Without this nothing below means anything: a digest that moves between two identical runs measures the machine.
    u64 first  = play(0xA11CE5EEDULL, FAST_TICKS, false);
    u64 second = play(0xA11CE5EEDULL, FAST_TICKS, false);

    nya_assert(first == second, "the same seed gave 0x%016llX and then 0x%016llX", (unsigned long long)first, (unsigned long long)second);

    // TEST: a different seed is a different run, so the digest is reading the run and not something constant about the scenario.
    {
        u64 other = play(0xB0B0B0B0ULL, FAST_TICKS, false);
        nya_assert(other != first, "two seeds gave the same digest 0x%016llX", (unsigned long long)other);
    }

    // TEST: fast forward is the same run as real time, and enormously faster. This is the claim the whole facility rests on: the tick is a fixed timestep and every draw is hashed from (seed, tick, index), so the clock decides how often a tick happens and never what one does.
    {
        u64 fast_started_ns = nya_clock_get_monotonic_ns();
        u64 fast            = play(0xFA57ULL, REAL_TICKS, false);
        u64 fast_ns         = nya_clock_get_monotonic_ns() - fast_started_ns;

        u64 real_started_ns = nya_clock_get_monotonic_ns();
        u64 real            = play(0xFA57ULL, REAL_TICKS, true);
        u64 real_ns         = nya_clock_get_monotonic_ns() - real_started_ns;

        nya_assert(fast == real, "the same seed gave 0x%016llX fast forwarded and 0x%016llX in real time", (unsigned long long)fast,
                   (unsigned long long)real);

        f64 speed_up = fast_ns > 0 ? (f64)real_ns / (f64)fast_ns : 0.0;

        printf("  %d ticks: %.1f ms fast forwarded, %.1f ms in real time, %.0fx\n", REAL_TICKS, nya_time_ns_to_ms(fast_ns),
               nya_time_ns_to_ms(real_ns), speed_up);

        nya_assert(speed_up >= MINIMUM_SPEED_UP, "fast forward was only %.1fx real time; the simulated clock is not driving the loop", speed_up);
    }

    printf("All tests passed.\n");
    return 0;
}
