#include "SDL3/SDL_gpu.h"

#include "nyangine/nyangine.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API DECLARATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

NYA_INTERNAL NYA_Window*   _nya_window_from_sdl(SDL_Window* sdl_window);
NYA_INTERNAL NYA_Window*   _nya_window_require(NYA_WindowHandle window, NYA_ConstCString caller);
NYA_INTERNAL SDL_DisplayID _nya_window_display(NYA_WindowHandle window);
NYA_INTERNAL NYA_Rect      _nya_rect_from_sdl(SDL_Rect rect);

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PUBLIC API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/*
 * ─────────────────────────────────────────────────────────
 * SYSTEM FUNCTIONS
 * ─────────────────────────────────────────────────────────
 */

void nya_system_window_init(void) {
    NYA_App* app = nya_app_get();

    NYA_Arena* allocator = nya_arena_create(.name = "window_system_allocator");

    // Wayland clients cannot position or size themselves; the compositor decides and the client is
    // told. Asked once here rather than at every call site, since the driver cannot change while the
    // process is running.
    NYA_ConstCString driver          = SDL_GetCurrentVideoDriver();
    b8               client_geometry = driver == nullptr || !nya_string_equals((NYA_CString)driver, "wayland");

    app->window_system = (NYA_WindowSystem){
        .allocator                  = allocator,
        .windows                    = nya_arena_alloc(allocator, NYA_WINDOW_MAX * sizeof(NYA_Window)),
        .occupied                   = nya_arena_alloc(allocator, NYA_WINDOW_MAX * sizeof(b8)),
        .generations                = nya_arena_alloc(allocator, NYA_WINDOW_MAX * sizeof(u32)),
        .client_controlled_geometry = client_geometry,
    };

    nya_memset(app->window_system.windows, 0, NYA_WINDOW_MAX * sizeof(NYA_Window));
    nya_memset(app->window_system.occupied, 0, NYA_WINDOW_MAX * sizeof(b8));

    // Generations start at 1 so that a zeroed NYA_WindowHandle, which is what an uninitialized
    // struct field holds, never resolves to slot 0.
    for (u32 i = 0; i < NYA_WINDOW_MAX; i++) app->window_system.generations[i] = 1;

    nya_info("Window system initialized (video driver: %s, client geometry: %s).", driver ? driver : "none", client_geometry ? "yes" : "no");
}

void nya_system_window_deinit(void) {
    NYA_App* app = nya_app_get();

    SDL_WaitForGPUIdle(app->render_system.gpu_device);

    for (u32 i = 0; i < NYA_WINDOW_MAX; i++) {
        if (!app->window_system.occupied[i]) continue;
        nya_window_destroy(app->window_system.windows[i].handle);
    }

    nya_arena_destroy(app->window_system.allocator);

    nya_info("Window system deinitialized.");
}

void nya_system_window_handle_event(NYA_Event* event) {
    nya_assert(event != nullptr);

    NYA_App* app = nya_app_get();

    if (event->type == NYA_EVENT_QUIT) app->should_quit = true;

    if (event->type == NYA_EVENT_WINDOW_CLOSE_REQUESTED) {
        nya_window_destroy(event->as_window_event.window);
        if (app->window_system.count == 0) app->should_quit = true;
    }

    // The authoritative size. Everything else is a request that may have been ignored.
    if (event->type == NYA_EVENT_WINDOW_RESIZED) {
        NYA_Window* window = nya_window_get(event->as_window_resized_event.window);
        if (window == nullptr) return;

        window->width  = event->as_window_resized_event.width;
        window->height = event->as_window_resized_event.height;
    }
}

NYA_Window* nya_window_at_slot(u32 index) {
    NYA_App* app = nya_app_get();

    if (index >= NYA_WINDOW_MAX) return nullptr;
    if (!app->window_system.occupied[index]) return nullptr;

    return &app->window_system.windows[index];
}

u32 nya_window_count(void) {
    NYA_App* app = nya_app_get();
    return app->window_system.count;
}

/*
 * ─────────────────────────────────────────────────────────
 * LIFETIME
 * ─────────────────────────────────────────────────────────
 */

