#include "SDL3/SDL_init.h"
#include "SDL3/SDL_timer.h"

#include "nyangine/nyangine.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API DECLARATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

NYA_App _NYA_APP_INSTANCE;

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
 * Whether any live window holds input focus. Drives the unfocused frame cap. Any rather than all, so a
 * focused tool window does not slow the game beside it.
 * */
NYA_INTERNAL b8 _nya_app_any_window_has_focus(void) __attr_no_discard;

/**
 * Renders, and keeps simulating, while the window is being dragged by its edge.
 * */
NYA_INTERNAL bool SDLCALL _nya_app_live_resize_event_watch(void* userdata, SDL_Event* event);

/*
 * Audio propagation traces the physics worlds through these. Wired here so neither module knows the other: a miss
 * and a body with no entity both read as open.
 */
NYA_INTERNAL void _nya_app_audio_rays_3d(const NYA_AudioRay* rays, f32* out_fractions, u32 count, void* user_data);
NYA_INTERNAL void _nya_app_audio_rays_2d(const NYA_AudioRay* rays, f32* out_fractions, u32 count, void* user_data);

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * SUBSYSTEMS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/** Registers every engine subsystem with core_system.h's registry, in bring-up order. */
NYA_INTERNAL void _nya_app_register_subsystems(void);

NYA_INTERNAL NYA_Error _nya_app_bring_up_logfile(void) {
#ifdef NYA_LOG_DIRECTORY
    NYA_Error opened = nya_log_directory_open(NYA_LOG_DIRECTORY, NYA_LOG_RETENTION_DAYS);
#else
    NYA_Arena*  arena  = nya_arena_create(.name = "log_directory");
    defer       nya_arena_destroy(arena);
    NYA_String* root   = nullptr;
    NYA_Error   opened = nya_filesystem_user_data_directory(arena, nya_save_application(), &root);
    if (opened.ok) {
        NYA_String* directory = nya_path_join(arena, nya_string_to_cstring(arena, root), "logs");
        opened                = nya_log_directory_open(nya_string_to_cstring(arena, directory), NYA_LOG_RETENTION_DAYS);
        if (opened.ok) nya_log_info("Logging to '%s'.", nya_string_to_cstring(arena, directory));
    }
#endif
    if (!opened.ok) nya_log_warn("Continuing without a log file: %s", (NYA_ConstCString)opened.message);
    return NYA_OK;
}

NYA_INTERNAL NYA_Error _nya_app_bring_up_save(void) { (void)nya_system_save_init(); return NYA_OK; }
NYA_INTERNAL NYA_Error _nya_app_bring_up_settings(void) { nya_system_settings_init(); return NYA_OK; }
NYA_INTERNAL NYA_Error _nya_app_bring_up_job(void) { return nya_system_job_init(); }
NYA_INTERNAL NYA_Error _nya_app_bring_up_callback(void) { nya_system_callback_init(); return NYA_OK; }
NYA_INTERNAL NYA_Error _nya_app_bring_up_tween(void) { nya_system_tween_init(); return NYA_OK; }
NYA_INTERNAL NYA_Error _nya_app_bring_up_renderer(void) { return nya_system_renderer_init(); }
NYA_INTERNAL NYA_Error _nya_app_bring_up_window(void) { nya_system_window_init(); return NYA_OK; }
NYA_INTERNAL NYA_Error _nya_app_bring_up_events(void) { return nya_system_events_init(); }
NYA_INTERNAL NYA_Error _nya_app_bring_up_input(void) { nya_system_input_init(); return NYA_OK; }
NYA_INTERNAL NYA_Error _nya_app_bring_up_gamepad(void) { nya_system_gamepad_init(); return NYA_OK; }
NYA_INTERNAL NYA_Error _nya_app_bring_up_asset(void) { nya_system_asset_init(); return NYA_OK; }
NYA_INTERNAL NYA_Error _nya_app_bring_up_i18n(void) { nya_system_i18n_init(); return NYA_OK; }
NYA_INTERNAL NYA_Error _nya_app_bring_up_config(void) { nya_system_config_init(); return NYA_OK; }
NYA_INTERNAL NYA_Error _nya_app_bring_up_audio(void) {
    NYA_Error error = nya_system_audio_init();
    nya_audio_rays_set(NYA_AUDIO_SPACE_3D, _nya_app_audio_rays_3d, nullptr);
    nya_audio_rays_set(NYA_AUDIO_SPACE_2D, _nya_app_audio_rays_2d, nullptr);
    return error;
}

