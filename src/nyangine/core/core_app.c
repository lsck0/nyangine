#include "SDL3/SDL_init.h"
#include "SDL3/SDL_timer.h"

#include "nyangine/nyangine.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API DECLARATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

NYA_INTERNAL NYA_App _NYA_APP_INSTANCE;

/**
 * Most fixed timestep ticks one frame is allowed to run to catch up.
 * */
#define _NYA_APP_MAX_CATCH_UP_TICKS 5

NYA_INTERNAL void _nya_app_handle_shutdown_signal(NYA_Signal signal);

/** Samples the clock once for the frame and books the time since the last one against the update debt. */
NYA_INTERNAL void _nya_app_advance_frame_clock(void);

/** Runs the fixed timestep update until the debt is paid off. */
NYA_INTERNAL void _nya_app_update(void);

/** Draws every window that has something to draw into. */
NYA_INTERNAL void _nya_app_render(void);

/**
 * Whether any live window holds input focus. Drives the unfocused frame cap.
 *
 * Any rather than all: a game with a tool window beside its main one is still being worked in, and
 * slowing both down because the inspector is the focused one would be the wrong answer.
 * */
NYA_INTERNAL b8 _nya_app_any_window_has_focus(void) __attr_no_discard;

/**
 * Renders, and keeps simulating, while the window is being dragged by its edge.
 * */
NYA_INTERNAL bool SDLCALL _nya_app_live_resize_event_watch(void* userdata, SDL_Event* event);

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * SUBSYSTEMS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/**
 * Registers every engine subsystem with core_system.h's registry, in bring-up order. See
 * `_nya_app_register_subsystems` below for the table itself and the ordering notes that used to hang
 * above a `NYA_Subsystem _NYA_SUBSYSTEMS[]` array — the registration calls carry the same comments,
 * in the same order, now that the array is gone.
 * */
NYA_INTERNAL void _nya_app_register_subsystems(void);

NYA_INTERNAL NYA_Error _nya_app_bring_up_logfile(void) {
    NYA_Error opened = nya_log_directory_open(NYA_LOG_DIRECTORY, NYA_LOG_RETENTION_DAYS);
    if (!opened.ok) nya_log_warn("Continuing without a log file: %s", (NYA_ConstCString)opened.message);
    return NYA_OK;
}

/*
 * First, and before the settings it feeds.
 */
NYA_INTERNAL NYA_Error _nya_app_bring_up_save(void) { (void)nya_system_save_init(); return NYA_OK; }

/* Cannot fail: it owns no memory, and everything else may want to read a setting while coming up.
 * Loads whatever the save system found, or the defaults when it found nothing. */
NYA_INTERNAL NYA_Error _nya_app_bring_up_settings(void) { nya_system_settings_init(); return NYA_OK; }

NYA_INTERNAL NYA_Error _nya_app_bring_up_job(void) { return nya_system_job_init(); }
NYA_INTERNAL NYA_Error _nya_app_bring_up_callback(void) { nya_system_callback_init(); return NYA_OK; }
/* After the callback registry, whose handles a tween's on_complete resolves through. */
NYA_INTERNAL NYA_Error _nya_app_bring_up_tween(void) { nya_system_tween_init(); return NYA_OK; }
NYA_INTERNAL NYA_Error _nya_app_bring_up_renderer(void) { return nya_system_renderer_init(); }
NYA_INTERNAL NYA_Error _nya_app_bring_up_window(void) { nya_system_window_init(); return NYA_OK; }
NYA_INTERNAL NYA_Error _nya_app_bring_up_events(void) { return nya_system_events_init(); }
NYA_INTERNAL NYA_Error _nya_app_bring_up_input(void) { nya_system_input_init(); return NYA_OK; }
/* After the event system, whose drain loop hands it the SDL events it consumes. */
NYA_INTERNAL NYA_Error _nya_app_bring_up_gamepad(void) { nya_system_gamepad_init(); return NYA_OK; }
NYA_INTERNAL NYA_Error _nya_app_bring_up_asset(void) { nya_system_asset_init(); return NYA_OK; }

/*
 * After the asset system, because a locale file is an asset: the bytes are read through nya_asset_read
 * and the file is watched by registering it, both of which need the registry up.
 */
NYA_INTERNAL NYA_Error _nya_app_bring_up_i18n(void) { nya_system_i18n_init(); return NYA_OK; }