NYA_WindowHandle nya_window_create(NYA_ConstCString title, u32 requested_width, u32 requested_height, NYA_WindowFlags flags) {
    nya_assert(title != nullptr);
    nya_assert(requested_width > 0, "Requested width must be greater than 0.");
    nya_assert(requested_height > 0, "Requested height must be greater than 0.");

    NYA_App* app = nya_app_get();

    u32 slot = NYA_WINDOW_MAX;
    for (u32 i = 0; i < NYA_WINDOW_MAX; i++) {
        if (app->window_system.occupied[i]) continue;
        slot = i;
        break;
    }

    if (slot == NYA_WINDOW_MAX) {
        nya_warn("Cannot create window '%s': all %d window slots are in use.", title, NYA_WINDOW_MAX);
        return NYA_WINDOW_HANDLE_NONE;
    }

    SDL_Window* sdl_window = SDL_CreateWindow(title, (s32)requested_width, (s32)requested_height, flags);
    if (sdl_window == nullptr) {
        nya_warn("SDL_CreateWindow() failed for '%s': %s", title, SDL_GetError());
        return NYA_WINDOW_HANDLE_NONE;
    }

    NYA_WindowHandle handle = { .index = slot, .generation = app->window_system.generations[slot] };

    // Written into the slot before the renderer is told about it, so the renderer is handed the
    // address the window will keep rather than a stack copy that is about to be moved.
    NYA_Window* window = &app->window_system.windows[slot];
    *window            = (NYA_Window){
        .handle      = handle,
        .title       = title,
        .sdl_window  = sdl_window,
        .layer_stack = nya_array_create(app->window_system.allocator, NYA_Layer),
    };

    app->window_system.occupied[slot] = true;
    app->window_system.count++;

    // Seeded from what the platform actually produced, not from what was asked for. Under Wayland
    // those already differ by the time this runs.
    s32 actual_width = 0, actual_height = 0;
    SDL_GetWindowSize(sdl_window, &actual_width, &actual_height);
    window->width  = (u32)actual_width;
    window->height = (u32)actual_height;

    nya_system_renderer_for_window_init(window);

    nya_info("Created window '%s' (slot %u, generation %u, %dx%d).", title, slot, handle.generation, actual_width, actual_height);

    return handle;
}

void nya_window_destroy(NYA_WindowHandle window) {
    NYA_App* app = nya_app_get();

    NYA_Window* target = nya_window_get(window);
    if (target == nullptr) return;

    nya_array_foreach_reverse (target->layer_stack, layer) {
        NYA_LayerOnDestroyFn on_destroy_fn = nya_callback_get(layer->on_destroy);
        if (on_destroy_fn != nullptr) on_destroy_fn(target);
    }
    nya_array_destroy(target->layer_stack);

    nya_system_renderer_for_window_deinit(target);
    SDL_DestroyWindow(target->sdl_window);

    nya_info("Destroyed window '%s' (slot %u).", target->title, window.index);

    // Bumping the generation is what makes every outstanding handle to this window stop resolving.
    app->window_system.generations[window.index]++;
    app->window_system.occupied[window.index] = false;
    app->window_system.count--;

    *target = (NYA_Window){ 0 };
}

NYA_Window* nya_window_get(NYA_WindowHandle window) {
    NYA_App* app = nya_app_get();

    if (window.index >= NYA_WINDOW_MAX) return nullptr;
    if (!app->window_system.occupied[window.index]) return nullptr;
    if (app->window_system.generations[window.index] != window.generation) return nullptr;

    return &app->window_system.windows[window.index];
}

b8 nya_window_is_valid(NYA_WindowHandle window) {
    return nya_window_get(window) != nullptr;
}

/*
 * ─────────────────────────────────────────────────────────
 * GEOMETRY, READ
 * ─────────────────────────────────────────────────────────
 */

void nya_window_size(NYA_WindowHandle window, OUT u32* width, OUT u32* height) {
    NYA_Window* target = _nya_window_require(window, "nya_window_size");
    if (target == nullptr) return;

    s32 w = 0, h = 0;
    SDL_GetWindowSize(target->sdl_window, &w, &h);

    if (width) *width = (u32)w;
    if (height) *height = (u32)h;
}

