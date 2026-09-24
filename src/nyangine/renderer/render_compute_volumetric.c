/**
 * @file render_compute_volumetric.c
 * */
#include "nyangine/nyangine.h"

// The parameter accessors live beside the other options in render_post.c and compile everywhere; this file is the GPU pass that reads them, desktop only.

#if !OS_WASM

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * CONSTANTS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/** The cost is per texel times the march, so the image is kept modest, like the particle field's. */
#define NYA_GPU_VOLUMETRIC_MAX_RESOLUTION 512

/** The workgroup size, matching the `[numthreads]` in volumetric.comp.hlsl. */
#define NYA_GPU_VOLUMETRIC_GROUP 8

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * TYPES
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/**
 * The uniform volumetric.comp.hlsl reads, laid out in 16-byte groups so the bytes match its `cbuffer Params`
 * exactly: a float4 for the light direction, then the scalars, then the padding the cbuffer rounds up to.
 * */
typedef struct {
    f32 light_direction[4];
    f32 density;
    f32 absorption;
    u32 steps;
    f32 time;
    u32 width;
    u32 height;
    u32 pad[2];
} _NYA_GPUVolumetricUniform;

static_assert(sizeof(_NYA_GPUVolumetricUniform) == 48, "the uniform matches volumetric.comp.hlsl's cbuffer layout");

struct NYA_GPUVolumetric {
    /** Owns everything below; freed whole by destroy. */
    NYA_Arena* arena;

    u32 resolution;
    f32 time_s;

    /** The volume image: written by the march, sampled by the 2D composite. */
    SDL_GPUTexture* image;

    /** A view of `image` for the 2D textured path, which only reads the texture, size and sample count. */
    NYA_RenderTexture texture;

    SDL_GPUComputePipeline* march;
};

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PUBLIC API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

NYA_GPUVolumetric* nya_gpu_volumetric_create(NYA_Window* window, u32 resolution) {
    nya_assert(window != nullptr);

    SDL_GPUDevice* device = nya_app_get()->render_system.gpu_device;
    nya_assert(device != nullptr);

    if (!nya_gpu_compute_supported(device)) {
        nya_log_warn("GPU volumetric: the device has no compute; the pass is disabled.");
        return nullptr;
    }

    resolution = nya_clamp(resolution, 1U, (u32)NYA_GPU_VOLUMETRIC_MAX_RESOLUTION);

    NYA_Arena*         arena  = nya_arena_create(.name = "gpu_volumetric");
    NYA_GPUVolumetric* volume = nya_arena_alloc(arena, sizeof(NYA_GPUVolumetric));
    *volume                   = (NYA_GPUVolumetric){ .arena = arena, .resolution = resolution };

    // Written by the march (COMPUTE_STORAGE_WRITE), then sampled by the 2D composite (SAMPLER).
    volume->image = nya_gpu_texture_create(device, &(SDL_GPUTextureCreateInfo){
                                                       .type                 = SDL_GPU_TEXTURETYPE_2D,
                                                       .format               = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM,
                                                       .usage                = SDL_GPU_TEXTUREUSAGE_SAMPLER | SDL_GPU_TEXTUREUSAGE_COMPUTE_STORAGE_WRITE,
                                                       .width                = resolution,
                                                       .height               = resolution,
                                                       .layer_count_or_depth = 1,
                                                       .num_levels           = 1,
                                                   });

    volume->march = nya_gpu_compute_pipeline_create(device, (NYA_GPUComputePipelineDesc){
                                                                .shader                         = NYA_ASSET_SHADER_VOLUMETRIC_COMP,
                                                                .threadcount_x                  = NYA_GPU_VOLUMETRIC_GROUP,
                                                                .threadcount_y                  = NYA_GPU_VOLUMETRIC_GROUP,
                                                                .threadcount_z                  = 1,
                                                                .num_readwrite_storage_textures = 1,
                                                                .num_uniform_buffers            = 1,
                                                            });

    if (volume->image == nullptr || volume->march == nullptr) {
        nya_log_error("GPU volumetric: a resource or pipeline would not build; the pass is disabled.");
        nya_gpu_volumetric_destroy(window, volume);
        return nullptr;
    }

    // A view of the image for the 2D textured path. Single sampled, its own size.
    volume->texture = (NYA_RenderTexture){
        .texture      = volume->image,
        .width        = resolution,
        .height       = resolution,
        .sample_count = SDL_GPU_SAMPLECOUNT_1,
        .options      = { .single_sampled = true, .format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM },
    };

    nya_log_info("GPU volumetric: a %ux%u volume, on %s.", resolution, resolution, SDL_GetGPUDeviceDriver(device));

    return volume;
}

