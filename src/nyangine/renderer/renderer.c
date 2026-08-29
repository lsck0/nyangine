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
    nya_log_info("Render system initialized (headless: no GPU device, nothing will be drawn).");
    return NYA_OK;
}

void nya_system_renderer_deinit(void) {
    nya_log_info("Render system deinitialized (headless).");
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
        .allocator  = nya_arena_create(.name = "render_system_allocator"),
    };

    /*
     * Linear filtering, clamped to the edge. See NYA_RenderSystem.sampler for why there is one.
     *
     * CLAMP_TO_EDGE rather than REPEAT matters for the glyph atlas and for sprite sheets: a uv that
     * lands a hair outside a sub-rectangle wraps to the far side of the texture under REPEAT, which
     * shows up as a stray line of some unrelated glyph along an edge.
     */
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
                // Nearest between mip levels for the point sampled one too, so a minified tile does
                // not blend two levels back into the blur that nearest was chosen to avoid.
                .mipmap_mode    = i == NYA_TEXTURE_FILTER_NEAREST ? SDL_GPU_SAMPLERMIPMAPMODE_NEAREST : SDL_GPU_SAMPLERMIPMAPMODE_LINEAR,
                .address_mode_u = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE,
                .address_mode_v = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE,
                .address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE,
            }
        );
        nya_assert(app->render_system.samplers[i] != nullptr, "SDL_CreateGPUSampler() failed for filter %u: %s", i, SDL_GetError());
    }

    /*
     * The sample count is settled when the first window is claimed, not here.
     *
     * It depends on the swapchain's texture format, and a swapchain belongs to a window — at this
     * point there is none, and asking with a null window returns INVALID, which silently disabled
     * multisampling altogether.
     */
    app->render_system.sample_count = SDL_GPU_SAMPLECOUNT_1;

    nya_log_info("Render system initialized.");
    return NYA_OK;
}

void nya_system_renderer_deinit(void) {
    NYA_App* app = nya_app_get();

    SDL_WaitForGPUIdle(app->render_system.gpu_device);

    // The glyph atlases, which belong to no window and so are freed with the renderer rather than
    // with one of them.
    nya_render2d_shutdown();

    // Before the device, which owns them.
    for (u32 i = 0; i < NYA_TEXTURE_FILTER_COUNT; i++) {
        if (app->render_system.samplers[i] != nullptr) SDL_ReleaseGPUSampler(app->render_system.gpu_device, app->render_system.samplers[i]);
    }

    SDL_DestroyGPUDevice(app->render_system.gpu_device);

    // After the device, because what it holds is the CPU side staging for buffers the device owned.
    nya_arena_destroy(app->render_system.allocator);
    app->render_system.allocator = nullptr;

    nya_log_info("Render system deinitialized.");
}

/**
 * Rebuilds the window's multisampled colour buffer if the swapchain has changed size.
 *
 * Lazy rather than done on a resize event: the swapchain's real dimensions are only known once a
 * texture has been acquired, and on a scaled display they are not the window size.
 * */
/**
 * Builds or rebuilds the window's depth buffer for a swapchain of this size.
 *
 * Unconditional, unlike the MSAA one, which returns early when multisampling is off. There is no
 * "depth is disabled" state: the pass always has a depth attachment so that 2D and 3D can share it.
 * */
NYA_INTERNAL void _nya_renderer_ensure_depth_texture(NYA_Window* window, u32 width, u32 height);

NYA_INTERNAL void _nya_renderer_ensure_msaa_texture(NYA_Window* window, u32 width, u32 height) {
    NYA_App* app = nya_app_get();

    if (app->render_system.sample_count == SDL_GPU_SAMPLECOUNT_1) return;
    if (window->render_system.msaa_texture != nullptr && window->render_system.msaa_width == width && window->render_system.msaa_height == height) {
        return;
    }

    // No wait: SDL frees a released texture once it is safe, so stalling the pipeline on every
    // resize bought nothing.
    if (window->render_system.msaa_texture != nullptr) SDL_ReleaseGPUTexture(app->render_system.gpu_device, window->render_system.msaa_texture);

    window->render_system.msaa_texture = SDL_CreateGPUTexture(
        app->render_system.gpu_device,
        &(SDL_GPUTextureCreateInfo){
            .type                 = SDL_GPU_TEXTURETYPE_2D,
            .format               = SDL_GetGPUSwapchainTextureFormat(app->render_system.gpu_device, window->sdl_window),
            // COLOR_TARGET only: a multisampled texture is rendered into and resolved from, never
            // sampled, and asking for SAMPLER on one is rejected outright by some backends.
            .usage                = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET,
            .width                = width,
            .height               = height,
            .layer_count_or_depth = 1,
            .num_levels           = 1,
            .sample_count         = app->render_system.sample_count,
        }
    );
    nya_assert(window->render_system.msaa_texture != nullptr, "SDL_CreateGPUTexture() failed for the MSAA buffer: %s", SDL_GetError());

    window->render_system.msaa_width  = width;
    window->render_system.msaa_height = height;
}

void _nya_renderer_ensure_depth_texture(NYA_Window* window, u32 width, u32 height) {
    NYA_App* app = nya_app_get();

    if (window->render_system.depth_texture != nullptr && window->render_system.depth_width == width
        && window->render_system.depth_height == height) {
        return;
    }

    if (window->render_system.depth_texture != nullptr) {
        SDL_ReleaseGPUTexture(app->render_system.gpu_device, window->render_system.depth_texture);
    }

    window->render_system.depth_texture = SDL_CreateGPUTexture(
        app->render_system.gpu_device,
        &(SDL_GPUTextureCreateInfo){
            .type                 = SDL_GPU_TEXTURETYPE_2D,
            .format               = app->render_system.depth_format,
            .usage                = SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET,
            .width                = width,
            .height               = height,
            .layer_count_or_depth = 1,
            .num_levels           = 1,
            // The same count as the colour target, because a pass's attachments must agree on it.
            // A single-sampled depth buffer against a four-sample colour buffer fails to begin.
            .sample_count         = app->render_system.sample_count,
        }
    );
    nya_assert(window->render_system.depth_texture != nullptr, "SDL_CreateGPUTexture() failed for the depth buffer: %s", SDL_GetError());

    window->render_system.depth_width  = width;
    window->render_system.depth_height = height;
}