/*
 * After the asset system, for the same reason i18n is: a config file is read through nya_asset_read
 * and, under NYA_ASSET_HOT_RELOAD, watched by registering it, both of which need the registry up.
 */
NYA_INTERNAL NYA_Error _nya_app_bring_up_config(void) { nya_system_config_init(); return NYA_OK; }

/* After the asset system, which creates the mixer these tracks are made on. */
NYA_INTERNAL NYA_Error _nya_app_bring_up_audio(void) { return nya_system_audio_init(); }

/*
 * The world, which is entities, physics and the simulation barrier as one lifetime.
 */
NYA_INTERNAL NYA_Error _nya_app_bring_up_world(void) {
    NYA_App* app = nya_app_get();
    app->world   = nya_world_create();
    (void)nya_world_set(app->world);
    return NYA_OK;
}

NYA_INTERNAL void _nya_app_tear_down_save(void) { nya_system_save_deinit(); }

/* Last, so the teardown of everything above it is in the file. */
NYA_INTERNAL void _nya_app_tear_down_logfile(void) { nya_log_file_close(); }

/* After settings, which writes its file on the way out into the directory the save system owns. */
NYA_INTERNAL void _nya_app_tear_down_settings(void) { nya_system_settings_deinit(); }

NYA_INTERNAL void _nya_app_tear_down_job(void) { nya_system_job_deinit(); }
NYA_INTERNAL void _nya_app_tear_down_callback(void) { nya_system_callback_deinit(); }
/* Before the callback registry it resolves completion handles through. */
NYA_INTERNAL void _nya_app_tear_down_tween(void) { nya_system_tween_deinit(); }
NYA_INTERNAL void _nya_app_tear_down_renderer(void) { nya_system_renderer_deinit(); }
NYA_INTERNAL void _nya_app_tear_down_window(void) { nya_system_window_deinit(); }
NYA_INTERNAL void _nya_app_tear_down_events(void) { nya_system_events_deinit(); }
NYA_INTERNAL void _nya_app_tear_down_input(void) { nya_system_input_deinit(); }
NYA_INTERNAL void _nya_app_tear_down_asset(void) { nya_system_asset_deinit(); }

/* Before the event system stops feeding it, and before the process ends with a pad still buzzing. */
NYA_INTERNAL void _nya_app_tear_down_gamepad(void) { nya_system_gamepad_deinit(); }

/* Before the asset system, since its watch hook resolves locale files through the registry. */
NYA_INTERNAL void _nya_app_tear_down_i18n(void) { nya_system_i18n_deinit(); }

/* Before the asset system, since its watch hook resolves config files through the registry. */
NYA_INTERNAL void _nya_app_tear_down_config(void) { nya_system_config_deinit(); }

/* Before the asset system destroys the mixer the tracks belong to. */
NYA_INTERNAL void _nya_app_tear_down_audio(void) { nya_system_audio_deinit(); }

/* Everything the world holds — every entity, every rigid body, and whatever the game hung off
 * user_data — goes here, in the order nya_world_destroy knows about. */
NYA_INTERNAL void _nya_app_tear_down_world(void) {
    NYA_App* app = nya_app_get();
    (void)nya_world_set(nullptr);
    nya_world_destroy(app->world);
    app->world = nullptr;
}

/**
 * Registers every engine subsystem, in bring-up order, each chained `after` the one before it so
 * nya_system_registry_finalize cannot produce anything but this exact order. **Teardown is this order
 * in reverse** — nya_system_registry_run_deinit's own contract, not something this function arranges.
 * */