void nya_gpu_volumetric_begin(NYA_Window* window, NYA_GPUVolumetric* volume, f32 delta_time_s) {
    if (volume == nullptr) return;

    NYA_VolumetricParams params = nya_volumetric_params(window);

    // off wins over on: the feature switch and the parameters' own switch both gate the march, the SSR pattern.
    if (!nya_render_feature_on(window, NYA_RENDER_FEATURE_VOLUMETRICS, params.enabled)) return;

    volume->time_s += delta_time_s;

    _NYA_GPUVolumetricUniform uniform = {
        .light_direction = { params.light_direction[0], params.light_direction[1], params.light_direction[2], 0.0F },
        .density         = params.density > 0.0F ? params.density : NYA_VOLUMETRIC_DENSITY,
        .absorption      = params.absorption > 0.0F ? params.absorption : NYA_VOLUMETRIC_ABSORPTION,
        .steps           = params.steps > 0 ? params.steps : (u32)NYA_VOLUMETRIC_STEPS,
        .time            = volume->time_s,
        .width           = volume->resolution,
        .height          = volume->resolution,
    };

    SDL_GPUDevice*        device   = nya_app_get()->render_system.gpu_device;
    SDL_GPUCommandBuffer* commands = SDL_AcquireGPUCommandBuffer(device);
    nya_assert(commands != nullptr, "SDL_AcquireGPUCommandBuffer() failed: %s", SDL_GetError());

    u32 groups = (volume->resolution + NYA_GPU_VOLUMETRIC_GROUP - 1) / NYA_GPU_VOLUMETRIC_GROUP;

    nya_gpu_compute_dispatch(commands, &(NYA_GPUComputeDispatch){
                                           .pipeline                = volume->march,
                                           .readwrite_textures      = &(SDL_GPUStorageTextureReadWriteBinding){ .texture = volume->image },
                                           .readwrite_texture_count = 1,
                                           .uniform                 = &uniform,
                                           .uniform_size            = sizeof(uniform),
                                           .groupcount_x            = groups,
                                           .groupcount_y            = groups,
                                           .groupcount_z            = 1,
                                       });

    (void)SDL_SubmitGPUCommandBuffer(commands);
}

void nya_gpu_volumetric_end(NYA_Window* window, NYA_GPUVolumetric* volume, f32 x, f32 y, f32 width, f32 height) {
    if (volume == nullptr) return;

    // the same gate the march answers to: with the pass off, the image holds a stale frame, so it is not drawn.
    NYA_VolumetricParams params = nya_volumetric_params(window);
    if (!nya_render_feature_on(window, NYA_RENDER_FEATURE_VOLUMETRICS, params.enabled)) return;

    nya_render2d_render_texture(window, &volume->texture, x, y, width, height, NYA_COLOR_WHITE);
}

void nya_gpu_volumetric_destroy(NYA_Window* window, NYA_GPUVolumetric* volume) {
    nya_unused(window);
    if (volume == nullptr) return;

    SDL_GPUDevice* device = nya_app_get()->render_system.gpu_device;

    // The frames that used these may still be in flight; wait so the release is safe.
    SDL_WaitForGPUIdle(device);

    nya_gpu_compute_pipeline_release(device, volume->march);
    nya_gpu_texture_release(device, volume->image);

    // Frees the volume struct itself, allocated from this arena.
    nya_arena_destroy(volume->arena);
}

#endif // !OS_WASM