#ifdef NYA_PLUGIN_STEAM
// never fails: a player without the client still plays.
NYA_INTERNAL NYA_Error _nya_app_bring_up_steam(void) { if (nya_app_get()->options.steam_app_id != 0) (void)nya_system_steam_init(); return NYA_OK; }
NYA_INTERNAL void _nya_app_tear_down_steam(void) { nya_system_steam_deinit(); }
#endif

NYA_INTERNAL NYA_Error _nya_app_bring_up_world(void) {
    NYA_App* app = nya_app_get();
    app->world   = nya_world_create();
    (void)nya_world_set(app->world);
    return NYA_OK;
}

NYA_INTERNAL void _nya_app_tear_down_save(void) { nya_system_save_deinit(); }
NYA_INTERNAL void _nya_app_tear_down_logfile(void) { nya_log_file_close(); }
NYA_INTERNAL void _nya_app_tear_down_settings(void) { nya_system_settings_deinit(); }
NYA_INTERNAL void _nya_app_tear_down_job(void) { nya_system_job_deinit(); }
NYA_INTERNAL void _nya_app_tear_down_callback(void) { nya_system_callback_deinit(); }
NYA_INTERNAL void _nya_app_tear_down_tween(void) { nya_system_tween_deinit(); }
NYA_INTERNAL void _nya_app_tear_down_renderer(void) { nya_system_renderer_deinit(); }
NYA_INTERNAL void _nya_app_tear_down_window(void) { nya_system_window_deinit(); }
NYA_INTERNAL void _nya_app_tear_down_events(void) { nya_system_events_deinit(); }
NYA_INTERNAL void _nya_app_tear_down_input(void) { nya_system_input_deinit(); }
NYA_INTERNAL void _nya_app_tear_down_asset(void) { nya_system_asset_deinit(); }
NYA_INTERNAL void _nya_app_tear_down_gamepad(void) { nya_system_gamepad_deinit(); }
NYA_INTERNAL void _nya_app_tear_down_i18n(void) { nya_system_i18n_deinit(); }
NYA_INTERNAL void _nya_app_tear_down_config(void) { nya_system_config_deinit(); }
NYA_INTERNAL void _nya_app_tear_down_audio(void) { nya_system_audio_deinit(); }

NYA_INTERNAL void _nya_app_tear_down_world(void) {
    NYA_App* app = nya_app_get();
    (void)nya_world_set(nullptr);
    nya_world_destroy(app->world);
    app->world = nullptr;
}

/**
 * Registers every engine subsystem in bring-up order, each chained `after` the previous one so
 * nya_system_registry_finalize can only produce this order. Teardown runs in reverse.
 * */