void _nya_app_register_subsystems(void) {
    // First up and last down, so every line another subsystem writes on its way up or down is in the file.
    nya_system_register((NYA_SystemEntry){ .name = "logfile", .init = _nya_app_bring_up_logfile, .deinit = _nya_app_tear_down_logfile });

    // Before the settings it feeds.
    nya_system_register((NYA_SystemEntry){ .name = "save", .after = "logfile", .init = _nya_app_bring_up_save, .deinit = _nya_app_tear_down_save });
    nya_system_register((NYA_SystemEntry){ .name         = "settings",
                                            .after        = "save",
                                            .init         = _nya_app_bring_up_settings,
                                            .deinit       = _nya_app_tear_down_settings });

    // Before job, so the workers stop before the registry they resolve through is freed.
    nya_system_register((NYA_SystemEntry){ .name         = "callback",
                                            .after        = "settings",
                                            .init         = _nya_app_bring_up_callback,
                                            .deinit       = _nya_app_tear_down_callback });
    nya_system_register((NYA_SystemEntry){ .name = "job", .after = "callback", .init = _nya_app_bring_up_job, .deinit = _nya_app_tear_down_job });

    // After the callback registry, whose handles a tween's on_complete resolves through.
    nya_system_register((NYA_SystemEntry){ .name = "tween", .after = "job", .init = _nya_app_bring_up_tween, .deinit = _nya_app_tear_down_tween });

    nya_system_register((NYA_SystemEntry){ .name         = "renderer",
                                            .after        = "tween",
                                            .init         = _nya_app_bring_up_renderer,
                                            .deinit       = _nya_app_tear_down_renderer });
    nya_system_register((NYA_SystemEntry){ .name = "events", .after = "renderer", .init = _nya_app_bring_up_events, .deinit = _nya_app_tear_down_events });
    nya_system_register((NYA_SystemEntry){ .name = "input", .after = "events", .init = _nya_app_bring_up_input, .deinit = _nya_app_tear_down_input });

    // After events, whose drain loop hands it the SDL events it consumes.
    nya_system_register((NYA_SystemEntry){ .name         = "gamepad",
                                            .after        = "input",
                                            .init         = _nya_app_bring_up_gamepad,
                                            .deinit       = _nya_app_tear_down_gamepad });

    nya_system_register((NYA_SystemEntry){ .name = "asset", .after = "gamepad", .init = _nya_app_bring_up_asset, .deinit = _nya_app_tear_down_asset });

    // After the asset system: a locale file is an asset, read and watched through the registry.
    nya_system_register((NYA_SystemEntry){ .name = "i18n", .after = "asset", .init = _nya_app_bring_up_i18n, .deinit = _nya_app_tear_down_i18n });

    // After the asset system, for the same reason i18n is: a config file is read and, under hot
    // reload, watched through the registry.
    nya_system_register((NYA_SystemEntry){ .name = "config", .after = "i18n", .init = _nya_app_bring_up_config, .deinit = _nya_app_tear_down_config });

    // After the asset system, which creates the mixer these tracks are made on — and so torn down
    // before it destroys that mixer.
    nya_system_register((NYA_SystemEntry){ .name = "audio", .after = "config", .init = _nya_app_bring_up_audio, .deinit = _nya_app_tear_down_audio });

    nya_system_register((NYA_SystemEntry){ .name = "world", .after = "audio", .init = _nya_app_bring_up_world, .deinit = _nya_app_tear_down_world });

    // Last, so it is the first thing torn down. See the note above.
    nya_system_register((NYA_SystemEntry){ .name = "window", .after = "world", .init = _nya_app_bring_up_window, .deinit = _nya_app_tear_down_window });

    // A finalize failure here is a typo in one of the .after strings above, in a table only this
    // function writes — a programmer error to catch at the next boot, not a runtime condition to
    // recover from, which is why this asserts instead of propagating an NYA_Error to its own caller.
    NYA_Error finalized = nya_system_registry_finalize();
    nya_assert(finalized.ok, "engine subsystem registration is broken: %s", (NYA_ConstCString)finalized.message);
}

/**
 * One frame of update and render, without the parts of the loop that cannot safely run nested.
 * */
NYA_INTERNAL void _nya_app_frame_step(b8 live_resize);

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PUBLIC API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

