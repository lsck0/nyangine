/**
 * @file render_compute_particles.c
 * */
#include "nyangine/nyangine.h"

#if !OS_WASM

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * CONSTANTS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/** The draw is a per-texel gather over every particle, so both are kept modest. See particle_render.comp.hlsl. */
#define NYA_GPU_PARTICLE_FIELD_MAX_COUNT      4096
#define NYA_GPU_PARTICLE_FIELD_MAX_RESOLUTION 512

/** Workgroup sizes, matching the `[numthreads]` in the two shaders. */
#define NYA_GPU_PARTICLE_UPDATE_GROUP 64
#define NYA_GPU_PARTICLE_RENDER_GROUP 8

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * TYPES
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/** One particle, sixteen bytes, laid out exactly as the shaders' `struct Particle`. */
typedef struct {
    f32 position[2];
    f32 velocity[2];
} _NYA_GPUParticle;

/** The uniform particle_update.comp.hlsl reads. Four scalars, one constant-buffer register. */
typedef struct {
    u32 count;
    f32 dt;
    f32 time;
    u32 seed;
} _NYA_GPUParticleUpdateUniform;

/** The uniform particle_render.comp.hlsl reads. */
typedef struct {
    u32 count;
    u32 width;
    u32 height;
    f32 radius;
} _NYA_GPUParticleRenderUniform;

struct NYA_GPUParticleField {
    /** Owns everything below; freed whole by destroy. */
    NYA_Arena* arena;

    u32 count;
    u32 resolution;
    f32 time_s;

    /** Positions and velocities, read-write in the update pass and read-only in the render pass. */
    SDL_GPUBuffer* particles;

    /** The field image: written by the render pass, sampled by the 2D draw. */
    SDL_GPUTexture* image;

    /** A view of `image` for the 2D textured path, which only reads the texture, size and sample count. */
    NYA_RenderTexture texture;

    SDL_GPUComputePipeline* update;
    SDL_GPUComputePipeline* render;
};

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API DECLARATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/** Integrates (or, with seed set, initialises) the particle buffer on `commands`. */
NYA_INTERNAL void _nya_gpu_particle_field_update(NYA_GPUParticleField* field, SDL_GPUCommandBuffer* commands, f32 delta_time_s, b8 seed);

/** Gathers the particles into the field image on `commands`. */
NYA_INTERNAL void _nya_gpu_particle_field_render(NYA_GPUParticleField* field, SDL_GPUCommandBuffer* commands);

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PUBLIC API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

NYA_GPUParticleField* nya_gpu_particle_field_create(NYA_Window* window, u32 count, u32 resolution) {
    nya_assert(window != nullptr);

    SDL_GPUDevice* device = nya_app_get()->render_system.gpu_device;
    nya_assert(device != nullptr);

    if (!nya_gpu_compute_supported(device)) {
        nya_log_warn("GPU particle field: the device has no compute; the field is disabled.");
        return nullptr;
    }

    count      = nya_clamp(count, 1U, (u32)NYA_GPU_PARTICLE_FIELD_MAX_COUNT);
    resolution = nya_clamp(resolution, 1U, (u32)NYA_GPU_PARTICLE_FIELD_MAX_RESOLUTION);

    NYA_Arena*            arena = nya_arena_create(.name = "gpu_particle_field");
    NYA_GPUParticleField* field = nya_arena_alloc(arena, sizeof(NYA_GPUParticleField));
    *field                      = (NYA_GPUParticleField){ .arena = arena, .count = count, .resolution = resolution };

    // The buffer is written by the update pass and read by the render pass, so it carries both usages.
    field->particles = nya_gpu_buffer_create(device, &(SDL_GPUBufferCreateInfo){
                                                          .usage = SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_READ | SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_WRITE,
                                                          .size  = count * (u32)sizeof(_NYA_GPUParticle),
                                                      });

    // Written by the render pass (COMPUTE_STORAGE_WRITE), then sampled by the 2D draw (SAMPLER).
    field->image = nya_gpu_texture_create(device, &(SDL_GPUTextureCreateInfo){
                                                       .type                 = SDL_GPU_TEXTURETYPE_2D,
                                                       .format               = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM,
                                                       .usage                = SDL_GPU_TEXTUREUSAGE_SAMPLER | SDL_GPU_TEXTUREUSAGE_COMPUTE_STORAGE_WRITE,
                                                       .width                = resolution,
                                                       .height               = resolution,
                                                       .layer_count_or_depth = 1,
                                                       .num_levels           = 1,
                                                   });

    field->update = nya_gpu_compute_pipeline_create(device, (NYA_GPUComputePipelineDesc){
                                                                 .shader                        = NYA_ASSET_SHADER_PARTICLE_UPDATE_COMP,
                                                                 .threadcount_x                 = NYA_GPU_PARTICLE_UPDATE_GROUP,
                                                                 .threadcount_y                 = 1,
                                                                 .threadcount_z                 = 1,
                                                                 .num_readwrite_storage_buffers = 1,
                                                                 .num_uniform_buffers           = 1,
                                                             });

    field->render = nya_gpu_compute_pipeline_create(device, (NYA_GPUComputePipelineDesc){
                                                                 .shader                         = NYA_ASSET_SHADER_PARTICLE_RENDER_COMP,
                                                                 .threadcount_x                  = NYA_GPU_PARTICLE_RENDER_GROUP,
                                                                 .threadcount_y                  = NYA_GPU_PARTICLE_RENDER_GROUP,
                                                                 .threadcount_z                  = 1,
                                                                 .num_readonly_storage_buffers   = 1,
                                                                 .num_readwrite_storage_textures = 1,
                                                                 .num_uniform_buffers            = 1,
                                                             });

    if (field->particles == nullptr || field->image == nullptr || field->update == nullptr || field->render == nullptr) {
        nya_log_error("GPU particle field: a resource or pipeline would not build; the field is disabled.");
        nya_gpu_particle_field_destroy(window, field);
        return nullptr;
    }

    // A view of the image for the 2D textured path. Single sampled, its own size.
    field->texture = (NYA_RenderTexture){
        .texture        = field->image,
        .width          = resolution,
        .height         = resolution,
        .sample_count   = SDL_GPU_SAMPLECOUNT_1,
        .options        = { .single_sampled = true, .format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM },
    };

    // Seed the particles on the GPU once, on its own command buffer, ordered before the first step's integration.
    SDL_GPUCommandBuffer* seed_commands = SDL_AcquireGPUCommandBuffer(device);
    nya_assert(seed_commands != nullptr, "SDL_AcquireGPUCommandBuffer() failed: %s", SDL_GetError());
    _nya_gpu_particle_field_update(field, seed_commands, 0.0F, true);
    (void)SDL_SubmitGPUCommandBuffer(seed_commands);

    nya_log_info("GPU particle field: %u particles into a %ux%u image, on %s.", count, resolution, resolution,
                 SDL_GetGPUDeviceDriver(device));

    return field;
}

