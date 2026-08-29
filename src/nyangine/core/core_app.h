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

typedef struct NYA_App        NYA_App;
typedef struct NYA_AppOptions NYA_AppOptions;
typedef struct NYA_FrameStats NYA_FrameStats;

#define _NYA_APP_DEFAULT_OPTIONS .time_step_ns = nya_time_ms_to_ns(16), .frame_rate_limit = 120, .vsync_enabled = false, .max_concurrent_jobs = 4

struct NYA_AppOptions {
    u64 time_step_ns;
    u32 frame_rate_limit;
    b8  vsync_enabled;
    u8  max_concurrent_jobs;
};

struct NYA_FrameStats {
    /** Wall clock timestamp of nya_app_init, the origin every other time here is measured from. */
    u64 started_ns;

    /**
     * How long the app has been running, as of the start of the current frame.
     *
     * Sampled once per frame rather than read from the clock on demand, so everything within a frame
     * agrees on what time it is. Use nya_app_uptime_ns for the live value.
     * */
    u64 uptime_ns;

    /**
     * uptime_ns as seconds, ready for a shader uniform without a cast. f32 because that is what lands
     * in a uniform buffer; loses resolution as the app runs — about a millisecond after three hours —
     * so drive animation from it, not anything that needs to stay exact. uptime_ns is the precise value.
     * */
    f32 uptime_s;

    u64 min_frame_time_ns;
    f32 delta_time_s;
    f32 fps;

    u64 frame_start_time_ns;
    u64 frame_end_time_ns;
    u64 prev_frame_time_ns;

    /**
     * Frame period: start of this frame to start of the last, sleep included. What fps is computed
     * from. Not a cost — at the default 120 limit it sits at 8.3 ms regardless of frame work, which
     * reads like a slow frame and isn't one.
     * */
    u64 elapsed_ns;

    /**
     * Frame work: what the frame actually spent before the limiter slept. The number to look at when
     * asking "is this slow" — `elapsed_ns` minus this is the sleep.
     * */
    u64 work_ns;

    /** What the limiter slept to hold the frame rate. Zero when the frame ran over budget. */
    u64 sleep_ns;

    s64 time_behind_ns;
};

struct NYA_App {
    b8 initialized;
    b8 should_quit;

    /**
     * The world this application created at startup, and destroys on the way out. Owning it is all
     * NYA_App does with it — everything that operates on a world goes through nya_world instead.
     *
     * Also what makes hot reloading work. The game is a shared library that gets closed and reopened,
     * so every file scope variable in it reinitialises on each reload; a game keeping state in a
     * static loses it every reload. NYA_App lives in the executable and outlives reloads, so
     * `nya_world()->user_data` is still there on the other side.
     * */
    NYA_World* world;

    /** use `nya_app_options_update` to change config */
    NYA_AppOptions options;

    NYA_Arena* frame_allocator;

    /**
     * Stands in for frame_allocator while a window is dragged by its edge. Frames produced during a
     * drag nest inside an outer frame parked in the event pump, so they cannot reset that frame's
     * arena; emptying this one per nested frame keeps a long drag from growing memory until the mouse
     * comes up.
     * */
    NYA_Arena* live_resize_allocator;

    NYA_FrameStats frame_stats;

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
 *
 * On failure nothing is left standing — whatever came up before the failure is torn back down — so
 * the caller must not follow a failed init with nya_app_deinit.
 * */
#define nya_app_init(...) nya_app_init_with_options((NYA_AppOptions){ _NYA_APP_DEFAULT_OPTIONS, __VA_ARGS__ })
NYA_API NYA_Error nya_app_init_with_options(NYA_AppOptions options) __attr_no_discard;

/** Time since nya_app_init, live rather than the once per frame NYA_FrameStats.uptime_ns. */
NYA_API u64      nya_app_uptime_ns(void) __attr_no_discard;
NYA_API f64      nya_app_uptime_s(void) __attr_no_discard;
NYA_API void     nya_app_deinit(void);
NYA_API void     nya_app_run(void);
NYA_API NYA_App* nya_app_get(void);

/*
 * The game's root pointer moved to nya_world_user_data; see core_world.h. It used to be `void* state`
 * here; now the world owns the arena, so the arena, entities and game state are one lifetime instead
 * of three that had to be unwound in order by hand.
 */

NYA_API void nya_app_options_update(NYA_AppOptions options);