NYA_Error nya_app_init_with_options(NYA_AppOptions options) {
    nya_assert(options.time_step_ns > 0, "time_step_ns must be greater than 0");
    nya_assert(options.frame_rate_limit > 0, "frame_rate_limit must be greater than 0");
    nya_assert(options.max_concurrent_jobs > 0, "max_concurrent_jobs must be greater than 0");

    nya_integrity_assert();

    // As early as possible: the baseline is only meaningful if nothing has had a chance to hook the
    // process yet, and everything below this line is a chance.
    nya_integrity_baseline_capture();

    nya_signals_init();
    nya_signals_set_handler(NYA_SIGNAL_HANGUP, _nya_app_handle_shutdown_signal);
    nya_signals_set_handler(NYA_SIGNAL_INTERRUPT, _nya_app_handle_shutdown_signal);
    nya_signals_set_handler(NYA_SIGNAL_TERMINATE, _nya_app_handle_shutdown_signal);

    if (!SDL_Init(SDL_INIT_EVENTS | SDL_INIT_VIDEO | SDL_INIT_AUDIO)) {
        nya_signals_deinit();
        return nya_error(NYA_ERROR_NOT_OK, "SDL_Init() failed: %s", SDL_GetError());
    }

    NYA_App* app = &_NYA_APP_INSTANCE;
    *app         = (NYA_App){
        .initialized                    = true,
        .options                        = options,
        .frame_allocator                = nya_arena_create(.name = "frame_allocator"),
        .live_resize_allocator          = nya_arena_create(.name = "live_resize_allocator"),
        .frame_stats.started_ns         = nya_clock_get_monotonic_ns(),
        .frame_stats.prev_frame_time_ns = nya_clock_get_monotonic_ns(),
        .frame_stats.min_frame_time_ns  = 1'000'000'000 / (u64)options.frame_rate_limit,
    };

    nya_log_info("Nyangine initialized. Initializing subsystems...");

    _nya_app_register_subsystems();

    // Brought up in registration order and unwound in reverse of however far we got. Returning early
    // and leaving half a world standing would leave the caller with nothing safe to do: nya_app_deinit
    // would tear down systems that were never built.
    //
    // Deliberately not nya_system_registry_run_init: that stops on the first error too, but this still
    // needs init_at/deinit_at rather than run_deinit for the unwind, because run_deinit tears down
    // *everything* registered, not just what actually came up before the failure. See core_system.h's
    // note on why those two accessors exist.
    NYA_Error result     = NYA_OK;
    u32       brought_up = 0;

    for (; brought_up < nya_system_registry_count(); brought_up++) {
        NYA_SystemInitFn init = nya_system_registry_init_at(brought_up);
        result                = init != nullptr ? init() : NYA_OK;
        if (!result.ok) goto unwind;
    }

    // After the renderer and the windows, since the watcher draws. Failing to register it is not
    // fatal: it only costs the frames that would have been produced during a resize drag.
    if (!SDL_AddEventWatch(_nya_app_live_resize_event_watch, nullptr)) {
        nya_log_warn("SDL_AddEventWatch() failed, the window will not redraw while being resized: %s", SDL_GetError());
    }

    nya_log_info("Subsystems initialized successfully.");
    return NYA_OK;

unwind:
    nya_log_error("Subsystem initialization failed at '%s'; unwinding. %s", nya_system_registry_name_at(brought_up),
                  (NYA_ConstCString)result.message);

    // Reverse, skipping the one that failed and everything after it.
    for (u32 i = brought_up; i > 0; i--) {
        NYA_SystemDeinitFn deinit = nya_system_registry_deinit_at(i - 1);
        if (deinit != nullptr) deinit();
    }

    nya_arena_destroy(app->frame_allocator);
    nya_arena_destroy(app->live_resize_allocator);
    app->initialized = false;

    SDL_Quit();
    nya_signals_deinit();

    return result;
}

u64 nya_app_uptime_ns(void) {
    NYA_App* app = nya_app_get();
    return nya_clock_get_monotonic_ns() - app->frame_stats.started_ns;
}

f64 nya_app_uptime_s(void) {
    return (f64)nya_app_uptime_ns() / 1'000'000'000.0;
}

void nya_app_deinit(void) {
    NYA_App* app = nya_app_get();

    nya_log_info("Deinitializing subsystems...");

    // Before the renderer goes away, or a late expose could still ask a dead device to draw.
    SDL_RemoveEventWatch(_nya_app_live_resize_event_watch, nullptr);

    // The registered order in reverse — the same order the unwind path uses, and for the same reasons.
    // Safe to use run_deinit here unlike in the unwind path: by the time nya_app_deinit runs, every
    // subsystem came up, so "deinit everything registered" and "deinit everything that came up" agree.
    nya_system_registry_run_deinit();

    nya_log_info("Subsystems deinitialized successfully.");

    nya_arena_destroy(app->frame_allocator);
    nya_arena_destroy(app->live_resize_allocator);

    SDL_Quit();

    nya_signals_deinit();

    app->initialized = false;

    nya_log_info("Nyangine deinitialized. Goodbye.");
}

