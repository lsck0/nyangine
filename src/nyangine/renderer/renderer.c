#include "nyangine/nyangine.h"

#include "nyangine/renderer/render_color.c"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API DECLARATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

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

/*
 * ─────────────────────────────────────────────────────────
 * HEADLESS
 * ─────────────────────────────────────────────────────────
 */

#if NYA_HEADLESS_ENABLED

/*
 * No GPU device is created, so there is nothing to claim a window for, nothing to present, and
 * nothing to wait on at shutdown.
 *
 * The whole surface is stubbed here rather than sprinkling `#if NYA_HEADLESS_ENABLED` through the
 * real implementations: the two versions then cannot drift, and the frame loop needs no knowledge
 * of the mode at all. nya_render_begin returning false is the one that matters — that is the same
 * "nothing to draw into" answer an occluded window gives, so the loop already skips the layers
 * without a special case.
 */

NYA_Error nya_system_renderer_init(void) {
    nya_info("Render system initialized (headless: no GPU device, nothing will be drawn).");
    return NYA_OK;
}

void nya_system_renderer_deinit(void) {
    nya_info("Render system deinitialized (headless).");
}

void nya_system_renderer_for_window_init(NYA_Window* window) {
    nya_assert(window != nullptr);
    window->render_system = (NYA_RenderSystemWindow){ 0 };
}

void nya_system_renderer_for_window_deinit(NYA_Window* window) {
    nya_assert(window != nullptr);
    window->render_system = (NYA_RenderSystemWindow){ 0 };
}

void nya_system_renderer_set_vsync(b8 enabled) {
    nya_unused(enabled);
}

b8 nya_render_begin(NYA_Window* window) {
    nya_assert(window != nullptr);

    window->render_system.render_commands   = nullptr;
    window->render_system.render_pass       = nullptr;
    window->render_system.swapchain_texture = nullptr;

    return false;
}

void nya_render_end(NYA_Window* window) {
    nya_assert(window != nullptr);
}

#else

NYA_Error nya_system_renderer_init(void) {
    NYA_App* app = nya_app_get();

    // Recoverable rather than fatal: no usable GPU backend is the normal outcome on a machine
    // without drivers, or in CI. The caller gets to decide whether that means "quit" or "fall back
    // to something headless", which an assert here would take away.
    SDL_GPUDevice* gpu_device =
        SDL_CreateGPUDevice(SDL_GPU_SHADERFORMAT_DXIL | SDL_GPU_SHADERFORMAT_METALLIB | SDL_GPU_SHADERFORMAT_SPIRV, NYA_DEVELOPMENT_BUILD, nullptr);
    if (gpu_device == nullptr) return nya_error(NYA_ERROR_NOT_SUPPORTED, "SDL_CreateGPUDevice() failed: %s", SDL_GetError());

    app->render_system = (NYA_RenderSystem){
        .gpu_device = gpu_device,
    };

    nya_info("Render system initialized.");
    return NYA_OK;
}

void nya_system_renderer_deinit(void) {
    NYA_App* app = nya_app_get();

    SDL_WaitForGPUIdle(app->render_system.gpu_device);
    SDL_DestroyGPUDevice(app->render_system.gpu_device);

    nya_info("Render system deinitialized.");
}