void nya_system_renderer_for_window_init(NYA_Window* window) {
    nya_assert(window != nullptr);

    NYA_App* app = nya_app_get();

    b8 ok = SDL_ClaimWindowForGPUDevice(app->render_system.gpu_device, window->sdl_window);
    nya_assert(ok, "SDL_ClaimWindowForGPUDevice() failed: %s", SDL_GetError());

    window->render_system = (NYA_RenderSystemWindow){ 0 };

    /*
     * Four samples if the device will take them, one otherwise — decided on the first window, whose
     * swapchain format is what everything ultimately resolves onto.
     *
     * Asked rather than assumed: multisampling is optional, and a pipeline built for a count the
     * device does not support fails to create, which would take out every draw rather than only the
     * anti-aliasing. Decided once and never revisited, because a pipeline bakes the count in and a
     * second window changing it would invalidate every pipeline the first one built.
     */
    if (!app->render_system.sample_count_decided) {
        SDL_GPUTextureFormat swapchain_format = SDL_GetGPUSwapchainTextureFormat(app->render_system.gpu_device, window->sdl_window);

        if (swapchain_format != SDL_GPU_TEXTUREFORMAT_INVALID
            && SDL_GPUTextureSupportsSampleCount(app->render_system.gpu_device, swapchain_format, SDL_GPU_SAMPLECOUNT_4)) {
            app->render_system.sample_count = SDL_GPU_SAMPLECOUNT_4;
        }

        /*
         * The depth format, decided in the same breath and for the same reason: a pipeline bakes it
         * in, so it has to be settled before any pipeline is built.
         *
         * D24_UNORM_S8_UINT first because it is the cheapest thing every desktop backend has, and
         * D32_FLOAT as the fallback because it is the one guaranteed everywhere. Asked with the
         * sample count that was just chosen, since a format can be supported single-sampled and not
         * multisampled.
         */
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

        nya_log_info("Multisampling: %s.", app->render_system.sample_count == SDL_GPU_SAMPLECOUNT_4 ? "4x" : "unsupported, falling back to none");
        nya_log_info("Depth buffer: %s.", app->render_system.depth_format == SDL_GPU_TEXTUREFORMAT_D32_FLOAT ? "D32_FLOAT" : "D24_UNORM_S8_UINT");
    }

    /*
     * The window's 2D batch, set up here rather than behind a nya_render2d_for_window_init.
     *
     * That function was public API that no game could correctly call and nothing but this line ever
     * did — two entry points for one thing, with the real one hidden behind the decorative one.
     * NYA_Render2DBatch is declared in renderer.h and belongs to the render system, so it is set up
     * where the rest of the render system is.
     *
     * Before the swapchain tuning below, which returns early whenever the driver refuses a
     * parameter. Placed after it, that early return would leave the window with no batch and every
     * draw call into it silently doing nothing.
     */

    NYA_Render2DBatch* batch       = &window->render_system.draw_batch;
    SDL_GPUDevice* gpu_device  = app->render_system.gpu_device;
    u32            buffer_size = (u32)(NYA_RENDER2D_MAX_VERTICES * sizeof(NYA_Vertex2D));

    *batch = (NYA_Render2DBatch){ 0 };

    /*
     * Queued, not loaded. The asset system resolves these over the following frames, and a flush
     * whose pipeline is not loaded yet draws nothing — so the first frames are blank rather than
     * dereferencing a pipeline that does not exist.
     *
     * Queueing the same handles for a second window is harmless: the asset system keys on the
     * handle, so the shaders and pipelines are shared rather than rebuilt.
     */
    NYA_EXPECT(nya_asset_load((NYA_AssetLoadParameters){
      .type      = NYA_ASSET_TYPE_SHADER_VERTEX,
      .handle    = NYA_ASSET_SHADER_BATCH2D_VERT,
      .as_shader = {
          // One: the projection matrix, pushed per flush.
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
          // The sampler the textured pipeline reads from. Declared here rather than inferred,
          // because SDL validates the count against what the shader actually binds.
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
          // Text is the reason this is not optional: an anti-aliased glyph is mostly partial alpha,
          // and without blending every one draws inside an opaque box.
          .blend                  = true,
          .vertex_layout          = NYA_VERTEX_LAYOUT_2D,
      },
  }), "while queueing the textured pipeline");

    batch->vertex_buffer = SDL_CreateGPUBuffer(
        gpu_device,
        &(SDL_GPUBufferCreateInfo){
            .usage = SDL_GPU_BUFFERUSAGE_VERTEX,
            .size  = buffer_size,
        }
    );
    nya_assert(batch->vertex_buffer != nullptr, "SDL_CreateGPUBuffer() failed: %s", SDL_GetError());

    batch->transfer_buffer = SDL_CreateGPUTransferBuffer(
        gpu_device,
        &(SDL_GPUTransferBufferCreateInfo){
            .usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD,
            .size  = buffer_size,
        }
    );
    nya_assert(batch->transfer_buffer != nullptr, "SDL_CreateGPUTransferBuffer() failed: %s", SDL_GetError());

    u32 index_buffer_size = (u32)(NYA_RENDER2D_MAX_INDICES * sizeof(u32));

    batch->index_buffer = SDL_CreateGPUBuffer(gpu_device, &(SDL_GPUBufferCreateInfo){ .usage = SDL_GPU_BUFFERUSAGE_INDEX, .size = index_buffer_size });
    nya_assert(batch->index_buffer != nullptr, "SDL_CreateGPUBuffer() failed for indices: %s", SDL_GetError());

    batch->index_transfer_buffer = SDL_CreateGPUTransferBuffer(
        gpu_device,
        &(SDL_GPUTransferBufferCreateInfo){ .usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD, .size = index_buffer_size }
    );
    nya_assert(batch->index_transfer_buffer != nullptr, "SDL_CreateGPUTransferBuffer() failed for indices: %s", SDL_GetError());

    // From the render system arena rather than a frame arena: this lives as long as the window does,
    // and is rewritten every frame.
    batch->vertices = nya_arena_alloc(app->render_system.allocator, NYA_RENDER2D_MAX_VERTICES * sizeof(NYA_Vertex2D));
    batch->indices  = nya_arena_alloc(app->render_system.allocator, NYA_RENDER2D_MAX_INDICES * sizeof(u32));

    // The deferred draw ranges. Fixed, like the staging arrays, because a frame that needs more than
    // this many state changes has a batching problem the allocator cannot fix. See NYA_Render2DDrawRange.
    batch->ranges = nya_arena_alloc(app->render_system.allocator, NYA_RENDER2D_MAX_RANGES * sizeof(NYA_Render2DDrawRange));

    /*
     * ── The 3D mesh batch ──
     *
     * Set up unconditionally, alongside the 2D one, even for a game that never draws a triangle in
     * three dimensions. The alternative is bringing it up on the first nya_render3d_begin, which
     * means allocating GPU buffers in the middle of a frame — and getting it wrong means the first
     * frame of a scene silently draws nothing.
     *
     * What it costs a purely 2D game is one vertex buffer and one index buffer, never uploaded to,
     * plus the CPU staging behind them. Nothing per frame.
     */
    NYA_Render3DBatch* mesh_batch = &window->render_system.mesh_batch;

    *mesh_batch = (NYA_Render3DBatch){ 0 };

    NYA_EXPECT(nya_asset_load((NYA_AssetLoadParameters){
      .type      = NYA_ASSET_TYPE_SHADER_VERTEX,
      .handle    = NYA_ASSET_SHADER_MESH3D_VERT,
      .as_shader = {
          // One: the combined view-projection, pushed per flush. There is no model matrix — the
          // batch bakes the transform into the vertices. See render3d.h.
          .num_uniform_buffers = 1,
      },
  }), "while queueing the mesh vertex shader");

    NYA_EXPECT(nya_asset_load((NYA_AssetLoadParameters){
      .type      = NYA_ASSET_TYPE_SHADER_FRAGMENT,
      .handle    = NYA_ASSET_SHADER_MESH3D_FRAG,
      .as_shader = {
          // One: the light and the material together, which is what makes a material change cost a
          // draw call. See NYA_ShaderMesh3DUniform.
          .num_uniform_buffers = 1,

          /*
           * One, for the shadow map — even though this is the *untextured* pipeline.
           *
           * SDL validates this against what the compiled shader actually samples, and a count lower than
           * the truth is not caught here: the shader reads a descriptor nothing bound, which on this
           * driver is a GPUVM fault and a lost device rather than a message. It was zero for exactly as
           * long as the shader sampled nothing.
           */
          .num_samplers = 1,
      },
  }), "while queueing the mesh fragment shader");

    NYA_EXPECT(nya_asset_load((NYA_AssetLoadParameters){
      .type      = NYA_ASSET_TYPE_SHADER_FRAGMENT,
      .handle    = NYA_ASSET_SHADER_MESH3D_TEXTURED_FRAG,
      .as_shader = {
          .num_uniform_buffers = 1,

          // Two: the base colour at t0 and the shadow map at t1, in the order the shader declares them
          // and the flush binds them. See the note on the untextured shader's count.
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
          // The three things that separate this from every 2D pipeline in the tree, and the whole
          // reason those flags exist: geometry that occludes itself, and back faces discarded.
          .depth_test             = true,
          .depth_write            = true,
          .cull_back_faces        = true,
      },
  }), "while queueing the mesh pipeline");

    // Same depth and culling state; only the fragment shader differs. Anything that changes about the
    // 3D pass has to change in both, which is the standing cost of the two-pipeline approach.
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

    /*
     * The shadow pass: depth only, and the one 3D pipeline that culls *front* faces.
     *
     * Culling the front is the cheapest fix for the acne the bias also fights. The surface recorded in the
     * map becomes the far side of each object rather than the side facing the light, which moves the
     * recorded depth away from the surface being tested by the thickness of the object — so a lit face is
     * nowhere near its own occluder. The cost is that it is wrong for open geometry with no back side; a
     * single-sided ground plane casts nothing, which is exactly what a floor should do anyway.
     */
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

          // No blending: the map holds the nearest depth, and blending two depths averages them into a
          // distance that describes neither surface.
          .blend         = false,
          .vertex_layout = NYA_VERTEX_LAYOUT_3D,
          .depth_test    = true,
          .depth_write   = true,

          // See the note above: the *front* faces are the ones discarded here.
          .cull_front_faces = true,

          // R32_FLOAT, not the swapchain format. The pipeline has to be told, or it is built against a
          // target it will never be bound to and the draw is rejected at bind time.
          .color_format = SDL_GPU_TEXTUREFORMAT_R32_FLOAT,

          // The map is deliberately not multisampled; see single_sampled.
          .single_sampled = true,
      },
  }), "while queueing the shadow pipeline");

    /*
     * ── The retained mesh path ──
     *
     * Three more pipelines sharing every fragment stage above and differing only in their vertex one.
     * That sharing is deliberate: a model has to look identical whichever path drew it, or the split
     * stops being an internal performance decision and becomes something a caller has to reason about.
     */

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

          // Front faces discarded, matching the immediate shadow pipeline. See the note there.
          .cull_front_faces = true,
          .color_format     = SDL_GPU_TEXTUREFORMAT_R32_FLOAT,
          .single_sampled   = true,
      },
  }), "while queueing the instanced shadow pipeline");

    /*
     * ── The 2D light pass ──
     *
     * A fullscreen triangle with no vertex buffer, so it reuses the procedural vertex shader. The
     * layout is still declared as 2D: nothing is bound to feed it, but a pipeline has to name one and
     * the 2D table is the one whose stride matches what a later change would most likely bind.
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
          // The whole mechanism: the shader outputs how much light reaches a pixel, and the multiply
          // applies it to the scene already in the target.
          .blend         = NYA_BLEND_MULTIPLY,
          .vertex_layout = NYA_VERTEX_LAYOUT_2D,
      },
  }), "while queueing the light pipeline");

    /*
     * The sky, after the light pipeline and not before it, which is not a matter of taste.
     *
     * It is built on procedural.vert, and that shader is queued as part of the light2d block above. The
     * loading queue is processed in order, so registering this pipeline earlier asked the asset system to
     * assemble it from a shader that had not been read yet — which failed, named the shader, and left the
     * scene compositing through a pipeline that would never exist.
     *
     * No depth test and no depth write, which is what makes it a background rather than geometry: it is
     * drawn first and everything afterwards covers it, with nothing in the depth buffer to argue about.
     * A skybox drawn *last* at maximum depth is the other standard arrangement and needs a LESS_OR_EQUAL
     * comparison this renderer's pipelines do not expose.
     */

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

          // Opaque, and first. Blending a background against whatever the target was cleared to is a
          // way to tint the whole sky by accident.
          .blend         = false,
          .vertex_layout = NYA_VERTEX_LAYOUT_2D,
      },
  }), "while queueing the sky pipeline");

    /*
     * The outline, as an inverted hull: the mesh again, expanded along its normals with the *front* faces
     * discarded so only the part poking past the silhouette survives.
     *
     * Culling the front is what makes this work at all rather than being a decorative flag. Without it the
     * expanded shell is drawn over the model and hides it entirely.
     *
     * It writes depth as well as testing it, so a nearer object's ink still occludes a further object —
     * an outline that did not write depth would show through everything in front of it.
     */
    /*
     * The skinned mesh pipeline.
     *
     * Two uniform buffers, like the outline: the view-projection at b0 and the bone palette at b1.
     * Declaring one would be the bloom pipeline's bug again — Vulkan tolerates a shader reading a slot
     * the registration never mentioned and D3D12 refuses outright, so it would work here and fail on
     * Windows only.
     *
     * It reuses the ordinary mesh fragment stage. Nothing about lighting changes because a vertex was
     * moved by a bone rather than by the CPU, which is the point of doing the skinning in the vertex
     * stage at all.
     */
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

    /*
     * The depth-only skinned pipeline, so a character casts a shadow.
     *
     * It reuses the ordinary shadow fragment stage, and cull_front_faces for the reason that stage's
     * own registration gives at length: recording the far side of an object rather than the lit side
     * moves the stored depth away from the surface being tested, which is most of what fights acne.
     */
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
          // R32_FLOAT, matching the shadow pass target rather than the swapchain. See the pipeline above.
          .color_format = SDL_GPU_TEXTUREFORMAT_R32_FLOAT,
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
      .type      = NYA_ASSET_TYPE_SHADER_VERTEX,
      .handle    = NYA_ASSET_SHADER_MESH3D_OUTLINE_VERT,
      .as_shader = { .num_uniform_buffers = 2 },
  }), "while queueing the outline vertex shader");

    NYA_EXPECT(nya_asset_load((NYA_AssetLoadParameters){
      .type   = NYA_ASSET_TYPE_SHADER_FRAGMENT,
      .handle = NYA_ASSET_SHADER_MESH3D_OUTLINE_FRAG,
  }), "while queueing the outline fragment shader");

    NYA_EXPECT(nya_asset_load((NYA_AssetLoadParameters){
      .type                 = NYA_ASSET_TYPE_GRAPHICS_PIPELINE,
      .handle               = NYA_RENDER3D_PIPELINE_OUTLINE,
      .as_graphics_pipeline = {
          .window                 = window,
          .vertex_shader_handle   = NYA_ASSET_SHADER_MESH3D_OUTLINE_VERT,
          .fragment_shader_handle = NYA_ASSET_SHADER_MESH3D_OUTLINE_FRAG,
          .blend                  = true,
          .vertex_layout          = NYA_VERTEX_LAYOUT_3D_INSTANCED,
          .depth_test             = true,
          .depth_write            = true,
          .cull_front_faces       = true,
      },
  }), "while queueing the outline pipeline");

    /*
     * The transparent pass: four pipelines that are the four above with depth writing switched off.
     *
     * Written out rather than generated in a loop because each names its own pair of shaders, and a loop
     * over a table of four entries to save twenty lines of struct literal would hide which shader goes
     * with which pipeline — the exact thing that is worth being able to read at a glance here.
     */
    /*
     * The overlay pass: the transparent pipeline with depth *testing* off as well.
     *
     * One pipeline rather than four, because a gizmo is flat colour by definition — there is no
     * textured or additive gizmo, and the outline pipeline already covers the one other case.
     */
    NYA_EXPECT(nya_asset_load((NYA_AssetLoadParameters){
      .type                 = NYA_ASSET_TYPE_GRAPHICS_PIPELINE,
      .handle               = NYA_RENDER3D_PIPELINE_OVERLAY,
      .as_graphics_pipeline = {
          .window                 = window,
          .vertex_shader_handle   = NYA_ASSET_SHADER_MESH3D_VERT,
          .fragment_shader_handle = NYA_ASSET_SHADER_MESH3D_FRAG,
          .blend                  = true,
          .vertex_layout          = NYA_VERTEX_LAYOUT_3D,

          // The whole point. Not tested, so nothing hides it; not written, so it hides nothing.
          .depth_test  = false,
          .depth_write = false,

          // Both sides, like the transparent pass: a gizmo ring seen from behind is still a ring.
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

          /*
           * Both sides, unlike the opaque pass, and this is what makes a translucent *solid* work.
           *
           * Back-face culling exists because you cannot see the inside of an opaque object. You can see
           * the inside of a glass one — its far walls are visible through its near ones, and that is most
           * of what reads as glass. Culling them leaves a shape you see *through* into nothing, which
           * looks like a hole rather than like a solid.
           *
           * It is only correct because the transparent pass sorts per *triangle*: six faces of a cube
           * arriving in arbitrary order would blend wrongly, and sorting them back to front is exactly
           * what puts the far wall behind the near one. A renderer sorting per object could not turn this
           * on.
           *
           * The cost is that a closed translucent shape shades twice the fragments. That is the price of
           * it being a solid rather than a shell.
           */
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
          // Both sides, for the reason the untextured transparent pipeline gives at length.
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
          // Both sides, for the reason the untextured transparent pipeline gives at length.
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
          // Both sides, for the reason the untextured transparent pipeline gives at length.
          .cull_back_faces        = false,
      },
  }), "while queueing the instanced transparent textured pipeline");

    /*
     * The additive pass: what fire, sparks and glow go through.
     *
     * The same shaders again, with the blend switched from "over" to "plus". No depth write, for the
     * reason the transparent pass has none — and no sorting needed either, because addition does not care
     * what order it happens in, which is the other reason additive geometry is worth separating out.
     */
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

          // Both sides, unlike everything else here. A billboard is a single flat quad, and a spark drawn
          // from behind should still be a spark rather than nothing.
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

    /*
     * Refractive glass.
     *
     * Blending is *off* and depth writing is on, unlike the rest of the transparent pass — because this is
     * not a translucent surface. It samples what is behind it, tints that, and writes the result opaque;
     * blending a sample of the destination back over the destination would count it twice. See the note at
     * the top of mesh3d_glass.frag.hlsl.
     *
     * Depth *is* written, which follows from the same fact: an opaque surface occludes. That also means
     * glass drawn in front of glass hides it rather than compounding with it, which is the honest limit of
     * a single capture.
     *
     * Two samplers — the capture and the shadow map — so num_samplers is two, and two uniform blocks,
     * because the glass parameters are their own.
     */
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

          // Front faces only. A refractive solid already shows its interior through the refraction, and
          // drawing the far walls as well would sample the capture twice for the same pixel.
          .cull_back_faces = true,
      },
  }), "while queueing the glass pipeline");

    u32 mesh_buffer_size = (u32)(NYA_RENDER3D_MAX_VERTICES * sizeof(NYA_Vertex3D));
    u32 mesh_index_size  = (u32)(NYA_RENDER3D_MAX_INDICES * sizeof(u32));

    mesh_batch->vertex_buffer = SDL_CreateGPUBuffer(gpu_device, &(SDL_GPUBufferCreateInfo){ .usage = SDL_GPU_BUFFERUSAGE_VERTEX, .size = mesh_buffer_size });
    nya_assert(mesh_batch->vertex_buffer != nullptr, "SDL_CreateGPUBuffer() failed for the 3D batch: %s", SDL_GetError());

    mesh_batch->transfer_buffer = SDL_CreateGPUTransferBuffer(
        gpu_device,
        &(SDL_GPUTransferBufferCreateInfo){ .usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD, .size = mesh_buffer_size }
    );
    nya_assert(mesh_batch->transfer_buffer != nullptr, "SDL_CreateGPUTransferBuffer() failed for the 3D batch: %s", SDL_GetError());

    mesh_batch->index_buffer = SDL_CreateGPUBuffer(gpu_device, &(SDL_GPUBufferCreateInfo){ .usage = SDL_GPU_BUFFERUSAGE_INDEX, .size = mesh_index_size });
    nya_assert(mesh_batch->index_buffer != nullptr, "SDL_CreateGPUBuffer() failed for the 3D batch's indices: %s", SDL_GetError());

    mesh_batch->index_transfer_buffer = SDL_CreateGPUTransferBuffer(
        gpu_device,
        &(SDL_GPUTransferBufferCreateInfo){ .usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD, .size = mesh_index_size }
    );
    nya_assert(mesh_batch->index_transfer_buffer != nullptr, "SDL_CreateGPUTransferBuffer() failed for the 3D batch's indices: %s", SDL_GetError());

    /*
     * Two staging streams, each sized for the whole batch, sharing one GPU buffer.
     *
     * Sized for the whole rather than half each because the split between them is decided by what a frame
     * happens to draw: a scene with no transparency would waste half its capacity, and one that is all
     * glass would flush twice as often. _nya_render3d_reserve checks the *combined* total against the
     * buffer, so the CPU side is generous and the GPU side is exactly what it was.
     */
    mesh_batch->opaque.vertices = nya_arena_alloc(app->render_system.allocator, NYA_RENDER3D_MAX_VERTICES * sizeof(NYA_Vertex3D));
    mesh_batch->opaque.indices  = nya_arena_alloc(app->render_system.allocator, NYA_RENDER3D_MAX_INDICES * sizeof(u32));

    mesh_batch->transparent.vertices = nya_arena_alloc(app->render_system.allocator, NYA_RENDER3D_MAX_VERTICES * sizeof(NYA_Vertex3D));
    mesh_batch->transparent.indices  = nya_arena_alloc(app->render_system.allocator, NYA_RENDER3D_MAX_INDICES * sizeof(u32));

    // Sort scratch: one key per triangle the index array could hold, and somewhere to write the
    // reordered run. Allocated once here rather than per flush, which is a per-frame allocation.
    mesh_batch->sort_keys      = nya_arena_alloc(app->render_system.allocator, (NYA_RENDER3D_MAX_INDICES / 3) * sizeof(NYA_Render3DSortKey));
    mesh_batch->sort_keys_scratch = nya_arena_alloc(app->render_system.allocator, (NYA_RENDER3D_MAX_INDICES / 3) * sizeof(NYA_Render3DSortKey));
    mesh_batch->sorted_indices    = nya_arena_alloc(app->render_system.allocator, NYA_RENDER3D_MAX_INDICES * sizeof(u32));

    mesh_batch->sorted_instances = nya_arena_alloc(app->render_system.allocator, NYA_RENDER3D_MAX_INSTANCES * sizeof(NYA_Render3DInstance));

    /*
     * The instance buffer for the retained mesh path.
     *
     * A vertex buffer as far as SDL is concerned — an instance stream is bound with
     * SDL_BindGPUVertexBuffers like any other, at slot 1, and only the pipeline's input rate makes it
     * per-instance. There is no separate usage flag for it.
     */
    u32 instance_buffer_size = (u32)(NYA_RENDER3D_MAX_INSTANCES * sizeof(NYA_Render3DInstance));

    mesh_batch->instance_buffer =
        SDL_CreateGPUBuffer(gpu_device, &(SDL_GPUBufferCreateInfo){ .usage = SDL_GPU_BUFFERUSAGE_VERTEX, .size = instance_buffer_size });
    nya_assert(mesh_batch->instance_buffer != nullptr, "SDL_CreateGPUBuffer() failed for the 3D instance stream: %s", SDL_GetError());

    mesh_batch->instance_transfer_buffer = SDL_CreateGPUTransferBuffer(
        gpu_device,
        &(SDL_GPUTransferBufferCreateInfo){ .usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD, .size = instance_buffer_size }
    );
    nya_assert(mesh_batch->instance_transfer_buffer != nullptr, "SDL_CreateGPUTransferBuffer() failed for the 3D instance stream: %s",
               SDL_GetError());

    mesh_batch->instances = nya_arena_alloc(app->render_system.allocator, NYA_RENDER3D_MAX_INSTANCES * sizeof(NYA_Render3DInstance));

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
        nya_log_warn("Swapchain composition SDR is unsupported for window '%s'; keeping the driver's default.", window->title);
        return;
    }

    // MAILBOX is the low latency choice and the first thing a driver drops. VSYNC is required to be
    // supported everywhere, so it is the fallback rather than another thing to check.
    SDL_GPUPresentMode present_mode = app->options.vsync_enabled ? SDL_GPU_PRESENTMODE_VSYNC : SDL_GPU_PRESENTMODE_MAILBOX;
    if (!SDL_WindowSupportsGPUPresentMode(app->render_system.gpu_device, window->sdl_window, present_mode)) {
        nya_log_warn("Present mode %d is unsupported for window '%s'; falling back to vsync.", (int)present_mode, window->title);
        present_mode = SDL_GPU_PRESENTMODE_VSYNC;
    }

    if (!SDL_SetGPUSwapchainParameters(app->render_system.gpu_device, window->sdl_window, composition, present_mode)) {
        nya_log_warn("SDL_SetGPUSwapchainParameters() failed for window '%s', keeping the default: %s", window->title, SDL_GetError());
    }

    nya_log_info("Render system initialized for window '%s' (slot %u).", window->title, window->handle.index);
}

