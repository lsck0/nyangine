/**
 * @file render_compute.h
 *
 * A thin, reusable skin over SDL_GPU's compute pipeline API: build a pipeline from a compiled `.comp`
 * shader, then dispatch it with its storage buffers, storage textures and samplers bound in one call.
 * The graphics side of the renderer draws from compiled vertex and fragment shaders; this is the same
 * idea for the compute stage, and nothing more — it holds no state and owns no resources beyond the
 * pipeline object the caller asked for.
 *
 * ```c
 * SDL_GPUComputePipeline* blur = nya_gpu_compute_pipeline_create(device, (NYA_GPUComputePipelineDesc){
 *     .shader                        = NYA_ASSET_SHADER_MY_BLUR_COMP,
 *     .threadcount_x = 8, .threadcount_y = 8, .threadcount_z = 1,   // must match [numthreads]
 *     .num_samplers                  = 1,
 *     .num_readwrite_storage_textures = 1,
 *     .num_uniform_buffers           = 1,
 * });
 *
 * // once a frame, on a command buffer with no pass open:
 * nya_gpu_compute_dispatch(commands, &(NYA_GPUComputeDispatch){
 *     .pipeline                = blur,
 *     .readwrite_textures      = &(SDL_GPUStorageTextureReadWriteBinding){ .texture = out },
 *     .readwrite_texture_count = 1,
 *     .samplers                = &(SDL_GPUTextureSamplerBinding){ .texture = in, .sampler = linear },
 *     .sampler_count           = 1,
 *     .uniform = &params, .uniform_size = sizeof(params),
 *     .groupcount_x = (w + 7) / 8, .groupcount_y = (h + 7) / 8, .groupcount_z = 1,
 * });
 * ```
 *
 * DESKTOP ONLY. Compute shaders do not cross-compile to WebGL2/GLES3 — that target has no compute stage
 * — so the whole header is compiled out on the web build, and its one caller (render_compute_particles.c)
 * is gated the same way. See the renderer-web wall: the renderer is SDL_GPU throughout, and the web path
 * is a later GLES3 shim that will never carry compute. The build's shader step skips the GLSL variant of
 * a `.comp` shader for the same reason; see src/nyangine-build/pp/asset.c.
 * */
#pragma once

#include "SDL3/SDL_gpu.h"

#include "nyangine-std/base/base_attributes.h"
#include "nyangine-std/base/base_basic.h"
#include "nyangine-std/base/base_types.h"

// The web build has no compute stage to skin over. Everything below is desktop only.
#if !OS_WASM

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * TYPES
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/**
 * How to build a compute pipeline: the shader, its workgroup size, and how many of each resource it
 * declares. The counts must match the shader's bindings exactly, and the thread counts its `[numthreads]`,
 * or SDL refuses the pipeline.
 * */
typedef struct NYA_GPUComputePipelineDesc {
    /** The source `.comp.hlsl` asset handle, e.g. NYA_ASSET_SHADER_PARTICLE_UPDATE_COMP. The compiled
     *  artifact for the device's format (SPIR-V, DXIL or MSL) is picked and loaded by the create call. */
    NYA_ConstCString shader;

    /** The workgroup size, matching the shader's `[numthreads(x, y, z)]`. Zeros are read as one. */
    u32 threadcount_x;
    u32 threadcount_y;
    u32 threadcount_z;

    /* How many of each resource the shader binds. See SDL_CreateGPUComputePipeline for the binding order. */
    u32 num_samplers;
    u32 num_readonly_storage_textures;
    u32 num_readonly_storage_buffers;
    u32 num_readwrite_storage_textures;
    u32 num_readwrite_storage_buffers;
    u32 num_uniform_buffers;
} NYA_GPUComputePipelineDesc;

/**
 * One compute dispatch: the pipeline, its resource bindings, an optional uniform, and the workgroup count.
 * The read-write bindings open the pass (a compute pass is defined by what it may write); the read-only
 * ones and the samplers are bound onto it before the dispatch.
 * */
typedef struct NYA_GPUComputeDispatch {
    SDL_GPUComputePipeline* pipeline;

    /* Written by the shader. Their textures and buffers need the COMPUTE_STORAGE_WRITE usage. */
    const SDL_GPUStorageTextureReadWriteBinding* readwrite_textures;
    u32                                          readwrite_texture_count;
    const SDL_GPUStorageBufferReadWriteBinding*  readwrite_buffers;
    u32                                          readwrite_buffer_count;

    /* Read by the shader. Storage textures and buffers need the COMPUTE_STORAGE_READ usage. */
    const SDL_GPUTextureSamplerBinding* samplers;
    u32                                 sampler_count;
    SDL_GPUTexture* const*              readonly_textures;
    u32                                 readonly_texture_count;
    SDL_GPUBuffer* const*               readonly_buffers;
    u32                                 readonly_buffer_count;

    /** Pushed at compute uniform slot 0, or null for a shader that takes no uniform. */
    const void* uniform;
    u32         uniform_size;

    /** Workgroups to launch. The shader's `[numthreads]` says how big each one is. */
    u32 groupcount_x;
    u32 groupcount_y;
    u32 groupcount_z;
} NYA_GPUComputeDispatch;

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * FUNCTIONS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/**
 * Whether `device` can run the compute work this module builds. Probes for compute storage-write support,
 * which the offscreen and software backends do not all have. A caller uses it to skip a compute effect and
 * carry on rather than fail; nya_gpu_compute_pipeline_create also returns null on a device that cannot,
 * so a caller that checks the create result need not call this first.
 * */
NYA_API b8 nya_gpu_compute_supported(SDL_GPUDevice* device) __attr_no_discard;

/**
 * Builds a compute pipeline from `desc`. Loads the compiled `.comp` artifact for the device's shader
 * format and hands it to SDL_CreateGPUComputePipeline. Returns null and logs when the shader is missing,
 * the device rejects it, or the device has no compute — the caller then runs without the effect. Release
 * it with nya_gpu_compute_pipeline_release.
 * */
NYA_API SDL_GPUComputePipeline* nya_gpu_compute_pipeline_create(SDL_GPUDevice* device, NYA_GPUComputePipelineDesc desc) __attr_no_discard;

/** SDL_ReleaseGPUComputePipeline. Null is ignored, as SDL does. */
NYA_API void nya_gpu_compute_pipeline_release(SDL_GPUDevice* device, SDL_GPUComputePipeline* pipeline);

/**
 * Runs one dispatch: begins a compute pass with the read-write bindings, binds the pipeline, the read-only
 * storage resources and the samplers, pushes the uniform, dispatches, and ends the pass. The caller owns
 * `commands`, which must have no render, compute or copy pass already open, and submits it afterwards.
 * */
NYA_API void nya_gpu_compute_dispatch(SDL_GPUCommandBuffer* commands, const NYA_GPUComputeDispatch* dispatch);

#endif // !OS_WASM