void nya_gpu_particle_field_step(NYA_Window* window, NYA_GPUParticleField* field, f32 delta_time_s) {
    nya_unused(window);
    if (field == nullptr) return;

    field->time_s += delta_time_s;

    SDL_GPUDevice*        device   = nya_app_get()->render_system.gpu_device;
    SDL_GPUCommandBuffer* commands = SDL_AcquireGPUCommandBuffer(device);
    nya_assert(commands != nullptr, "SDL_AcquireGPUCommandBuffer() failed: %s", SDL_GetError());

    // Two passes on one command buffer: integrate then gather; SDL's barrier between them lets render see the integrated positions.
    _nya_gpu_particle_field_update(field, commands, delta_time_s, false);
    _nya_gpu_particle_field_render(field, commands);

    (void)SDL_SubmitGPUCommandBuffer(commands);
}

void nya_gpu_particle_field_draw(NYA_Window* window, NYA_GPUParticleField* field, f32 x, f32 y, f32 width, f32 height) {
    if (field == nullptr) return;

    nya_render2d_render_texture(window, &field->texture, x, y, width, height, NYA_COLOR_WHITE);
}

void nya_gpu_particle_field_destroy(NYA_Window* window, NYA_GPUParticleField* field) {
    nya_unused(window);
    if (field == nullptr) return;

    SDL_GPUDevice* device = nya_app_get()->render_system.gpu_device;

    // The frames that used these may still be in flight; wait so the release is safe.
    SDL_WaitForGPUIdle(device);

    nya_gpu_compute_pipeline_release(device, field->update);
    nya_gpu_compute_pipeline_release(device, field->render);
    nya_gpu_texture_release(device, field->image);
    nya_gpu_buffer_release(device, field->particles);

    // Frees the field struct itself, allocated from this arena.
    nya_arena_destroy(field->arena);
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

void _nya_gpu_particle_field_update(NYA_GPUParticleField* field, SDL_GPUCommandBuffer* commands, f32 delta_time_s, b8 seed) {
    _NYA_GPUParticleUpdateUniform uniform = {
        .count = field->count,
        .dt    = delta_time_s,
        .time  = field->time_s,
        .seed  = seed ? 1U : 0U,
    };

    nya_gpu_compute_dispatch(commands, &(NYA_GPUComputeDispatch){
                                           .pipeline               = field->update,
                                           .readwrite_buffers      = &(SDL_GPUStorageBufferReadWriteBinding){ .buffer = field->particles },
                                           .readwrite_buffer_count = 1,
                                           .uniform                = &uniform,
                                           .uniform_size           = sizeof(uniform),
                                           .groupcount_x           = (field->count + NYA_GPU_PARTICLE_UPDATE_GROUP - 1) / NYA_GPU_PARTICLE_UPDATE_GROUP,
                                           .groupcount_y           = 1,
                                           .groupcount_z           = 1,
                                       });
}

void _nya_gpu_particle_field_render(NYA_GPUParticleField* field, SDL_GPUCommandBuffer* commands) {
    _NYA_GPUParticleRenderUniform uniform = {
        .count  = field->count,
        .width  = field->resolution,
        .height = field->resolution,
        // A splat a few texels wide at this resolution, so the dots read without smearing into one blob.
        .radius = 0.02F,
    };

    u32 groups = (field->resolution + NYA_GPU_PARTICLE_RENDER_GROUP - 1) / NYA_GPU_PARTICLE_RENDER_GROUP;

    nya_gpu_compute_dispatch(commands, &(NYA_GPUComputeDispatch){
                                           .pipeline                = field->render,
                                           .readwrite_textures      = &(SDL_GPUStorageTextureReadWriteBinding){ .texture = field->image },
                                           .readwrite_texture_count = 1,
                                           .readonly_buffers        = &field->particles,
                                           .readonly_buffer_count   = 1,
                                           .uniform                 = &uniform,
                                           .uniform_size            = sizeof(uniform),
                                           .groupcount_x            = groups,
                                           .groupcount_y            = groups,
                                           .groupcount_z            = 1,
                                       });
}

#endif // !OS_WASM
