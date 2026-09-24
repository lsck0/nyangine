#include "nyangine/nyangine.h"

#include "nyangine/renderer/render_internal.h"

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
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * VERTICES
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 *
 * Outside the headless split: packing floats into halves is arithmetic, and a headless build builds
 * the same vertices without uploading them.
 */

void nya_render_clear_color_set(NYA_Window* window, NYA_Color color) {
    nya_assert(window != nullptr);

    window->render_system.clear_color = color;
}

NYA_Color nya_render_clear_color(NYA_Window* window) {
    nya_assert(window != nullptr);

    return window->render_system.clear_color;
}

void nya_render_options_set(NYA_Window* window, NYA_RenderOptions options) {
    nya_assert(window != nullptr);
    nya_assert(options.msaa_samples <= 8, "msaa_samples is 0 for the default, or 1, 2, 4 or 8, got %u", options.msaa_samples);

    // a few degrees to nearly a half turn, and a quarter of the pixels at least, so a config file cannot break a frame.
    options.fov_y        = options.fov_y > 0.0F ? nya_clamp(options.fov_y, 0.1F, 3.0F) : 0.0F;
    options.render_scale = options.render_scale > 0.0F ? nya_clamp(options.render_scale, 0.25F, 1.0F) : 0.0F;

    nya_app_get()->render_system.options = options;
}

NYA_RenderOptions nya_render_options_get(NYA_Window* window) {
    nya_assert(window != nullptr);

    const NYA_RenderSystem* render_system = &nya_app_get()->render_system;

    return (NYA_RenderOptions){
        .msaa_samples = 1U << (u32)render_system->sample_count,
        .fov_y        = nya_render_fov_y(),
        .render_scale = render_system->options.render_scale > 0.0F ? render_system->options.render_scale : 1.0F,
    };
}

f32 nya_render_fov_y(void) {
    f32 fov_y = nya_app_get()->render_system.options.fov_y;

    return fov_y > 0.0F ? fov_y : NYA_RENDER3D_FOV_Y;
}

NYA_RenderFrameStats nya_render_frame_stats(NYA_Window* window) {
    nya_assert(window != nullptr);

    return window->render_system.frame_stats_last;
}

NYA_Vertex3D nya_vertex3d(f32x3 position, NYA_Color color, f32x3 normal, f32x2 uv) {
    /*
     * Colour is not clamped, unlike _nya_render2d_pack_color: values above one lift an emissive surface past the
     * bloom threshold (see GNY_CUBE3D_FIRE_COLOR_START), which is why the field is a half.
     */
    return (NYA_Vertex3D){
        .position = { position.x, position.y, position.z },
        .uv       = { (f16)uv.x, (f16)uv.y },
        .normals  = { normal.x, normal.y, normal.z },
        .color    = { (f16)color.r, (f16)color.g, (f16)color.b, (f16)color.a },
    };
}

f32x3 nya_vertex3d_position(NYA_Vertex3D vertex) {
    return (f32x3){ vertex.position[0], vertex.position[1], vertex.position[2] };
}

/*
 * ─────────────────────────────────────────────────────────
 * HEADLESS
 * ─────────────────────────────────────────────────────────
 */

#if NYA_HEADLESS_ENABLED

/* No GPU device, so no window to claim, nothing to present, nothing to wait on. */

NYA_Error nya_system_renderer_init(void) {
    nya_log_info("Render system initialized (headless: no GPU device, nothing will be drawn).");
    return NYA_OK;
}

void nya_system_renderer_deinit(void) {
    nya_log_info("Render system deinitialized (headless).");
}

void nya_system_renderer_for_window_init(NYA_Window* window) {
    nya_assert(window != nullptr);
    // opaque black, not the zeroed struct's transparent. see NYA_RenderSystemWindow.clear_color.
    window->render_system             = (NYA_RenderSystemWindow){ 0 };
    window->render_system.clear_color = NYA_COLOR_BLACK;
}

void nya_system_renderer_for_window_deinit(NYA_Window* window) {
    nya_assert(window != nullptr);
    // opaque black, not the zeroed struct's transparent.
    window->render_system             = (NYA_RenderSystemWindow){ 0 };
    window->render_system.clear_color = NYA_COLOR_BLACK;
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

    // recoverable: no GPU backend is normal without drivers or on CI, and the caller decides what that means.
    SDL_GPUDevice* gpu_device =
        SDL_CreateGPUDevice(SDL_GPU_SHADERFORMAT_DXIL | SDL_GPU_SHADERFORMAT_MSL | SDL_GPU_SHADERFORMAT_SPIRV, NYA_DEVELOPMENT_BUILD, nullptr);

    if (gpu_device == nullptr) {
        /*
         * One failure has a known cause and a one line answer, so it gets said rather than left as
         * "no supported backend".
         *
         * RenderDoc's Vulkan layer has no Wayland support. Under it SDL cannot build an instance that
         * can make a surface, its Vulkan backend reports itself unsupported, and the only thing anyone
         * sees is a program that closes the moment it is captured. Forcing SDL onto x11, which is
         * XWayland here, makes both of them work.
         *
         * Probed from the environment rather than by asking Vulkan, because by this point Vulkan has
         * already refused to say anything at all.
         */
        const b8 renderdoc = getenv("ENABLE_VULKAN_RENDERDOC_CAPTURE") != nullptr || getenv("RENDERDOC_CAPTUREOPTS") != nullptr;
        NYA_ConstCString video = SDL_GetCurrentVideoDriver();

        if (renderdoc && video != nullptr && nya_string_equals((NYA_CString)video, "wayland")) {
            return nya_error(NYA_ERROR_NOT_SUPPORTED,
                             "SDL_CreateGPUDevice() failed under RenderDoc on Wayland, which RenderDoc's Vulkan layer does not support. "
                             "Run with SDL_VIDEO_DRIVER=x11 to capture. (%s)",
                             SDL_GetError());
        }

        return nya_error(NYA_ERROR_NOT_SUPPORTED, "SDL_CreateGPUDevice() failed: %s", SDL_GetError());
    }

    app->render_system = (NYA_RenderSystem){
        .gpu_device = gpu_device,
        .allocator  = nya_arena_create(.name = "render_system_allocator"),
    };

    /* Linear filtering, clamped to the edge. See NYA_RenderSystem.sampler. */
    const SDL_GPUFilter filters[NYA_TEXTURE_FILTER_COUNT] = {
        [NYA_TEXTURE_FILTER_LINEAR]  = SDL_GPU_FILTER_LINEAR,
        [NYA_TEXTURE_FILTER_NEAREST] = SDL_GPU_FILTER_NEAREST,
    };

    for (u32 i = 0; i < NYA_TEXTURE_FILTER_COUNT; i++) {
        app->render_system.samplers[i] = SDL_CreateGPUSampler(
            gpu_device,
            &(SDL_GPUSamplerCreateInfo){
                .min_filter = filters[i],
                .mag_filter = filters[i],
                // nearest between mip levels too, so a minified tile does not blur again.
                .mipmap_mode    = i == NYA_TEXTURE_FILTER_NEAREST ? SDL_GPU_SAMPLERMIPMAPMODE_NEAREST : SDL_GPU_SAMPLERMIPMAPMODE_LINEAR,
                .address_mode_u = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE,
                .address_mode_v = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE,
                .address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE,
            }
        );
        nya_assert(app->render_system.samplers[i] != nullptr, "SDL_CreateGPUSampler() failed for filter %u: %s", i, SDL_GetError());
    }

    /* The sample count is settled when the first window is claimed. */
    app->render_system.sample_count = SDL_GPU_SAMPLECOUNT_1;

    nya_log_info("Render system initialized (%s).", SDL_GetGPUDeviceDriver(app->render_system.gpu_device));
    return NYA_OK;
}

void nya_system_renderer_deinit(void) {
    NYA_App* app = nya_app_get();

    SDL_WaitForGPUIdle(app->render_system.gpu_device);

    _nya_render_trace_shutdown();

    // the glyph atlases belong to no window, so they go with the renderer.
    nya_render2d_shutdown();

    // before the device, which owns them.
    for (u32 i = 0; i < NYA_TEXTURE_FILTER_COUNT; i++) {
        if (app->render_system.samplers[i] != nullptr) SDL_ReleaseGPUSampler(app->render_system.gpu_device, app->render_system.samplers[i]);
    }

    SDL_DestroyGPUDevice(app->render_system.gpu_device);

    // after the device, since it holds the CPU staging for the device's buffers.
    nya_arena_destroy(app->render_system.allocator);
    app->render_system.allocator = nullptr;

    nya_log_info("Render system deinitialized.");
}

/**
 * Builds or rebuilds the window's depth buffer for a swapchain of this size.
 * */
NYA_INTERNAL void _nya_renderer_ensure_depth_texture(NYA_Window* window, u32 width, u32 height);

/**
 * The most samples per pixel up to `samples` that the window's colour format and the depth format both take. Zero
 * asks for NYA_RENDER_MSAA_SAMPLES_DEFAULT.
 * */
NYA_INTERNAL SDL_GPUSampleCount _nya_renderer_sample_count_for(NYA_Window* window, u32 samples) __attr_no_discard;

/** Takes up a changed NYA_RenderOptions.msaa_samples. Only between frames, since a pass bakes in its count. */
NYA_INTERNAL void _nya_renderer_options_apply(NYA_Window* window);

/**
 * Rebuilds the window's multisampled colour buffer if the swapchain has changed size or the sample count changed,
 * and releases it when multisampling is off.
 * */