void nya_app_run(void) {
    NYA_App* app = nya_app_get();

    while (!app->should_quit) {
        // Before the frame timer opens, so that timer is itself the frame's depth 0 span. Every
        // scope entered from here until the next iteration is tagged with this frame number, which
        // is what nya_perf_frame_spans selects on to reconstruct the breakdown.
        nya_perf_frame_begin();

        nya_perf_time_this_scope("frame");

        // start of frame tasks
        {
            nya_event_dispatch((NYA_Event){
                .type = NYA_EVENT_FRAME_STARTED,
            });

            _nya_app_advance_frame_clock();

            // Cheap, and does nothing until the UTC date actually changes. Without it a process that
            // runs across midnight puts every following day into the file it started in.
            nya_log_directory_roll();

            // Before events are drained: the edges this clears are set by the events about to arrive.
            nya_system_gamepad_frame_begin();
        }

        // handle events
        {
            nya_perf_time_this_scope("frame_event_handling");
            nya_event_dispatch((NYA_Event){
                .type = NYA_EVENT_HANDLING_STARTED,
            });

            nya_system_event_drain_sdl_events();

            NYA_Event event;
            while (nya_system_event_poll(&event)) {
                if (event.was_handled) continue;

                nya_system_window_handle_event(&event);
                if (event.was_handled) continue;

                nya_system_input_handle_event(&event);
                if (event.was_handled) continue;

                for (u32 slot = 0; slot < NYA_WINDOW_MAX; slot++) {
                    NYA_Window* window = nya_window_at_slot(slot);
                    if (window == nullptr) continue;

                    nya_array_foreach_reverse (window->layer_stack, layer) {
                        NYA_LayerOnEventFn on_event_fn = nya_callback_get(layer->on_event);
                        if (layer->enabled && on_event_fn != nullptr) {
                            on_event_fn(window, &event);
                            if (event.was_handled) break;
                        }
                    }
                    if (event.was_handled) break;
                }
            }

            nya_event_dispatch((NYA_Event){
                .type = NYA_EVENT_HANDLING_ENDED,
            });
        }

        _nya_app_update();
        _nya_app_render();

        // end of frame tasks
        {
            app->frame_stats.frame_end_time_ns  = nya_clock_get_monotonic_ns();
            app->frame_stats.prev_frame_time_ns = app->frame_stats.frame_start_time_ns;
            app->frame_stats.fps                = 1.0F / (f32)nya_time_ns_to_s(app->frame_stats.elapsed_ns);

            // Observers read the frame's records here, then the records are dropped. Before the
            // frame allocator is reset, so an observer may still touch anything a layer put there.
            nya_system_sim_end_frame();

            nya_arena_free_all(app->frame_allocator);

            nya_event_dispatch((NYA_Event){
                .type = NYA_EVENT_FRAME_ENDED,
            });
        }

        /*
         * Framerate limiting, against the work *this* frame did.
         */
        // Recorded rather than computed and dropped. "What did this frame cost" is the first question
        // anyone asks of a profiler, and the limiter is the only place that already knows.
        app->frame_stats.work_ns  = app->frame_stats.frame_end_time_ns - app->frame_stats.frame_start_time_ns;
        app->frame_stats.sleep_ns = 0;

        /*
         * The unfocused cap wins while nothing has focus, and applies even under vsync.
         *
         * vsync caps at the display's rate; this asks for slower than that, so sleeping here lands before
         * the swap would have blocked rather than fighting it. The focused cap keeps its old condition,
         * because asking for *faster* than vsync is the case the two really do disagree about.
         */
        u64 floor_ns = 0;

        if (app->options.unfocused_frame_rate_limit > 0 && !_nya_app_any_window_has_focus()) {
            floor_ns = 1'000'000'000 / (u64)app->options.unfocused_frame_rate_limit;
        } else if (!app->options.vsync_enabled && app->options.frame_rate_limit > 0) {
            floor_ns = app->frame_stats.min_frame_time_ns;
        }

        if (floor_ns > 0 && app->frame_stats.work_ns < floor_ns) {
            app->frame_stats.sleep_ns = floor_ns - app->frame_stats.work_ns;
            SDL_DelayNS(app->frame_stats.sleep_ns);
        }
    }
}