void _nya_app_register_subsystems(void) {
    // first up and last down, so every other subsystem's log lines reach the file.
    nya_system_register((NYA_SystemEntry){ .name = "logfile", .init = _nya_app_bring_up_logfile, .deinit = _nya_app_tear_down_logfile });

    // before settings, which it feeds. settings cannot fail: it owns no memory and loads defaults when nothing
    // was saved. on the way out it writes into the save system's directory.
    nya_system_register((NYA_SystemEntry){ .name = "save", .after = "logfile", .init = _nya_app_bring_up_save, .deinit = _nya_app_tear_down_save });
    nya_system_register((NYA_SystemEntry){ .name         = "settings",
                                            .after        = "save",
                                            .init         = _nya_app_bring_up_settings,
                                            .deinit       = _nya_app_tear_down_settings });

#ifdef NYA_PLUGIN_STEAM
    // early, so the overlay can hook the renderer's device when it is created.
    nya_system_register((NYA_SystemEntry){ .name = "steam", .after = "settings", .init = _nya_app_bring_up_steam, .deinit = _nya_app_tear_down_steam });
#endif

    // before job, so the workers stop before the registry they resolve through is freed.
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

    // after the asset system: a locale file is read and watched through the registry.
    nya_system_register((NYA_SystemEntry){ .name = "i18n", .after = "asset", .init = _nya_app_bring_up_i18n, .deinit = _nya_app_tear_down_i18n });

    // after the asset system: a config file is read and, under hot reload, watched through the registry.
    nya_system_register((NYA_SystemEntry){ .name = "config", .after = "i18n", .init = _nya_app_bring_up_config, .deinit = _nya_app_tear_down_config });

    // after the asset system, which creates the mixer these tracks use and destroys it after them.
    nya_system_register((NYA_SystemEntry){ .name = "audio", .after = "config", .init = _nya_app_bring_up_audio, .deinit = _nya_app_tear_down_audio });

    nya_system_register((NYA_SystemEntry){ .name = "world", .after = "audio", .init = _nya_app_bring_up_world, .deinit = _nya_app_tear_down_world });

    // last up, first down. the world is entities, physics and the simulation barrier as one lifetime.
    nya_system_register((NYA_SystemEntry){ .name = "window", .after = "world", .init = _nya_app_bring_up_window, .deinit = _nya_app_tear_down_window });

    // a finalize failure is a typo in an `after` string above, which only this function writes, so it asserts.
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

    // uptime starts here, so it includes the integrity check and SDL_Init below.
    u64 started_ns = nya_clock_get_monotonic_ns();

#ifdef NYA_PLUGIN_STEAM
    // before anything is brought up, since Steam starts a new copy of the game and this one has nothing to undo.
    if (options.steam_app_id != 0 && nya_system_steam_restart_if_necessary(options.steam_app_id)) {
        nya_log_info("Started outside Steam; relaunching through the Steam client.");
        exit(EXIT_SUCCESS);
    }
#endif

    // as early as possible: a code baseline only means something before anything could hook the process.
    nya_integrity_start();

    nya_signals_init();
    nya_signals_set_handler(NYA_SIGNAL_HANGUP, _nya_app_handle_shutdown_signal);
    nya_signals_set_handler(NYA_SIGNAL_INTERRUPT, _nya_app_handle_shutdown_signal);
    nya_signals_set_handler(NYA_SIGNAL_TERMINATE, _nya_app_handle_shutdown_signal);

    if (options.app_id != nullptr) SDL_SetHint(SDL_HINT_APP_ID, options.app_id);

#if OS_LINUX
    // native Wayland first. SDL falls back to X11 on compositors missing protocols it wants, which runs the
    // game through XWayland. A hint rather than an override, so SDL_VIDEO_DRIVER in the environment still wins.
    SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "wayland,x11");
#endif

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
        .frame_stats.started_ns         = started_ns,
        .frame_stats.prev_frame_time_ns = nya_clock_get_monotonic_ns(),
        .frame_stats.min_frame_time_ns  = 1'000'000'000 / (u64)options.frame_rate_limit,
    };

    nya_log_info("Nyangine initialized in %.1f ms. Initializing subsystems...", nya_time_ns_to_ms(nya_clock_get_monotonic_ns() - started_ns));

    _nya_app_register_subsystems();

    // Brought up in order and unwound in reverse of however far it got, so a failure leaves nothing half built.
    // Not nya_system_registry_run_init, because its deinit tears down everything registered, not just what came
    // up; see core_system.h.
    NYA_Error result     = NYA_OK;
    u32       brought_up = 0;

    for (; brought_up < nya_system_registry_count(); brought_up++) {
        NYA_SystemInitFn init       = nya_system_registry_init_at(brought_up);
        u64              init_start = nya_clock_get_monotonic_ns();
        result                      = init != nullptr ? init() : NYA_OK;
        if (!result.ok) goto unwind;

        nya_log_debug("Brought up '%s' in %.1f ms.", nya_system_registry_name_at(brought_up), nya_time_ns_to_ms(nya_clock_get_monotonic_ns() - init_start));
    }

    // after the renderer and windows, since the watcher draws. not fatal: it only costs frames during a resize
    // drag.
    if (!SDL_AddEventWatch(_nya_app_live_resize_event_watch, nullptr)) {
        nya_log_warn("SDL_AddEventWatch() failed, the window will not redraw while being resized: %s", SDL_GetError());
    }

    nya_log_info("Subsystems initialized after %.1f ms.", nya_time_ns_to_ms(nya_app_uptime_ns()));
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

f32 nya_app_tick_alpha(void) {
    NYA_App* app = nya_app_get();
    if (!app->initialized || app->options.time_step_ns == 0) return 1.0F;

    return nya_clamp((f32)app->frame_stats.time_behind_ns / (f32)app->options.time_step_ns, 0.0F, 1.0F);
}