NYA_INTERNAL void _nya_renderer_ensure_msaa_texture(NYA_Window* window, u32 width, u32 height) {
    nya_trace_scope(NYA_TRACE_TARGETS);

    NYA_App* app = nya_app_get();

    if (window->render_system.msaa_texture != nullptr && window->render_system.msaa_width == width && window->render_system.msaa_height == height
        && window->render_system.msaa_sample_count == app->render_system.sample_count) {
        return;
    }

    // SDL frees a released texture once it is safe, so no wait.
    if (window->render_system.msaa_texture != nullptr) nya_gpu_texture_release(app->render_system.gpu_device, window->render_system.msaa_texture);

    window->render_system.msaa_texture = nullptr;

    if (app->render_system.sample_count == SDL_GPU_SAMPLECOUNT_1) return;

    window->render_system.msaa_texture = nya_gpu_texture_create(
        app->render_system.gpu_device,
        &(SDL_GPUTextureCreateInfo){
            .type                 = SDL_GPU_TEXTURETYPE_2D,
            .format               = window->render_system.color_format,
            // COLOR_TARGET only: a multisampled texture is never sampled, and some backends reject SAMPLER on one.
            .usage                = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET,
            .width                = width,
            .height               = height,
            .layer_count_or_depth = 1,
            .num_levels           = 1,
            .sample_count         = app->render_system.sample_count,
        }
    );
    nya_assert(window->render_system.msaa_texture != nullptr, "SDL_CreateGPUTexture() failed for the MSAA buffer: %s", SDL_GetError());

    window->render_system.msaa_width        = width;
    window->render_system.msaa_height       = height;
    window->render_system.msaa_sample_count = app->render_system.sample_count;
}

void _nya_renderer_ensure_depth_texture(NYA_Window* window, u32 width, u32 height) {
    nya_trace_scope(NYA_TRACE_TARGETS);

    NYA_App* app = nya_app_get();

    if (window->render_system.depth_texture != nullptr && window->render_system.depth_width == width
        && window->render_system.depth_height == height && window->render_system.depth_sample_count == app->render_system.sample_count) {
        return;
    }

    if (window->render_system.depth_texture != nullptr) {
        nya_gpu_texture_release(app->render_system.gpu_device, window->render_system.depth_texture);
    }

    window->render_system.depth_texture = nya_gpu_texture_create(
        app->render_system.gpu_device,
        &(SDL_GPUTextureCreateInfo){
            .type                 = SDL_GPU_TEXTURETYPE_2D,
            .format               = app->render_system.depth_format,
            .usage                = SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET,
            .width                = width,
            .height               = height,
            .layer_count_or_depth = 1,
            .num_levels           = 1,
            // the colour target's sample count: a pass's attachments must agree.
            .sample_count         = app->render_system.sample_count,
        }
    );
    nya_assert(window->render_system.depth_texture != nullptr, "SDL_CreateGPUTexture() failed for the depth buffer: %s", SDL_GetError());

    window->render_system.depth_width        = width;
    window->render_system.depth_height       = height;
    window->render_system.depth_sample_count = app->render_system.sample_count;
}

SDL_GPUSampleCount _nya_renderer_sample_count_for(NYA_Window* window, u32 samples) {
    NYA_RenderSystem* render_system = &nya_app_get()->render_system;

    if (samples == 0) samples = NYA_RENDER_MSAA_SAMPLES_DEFAULT;

    SDL_GPUTextureFormat color_format = window->render_system.color_format;
    if (color_format == SDL_GPU_TEXTUREFORMAT_INVALID) return SDL_GPU_SAMPLECOUNT_1;

    for (SDL_GPUSampleCount count = SDL_GPU_SAMPLECOUNT_8; count > SDL_GPU_SAMPLECOUNT_1; count--) {
        if ((1U << (u32)count) > samples) continue;
        if (!SDL_GPUTextureSupportsSampleCount(render_system->gpu_device, color_format, count)) continue;

        // before the first window the depth format is still open, and it is picked to suit this count.
        if (render_system->sample_count_decided && !SDL_GPUTextureSupportsSampleCount(render_system->gpu_device, render_system->depth_format, count)) {
            continue;
        }

        return count;
    }

    return SDL_GPU_SAMPLECOUNT_1;
}

void _nya_renderer_options_apply(NYA_Window* window) {
    NYA_RenderSystem* render_system = &nya_app_get()->render_system;

    if (render_system->options.msaa_samples == render_system->applied_msaa_samples) return;

    render_system->applied_msaa_samples = render_system->options.msaa_samples;

    SDL_GPUSampleCount sample_count = _nya_renderer_sample_count_for(window, render_system->options.msaa_samples);
    if (sample_count == render_system->sample_count) return;

    nya_log_info("Multisampling: %ux, was %ux.", 1U << (u32)sample_count, 1U << (u32)render_system->sample_count);

    // the window's buffers, render textures and pipelines each rebuild at their next use.
    render_system->sample_count = sample_count;
}