void nya_window_size_in_pixels(NYA_WindowHandle window, OUT u32* width, OUT u32* height) {
    NYA_Window* target = _nya_window_require(window, "nya_window_size_in_pixels");
    if (target == nullptr) return;

    s32 w = 0, h = 0;
    SDL_GetWindowSizeInPixels(target->sdl_window, &w, &h);

    if (width) *width = (u32)w;
    if (height) *height = (u32)h;
}

void nya_window_position(NYA_WindowHandle window, OUT s32* x, OUT s32* y) {
    NYA_Window* target = _nya_window_require(window, "nya_window_position");
    if (target == nullptr) return;

    SDL_GetWindowPosition(target->sdl_window, x, y);
}

f32 nya_window_display_scale(NYA_WindowHandle window) {
    NYA_Window* target = _nya_window_require(window, "nya_window_display_scale");
    return target ? SDL_GetWindowDisplayScale(target->sdl_window) : 1.0F;
}

f32 nya_window_pixel_density(NYA_WindowHandle window) {
    NYA_Window* target = _nya_window_require(window, "nya_window_pixel_density");
    return target ? SDL_GetWindowPixelDensity(target->sdl_window) : 1.0F;
}

NYA_Rect nya_window_safe_area(NYA_WindowHandle window) {
    NYA_Window* target = _nya_window_require(window, "nya_window_safe_area");
    if (target == nullptr) return (NYA_Rect){ 0 };

    SDL_Rect rect = { 0 };
    SDL_GetWindowSafeArea(target->sdl_window, &rect);

    return _nya_rect_from_sdl(rect);
}

NYA_WindowFlags nya_window_flags(NYA_WindowHandle window) {
    NYA_Window* target = _nya_window_require(window, "nya_window_flags");
    return target ? (NYA_WindowFlags)SDL_GetWindowFlags(target->sdl_window) : NYA_WINDOW_NONE;
}

NYA_ConstCString nya_window_title(NYA_WindowHandle window) {
    NYA_Window* target = _nya_window_require(window, "nya_window_title");
    return target ? SDL_GetWindowTitle(target->sdl_window) : "";
}

f32 nya_window_opacity(NYA_WindowHandle window) {
    NYA_Window* target = _nya_window_require(window, "nya_window_opacity");
    return target ? SDL_GetWindowOpacity(target->sdl_window) : 1.0F;
}

b8 nya_window_has_focus(NYA_WindowHandle window) {
    return (nya_window_flags(window) & SDL_WINDOW_INPUT_FOCUS) != 0;
}

b8 nya_window_is_fullscreen(NYA_WindowHandle window) {
    return (nya_window_flags(window) & SDL_WINDOW_FULLSCREEN) != 0;
}

b8 nya_window_is_maximized(NYA_WindowHandle window) {
    return (nya_window_flags(window) & SDL_WINDOW_MAXIMIZED) != 0;
}

b8 nya_window_is_minimized(NYA_WindowHandle window) {
    return (nya_window_flags(window) & SDL_WINDOW_MINIMIZED) != 0;
}

b8 nya_window_is_visible(NYA_WindowHandle window) {
    return (nya_window_flags(window) & SDL_WINDOW_HIDDEN) == 0;
}

b8 nya_window_is_occluded(NYA_WindowHandle window) {
    return (nya_window_flags(window) & SDL_WINDOW_OCCLUDED) != 0;
}

b8 nya_window_is_resizable(NYA_WindowHandle window) {
    return (nya_window_flags(window) & SDL_WINDOW_RESIZABLE) != 0;
}

b8 nya_window_is_borderless(NYA_WindowHandle window) {
    return (nya_window_flags(window) & SDL_WINDOW_BORDERLESS) != 0;
}

b8 nya_window_is_always_on_top(NYA_WindowHandle window) {
    return (nya_window_flags(window) & SDL_WINDOW_ALWAYS_ON_TOP) != 0;
}

/*
 * ─────────────────────────────────────────────────────────
 * GEOMETRY, REQUEST
 * ─────────────────────────────────────────────────────────
 */