void nya_system_renderer_for_window_deinit(NYA_Window* window) {
    nya_assert(window != nullptr);

    NYA_App* app = nya_app_get();

    SDL_WaitForGPUIdle(app->render_system.gpu_device);

    // After the wait, before the release: these are GPU buffers, so freeing them while the device
    // may still be reading last frame's copy is a use after free the driver reports as a crash
    // somewhere unrelated.

    NYA_Render2DBatch* batch      = &window->render_system.draw_batch;
    SDL_GPUDevice* gpu_device = app->render_system.gpu_device;

    if (batch->vertex_buffer != nullptr) SDL_ReleaseGPUBuffer(gpu_device, batch->vertex_buffer);
    if (batch->transfer_buffer != nullptr) SDL_ReleaseGPUTransferBuffer(gpu_device, batch->transfer_buffer);
    if (batch->index_buffer != nullptr) SDL_ReleaseGPUBuffer(gpu_device, batch->index_buffer);
    if (batch->index_transfer_buffer != nullptr) SDL_ReleaseGPUTransferBuffer(gpu_device, batch->index_transfer_buffer);

    // The vertices came from the render system arena, which frees as a whole. Nothing here.
    *batch = (NYA_Render2DBatch){ 0 };

    NYA_Render3DBatch* mesh_batch = &window->render_system.mesh_batch;

    if (mesh_batch->vertex_buffer != nullptr) SDL_ReleaseGPUBuffer(gpu_device, mesh_batch->vertex_buffer);
    if (mesh_batch->transfer_buffer != nullptr) SDL_ReleaseGPUTransferBuffer(gpu_device, mesh_batch->transfer_buffer);
    if (mesh_batch->index_buffer != nullptr) SDL_ReleaseGPUBuffer(gpu_device, mesh_batch->index_buffer);
    if (mesh_batch->index_transfer_buffer != nullptr) SDL_ReleaseGPUTransferBuffer(gpu_device, mesh_batch->index_transfer_buffer);

    // The instance stream. A vertex buffer as far as SDL is concerned; only the pipeline's input rate
    // makes it per-instance, so it is released exactly like one.
    if (mesh_batch->instance_buffer != nullptr) SDL_ReleaseGPUBuffer(gpu_device, mesh_batch->instance_buffer);
    if (mesh_batch->instance_transfer_buffer != nullptr) SDL_ReleaseGPUTransferBuffer(gpu_device, mesh_batch->instance_transfer_buffer);

    // Geometry the game registered. Owned by the window, so it goes with it — a registered mesh has no
    // asset behind it to be unloaded and nothing else would ever release these.
    for (u32 i = 0; i < NYA_RENDER3D_MAX_REGISTERED_MESHES; i++) {
        if (mesh_batch->registered_meshes[i].vertices == nullptr) continue;

        SDL_ReleaseGPUBuffer(gpu_device, mesh_batch->registered_meshes[i].vertices);

        // A copy that never got a frame to happen in, if the window is torn down before one.
        if (mesh_batch->registered_meshes[i].pending_upload != nullptr) {
            SDL_ReleaseGPUTransferBuffer(gpu_device, mesh_batch->registered_meshes[i].pending_upload);
        }

        mesh_batch->registered_meshes[i] = (NYA_Render3DRegisteredMesh){ 0 };
    }

    // The refraction capture, created lazily by the first glass draw. Same lifetime rule as the shadow map.
    if (mesh_batch->refraction_capture != nullptr) SDL_ReleaseGPUTexture(gpu_device, mesh_batch->refraction_capture);

    mesh_batch->refraction_capture = nullptr;

    // The shadow map, which is created lazily by the first pass and lives as long as the window does.
    if (mesh_batch->shadow_color != nullptr) SDL_ReleaseGPUTexture(gpu_device, mesh_batch->shadow_color);
    if (mesh_batch->shadow_depth != nullptr) SDL_ReleaseGPUTexture(gpu_device, mesh_batch->shadow_depth);

    mesh_batch->shadow_color = nullptr;
    mesh_batch->shadow_depth = nullptr;

    *mesh_batch = (NYA_Render3DBatch){ 0 };

    if (window->render_system.msaa_texture != nullptr) {
        SDL_ReleaseGPUTexture(app->render_system.gpu_device, window->render_system.msaa_texture);
        window->render_system.msaa_texture = nullptr;
    }

    if (window->render_system.depth_texture != nullptr) {
        SDL_ReleaseGPUTexture(app->render_system.gpu_device, window->render_system.depth_texture);
        window->render_system.depth_texture = nullptr;
    }

    SDL_ReleaseWindowFromGPUDevice(app->render_system.gpu_device, window->sdl_window);

    nya_log_info("Render system deinitialized for window '%s' (slot %u).", window->title, window->handle.index);
}