void nya_system_renderer_for_window_init(NYA_Window* window) {
    nya_assert(window != nullptr);

    NYA_App* app = nya_app_get();

    b8 ok = SDL_ClaimWindowForGPUDevice(app->render_system.gpu_device, window->sdl_window);
    nya_assert(ok, "SDL_ClaimWindowForGPUDevice() failed: %s", SDL_GetError());

    // opaque black, not the zeroed struct's transparent.
    window->render_system              = (NYA_RenderSystemWindow){ 0 };
    window->render_system.clear_color  = NYA_COLOR_BLACK;
    window->render_system.color_format = SDL_GetGPUSwapchainTextureFormat(app->render_system.gpu_device, window->sdl_window);

    /*
     * The asked for sample count as far as the device takes it. Decided on the first window, whose swapchain format
     * everything resolves onto; later changes go through nya_render_options_set.
     */
    if (!app->render_system.sample_count_decided) {
        app->render_system.sample_count         = _nya_renderer_sample_count_for(window, app->render_system.options.msaa_samples);
        app->render_system.applied_msaa_samples = app->render_system.options.msaa_samples;

        /* The depth format, settled here because pipelines bake it in. */
        app->render_system.depth_format = SDL_GPU_TEXTUREFORMAT_D32_FLOAT;

        if (SDL_GPUTextureSupportsFormat(
                app->render_system.gpu_device, SDL_GPU_TEXTUREFORMAT_D24_UNORM_S8_UINT, SDL_GPU_TEXTURETYPE_2D,
                SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET
            )
            && SDL_GPUTextureSupportsSampleCount(
                app->render_system.gpu_device, SDL_GPU_TEXTUREFORMAT_D24_UNORM_S8_UINT, app->render_system.sample_count
            )) {
            app->render_system.depth_format = SDL_GPU_TEXTUREFORMAT_D24_UNORM_S8_UINT;
        }

        app->render_system.sample_count_decided = true;

        nya_log_info("Multisampling: %ux.", 1U << (u32)app->render_system.sample_count);
        nya_log_info("Depth buffer: %s.", app->render_system.depth_format == SDL_GPU_TEXTUREFORMAT_D32_FLOAT ? "D32_FLOAT" : "D24_UNORM_S8_UINT");
    }

    /* The window's 2D batch. */

    NYA_Render2DBatch* batch       = &window->render_system.draw_batch;
    SDL_GPUDevice* gpu_device  = app->render_system.gpu_device;
    u32            buffer_size = (u32)(NYA_RENDER2D_MAX_VERTICES * sizeof(NYA_Vertex2D));

    *batch = (NYA_Render2DBatch){ 0 };

    /* Queued, not loaded. A flush without a loaded pipeline draws nothing, so the first frames are blank. */
    NYA_EXPECT(nya_asset_load((NYA_AssetLoadParameters){
      .type      = NYA_ASSET_TYPE_SHADER_VERTEX,
      .handle    = NYA_ASSET_SHADER_BATCH2D_VERT,
      .as_shader = {
          // one: the projection matrix, pushed per flush.
          .num_uniform_buffers = 1,
      },
  }), "while queueing the shape vertex shader");

    NYA_EXPECT(nya_asset_load((NYA_AssetLoadParameters){
      .type   = NYA_ASSET_TYPE_SHADER_FRAGMENT,
      .handle = NYA_ASSET_SHADER_SHAPE_FRAG,
  }), "while queueing the shape fragment shader");

    NYA_EXPECT(nya_asset_load((NYA_AssetLoadParameters){
      .type      = NYA_ASSET_TYPE_SHADER_FRAGMENT,
      .handle    = NYA_ASSET_SHADER_TEXTURED_FRAG,
      .as_shader = {
          // declared because SDL validates the count against the shader.
          .num_samplers = 1,
      },
  }), "while queueing the textured fragment shader");

    NYA_EXPECT(nya_asset_load((NYA_AssetLoadParameters){
      .type                 = NYA_ASSET_TYPE_GRAPHICS_PIPELINE,
      .handle               = NYA_RENDER2D_PIPELINE_SHAPES,
      .as_graphics_pipeline = {
          .window                 = window,
          .vertex_shader_handle   = NYA_ASSET_SHADER_BATCH2D_VERT,
          .fragment_shader_handle = NYA_ASSET_SHADER_SHAPE_FRAG,
          .blend                  = true,
          .vertex_layout          = NYA_VERTEX_LAYOUT_2D,
      },
  }), "while queueing the shape pipeline");

    NYA_EXPECT(nya_asset_load((NYA_AssetLoadParameters){
      .type                 = NYA_ASSET_TYPE_GRAPHICS_PIPELINE,
      .handle               = NYA_RENDER2D_PIPELINE_TEXTURED,
      .as_graphics_pipeline = {
          .window                 = window,
          .vertex_shader_handle   = NYA_ASSET_SHADER_BATCH2D_VERT,
          .fragment_shader_handle = NYA_ASSET_SHADER_TEXTURED_FRAG,
          // antialiased glyphs are mostly partial alpha, so without blending every glyph draws in a box.
          .blend                  = true,
          .vertex_layout          = NYA_VERTEX_LAYOUT_2D,
      },
  }), "while queueing the textured pipeline");

    NYA_EXPECT(nya_asset_load((NYA_AssetLoadParameters){
      .type      = NYA_ASSET_TYPE_SHADER_FRAGMENT,
      .handle    = NYA_ASSET_SHADER_TEXT_FRAG,
      .as_shader = {
          // the glyph atlas, the same texture the textured pipeline reads.
          .num_samplers = 1,
      },
  }), "while queueing the text fragment shader");

    /* Queued for every window whether or not text is drawn. */
    NYA_EXPECT(nya_asset_load((NYA_AssetLoadParameters){
      .type                 = NYA_ASSET_TYPE_GRAPHICS_PIPELINE,
      .handle               = NYA_RENDER2D_PIPELINE_TEXT,
      .as_graphics_pipeline = {
          .window                 = window,
          .vertex_shader_handle   = NYA_ASSET_SHADER_BATCH2D_VERT,
          .fragment_shader_handle = NYA_ASSET_SHADER_TEXT_FRAG,
          // glyphs are mostly partial coverage.
          .blend                  = true,
          .vertex_layout          = NYA_VERTEX_LAYOUT_2D,
      },
  }), "while queueing the text pipeline");

    NYA_EXPECT(nya_asset_load((NYA_AssetLoadParameters){
      .type      = NYA_ASSET_TYPE_SHADER_FRAGMENT,
      .handle    = NYA_ASSET_SHADER_TEXT_SDF_FRAG,
      .as_shader = {
          // the glyph atlas.
          .num_samplers = 1,
      },
  }), "while queueing the distance field text fragment shader");

    /* Queued for every window whether or not a font asks for it. */
    NYA_EXPECT(nya_asset_load((NYA_AssetLoadParameters){
      .type                 = NYA_ASSET_TYPE_GRAPHICS_PIPELINE,
      .handle               = NYA_RENDER2D_PIPELINE_TEXT_SDF,
      .as_graphics_pipeline = {
          .window                 = window,
          .vertex_shader_handle   = NYA_ASSET_SHADER_BATCH2D_VERT,
          .fragment_shader_handle = NYA_ASSET_SHADER_TEXT_SDF_FRAG,
          // a thresholded distance field is all partial alpha at its edge.
          .blend                  = true,
          .vertex_layout          = NYA_VERTEX_LAYOUT_2D,
      },
  }), "while queueing the distance field text pipeline");

    // what follows is counted against the 2D batch, then against the 3D scene from the mesh batch on.
    nya_trace_scope(NYA_TRACE_BATCH2D);

    batch->vertex_buffer = nya_gpu_buffer_create(
        gpu_device,
        &(SDL_GPUBufferCreateInfo){
            .usage = SDL_GPU_BUFFERUSAGE_VERTEX,
            .size  = buffer_size,
        }
    );
    nya_assert(batch->vertex_buffer != nullptr, "SDL_CreateGPUBuffer() failed: %s", SDL_GetError());

    batch->transfer_buffer = nya_gpu_transfer_buffer_create(
        gpu_device,
        &(SDL_GPUTransferBufferCreateInfo){
            .usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD,
            .size  = buffer_size,
        }
    );
    nya_assert(batch->transfer_buffer != nullptr, "SDL_CreateGPUTransferBuffer() failed: %s", SDL_GetError());

    u32 index_buffer_size = (u32)((u64)NYA_RENDER2D_MAX_INDICES * sizeof(u32));

    batch->index_buffer = nya_gpu_buffer_create(gpu_device, &(SDL_GPUBufferCreateInfo){ .usage = SDL_GPU_BUFFERUSAGE_INDEX, .size = index_buffer_size });
    nya_assert(batch->index_buffer != nullptr, "SDL_CreateGPUBuffer() failed for indices: %s", SDL_GetError());

    batch->index_transfer_buffer = nya_gpu_transfer_buffer_create(
        gpu_device,
        &(SDL_GPUTransferBufferCreateInfo){ .usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD, .size = index_buffer_size }
    );
    nya_assert(batch->index_transfer_buffer != nullptr, "SDL_CreateGPUTransferBuffer() failed for indices: %s", SDL_GetError());

    // from the render system arena: lives as long as the window, rewritten every frame.
    batch->vertices = nya_arena_alloc(app->render_system.allocator, NYA_RENDER2D_MAX_VERTICES * sizeof(NYA_Vertex2D));
    batch->indices  = nya_arena_alloc(app->render_system.allocator, (u64)NYA_RENDER2D_MAX_INDICES * sizeof(u32));

    // fixed: a frame needing more state changes than this has a batching problem. see NYA_Render2DDrawRange.
    batch->ranges = nya_arena_alloc(app->render_system.allocator, NYA_RENDER2D_MAX_RANGES * sizeof(NYA_Render2DDrawRange));
    batch->draws  = nya_arena_alloc(app->render_system.allocator, NYA_RENDER2D_MAX_RANGES * sizeof(NYA_Render2DDraw));

    /*
     * The 3D mesh batch, set up for every window. Creating it on the first nya_render3d_begin would allocate GPU
     * buffers mid-frame. A 2D game pays two buffers that are never uploaded to.
     */
    NYA_Render3DBatch* mesh_batch = &window->render_system.mesh_batch;

    *mesh_batch = (NYA_Render3DBatch){ 0 };

    mesh_batch->registered_meshes = nya_cache_create(
        app->render_system.allocator,
        NYA_Render3DRegisteredMesh,
        .name         = "registered_meshes",
        .capacity     = NYA_RENDER3D_MAX_REGISTERED_MESHES,
        // the handle without its terminator.
        .key_size_max = NYA_RENDER3D_MESH_HANDLE_MAX - 1,
        // refused when full: a registration is expected to stay until released.
        .eviction     = NYA_CACHE_EVICTION_REFUSE,
        .destructor   = _nya_render3d_registered_destroy,
    );

    NYA_EXPECT(nya_asset_load((NYA_AssetLoadParameters){
      .type      = NYA_ASSET_TYPE_SHADER_VERTEX,
      .handle    = NYA_ASSET_SHADER_MESH3D_VERT,
      .as_shader = {
          // one: the view-projection. the batch bakes transforms into vertices, so there is no model matrix.
          .num_uniform_buffers = 1,
      },
  }), "while queueing the mesh vertex shader");

    NYA_EXPECT(nya_asset_load((NYA_AssetLoadParameters){
      .type      = NYA_ASSET_TYPE_SHADER_FRAGMENT,
      .handle    = NYA_ASSET_SHADER_MESH3D_FRAG,
      .as_shader = {
          // one: light and material together, so a material change costs a draw call. see NYA_ShaderMesh3DUniform.
          .num_uniform_buffers = 1,

          /* One, for the shadow map, even in the untextured pipeline. */
          .num_samplers = 1,
      },
  }), "while queueing the mesh fragment shader");

    NYA_EXPECT(nya_asset_load((NYA_AssetLoadParameters){
      .type      = NYA_ASSET_TYPE_SHADER_FRAGMENT,
      .handle    = NYA_ASSET_SHADER_MESH3D_TEXTURED_FRAG,
      .as_shader = {
          .num_uniform_buffers = 1,

          // two: base colour at t0 and shadow map at t1, the order the shader declares.
          .num_samplers = 2,
      },
  }), "while queueing the textured mesh fragment shader");

    NYA_EXPECT(nya_asset_load((NYA_AssetLoadParameters){
      .type                 = NYA_ASSET_TYPE_GRAPHICS_PIPELINE,
      .handle               = NYA_RENDER3D_PIPELINE_MESH,
      .as_graphics_pipeline = {
          .window                 = window,
          .vertex_shader_handle   = NYA_ASSET_SHADER_MESH3D_VERT,
          .fragment_shader_handle = NYA_ASSET_SHADER_MESH3D_FRAG,
          .blend                  = true,
          .vertex_layout          = NYA_VERTEX_LAYOUT_3D,
          // depth testing, depth writing and back-face culling: what separates this from the 2D pipelines.
          .depth_test             = true,
          .depth_write            = true,
          .cull_back_faces        = true,
      },
  }), "while queueing the mesh pipeline");

    // same state, different fragment shader. changes to the 3D pass apply to both.
    NYA_EXPECT(nya_asset_load((NYA_AssetLoadParameters){
      .type                 = NYA_ASSET_TYPE_GRAPHICS_PIPELINE,
      .handle               = NYA_RENDER3D_PIPELINE_MESH_TEXTURED,
      .as_graphics_pipeline = {
          .window                 = window,
          .vertex_shader_handle   = NYA_ASSET_SHADER_MESH3D_VERT,
          .fragment_shader_handle = NYA_ASSET_SHADER_MESH3D_TEXTURED_FRAG,
          .blend                  = true,
          .vertex_layout          = NYA_VERTEX_LAYOUT_3D,
          .depth_test             = true,
          .depth_write            = true,
          .cull_back_faces        = true,
      },
  }), "while queueing the textured mesh pipeline");

    /* Foliage: the mesh fragment shader, but a vertex stage that bends model-space geometry in the wind. */
    NYA_EXPECT(nya_asset_load((NYA_AssetLoadParameters){
      .type      = NYA_ASSET_TYPE_SHADER_VERTEX,
      .handle    = NYA_ASSET_SHADER_FOLIAGE_VERT,
      .as_shader = {
          // two: the view-projection at b0 and the placement, wind and sway parameters at b1. see the skinned path.
          .num_uniform_buffers = 2,
      },
  }), "while queueing the foliage vertex shader");

    NYA_EXPECT(nya_asset_load((NYA_AssetLoadParameters){
      .type                 = NYA_ASSET_TYPE_GRAPHICS_PIPELINE,
      .handle               = NYA_RENDER3D_PIPELINE_FOLIAGE,
      .as_graphics_pipeline = {
          .window                 = window,
          .vertex_shader_handle   = NYA_ASSET_SHADER_FOLIAGE_VERT,
          .fragment_shader_handle = NYA_ASSET_SHADER_MESH3D_FRAG,
          .blend                  = true,
          .vertex_layout          = NYA_VERTEX_LAYOUT_3D,
          .depth_test             = true,
          .depth_write            = true,
          // no culling: grass blades and leaves are single sheets seen from both sides.
      },
  }), "while queueing the foliage pipeline");

    /* Instanced foliage: the foliage bend on a per-instance model matrix, so a whole field draws at once. */
    NYA_EXPECT(nya_asset_load((NYA_AssetLoadParameters){
      .type      = NYA_ASSET_TYPE_SHADER_VERTEX,
      .handle    = NYA_ASSET_SHADER_FOLIAGE_INSTANCED_VERT,
      .as_shader = {
          // two, as the scalar foliage vertex shader: the view-projection at b0 and the shared wind/sway at b1.
          .num_uniform_buffers = 2,
      },
  }), "while queueing the instanced foliage vertex shader");

    NYA_EXPECT(nya_asset_load((NYA_AssetLoadParameters){
      .type                 = NYA_ASSET_TYPE_GRAPHICS_PIPELINE,
      .handle               = NYA_RENDER3D_PIPELINE_FOLIAGE_INSTANCED,
      .as_graphics_pipeline = {
          .window                 = window,
          .vertex_shader_handle   = NYA_ASSET_SHADER_FOLIAGE_INSTANCED_VERT,
          .fragment_shader_handle = NYA_ASSET_SHADER_MESH3D_FRAG,
          .blend                  = true,
          // the per-instance model matrix arrives in buffer 1, the same layout the retained mesh path reads.
          .vertex_layout          = NYA_VERTEX_LAYOUT_3D_INSTANCED,
          .depth_test             = true,
          .depth_write            = true,
          // no culling: grass blades are single sheets seen from both sides, as the scalar foliage pipeline.
      },
  }), "while queueing the instanced foliage pipeline");

    /* The shadow pass: depth only, culling front faces. */
    NYA_EXPECT(nya_asset_load((NYA_AssetLoadParameters){
      .type      = NYA_ASSET_TYPE_SHADER_VERTEX,
      .handle    = NYA_ASSET_SHADER_MESH3D_SHADOW_VERT,
      .as_shader = { .num_uniform_buffers = 1 },
  }), "while queueing the shadow vertex shader");

    NYA_EXPECT(nya_asset_load((NYA_AssetLoadParameters){
      .type   = NYA_ASSET_TYPE_SHADER_FRAGMENT,
      .handle = NYA_ASSET_SHADER_MESH3D_SHADOW_FRAG,
  }), "while queueing the shadow fragment shader");

    NYA_EXPECT(nya_asset_load((NYA_AssetLoadParameters){
      .type                 = NYA_ASSET_TYPE_GRAPHICS_PIPELINE,
      .handle               = NYA_RENDER3D_PIPELINE_SHADOW,
      .as_graphics_pipeline = {
          .window                 = window,
          .vertex_shader_handle   = NYA_ASSET_SHADER_MESH3D_SHADOW_VERT,
          .fragment_shader_handle = NYA_ASSET_SHADER_MESH3D_SHADOW_FRAG,

          // no blending: averaging two depths describes neither surface.
          .blend         = false,
          .vertex_layout = NYA_VERTEX_LAYOUT_3D,
          .depth_test    = true,
          .depth_write   = true,

          // front faces are discarded here.
          .cull_front_faces = true,

          // the shadow format, or the pipeline is rejected when bound to the map.
          .color_format = NYA_RENDER3D_SHADOW_FORMAT,

          // the map is not multisampled; see single_sampled.
          .single_sampled = true,
      },
  }), "while queueing the shadow pipeline");

    /* Retained mesh pipelines, sharing every fragment stage above, so a model looks the same on either path. */

    NYA_EXPECT(nya_asset_load((NYA_AssetLoadParameters){
      .type      = NYA_ASSET_TYPE_SHADER_VERTEX,
      .handle    = NYA_ASSET_SHADER_MESH3D_INSTANCED_VERT,
      .as_shader = { .num_uniform_buffers = 1 },
  }), "while queueing the instanced mesh vertex shader");

    NYA_EXPECT(nya_asset_load((NYA_AssetLoadParameters){
      .type      = NYA_ASSET_TYPE_SHADER_VERTEX,
      .handle    = NYA_ASSET_SHADER_MESH3D_SHADOW_INSTANCED_VERT,
      .as_shader = { .num_uniform_buffers = 1 },
  }), "while queueing the instanced shadow vertex shader");

    NYA_EXPECT(nya_asset_load((NYA_AssetLoadParameters){
      .type                 = NYA_ASSET_TYPE_GRAPHICS_PIPELINE,
      .handle               = NYA_RENDER3D_PIPELINE_INSTANCED,
      .as_graphics_pipeline = {
          .window                 = window,
          .vertex_shader_handle   = NYA_ASSET_SHADER_MESH3D_INSTANCED_VERT,
          .fragment_shader_handle = NYA_ASSET_SHADER_MESH3D_FRAG,
          .blend                  = true,
          .vertex_layout          = NYA_VERTEX_LAYOUT_3D_INSTANCED,
          .depth_test             = true,
          .depth_write            = true,
          .cull_back_faces        = true,
      },
  }), "while queueing the instanced mesh pipeline");

    NYA_EXPECT(nya_asset_load((NYA_AssetLoadParameters){
      .type                 = NYA_ASSET_TYPE_GRAPHICS_PIPELINE,
      .handle               = NYA_RENDER3D_PIPELINE_INSTANCED_TEXTURED,
      .as_graphics_pipeline = {
          .window                 = window,
          .vertex_shader_handle   = NYA_ASSET_SHADER_MESH3D_INSTANCED_VERT,
          .fragment_shader_handle = NYA_ASSET_SHADER_MESH3D_TEXTURED_FRAG,
          .blend                  = true,
          .vertex_layout          = NYA_VERTEX_LAYOUT_3D_INSTANCED,
          .depth_test             = true,
          .depth_write            = true,
          .cull_back_faces        = true,
      },
  }), "while queueing the instanced textured mesh pipeline");

    NYA_EXPECT(nya_asset_load((NYA_AssetLoadParameters){
      .type                 = NYA_ASSET_TYPE_GRAPHICS_PIPELINE,
      .handle               = NYA_RENDER3D_PIPELINE_INSTANCED_SHADOW,
      .as_graphics_pipeline = {
          .window                 = window,
          .vertex_shader_handle   = NYA_ASSET_SHADER_MESH3D_SHADOW_INSTANCED_VERT,
          .fragment_shader_handle = NYA_ASSET_SHADER_MESH3D_SHADOW_FRAG,
          .blend                  = false,
          .vertex_layout          = NYA_VERTEX_LAYOUT_3D_INSTANCED,
          .depth_test             = true,
          .depth_write            = true,

          // front faces discarded, as in the immediate shadow pipeline.
          .cull_front_faces = true,
          .color_format     = NYA_RENDER3D_SHADOW_FORMAT,
          .single_sampled   = true,
      },
  }), "while queueing the instanced shadow pipeline");

    /*
     * The 2D light pass: a fullscreen triangle with no vertex buffer, reusing the procedural vertex shader. A
     * pipeline must name a layout, and 2D is the likeliest to be bound later.
     */
    NYA_EXPECT(nya_asset_load((NYA_AssetLoadParameters){
      .type   = NYA_ASSET_TYPE_SHADER_VERTEX,
      .handle = NYA_ASSET_SHADER_PROCEDURAL_VERT,
  }), "while queueing the fullscreen vertex shader");

    NYA_EXPECT(nya_asset_load((NYA_AssetLoadParameters){
      .type      = NYA_ASSET_TYPE_SHADER_FRAGMENT,
      .handle    = NYA_ASSET_SHADER_LIGHT2D_FRAG,
      .as_shader = { .num_uniform_buffers = 1 },
  }), "while queueing the light fragment shader");

    NYA_EXPECT(nya_asset_load((NYA_AssetLoadParameters){
      .type                 = NYA_ASSET_TYPE_GRAPHICS_PIPELINE,
      .handle               = NYA_RENDER2D_PIPELINE_LIGHT,
      .as_graphics_pipeline = {
          .window                 = window,
          .vertex_shader_handle   = NYA_ASSET_SHADER_PROCEDURAL_VERT,
          .fragment_shader_handle = NYA_ASSET_SHADER_LIGHT2D_FRAG,
          // the shader outputs how much light reaches a pixel, and the multiply applies it to the scene.
          .blend         = NYA_BLEND_MULTIPLY,
          .vertex_layout = NYA_VERTEX_LAYOUT_2D,
      },
  }), "while queueing the light pipeline");

    /* The sky comes after the light pipeline. */

    NYA_EXPECT(nya_asset_load((NYA_AssetLoadParameters){
      .type      = NYA_ASSET_TYPE_SHADER_FRAGMENT,
      .handle    = NYA_ASSET_SHADER_SKY3D_FRAG,
      .as_shader = { .num_uniform_buffers = 1 },
  }), "while queueing the sky fragment shader");

    NYA_EXPECT(nya_asset_load((NYA_AssetLoadParameters){
      .type                 = NYA_ASSET_TYPE_GRAPHICS_PIPELINE,
      .handle               = NYA_RENDER3D_PIPELINE_SKY,
      .as_graphics_pipeline = {
          .window                 = window,
          .vertex_shader_handle   = NYA_ASSET_SHADER_PROCEDURAL_VERT,
          .fragment_shader_handle = NYA_ASSET_SHADER_SKY3D_FRAG,

          // opaque and first, so the clear colour cannot tint the sky.
          .blend         = false,
          .vertex_layout = NYA_VERTEX_LAYOUT_2D,
      },
  }), "while queueing the sky pipeline");

    /* The skinned mesh pipeline. */
    NYA_EXPECT(nya_asset_load((NYA_AssetLoadParameters){
      .type      = NYA_ASSET_TYPE_SHADER_VERTEX,
      .handle    = NYA_ASSET_SHADER_MESH3D_SKINNED_VERT,
      .as_shader = { .num_uniform_buffers = 2 },
  }), "while queueing the skinned vertex shader");

    NYA_EXPECT(nya_asset_load((NYA_AssetLoadParameters){
      .type      = NYA_ASSET_TYPE_SHADER_VERTEX,
      .handle    = NYA_ASSET_SHADER_MESH3D_SHADOW_SKINNED_VERT,
      .as_shader = { .num_uniform_buffers = 2 },
  }), "while queueing the skinned shadow vertex shader");

    /* The depth-only skinned pipeline, so a character casts a shadow. */
    NYA_EXPECT(nya_asset_load((NYA_AssetLoadParameters){
      .type                 = NYA_ASSET_TYPE_GRAPHICS_PIPELINE,
      .handle               = NYA_RENDER3D_PIPELINE_SKINNED_SHADOW,
      .as_graphics_pipeline = {
          .window                 = window,
          .vertex_shader_handle   = NYA_ASSET_SHADER_MESH3D_SHADOW_SKINNED_VERT,
          .fragment_shader_handle = NYA_ASSET_SHADER_MESH3D_SHADOW_FRAG,
          .blend                  = false,
          .vertex_layout          = NYA_VERTEX_LAYOUT_3D_SKINNED,
          .depth_test             = true,
          .depth_write            = true,
          .cull_front_faces       = true,
          // the shadow format, matching the shadow target.
          .color_format = NYA_RENDER3D_SHADOW_FORMAT,
          .single_sampled = true,
      },
  }), "while queueing the skinned shadow pipeline");

    NYA_EXPECT(nya_asset_load((NYA_AssetLoadParameters){
      .type                 = NYA_ASSET_TYPE_GRAPHICS_PIPELINE,
      .handle               = NYA_RENDER3D_PIPELINE_SKINNED,
      .as_graphics_pipeline = {
          .window                 = window,
          .vertex_shader_handle   = NYA_ASSET_SHADER_MESH3D_SKINNED_VERT,
          .fragment_shader_handle = NYA_ASSET_SHADER_MESH3D_FRAG,
          .vertex_layout          = NYA_VERTEX_LAYOUT_3D_SKINNED,
          .depth_test             = true,
          .depth_write            = true,
          .cull_back_faces        = true,
      },
  }), "while queueing the skinned mesh pipeline");

    NYA_EXPECT(nya_asset_load((NYA_AssetLoadParameters){
      .type                 = NYA_ASSET_TYPE_GRAPHICS_PIPELINE,
      .handle               = NYA_RENDER3D_PIPELINE_SKINNED_TEXTURED,
      .as_graphics_pipeline = {
          .window                 = window,
          .vertex_shader_handle   = NYA_ASSET_SHADER_MESH3D_SKINNED_VERT,
          .fragment_shader_handle = NYA_ASSET_SHADER_MESH3D_TEXTURED_FRAG,
          .vertex_layout          = NYA_VERTEX_LAYOUT_3D_SKINNED,
          .depth_test             = true,
          .depth_write            = true,
          .cull_back_faces        = true,
      },
  }), "while queueing the textured skinned mesh pipeline");

    /* The transparent pass: the four pipelines above without depth writing. */
    /* The overlay pass: the transparent pipeline without depth testing either. */
    NYA_EXPECT(nya_asset_load((NYA_AssetLoadParameters){
      .type                 = NYA_ASSET_TYPE_GRAPHICS_PIPELINE,
      .handle               = NYA_RENDER3D_PIPELINE_OVERLAY,
      .as_graphics_pipeline = {
          .window                 = window,
          .vertex_shader_handle   = NYA_ASSET_SHADER_MESH3D_VERT,
          .fragment_shader_handle = NYA_ASSET_SHADER_MESH3D_FRAG,
          .blend                  = true,
          .vertex_layout          = NYA_VERTEX_LAYOUT_3D,

          // not tested, so nothing hides it; not written, so it hides nothing.
          .depth_test  = false,
          .depth_write = false,

          // both sides: a gizmo ring seen from behind is still a ring.
          .cull_back_faces = false,
      },
  }), "while queueing the overlay pipeline");

    NYA_EXPECT(nya_asset_load((NYA_AssetLoadParameters){
      .type                 = NYA_ASSET_TYPE_GRAPHICS_PIPELINE,
      .handle               = NYA_RENDER3D_PIPELINE_TRANSPARENT,
      .as_graphics_pipeline = {
          .window                 = window,
          .vertex_shader_handle   = NYA_ASSET_SHADER_MESH3D_VERT,
          .fragment_shader_handle = NYA_ASSET_SHADER_MESH3D_FRAG,
          .blend                  = true,
          .vertex_layout          = NYA_VERTEX_LAYOUT_3D,
          .depth_test             = true,
          .depth_write            = false,

          /* Both sides, so a translucent solid shows its back faces. */
          .cull_back_faces = false,
      },
  }), "while queueing the transparent mesh pipeline");

    NYA_EXPECT(nya_asset_load((NYA_AssetLoadParameters){
      .type                 = NYA_ASSET_TYPE_GRAPHICS_PIPELINE,
      .handle               = NYA_RENDER3D_PIPELINE_TRANSPARENT_TEXTURED,
      .as_graphics_pipeline = {
          .window                 = window,
          .vertex_shader_handle   = NYA_ASSET_SHADER_MESH3D_VERT,
          .fragment_shader_handle = NYA_ASSET_SHADER_MESH3D_TEXTURED_FRAG,
          .blend                  = true,
          .vertex_layout          = NYA_VERTEX_LAYOUT_3D,
          .depth_test             = true,
          .depth_write            = false,
          // both sides, as for the untextured transparent pipeline.
          .cull_back_faces        = false,
      },
  }), "while queueing the transparent textured mesh pipeline");

    NYA_EXPECT(nya_asset_load((NYA_AssetLoadParameters){
      .type                 = NYA_ASSET_TYPE_GRAPHICS_PIPELINE,
      .handle               = NYA_RENDER3D_PIPELINE_INSTANCED_TRANSPARENT,
      .as_graphics_pipeline = {
          .window                 = window,
          .vertex_shader_handle   = NYA_ASSET_SHADER_MESH3D_INSTANCED_VERT,
          .fragment_shader_handle = NYA_ASSET_SHADER_MESH3D_FRAG,
          .blend                  = true,
          .vertex_layout          = NYA_VERTEX_LAYOUT_3D_INSTANCED,
          .depth_test             = true,
          .depth_write            = false,
          // both sides, as for the untextured transparent pipeline.
          .cull_back_faces        = false,
      },
  }), "while queueing the instanced transparent pipeline");

    NYA_EXPECT(nya_asset_load((NYA_AssetLoadParameters){
      .type                 = NYA_ASSET_TYPE_GRAPHICS_PIPELINE,
      .handle               = NYA_RENDER3D_PIPELINE_INSTANCED_TRANSPARENT_TEXTURED,
      .as_graphics_pipeline = {
          .window                 = window,
          .vertex_shader_handle   = NYA_ASSET_SHADER_MESH3D_INSTANCED_VERT,
          .fragment_shader_handle = NYA_ASSET_SHADER_MESH3D_TEXTURED_FRAG,
          .blend                  = true,
          .vertex_layout          = NYA_VERTEX_LAYOUT_3D_INSTANCED,
          .depth_test             = true,
          .depth_write            = false,
          // both sides, as for the untextured transparent pipeline.
          .cull_back_faces        = false,
      },
  }), "while queueing the instanced transparent textured pipeline");

    /* The additive pass, for fire, sparks and glow. */
    NYA_EXPECT(nya_asset_load((NYA_AssetLoadParameters){
      .type                 = NYA_ASSET_TYPE_GRAPHICS_PIPELINE,
      .handle               = NYA_RENDER3D_PIPELINE_ADDITIVE,
      .as_graphics_pipeline = {
          .window                 = window,
          .vertex_shader_handle   = NYA_ASSET_SHADER_MESH3D_VERT,
          .fragment_shader_handle = NYA_ASSET_SHADER_MESH3D_FRAG,
          .blend                  = NYA_BLEND_ADDITIVE,
          .vertex_layout          = NYA_VERTEX_LAYOUT_3D,
          .depth_test             = true,
          .depth_write            = false,

          // both sides: a billboard is one quad, and a spark seen from behind is still a spark.
          .cull_back_faces = false,
      },
  }), "while queueing the additive pipeline");

    NYA_EXPECT(nya_asset_load((NYA_AssetLoadParameters){
      .type                 = NYA_ASSET_TYPE_GRAPHICS_PIPELINE,
      .handle               = NYA_RENDER3D_PIPELINE_ADDITIVE_TEXTURED,
      .as_graphics_pipeline = {
          .window                 = window,
          .vertex_shader_handle   = NYA_ASSET_SHADER_MESH3D_VERT,
          .fragment_shader_handle = NYA_ASSET_SHADER_MESH3D_TEXTURED_FRAG,
          .blend                  = NYA_BLEND_ADDITIVE,
          .vertex_layout          = NYA_VERTEX_LAYOUT_3D,
          .depth_test             = true,
          .depth_write            = false,
          .cull_back_faces        = false,
      },
  }), "while queueing the additive textured pipeline");

    /* Refractive glass. */
    NYA_EXPECT(nya_asset_load((NYA_AssetLoadParameters){
      .type      = NYA_ASSET_TYPE_SHADER_FRAGMENT,
      .handle    = NYA_ASSET_SHADER_MESH3D_GLASS_FRAG,
      .as_shader = { .num_samplers = 2, .num_uniform_buffers = 2 },
  }), "while queueing the glass fragment shader");

    NYA_EXPECT(nya_asset_load((NYA_AssetLoadParameters){
      .type                 = NYA_ASSET_TYPE_GRAPHICS_PIPELINE,
      .handle               = NYA_RENDER3D_PIPELINE_GLASS,
      .as_graphics_pipeline = {
          .window                 = window,
          .vertex_shader_handle   = NYA_ASSET_SHADER_MESH3D_VERT,
          .fragment_shader_handle = NYA_ASSET_SHADER_MESH3D_GLASS_FRAG,
          .blend                  = false,
          .vertex_layout          = NYA_VERTEX_LAYOUT_3D,
          .depth_test             = true,
          .depth_write            = true,

          // front faces only: the refraction already shows the interior, and far walls would sample the capture twice.
          .cull_back_faces = true,
      },
  }), "while queueing the glass pipeline");

    /* A flowing water surface: waves in the vertex stage, refraction and foam in the fragment stage. */
    NYA_EXPECT(nya_asset_load((NYA_AssetLoadParameters){
      .type      = NYA_ASSET_TYPE_SHADER_VERTEX,
      .handle    = NYA_ASSET_SHADER_WATER_VERT,
      .as_shader = {
          // two: the view-projection at b0 and the placement, waves, flow and wind at b1. see the foliage path.
          .num_uniform_buffers = 2,
      },
  }), "while queueing the water vertex shader");

    NYA_EXPECT(nya_asset_load((NYA_AssetLoadParameters){
      .type      = NYA_ASSET_TYPE_SHADER_FRAGMENT,
      .handle    = NYA_ASSET_SHADER_WATER_FRAG,
      // two samplers: the captured scene at t0 and the mirrored-sky planar reflection at t1 (water reads no
      // shadow map). two uniform blocks: the shared lighting at b0 and the water look at b1.
      .as_shader = { .num_samplers = 2, .num_uniform_buffers = 2 },
  }), "while queueing the water fragment shader");

    NYA_EXPECT(nya_asset_load((NYA_AssetLoadParameters){
      .type                 = NYA_ASSET_TYPE_GRAPHICS_PIPELINE,
      .handle               = NYA_RENDER3D_PIPELINE_WATER,
      .as_graphics_pipeline = {
          .window                 = window,
          .vertex_shader_handle   = NYA_ASSET_SHADER_WATER_VERT,
          .fragment_shader_handle = NYA_ASSET_SHADER_WATER_FRAG,

          // blended, so where the surface does not refract its opacity shows what is behind it.
          .blend           = true,
          .vertex_layout   = NYA_VERTEX_LAYOUT_3D,
          .depth_test      = true,
          .depth_write     = true,
          // both faces: a river is seen from above, but a low camera catches the far bank's underside.
          .cull_back_faces = false,
      },
  }), "while queueing the water pipeline");

    nya_trace_scope(NYA_TRACE_SCENE);

    u32 mesh_buffer_size = (u32)(NYA_RENDER3D_MAX_VERTICES * sizeof(NYA_Vertex3D));

    // room for every pass's list of the indices it sees, which share one upload.
    u32 mesh_index_size = (u32)((u64)NYA_RENDER3D_MAX_INDICES * NYA_RENDER3D_PASSES * sizeof(u16));

    mesh_batch->vertex_buffer = nya_gpu_buffer_create(gpu_device, &(SDL_GPUBufferCreateInfo){ .usage = SDL_GPU_BUFFERUSAGE_VERTEX, .size = mesh_buffer_size });
    nya_assert(mesh_batch->vertex_buffer != nullptr, "SDL_CreateGPUBuffer() failed for the 3D batch: %s", SDL_GetError());

    mesh_batch->transfer_buffer = nya_gpu_transfer_buffer_create(
        gpu_device,
        &(SDL_GPUTransferBufferCreateInfo){ .usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD, .size = mesh_buffer_size }
    );
    nya_assert(mesh_batch->transfer_buffer != nullptr, "SDL_CreateGPUTransferBuffer() failed for the 3D batch: %s", SDL_GetError());

    mesh_batch->index_buffer = nya_gpu_buffer_create(gpu_device, &(SDL_GPUBufferCreateInfo){ .usage = SDL_GPU_BUFFERUSAGE_INDEX, .size = mesh_index_size });
    nya_assert(mesh_batch->index_buffer != nullptr, "SDL_CreateGPUBuffer() failed for the 3D batch's indices: %s", SDL_GetError());

    mesh_batch->index_transfer_buffer = nya_gpu_transfer_buffer_create(
        gpu_device,
        &(SDL_GPUTransferBufferCreateInfo){ .usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD, .size = mesh_index_size }
    );
    nya_assert(mesh_batch->index_transfer_buffer != nullptr, "SDL_CreateGPUTransferBuffer() failed for the 3D batch's indices: %s", SDL_GetError());

    NYA_Arena* allocator = app->render_system.allocator;

    /* Two staging streams, each sized for the whole batch, sharing one GPU buffer. An object has three indices at least. */
    NYA_Render3DStream* streams[] = { &mesh_batch->opaque, &mesh_batch->transparent };

    for (u32 i = 0; i < nya_carray_length(streams); i++) {
        streams[i]->vertices = nya_arena_alloc(allocator, NYA_RENDER3D_MAX_VERTICES * sizeof(NYA_Vertex3D));
        streams[i]->indices  = nya_arena_alloc(allocator, (u64)NYA_RENDER3D_MAX_INDICES * sizeof(u16));
        streams[i]->objects  = nya_arena_alloc(allocator, (u64)(NYA_RENDER3D_MAX_INDICES / 3) * sizeof(NYA_Render3DObject));
    }

    // sort scratch sized for the index array, allocated once instead of per playback.
    mesh_batch->sort_keys         = nya_arena_alloc(allocator, (NYA_RENDER3D_MAX_INDICES / 3) * sizeof(NYA_Render3DSortKey));
    mesh_batch->sort_keys_scratch = nya_arena_alloc(allocator, (NYA_RENDER3D_MAX_INDICES / 3) * sizeof(NYA_Render3DSortKey));
    mesh_batch->sorted_indices    = nya_arena_alloc(allocator, (u64)NYA_RENDER3D_MAX_INDICES * sizeof(u16));

    mesh_batch->sorted_instances = nya_arena_alloc(allocator, NYA_RENDER3D_MAX_INSTANCES * sizeof(NYA_Render3DInstance));

    mesh_batch->segments         = nya_arena_alloc(allocator, NYA_RENDER3D_MAX_SEGMENTS * sizeof(NYA_Render3DSegment));
    mesh_batch->segment_uniforms = nya_arena_alloc(allocator, NYA_RENDER3D_MAX_SEGMENTS * sizeof(struct NYA_ShaderMesh3DUniform));

    nya_assert(mesh_batch->segments != nullptr && mesh_batch->segment_uniforms != nullptr);

    // the empty scene's open segment.
    *mesh_batch->segments = (NYA_Render3DSegment){ 0 };

    static b8 segments_registered = false;

    // the first window's count; every window has the same capacity.
    if (!segments_registered) {
        nya_ceiling_register("render3d_segments", NYA_RENDER3D_MAX_SEGMENTS, &mesh_batch->segment_count_worst);
        nya_ceiling_register("foliage_disturbers", NYA_RENDER3D_FOLIAGE_DISTURBERS_MAX, &mesh_batch->foliage_disturber_worst);
        nya_ceiling_register("grass_instances", NYA_RENDER3D_MAX_GRASS_INSTANCES, &mesh_batch->grass_instance_worst);
    }
    segments_registered = true;

    /* The instance buffer for the retained mesh path. */
    u32 instance_buffer_size = (u32)(NYA_RENDER3D_MAX_INSTANCES * sizeof(NYA_Render3DInstance));

    mesh_batch->instance_buffer =
        nya_gpu_buffer_create(gpu_device, &(SDL_GPUBufferCreateInfo){ .usage = SDL_GPU_BUFFERUSAGE_VERTEX, .size = instance_buffer_size });
    nya_assert(mesh_batch->instance_buffer != nullptr, "SDL_CreateGPUBuffer() failed for the 3D instance stream: %s", SDL_GetError());

    mesh_batch->instance_transfer_buffer = nya_gpu_transfer_buffer_create(
        gpu_device,
        &(SDL_GPUTransferBufferCreateInfo){ .usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD, .size = instance_buffer_size }
    );
    nya_assert(mesh_batch->instance_transfer_buffer != nullptr, "SDL_CreateGPUTransferBuffer() failed for the 3D instance stream: %s",
               SDL_GetError());

    mesh_batch->instances       = nya_arena_alloc(allocator, NYA_RENDER3D_MAX_INSTANCES * sizeof(NYA_Render3DInstance));
    mesh_batch->instance_passes = nya_arena_alloc(allocator, NYA_RENDER3D_MAX_INSTANCES * sizeof(u8));

    /* The grass instance stream: its own, larger buffer, since a dense field is the whole point. */
    u32 grass_instance_buffer_size = (u32)(NYA_RENDER3D_MAX_GRASS_INSTANCES * sizeof(NYA_Render3DInstance));

    mesh_batch->grass_instance_buffer =
        nya_gpu_buffer_create(gpu_device, &(SDL_GPUBufferCreateInfo){ .usage = SDL_GPU_BUFFERUSAGE_VERTEX, .size = grass_instance_buffer_size });
    nya_assert(mesh_batch->grass_instance_buffer != nullptr, "SDL_CreateGPUBuffer() failed for the grass instance stream: %s", SDL_GetError());

    mesh_batch->grass_instance_transfer_buffer = nya_gpu_transfer_buffer_create(
        gpu_device,
        &(SDL_GPUTransferBufferCreateInfo){ .usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD, .size = grass_instance_buffer_size }
    );
    nya_assert(mesh_batch->grass_instance_transfer_buffer != nullptr, "SDL_CreateGPUTransferBuffer() failed for the grass instance stream: %s",
               SDL_GetError());

    mesh_batch->grass_instances = nya_arena_alloc(allocator, NYA_RENDER3D_MAX_GRASS_INSTANCES * sizeof(NYA_Render3DInstance));

    // never sampled, as a scene without cascades has zero shadow strength.
    mesh_batch->shadow_none = nya_gpu_texture_create(
        gpu_device,
        &(SDL_GPUTextureCreateInfo){
            .type                 = SDL_GPU_TEXTURETYPE_2D,
            .format               = NYA_RENDER3D_SHADOW_FORMAT,
            .usage                = SDL_GPU_TEXTUREUSAGE_SAMPLER,
            .width                = 1,
            .height               = 1,
            .layer_count_or_depth = 1,
            .num_levels           = 1,
        }
    );
    nya_assert(mesh_batch->shadow_none != nullptr, "SDL_CreateGPUTexture() failed for the shadow placeholder: %s", SDL_GetError());

    /*
     * Claiming the window installed a working swapchain already. Everything below is an improvement, so a
     * refusal keeps the default.
     */
    SDL_GPUSwapchainComposition composition = SDL_GPU_SWAPCHAINCOMPOSITION_SDR;
    if (!SDL_WindowSupportsGPUSwapchainComposition(app->render_system.gpu_device, window->sdl_window, composition)) {
        nya_log_warn("Swapchain composition SDR is unsupported for window '%s'; keeping the driver's default.", window->title);
        return;
    }

    if (!SDL_SetGPUSwapchainParameters(app->render_system.gpu_device, window->sdl_window, composition, _nya_render_present_mode(window))) {
        nya_log_warn("SDL_SetGPUSwapchainParameters() failed for window '%s', keeping the default: %s", window->title, SDL_GetError());
    }

    nya_log_info("Render system initialized for window '%s' (slot %u).", window->title, window->handle.index);
}

