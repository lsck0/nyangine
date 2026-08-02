#include "SDL3/SDL_init.h"
#include "SDL3/SDL_timer.h"

#include "nyangine/nyangine.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API DECLARATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

NYA_INTERNAL NYA_App _NYA_APP_INSTANCE;

NYA_INTERNAL void _nya_app_handle_shutdown_signal(NYA_Signal signal);

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
        .frame_stats.started_ns         = nya_clock_get_timestamp_ns(),
        .frame_stats.prev_frame_time_ns = nya_clock_get_timestamp_ns(),
        .frame_stats.min_frame_time_ns  = 1'000'000'000 / (u64)options.frame_rate_limit,
    };

    nya_info("Nyangine initialized. Initializing subsystems...");

    // Brought up in dependency order, and unwound in the reverse of however far we got. The
    // alternative, returning early and leaving half a world standing, would leave the caller with
    // nothing safe to do: nya_app_deinit would tear down systems that were never built.
    NYA_Error result     = NYA_OK;
    u32       brought_up = 0;

#define _NYA_APP_BRING_UP(expression)                                                                                                                \
    result = (expression);                                                                                                                           \
    if (result.kind != NYA_ERROR_NONE) goto unwind;                                                                                                  \
    brought_up++;

    _NYA_APP_BRING_UP(nya_system_job_init());
    nya_system_callback_init();
    brought_up++;
    _NYA_APP_BRING_UP(nya_system_renderer_init());
    nya_system_window_init();
    brought_up++;
    _NYA_APP_BRING_UP(nya_system_events_init());
    nya_system_input_init();
    brought_up++;
    nya_system_asset_init();
    brought_up++;
    nya_system_entity_init();
    brought_up++;
    nya_system_sim_init();
    brought_up++;

#undef _NYA_APP_BRING_UP

    nya_info("Subsystems initialized successfully.");
    return NYA_OK;

unwind:
    nya_warn("Subsystem initialization failed after %u of 9 systems; unwinding. %s", brought_up, (NYA_ConstCString)result.message);

    // Reverse order, skipping everything that never came up.
    if (brought_up > 8) nya_system_sim_deinit();
    if (brought_up > 7) nya_system_entity_deinit();
    if (brought_up > 6) nya_system_asset_deinit();
    if (brought_up > 5) nya_system_input_deinit();
    if (brought_up > 4) nya_system_events_deinit();
    if (brought_up > 3) nya_system_window_deinit();
    if (brought_up > 2) nya_system_renderer_deinit();
    if (brought_up > 1) nya_system_callback_deinit();
    if (brought_up > 0) nya_system_job_deinit();

    nya_arena_destroy(app->frame_allocator);
    app->initialized = false;

    SDL_Quit();
    nya_signals_deinit();

    return result;
}

u64 nya_app_uptime_ns(void) {
    NYA_App* app = nya_app_get();
    return nya_clock_get_timestamp_ns() - app->frame_stats.started_ns;
}

f64 nya_app_uptime_s(void) {
    return (f64)nya_app_uptime_ns() / 1'000'000'000.0;
}

void nya_app_deinit(void) {
    NYA_App* app = nya_app_get();

    nya_info("Deinitializing subsystems...");

    nya_system_sim_deinit();
    nya_system_entity_deinit();
    nya_system_asset_deinit();
    nya_system_input_deinit();
    nya_system_events_deinit();
    nya_system_window_deinit();
    nya_system_renderer_deinit();
    nya_system_job_deinit();
    nya_system_callback_deinit();

    nya_info("Subsystems deinitialized successfully.");

    nya_arena_destroy(app->frame_allocator);

    SDL_Quit();

    nya_signals_deinit();

    app->initialized = false;

    nya_info("Nyangine deinitialized. Goodbye.");
}

void nya_app_run(void) {
    NYA_App* app = nya_app_get();

    while (!app->should_quit) {
        nya_perf_time_this_scope("frame");

        // start of frame tasks
        {
            nya_event_dispatch((NYA_Event){
                .type = NYA_EVENT_FRAME_STARTED,
            });

            app->frame_stats.uptime_ns            = nya_app_uptime_ns();
            app->frame_stats.frame_start_time_ns  = nya_clock_get_timestamp_ns();
            app->frame_stats.elapsed_ns           = app->frame_stats.frame_start_time_ns - app->frame_stats.prev_frame_time_ns;
            app->frame_stats.time_behind_ns      += (s64)app->frame_stats.elapsed_ns;
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

        // updating
        {
            while (app->frame_stats.time_behind_ns >= (s64)app->options.time_step_ns) {
                nya_perf_time_this_scope("frame_updating");
                nya_event_dispatch((NYA_Event){
                    .type = NYA_EVENT_UPDATING_STARTED,
                });

                for (u32 slot = 0; slot < NYA_WINDOW_MAX; slot++) {
                    NYA_Window* window = nya_window_at_slot(slot);
                    if (window == nullptr) continue;

                    nya_array_foreach (window->layer_stack, layer) {
                        NYA_LayerOnUpdateFn on_update_fn = nya_callback_get(layer->on_update);
                        if (layer->enabled && on_update_fn != nullptr) { /**/
                            app->frame_stats.delta_time_s = (f32)nya_time_ns_to_s(app->options.time_step_ns);
                            on_update_fn(window, app->frame_stats.delta_time_s);
                        }
                    }
                }

                // The barrier. Every update for this tick has run, so nothing is mid iteration and
                // the queued mutations can be applied without changing what anyone is walking.
                nya_system_sim_apply_commands();
                app->sim_system.tick++;

                app->frame_stats.time_behind_ns -= (s64)app->options.time_step_ns;
                nya_event_dispatch((NYA_Event){
                    .type = NYA_EVENT_UPDATING_ENDED,
                });
            }
        }

        // rendering
        {
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
                    if (layer->enabled && on_render_fn != nullptr) on_render_fn(window);
                }

                nya_render_end(window);
            }

            nya_event_dispatch((NYA_Event){
                .type = NYA_EVENT_RENDERING_ENDED,
            });
        }

        // end of frame tasks
        {
            app->frame_stats.frame_end_time_ns  = nya_clock_get_timestamp_ns();
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

        // framerate limiting
        if (!app->options.vsync_enabled && app->options.frame_rate_limit > 0) {
            if (app->frame_stats.elapsed_ns < app->frame_stats.min_frame_time_ns) { /**/
                SDL_DelayNS(app->frame_stats.min_frame_time_ns - app->frame_stats.elapsed_ns);
            }
        }
    }
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
