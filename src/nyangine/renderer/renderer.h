#pragma once

#include "SDL3/SDL_gpu.h"

#include "nyangine/base/base.h"
#include "nyangine/base/base_types.h"
#include "nyangine/core/core_event.h"
#include "nyangine/renderer/render_color.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * TYPES
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

typedef struct NYA_RenderSystem       NYA_RenderSystem;
typedef struct NYA_RenderSystemWindow NYA_RenderSystemWindow;
typedef struct NYA_Vertex             NYA_Vertex;

/*
 * ─────────────────────────────────────────────────────────
 * SYSTEM STRUCT
 * ─────────────────────────────────────────────────────────
 */

struct NYA_RenderSystem {
    SDL_GPUDevice* gpu_device;
};

struct NYA_RenderSystemWindow {
    SDL_GPURenderPass*    render_pass;
    SDL_GPUCommandBuffer* render_commands;
    SDL_GPUTexture*       swapchain_texture;
};

/*
 * ─────────────────────────────────────────────────────────
 * RENDERING STRUCTS
 * ─────────────────────────────────────────────────────────
 */

struct NYA_Vertex {
    f32x3     position;
    NYA_Color color;
    f32x3     normals;
    f32x2     uv;
};

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * FUNCTIONS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/*
 * ─────────────────────────────────────────────────────────
 * SYSTEM FUNCTIONS
 * ─────────────────────────────────────────────────────────
 */

typedef struct NYA_Window NYA_Window;

NYA_API NYA_Error nya_system_renderer_init(void) __attr_no_discard;
NYA_API void      nya_system_renderer_deinit(void);
NYA_API void      nya_system_renderer_for_window_init(NYA_Window* window);
NYA_API void      nya_system_renderer_for_window_deinit(NYA_Window* window);
NYA_API void      nya_system_renderer_set_vsync(b8 enabled);

/*
 * ─────────────────────────────────────────────────────────
 * RENDERING FUNCTIONS
 * ─────────────────────────────────────────────────────────
 */

/**
 * Acquires a swapchain image and opens a render pass on it.
 *
 * Returns false when there is nothing to draw into this frame, which is normal rather than an
 * error: a minimised or occluded window has no swapchain image, and on Wayland that happens
 * routinely. **The caller must not draw when this returns false** — there is no render pass, and
 * the command buffer has already been cancelled.
 * */
NYA_API b8   nya_render_begin(NYA_Window* window) __attr_no_discard;
NYA_API void nya_render_end(NYA_Window* window);
