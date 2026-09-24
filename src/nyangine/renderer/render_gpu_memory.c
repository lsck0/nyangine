/**
 * @file render_gpu_memory.c
 * */
#include "nyangine/nyangine.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * TYPES
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/** Twice the tracked maximum, so linear probing stays short when the ceiling is nearly full. */
#define NYA_GPU_MEMORY_SLOTS (NYA_GPU_MEMORY_TRACKED_MAX * 2)

typedef struct {
    /** Null means the slot is free. Only compared, never dereferenced. */
    const void* handle;

    u64               bytes;
    NYA_GPUMemoryKind kind;

    /** The trace feature open when it was created. Fits in the padding, so a slot stays 24 bytes. */
    NYA_TraceFeature feature;
} _NYA_GPUAllocation;

typedef struct {
    /** Keyed by handle, since a release only has the pointer to go on. */
    _NYA_GPUAllocation slots[NYA_GPU_MEMORY_SLOTS];

    /** u32 for the ceiling registry. */
    u32 count;

    u64 bytes[NYA_GPU_MEMORY_KIND_COUNT];

    u64 feature_bytes[NYA_TRACE_FEATURE_MAX];

    /** A full table warns once, since every later create would repeat it. */
    b8 full_warned;
} _NYA_GPUMemory;

/* No init: a zeroed table is empty. */
NYA_INTERNAL _NYA_GPUMemory _nya_gpu_memory = { 0 };

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API DECLARATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/** The slot a handle hashes to. */
NYA_INTERNAL u32 _nya_gpu_memory_home(const void* handle) __attr_no_discard;

/** Counts `bytes` of `kind` against `handle`. A handle counted twice keeps its first entry. */
NYA_INTERNAL void _nya_gpu_memory_track(NYA_GPUMemoryKind kind, const void* handle, u64 bytes);

/** Uncounts whatever `handle` was counted as. An uncounted handle is ignored. */
NYA_INTERNAL void _nya_gpu_memory_untrack(const void* handle);

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PUBLIC API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

SDL_GPUTexture* nya_gpu_texture_create(SDL_GPUDevice* device, const SDL_GPUTextureCreateInfo* info) {
    nya_assert(device != nullptr);
    nya_assert(info != nullptr);

    SDL_GPUTexture* texture = SDL_CreateGPUTexture(device, info);
    if (texture != nullptr) _nya_gpu_memory_track(NYA_GPU_MEMORY_TEXTURE, texture, nya_gpu_texture_bytes(info));

    return texture;
}

void nya_gpu_texture_release(SDL_GPUDevice* device, SDL_GPUTexture* texture) {
    if (texture == nullptr) return;

    _nya_gpu_memory_untrack(texture);
    SDL_ReleaseGPUTexture(device, texture);
}

SDL_GPUBuffer* nya_gpu_buffer_create(SDL_GPUDevice* device, const SDL_GPUBufferCreateInfo* info) {
    nya_assert(device != nullptr);
    nya_assert(info != nullptr);

    SDL_GPUBuffer* buffer = SDL_CreateGPUBuffer(device, info);
    if (buffer != nullptr) _nya_gpu_memory_track(NYA_GPU_MEMORY_BUFFER, buffer, info->size);

    return buffer;
}

void nya_gpu_buffer_release(SDL_GPUDevice* device, SDL_GPUBuffer* buffer) {
    if (buffer == nullptr) return;

    _nya_gpu_memory_untrack(buffer);
    SDL_ReleaseGPUBuffer(device, buffer);
}

SDL_GPUTransferBuffer* nya_gpu_transfer_buffer_create(SDL_GPUDevice* device, const SDL_GPUTransferBufferCreateInfo* info) {
    nya_assert(device != nullptr);
    nya_assert(info != nullptr);

    SDL_GPUTransferBuffer* transfer_buffer = SDL_CreateGPUTransferBuffer(device, info);
    if (transfer_buffer != nullptr) _nya_gpu_memory_track(NYA_GPU_MEMORY_TRANSFER, transfer_buffer, info->size);

    return transfer_buffer;
}

void nya_gpu_transfer_buffer_release(SDL_GPUDevice* device, SDL_GPUTransferBuffer* transfer_buffer) {
    if (transfer_buffer == nullptr) return;

    _nya_gpu_memory_untrack(transfer_buffer);
    SDL_ReleaseGPUTransferBuffer(device, transfer_buffer);
}

u64 nya_gpu_memory_bytes(NYA_GPUMemoryKind kind) {
    nya_assert(kind < NYA_GPU_MEMORY_KIND_COUNT);

    return _nya_gpu_memory.bytes[kind];
}

u64 nya_gpu_memory_feature_bytes(NYA_TraceFeature feature) {
    nya_assert(feature < NYA_TRACE_FEATURE_MAX);

    return _nya_gpu_memory.feature_bytes[feature];
}