f64 nya_app_uptime_s(void) {
    return (f64)nya_app_uptime_ns() / 1'000'000'000.0;
}

void nya_app_deinit(void) {
    NYA_App* app = nya_app_get();

    nya_log_info("Deinitializing subsystems...");

    // Before the renderer goes away, or a late expose could still ask a dead device to draw.
    SDL_RemoveEventWatch(_nya_app_live_resize_event_watch, nullptr);

    // the registered order in reverse. run_deinit is safe here: everything came up.
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

    // startup time is the number a player feels first, so it is reported once.
    static b8 first_frame_reported = false;

    while (!app->should_quit) {
        // before the frame timer opens, so that timer is the frame's depth 0 span. nya_perf_frame_spans selects on
        // this frame number.
        nya_perf_frame_begin();

        nya_perf_time_this_scope("frame");

        {
            nya_event_dispatch((NYA_Event){
                .type = NYA_EVENT_FRAME_STARTED,
            });

            _nya_app_advance_frame_clock();

            // does nothing until the UTC date changes; without it a run across midnight logs every later day into the
            // first file.
            nya_log_directory_roll();

            nya_system_gamepad_frame_begin();

#ifdef NYA_PLUGIN_STEAM
            nya_system_steam_update();
#endif
        }

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

        {
            app->frame_stats.frame_end_time_ns  = nya_clock_get_monotonic_ns();
            app->frame_stats.prev_frame_time_ns = app->frame_stats.frame_start_time_ns;
            app->frame_stats.fps                = 1.0F / (f32)nya_time_ns_to_s(app->frame_stats.elapsed_ns);

            // observers read the frame's records, which are then dropped, before the frame allocator resets.
            nya_system_sim_end_frame();

            nya_arena_free_all(app->frame_allocator);

            nya_event_dispatch((NYA_Event){
                .type = NYA_EVENT_FRAME_ENDED,
            });

            // the sweep and a second copy of the watchdog, which also runs every tick, so neither is one patch away.
            nya_integrity_sweep(app->frame_stats.frame_end_time_ns);
            nya_integrity_watchdog(app->frame_stats.frame_end_time_ns);

            if (!first_frame_reported) {
                first_frame_reported = true;
                nya_log_info("First frame presented after %.1f ms.", nya_time_ns_to_ms(nya_app_uptime_ns()));
            }
        }

        /* Framerate limiting, against the work this frame did. */
        // recorded: "what did this frame cost" is the first question a profiler gets.
        app->frame_stats.work_ns  = app->frame_stats.frame_end_time_ns - app->frame_stats.frame_start_time_ns;
        app->frame_stats.sleep_ns = 0;

        /*
         * The unfocused cap applies whenever nothing has focus, even under vsync, since it asks for slower than the
         * display rate. The focused cap still defers to vsync.
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

    // unpaid debt is dropped; carrying it turns one slow frame into permanent catch-up.
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

        // set once per tick before anything reads it, so every observer of NYA_EVENT_UPDATING_STARTED sees this
        // tick's value.
        app->frame_stats.delta_time_s = (f32)nya_time_ns_to_s(app->options.time_step_ns);

        nya_integrity_watchdog(app->frame_stats.frame_start_time_ns);

        // before anything moves an entity, so a draw between this tick and the next starts from here.
        nya_system_entity_transforms_capture();

        /* The solver runs at the top of the tick, before anything reads the world. */
        nya_system_physics2d_update(app->frame_stats.delta_time_s);

        // both worlds every tick; an empty one returns immediately.
        nya_system_physics3d_update(app->frame_stats.delta_time_s);

        for (u32 slot = 0; slot < NYA_WINDOW_MAX; slot++) {
            NYA_Window* window = nya_window_at_slot(slot);
            if (window == nullptr) continue;

            nya_array_foreach (window->layer_stack, layer) {
                NYA_LayerOnUpdateFn on_update_fn = nya_callback_get(layer->on_update);
                if (!layer->enabled || on_update_fn == nullptr) continue;

                // a layer's id is its span name, so the breakdown says which layer.
                nya_perf_time_this_scope(layer->id);

                on_update_fn(window, app->frame_stats.delta_time_s);
            }
        }

        /*
         * Tweens after the layers and before the entities: layers start tweens this tick, and entities read the
         * values tweens write.
         */
        nya_system_tween_update(app->frame_stats.delta_time_s);

        // entities after the layers, so something a layer spawns is simulated this tick.
        nya_system_entity_update(app->frame_stats.delta_time_s);

#ifndef NYA_NO_SDL
        /* Networking, after everything that changes the world and before the barrier. */
        nya_net_server_tick(nya_world()->sim_system.tick, app->frame_stats.delta_time_s);
        nya_net_client_tick(nya_world()->sim_system.tick, app->frame_stats.delta_time_s);
#endif

        // the barrier: every update has run, so queued mutations apply without disturbing iteration.
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

        // nothing to draw into: minimised, occluded or mid resize. drawing anyway would target last frame's pass.
        if (!nya_render_begin(window)) continue;

        nya_array_foreach (window->layer_stack, layer) {
            NYA_LayerOnRenderFn on_render_fn = nya_callback_get(layer->on_render);
            if (!layer->enabled || on_render_fn == nullptr) continue;

            // the layer's id names its span.
            nya_perf_time_this_scope(layer->id);

            on_render_fn(window);
        }

        nya_render_end(window);
    }

    nya_event_dispatch((NYA_Event){
        .type = NYA_EVENT_RENDERING_ENDED,
    });

    // after drawing, which is where layers place the listener. once a frame: it is heard, not simulated.
    nya_system_audio_update((f32)nya_time_ns_to_s(nya_app_get()->frame_stats.elapsed_ns));
}

