#pragma once

#include "nyangine/base/base_arena.h"
#include "nyangine/base/base_array.h"
#include "nyangine/base/base_attributes.h"
#include "nyangine/base/base_string.h"
#include "nyangine/core/core_asset.h"
#include "nyangine/core/core_callback.h"
#include "nyangine/core/core_config.h"
#include "nyangine/core/core_entity.h"
#include "nyangine/core/core_event.h"
#include "nyangine/core/core_i18n.h"
#include "nyangine/core/core_input.h"
#include "nyangine/core/core_job.h"
#include "nyangine/physics/physics2d.h"
#include "nyangine/core/core_save.h"
#include "nyangine/core/core_settings.h"
#include "nyangine/core/core_sim.h"
#include "nyangine/core/core_window.h"
#include "nyangine/core/core_world.h"
#include "nyangine/renderer/renderer.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * TYPES
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

typedef struct NYA_App           NYA_App;
typedef struct NYA_AppOptions    NYA_AppOptions;
typedef struct NYA_AppTimeSource NYA_AppTimeSource;
typedef struct NYA_FrameStats NYA_FrameStats;

#define _NYA_APP_DEFAULT_OPTIONS                                                                                                             \
    .time_step_ns = nya_time_ms_to_ns(16), .frame_rate_limit = 120, .unfocused_frame_rate_limit = 0, .vsync_enabled = false,                  \
    .max_concurrent_jobs = 4

struct NYA_AppOptions {
    u64 time_step_ns;
    u32 frame_rate_limit;

    /**
     * Frames per second to cap at while no window has focus. Zero leaves the focused cap in force.
     *
     * A window left in a corner is unfocused most of its life, and animating it at 120 fps spends a GPU
     * on nothing. Fifteen still reads as alive.
     *
     * Applied even with `vsync_enabled`, unlike `frame_rate_limit`: this asks for slower than the display,
     * so the sleep lands before the swap would have blocked. An occluded window draws nothing regardless,
     * since nya_render_begin refuses it.
     * */
    u32 unfocused_frame_rate_limit;

    b8  vsync_enabled;
    u8  max_concurrent_jobs;

    /**
     * No display and no window: a dedicated server, a smoke run or CI.
     *
     * Subsystems that only make sense with one are registered optional, so a missing GPU backend is a
     * subsystem reported unavailable at debug level rather than a failed start. Left false, the same
     * failure is loud and stops bring-up, which is what a player's machine wants. See
     * NYA_SystemEntry.optional.
     * */
    b8 headless;

    /**
     * Reverse-DNS or plain application id, such as "gnyame". Null leaves SDL's default. On Wayland this
     * is the app_id a desktop file is matched by, which is where the window's icon and name come from.
     * */
    NYA_ConstCString app_id;

    /**
     * The game's Steam app id. A build with NYA_PLUGIN_STEAM relaunches through Steam when started outside it and
     * connects to the client; zero, or a build without the plugin, leaves Steam alone.
     * */
    u32 steam_app_id;
};

struct NYA_FrameStats {
    /** Wall clock timestamp of nya_app_init, the origin every other time here is measured from. */
    u64 started_ns;

    /**
     * How long the app has been running, as of the start of the current frame.
     * */
    u64 uptime_ns;

    /**
     * uptime_ns as seconds, for shader uniforms. f32 loses resolution over time (about a millisecond
     * after three hours), so use it for animation only. uptime_ns is the precise value.
     * */
    f32 uptime_s;

    u64 min_frame_time_ns;
    f32 delta_time_s;
    f32 fps;

    u64 frame_start_time_ns;
    u64 frame_end_time_ns;
    u64 prev_frame_time_ns;

    /**
     * Frame period: start of the last frame to start of this one, sleep included. fps is computed from
     * it. Not a cost: at the default 120 limit it sits at 8.3 ms whatever the frame did.
     * */
    u64 elapsed_ns;

    /**
     * Frame work: time spent before the limiter slept. The number to read for "is this slow".
     * `elapsed_ns` minus this is the sleep.
     * */
    u64 work_ns;

    /** What the limiter slept to hold the frame rate. Zero when the frame ran over budget. */
    u64 sleep_ns;

    s64 time_behind_ns;
};

/**
 * Where the frame loop reads time, and whether it may sleep for the frame rate limit.
 *
 * The one seam through which the loop's dependency on the wall clock is replaced. A simulation
 * installs a clock it advances itself, so a session runs as fast as the CPU allows and every frame
 * books exactly one tick of debt, which is what makes a seeded session replay the same way on a fast
 * machine and a slow one. Zeroed is the real clock and the real limiter, which is what a game runs
 * with and what costs it nothing.
 *
 * See nya_app_time_source_set, and testing_session.h for the harness that installs one.
 * */
struct NYA_AppTimeSource {
    /**
     * Monotonic nanoseconds. Null means nya_clock_get_monotonic_ns, the real one.
     *
     * It must never go backwards, exactly as the real clock must not: a frame's elapsed time is the
     * unsigned difference of two readings, and one reading behind the last is an enormous delta.
     * */
    u64 (*now_ns)(void);

