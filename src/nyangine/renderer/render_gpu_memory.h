/**
 * @file render_gpu_memory.h
 *
 * ```c
 * // in place of SDL_CreateGPUTexture and SDL_ReleaseGPUTexture, so the bytes are counted:
 * SDL_GPUTexture* texture = nya_gpu_texture_create(gpu_device, &info);
 * nya_gpu_texture_release(gpu_device, texture);
 *
 * printf("textures: %llu bytes\n", nya_gpu_memory_bytes(NYA_GPU_MEMORY_TEXTURE));
 * ```
 *
 * Counts what the engine asks SDL for, not what the driver reserves: SDL rounds allocations up into
 * larger pages, and a transfer buffer mapped with cycling can hold more than one copy.
 * */
#pragma once

#include "SDL3/SDL_gpu.h"

#include "nyangine/base/base_attributes.h"
#include "nyangine/base/base_types.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * CONSTANTS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/**
 * GPU objects counted at once. The 3D demo holds 32 and every loaded texture or mesh adds one or two, so
 * a few hundred assets fit. Past it an object still works but goes uncounted, which the gpu_allocations
 * ceiling shows. The table is twice this at 24 bytes a slot. A power of two, for the masked hash.
 * */
#ifndef NYA_GPU_MEMORY_TRACKED_MAX
#define NYA_GPU_MEMORY_TRACKED_MAX 1024
#endif

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * TYPES
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

typedef enum {
    /** Asset textures, glyph atlases, render targets with their MSAA and depth buffers, shadow maps. */
    NYA_GPU_MEMORY_TEXTURE,

    /** Vertex, index and instance buffers. */
    NYA_GPU_MEMORY_BUFFER,

    /** Upload staging. Host visible, so it is counted apart from what shaders read. */
    NYA_GPU_MEMORY_TRANSFER,

    NYA_GPU_MEMORY_KIND_COUNT,
} NYA_GPUMemoryKind;

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * FUNCTIONS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/** SDL_CreateGPUTexture, counted. Null when SDL fails, with nothing counted. */
NYA_API SDL_GPUTexture* nya_gpu_texture_create(SDL_GPUDevice* device, const SDL_GPUTextureCreateInfo* info) __attr_no_discard;

/** SDL_ReleaseGPUTexture, uncounted. Null is ignored, as SDL does. */
NYA_API void nya_gpu_texture_release(SDL_GPUDevice* device, SDL_GPUTexture* texture);

NYA_API SDL_GPUBuffer* nya_gpu_buffer_create(SDL_GPUDevice* device, const SDL_GPUBufferCreateInfo* info) __attr_no_discard;
NYA_API void           nya_gpu_buffer_release(SDL_GPUDevice* device, SDL_GPUBuffer* buffer);

NYA_API SDL_GPUTransferBuffer* nya_gpu_transfer_buffer_create(SDL_GPUDevice* device, const SDL_GPUTransferBufferCreateInfo* info) __attr_no_discard;
NYA_API void                   nya_gpu_transfer_buffer_release(SDL_GPUDevice* device, SDL_GPUTransferBuffer* transfer_buffer);

/** Bytes live in one kind. */
NYA_API u64 nya_gpu_memory_bytes(NYA_GPUMemoryKind kind) __attr_no_discard;

/** What a texture created from `info` holds: every mip level, layer and sample. */
NYA_API u64 nya_gpu_texture_bytes(const SDL_GPUTextureCreateInfo* info) __attr_no_discard;