void _nya_app_audio_rays_3d(const NYA_AudioRay* rays, f32* out_fractions, u32 count, void* user_data) {
    nya_unused(user_data);

    for (u32 i = 0; i < count; i++) {
        f32 length = nya_vector_length(rays[i].direction);

        out_fractions[i] = 1.0F;
        if (length < NYA_EPSILON) continue;

        f32x3 point = { 0 };
        if (nya_entity_is_valid(nya_physics3d_raycast(rays[i].origin, rays[i].direction, &point, nullptr))) {
            out_fractions[i] = nya_vector_length(point - rays[i].origin) / length;
        }
    }
}

void _nya_app_audio_rays_2d(const NYA_AudioRay* rays, f32* out_fractions, u32 count, void* user_data) {
    nya_unused(user_data);

    for (u32 i = 0; i < count; i++) {
        f32x2 origin    = { rays[i].origin.x, rays[i].origin.y };
        f32x2 direction = { rays[i].direction.x, rays[i].direction.y };
        f32   length    = nya_vector_length(direction);

        out_fractions[i] = 1.0F;
        if (length < NYA_EPSILON) continue;

        f32x2 point = { 0 };
        if (nya_entity_is_valid(nya_physics2d_raycast(origin, direction, &point, nullptr))) {
            out_fractions[i] = nya_vector_length(point - origin) / length;
        }
    }
}

void _nya_app_frame_step(b8 live_resize) {
    NYA_App* app = nya_app_get();

    NYA_Arena* outer_allocator = app->frame_allocator;
    if (live_resize) {
        // the outer frame's allocations must survive the drag, so a nested step uses its own arena.
        nya_arena_free_all(app->live_resize_allocator);
        app->frame_allocator = app->live_resize_allocator;
    }

    _nya_app_advance_frame_clock();
    _nya_app_update();
    _nya_app_render();

    // the outer loop's end-of-frame bookkeeping, without the allocator reset. otherwise the whole drag arrives as
    // one delta and the fixed step catches up in a burst.
    app->frame_stats.frame_end_time_ns  = nya_clock_get_monotonic_ns();
    app->frame_stats.prev_frame_time_ns = app->frame_stats.frame_start_time_ns;
    if (app->frame_stats.elapsed_ns > 0) app->frame_stats.fps = 1.0F / (f32)nya_time_ns_to_s(app->frame_stats.elapsed_ns);

    if (live_resize) app->frame_allocator = outer_allocator;
}

bool SDLCALL _nya_app_live_resize_event_watch(void* userdata, SDL_Event* event) {
    nya_unused(userdata);

    // watchers see events on the way in, so a step must never start inside another.
    static b8 stepping = false;

    // data1 == 0 is an ordinary expose, which the main loop handles.
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

void _nya_app_handle_shutdown_signal(NYA_Signal signal) {
    nya_unused(signal);

    NYA_App* app     = nya_app_get();
    app->should_quit = true;
}