void _nya_app_advance_frame_clock(void) {
    NYA_App* app = nya_app_get();

    app->frame_stats.uptime_ns            = nya_app_uptime_ns();
    app->frame_stats.uptime_s             = (f32)nya_time_ns_to_s(app->frame_stats.uptime_ns);
    app->frame_stats.frame_start_time_ns  = nya_clock_get_monotonic_ns();
    app->frame_stats.elapsed_ns           = app->frame_stats.frame_start_time_ns - app->frame_stats.prev_frame_time_ns;
    app->frame_stats.time_behind_ns      += (s64)app->frame_stats.elapsed_ns;

    // Debt the update loop could not pay off is dropped rather than carried. See
    // _NYA_APP_MAX_CATCH_UP_TICKS: carrying it is what turns one slow frame into a permanent one.
    s64 max_debt_ns = (s64)app->options.time_step_ns * _NYA_APP_MAX_CATCH_UP_TICKS;
    if (app->frame_stats.time_behind_ns > max_debt_ns) app->frame_stats.time_behind_ns = max_debt_ns;
}

void _nya_app_update(void) {
    NYA_App* app = nya_app_get();

    while (app->frame_stats.time_behind_ns >= (s64)app->options.time_step_ns) {
        nya_perf_time_this_scope("frame_updating");
        nya_event_dispatch((NYA_Event){
            .type = NYA_EVENT_UPDATING_STARTED,
        });

        // Once per tick, before anything reads it. This is the fixed step and nothing about it
        // varies per window or per layer, so assigning it inside those loops only meant that an app
        // with no layer to update left the field holding whatever the last differently configured
        // tick put there — and every observer of NYA_EVENT_UPDATING_STARTED sees it.
        app->frame_stats.delta_time_s = (f32)nya_time_ns_to_s(app->options.time_step_ns);

        /*
         * The solver runs at the top of the tick, before anything reads the world.
         */
        nya_system_physics2d_update(app->frame_stats.delta_time_s);

        // Both worlds, every tick, in a fixed order. A scene uses one of them and the other steps
        // over an empty body list, which nya_system_physics3d_update short circuits — so a purely 2D
        // game pays a branch for the 3D world it never touched.
        nya_system_physics3d_update(app->frame_stats.delta_time_s);

        for (u32 slot = 0; slot < NYA_WINDOW_MAX; slot++) {
            NYA_Window* window = nya_window_at_slot(slot);
            if (window == nullptr) continue;

            nya_array_foreach (window->layer_stack, layer) {
                NYA_LayerOnUpdateFn on_update_fn = nya_callback_get(layer->on_update);
                if (!layer->enabled || on_update_fn == nullptr) continue;

                // A layer's id is the string literal it was declared with, so it doubles as the span
                // name — which is what turns "frame_updating took 2 ms" into "which layer".
                nya_perf_time_this_scope((NYA_ConstCString)layer->id);

                on_update_fn(window, app->frame_stats.delta_time_s);
            }
        }

        /*
         * Tweens after the layers and before the entities, and the ordering is load-bearing in both
         * directions.
         */
        nya_system_tween_update(app->frame_stats.delta_time_s);

        // Entities update after the layers, so a layer that spawns something gets it
        // simulated on the same tick rather than a frame late.
        nya_system_entity_update(app->frame_stats.delta_time_s);

#ifndef NYA_NO_SDL
        /*
         * Networking, after everything that changes the world and before the barrier.
         */
        nya_net_server_tick(nya_world()->sim_system.tick, app->frame_stats.delta_time_s);
        nya_net_client_tick(nya_world()->sim_system.tick, app->frame_stats.delta_time_s);
#endif

        // The barrier. Every update for this tick has run, so nothing is mid iteration and
        // the queued mutations can be applied without changing what anyone is walking.
        nya_system_sim_apply_commands();
        nya_world()->sim_system.tick++;

        app->frame_stats.time_behind_ns -= (s64)app->options.time_step_ns;
        nya_event_dispatch((NYA_Event){
            .type = NYA_EVENT_UPDATING_ENDED,
        });
    }
}

b8 _nya_app_any_window_has_focus(void) {
    for (u32 slot = 0; slot < NYA_WINDOW_MAX; slot++) {
        NYA_Window* window = nya_window_at_slot(slot);
        if (window == nullptr) continue;

        if (nya_window_has_focus(window->handle)) return true;
    }

    return false;
}

