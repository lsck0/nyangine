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

/** Time since nya_app_init, live rather than the once per frame NYA_FrameStats.uptime_ns. */
NYA_API u64      nya_app_uptime_ns(void) __attr_no_discard;
NYA_API f64      nya_app_uptime_s(void) __attr_no_discard;
NYA_API void     nya_app_deinit(void);
NYA_API void     nya_app_run(void);
NYA_API NYA_App* nya_app_get(void);

/*
 * The game's root pointer lives in nya_world_user_data, so the world arena, entities and game state
 * share one lifetime. See core_world.h.
 */

NYA_API void nya_app_options_update(NYA_AppOptions options);