b8 nya_window_geometry_is_client_controlled(void) {
    NYA_App* app = nya_app_get();
    return app->window_system.client_controlled_geometry;
}

/*
 * Each of these reports whether the platform entertains client geometry at all. The call is still
 * made when it does not, because SDL is free to start honouring it and because a no-op is harmless;
 * what would not be harmless is the caller believing the window is now the size it asked for.
 */

b8 nya_window_request_size(NYA_WindowHandle window, u32 width, u32 height) {
    NYA_Window* target = _nya_window_require(window, "nya_window_request_size");
    if (target == nullptr) return false;

    SDL_SetWindowSize(target->sdl_window, (s32)width, (s32)height);
    return nya_window_geometry_is_client_controlled();
}

b8 nya_window_request_position(NYA_WindowHandle window, s32 x, s32 y) {
    NYA_Window* target = _nya_window_require(window, "nya_window_request_position");
    if (target == nullptr) return false;

    SDL_SetWindowPosition(target->sdl_window, x, y);
    return nya_window_geometry_is_client_controlled();
}

b8 nya_window_request_minimum_size(NYA_WindowHandle window, u32 min_width, u32 min_height) {
    NYA_Window* target = _nya_window_require(window, "nya_window_request_minimum_size");
    if (target == nullptr) return false;

    SDL_SetWindowMinimumSize(target->sdl_window, (s32)min_width, (s32)min_height);
    return nya_window_geometry_is_client_controlled();
}

b8 nya_window_request_maximum_size(NYA_WindowHandle window, u32 max_width, u32 max_height) {
    NYA_Window* target = _nya_window_require(window, "nya_window_request_maximum_size");
    if (target == nullptr) return false;

    SDL_SetWindowMaximumSize(target->sdl_window, (s32)max_width, (s32)max_height);
    return nya_window_geometry_is_client_controlled();
}

b8 nya_window_request_aspect_ratio(NYA_WindowHandle window, f32 min_aspect, f32 max_aspect) {
    NYA_Window* target = _nya_window_require(window, "nya_window_request_aspect_ratio");
    if (target == nullptr) return false;

    SDL_SetWindowAspectRatio(target->sdl_window, min_aspect, max_aspect);
    return nya_window_geometry_is_client_controlled();
}

/*
 * ─────────────────────────────────────────────────────────
 * STATE
 * ─────────────────────────────────────────────────────────
 */

void nya_window_show(NYA_WindowHandle window) {
    NYA_Window* target = _nya_window_require(window, "nya_window_show");
    if (target) SDL_ShowWindow(target->sdl_window);
}

void nya_window_hide(NYA_WindowHandle window) {
    NYA_Window* target = _nya_window_require(window, "nya_window_hide");
    if (target) SDL_HideWindow(target->sdl_window);
}

void nya_window_raise(NYA_WindowHandle window) {
    NYA_Window* target = _nya_window_require(window, "nya_window_raise");
    if (target) SDL_RaiseWindow(target->sdl_window);
}

void nya_window_maximize(NYA_WindowHandle window) {
    NYA_Window* target = _nya_window_require(window, "nya_window_maximize");
    if (target) SDL_MaximizeWindow(target->sdl_window);
}

void nya_window_minimize(NYA_WindowHandle window) {
    NYA_Window* target = _nya_window_require(window, "nya_window_minimize");
    if (target) SDL_MinimizeWindow(target->sdl_window);
}

void nya_window_restore(NYA_WindowHandle window) {
    NYA_Window* target = _nya_window_require(window, "nya_window_restore");
    if (target) SDL_RestoreWindow(target->sdl_window);
}

void nya_window_set_title(NYA_WindowHandle window, NYA_ConstCString title) {
    nya_assert(title != nullptr);

    NYA_Window* target = _nya_window_require(window, "nya_window_set_title");
    if (target == nullptr) return;

    SDL_SetWindowTitle(target->sdl_window, title);
    target->title = title;
}

void nya_window_set_fullscreen(NYA_WindowHandle window, b8 fullscreen) {
    NYA_Window* target = _nya_window_require(window, "nya_window_set_fullscreen");
    if (target) SDL_SetWindowFullscreen(target->sdl_window, fullscreen);
}