void _nya_app_render(void) {
    nya_perf_time_this_scope("frame_rendering");
    nya_event_dispatch((NYA_Event){
        .type = NYA_EVENT_RENDERING_STARTED,
    });

    for (u32 slot = 0; slot < NYA_WINDOW_MAX; slot++) {
        NYA_Window* window = nya_window_at_slot(slot);
        if (window == nullptr) continue;

        // Nothing to draw into: the window is minimised, occluded, or its swapchain is mid
        // resize. Skipping the layers is the point — drawing anyway used to land in the
        // previous frame's render pass, whose texture was the previous window size.
        if (!nya_render_begin(window)) continue;

        nya_array_foreach (window->layer_stack, layer) {
            NYA_LayerOnRenderFn on_render_fn = nya_callback_get(layer->on_render);
            if (!layer->enabled || on_render_fn == nullptr) continue;

            // Same as the update loop: the layer's id names its span, so the render breakdown says
            // which layer rather than only how long all of them took together.
            nya_perf_time_this_scope((NYA_ConstCString)layer->id);

            on_render_fn(window);
        }

        nya_render_end(window);
    }

    nya_event_dispatch((NYA_Event){
        .type = NYA_EVENT_RENDERING_ENDED,
    });
}

void _nya_app_frame_step(b8 live_resize) {
    NYA_App* app = nya_app_get();

    NYA_Arena* outer_allocator = app->frame_allocator;
    if (live_resize) {
        // The outer frame's allocations have to survive the drag, so a nested step gets its own
        // arena and empties that one instead.
        nya_arena_free_all(app->live_resize_allocator);
        app->frame_allocator = app->live_resize_allocator;
    }

    _nya_app_advance_frame_clock();
    _nya_app_update();
    _nya_app_render();

    // Same bookkeeping the outer loop does at the end of a frame, minus the frame allocator reset.
    // Without it the drag's whole duration arrives as one delta when the mouse comes up, and the
    // fixed timestep loop pays that debt off as a burst of catch up ticks.
    app->frame_stats.frame_end_time_ns  = nya_clock_get_monotonic_ns();
    app->frame_stats.prev_frame_time_ns = app->frame_stats.frame_start_time_ns;
    if (app->frame_stats.elapsed_ns > 0) app->frame_stats.fps = 1.0F / (f32)nya_time_ns_to_s(app->frame_stats.elapsed_ns);

    if (live_resize) app->frame_allocator = outer_allocator;
}

bool SDLCALL _nya_app_live_resize_event_watch(void* userdata, SDL_Event* event) {
    nya_unused(userdata);

    // Watchers see every event on the way in, including the ones this frame is about to produce, so
    // a step must never start inside another one.
    static b8 stepping = false;

    // data1 == 0 is an ordinary expose, which the main loop is awake to handle by itself.
    b8 is_live_resize_expose = event->type == SDL_EVENT_WINDOW_EXPOSED && event->window.data1 == 1;
    if (!is_live_resize_expose || stepping) return true;

    NYA_App* app = &_NYA_APP_INSTANCE;
    if (!app->initialized || app->should_quit) return true;

    stepping = true;
    _nya_app_frame_step(true);
    stepping = false;

    return true;
}

NYA_App* nya_app_get(void) {
    nya_assert(_NYA_APP_INSTANCE.initialized);
    return &_NYA_APP_INSTANCE;
}

void nya_app_options_update(NYA_AppOptions options) {
    nya_assert(options.time_step_ns > 0, "time_step_ns must be greater than 0");
    nya_assert(options.frame_rate_limit > 0, "frame_rate_limit must be greater than 0");
    nya_assert(options.max_concurrent_jobs > 0, "max_concurrent_jobs must be greater than 0");

    NYA_App* app = nya_app_get();

    nya_system_renderer_set_vsync(options.vsync_enabled);

    app->options                       = options;
    app->frame_stats.min_frame_time_ns = 1'000'000'000 / (u64)options.frame_rate_limit;
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

NYA_INTERNAL void _nya_app_handle_shutdown_signal(NYA_Signal signal) {
    nya_unused(signal);

    NYA_App* app     = nya_app_get();
    app->should_quit = true;
}