void nya_system_renderer_set_vsync(b8 enabled) {
    NYA_App* app = nya_app_get();

    if (app->options.vsync_enabled != enabled) {
        /*
         * The option is updated here, not left to the caller.
         *
         * This reads app->options.vsync_enabled as "what is currently applied" but used to never
         * write it, so it was only correct because its one caller — nya_app_options_update —
         * assigns the whole options struct immediately afterwards. It is NYA_API, and a direct
         * caller got the field and the real present mode disagreeing: the software frame limiter in
         * core_app.c keys off the same field, and the next call in the opposite direction saw no
         * change and did nothing, so vsync could not be turned back off.
         *
         * Written before the loop, since the loop only reports failures and does not roll back.
         */
        app->options.vsync_enabled = enabled;

        for (u32 slot = 0; slot < NYA_WINDOW_MAX; slot++) {
            NYA_Window* window = nya_window_at_slot(slot);
            if (window == nullptr) continue;

            SDL_GPUPresentMode present_mode = enabled ? SDL_GPU_PRESENTMODE_VSYNC : SDL_GPU_PRESENTMODE_MAILBOX;
            if (!SDL_WindowSupportsGPUPresentMode(app->render_system.gpu_device, window->sdl_window, present_mode)) {
                present_mode = SDL_GPU_PRESENTMODE_VSYNC;
            }

            if (!SDL_SetGPUSwapchainParameters(app->render_system.gpu_device, window->sdl_window, SDL_GPU_SWAPCHAINCOMPOSITION_SDR, present_mode)) {
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
        nya_log_warn("SDL_WaitAndAcquireGPUSwapchainTexture() failed for window '%s': %s", window->title, SDL_GetError());
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

    _nya_renderer_ensure_msaa_texture(window, swapchain_width, swapchain_height);
    _nya_renderer_ensure_depth_texture(window, swapchain_width, swapchain_height);

    SDL_GPUTexture* msaa = window->render_system.msaa_texture;

    /*
     * With multisampling the pass draws into the MSAA buffer and resolves onto the swapchain as it
     * ends; without it the swapchain is drawn into directly.
     *
     * RESOLVE_AND_STORE rather than RESOLVE, because the pass is suspended and reopened whenever the
     * batch has to upload — a plain RESOLVE is allowed to discard the multisample contents, and
     * everything drawn before the suspend would be lost.
     */
    SDL_GPUColorTargetInfo target_info = {
        .texture          = msaa != nullptr ? msaa : swapchain_texture,
        .resolve_texture  = msaa != nullptr ? swapchain_texture : nullptr,
        // Opaque black. `(SDL_FColor){ 0 }` looks like "clear to black" but sets a = 0 too, which
        // clears to *transparent*: the compositor then blends the desktop through every pixel the
        // frame does not draw over. It reads as the window rendering in the wrong place, because
        // what you are seeing is whatever sits behind it.
        .clear_color = (SDL_FColor){ .r = 0.0F, .g = 0.0F, .b = 0.0F, .a = 1.0F },
        .load_op          = SDL_GPU_LOADOP_CLEAR,
        /*
         * The opening pass resolves; every pass the batch reopens after it does not, until the last
         * one. See _nya_render2d_pass_resume.
         *
         * Two resolves a frame rather than the ideal one, and that second is deliberate insurance.
         * The final resolve rides on the last flush, and a flush that finds its pipeline still
         * loading returns without opening a pass at all — so during the first frames of a run there
         * may be no last pass to carry it. Resolving here means those frames show their clear colour
         * instead of whatever the swapchain happened to hold.
         */
        .store_op         = msaa != nullptr ? SDL_GPU_STOREOP_RESOLVE_AND_STORE : SDL_GPU_STOREOP_STORE,
    };

    /*
     * Cleared to the far plane every frame, and stored, because the pass is suspended and reopened
     * whenever the batch has to upload — a DONT_CARE store would throw the depth away mid-frame and
     * the geometry drawn after the suspend would not occlude the geometry drawn before it.
     *
     * Attached unconditionally. See NYA_RenderSystemWindow.depth_texture: the attachment is fixed
     * when a pass opens, so a frame cannot decide it wants depth after it has already drawn.
     */
    SDL_GPUDepthStencilTargetInfo depth_info = {
        .texture          = window->render_system.depth_texture,
        .clear_depth      = 1.0F,
        .load_op          = SDL_GPU_LOADOP_CLEAR,
        .store_op         = SDL_GPU_STOREOP_STORE,
        .stencil_load_op  = SDL_GPU_LOADOP_DONT_CARE,
        .stencil_store_op = SDL_GPU_STOREOP_DONT_CARE,
    };

    SDL_GPURenderPass* render_pass = SDL_BeginGPURenderPass(command_buffer, &target_info, 1, &depth_info);
    nya_assert(render_pass != nullptr, "SDL_BeginGPURenderPass() failed: %s", SDL_GetError());

    window->render_system.render_commands   = command_buffer;
    window->render_system.swapchain_texture = swapchain_texture;
    window->render_system.render_pass       = render_pass;

    /*
     * The batch draws into the swapchain until something says otherwise, and its projection comes
     * from whatever is set here — so this is what makes drawing land at window coordinates.
     *
     * Reset every frame rather than once at init: the swapchain texture is a different one each
     * frame, and target_is_texture has to come back to false even if a layer left a render texture
     * open, which would otherwise strand every later frame drawing into it.
     */
    // Counters are per frame, so they reset with it rather than accumulating for the process.
    window->render_system.draw_batch.frame_flushes       = 0;
    window->render_system.draw_batch.frame_vertices      = 0;
    window->render_system.draw_batch.frame_indices       = 0;
    window->render_system.draw_batch.frame_dropped_draws = 0;
    window->render_system.draw_batch.pending_flush_reason = NYA_RENDER2D_FLUSH_FRAME_END;

    // Set once, by nya_render_end, so exactly one pass this frame carries the resolve.
    window->render_system.draw_batch.resolve_pending = false;

    for (u32 i = 0; i < NYA_RENDER2D_FLUSH_REASON_COUNT; i++) window->render_system.draw_batch.frame_flush_reasons[i] = 0;

    /*
     * The 3D batch's counters too, which were never reset.
     *
     * They were incremented on every draw, never cleared and never read — three write-only numbers
     * growing for the life of the window, under a comment saying they were per frame. See
     * nya_render3d_frame_stats, which is what reads them now.
     */
    window->render_system.mesh_batch.frame_draw_calls    = 0;
    window->render_system.mesh_batch.frame_vertices      = 0;
    window->render_system.mesh_batch.frame_indices       = 0;
    window->render_system.mesh_batch.frame_instances     = 0;
    window->render_system.mesh_batch.frame_culled        = 0;
    window->render_system.mesh_batch.frame_occluded      = 0;
    window->render_system.mesh_batch.frame_dropped_draws = 0;

    window->render_system.draw_batch.target_texture    = swapchain_texture;
    window->render_system.draw_batch.target_msaa       = msaa;
    window->render_system.draw_batch.target_depth      = window->render_system.depth_texture;
    window->render_system.draw_batch.target_width      = swapchain_width;
    window->render_system.draw_batch.target_height     = swapchain_height;
    window->render_system.draw_batch.target_is_texture = false;
    window->render_system.draw_batch.shader_override     = nullptr;
    window->render_system.draw_batch.shader_uniform_size = 0;

    return true;
}

void nya_render_end(NYA_Window* window) {
    // Submission and present, which is where a frame waits on the GPU. Separate from frame_rendering
    // so a stall there is not mistaken for the draw work that preceded it.
    nya_perf_time_this_function();

    nya_assert(window != nullptr);

    // nya_render_begin decided there was nothing to draw into. Matching that with a no-op keeps the
    // caller free to run begin/draw/end unconditionally.
    if (window->render_system.render_pass == nullptr) return;

    NYA_Render2DBatch* batch = &window->render_system.draw_batch;

    /*
     * The frame's one multisample resolve, arranged so that the pass carrying it always has a draw
     * in it.
     *
     * Every pass before this one stored its multisample contents without resolving, so nothing has
     * reached the swapchain yet. Setting the flag makes the pass that the flush below reopens the
     * resolving one — and that pass is guaranteed non-empty, because the flush only reopens a pass
     * when it has something to draw.
     *
     * The exception is a frame that queued nothing at all, or whose last flush emptied the batch.
     * One fully transparent pixel gives the flush something to do, which costs a quad and avoids the
     * empty-resolving-pass shape that segfaults the AMD Vulkan driver.
     */
    if (batch->target_msaa != nullptr) {
        batch->resolve_pending = true;

        if (batch->index_count == 0) nya_render2d_rect(window, 0.0F, 0.0F, 1.0F, 1.0F, (NYA_Color){ 0.0F, 0.0F, 0.0F, 0.0F });
    }

    // Whatever the layers queued and did not fill a batch with. Without this a frame that drew
    // fewer than NYA_RENDER2D_MAX_VERTICES worth of shapes would never draw any of them.
    nya_render2d_flush(window);

    SDL_EndGPURenderPass(window->render_system.render_pass);
    window->render_system.render_pass = nullptr;

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

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * SHARED INTERNALS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

// Maybe-unused because its only callers are render2d.c and render3d.c, which a headless build swaps
// out for the stubs — so a headless translation unit compiles this and calls it from nowhere.
__attr_maybe_unused SDL_GPUSampler* _nya_render_sampler_for(NYA_TextureFilter filter) {
    // Clamped rather than asserted: the filter reaches here off an asset's load parameters, which a game
    // fills in, and a value out of range should draw with a sensible sampler rather than end the process.
    if (filter >= NYA_TEXTURE_FILTER_COUNT) filter = NYA_TEXTURE_FILTER_LINEAR;

    return nya_app_get()->render_system.samplers[filter];
}