void nya_system_renderer_for_window_deinit(NYA_Window* window) {
    nya_assert(window != nullptr);

    NYA_App* app = nya_app_get();

    SDL_WaitForGPUIdle(app->render_system.gpu_device);

    // after the wait, before the release: the device may still read last frame's copy.

    NYA_Render2DBatch* batch      = &window->render_system.draw_batch;
    SDL_GPUDevice* gpu_device = app->render_system.gpu_device;

    if (batch->vertex_buffer != nullptr) nya_gpu_buffer_release(gpu_device, batch->vertex_buffer);
    if (batch->transfer_buffer != nullptr) nya_gpu_transfer_buffer_release(gpu_device, batch->transfer_buffer);
    if (batch->index_buffer != nullptr) nya_gpu_buffer_release(gpu_device, batch->index_buffer);
    if (batch->index_transfer_buffer != nullptr) nya_gpu_transfer_buffer_release(gpu_device, batch->index_transfer_buffer);

    // the vertices came from the render system arena, freed as a whole.
    *batch = (NYA_Render2DBatch){ 0 };

    NYA_Render3DBatch* mesh_batch = &window->render_system.mesh_batch;

    if (mesh_batch->vertex_buffer != nullptr) nya_gpu_buffer_release(gpu_device, mesh_batch->vertex_buffer);
    if (mesh_batch->transfer_buffer != nullptr) nya_gpu_transfer_buffer_release(gpu_device, mesh_batch->transfer_buffer);
    if (mesh_batch->index_buffer != nullptr) nya_gpu_buffer_release(gpu_device, mesh_batch->index_buffer);
    if (mesh_batch->index_transfer_buffer != nullptr) nya_gpu_transfer_buffer_release(gpu_device, mesh_batch->index_transfer_buffer);

    // the instance stream is a vertex buffer to SDL; only the pipeline's input rate makes it per instance.
    if (mesh_batch->instance_buffer != nullptr) nya_gpu_buffer_release(gpu_device, mesh_batch->instance_buffer);
    if (mesh_batch->instance_transfer_buffer != nullptr) nya_gpu_transfer_buffer_release(gpu_device, mesh_batch->instance_transfer_buffer);

    // the grass instance stream, the same kind of vertex buffer, on its own larger allocation.
    if (mesh_batch->grass_instance_buffer != nullptr) nya_gpu_buffer_release(gpu_device, mesh_batch->grass_instance_buffer);
    if (mesh_batch->grass_instance_transfer_buffer != nullptr) nya_gpu_transfer_buffer_release(gpu_device, mesh_batch->grass_instance_transfer_buffer);

    // geometry the game registered belongs to the window; nothing else would release it.
    if (mesh_batch->registered_meshes != nullptr) nya_cache_destroy(mesh_batch->registered_meshes);

    // the refraction capture, created by the first glass draw.
    if (mesh_batch->refraction_capture != nullptr) nya_gpu_texture_release(gpu_device, mesh_batch->refraction_capture);

    mesh_batch->refraction_capture = nullptr;

    // the planar reflection capture, created by the first reflecting water surface.
    if (mesh_batch->reflection_capture != nullptr) nya_gpu_texture_release(gpu_device, mesh_batch->reflection_capture);

    mesh_batch->reflection_capture = nullptr;

    // the shadow map, created by the first scene that cast shadows, and its placeholder.
    _nya_render3d_shadow_release(window);

    if (mesh_batch->shadow_none != nullptr) nya_gpu_texture_release(gpu_device, mesh_batch->shadow_none);

    *mesh_batch = (NYA_Render3DBatch){ 0 };

    // decals allocate only while on, so switching them off is their release.
    nya_render3d_decals_set(window, (NYA_Render3DDecals){ 0 });
    nya_gpu_texture_release(gpu_device, window->render_system.output_gpu.scene);

    if (window->render_system.msaa_texture != nullptr) {
        nya_gpu_texture_release(app->render_system.gpu_device, window->render_system.msaa_texture);
        window->render_system.msaa_texture = nullptr;
    }

    if (window->render_system.depth_texture != nullptr) {
        nya_gpu_texture_release(app->render_system.gpu_device, window->render_system.depth_texture);
        window->render_system.depth_texture = nullptr;
    }

    SDL_ReleaseWindowFromGPUDevice(app->render_system.gpu_device, window->sdl_window);

    nya_log_info("Render system deinitialized for window '%s' (slot %u).", window->title, window->handle.index);
}