void nya_window_set_borderless(NYA_WindowHandle window, b8 borderless) {
    NYA_Window* target = _nya_window_require(window, "nya_window_set_borderless");
    if (target) SDL_SetWindowBordered(target->sdl_window, !borderless);
}

void nya_window_set_resizable(NYA_WindowHandle window, b8 resizable) {
    NYA_Window* target = _nya_window_require(window, "nya_window_set_resizable");
    if (target) SDL_SetWindowResizable(target->sdl_window, resizable);
}

void nya_window_set_always_on_top(NYA_WindowHandle window, b8 always_on_top) {
    NYA_Window* target = _nya_window_require(window, "nya_window_set_always_on_top");
    if (target) SDL_SetWindowAlwaysOnTop(target->sdl_window, always_on_top);
}

void nya_window_set_opacity(NYA_WindowHandle window, f32 opacity) {
    NYA_Window* target = _nya_window_require(window, "nya_window_set_opacity");
    if (target) SDL_SetWindowOpacity(target->sdl_window, nya_clamp(opacity, 0.0F, 1.0F));
}

void nya_window_set_focusable(NYA_WindowHandle window, b8 focusable) {
    NYA_Window* target = _nya_window_require(window, "nya_window_set_focusable");
    if (target) SDL_SetWindowFocusable(target->sdl_window, focusable);
}

void nya_window_set_mouse_grabbed(NYA_WindowHandle window, b8 grabbed) {
    NYA_Window* target = _nya_window_require(window, "nya_window_set_mouse_grabbed");
    if (target) SDL_SetWindowMouseGrab(target->sdl_window, grabbed);
}

b8 nya_window_is_mouse_grabbed(NYA_WindowHandle window) {
    NYA_Window* target = _nya_window_require(window, "nya_window_is_mouse_grabbed");
    return target ? SDL_GetWindowMouseGrab(target->sdl_window) : false;
}

void nya_window_set_relative_mouse(NYA_WindowHandle window, b8 relative) {
    NYA_Window* target = _nya_window_require(window, "nya_window_set_relative_mouse");
    if (target) SDL_SetWindowRelativeMouseMode(target->sdl_window, relative);
}

b8 nya_window_is_relative_mouse(NYA_WindowHandle window) {
    NYA_Window* target = _nya_window_require(window, "nya_window_is_relative_mouse");
    return target ? SDL_GetWindowRelativeMouseMode(target->sdl_window) : false;
}

void nya_window_flash(NYA_WindowHandle window, NYA_FlashOperation operation) {
    NYA_Window* target = _nya_window_require(window, "nya_window_flash");
    if (target) SDL_FlashWindow(target->sdl_window, (SDL_FlashOperation)operation);
}

void nya_window_sync(NYA_WindowHandle window) {
    NYA_Window* target = _nya_window_require(window, "nya_window_sync");
    if (target) SDL_SyncWindow(target->sdl_window);
}

/*
 * ─────────────────────────────────────────────────────────
 * DISPLAY
 * ─────────────────────────────────────────────────────────
 */

NYA_ConstCString nya_window_display_name(NYA_WindowHandle window) {
    SDL_DisplayID display = _nya_window_display(window);
    if (display == 0) return "";

    NYA_ConstCString name = SDL_GetDisplayName(display);
    return name ? name : "";
}

NYA_Rect nya_window_display_bounds(NYA_WindowHandle window) {
    SDL_DisplayID display = _nya_window_display(window);
    if (display == 0) return (NYA_Rect){ 0 };

    SDL_Rect rect = { 0 };
    SDL_GetDisplayBounds(display, &rect);

    return _nya_rect_from_sdl(rect);
}

NYA_Rect nya_window_display_usable_bounds(NYA_WindowHandle window) {
    SDL_DisplayID display = _nya_window_display(window);
    if (display == 0) return (NYA_Rect){ 0 };

    SDL_Rect rect = { 0 };
    SDL_GetDisplayUsableBounds(display, &rect);

    return _nya_rect_from_sdl(rect);
}