void nya_system_renderer_for_window_init(NYA_Window* window) {
    nya_assert(window != nullptr);

    NYA_App* app = nya_app_get();

    b8 ok = SDL_ClaimWindowForGPUDevice(app->render_system.gpu_device, window->sdl_window);
    nya_assert(ok, "SDL_ClaimWindowForGPUDevice() failed: %s", SDL_GetError());

    window->render_system = (NYA_RenderSystemWindow){ 0 };

    /*
     * Claiming the window already installed a working swapchain. Everything below is an attempt to
     * improve on it, so a driver that refuses is a reason to keep the default, not to stop.
     *
     * Both parameters have to be asked about first: not every backend supports every combination,
     * and SDL_SetGPUSwapchainParameters rejects the whole call if either half is unsupported. This
     * asserted on the result, which is a hard crash on any driver that says no — observed on D3D12
     * under Wine, where SDR composition is unavailable.
     */
    SDL_GPUSwapchainComposition composition = SDL_GPU_SWAPCHAINCOMPOSITION_SDR;
    if (!SDL_WindowSupportsGPUSwapchainComposition(app->render_system.gpu_device, window->sdl_window, composition)) {
        nya_warn("Swapchain composition SDR is unsupported for window '%s'; keeping the driver's default.", window->title);
        return;
    }

    // MAILBOX is the low latency choice and the first thing a driver drops. VSYNC is required to be
    // supported everywhere, so it is the fallback rather than another thing to check.
    SDL_GPUPresentMode present_mode = app->options.vsync_enabled ? SDL_GPU_PRESENTMODE_VSYNC : SDL_GPU_PRESENTMODE_MAILBOX;
    if (!SDL_WindowSupportsGPUPresentMode(app->render_system.gpu_device, window->sdl_window, present_mode)) {
        nya_warn("Present mode %d is unsupported for window '%s'; falling back to vsync.", (int)present_mode, window->title);
        present_mode = SDL_GPU_PRESENTMODE_VSYNC;
    }

    if (!SDL_SetGPUSwapchainParameters(app->render_system.gpu_device, window->sdl_window, composition, present_mode)) {
        nya_warn("SDL_SetGPUSwapchainParameters() failed for window '%s', keeping the default: %s", window->title, SDL_GetError());
    }

    nya_info("Render system initialized for window '%s' (slot %u).", window->title, window->handle.index);
}

void nya_system_renderer_for_window_deinit(NYA_Window* window) {
    nya_assert(window != nullptr);

    NYA_App* app = nya_app_get();

    SDL_WaitForGPUIdle(app->render_system.gpu_device);
    SDL_ReleaseWindowFromGPUDevice(app->render_system.gpu_device, window->sdl_window);

    nya_info("Render system deinitialized for window '%s' (slot %u).", window->title, window->handle.index);
}

void nya_system_renderer_set_vsync(b8 enabled) {
    NYA_App* app = nya_app_get();

    if (app->options.vsync_enabled != enabled) {
        for (u32 slot = 0; slot < NYA_WINDOW_MAX; slot++) {
            NYA_Window* window = nya_window_at_slot(slot);
            if (window == nullptr) continue;

            SDL_GPUPresentMode present_mode = enabled ? SDL_GPU_PRESENTMODE_VSYNC : SDL_GPU_PRESENTMODE_MAILBOX;
            if (!SDL_WindowSupportsGPUPresentMode(app->render_system.gpu_device, window->sdl_window, present_mode)) {
                present_mode = SDL_GPU_PRESENTMODE_VSYNC;
            }

            if (!SDL_SetGPUSwapchainParameters(app->render_system.gpu_device, window->sdl_window, SDL_GPU_SWAPCHAINCOMPOSITION_SDR, present_mode)) {
                nya_warn("Could not change the present mode for window '%s': %s", window->title, SDL_GetError());
            }
        }
    }
}

/*
 * ─────────────────────────────────────────────────────────
 * RENDERING FUNCTIONS
 * ─────────────────────────────────────────────────────────
 */

