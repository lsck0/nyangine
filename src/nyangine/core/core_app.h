#pragma once

#include "nyangine/base/base_arena.h"
#include "nyangine/base/base_array.h"
#include "nyangine/base/base_attributes.h"
#include "nyangine/base/base_string.h"
#include "nyangine/core/core_asset.h"
#include "nyangine/core/core_callback.h"
#include "nyangine/core/core_entity.h"
#include "nyangine/core/core_event.h"
#include "nyangine/core/core_input.h"
#include "nyangine/core/core_job.h"
#include "nyangine/core/core_settings.h"
#include "nyangine/core/core_sim.h"
#include "nyangine/core/core_window.h"
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

    u64 min_frame_time_ns;
    f32 delta_time_s;
    f32 fps;

    u64 frame_start_time_ns;
    u64 frame_end_time_ns;
    u64 prev_frame_time_ns;
    u64 elapsed_ns;
    s64 time_behind_ns;
};

struct NYA_App {
    b8 initialized;
    b8 should_quit;

    /** use `nya_app_options_update` to change config */
    NYA_AppOptions options;

    NYA_Arena* frame_allocator;

    NYA_FrameStats frame_stats;

    NYA_AssetSystem    asset_system;
    NYA_CallbackSystem callback_system;
    NYA_EntitySystem   entity_system;
    NYA_EventSystem    event_system;
    NYA_InputSystem    input_system;
    NYA_JobSystem      job_system;
    NYA_RenderSystem   render_system;
    NYA_SettingsSystem settings_system;
    NYA_SimSystem      sim_system;
    NYA_WindowSystem   window_system;
};

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * FUNCTIONS AND MACROS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/**
 * Brings up SDL and every subsystem.
 *
 * Returns an error rather than panicking, because what fails here is environmental rather than
 * programmer error: no GPU backend, no display, out of handles. Whether that means quit or fall
 * back to something else is the caller's decision to make, and an assert would take it away.
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
NYA_API void     nya_app_options_update(NYA_AppOptions options);