void nya_system_renderer_set_vsync(b8 enabled) {
    NYA_App* app = nya_app_get();

    if (app->options.vsync_enabled != enabled) {
        /* The option is updated here. */
        app->options.vsync_enabled = enabled;

        for (u32 slot = 0; slot < NYA_WINDOW_MAX; slot++) {
            NYA_Window* window = nya_window_at_slot(slot);
            if (window == nullptr) continue;

            // the output's composition, so toggling vsync does not drop HDR.
            if (!SDL_SetGPUSwapchainParameters(app->render_system.gpu_device, window->sdl_window, window->render_system.output_gpu.composition,
                                               _nya_render_present_mode(window))) {
                nya_log_warn("Could not change the present mode for window '%s': %s", window->title, SDL_GetError());
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

    // cleared first: everything below can bail, and nya_render_end checks render_pass for null. stale handles
    // would draw into a pass sized for the previous frame.
    window->render_system.render_commands   = nullptr;
    window->render_system.render_pass       = nullptr;
    window->render_system.swapchain_texture = nullptr;

    _nya_render_output_apply(window);

    SDL_GPUCommandBuffer* command_buffer = SDL_AcquireGPUCommandBuffer(app->render_system.gpu_device);
    nya_assert(command_buffer != nullptr, "SDL_AcquireGPUCommandBuffer() failed: %s", SDL_GetError());

    SDL_GPUTexture* swapchain_texture = nullptr;

    u32 swapchain_width  = 0;
    u32 swapchain_height = 0;
    // The waiting variant, as SDL's header recommends. The non-waiting acquire can return a texture at the
    // window's previous size after a resize, which the compositor draws at the wrong size and offset, and it
    // lets command buffers pile up.
    if (!SDL_WaitAndAcquireGPUSwapchainTexture(command_buffer, window->sdl_window, &swapchain_texture, &swapchain_width, &swapchain_height)) {
        nya_log_warn("SDL_WaitAndAcquireGPUSwapchainTexture() failed for window '%s': %s", window->title, SDL_GetError());
        SDL_CancelGPUCommandBuffer(command_buffer);
        return false;
    }

    // no image: minimised, occluded or mid resize. the command buffer is cancelled, not submitted.
    if (swapchain_texture == nullptr) {
        SDL_CancelGPUCommandBuffer(command_buffer);
        return false;
    }

    // the drawable's size in pixels, which differs from the window size on scaled displays.
    window->screen_width  = swapchain_width;
    window->screen_height = swapchain_height;

    // debug only, and only on change: a swapchain that disagrees with the window puts the image outside the frame.
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

            nya_log_info(
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

    // in HDR the frame is drawn in SDR and encoded onto the swapchain by nya_render_end.
    swapchain_texture = _nya_render_output_target(window, swapchain_texture, swapchain_width, swapchain_height);

    _nya_renderer_options_apply(window);
    _nya_renderer_ensure_msaa_texture(window, swapchain_width, swapchain_height);
    _nya_renderer_ensure_depth_texture(window, swapchain_width, swapchain_height);

    SDL_GPUTexture* msaa = window->render_system.msaa_texture;

    /*
     * With multisampling the pass draws into the MSAA buffer and resolves onto the swapchain; without it, into the
     * swapchain directly.
     */
    SDL_GPUColorTargetInfo target_info = {
        .texture          = msaa != nullptr ? msaa : swapchain_texture,
        .resolve_texture  = msaa != nullptr ? swapchain_texture : nullptr,
        // opaque black unless the window asks otherwise. a zeroed SDL_FColor has zero alpha, which lets the desktop
        // show through undrawn pixels. see nya_render_clear_color_set.
        .clear_color = (SDL_FColor){ .r = window->render_system.clear_color.r,
                                     .g = window->render_system.clear_color.g,
                                     .b = window->render_system.clear_color.b,
                                     .a = window->render_system.clear_color.a },
        .load_op          = SDL_GPU_LOADOP_CLEAR,
        /*
         * The opening pass resolves; passes reopened after it do not, until the last one. See
         * _nya_render2d_pass_resume.
         */
        .store_op         = msaa != nullptr ? SDL_GPU_STOREOP_RESOLVE_AND_STORE : SDL_GPU_STOREOP_STORE,
    };

    /*
     * Cleared every frame and stored, because the pass is suspended and reopened for uploads. DONT_CARE would
     * discard depth mid-frame.
     */
    SDL_GPUDepthStencilTargetInfo depth_info = {
        .texture          = window->render_system.depth_texture,
        .clear_depth      = 1.0F,
        .load_op          = SDL_GPU_LOADOP_CLEAR,
        .store_op         = SDL_GPU_STOREOP_STORE,
        .stencil_load_op  = SDL_GPU_LOADOP_DONT_CARE,
        .stencil_store_op = SDL_GPU_STOREOP_DONT_CARE,
    };

    // while tracing, drawing goes into command buffers of its own and this one only presents.
    command_buffer = _nya_render_trace_begin(window, command_buffer);

    SDL_GPURenderPass* render_pass = SDL_BeginGPURenderPass(command_buffer, &target_info, 1, &depth_info);
    nya_assert(render_pass != nullptr, "SDL_BeginGPURenderPass() failed: %s", SDL_GetError());

    window->render_system.frame_stats.passes++;

    window->render_system.render_commands     = command_buffer;
    window->render_system.swapchain_texture   = swapchain_texture;
    window->render_system.render_pass         = render_pass;
    window->render_system.render_pass_normals = false;

    /* The batch draws into the swapchain until told otherwise, and its projection comes from this target. */
    // counters are per frame.
    window->render_system.draw_batch.frame_flushes       = 0;
    window->render_system.draw_batch.frame_vertices      = 0;
    window->render_system.draw_batch.frame_indices       = 0;
    window->render_system.draw_batch.frame_dropped_draws = 0;
    window->render_system.draw_batch.pending_flush_reason = NYA_RENDER2D_FLUSH_FRAME_END;

    // set once by nya_render_end, so one pass per frame resolves.
    window->render_system.draw_batch.resolve_pending = false;

    for (u32 i = 0; i < NYA_RENDER2D_FLUSH_REASON_COUNT; i++) window->render_system.draw_batch.frame_flush_reasons[i] = 0;

    /* The 3D batch's counters too. */
    window->render_system.mesh_batch.frame_draw_calls    = 0;
    window->render_system.mesh_batch.frame_vertices      = 0;
    window->render_system.mesh_batch.frame_indices       = 0;
    window->render_system.mesh_batch.frame_instances     = 0;
    window->render_system.mesh_batch.frame_culled        = 0;
    window->render_system.mesh_batch.frame_occluded      = 0;
    window->render_system.mesh_batch.frame_dropped_draws = 0;
    window->render_system.mesh_batch.frame_passes        = 0;
    window->render_system.decals_gpu.frame_count         = 0;

    window->render_system.draw_batch.target_texture    = swapchain_texture;
    window->render_system.draw_batch.target_msaa       = msaa;
    window->render_system.draw_batch.target_sample_count = msaa != nullptr ? window->render_system.msaa_sample_count : SDL_GPU_SAMPLECOUNT_1;
    window->render_system.draw_batch.target_depth      = window->render_system.depth_texture;
    window->render_system.draw_batch.target_width      = swapchain_width;
    window->render_system.draw_batch.target_height     = swapchain_height;
    window->render_system.draw_batch.target_is_texture = false;
    window->render_system.draw_batch.target_normal       = nullptr;
    window->render_system.draw_batch.target_normal_msaa  = nullptr;
    window->render_system.draw_batch.shader_override     = nullptr;
    window->render_system.draw_batch.shader_uniform_size = 0;

    return true;
}

void nya_render_end(NYA_Window* window) {
    // submission and present, where a frame waits on the GPU, timed apart from the draw work.
    nya_perf_time_this_function();

    nya_assert(window != nullptr);

    // nya_render_begin found nothing to draw into, so begin/draw/end can run unconditionally.
    if (window->render_system.render_pass == nullptr) return;

    NYA_Render2DBatch* batch = &window->render_system.draw_batch;

    /* The frame's one multisample resolve, placed so the pass carrying it always has a draw. */
    if (batch->target_msaa != nullptr) {
        batch->resolve_pending = true;

        if (batch->index_count == 0) nya_render2d_rect(window, 0.0F, 0.0F, 1.0F, 1.0F, (NYA_Color){ 0.0F, 0.0F, 0.0F, 0.0F });
    }

    // whatever the layers queued without filling a batch.
    nya_render2d_flush(window);

    SDL_EndGPURenderPass(window->render_system.render_pass);
    window->render_system.render_pass = nullptr;

    _nya_render_output_present(window);

    _nya_render_trace_end(window);

    NYA_RenderSystemWindow* render = &window->render_system;

    // kept whole for the overlay, which draws before its own frame has finished counting.
    render->frame_stats_last            = render->frame_stats;
    render->frame_stats_last.draw_calls = render->draw_batch.frame_flushes + render->mesh_batch.frame_draw_calls;
    render->frame_stats                 = (NYA_RenderFrameStats){ 0 };

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

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * SHARED INTERNALS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

// maybe-unused: its callers are render2d.c and render3d.c, which the headless build replaces.
__attr_maybe_unused SDL_GPUSampler* _nya_render_sampler_for(NYA_TextureFilter filter) {
    // clamped: the filter comes from game-filled load parameters, and a bad value should still draw.
    if (filter >= NYA_TEXTURE_FILTER_COUNT) filter = NYA_TEXTURE_FILTER_LINEAR;

    return nya_app_get()->render_system.samplers[filter];
}

// maybe-unused: the headless build creates no swapchain.
__attr_maybe_unused SDL_GPUPresentMode _nya_render_present_mode(NYA_Window* window) {
    SDL_GPUDevice* gpu_device = nya_app_get()->render_system.gpu_device;

    // MAILBOX is the low latency choice and drivers often lack it. VSYNC is always supported.
    SDL_GPUPresentMode present_mode = nya_app_get()->options.vsync_enabled ? SDL_GPU_PRESENTMODE_VSYNC : SDL_GPU_PRESENTMODE_MAILBOX;

    if (SDL_WindowSupportsGPUPresentMode(gpu_device, window->sdl_window, present_mode)) return present_mode;

    nya_log_warn("Present mode %d is unsupported for window '%s'; falling back to vsync.", (int)present_mode, window->title);
    return SDL_GPU_PRESENTMODE_VSYNC;
}

// maybe-unused for the same reason as _nya_render_sampler_for.
__attr_maybe_unused SDL_GPUGraphicsPipeline* _nya_render_pipeline(NYA_Window* window, NYA_Asset* asset) {
    nya_assert(window != nullptr);

    const NYA_RenderSystemWindow* render = &window->render_system;

    // face culling is a switch, and the build with it off is only ever created once someone turns it off.
    return nya_asset_graphics_pipeline(asset, render->draw_batch.target_sample_count, render->render_pass_normals,
                                       nya_render_feature_enabled(window, NYA_RENDER_FEATURE_BACKFACE_CULLING));
}
