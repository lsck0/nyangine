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

/** Monotonic nanoseconds from whichever clock is installed. The one place the loop reads time. */
NYA_INTERNAL u64 _nya_app_now_ns(void);

/** Runs the registry's tick phase until the debt is paid off. */
NYA_INTERNAL void _nya_app_update(void);

/** Runs the registry's render phase, once. */
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

/*
 * Everything from here to the end of the PHASES section is registered with nya_callback and so is
 * NYA_INTERNAL_CALLBACK: internal in a build that cannot reload code, and visible in one that can,
 * because dlsym and GetProcAddress find neither a static nor a hidden symbol. clang-tidy sees only
 * that each is used in one translation unit and asks for `static`, which is the one thing they must
 * not be. Same reason as the single sites in core_input.c and core_asset.c.
 */
// NOLINTBEGIN(misc-use-internal-linkage)

NYA_INTERNAL_CALLBACK NYA_Error _nya_app_bring_up_logfile(void) {
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

NYA_INTERNAL_CALLBACK NYA_Error _nya_app_bring_up_crash_reporter(void) { return nya_crash_reporter_init(); }
NYA_INTERNAL_CALLBACK void _nya_app_tear_down_crash_reporter(void) { nya_crash_reporter_deinit(); }

NYA_INTERNAL_CALLBACK NYA_Error _nya_app_bring_up_save(void) { (void)nya_system_save_init(); return NYA_OK; }
NYA_INTERNAL_CALLBACK NYA_Error _nya_app_bring_up_settings(void) { nya_system_settings_init(); return NYA_OK; }
NYA_INTERNAL_CALLBACK NYA_Error _nya_app_bring_up_job(void) { return nya_system_job_init(); }
NYA_INTERNAL_CALLBACK NYA_Error _nya_app_bring_up_tween(void) { nya_system_tween_init(); return NYA_OK; }
NYA_INTERNAL_CALLBACK NYA_Error _nya_app_bring_up_renderer(void) { return nya_system_renderer_init(); }
NYA_INTERNAL_CALLBACK NYA_Error _nya_app_bring_up_window(void) { nya_system_window_init(); return NYA_OK; }
NYA_INTERNAL_CALLBACK NYA_Error _nya_app_bring_up_events(void) { return nya_system_events_init(); }
NYA_INTERNAL_CALLBACK NYA_Error _nya_app_bring_up_input(void) { nya_system_input_init(); return NYA_OK; }
NYA_INTERNAL_CALLBACK NYA_Error _nya_app_bring_up_gamepad(void) { nya_system_gamepad_init(); return NYA_OK; }
NYA_INTERNAL_CALLBACK NYA_Error _nya_app_bring_up_asset(void) { nya_system_asset_init(); return NYA_OK; }
NYA_INTERNAL_CALLBACK NYA_Error _nya_app_bring_up_i18n(void) { nya_system_i18n_init(); return NYA_OK; }
NYA_INTERNAL_CALLBACK NYA_Error _nya_app_bring_up_config(void) { nya_system_config_init(); return NYA_OK; }
NYA_INTERNAL_CALLBACK NYA_Error _nya_app_bring_up_audio(void) {
    NYA_Error error = nya_system_audio_init();
    nya_audio_rays_set(NYA_AUDIO_SPACE_3D, _nya_app_audio_rays_3d, nullptr);
    nya_audio_rays_set(NYA_AUDIO_SPACE_2D, _nya_app_audio_rays_2d, nullptr);
    return error;
}

#ifdef NYA_PLUGIN_STEAM
// never fails: a player without the client still plays.
NYA_INTERNAL_CALLBACK NYA_Error _nya_app_bring_up_steam(void) { if (nya_app_get()->options.steam_app_id != 0) (void)nya_system_steam_init(); return NYA_OK; }
NYA_INTERNAL_CALLBACK void _nya_app_tear_down_steam(void) { nya_system_steam_deinit(); }
NYA_INTERNAL_CALLBACK void _nya_app_frame_steam(f32 delta_time_s) { nya_unused(delta_time_s); nya_system_steam_update(); }
#endif

NYA_INTERNAL_CALLBACK NYA_Error _nya_app_bring_up_world(void) {
    NYA_App* app = nya_app_get();
    app->world   = nya_world_create();
    (void)nya_world_set(app->world);
    return NYA_OK;
}

NYA_INTERNAL_CALLBACK void _nya_app_tear_down_save(void) { nya_system_save_deinit(); }
NYA_INTERNAL_CALLBACK void _nya_app_tear_down_logfile(void) { nya_log_file_close(); }
NYA_INTERNAL_CALLBACK void _nya_app_tear_down_settings(void) { nya_system_settings_deinit(); }
NYA_INTERNAL_CALLBACK void _nya_app_tear_down_job(void) { nya_system_job_deinit(); }
NYA_INTERNAL_CALLBACK void _nya_app_tear_down_tween(void) { nya_system_tween_deinit(); }
NYA_INTERNAL_CALLBACK void _nya_app_tear_down_renderer(void) { nya_system_renderer_deinit(); }
NYA_INTERNAL_CALLBACK void _nya_app_tear_down_window(void) { nya_system_window_deinit(); }
NYA_INTERNAL_CALLBACK void _nya_app_tear_down_events(void) { nya_system_events_deinit(); }
NYA_INTERNAL_CALLBACK void _nya_app_tear_down_input(void) { nya_system_input_deinit(); }
NYA_INTERNAL_CALLBACK void _nya_app_tear_down_asset(void) { nya_system_asset_deinit(); }
NYA_INTERNAL_CALLBACK void _nya_app_tear_down_gamepad(void) { nya_system_gamepad_deinit(); }
NYA_INTERNAL_CALLBACK void _nya_app_tear_down_i18n(void) { nya_system_i18n_deinit(); }
NYA_INTERNAL_CALLBACK void _nya_app_tear_down_config(void) { nya_system_config_deinit(); }
NYA_INTERNAL_CALLBACK void _nya_app_tear_down_audio(void) { nya_system_audio_deinit(); }

NYA_INTERNAL_CALLBACK void _nya_app_tear_down_world(void) {
    NYA_App* app = nya_app_get();
    (void)nya_world_set(nullptr);
    nya_world_destroy(app->world);
    app->world = nullptr;
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PHASES
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 *
 * The frame is a list of registered systems rather than a list of calls, so a plugin can slot into it and a
 * game can switch one off. Each of these is one line of what used to be hardcoded in _nya_app_update,
 * _nya_app_render and nya_app_run, in the same order.
 */

NYA_INTERNAL_CALLBACK void _nya_app_frame_logfile(f32 delta_time_s) {
    nya_unused(delta_time_s);

    // does nothing until the UTC date changes; without it a run across midnight logs every later day into the
    // first file.
    nya_log_directory_roll();
}

NYA_INTERNAL_CALLBACK void _nya_app_frame_gamepad(f32 delta_time_s) {
    nya_unused(delta_time_s);
    nya_system_gamepad_frame_begin();
}

NYA_INTERNAL_CALLBACK void _nya_app_tick_entity_transforms(f32 delta_time_s) {
    nya_unused(delta_time_s);

    // before anything moves an entity, so a draw between this tick and the next starts from here.
    nya_system_entity_transforms_capture();
}

/* The solver runs at the top of the tick, before anything reads the world. */
NYA_INTERNAL_CALLBACK void _nya_app_tick_physics2d(f32 delta_time_s) { nya_system_physics2d_update(delta_time_s); }

/* both worlds every tick; an empty one returns immediately. */
NYA_INTERNAL_CALLBACK void _nya_app_tick_physics3d(f32 delta_time_s) { nya_system_physics3d_update(delta_time_s); }

NYA_INTERNAL_CALLBACK void _nya_app_tick_layers(f32 delta_time_s) {
    for (u32 slot = 0; slot < NYA_WINDOW_MAX; slot++) {
        NYA_Window* window = nya_window_at_slot(slot);
        if (window == nullptr) continue;

        nya_array_foreach (window->layer_stack, layer) {
            NYA_LayerOnUpdateFn on_update_fn = nya_callback_get(layer->on_update);
            if (!layer->enabled || on_update_fn == nullptr) continue;

            // a layer's id is its span name, so the breakdown says which layer.
            nya_perf_time_this_scope(layer->id);

            on_update_fn(window, delta_time_s);
        }
    }
}

/*
 * Tweens after the layers and before the entities: layers start tweens this tick, and entities read the
 * values tweens write.
 */
NYA_INTERNAL_CALLBACK void _nya_app_tick_tween(f32 delta_time_s) { nya_system_tween_update(delta_time_s); }

/* entities after the layers, so something a layer spawns is simulated this tick. */
NYA_INTERNAL_CALLBACK void _nya_app_tick_entity(f32 delta_time_s) { nya_system_entity_update(delta_time_s); }

#ifndef NYA_NO_SDL
/* Networking, after everything that changes the world and before the barrier. */
NYA_INTERNAL_CALLBACK void _nya_app_tick_net_server(f32 delta_time_s) { nya_net_server_tick(nya_world()->sim_system.tick, delta_time_s); }
NYA_INTERNAL_CALLBACK void _nya_app_tick_net_client(f32 delta_time_s) { nya_net_client_tick(nya_world()->sim_system.tick, delta_time_s); }
#endif

NYA_INTERNAL_CALLBACK void _nya_app_render_layers(f32 delta_time_s) {
    nya_unused(delta_time_s);

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
}

/* after drawing, which is where layers place the listener. once a frame: it is heard, not simulated. */
NYA_INTERNAL_CALLBACK void _nya_app_render_audio(f32 delta_time_s) { nya_system_audio_update(delta_time_s); }

// NOLINTEND(misc-use-internal-linkage)

/**
 * Registers every engine subsystem in bring-up order, each chained `after` the previous one so
 * nya_system_registry_finalize can only produce this order. Teardown runs in reverse.
 *
 * One order serves every phase, so a subsystem whose per-frame position differs from its bring-up
 * position is registered twice: the lifetime under its own name, the phase work under `<name>_<phase>`.
 * Steam initializes before the renderer but pumps after the gamepad, and tween initializes before the
 * renderer but ticks after the layers, which is two positions each and therefore two entries each.
 * */
void _nya_app_register_subsystems(void) {
    // first up and last down, so every other subsystem's log lines reach the file. its frame work is the
    // midnight roll, which has to happen before anything else writes a line this frame.
    nya_system_register((NYA_SystemEntry){ .name   = "logfile",
                                           .init   = nya_callback(_nya_app_bring_up_logfile),
                                           .deinit = nya_callback(_nya_app_tear_down_logfile),
                                           .frame  = nya_callback(_nya_app_frame_logfile) });

    // straight after the log file, and before anything that can fail: a subsystem that dies during bring-up
    // is exactly the crash a report is worth having for, and the report is written beside that log file.
    nya_system_register((NYA_SystemEntry){ .name   = "crash_reporter",
                                            .after  = "logfile",
                                            .init   = nya_callback(_nya_app_bring_up_crash_reporter),
                                            .deinit = nya_callback(_nya_app_tear_down_crash_reporter) });

    // before settings, which it feeds. settings cannot fail: it owns no memory and loads defaults when nothing
    // was saved. on the way out it writes into the save system's directory.
    nya_system_register((NYA_SystemEntry){ .name = "save", .after = "crash_reporter", .init = nya_callback(_nya_app_bring_up_save), .deinit = nya_callback(_nya_app_tear_down_save) });
    nya_system_register((NYA_SystemEntry){ .name         = "settings",
                                            .after        = "save",
                                            .init         = nya_callback(_nya_app_bring_up_settings),
                                            .deinit       = nya_callback(_nya_app_tear_down_settings) });

#ifdef NYA_PLUGIN_STEAM
    // early, so the overlay can hook the renderer's device when it is created.
    nya_system_register((NYA_SystemEntry){ .name = "steam", .after = "settings", .init = nya_callback(_nya_app_bring_up_steam), .deinit = nya_callback(_nya_app_tear_down_steam) });
#endif

    /*
     * The callback registry is not a system: every entry here is registered *through* it, so it is up
     * before this function is called and torn down after the last system has gone. See
     * nya_app_init_with_options.
     */
    nya_system_register((NYA_SystemEntry){ .name = "job", .after = "settings", .init = nya_callback(_nya_app_bring_up_job), .deinit = nya_callback(_nya_app_tear_down_job) });

    // After the callback registry, whose handles a tween's on_complete resolves through.
    nya_system_register((NYA_SystemEntry){ .name = "tween", .after = "job", .init = nya_callback(_nya_app_bring_up_tween), .deinit = nya_callback(_nya_app_tear_down_tween) });

    /*
     * Optional only in a headless run, where there is no display to make a GPU device for and no window to
     * draw into: that is a correct dedicated server, not a failure, so it is skipped at debug level. The
     * same failure in a windowed run is still loud and still stops bring-up. See NYA_SystemEntry.optional.
     */
    nya_system_register((NYA_SystemEntry){ .name     = "renderer",
                                            .after    = "tween",
                                            .init     = nya_callback(_nya_app_bring_up_renderer),
                                            .deinit   = nya_callback(_nya_app_tear_down_renderer),
                                            .optional = nya_app_get()->options.headless });
    nya_system_register((NYA_SystemEntry){ .name = "events", .after = "renderer", .init = nya_callback(_nya_app_bring_up_events), .deinit = nya_callback(_nya_app_tear_down_events) });
    nya_system_register((NYA_SystemEntry){ .name = "input", .after = "events", .init = nya_callback(_nya_app_bring_up_input), .deinit = nya_callback(_nya_app_tear_down_input) });

    // After events, whose drain loop hands it the SDL events it consumes. Its frame work rolls the held
    // buttons over, so it comes before anything that reads them.
    nya_system_register((NYA_SystemEntry){ .name         = "gamepad",
                                            .after        = "input",
                                            .init         = nya_callback(_nya_app_bring_up_gamepad),
                                            .deinit       = nya_callback(_nya_app_tear_down_gamepad),
                                            .frame        = nya_callback(_nya_app_frame_gamepad) });

#ifdef NYA_PLUGIN_STEAM
    // the pump only, after the gamepad. `steam` itself is far earlier, because it has to be up before the
    // renderer; see the note on this function.
    nya_system_register((NYA_SystemEntry){ .name = "steam_frame", .after = "gamepad", .frame = nya_callback(_nya_app_frame_steam) });
#endif

    nya_system_register((NYA_SystemEntry){ .name = "asset", .after = "gamepad", .init = nya_callback(_nya_app_bring_up_asset), .deinit = nya_callback(_nya_app_tear_down_asset) });

    // after the asset system: a locale file is read and watched through the registry.
    nya_system_register((NYA_SystemEntry){ .name = "i18n", .after = "asset", .init = nya_callback(_nya_app_bring_up_i18n), .deinit = nya_callback(_nya_app_tear_down_i18n) });

    // after the asset system: a config file is read and, under hot reload, watched through the registry.
    nya_system_register((NYA_SystemEntry){ .name = "config", .after = "i18n", .init = nya_callback(_nya_app_bring_up_config), .deinit = nya_callback(_nya_app_tear_down_config) });

    // after the asset system, which creates the mixer these tracks use and destroys it after them.
    nya_system_register((NYA_SystemEntry){ .name = "audio", .after = "config", .init = nya_callback(_nya_app_bring_up_audio), .deinit = nya_callback(_nya_app_tear_down_audio) });

    nya_system_register((NYA_SystemEntry){ .name = "world", .after = "audio", .init = nya_callback(_nya_app_bring_up_world), .deinit = nya_callback(_nya_app_tear_down_world) });

    // last up, first down. the world is entities, physics and the simulation barrier as one lifetime.
    nya_system_register((NYA_SystemEntry){ .name = "window", .after = "world", .init = nya_callback(_nya_app_bring_up_window), .deinit = nya_callback(_nya_app_tear_down_window) });

    /*
     * Teardown only, deliberately. Loading is nya_plugin_load_all and the *game* calls it, because a
     * plugin's `on_load` runs in the game's world and bringing plugins up here would run it before the
     * game has one. Registered all the same, so whatever the game loaded goes down in registry order
     * with everything else rather than leaking a VM per plugin at exit.
     */
    nya_system_register((NYA_SystemEntry){ .name = "plugins", .after = "window", .deinit = nya_callback(nya_plugin_unload_all) });

    /*
     * ── the frame, in order ──────────────────────────────────────────────────────────────────────────
     *
     * Nothing here owns a lifetime; they are the work that used to be a hardcoded call list in
     * _nya_app_update and _nya_app_render. Chained one after the next because the order between them is
     * the whole point, and registered last so a game or a plugin can name any of them in its own `after`.
     */
    nya_system_register((NYA_SystemEntry){ .name = "entity_transforms", .after = "window", .tick = nya_callback(_nya_app_tick_entity_transforms) });
    nya_system_register((NYA_SystemEntry){ .name = "physics2d", .after = "entity_transforms", .tick = nya_callback(_nya_app_tick_physics2d) });
    nya_system_register((NYA_SystemEntry){ .name = "physics3d", .after = "physics2d", .tick = nya_callback(_nya_app_tick_physics3d) });

    // the layer stack, updated and drawn. Everything a game registers of its own belongs either side of
    // this, which is why it is a system rather than a call the loop makes around them.
    nya_system_register((NYA_SystemEntry){ .name   = "layers",
                                           .after  = "physics3d",
                                           .tick   = nya_callback(_nya_app_tick_layers),
                                           .render = nya_callback(_nya_app_render_layers) });

    nya_system_register((NYA_SystemEntry){ .name = "tween_tick", .after = "layers", .tick = nya_callback(_nya_app_tick_tween) });
    nya_system_register((NYA_SystemEntry){ .name = "entity", .after = "tween_tick", .tick = nya_callback(_nya_app_tick_entity) });

#ifndef NYA_NO_SDL
    nya_system_register((NYA_SystemEntry){ .name = "net_server", .after = "entity", .tick = nya_callback(_nya_app_tick_net_server) });
    nya_system_register((NYA_SystemEntry){ .name = "net_client", .after = "net_server", .tick = nya_callback(_nya_app_tick_net_client) });
#endif

    // after "layers" in the render phase, which is what `after = entity` buys it: the only other render
    // system is the layer stack, and it sits well before this.
    nya_system_register((NYA_SystemEntry){ .name = "audio_render", .after = "entity", .render = nya_callback(_nya_app_render_audio) });

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

    /*
     * Before the registrations rather than as one of them: a system entry holds callback handles, so
     * every nya_system_register below resolves through this registry, and a registry that was itself a
     * system would have to be up before it could be registered. It is torn down in nya_app_deinit,
     * after the last system's `deinit` has run, so nothing resolves a handle through a freed arena.
     */
    nya_system_callback_init();

    _nya_app_register_subsystems();

    // Brought up in order and unwound in reverse of however far it got, so a failure leaves nothing half
    // built. An optional subsystem that this run has no use for is skipped rather than fatal; see
    // NYA_SystemEntry.optional.
    NYA_Error result = nya_system_registry_run_init();

    if (!result.ok) {
        nya_system_callback_deinit();

        nya_arena_destroy(app->frame_allocator);
        nya_arena_destroy(app->live_resize_allocator);
        app->initialized = false;

        SDL_Quit();
        nya_signals_deinit();

        return result;
    }

    // after the renderer and windows, since the watcher draws. not fatal: it only costs frames during a resize
    // drag.
    if (!SDL_AddEventWatch(_nya_app_live_resize_event_watch, nullptr)) {
        nya_log_warn("SDL_AddEventWatch() failed, the window will not redraw while being resized: %s", SDL_GetError());
    }

    nya_log_info("Subsystems initialized after %.1f ms.", nya_time_ns_to_ms(nya_app_uptime_ns()));
    return NYA_OK;
}

void nya_app_events_pump(void) {
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

u64 nya_app_uptime_ns(void) {
    NYA_App* app = nya_app_get();
    return _nya_app_now_ns() - app->frame_stats.started_ns;
}

void nya_app_time_source_set(NYA_AppTimeSource source) {
    NYA_App* app = nya_app_get();

    u64 was = _nya_app_now_ns();
    u64 now = source.now_ns != nullptr ? source.now_ns() : nya_clock_get_monotonic_ns();

    app->time_source = source;

    /*
     * A clock swap is a discontinuity rather than a frame. The next frame's elapsed time is the new
     * clock's reading minus a timestamp the outgoing clock wrote, which is an enormous delta in one
     * direction and an unsigned wrap in the other: a simulated clock that ran ahead of the wall clock
     * and is then handed back leaves the loop believing it is centuries behind. Rebased, the first
     * frame after a swap measures nothing and owes nothing, which is what actually happened.
     */
    app->frame_stats.prev_frame_time_ns  = now;
    app->frame_stats.frame_start_time_ns = now;
    app->frame_stats.time_behind_ns      = 0;

    // uptime is measured against started_ns, so the origin moves with the clock and the program's age
    // neither jumps nor goes backwards across a swap.
    app->frame_stats.started_ns = now - (was - app->frame_stats.started_ns);
}

NYA_AppTimeSource nya_app_time_source(void) {
    return nya_app_get()->time_source;
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

    // after every `deinit`, each of which was reached by resolving a handle through this registry.
    nya_system_callback_deinit();

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

            // the log roll, the gamepad's frame edge and Steam's pump, in that order. See
            // _nya_app_register_subsystems.
            nya_system_registry_run(NYA_SYSTEM_PHASE_FRAME, (f32)nya_time_ns_to_s(app->frame_stats.elapsed_ns));
        }

        nya_app_events_pump();

        _nya_app_update();
        _nya_app_render();

        {
            app->frame_stats.frame_end_time_ns  = _nya_app_now_ns();
            app->frame_stats.prev_frame_time_ns = app->frame_stats.frame_start_time_ns;

            // guarded like the nested step's: the first frame after the clock was rebased measures no
            // elapsed time at all, and a rate over no time is a division by zero rather than a number.
            if (app->frame_stats.elapsed_ns > 0) app->frame_stats.fps = 1.0F / (f32)nya_time_ns_to_s(app->frame_stats.elapsed_ns);

            // observers read the frame's records, which are then dropped, before the frame allocator resets.
            nya_system_sim_end_frame();

            // the frame's per system times become "the last frame" here, since no single phase run is
            // the frame boundary. A no-op unless something asked for the numbers.
            nya_system_accounting_frame_end();

            nya_arena_free_all(app->frame_allocator);

            nya_trace_frame_end();

            nya_event_dispatch((NYA_Event){
                .type = NYA_EVENT_FRAME_ENDED,
            });

            // a second copy of the watchdog, which also runs every tick, so it is not one patch away.
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

        // a simulated clock never earns a sleep: its frames take no time at all, so every one of them
        // would sleep the whole frame budget and the session would run at the frame rate limit.
        if (app->time_source.never_sleep) floor_ns = 0;

        if (floor_ns > 0 && app->frame_stats.work_ns < floor_ns) {
            app->frame_stats.sleep_ns = floor_ns - app->frame_stats.work_ns;
            SDL_DelayNS(app->frame_stats.sleep_ns);
        }
    }
}

u64 _nya_app_now_ns(void) {
    NYA_App* app = &_NYA_APP_INSTANCE;

    // read straight off the instance rather than through nya_app_get: this runs before the app is
    // initialized, from nya_app_uptime_ns in the startup log line.
    if (app->time_source.now_ns != nullptr) return app->time_source.now_ns();

    return nya_clock_get_monotonic_ns();
}

void _nya_app_advance_frame_clock(void) {
    NYA_App* app = nya_app_get();

    app->frame_stats.uptime_ns            = nya_app_uptime_ns();
    app->frame_stats.uptime_s             = (f32)nya_time_ns_to_s(app->frame_stats.uptime_ns);
    app->frame_stats.frame_start_time_ns  = _nya_app_now_ns();
    app->frame_stats.elapsed_ns           = app->frame_stats.frame_start_time_ns - app->frame_stats.prev_frame_time_ns;
    app->frame_stats.time_behind_ns      += (s64)app->frame_stats.elapsed_ns;

    // unpaid debt is dropped; carrying it turns one slow frame into permanent catch-up.
    u32 catch_up_ticks = app->time_source.catch_up_ticks_max > 0 ? app->time_source.catch_up_ticks_max : _NYA_APP_MAX_CATCH_UP_TICKS;

    s64 max_debt_ns = (s64)app->options.time_step_ns * catch_up_ticks;
    if (app->frame_stats.time_behind_ns > max_debt_ns) app->frame_stats.time_behind_ns = max_debt_ns;
}

void _nya_app_update(void) {
    NYA_App* app = nya_app_get();

    // here rather than in the main loop, so a window dragged by its edge keeps sweeping while its ticks watch.
    nya_integrity_sweep(app->frame_stats.frame_start_time_ns);

    while (app->frame_stats.time_behind_ns >= (s64)app->options.time_step_ns) {
        nya_perf_time_this_scope("frame_updating");
        nya_event_dispatch((NYA_Event){
            .type = NYA_EVENT_UPDATING_STARTED,
        });

        // set once per tick before anything reads it, so every observer of NYA_EVENT_UPDATING_STARTED sees this
        // tick's value.
        app->frame_stats.delta_time_s = (f32)nya_time_ns_to_s(app->options.time_step_ns);

        nya_integrity_watchdog(app->frame_stats.frame_start_time_ns);

        /*
         * Every registered tick, engine and game alike, in the one order the registry holds: transform
         * capture, both solvers, the layer stack, tweens, entities, networking, and whatever a game or a
         * plugin ordered between them. See _nya_app_register_subsystems.
         */
        nya_system_registry_run(NYA_SYSTEM_PHASE_TICK, app->frame_stats.delta_time_s);

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

    // the layer stack, then the audio listener it placed. Wall clock, not the fixed tick: what is drawn
    // and what is heard both advance in real time.
    nya_system_registry_run(NYA_SYSTEM_PHASE_RENDER, (f32)nya_time_ns_to_s(nya_app_get()->frame_stats.elapsed_ns));

    nya_event_dispatch((NYA_Event){
        .type = NYA_EVENT_RENDERING_ENDED,
    });
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
    app->frame_stats.frame_end_time_ns  = _nya_app_now_ns();
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