b8 nya_render_begin(NYA_Window* window) {
    nya_assert(window != nullptr);

    NYA_App* app = nya_app_get();

    // Cleared up front. Everything below can bail, and nya_render_end keys off render_pass being
    // null to know the frame never opened; leaving last frame's handles in place is what used to
    // make the layers draw into a stale pass whose texture was the previous window size.
    window->render_system.render_commands   = nullptr;
    window->render_system.render_pass       = nullptr;
    window->render_system.swapchain_texture = nullptr;

    SDL_GPUCommandBuffer* command_buffer = SDL_AcquireGPUCommandBuffer(app->render_system.gpu_device);
    nya_assert(command_buffer != nullptr, "SDL_AcquireGPUCommandBuffer() failed: %s", SDL_GetError());

    SDL_GPUTexture* swapchain_texture = nullptr;

    u32 swapchain_width  = 0;
    u32 swapchain_height = 0;
    // The waiting variant, which SDL's own header recommends: "You should use
    // SDL_WaitAndAcquireGPUSwapchainTexture() unless you know what you are doing with timing."
    //
    // The non-waiting SDL_AcquireGPUSwapchainTexture hands back whatever is available right now,
    // which after a resize can be a texture whose dimensions are the window's *previous* size. The
    // frame then renders correctly into a buffer the compositor draws at the wrong size and offset,
    // which is what puts the image outside the window frame. It also lets command buffers pile up
    // while the GPU catches up.
    if (!SDL_WaitAndAcquireGPUSwapchainTexture(command_buffer, window->sdl_window, &swapchain_texture, &swapchain_width, &swapchain_height)) {
        nya_warn("SDL_WaitAndAcquireGPUSwapchainTexture() failed for window '%s': %s", window->title, SDL_GetError());
        SDL_CancelGPUCommandBuffer(command_buffer);
        return false;
    }

    // No image this frame: minimised, occluded, or the swapchain is mid resize. Not an error, but
    // there is nothing to render into, so the command buffer is thrown away rather than submitted.
    if (swapchain_texture == nullptr) {
        SDL_CancelGPUCommandBuffer(command_buffer);
        return false;
    }

    // Only trusted once the acquire succeeded. These are the drawable's dimensions in pixels, which
    // is not the window size when the display is scaled.
    window->screen_width  = swapchain_width;
    window->screen_height = swapchain_height;

    // Debug only. A swapchain that stops agreeing with the window is what makes the presented image
    // sit outside the frame, and it only diverges on a resize, so this reports the transition rather
    // than spamming every frame.
#if NYA_DEBUG
    {
        static u32 previous_width  = 0;
        static u32 previous_height = 0;

        if (swapchain_width != previous_width || swapchain_height != previous_height) {
            previous_width  = swapchain_width;
            previous_height = swapchain_height;

            s32 logical_width = 0, logical_height = 0, pixel_width = 0, pixel_height = 0;
            s32 position_x = 0, position_y = 0;
            SDL_GetWindowSize(window->sdl_window, &logical_width, &logical_height);
            SDL_GetWindowSizeInPixels(window->sdl_window, &pixel_width, &pixel_height);
            SDL_GetWindowPosition(window->sdl_window, &position_x, &position_y);

            SDL_WindowFlags flags = SDL_GetWindowFlags(window->sdl_window);

            nya_info(
                "swapchain=%ux%u logical=%dx%d pixels=%dx%d position=%d,%d scale=%.3f%s%s%s%s",
                swapchain_width,
                swapchain_height,
                logical_width,
                logical_height,
                pixel_width,
                pixel_height,
                position_x,
                position_y,
                (f64)SDL_GetWindowDisplayScale(window->sdl_window),
                (flags & SDL_WINDOW_MAXIMIZED) ? " MAXIMIZED" : "",
                (flags & SDL_WINDOW_FULLSCREEN) ? " FULLSCREEN" : "",
                (flags & SDL_WINDOW_HIGH_PIXEL_DENSITY) ? " HIGH_DPI" : "",
                (flags & SDL_WINDOW_MINIMIZED) ? " MINIMIZED" : ""
            );
        }
    }
#endif

    SDL_GPUColorTargetInfo target_info = {
        .texture = swapchain_texture,
        // Opaque black. `(SDL_FColor){ 0 }` looks like "clear to black" but sets a = 0 too, which
        // clears to *transparent*: the compositor then blends the desktop through every pixel the
        // frame does not draw over. It reads as the window rendering in the wrong place, because
        // what you are seeing is whatever sits behind it.
        .clear_color = (SDL_FColor){ .r = 0.0F, .g = 0.0F, .b = 0.0F, .a = 1.0F },
        .load_op     = SDL_GPU_LOADOP_CLEAR,
        .store_op    = SDL_GPU_STOREOP_STORE,
    };

    SDL_GPURenderPass* render_pass = SDL_BeginGPURenderPass(command_buffer, &target_info, 1, nullptr);
    nya_assert(render_pass != nullptr, "SDL_BeginGPURenderPass() failed: %s", SDL_GetError());

    window->render_system.render_commands   = command_buffer;
    window->render_system.swapchain_texture = swapchain_texture;
    window->render_system.render_pass       = render_pass;

    return true;
}

void nya_render_end(NYA_Window* window) {
    nya_assert(window != nullptr);

    // nya_render_begin decided there was nothing to draw into. Matching that with a no-op keeps the
    // caller free to run begin/draw/end unconditionally.
    if (window->render_system.render_pass == nullptr) return;

    SDL_EndGPURenderPass(window->render_system.render_pass);
    SDL_SubmitGPUCommandBuffer(window->render_system.render_commands);

    window->render_system.render_pass       = nullptr;
    window->render_system.render_commands   = nullptr;
    window->render_system.swapchain_texture = nullptr;
}

#endif // NYA_HEADLESS_ENABLED

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */
