/**
 * @file render_output.c
 * */
#include "assets/shader/uniforms.h"

#include "nyangine-core/nyangine.h"

#include "nyangine-core/renderer/render_internal.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PUBLIC API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

NYA_RenderOutput nya_render_output(NYA_Window* window) {
    nya_assert(window != nullptr);

    return window->render_system.output;
}

b8 nya_render_output_hdr_active(NYA_Window* window) {
    nya_assert(window != nullptr);

    return window->render_system.output_gpu.composition != SDL_GPU_SWAPCHAINCOMPOSITION_SDR;
}

void nya_render_output_scene_end(NYA_Window* window) {
    nya_assert(window != nullptr);

    NYA_RenderOutputGPU* gpu = &window->render_system.output_gpu;

    // a render texture is not the frame, and a scene drawn into one is marked when it reaches the window.
    if (gpu->composition == SDL_GPU_SWAPCHAINCOMPOSITION_SDR || window->render_system.draw_batch.target_is_texture) return;

    if (nya_asset_status(NYA_RENDER_PIPELINE_OUTPUT_MASK) != NYA_ASSET_STATUS_LOADED) return;

    // alpha is replaced under this pipeline, so what is drawn after raises it again by its own coverage.
    nya_render2d_fullscreen(window, NYA_RENDER_PIPELINE_OUTPUT_MASK, nullptr, 0, nullptr, 0);

    gpu->scene_marked = true;
}

void nya_render_output_set(NYA_Window* window, NYA_RenderOutput output) {
    nya_assert(window != nullptr);

    // applied by the next nya_render_begin: changing the swapchain would pull this frame's image from under it.
    window->render_system.output = output;
}

#if !NYA_HEADLESS_ENABLED

void _nya_render_output_apply(NYA_Window* window) {
    NYA_RenderSystemWindow* render     = &window->render_system;
    NYA_RenderOutputGPU*    gpu        = &render->output_gpu;
    SDL_GPUDevice*          gpu_device = nya_app_get()->render_system.gpu_device;

    // asking the driver what the window supports is not free, so only when the request changes.
    if (render->output.hdr == gpu->hdr_requested) return;

    gpu->hdr_requested = render->output.hdr;

    const NYA_RenderOutput output = render->output;

    // linear first: it needs no encoding beyond the sRGB curve, and HDR10 is the fallback some drivers offer alone.
    SDL_GPUSwapchainComposition composition = SDL_GPU_SWAPCHAINCOMPOSITION_SDR;

    if (output.hdr && SDL_WindowSupportsGPUSwapchainComposition(gpu_device, window->sdl_window, SDL_GPU_SWAPCHAINCOMPOSITION_HDR_EXTENDED_LINEAR)) {
        composition = SDL_GPU_SWAPCHAINCOMPOSITION_HDR_EXTENDED_LINEAR;
    } else if (output.hdr && SDL_WindowSupportsGPUSwapchainComposition(gpu_device, window->sdl_window, SDL_GPU_SWAPCHAINCOMPOSITION_HDR10_ST2084)) {
        composition = SDL_GPU_SWAPCHAINCOMPOSITION_HDR10_ST2084;
    } else if (output.hdr && gpu->composition == SDL_GPU_SWAPCHAINCOMPOSITION_SDR) {
        nya_log_info("HDR output was asked for, but window '%s' does not support it; staying SDR.", window->title);
    }

    if (composition == gpu->composition) return;

    if (!SDL_SetGPUSwapchainParameters(gpu_device, window->sdl_window, composition, _nya_render_present_mode(window))) {
        nya_log_warn("SDL_SetGPUSwapchainParameters() failed for window '%s', keeping the current output: %s", window->title, SDL_GetError());
        return;
    }

    gpu->composition = composition;

    if (composition == SDL_GPU_SWAPCHAINCOMPOSITION_SDR) {
        nya_gpu_texture_release(gpu_device, gpu->scene);

        *gpu = (NYA_RenderOutputGPU){ .composition = composition, .hdr_requested = gpu->hdr_requested };

        nya_log_info("Window '%s' presents in SDR.", window->title);
        return;
    }

    b8 linear = composition == SDL_GPU_SWAPCHAINCOMPOSITION_HDR_EXTENDED_LINEAR;

    nya_log_info("Window '%s' presents in HDR (%s).", window->title, linear ? "extended linear" : "HDR10");

    NYA_EXPECT(nya_asset_load((NYA_AssetLoadParameters){
        .type      = NYA_ASSET_TYPE_SHADER_FRAGMENT,
        .handle    = NYA_ASSET_SHADER_EFFECT_OUTPUT_HDR_FRAG,
        .as_shader = { .num_samplers = 1, .num_uniform_buffers = 1 },
    }), "while queueing the HDR output shader");

    // built for the format the swapchain has now, which differs between the two compositions.
    NYA_EXPECT(nya_asset_load((NYA_AssetLoadParameters){
        .type                 = NYA_ASSET_TYPE_GRAPHICS_PIPELINE,
        .handle               = linear ? NYA_RENDER_PIPELINE_OUTPUT_LINEAR : NYA_RENDER_PIPELINE_OUTPUT_PQ,
        .as_graphics_pipeline = {
            .window                 = window,
            .vertex_shader_handle   = NYA_ASSET_SHADER_PROCEDURAL_VERT,
            .fragment_shader_handle = NYA_ASSET_SHADER_EFFECT_OUTPUT_HDR_FRAG,
            .vertex_layout          = NYA_VERTEX_LAYOUT_2D,
            .color_format           = SDL_GetGPUSwapchainTextureFormat(gpu_device, window->sdl_window),
            .single_sampled         = true,
        },
    }), "while queueing the HDR output pipeline");

    NYA_EXPECT(nya_asset_load((NYA_AssetLoadParameters){
        .type = NYA_ASSET_TYPE_SHADER_FRAGMENT,
        .handle = NYA_ASSET_SHADER_EFFECT_OUTPUT_MASK_FRAG,
    }), "while queueing the HDR scene mask shader");

    // drawn inside the frame's pass, so at the window's format and sample count like everything else there.
    NYA_EXPECT(nya_asset_load((NYA_AssetLoadParameters){
        .type                 = NYA_ASSET_TYPE_GRAPHICS_PIPELINE,
        .handle               = NYA_RENDER_PIPELINE_OUTPUT_MASK,
        .as_graphics_pipeline = {
            .window                 = window,
            .vertex_shader_handle   = NYA_ASSET_SHADER_PROCEDURAL_VERT,
            .fragment_shader_handle = NYA_ASSET_SHADER_EFFECT_OUTPUT_MASK_FRAG,
            .vertex_layout          = NYA_VERTEX_LAYOUT_2D,
            .blend                  = NYA_BLEND_ALPHA_REPLACE,
        },
    }), "while queueing the HDR scene mask pipeline");
}

SDL_GPUTexture* _nya_render_output_target(NYA_Window* window, SDL_GPUTexture* swapchain, u32 width, u32 height) {
    nya_trace_scope(NYA_TRACE_HDR_OUTPUT);

    NYA_RenderOutputGPU* gpu        = &window->render_system.output_gpu;
    SDL_GPUDevice*       gpu_device = nya_app_get()->render_system.gpu_device;

    gpu->swapchain = nullptr;

    if (gpu->composition == SDL_GPU_SWAPCHAINCOMPOSITION_SDR) return swapchain;

    if (gpu->scene != nullptr && (gpu->width != width || gpu->height != height)) {
        nya_gpu_texture_release(gpu_device, gpu->scene);
        gpu->scene = nullptr;
    }

    if (gpu->scene == nullptr) {
        gpu->scene = nya_gpu_texture_create(
            gpu_device,
            &(SDL_GPUTextureCreateInfo){
                .type                 = SDL_GPU_TEXTURETYPE_2D,
                .format               = window->render_system.color_format,
                .usage                = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER,
                .width                = width,
                .height               = height,
                .layer_count_or_depth = 1,
                .num_levels           = 1,
            }
        );

        // drawn straight to the swapchain then, which is wrong in HDR but still a picture.
        if (gpu->scene == nullptr) {
            nya_log_error("SDL_CreateGPUTexture() failed for the HDR output's scene: %s", SDL_GetError());
            return swapchain;
        }

        gpu->width  = width;
        gpu->height = height;
    }

    gpu->swapchain = swapchain;

    return gpu->scene;
}

void _nya_render_output_present(NYA_Window* window) {
    nya_trace_scope(NYA_TRACE_HDR_OUTPUT);

    NYA_RenderSystemWindow* render = &window->render_system;
    NYA_RenderOutputGPU*    gpu    = &render->output_gpu;

    if (gpu->swapchain == nullptr) return;

    _nya_render_trace_mark(window);

    b8 linear = gpu->composition == SDL_GPU_SWAPCHAINCOMPOSITION_HDR_EXTENDED_LINEAR;

    // black until the pipeline has loaded, rather than SDR values read as linear light.
    SDL_GPURenderPass* pass = SDL_BeginGPURenderPass(
        render->render_commands,
        &(SDL_GPUColorTargetInfo){ .texture = gpu->swapchain, .clear_color = { 0.0F, 0.0F, 0.0F, 1.0F }, .load_op = SDL_GPU_LOADOP_CLEAR, .store_op = SDL_GPU_STOREOP_STORE },
        1,
        nullptr
    );

    render->frame_stats.passes++;

    // single sampled, like the swapchain image itself.
    SDL_GPUGraphicsPipeline* pipeline =
        nya_asset_graphics_pipeline(nya_asset_get(linear ? NYA_RENDER_PIPELINE_OUTPUT_LINEAR : NYA_RENDER_PIPELINE_OUTPUT_PQ), SDL_GPU_SAMPLECOUNT_1, false, true);

    if (pass != nullptr && pipeline != nullptr) {
        struct NYA_ShaderOutputUniform uniform = {
            .encoding    = linear ? 0.0F : 1.0F,
            // clamped here rather than asserted on set: it comes from a hand edited config.
            .peak        = render->output.peak > 0.0F ? nya_min(render->output.peak, NYA_RENDER_OUTPUT_PEAK_MAX) : NYA_RENDER_OUTPUT_PEAK,
            .highlight   = NYA_RENDER_OUTPUT_HIGHLIGHT,
            .paper_white = NYA_RENDER_OUTPUT_PAPER_WHITE_NITS,
            .masked      = gpu->scene_marked ? 1.0F : 0.0F,
        };

        SDL_BindGPUGraphicsPipeline(pass, pipeline);
        SDL_BindGPUFragmentSamplers(pass, 0, &(SDL_GPUTextureSamplerBinding){ .texture = gpu->scene, .sampler = _nya_render_sampler_for(NYA_TEXTURE_FILTER_NEAREST) }, 1);
        SDL_PushGPUFragmentUniformData(render->render_commands, 0, &uniform, sizeof(uniform));
        SDL_DrawGPUPrimitives(pass, 3, 1, 0, 0);
    }

    if (pass != nullptr) SDL_EndGPURenderPass(pass);

    gpu->swapchain    = nullptr;
    gpu->scene_marked = false;
}

#endif // NYA_HEADLESS_ENABLED