    /**
     * Ticks the fixed step may catch up in one frame. Zero means the loop's own default.
     *
     * A simulation sets this to one: with a clock that advances by exactly one step per frame there is
     * never more than one tick of debt, and a cap of one turns a mistake about that into a visible
     * stall rather than a burst of ticks nobody asked for.
     * */
    u32 catch_up_ticks_max;

    /** True to skip the frame rate limiter's sleep. A simulated clock never earns one anyway. */
    b8 never_sleep;
};

struct NYA_App {
    b8 initialized;
    b8 should_quit;

    /**
     * The world created at startup and destroyed on exit. NYA_App only owns it; everything else goes
     * through nya_world.
     * */
    NYA_World* world;

    /** use `nya_app_options_update` to change config */
    NYA_AppOptions options;

    NYA_Arena* frame_allocator;

    /**
     * Replaces frame_allocator while a window is dragged by its edge. Those frames nest inside an outer
     * frame parked in the event pump and cannot reset its arena, so this one is emptied per nested frame.
     * */
    NYA_Arena* live_resize_allocator;

    NYA_FrameStats frame_stats;

    /** Zeroed is the real clock and the real frame rate limiter. See NYA_AppTimeSource. */
    NYA_AppTimeSource time_source;

    NYA_AssetSystem    asset_system;
    NYA_CallbackSystem callback_system;
    NYA_ConfigSystem   config_system;
    NYA_EventSystem    event_system;
    NYA_I18nSystem     i18n_system;
    NYA_InputSystem    input_system;
    NYA_JobSystem      job_system;
    NYA_RenderSystem   render_system;
    NYA_SaveSystem     save_system;
    NYA_SettingsSystem settings_system;
    NYA_WindowSystem   window_system;
};

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * FUNCTIONS AND MACROS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/**
 * Brings up SDL and every subsystem. Returns an error rather than panicking, because what fails here
 * is environmental rather than programmer error: no GPU backend, no display, out of handles. Whether
 * that means quit or fall back to something else is the caller's decision.
 * */
#define nya_app_init(...) nya_app_init_with_options((NYA_AppOptions){ _NYA_APP_DEFAULT_OPTIONS, __VA_ARGS__ })
NYA_API NYA_Error nya_app_init_with_options(NYA_AppOptions options) __attr_no_discard;

/**
 * Drains SDL's events into the queue and routes everything queued through window handling, the input
 * system and each window's layer stack, topmost first, stopping at whoever marks an event handled.
 *
 * The frame loop's own event step, called once per frame between the frame phase and the tick phase.
 * Exported because anything that has to make the application see an event from outside a frame would
 * otherwise re-implement that routing and get the order wrong: a simulated session pumps it once
 * before tick zero so the agent's pointer is where the session says it is. See testing_session.h.
 * */
NYA_API void nya_app_events_pump(void);

/** Time since nya_app_init, live rather than the once per frame NYA_FrameStats.uptime_ns. */
NYA_API u64      nya_app_uptime_ns(void) __attr_no_discard;
NYA_API f64      nya_app_uptime_s(void) __attr_no_discard;
NYA_API void     nya_app_deinit(void);
NYA_API void     nya_app_run(void);
NYA_API NYA_App* nya_app_get(void);

/**
 * Replaces where the frame loop reads time. A zeroed source restores the real clock and the limiter.
 *
 * ```c
 * nya_app_time_source_set((NYA_AppTimeSource){ .now_ns = simulated_clock, .catch_up_ticks_max = 1, .never_sleep = true });
 * nya_app_run();
 * nya_app_time_source_set((NYA_AppTimeSource){ 0 });
 * ```
 *
 * Swapping the clock rebases the frame clock on the new one: the next frame measures no elapsed time
 * and owes no ticks, and the uptime origin moves so the program's age does not jump. Without that, the
 * first frame after a swap takes the difference between two unrelated clocks, which is an enormous
 * delta one way and an unsigned wrap the other, and a simulated clock that ran ahead of the wall clock
 * and is then handed back leaves the loop believing it is centuries behind.
 * */
NYA_API void              nya_app_time_source_set(NYA_AppTimeSource source);
NYA_API NYA_AppTimeSource nya_app_time_source(void) __attr_no_discard;

/**
 * How far this frame sits between the last update tick and the next, in [0, 1]. Anything that moves per tick draws
 * from its previous tick toward its current one by this, so motion stays smooth when the display rate and the tick
 * rate do not line up. One outside a running app, so a draw shows the current tick.
 * */
NYA_API f32 nya_app_tick_alpha(void) __attr_no_discard;

/** What nya_app_get returns. Declared so a test can stand up an app without nya_app_init. */
NYA_API NYA_App _NYA_APP_INSTANCE;

/*
 * The game's root pointer lives in nya_world_user_data, so the world arena, entities and game state
 * share one lifetime. See core_world.h.
 */

NYA_API void nya_app_options_update(NYA_AppOptions options);