u64 nya_gpu_texture_bytes(const SDL_GPUTextureCreateInfo* info) {
    nya_assert(info != nullptr);

    u64 bytes  = 0;
    u32 levels = nya_max(info->num_levels, 1U);

    for (u32 level = 0; level < levels; level++) {
        u32 width  = nya_max(info->width >> level, 1U);
        u32 height = nya_max(info->height >> level, 1U);

        // a 3D texture's depth halves with each mip; array layers and cube faces do not.
        u32 depth = info->layer_count_or_depth;
        if (info->type == SDL_GPU_TEXTURETYPE_3D) depth = depth >> level;

        bytes += SDL_CalculateGPUTextureFormatSize(info->format, width, height, nya_max(depth, 1U));
    }

    // every sample is stored. the enum counts powers of two from one sample.
    return bytes << (u32)info->sample_count;
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/* The attribute is required: this multiply overflows on purpose, and the sanitized build aborts without it. */
__attr_no_sanitize("unsigned-integer-overflow") u32 _nya_gpu_memory_home(const void* handle) {
    // allocations are aligned, so the low bits carry nothing and the high bits of the product are used.
    u64 hash = (u64)(uintptr_t)handle * 0x9E3779B97F4A7C15ULL;

    return (u32)(hash >> 32) & (NYA_GPU_MEMORY_SLOTS - 1);
}

void _nya_gpu_memory_track(NYA_GPUMemoryKind kind, const void* handle, u64 bytes) {
    nya_assert(kind < NYA_GPU_MEMORY_KIND_COUNT);
    nya_assert(handle != nullptr);

    // registered once, with the first object, so a build that never touches the GPU shows no rows.
    static b8 registered = false;
    if (!registered) {
        nya_gauge_register("gpu_textures", &_nya_gpu_memory.bytes[NYA_GPU_MEMORY_TEXTURE]);
        nya_gauge_register("gpu_buffers", &_nya_gpu_memory.bytes[NYA_GPU_MEMORY_BUFFER]);
        nya_gauge_register("gpu_transfer", &_nya_gpu_memory.bytes[NYA_GPU_MEMORY_TRANSFER]);
        nya_ceiling_register("gpu_allocations", NYA_GPU_MEMORY_TRACKED_MAX, &_nya_gpu_memory.count);
        registered = true;
    }

    if (_nya_gpu_memory.count >= NYA_GPU_MEMORY_TRACKED_MAX) {
        if (!_nya_gpu_memory.full_warned) {
            nya_log_warn("GPU memory tracking is full at %d objects; later ones go uncounted. Raise NYA_GPU_MEMORY_TRACKED_MAX.",
                         NYA_GPU_MEMORY_TRACKED_MAX);
            _nya_gpu_memory.full_warned = true;
        }

        return;
    }

    // the table is at most half full, so a free slot is always reached.
    u32 slot = _nya_gpu_memory_home(handle);
    while (_nya_gpu_memory.slots[slot].handle != nullptr) {
        if (_nya_gpu_memory.slots[slot].handle == handle) return;

        slot = (slot + 1) & (NYA_GPU_MEMORY_SLOTS - 1);
    }

    NYA_TraceFeature feature = nya_trace_feature_current();

    _nya_gpu_memory.slots[slot] = (_NYA_GPUAllocation){ .handle = handle, .bytes = bytes, .kind = kind, .feature = feature };
    _nya_gpu_memory.count++;
    _nya_gpu_memory.bytes[kind]             += bytes;
    _nya_gpu_memory.feature_bytes[feature] += bytes;
}

void _nya_gpu_memory_untrack(const void* handle) {
    nya_assert(handle != nullptr);

    u32 mask = NYA_GPU_MEMORY_SLOTS - 1;
    u32 hole = _nya_gpu_memory_home(handle);

    while (_nya_gpu_memory.slots[hole].handle != handle) {
        // not counted: created while the table was full.
        if (_nya_gpu_memory.slots[hole].handle == nullptr) return;

        hole = (hole + 1) & mask;
    }

    const _NYA_GPUAllocation* found = &_nya_gpu_memory.slots[hole];
    nya_assert(_nya_gpu_memory.bytes[found->kind] >= found->bytes, "GPU memory count went negative");

    _nya_gpu_memory.bytes[found->kind]             -= found->bytes;
    _nya_gpu_memory.feature_bytes[found->feature] -= found->bytes;
    _nya_gpu_memory.count--;

    // Backward-shift deletion, not tombstones, so churn never fills the table with dead slots.
    for (u32 next = (hole + 1) & mask; _nya_gpu_memory.slots[next].handle != nullptr; next = (next + 1) & mask) {
        u32 home = _nya_gpu_memory_home(_nya_gpu_memory.slots[next].handle);

        b8 stays = hole <= next ? (hole < home && home <= next) : (hole < home || home <= next);
        if (stays) continue;

        _nya_gpu_memory.slots[hole] = _nya_gpu_memory.slots[next];
        hole                        = next;
    }

    _nya_gpu_memory.slots[hole] = (_NYA_GPUAllocation){ 0 };
}