NYA_DisplayMode nya_window_display_mode(NYA_WindowHandle window) {
    SDL_DisplayID display = _nya_window_display(window);
    if (display == 0) return (NYA_DisplayMode){ 0 };

    const SDL_DisplayMode* mode = SDL_GetCurrentDisplayMode(display);
    if (mode == nullptr) return (NYA_DisplayMode){ 0 };

    return (NYA_DisplayMode){
        .width         = mode->w,
        .height        = mode->h,
        .refresh_rate  = mode->refresh_rate,
        .pixel_density = mode->pixel_density,
    };
}

NYA_ConstCString nya_video_driver(void) {
    NYA_ConstCString driver = SDL_GetCurrentVideoDriver();
    return driver ? driver : "none";
}

/*
 * ─────────────────────────────────────────────────────────
 * LAYER FUNCTIONS
 * ─────────────────────────────────────────────────────────
 */

NYA_Layer* nya_layer_get(NYA_WindowHandle window, void* layer_id) {
    nya_assert(layer_id != nullptr);

    NYA_Window* target = _nya_window_require(window, "nya_layer_get");
    if (target == nullptr) return nullptr;

    nya_array_foreach (target->layer_stack, layer) {
        if (layer->id == layer_id) return layer;
    }

    return nullptr;
}

void nya_layer_enable(NYA_WindowHandle window, void* layer_id) {
    NYA_Layer* layer = nya_layer_get(window, layer_id);
    if (layer) layer->enabled = true;
}

void nya_layer_disable(NYA_WindowHandle window, void* layer_id) {
    NYA_Layer* layer = nya_layer_get(window, layer_id);
    if (layer) layer->enabled = false;
}

void nya_layer_push(NYA_WindowHandle window, NYA_Layer layer) {
    NYA_Window* target = _nya_window_require(window, "nya_layer_push");
    if (target == nullptr) return;

    layer.window = window;
    nya_array_push_back(target->layer_stack, layer);

    NYA_LayerOnCreateFn on_create_fn = nya_callback_get(layer.on_create);
    if (on_create_fn != nullptr) on_create_fn(target);
}

NYA_Layer nya_layer_pop(NYA_WindowHandle window) {
    NYA_Window* target = _nya_window_require(window, "nya_layer_pop");
    if (target == nullptr || target->layer_stack->length == 0) return (NYA_Layer){ 0 };

    NYA_Layer layer = target->layer_stack->items[target->layer_stack->length - 1];

    NYA_LayerOnDestroyFn on_destroy_fn = nya_callback_get(layer.on_destroy);
    if (on_destroy_fn != nullptr) on_destroy_fn(target);

    nya_array_remove(target->layer_stack, target->layer_stack->length - 1);

    return layer;
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/**
 * Resolves a handle, complaining if it does not.
 *
 * A stale handle is a bug in the caller, but not one worth killing the process over: the window was
 * closed and something still refers to it. Warning and returning null degrades to a no-op, which is
 * what almost every caller here wants.
 * */
NYA_INTERNAL NYA_Window* _nya_window_require(NYA_WindowHandle window, NYA_ConstCString caller) {
    NYA_Window* target = nya_window_get(window);
    if (target == nullptr) nya_warn("%s called with a stale window handle (slot %u, generation %u).", caller, window.index, window.generation);

    return target;
}

NYA_INTERNAL NYA_Window* _nya_window_from_sdl(SDL_Window* sdl_window) {
    if (sdl_window == nullptr) return nullptr;

    for (u32 i = 0; i < NYA_WINDOW_MAX; i++) {
        NYA_Window* window = nya_window_at_slot(i);
        if (window != nullptr && window->sdl_window == sdl_window) return window;
    }

    return nullptr;
}

NYA_INTERNAL SDL_DisplayID _nya_window_display(NYA_WindowHandle window) {
    NYA_Window* target = nya_window_get(window);
    return target ? SDL_GetDisplayForWindow(target->sdl_window) : 0;
}

NYA_INTERNAL NYA_Rect _nya_rect_from_sdl(SDL_Rect rect) {
    return (NYA_Rect){ .x = rect.x, .y = rect.y, .width = rect.w, .height = rect.h };
}
