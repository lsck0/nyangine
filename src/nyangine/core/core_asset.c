#include "SDL3/SDL_gpu.h"

#include "nyangine/nyangine.h"

// The FBX reader. An archive rather than part of this translation unit; see vendor_ufbx.h.
#include "ufbx.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API DECLRARATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/*
 * The filesystem is always there; the embedded blob is an optional layer in front of it.
 * NYA_ASSET_PREFER_BLOB bakes src/generated/assets.c into the binary and consults it first; anything not in
 * it still comes off disk. NYA_ASSET_HOT_RELOAD watches files that came off disk and reloads them on
 * change — orthogonal to the blob, since a blob-served asset has no file to watch.
 */
#ifdef NYA_ASSET_PREFER_BLOB
#include "generated/assets.c"

NYA_INTERNAL NYA_Error _nya_asset_load_raw_from_blob(NYA_AssetHandle path, OUT NYA_Asset* out_asset);
#endif // NYA_ASSET_PREFER_BLOB

/** Drops every memoised handle lookup. Defined beside the memo; declared here for nya_system_asset_init. */
NYA_INTERNAL void _nya_asset_lookup_invalidate(void);

NYA_INTERNAL NYA_Error _nya_asset_load_raw_from_filesystem(NYA_AssetHandle path, OUT NYA_Asset* out_asset);
NYA_INTERNAL void      _nya_asset_unload_raw_from_filesystem(NYA_Asset* asset);

/** Blob first when there is one, disk otherwise, and always disk for an external asset. */
NYA_INTERNAL NYA_Error _nya_asset_load_raw(NYA_AssetHandle handle, b8 external, OUT NYA_Asset* out_asset);

NYA_INTERNAL void      _nya_asset_fail(NYA_Asset* asset, const NYA_Error* error);
/**
 * A texture that has had its pixels staged and is waiting for the frame's copy pass. The transfer
 * buffer has to outlive the copy pass that reads it, which is why these are collected and released
 * together after the single submit rather than each upload cleaning up after itself.
 * */
typedef struct {
    SDL_GPUTransferBuffer* transfer;
    SDL_GPUTexture*        texture;
    u32                    width;
    u32                    height;
} _NYA_AssetPendingUpload;

nya_derive_array(_NYA_AssetPendingUpload);

/** Creates the texture and fills a transfer buffer with its pixels. Records no GPU commands. */
NYA_INTERNAL NYA_Error _nya_asset_stage_texture(SDL_Surface* surface, NYA_Arrayᐸ_NYA_AssetPendingUploadᐳ* pending, OUT NYA_Asset* out_asset);

/**
 * Turns FBX bytes into the triangles, material colour and texture of NYA_Asset.as_mesh. Takes the
 * frame's pending-upload list because the material's texture is uploaded through the same single
 * copy pass every other texture in the frame uses — see _nya_asset_flush_uploads.
 * */
NYA_INTERNAL NYA_Error _nya_asset_build_mesh(NYA_AssetHandle handle, const u8* data, u64 size, NYA_Arrayᐸ_NYA_AssetPendingUploadᐳ* pending,
                                             OUT NYA_Asset* out_asset);

/** Runs every staged upload in one copy pass, then releases the transfer buffers. */
NYA_INTERNAL void _nya_asset_flush_uploads(NYA_Arrayᐸ_NYA_AssetPendingUploadᐳ* pending);
NYA_INTERNAL void      _nya_asset_unload_raw(NYA_Asset* asset);
NYA_INTERNAL void      _nya_asset_cancel_queued_unload(NYA_Asset* asset);

/** Copies a caller supplied handle into the asset system's own memory. See the note at its definition. */
NYA_INTERNAL NYA_AssetHandle _nya_asset_intern(NYA_AssetHandle handle) __attr_no_discard;

#ifdef NYA_ASSET_HOT_RELOAD
#define _NYA_ASSET_RELOAD_GRACE_FRAMES 5

/**
 * The shortest interval between two stats of the same asset's file, while hot reload is on.
 * nya_asset_get is on every draw path, so a stat per call is a syscall per lookup per frame — tens of
 * thousands of them in a scene of any size. Rate limiting rather than removing: a tenth of a second is
 * far below the point anyone notices, and _NYA_ASSET_RELOAD_GRACE_FRAMES adds further latency on top.
 * */
#define _NYA_ASSET_STAT_INTERVAL_NS nya_time_ms_to_ns(100)
NYA_INTERNAL b8 _nya_asset_get_modification_time(NYA_Asset* asset, OUT u64* out_modification_time);

/*
 * ── Deliberately not NYA_INTERNAL ──
 *
 * Registered as callbacks with nya_callback, which stores the symbol *name*: update_callback_pointers
 * in main.c re-resolves each with dlsym after a hot reload, out of the executable's dynamic symbol
 * table. NYA_INTERNAL is `visibility("hidden") static`, which -rdynamic does not export, so making
 * these static builds fine and dies on the first reload's `nya_assert(callback->fn, "Could not find
 * symbol %s ...")`. clang-tidy's misc-use-internal-linkage flags all three — do not take its fix.
 */
// NOLINTNEXTLINE(misc-use-internal-linkage)
void _nya_asset_reload_process(NYA_Event* event);
#endif // NYA_ASSET_HOT_RELOAD

// Exported for the same reason as the reload hook above: resolved by name on every hot reload.
// NOLINTNEXTLINE(misc-use-internal-linkage)
void _nya_asset_loading_process(NYA_Event* event);
// NOLINTNEXTLINE(misc-use-internal-linkage)
void _nya_asset_unloading_process(NYA_Event* event);

NYA_INTERNAL NYA_AssetHandle _nya_asset_pick_correct_compiled_shader(NYA_AssetHandle source_shader, OUT SDL_GPUShaderFormat* out_format);

NYA_INTERNAL SDL_GPUVertexAttribute vertex_attributes[] = {
    {
     .location    = 0,
     .format      = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3,
     .offset      = nya_offsetof(NYA_Vertex3D,           position),
     .buffer_slot = 0,
     },
    {
     .location    = 1,
     .format      = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4,
     .offset      = nya_offsetof(NYA_Vertex3D,                                 color),
     .buffer_slot = 0,
     },
    {
     .location    = 2,
     .format      = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3,
     .offset      = nya_offsetof(NYA_Vertex3D,                                                normals),
     .buffer_slot = 0,
     },
    {
     .location    = 3,
     .format      = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2,
     .offset      = nya_offsetof(NYA_Vertex3D,uv),
     .buffer_slot = 0,
     },
};

NYA_INTERNAL SDL_GPUVertexBufferDescription vertex_buffer_description = {
    .slot               = 0,
    .input_rate         = SDL_GPU_VERTEXINPUTRATE_VERTEX,
    .instance_step_rate = 0,
    .pitch              = sizeof(NYA_Vertex3D),
};

/*
 * The retained mesh layout: the same vertices in buffer 0, plus a per-instance transform in buffer 1.
 * The four FLOAT4s at locations 4 to 7 are the *columns* of the model matrix, not its rows — no API
 * has a matrix element format, and the engine's matrices are column-major in memory. Splitting into
 * rows instead compiles, uploads, and silently transposes every model in the scene.
 *
 * `instance_step_rate` stays zero: it means "advance once per instance", where a rate of one advances
 * once per instance on some backends and once per *two* on others.
 */
NYA_INTERNAL SDL_GPUVertexAttribute vertex_attributes_3d_instanced[] = {
    { .location = 0, .format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3, .offset = nya_offsetof(NYA_Vertex3D, position), .buffer_slot = 0 },
    { .location = 1, .format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4, .offset = nya_offsetof(NYA_Vertex3D, color), .buffer_slot = 0 },
    { .location = 2, .format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3, .offset = nya_offsetof(NYA_Vertex3D, normals), .buffer_slot = 0 },
    { .location = 3, .format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, .offset = nya_offsetof(NYA_Vertex3D, uv), .buffer_slot = 0 },

    { .location = 4, .format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4, .offset = 0, .buffer_slot = 1 },
    { .location = 5, .format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4, .offset = 16, .buffer_slot = 1 },
    { .location = 6, .format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4, .offset = 32, .buffer_slot = 1 },
    { .location = 7, .format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4, .offset = 48, .buffer_slot = 1 },

    { .location = 8, .format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4, .offset = nya_offsetof(NYA_Render3DInstance, tint), .buffer_slot = 1 },
};

static_assert(nya_offsetof(NYA_Render3DInstance, model) == 0, "the instanced layout reads the model matrix from offset zero");
static_assert(nya_offsetof(NYA_Render3DInstance, tint) == 64, "the instanced layout expects the tint straight after a 4x4 of floats");

NYA_INTERNAL SDL_GPUVertexBufferDescription vertex_buffer_descriptions_3d_instanced[] = {
    { .slot = 0, .input_rate = SDL_GPU_VERTEXINPUTRATE_VERTEX,   .instance_step_rate = 0, .pitch = sizeof(NYA_Vertex3D) },
    { .slot = 1, .input_rate = SDL_GPU_VERTEXINPUTRATE_INSTANCE, .instance_step_rate = 0, .pitch = sizeof(NYA_Render3DInstance) },
};

/*
 * The compact layout the 2D batch uses. See NYA_Vertex2D. Same shader semantics as 3D — POSITION,
 * COLOR0, TEXCOORD0 — so one vertex shader source works against either; the differences are two
 * components instead of three for position, no normal, and a colour that arrives as four normalized
 * bytes.
 */
/*
 * The skinned 3D layout: the 3D one plus who moves each vertex. Locations 4 and 5 continue where the
 * base layout stops, so the shared part of the shader reads identically in both.
 *
 * Bone indices are UINT4 rather than a packed byte format — four bytes per vertex more than a UBYTE4,
 * in exchange for the index arriving in the shader as the integer it is, with no normalisation
 * convention to get wrong.
 */
NYA_INTERNAL SDL_GPUVertexAttribute vertex_attributes_3d_skinned[] = {
    {
     .location    = 0,
     .format      = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3,
     .offset      = nya_offsetof(NYA_VertexSkinned3D, position),
     .buffer_slot = 0,
     },
    {
     .location    = 1,
     .format      = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4,
     .offset      = nya_offsetof(NYA_VertexSkinned3D, color),
     .buffer_slot = 0,
     },
    {
     .location    = 2,
     .format      = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3,
     .offset      = nya_offsetof(NYA_VertexSkinned3D, normals),
     .buffer_slot = 0,
     },
    {
     .location    = 3,
     .format      = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2,
     .offset      = nya_offsetof(NYA_VertexSkinned3D, uv),
     .buffer_slot = 0,
     },
    {
     .location    = 4,
     .format      = SDL_GPU_VERTEXELEMENTFORMAT_UINT4,
     .offset      = nya_offsetof(NYA_VertexSkinned3D, bones),
     .buffer_slot = 0,
     },
    {
     .location    = 5,
     .format      = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4,
     .offset      = nya_offsetof(NYA_VertexSkinned3D, weights),
     .buffer_slot = 0,
     },
};

NYA_INTERNAL SDL_GPUVertexBufferDescription vertex_buffer_description_3d_skinned = {
    .slot               = 0,
    .input_rate         = SDL_GPU_VERTEXINPUTRATE_VERTEX,
    .instance_step_rate = 0,
    .pitch              = sizeof(NYA_VertexSkinned3D),
};

NYA_INTERNAL SDL_GPUVertexAttribute vertex_attributes_2d[] = {
    {
     .location    = 0,
     .format      = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2,
     .offset      = nya_offsetof(NYA_Vertex2D, x),
     .buffer_slot = 0,
     },
    {
     .location    = 1,
     // UBYTE4_NORM, not UBYTE4: NORM is what makes the input assembler divide by 255 and hand the
     // shader a float in 0..1. Without it the shader receives 0..255 and everything saturates white.
     .format      = SDL_GPU_VERTEXELEMENTFORMAT_UBYTE4_NORM,
     .offset      = nya_offsetof(NYA_Vertex2D, color),
     .buffer_slot = 0,
     },
    {
     .location    = 2,
     .format      = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2,
     .offset      = nya_offsetof(NYA_Vertex2D, u),
     .buffer_slot = 0,
     },
};

NYA_INTERNAL SDL_GPUVertexBufferDescription vertex_buffer_description_2d = {
    .slot               = 0,
    .input_rate         = SDL_GPU_VERTEXINPUTRATE_VERTEX,
    .instance_step_rate = 0,
    .pitch              = sizeof(NYA_Vertex2D),
};

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

void nya_system_asset_init(void) {
    NYA_App* app = nya_app_get();

    app->asset_system = (NYA_AssetSystem){
        .allocator = nya_arena_create(.name = "asset_system_allocator"),
    };

    // Each decoder is a library in its own right. Failure here is not fatal: a build with no audio
    // device still runs, it just cannot load sounds.
    if (!TTF_Init()) nya_log_warn("TTF_Init() failed, fonts will not load: %s", SDL_GetError());

    if (!MIX_Init()) {
        nya_log_warn("MIX_Init() failed, sounds will not load: %s", SDL_GetError());
    } else {
        app->asset_system.mixer = MIX_CreateMixerDevice(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, nullptr);
        if (app->asset_system.mixer == nullptr) nya_log_warn("MIX_CreateMixerDevice() failed, sounds will not load: %s", SDL_GetError());
    }

    app->asset_system.assets          = nya_dict_create(app->asset_system.allocator, NYA_Asset);

    // A fresh dictionary shares nothing with the last one's entries.
    _nya_asset_lookup_invalidate();
    app->asset_system.loading_queue   = nya_array_create(app->asset_system.allocator, NYA_AssetLoadParameters);
    app->asset_system.unloading_queue = nya_array_create(app->asset_system.allocator, NYA_AssetHandle);

    nya_event_hook_register((NYA_EventHook){
        .hook_type  = NYA_EVENT_HOOK_TYPE_IMMEDIATE,
        .event_type = NYA_EVENT_FRAME_ENDED,
        .fn         = nya_callback(_nya_asset_unloading_process),
    });

    nya_event_hook_register((NYA_EventHook){
        .hook_type  = NYA_EVENT_HOOK_TYPE_IMMEDIATE,
        .event_type = NYA_EVENT_FRAME_ENDED,
        .fn         = nya_callback(_nya_asset_loading_process),
    });

#ifdef NYA_ASSET_HOT_RELOAD
    app->asset_system.reload_queue = nya_array_create(app->asset_system.allocator, NYA_AssetHandle);
    nya_event_hook_register((NYA_EventHook){
        .hook_type  = NYA_EVENT_HOOK_TYPE_IMMEDIATE,
        .event_type = NYA_EVENT_FRAME_ENDED,
        .fn         = nya_callback(_nya_asset_reload_process),
    });
#endif // NYA_ASSET_HOT_RELOAD

    nya_log_info("Asset system initialized.");
}

void nya_system_asset_deinit(void) {
    NYA_App* app = nya_app_get();

    // submit all assets to be unloaded
    nya_dict_foreach_value(app->asset_system.assets, asset) {
        if (asset && asset->status != NYA_ASSET_STATUS_UNLOADED) { /**/
            nya_array_push_back(app->asset_system.unloading_queue, asset->handle);
        }
    }
    _nya_asset_unloading_process(nullptr); // process unloading immediately

    // After the assets, since a MIX_Audio outlives neither its mixer nor its bytes.
    if (app->asset_system.mixer != nullptr) {
        MIX_DestroyMixer(app->asset_system.mixer);
        app->asset_system.mixer = nullptr;
    }
    MIX_Quit();
    TTF_Quit();

    nya_array_destroy(app->asset_system.loading_queue);
    nya_array_destroy(app->asset_system.unloading_queue);
#ifdef NYA_ASSET_HOT_RELOAD
    nya_array_destroy(app->asset_system.reload_queue);
#endif // NYA_ASSET_HOT_RELOAD

    nya_dict_destroy(app->asset_system.assets);

    nya_arena_destroy(app->asset_system.allocator);

    nya_log_info("Asset system deinitialized.");
}

/*
 * ─────────────────────────────────────────────────────────
 * ASSET FUNCTIONS
 * ─────────────────────────────────────────────────────────
 */

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * HANDLE LOOKUP MEMO
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/**
 * Slots in the direct-mapped memo in front of the asset dictionary. A power of two.
 *
 * 256 is a few kilobytes and more distinct handles than a frame touches — a frame draws from a handful
 * of textures, a font and some sounds — so collisions are rare, and a collision only costs the lookup
 * that would have happened anyway.
 * */
#define _NYA_ASSET_LOOKUP_SLOTS 256

/**
 * One remembered lookup, keyed on the handle **pointer** rather than its text.
 *
 * Every handle in the tree is a generated `#define` — a string literal with a stable address — so the
 * same asset is almost always asked for through the same pointer. Comparing pointers turns a lookup
 * that hashed a path into one compare.
 *
 * Benchmarked: nya_hash_fnv1a over a real asset path is ~44 ns, against ~0.2 ns for an integer, and
 * nya_asset_get plus that hash together were 1.28% of a profile. See bench/bench_core.c.
 * */
typedef struct {
    NYA_AssetHandle handle;
    NYA_Asset*      asset;

    /** Which generation of the dictionary this was true for. See _nya_asset_lookup_generation. */
    u64 generation;
} _NYA_AssetLookupEntry;

NYA_INTERNAL _NYA_AssetLookupEntry _nya_asset_lookup[_NYA_ASSET_LOOKUP_SLOTS] = { 0 };

/**
 * Bumped whenever the dictionary changes shape, invalidating every memo entry at once.
 *
 * A pointer into a hash map is only valid until it rehashes, so a memo holding NYA_Asset* has to be
 * dropped whenever anything is inserted. Insertions happen at load time and lookups happen per frame,
 * so paying a whole-table invalidation for the rare one is the right way round. Starts at 1, so a
 * zeroed entry never matches.
 * */
NYA_INTERNAL u64 _nya_asset_lookup_generation = 1;

/** Invalidates the memo. Called wherever the dictionary is created or inserted into. */
NYA_INTERNAL void _nya_asset_lookup_invalidate(void) {
    _nya_asset_lookup_generation++;
}

NYA_Asset* nya_asset_get(NYA_AssetHandle handle) {
    nya_assert(handle != nullptr);

    NYA_AssetSystem* system = &nya_app_get()->asset_system;

    // Null before the asset system is up, rather than a fault: reachable outside tests too, since a
    // sprite atlas can legitimately be built during static setup, before anything has loaded.
    if (system->assets == nullptr) return nullptr;

    /*
     * The memo, before the dictionary.
     *
     * Keyed on the pointer, so this is exact rather than approximate: the same pointer with the
     * dictionary unchanged is necessarily the same asset. Two different pointers holding identical text
     * simply miss and fall through, costing what the lookup used to cost anyway.
     */
    _NYA_AssetLookupEntry* entry = &_nya_asset_lookup[((uintptr_t)handle >> 3) & (_NYA_ASSET_LOOKUP_SLOTS - 1)];

    NYA_Asset* asset = nullptr;

    if (entry->handle == handle && entry->generation == _nya_asset_lookup_generation) {
        asset = entry->asset;
    } else {
        asset = nya_dict_get(system->assets, handle);

        *entry = (_NYA_AssetLookupEntry){ .handle = handle, .asset = asset, .generation = _nya_asset_lookup_generation };
    }

#ifdef NYA_ASSET_HOT_RELOAD
    // Only assets that came off disk have a file to have changed; one served from the blob is part of
    // the executable.
    if (asset != nullptr && asset->status == NYA_ASSET_STATUS_LOADED && !asset->from_blob) {
        // Uptime rather than wall clock: sampled once per frame so every lookup within it agrees, and
        // it cannot jump backwards when the system clock is adjusted.
        u64 now_ns = nya_app_get()->frame_stats.uptime_ns;

        if (now_ns < asset->next_stat_time_ns) return asset;
        asset->next_stat_time_ns = now_ns + _NYA_ASSET_STAT_INTERVAL_NS;

        u64 file_modification_time = 0;
        _nya_asset_get_modification_time(asset, &file_modification_time);

        if (file_modification_time > asset->source_modification_time) { /**/
            if (nya_array_contains(system->reload_queue, asset->handle)) return asset;

            asset->reload_grace_frames = _NYA_ASSET_RELOAD_GRACE_FRAMES;
            nya_array_push_back(system->reload_queue, asset->handle);
            nya_log_debug("Asset marked for reload due to modification: %s", asset->handle);
        }
    }
#endif // NYA_ASSET_HOT_RELOAD

    return asset;
}

NYA_Error nya_asset_acquire(NYA_AssetHandle handle) {
    if (handle == nullptr) return nya_error(NYA_ERROR_INVALID_ARGUMENT, "asset handle is null");

    NYA_AssetSystem* system = &nya_app_get()->asset_system;

    NYA_Asset* asset = nya_dict_get(system->assets, handle);
    if (asset == nullptr) return nya_error(NYA_ERROR_NOT_FOUND, "cannot acquire '%s': it was never loaded", handle);
    if (asset->status == NYA_ASSET_STATUS_FAILED) return nya_error(NYA_ERROR_NOT_OK, "cannot acquire '%s': it failed to load", handle);

    // Taking a reference cancels a pending unload. The asset is still intact here since the queue is
    // not processed until the end of the frame, so this is a plain revival, not a resurrection.
    if (asset->queued_for_unload) {
        _nya_asset_cancel_queued_unload(asset);
        asset->queued_for_unload = false;
    }

    atomic_fetch_add(&asset->reference_count, 1);
    return NYA_OK;
}

void nya_asset_release(NYA_AssetHandle handle) {
    if (handle == nullptr) return;

    NYA_AssetSystem* system = &nya_app_get()->asset_system;

    NYA_Asset* asset = nya_dict_get(system->assets, handle);
    if (asset == nullptr) {
        nya_log_warn("Released asset '%s', which was never loaded. This release has no matching acquire.", handle);
        return;
    }

    // Checked rather than subtracted blindly. atomic_fetch_sub on zero wraps to UINT64_MAX, and the
    // asset then has a reference count nothing can ever bring back down — it would never unload, and
    // nothing would say why.
    u64 previous = atomic_load(&asset->reference_count);
    while (previous > 0) {
        if (atomic_compare_exchange_weak(&asset->reference_count, &previous, previous - 1)) break;
    }

    if (previous == 0) {
        nya_log_warn("Released asset '%s' more times than it was acquired.", handle);
        return;
    }

    if (previous == 1) (void)nya_asset_unload(handle);
}

u64 nya_asset_reference_count(NYA_AssetHandle handle) {
    if (handle == nullptr) return 0;

    NYA_AssetSystem* system = &nya_app_get()->asset_system;

    NYA_Asset* asset = nya_dict_get(system->assets, handle);
    return asset ? atomic_load(&asset->reference_count) : 0;
}

NYA_Error nya_asset_load(NYA_AssetLoadParameters parameters) {
    NYA_AssetSystem* system = &nya_app_get()->asset_system;

    if (parameters.handle == nullptr) return nya_error(NYA_ERROR_INVALID_ARGUMENT, "asset handle is null");

    NYA_Asset* asset = nya_dict_get(system->assets, parameters.handle);

    // Already known. A previous failure is terminal, so say so rather than silently doing nothing.
    if (asset != nullptr && asset->status == NYA_ASSET_STATUS_FAILED) {
        return nya_error(NYA_ERROR_NOT_OK, "asset '%s' previously failed to load", parameters.handle);
    }
    if (asset != nullptr && asset->status != NYA_ASSET_STATUS_UNLOADED) return NYA_OK;

    // Every handle is copied into its own memory first. Handles are string literals generated into
    // assets.h, living in the game's DLL; hot reload dlcloses that DLL and unmaps the .rodata they sit
    // in, which would leave the dict hashing keys that point into nothing. The asset system outlives
    // every DLL that talks to it, so the strings have to as well.
    parameters.handle = _nya_asset_intern(parameters.handle);

    if (parameters.type == NYA_ASSET_TYPE_GRAPHICS_PIPELINE) {
        parameters.as_graphics_pipeline.vertex_shader_handle   = _nya_asset_intern(parameters.as_graphics_pipeline.vertex_shader_handle);
        parameters.as_graphics_pipeline.fragment_shader_handle = _nya_asset_intern(parameters.as_graphics_pipeline.fragment_shader_handle);
    }

    NYA_Asset new_asset = (NYA_Asset){
        .handle          = parameters.handle,
        .status          = NYA_ASSET_STATUS_LOADING,
        .load_parameters = parameters,
        .reference_count = 0,
    };

    nya_dict_set(system->assets, parameters.handle, new_asset);

    // The insert may have rehashed, moving every NYA_Asset* the memo holds.
    _nya_asset_lookup_invalidate();
    nya_array_push_back(system->loading_queue, parameters);

    nya_log_debug("Queuing asset for loading: %s", parameters.handle);

    return NYA_OK;
}

NYA_AssetStatus nya_asset_status(NYA_AssetHandle handle) {
    NYA_AssetSystem* system = &nya_app_get()->asset_system;

    NYA_Asset* asset = nya_dict_get(system->assets, handle);
    return asset ? asset->status : NYA_ASSET_STATUS_UNLOADED;
}

b8 nya_asset_unload(NYA_AssetHandle handle) {
    if (handle == nullptr) return false;

    NYA_AssetSystem* system = &nya_app_get()->asset_system;

    NYA_Asset* asset = nya_dict_get(system->assets, handle);
    if (asset == nullptr) return false;

    // Still in use by someone else — not an error: sharing an asset between two systems is normal,
    // and whichever finishes first should not pull it out from under the other.
    if (atomic_load(&asset->reference_count) > 0) return false;

    if (asset->queued_for_unload) return true; // already on its way out
    if (asset->status == NYA_ASSET_STATUS_UNLOADED) return true;

    asset->queued_for_unload = true;
    nya_array_push_back(system->unloading_queue, asset->handle);

    nya_log_debug("Queuing asset for unloading: %s", asset->handle);
    return true;
}

NYA_Error nya_asset_read(NYA_Arena* arena, NYA_AssetHandle handle, OUT u8** out_data, OUT u64* out_size) {
    nya_assert(arena != nullptr);
    nya_assert(out_data != nullptr);
    nya_assert(out_size != nullptr);

    *out_data = nullptr;
    *out_size = 0;

    if (handle == nullptr) return nya_error(NYA_ERROR_INVALID_ARGUMENT, "asset handle is null");

    // A bare NYA_Asset used as a scratch buffer, never entered into the registry: this is a read,
    // not a load. Same trick nya_asset_set_window_icon uses below.
    NYA_Asset raw = { 0 };
    NYA_TRY(_nya_asset_load_raw(handle, false, &raw));
    defer _nya_asset_unload_raw(&raw);

    // Copied rather than handed out. Blob bytes point into the executable and would be fine, but
    // filesystem bytes live in the asset system's arena and are freed by the defer above — so
    // returning either directly would be right half the time, which is the worst kind of right.
    u8* copy = nya_arena_alloc(arena, raw.as_text.size);
    nya_memcpy(copy, raw.as_text.data, raw.as_text.size);

    *out_data = copy;
    *out_size = raw.as_text.size;

    return NYA_OK;
}

NYA_Error nya_asset_set_window_icon(NYA_WindowHandle window, NYA_AssetHandle handle) {
    if (handle == nullptr) return nya_error(NYA_ERROR_INVALID_ARGUMENT, "asset handle is null");

    // See the scratch-buffer note in nya_asset_read above.
    NYA_Asset raw = { 0 };
    NYA_TRY(_nya_asset_load_raw(handle, false, &raw));
    defer _nya_asset_unload_raw(&raw);

    return nya_window_set_icon(window, raw.as_text.data, raw.as_text.size);
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

#ifdef NYA_ASSET_PREFER_BLOB
NYA_INTERNAL NYA_Error _nya_asset_load_raw_from_blob(NYA_AssetHandle path, OUT NYA_Asset* out_asset) {
    nya_assert(path != nullptr);

    for (u64 asset_header_index = 0; asset_header_index < NYA_ASSET_BLOB_HEADER_COUNT; asset_header_index++) {
        NYA_AssetBlobHeader asset_header = NYA_ASSET_BLOB_HEADER[asset_header_index];
        if (!nya_string_equals(asset_header.path, path)) continue;

        out_asset->as_text.data = (u8*)NYA_ASSET_BLOB + asset_header.start;
        out_asset->as_text.size = asset_header.size;

        return NYA_OK;
    }

    // A build problem, but not worth killing the process for: one missing texture should not take
    // the game down.
    return nya_error(NYA_ERROR_NOT_FOUND, "asset not in the embedded blob: %s. Was it indexed at build time?", path);
}
#endif // NYA_ASSET_PREFER_BLOB

NYA_INTERNAL NYA_Error _nya_asset_load_raw_from_filesystem(NYA_CString path, OUT NYA_Asset* out_asset) {
    nya_assert(path != nullptr);

    // Recoverable, not fatal. An external asset is a path from outside the game and the file may
    // simply have moved since the player picked it.
    if (!nya_filesystem_exists(path)) return nya_error(NYA_ERROR_NOT_FOUND, "asset not found on disk: %s", path);

    NYA_App*   app   = nya_app_get();
    NYA_Arena* arena = app->asset_system.allocator;

    NYA_String* content = nya_string_create(arena);
    NYA_TRY(nya_file_read(path, content));
    nya_string_shrink_to_fit(content);
    u8* data = content->items;
    u64 size = content->length;
    nya_arena_free(content->arena, content, sizeof(NYA_String));

    u64 modification_time = 0;
    NYA_TRY(nya_filesystem_last_modified(path, &modification_time));

    out_asset->as_text.data = data;
    out_asset->as_text.size = size;

#ifdef NYA_ASSET_HOT_RELOAD
    out_asset->source_modification_time = modification_time;
#else
    nya_unused(modification_time); // nothing is watching, so nothing needs the timestamp
#endif

    return NYA_OK;
}

NYA_INTERNAL void _nya_asset_unload_raw_from_filesystem(NYA_Asset* asset) {
    nya_assert(asset != nullptr);

    NYA_App*   app   = nya_app_get();
    NYA_Arena* arena = app->asset_system.allocator;

    nya_arena_free(arena, asset->as_text.data, asset->as_text.size);

    asset->as_text.data = nullptr;
    asset->as_text.size = 0;
}

/**
 * Marks an asset as unloadable and says why, once.
 *
 * Not nya_log_panic: a missing texture should cost you a texture, not the process. The status is
 * terminal so the loader does not retry it every frame.
 * */
NYA_INTERNAL void _nya_asset_fail(NYA_Asset* asset, const NYA_Error* error) {
    nya_assert(asset != nullptr);
    nya_assert(error != nullptr);

    nya_log_error("Asset '%s' failed to load: %s", asset->handle, (NYA_ConstCString)error->message);

    asset->status = NYA_ASSET_STATUS_FAILED;

    nya_event_dispatch((NYA_Event){
        .type           = NYA_EVENT_ASSET_LOAD_FAILED,
        .as_asset_event = { .asset_handle = asset->handle },
    });
}

/**
 * Copies a decoded surface into GPU memory. The GPU cannot read the surface directly: pixels go
 * through a transfer buffer and are copied in on a command buffer. Everything is converted to RGBA32
 * first so the texture format is known regardless of what the file contained.
 * */
/*
 * Staging and submission are separate because a copy pass is per command buffer, not per texture.
 * Every texture used to acquire its own command buffer and submit individually, so twenty textures in
 * a frame meant twenty submissions. The pixel work below is per texture and unavoidable; the command
 * recording now happens once for the whole batch in _nya_asset_flush_uploads.
 */
NYA_INTERNAL NYA_Error _nya_asset_stage_texture(SDL_Surface* surface, NYA_Arrayᐸ_NYA_AssetPendingUploadᐳ* pending, OUT NYA_Asset* out_asset) {
    nya_assert(surface != nullptr);
    nya_assert(pending != nullptr);
    nya_assert(out_asset != nullptr);

    NYA_RenderSystem* render_system = &nya_app_get()->render_system;
    if (render_system->gpu_device == nullptr) return nya_error(NYA_ERROR_NOT_SUPPORTED, "no GPU device; cannot upload a texture");

    SDL_Surface* rgba      = surface;
    b8           converted = false;
    if (surface->format != SDL_PIXELFORMAT_RGBA32) {
        rgba = SDL_ConvertSurface(surface, SDL_PIXELFORMAT_RGBA32);
        if (rgba == nullptr) return nya_error(NYA_ERROR_NOT_OK, "could not convert image to RGBA32: %s", SDL_GetError());
        converted = true;
    }

    u32 width  = (u32)rgba->w;
    u32 height = (u32)rgba->h;

    // Computed wide and checked, because the transfer buffer's size is a u32 and four bytes per
    // pixel overflows one at around 32k by 32k. A decoder handing back something that large is a
    // reason to refuse the image, not to allocate a buffer a small fraction of the size needed.
    u64 size_wide = (u64)width * (u64)height * 4ULL;
    if (size_wide == 0 || size_wide > U32_MAX) {
        if (converted) SDL_DestroySurface(rgba);
        return nya_error(NYA_ERROR_NOT_SUPPORTED, "image is %ux%u, which does not fit a single GPU transfer buffer", width, height);
    }
    u32 size = (u32)size_wide;

    SDL_GPUTexture* texture = SDL_CreateGPUTexture(
        render_system->gpu_device,
        &(SDL_GPUTextureCreateInfo){
            .type                 = SDL_GPU_TEXTURETYPE_2D,
            .format               = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM,
            .usage                = SDL_GPU_TEXTUREUSAGE_SAMPLER,
            .width                = width,
            .height               = height,
            .layer_count_or_depth = 1,
            .num_levels           = 1,
        }
    );

    if (texture == nullptr) {
        if (converted) SDL_DestroySurface(rgba);
        return nya_error(NYA_ERROR_OUT_OF_MEMORY, "SDL_CreateGPUTexture() failed: %s", SDL_GetError());
    }

    SDL_GPUTransferBuffer* transfer = SDL_CreateGPUTransferBuffer(
        render_system->gpu_device,
        &(SDL_GPUTransferBufferCreateInfo){ .usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD, .size = size }
    );

    if (transfer == nullptr) {
        SDL_ReleaseGPUTexture(render_system->gpu_device, texture);
        if (converted) SDL_DestroySurface(rgba);
        return nya_error(NYA_ERROR_OUT_OF_MEMORY, "SDL_CreateGPUTransferBuffer() failed: %s", SDL_GetError());
    }

    void* mapped = SDL_MapGPUTransferBuffer(render_system->gpu_device, transfer, false);
    if (mapped == nullptr) {
        SDL_ReleaseGPUTransferBuffer(render_system->gpu_device, transfer);
        SDL_ReleaseGPUTexture(render_system->gpu_device, texture);
        if (converted) SDL_DestroySurface(rgba);
        return nya_error(NYA_ERROR_OUT_OF_MEMORY, "SDL_MapGPUTransferBuffer() failed: %s", SDL_GetError());
    }

    /*
     * Row by row, because a surface's pitch is not required to equal its width in bytes — SDL pads
     * scanlines, and a surface already in RGBA32 skips SDL_ConvertSurface entirely, so it can be a
     * sub-surface or come from a decoder that padded. Copying width * height * 4 in one go would read
     * the padding as pixels and walk off the end of the last row.
     */
    u32 row_bytes = width * 4;
    for (u32 row = 0; row < height; row++) {
        nya_memcpy((u8*)mapped + ((u64)row * row_bytes), (const u8*)rgba->pixels + ((u64)row * (u64)rgba->pitch), row_bytes);
    }

    SDL_UnmapGPUTransferBuffer(render_system->gpu_device, transfer);

    if (converted) SDL_DestroySurface(rgba);

    // Handed over now though nothing is copied in yet: nothing can sample it before the frame ends,
    // since the asset is marked loaded only after the flush.
    nya_array_push_back(pending, ((_NYA_AssetPendingUpload){ .transfer = transfer, .texture = texture, .width = width, .height = height }));

    out_asset->as_texture.texture = texture;
    out_asset->as_texture.width   = width;
    out_asset->as_texture.height  = height;

    return NYA_OK;
}

NYA_INTERNAL void _nya_asset_flush_uploads(NYA_Arrayᐸ_NYA_AssetPendingUploadᐳ* pending) {
    nya_assert(pending != nullptr);

    if (pending->length == 0) return;

    NYA_RenderSystem* render_system = &nya_app_get()->render_system;
    nya_assert(render_system->gpu_device != nullptr, "staged uploads with no GPU device to submit them to.");

    SDL_GPUCommandBuffer* command_buffer = SDL_AcquireGPUCommandBuffer(render_system->gpu_device);
    if (command_buffer == nullptr) {
        // Nothing has been copied, so the textures are undefined rather than wrong. Still release the
        // transfer buffers; the textures belong to their assets now and are freed on unload as usual.
        nya_log_error("SDL_AcquireGPUCommandBuffer() failed, dropping " FMTu64 " texture uploads: %s", pending->length, SDL_GetError());
        nya_array_foreach (pending, upload) SDL_ReleaseGPUTransferBuffer(render_system->gpu_device, upload->transfer);
        nya_array_clear(pending);
        return;
    }

    SDL_GPUCopyPass* copy_pass = SDL_BeginGPUCopyPass(command_buffer);

    nya_array_foreach (pending, upload) {
        SDL_UploadToGPUTexture(
            copy_pass,
            &(SDL_GPUTextureTransferInfo){ .transfer_buffer = upload->transfer, .offset = 0 },
            &(SDL_GPUTextureRegion){ .texture = upload->texture, .w = upload->width, .h = upload->height, .d = 1 },
            false
        );
    }

    SDL_EndGPUCopyPass(copy_pass);
    SDL_SubmitGPUCommandBuffer(command_buffer);

    // Released only after the submit: SDL keeps the buffer alive until the copy has run, but the
    // release must come after the referencing commands are recorded and handed over.
    nya_array_foreach (pending, upload) SDL_ReleaseGPUTransferBuffer(render_system->gpu_device, upload->transfer);

    nya_array_clear(pending);
}


/*
 * ─────────────────────────────────────────────────────────
 * SKINNING EXTRACTION
 * ─────────────────────────────────────────────────────────
 */

/** ufbx's matrix is three rows of an affine transform; this is the same thing as a full 4x4. */
NYA_INTERNAL ufbx_matrix _nya_asset_node_world_at(ufbx_anim* anim, ufbx_node* node, f64 time);

NYA_INTERNAL f32_4x4 _nya_asset_matrix_from_ufbx(ufbx_matrix matrix) {
    return nya_matrix_create(
        (f32x4){ (f32)matrix.m00, (f32)matrix.m01, (f32)matrix.m02, (f32)matrix.m03 },
        (f32x4){ (f32)matrix.m10, (f32)matrix.m11, (f32)matrix.m12, (f32)matrix.m13 },
        (f32x4){ (f32)matrix.m20, (f32)matrix.m21, (f32)matrix.m22, (f32)matrix.m23 },
        (f32x4){ 0.0F, 0.0F, 0.0F, 1.0F }
    );
}

NYA_INTERNAL NYA_BoneTransform _nya_asset_bone_transform(ufbx_transform transform) {
    return (NYA_BoneTransform){
        .translation = { (f32)transform.translation.x, (f32)transform.translation.y, (f32)transform.translation.z },
        .rotation    = { (f32)transform.rotation.x, (f32)transform.rotation.y, (f32)transform.rotation.z, (f32)transform.rotation.w },
        .scale       = { (f32)transform.scale.x, (f32)transform.scale.y, (f32)transform.scale.z },
    };
}

/** The skin deformer of the first mesh that has one, or null. */
NYA_INTERNAL ufbx_skin_deformer* _nya_asset_find_skin(ufbx_scene* scene) {
    for (u64 i = 0; i < scene->meshes.count; i++) {
        if (scene->meshes.data[i]->skin_deformers.count > 0) return scene->meshes.data[i]->skin_deformers.data[0];
    }

    return nullptr;
}

/**
 * Builds the skeleton and bakes every clip, or answers null when the file is not rigged.
 *
 * Bones come from the skin's clusters rather than `scene->bones`: a cluster is a bone that actually
 * *deforms this mesh* and carries the inverse bind matrix, whereas a file routinely contains control
 * bones and helpers that deform nothing and would leave the shader indexing unreferenced entries.
 * */
NYA_INTERNAL NYA_Skeleton* _nya_asset_mesh_skeleton(NYA_Arena* arena, ufbx_scene* scene) {
    ufbx_skin_deformer* skin = _nya_asset_find_skin(scene);

    if (skin == nullptr || skin->clusters.count == 0) return nullptr;

    if (skin->clusters.count > NYA_SKELETON_MAX_BONES) {
        nya_log_warn("The model has %llu bones and the palette holds %d; the rest are ignored.",
                 (unsigned long long)skin->clusters.count, NYA_SKELETON_MAX_BONES);
    }

    u32 bone_count = (u32)(skin->clusters.count < NYA_SKELETON_MAX_BONES ? skin->clusters.count : NYA_SKELETON_MAX_BONES);

    NYA_Skeleton* skeleton = nya_arena_alloc(arena, sizeof(NYA_Skeleton));

    *skeleton = (NYA_Skeleton){
        .bones      = nya_arena_alloc(arena, bone_count * sizeof(NYA_SkeletonBone)),
        .bone_count = bone_count,
    };

    for (u32 i = 0; i < bone_count; i++) {
        ufbx_skin_cluster* cluster = skin->clusters.data[i];

        NYA_SkeletonBone* bone = &skeleton->bones[i];

        *bone = (NYA_SkeletonBone){
            .parent = -1,

            // geometry_to_bone is exactly the inverse bind: geometry space in, bone space out.
            .inverse_bind = _nya_asset_matrix_from_ufbx(cluster->geometry_to_bone),
        };

        NYA_ConstCString name = cluster->bone_node != nullptr ? cluster->bone_node->name.data : "bone";

        (void)snprintf(bone->name, sizeof(bone->name), "%s", name);

        bone->rest = (NYA_BoneTransform){ .scale = { 1.0F, 1.0F, 1.0F } };
    }

    /*
     * Parents must come before children, which nya_skeleton_palette relies on to compose in one pass.
     * Rather than sorting — which would renumber every parent index and vertex weight — this checks
     * and warns instead. ufbx emits clusters in file declaration order; a child before its parent is
     * possible but not seen here.
     */
    for (u32 i = 0; i < bone_count; i++) {
        if (skeleton->bones[i].parent >= (s32)i) {
            nya_log_warn("Bone '%s' is declared before its parent; its animation will lag by a frame.", skeleton->bones[i].name);
        }
    }

    /*
     * Parents resolved by matching each cluster's node against the others. A cluster's node parent may
     * itself have no cluster — a helper, or the armature root — so walking up to a node that *is* a
     * cluster gives the nearest deforming ancestor: the parent as far as the palette is concerned.
     */
    for (u32 i = 0; i < bone_count; i++) {
        ufbx_node* node = skin->clusters.data[i]->bone_node;

        if (node == nullptr) continue;

        for (ufbx_node* ancestor = node->parent; ancestor != nullptr; ancestor = ancestor->parent) {
            b8 found = false;

            for (u32 j = 0; j < bone_count && !found; j++) {
                if (skin->clusters.data[j]->bone_node != ancestor) continue;

                skeleton->bones[i].parent = (s32)j;
                found                     = true;
            }

            if (found) break;
        }
    }


    /*
     * The rest pose, derived from the bind matrices rather than read off the nodes.
     * `bone_node->local_transform` is the obvious source and is wrong here: a bone's parent chain runs
     * through non-bone nodes — the armature object at minimum — so composing cluster-local transforms
     * would skip what those nodes contribute and displace the rest pose by the armature's transform.
     * Inverting the bind matrix instead gives each bone's model-space transform at bind time directly,
     * making "the rest palette is the identity" true by construction.
     */
    for (u32 i = 0; i < bone_count; i++) {
        ufbx_matrix geometry_to_bone = skin->clusters.data[i]->geometry_to_bone;
        ufbx_matrix bone_to_geometry = ufbx_matrix_invert(&geometry_to_bone);

        s32 parent = skeleton->bones[i].parent;

        ufbx_matrix local = bone_to_geometry;

        if (parent >= 0) {
            // Relative to the parent's bind transform, which is what a local transform means.
            ufbx_matrix parent_geometry_to_bone = skin->clusters.data[parent]->geometry_to_bone;

            local = ufbx_matrix_mul(&parent_geometry_to_bone, &bone_to_geometry);
        }

        skeleton->bones[i].rest = _nya_asset_bone_transform(ufbx_matrix_to_transform(&local));
    }

    /*
     * The mesh's geometry-to-world transform at bind time. `bind_to_world` is a bone's world
     * transform when bound, `geometry_to_bone` takes geometry into that bone — composing them
     * recovers the geometry-to-world the whole rig was bound in, the same for every cluster, hence
     * cluster zero. Needed only for root bones; between two bones it cancels (see the clip loop).
     */
    ufbx_matrix bind_to_world     = skin->clusters.data[0]->bind_to_world;
    ufbx_matrix geometry_to_bone0 = skin->clusters.data[0]->geometry_to_bone;
    ufbx_matrix geometry_bind     = ufbx_matrix_mul(&bind_to_world, &geometry_to_bone0);

    ufbx_matrix geometry_bind_inverse = ufbx_matrix_invert(&geometry_bind);

    // ── clips ──
    if (scene->anim_stacks.count > 0) {
        skeleton->clips      = nya_arena_alloc(arena, scene->anim_stacks.count * sizeof(NYA_SkeletonClip));
        skeleton->clip_count = (u32)scene->anim_stacks.count;

        for (u64 c = 0; c < scene->anim_stacks.count; c++) {
            ufbx_anim_stack* stack = scene->anim_stacks.data[c];

            NYA_SkeletonClip* clip = &skeleton->clips[c];

            f32 duration = (f32)(stack->time_end - stack->time_begin);

            if (duration < 0.0F) duration = 0.0F;

            /*
             * Baked at a fixed rate rather than the file's own keyframe times: a uniform grid makes
             * sampling a division instead of a search, and thirty per second is well above what this
             * art style resolves. A clip authored on twos loses nothing; one with curves is resampled.
             */
            f32 rate  = 30.0F;
            u32 frames = (u32)(duration * rate) + 1;

            *clip = (NYA_SkeletonClip){
                .duration_s  = duration,
                .frame_count = frames,
                .frame_rate  = rate,
                .frames      = nya_arena_alloc(arena, (u64)frames * bone_count * sizeof(NYA_BoneTransform)),
            };

            (void)snprintf(clip->name, sizeof(clip->name), "%s", stack->name.data);

            for (u32 f = 0; f < frames; f++) {
                f64 time = stack->time_begin + ((f64)f / (f64)rate);

                if (time > stack->time_end) time = stack->time_end;

                /*
                 * World first, then made relative — the same convention the rest pose is derived in.
                 * ufbx's node-local transform is wrong here: a bone's node parent is not its *bone*
                 * parent, so composing local transforms would put the root bone's animation into
                 * geometry space twice.
                 */
                ufbx_matrix world[NYA_SKELETON_MAX_BONES];

                for (u32 b = 0; b < bone_count; b++) {
                    ufbx_node* node = skin->clusters.data[b]->bone_node;

                    world[b] = node != nullptr ? _nya_asset_node_world_at(stack->anim, node, time) : ufbx_identity_matrix;
                }

                for (u32 b = 0; b < bone_count; b++) {
                    NYA_BoneTransform* out = &clip->frames[((u64)f * bone_count) + b];

                    s32 parent = skeleton->bones[b].parent;

                    ufbx_matrix local;

                    if (parent >= 0) {
                        // Between two bones the geometry constant cancels, so this is just the child
                        // expressed in the parent's frame.
                        ufbx_matrix parent_inverse = ufbx_matrix_invert(&world[parent]);

                        local = ufbx_matrix_mul(&parent_inverse, &world[b]);
                    } else {
                        // A root bone still has to be brought into geometry space, which is what the
                        // constant carries. See `geometry_bind` above.
                        local = ufbx_matrix_mul(&geometry_bind_inverse, &world[b]);
                    }

                    *out = _nya_asset_bone_transform(ufbx_matrix_to_transform(&local));
                }
            }
        }
    }

    return skeleton;
}


/**
 * A node's world transform at `time`, composed up its parent chain. ufbx_evaluate_transform answers a
 * transform *relative to its parent*, and there is no evaluated scene to read a world transform from
 * — see UFBX_NO_SCENE_EVALUATION in vendor_ufbx.h. This walks the real node chain rather than the
 * bone chain, so armature roots and helper nodes are included.
 * */
NYA_INTERNAL ufbx_matrix _nya_asset_node_world_at(ufbx_anim* anim, ufbx_node* node, f64 time) {
    ufbx_matrix world = ufbx_identity_matrix;

    for (ufbx_node* step = node; step != nullptr; step = step->parent) {
        ufbx_transform local  = ufbx_evaluate_transform(anim, step, time);
        ufbx_matrix    matrix = ufbx_transform_to_matrix(&local);

        // Prepended, because walking upward visits children before parents and a parent applies first.
        world = ufbx_matrix_mul(&matrix, &world);
    }

    return world;
}

/**
 * The four strongest influences on one vertex, normalised. ufbx sorts a vertex's weights by
 * descending influence, so the first four are the ones that matter.
 *
 * Renormalising is not optional: ufbx does not guarantee weights sum to one, and on the test rig they
 * run as low as 0.982 — a vertex weighted to that sits nearly two percent of the way toward the
 * origin, reading as a dent in the mesh rather than a weighting bug.
 * */
NYA_INTERNAL void _nya_asset_mesh_vertex_weights(ufbx_mesh* mesh, const NYA_Skeleton* skeleton, u32 index, OUT u32* out_bones,
                                                 OUT f32* out_weights) {
    for (u32 i = 0; i < NYA_SKELETON_WEIGHTS_PER_VERTEX; i++) {
        out_bones[i]   = 0;
        out_weights[i] = 0.0F;
    }

    ufbx_skin_deformer* skin = mesh->skin_deformers.count > 0 ? mesh->skin_deformers.data[0] : nullptr;

    if (skin == nullptr || index >= mesh->vertex_indices.count) return;

    u32 vertex = mesh->vertex_indices.data[index];

    if (vertex >= skin->vertices.count) return;

    ufbx_skin_vertex entry = skin->vertices.data[vertex];

    u32 taken = 0;
    f32 total = 0.0F;

    for (u32 w = 0; w < entry.num_weights && taken < NYA_SKELETON_WEIGHTS_PER_VERTEX; w++) {
        ufbx_skin_weight weight = skin->weights.data[entry.weight_begin + w];

        // A cluster past the palette is one the skeleton dropped; skipping it rather than clamping
        // keeps the weight from being handed to an unrelated bone.
        if (weight.cluster_index >= skeleton->bone_count) continue;

        out_bones[taken]   = weight.cluster_index;
        out_weights[taken] = (f32)weight.weight;

        total += (f32)weight.weight;
        taken++;
    }

    /*
     * A vertex with no influence at all is pinned to bone zero at full weight. Leaving it at zero
     * would multiply the vertex by a zero matrix and collapse it to the origin — the single most
     * recognisable skinning artefact — whereas pinning it looks like a weighting mistake, not a tear.
     */
    if (total <= 0.0F) {
        out_bones[0]   = 0;
        out_weights[0] = 1.0F;
        return;
    }

    for (u32 i = 0; i < taken; i++) out_weights[i] /= total;
}


NYA_INTERNAL NYA_Error _nya_asset_build_mesh(NYA_AssetHandle handle, const u8* data, u64 size, NYA_Arrayᐸ_NYA_AssetPendingUploadᐳ* pending,
                                             OUT NYA_Asset* out_asset) {
    NYA_Arena* arena = nya_app_get()->asset_system.allocator;

    /*
     * Triangulated and space-converted by ufbx rather than afterwards. `target_axes` and
     * `target_unit_meters` matter because FBX carries its own idea of up and unit length, and
     * Blender/Maya/3ds Max each answer differently; converting here means every export arrives y-up
     * and in metres, with no per-model correction matrix to guess at.
     */
    ufbx_load_opts options = {
        .target_axes              = ufbx_axes_right_handed_y_up,
        .target_unit_meters       = 1.0F,
        .generate_missing_normals = true,
    };

    ufbx_error  error = { 0 };
    ufbx_scene* scene = ufbx_load_memory(data, size, &options, &error);

    if (scene == nullptr) return nya_error(NYA_ERROR_NOT_OK, "could not read the FBX: %s", error.description.data);

    defer ufbx_free_scene(scene);

    /*
     * Counted per instance and per material, matching what the write loop below iterates — a mesh
     * placed twice emits its triangles twice. num_triangles is what triangulation produces; the raw
     * face count is smaller for anything modelled in quads and would under-allocate.
     */
    u32 total = 0;
    u32 parts = 0;

    for (u64 i = 0; i < scene->meshes.count; i++) {
        ufbx_mesh* mesh = scene->meshes.data[i];

        u64 instances = mesh->instances.count > 0 ? mesh->instances.count : 1;

        total += (u32)(mesh->num_triangles * instances) * 3;

        // At least one part per instance: a mesh with no material still draws, as one untextured run.
        parts += (u32)(instances * (mesh->material_parts.count > 0 ? mesh->material_parts.count : 1));
    }

    if (total == 0 || parts == 0) return nya_error(NYA_ERROR_NOT_OK, "the FBX has no triangles");

    /*
     * One vertex per index, no welding — deliberate: two faces meeting at a hard edge need the same
     * position with different normals, so sharing by position alone would smooth every crease in the
     * model. The batch uploads what it is given each frame, so the cost is bytes, not draw calls.
     */
    f32x3* positions = nya_arena_alloc(arena, total * sizeof(f32x3));
    f32x3* normals   = nya_arena_alloc(arena, total * sizeof(f32x3));
    f32x2* uvs       = nya_arena_alloc(arena, total * sizeof(f32x2));

    // ── skinning ── extracted only when a mesh in the file actually carries a deformer, so an
    // unrigged model costs one pointer check and nothing else. See _nya_asset_mesh_skeleton.
    NYA_Skeleton* skeleton = _nya_asset_mesh_skeleton(arena, scene);

    u32* bone_indices = nullptr;
    f32* bone_weights = nullptr;

    if (skeleton != nullptr) {
        bone_indices = nya_arena_alloc(arena, total * NYA_SKELETON_WEIGHTS_PER_VERTEX * sizeof(u32));
        bone_weights = nya_arena_alloc(arena, total * NYA_SKELETON_WEIGHTS_PER_VERTEX * sizeof(f32));

        nya_memset(bone_indices, 0, total * NYA_SKELETON_WEIGHTS_PER_VERTEX * sizeof(u32));
        nya_memset(bone_weights, 0, total * NYA_SKELETON_WEIGHTS_PER_VERTEX * sizeof(f32));
    }

    NYA_MeshPart* mesh_parts = nya_arena_alloc(arena, parts * sizeof(NYA_MeshPart));

    /*
     * Textures are deduplicated by the ufbx material's texture pointer. Two materials in one file
     * commonly point at the same image — pill.fbx has its atlas bound as both a base colour and an
     * alpha source — and decoding it twice would upload the same megabyte twice. `sources` is the
     * dedupe key; `textures` is what the asset ends up owning, index for index.
     */
    SDL_GPUTexture** textures = nya_arena_alloc(arena, parts * sizeof(SDL_GPUTexture*));
    const void**     sources  = nya_arena_alloc(arena, parts * sizeof(const void*));

    u32 texture_count = 0;
    u32 written       = 0;
    u32 part_count    = 0;

    for (u64 m = 0; m < scene->meshes.count; m++) {
        ufbx_mesh* mesh = scene->meshes.data[m];

        u64  corners  = (u64)mesh->max_face_triangles * 3;
        u32* triangle = nya_arena_alloc(arena, corners * sizeof(u32));

        u64 instances = mesh->instances.count > 0 ? mesh->instances.count : 1;

        for (u64 n = 0; n < instances; n++) {
            /*
             * The node's transform, applied here rather than ignored. ufbx_mesh.vertex_position is in
             * the mesh's *own* space; the node carries where it sits, how it is turned, and how it is
             * scaled. Not hypothetical: pill.fbx is a capsule stretched along one axis by its node, and
             * reading the raw vertices produced a sphere where a capsule was expected.
             *
             * `ufbx_matrix_for_normals` is the inverse transpose — the correct transform for a normal,
             * differing from the position one once scale is non-uniform, since the position matrix on
             * a stretched model tilts every normal and lights it wrongly.
             */
            ufbx_matrix to_world     = ufbx_identity_matrix;
            ufbx_matrix normal_world = ufbx_identity_matrix;

            if (mesh->instances.count > 0) {
                to_world     = mesh->instances.data[n]->geometry_to_world;
                normal_world = ufbx_matrix_for_normals(&to_world);
            }

            u64 material_parts = mesh->material_parts.count > 0 ? mesh->material_parts.count : 1;

            for (u64 g = 0; g < material_parts; g++) {
                NYA_MeshPart* part = &mesh_parts[part_count];

                *part = (NYA_MeshPart){
                    .first_vertex = written,
                    .texture      = -1,
                    .base_color   = NYA_COLOR_WHITE,
                };

                /*
                 * The material for this run, and the faces that belong to it. ufbx has already grouped
                 * them: material_parts[g].face_indices lists exactly the faces using materials[g].
                 */
                ufbx_material* material = nullptr;

                if (mesh->material_parts.count > 0 && g < mesh->materials.count) material = mesh->materials.data[g];

                if (material != nullptr) {
                    ufbx_vec3 diffuse = material->fbx.diffuse_color.value_vec3;

                    part->base_color = (NYA_Color){ (f32)diffuse.x, (f32)diffuse.y, (f32)diffuse.z, 1.0F };

                    ufbx_texture* texture = material->fbx.diffuse_color.texture;

                    // pbr.base_color as the fallback: an exporter writing a modern material graph fills
                    // that in and may leave the legacy fbx.diffuse_color slot without a texture.
                    if (texture == nullptr) texture = material->pbr.base_color.texture;

                    // Any texture at all, embedded or not. This used to require `content.size > 0`,
                    // which quietly skipped every material whose exporter did not embed its images —
                    // the reason a re-exported model lost its texture without a word.
                    if (texture != nullptr) {
                        // Already decoded for an earlier part? Then share it rather than upload it twice.
                        s32 existing = -1;

                        for (u32 i = 0; i < texture_count; i++) {
                            if (sources[i] == (const void*)texture->content.data) existing = (s32)i;
                        }

                        if (existing >= 0) {
                            part->texture = existing;
                        } else {
                            /*
                             * The blob inside the FBX first, the file it points at second. Both are
                             * normal: "Embed textures" is an exporter checkbox, off leaves a path to a
                             * sidecar — conventionally a `<model>.fbm` directory beside it. This loader
                             * once read only the blob, and a re-exported model drew untextured with no
                             * warning; a material naming an unfindable texture now says so.
                             */
                            NYA_Arena* texture_arena = nya_arena_create(.name = "fbx_texture");
                            defer      nya_arena_destroy(texture_arena);

                            const u8* image      = texture->content.data;
                            u64       image_size = texture->content.size;

                            if (image_size == 0) {
                                // Resolved against the model's own directory and read through the asset
                                // system, so a bundled build finds it in the blob exactly as a loose
                                // build finds it on disk. `relative_filename` is relative to the FBX.
                                NYA_ConstCString relative =
                                    texture->relative_filename.length > 0 ? texture->relative_filename.data : texture->filename.data;

                                if (relative != nullptr && relative[0] != '\0') {
                                    // Everything up to and including the model's last slash, so a texture
                                    // named `Cubie.fbm/Cubie.png` lands beside the `.fbx` that named it.
                                    u64 directory_length = 0;

                                    for (u64 c = 0; handle[c] != '\0'; c++) {
                                        if (handle[c] == '/') directory_length = c + 1;
                                    }

                                    NYA_String* path = nya_string_sprintf(texture_arena, "%.*s%s", (s32)directory_length, handle, relative);

                                    u8* file      = nullptr;
                                    u64 file_size = 0;

                                    NYA_Error read = nya_asset_read(texture_arena, nya_string_to_cstring(texture_arena, path), &file, &file_size);

                                    if (read.ok) {
                                        image      = file;
                                        image_size = file_size;
                                    } else {
                                        nya_log_warn("The material in '%s' names the texture '%s', which is neither embedded nor readable at "
                                                 "'%s' (%s); that part will draw untextured.",
                                                 handle, texture->name.data, nya_string_to_cstring(texture_arena, path),
                                                 (NYA_ConstCString)read.message);
                                    }
                                } else {
                                    nya_log_warn("The material in '%s' names the texture '%s' with no embedded data and no path; that part will "
                                             "draw untextured.",
                                             handle, texture->name.data);
                                }
                            }

                            SDL_Surface* surface = image_size > 0 ? IMG_Load_IO(SDL_IOFromConstMem(image, image_size), true) : nullptr;

                            if (surface == nullptr) {
                                // Only worth a second warning when there were bytes to decode; the path
                                // above has already explained an absent image.
                                if (image_size > 0) {
                                    nya_log_warn("Could not decode the texture '%s' from '%s' (%s); that part will draw untextured.",
                                             texture->name.data, handle, SDL_GetError());
                                }
                            } else {
                                /*
                                 * Staged into a scratch asset: as_texture and as_mesh are the same union,
                                 * so writing into out_asset would land on top of the triangle pointers
                                 * assembled above.
                                 */
                                NYA_Asset staged = { 0 };

                                NYA_Error upload = _nya_asset_stage_texture(surface, pending, &staged);
                                SDL_DestroySurface(surface);

                                if (!upload.ok) {
                                    nya_log_warn("Could not upload a texture embedded in an FBX (%s); that part will draw untextured.",
                                             (NYA_ConstCString)upload.message);
                                } else {
                                    sources[texture_count]  = (const void*)texture->content.data;
                                    textures[texture_count] = staged.as_texture.texture;

                                    part->texture = (s32)texture_count;
                                    texture_count++;
                                }
                            }
                        }
                    }
                }

                u64 face_total = mesh->material_parts.count > 0 ? mesh->material_parts.data[g].face_indices.count : mesh->faces.count;

                for (u64 f = 0; f < face_total; f++) {
                    u32 face_index = mesh->material_parts.count > 0 ? mesh->material_parts.data[g].face_indices.data[f] : (u32)f;

                    // Triangulated face by face, into a buffer sized for the worst polygon in this mesh.
                    // Skipping this turns a quad-modelled asset — most of them — into a mesh full of holes.
                    u32 triangles = ufbx_triangulate_face(triangle, corners, mesh, mesh->faces.data[face_index]);

                    for (u32 corner = 0; corner < triangles * 3 && written < total; corner++) {
                        u32 vertex = triangle[corner];

                        ufbx_vec3 position = ufbx_get_vertex_vec3(&mesh->vertex_position, vertex);
                        ufbx_vec3 normal   = ufbx_get_vertex_vec3(&mesh->vertex_normal, vertex);

                        /*
                         * The UV, or zero when the model has no UV set. `exists` has to be checked
                         * rather than trusted: ufbx_get_vertex_vec2 on a missing attribute reads
                         * through a null indices array. V is flipped: FBX puts the origin at the
                         * bottom left, every GPU convention here puts it at the top left, so an
                         * unflipped sample arrives upside down.
                         */
                        ufbx_vec2 uv = { 0 };

                        if (mesh->vertex_uv.exists) {
                            uv   = ufbx_get_vertex_vec2(&mesh->vertex_uv, vertex);
                            uv.y = 1.0 - uv.y;
                        }

                        /*
                         * The node transform is baked in for a static mesh and *not* for a skinned one.
                         * A skinned vertex has to stay in the space its inverse bind matrix expects,
                         * which is the geometry space ufbx hands it over in — baking geometry_to_world
                         * in as well applies the node's placement twice, once here and once through the
                         * bone, and the model turns inside out on the first animated frame. The static
                         * path still needs it: pill.fbx is a capsule only because its node stretches it.
                         */
                        if (skeleton == nullptr) {
                            position = ufbx_transform_position(&to_world, position);
                            normal   = ufbx_transform_direction(&normal_world, normal);
                        }

                        if (skeleton != nullptr) {
                            _nya_asset_mesh_vertex_weights(mesh, skeleton, vertex, &bone_indices[(u64)written * NYA_SKELETON_WEIGHTS_PER_VERTEX],
                                                           &bone_weights[(u64)written * NYA_SKELETON_WEIGHTS_PER_VERTEX]);
                        }

                        positions[written] = (f32x3){ (f32)position.x, (f32)position.y, (f32)position.z };

                        // Renormalised, because a non-uniform node scale changes a unit normal's length.
                        normals[written] = nya_vector_normalize((f32x3){ (f32)normal.x, (f32)normal.y, (f32)normal.z });

                        uvs[written] = (f32x2){ (f32)uv.x, (f32)uv.y };

                        written++;
                    }
                }

                part->vertex_count = written - part->first_vertex;

                // An empty run is dropped rather than kept: a part with no triangles is a draw call that
                // binds a texture and renders nothing.
                if (part->vertex_count > 0) part_count++;
            }
        }

        nya_arena_free(arena, triangle, corners * sizeof(u32));
    }

    nya_arena_free(arena, sources, parts * sizeof(const void*));

    out_asset->as_mesh.positions     = positions;
    out_asset->as_mesh.normals       = normals;
    out_asset->as_mesh.uvs           = uvs;
    out_asset->as_mesh.vertex_count  = written;
    out_asset->as_mesh.allocated     = total;
    out_asset->as_mesh.parts         = mesh_parts;
    out_asset->as_mesh.part_count    = part_count;
    out_asset->as_mesh.part_capacity = parts;

    /*
     * The skeleton, and the vertices in the wider layout that goes with it. Built here rather than in
     * the write loop because it needs the final `written` count: the loop drops degenerate runs as it
     * finds them, so the array is sized for the worst case and only this far in is the real length
     * known.
     */
    out_asset->as_mesh.skeleton = skeleton;

    if (skeleton != nullptr && written > 0) {
        NYA_VertexSkinned3D* skinned = nya_arena_alloc(arena, (u64)written * sizeof(NYA_VertexSkinned3D));

        for (u32 v = 0; v < written; v++) {
            skinned[v] = (NYA_VertexSkinned3D){
                .position = positions[v],
                .color    = NYA_COLOR_WHITE,
                .normals  = normals[v],
                .uv       = uvs[v],
            };

            for (u32 w = 0; w < NYA_SKELETON_WEIGHTS_PER_VERTEX; w++) {
                skinned[v].bones[w]   = bone_indices[((u64)v * NYA_SKELETON_WEIGHTS_PER_VERTEX) + w];
                skinned[v].weights[w] = bone_weights[((u64)v * NYA_SKELETON_WEIGHTS_PER_VERTEX) + w];
            }
        }

        out_asset->as_mesh.skinned_vertices = skinned;
    }
    out_asset->as_mesh.textures      = textures;
    out_asset->as_mesh.texture_count = texture_count;

    nya_log_debug("Read %u triangles from an FBX across %llu meshes, %u parts and %u textures.", written / 3,
              (unsigned long long)scene->meshes.count, part_count, texture_count);

    return NYA_OK;
}

NYA_INTERNAL NYA_Error _nya_asset_load_raw(NYA_AssetHandle handle, b8 external, OUT NYA_Asset* out_asset) {
    nya_assert(handle != nullptr);
    nya_assert(out_asset != nullptr);

    out_asset->from_blob = false;

    // Takes the handle rather than reading it off the parameters, because a shader loads the
    // *compiled* artifact whose handle is derived from the one that was requested. An external
    // asset is a path from outside the game — something dropped on the window, or a file the player
    // picked — so it was never a candidate for the blob and goes straight to disk.
    if (external) return _nya_asset_load_raw_from_filesystem(handle, out_asset);

#ifdef NYA_ASSET_PREFER_BLOB
    // The blob is a cache in front of the filesystem rather than a replacement for it. A handle it
    // does not carry is not an error: the asset may have been added since the build, or deliberately
    // shipped loose beside the executable, and either way disk is the answer.
    NYA_Error blob_result = _nya_asset_load_raw_from_blob(handle, out_asset);
    if (blob_result.ok) {
        out_asset->from_blob = true;
        return NYA_OK;
    }
    if (blob_result.kind != NYA_ERROR_NOT_FOUND) return blob_result;
#endif

    return _nya_asset_load_raw_from_filesystem(handle, out_asset);
}

/** Takes an asset back out of the unloading queue, for when something acquires it before the queue runs. */
/*
 * A handle the asset system can keep, copied out of whoever supplied it. Interned once per asset:
 * nya_asset_load returns early for a handle it already knows, so a repeat load does not allocate a
 * second copy. Freed with the asset system's arena at shutdown.
 * */
NYA_INTERNAL NYA_AssetHandle _nya_asset_intern(NYA_AssetHandle handle) {
    nya_assert(handle != nullptr);

    NYA_Arena*  arena = nya_app_get()->asset_system.allocator;
    NYA_String* owned = nya_string_from(arena, handle);

    return nya_string_to_cstring(arena, owned);
}

NYA_INTERNAL void _nya_asset_cancel_queued_unload(NYA_Asset* asset) {
    NYA_AssetSystem* system = &nya_app_get()->asset_system;

    for (u64 i = 0; i < system->unloading_queue->length; i++) {
        if (!nya_string_equals(system->unloading_queue->items[i], asset->handle)) continue;

        nya_array_remove(system->unloading_queue, i);
        return;
    }
}

NYA_INTERNAL void _nya_asset_unload_raw(NYA_Asset* asset) {
    nya_assert(asset != nullptr);

    // Blob assets point straight into the executable's own data, so there is nothing to free; only
    // something that was read off disk owns its memory. Which of the two it was is recorded on the
    // asset, because with a blob present both kinds exist in the same build.
    if (asset->from_blob) return;

    _nya_asset_unload_raw_from_filesystem(asset);
}

#ifdef NYA_ASSET_HOT_RELOAD
NYA_INTERNAL b8 _nya_asset_get_modification_time(NYA_Asset* asset, OUT u64* out_modification_time) {
    NYA_Error result;

    switch (asset->type) {
        /*
         * Everything backed directly by one file on disk. Only text used to be here, and every other
         * file-backed type — fonts, textures, sounds — fell through to the default below, which
         * reports a modification time of zero. Zero is never greater than the time recorded at load,
         * so the comparison in nya_asset_get could never fire and those assets never hot reloaded at
         * all — nothing said so; the asset was polled every frame and simply always concluded nothing
         * had changed.
         *
         * Stat the *source* rather than the handle, since a handle need not be a path — one .ttf
         * loaded at two point sizes is two assets keyed on something other than the file name.
         */
        case NYA_ASSET_TYPE_TEXT:
        case NYA_ASSET_TYPE_FONT:
        case NYA_ASSET_TYPE_SOUND:
        case NYA_ASSET_TYPE_MESH:
        case NYA_ASSET_TYPE_TEXTURE: {
            NYA_AssetHandle path = asset->load_parameters.source != nullptr ? (NYA_AssetHandle)asset->load_parameters.source : asset->handle;

            result = nya_filesystem_last_modified(path, out_modification_time);
        } break;

        case NYA_ASSET_TYPE_SHADER_VERTEX:
        case NYA_ASSET_TYPE_SHADER_FRAGMENT: {
            result = nya_filesystem_last_modified(asset->as_shader.compiled_handle, out_modification_time);
        } break;

        case NYA_ASSET_TYPE_GRAPHICS_PIPELINE: {
            /*
             * Watched through the compiled artifacts its shaders were built from, not the .hlsl those
             * were compiled out of. Editing the source changes nothing a running process can use:
             * compiling is a build step, and until it runs the .spv on disk is still what the shader
             * modules hold. This used to watch load_parameters.*_shader_handle, the source path — so
             * touching a shader rebuilt the pipeline out of unchanged shader modules, which looks
             * exactly like hot reload doing nothing.
             *
             * nya_dict_get rather than nya_asset_get: the latter calls back into this to decide
             * whether to queue a reload, and would recurse.
             */
            NYA_AssetSystem* system = &nya_app_get()->asset_system;

            NYA_Asset* vertex   = nya_dict_get(system->assets, asset->load_parameters.as_graphics_pipeline.vertex_shader_handle);
            NYA_Asset* fragment = nya_dict_get(system->assets, asset->load_parameters.as_graphics_pipeline.fragment_shader_handle);

            // Mid load, or gone. Reporting no time rather than guessing keeps the pipeline out of
            // the reload queue until its shaders are actually there to be compared against.
            if (vertex == nullptr || fragment == nullptr || vertex->status != NYA_ASSET_STATUS_LOADED ||
                fragment->status != NYA_ASSET_STATUS_LOADED) {
                *out_modification_time = 0;
                return false;
            }

            u64 vertex_shader_modification_time   = 0;
            u64 fragment_shader_modification_time = 0;

            result = nya_filesystem_last_modified(vertex->as_shader.compiled_handle, &vertex_shader_modification_time);
            if (result.ok) {
                result = nya_filesystem_last_modified(fragment->as_shader.compiled_handle, &fragment_shader_modification_time);
            }

            if (result.ok) *out_modification_time = nya_max(vertex_shader_modification_time, fragment_shader_modification_time);
        } break;

        default: {
            *out_modification_time = 0;
            return true;
        } break;
    }

    if (!result.ok) nya_log_warn("Loaded asset missing from filesystem: %s", asset->handle);
    return result.ok;
}
#endif // NYA_ASSET_HOT_RELOAD

void _nya_asset_loading_process(NYA_Event* event) {
    // Runs on NYA_EVENT_FRAME_ENDED, so its cost lands inside the frame without appearing in any of
    // the frame_* timers. Decoding an image or a font is the classic invisible spike.
    nya_perf_time_this_function();

    nya_unused(event);

    NYA_AssetSystem*  system        = &nya_app_get()->asset_system;
    NYA_RenderSystem* render_system = &nya_app_get()->render_system;

    // Every texture decoded in this pass stages into here and is uploaded in a single copy pass at
    // the end, rather than each one opening and submitting a command buffer of its own.
    NYA_Arrayᐸ_NYA_AssetPendingUploadᐳ pending_uploads = nya_array_create_on_stack(system->allocator, _NYA_AssetPendingUpload);
    defer                              nya_array_destroy_on_stack(&pending_uploads);

    nya_array_foreach (system->loading_queue, parameters) {
        nya_log_debug("Loading asset: %s", parameters->handle);

        NYA_Asset* asset = nya_dict_get(system->assets, parameters->handle);
        nya_assert(asset != nullptr);

        switch (parameters->type) {
            case NYA_ASSET_TYPE_TEXT: {
                NYA_Error result = _nya_asset_load_raw(parameters->source != nullptr ? (NYA_AssetHandle)parameters->source : parameters->handle, parameters->external, asset);
                if (!result.ok) {
                    _nya_asset_fail(asset, &result);
                    break;
                }

                asset->type   = NYA_ASSET_TYPE_TEXT;
                asset->status = NYA_ASSET_STATUS_LOADED;
            } break;

                /*
                 * All three decode from memory rather than from a path. The raw bytes already went
                 * through _nya_asset_load_raw, which is what makes a blob asset, a filesystem asset and
                 * a dropped file take exactly the same route from here on.
                 */

            case NYA_ASSET_TYPE_MESH: {
                NYA_Error result = _nya_asset_load_raw(parameters->source != nullptr ? (NYA_AssetHandle)parameters->source : parameters->handle, parameters->external, asset);
                if (!result.ok) {
                    _nya_asset_fail(asset, &result);
                    break;
                }

                /*
                 * Built into a scratch asset rather than into this one, because as_mesh and as_text are
                 * the same union. Writing the mesh straight into `asset` overwrites as_text.data with a
                 * pointer to the positions array, and the _nya_asset_unload_raw below then frees *that*
                 * as though it were the file's bytes. It is not a subtle failure — ASan aborts inside
                 * the arena — but it only happens once the parse succeeds, so a loader tested solely on
                 * a malformed file would look correct.
                 */
                NYA_Asset built_mesh = { 0 };

                NYA_Error built = _nya_asset_build_mesh(parameters->handle, asset->as_text.data, asset->as_text.size, &pending_uploads, &built_mesh);

                // While as_text is still the file. Freed whether or not the parse succeeded: the
                // triangles are what the asset is now, and the FBX bytes are several hundred kilobytes
                // nothing will read again.
                _nya_asset_unload_raw(asset);

                if (!built.ok) {
                    _nya_asset_fail(asset, &built);
                    break;
                }

                asset->as_mesh = built_mesh.as_mesh;

                // Carried across from the load rather than read from the file; see as_mesh_load.filter.
                asset->as_mesh.filter = parameters->as_mesh_load.filter;

                asset->type   = NYA_ASSET_TYPE_MESH;
                asset->status = NYA_ASSET_STATUS_LOADED;
            } break;

            case NYA_ASSET_TYPE_TEXTURE: {
                NYA_Error result = _nya_asset_load_raw(parameters->source != nullptr ? (NYA_AssetHandle)parameters->source : parameters->handle, parameters->external, asset);
                if (!result.ok) {
                    _nya_asset_fail(asset, &result);
                    break;
                }

                u8* encoded      = asset->as_text.data;
                u64 encoded_size = asset->as_text.size;

                /*
                 * A requested size only means anything to a vector image, so it is tried as SVG first
                 * and falls back to the ordinary loader. Tried rather than sniffed: IMG_LoadSizedSVG_IO
                 * reports failure cleanly on anything that is not SVG, and detecting the format here
                 * would mean duplicating the sniffing SDL_image already does — and getting it wrong for
                 * the XML declaration and comment cases a real .svg often starts with.
                 */
                SDL_Surface* surface = nullptr;

                u32 requested_width  = parameters->as_texture_load.width;
                u32 requested_height = parameters->as_texture_load.height;

                if (requested_width > 0 && requested_height > 0) {
                    /*
                     * `currentColor` resolved before rasterising, because afterwards is too late: it
                     * resolves to black with no document to inherit from, and a black texture cannot
                     * be recoloured by a draw tint — a tint is a multiply. Substituting a literal here
                     * is what makes an icon set tintable, and defaulting it to white makes tinting the
                     * normal way to use one.
                     *
                     * A plain byte substitution over the source, not an XML rewrite: `currentColor` is
                     * a fixed keyword, it cannot appear as an element or attribute name, and the
                     * alternative is a DOM for a token replacement.
                     */
                    NYA_Arena scratch = nya_arena_create_on_stack(.name = "svg_recolor");
                    defer     nya_arena_destroy_on_stack(&scratch);

                    u8* svg_source      = encoded;
                    u64 svg_source_size = encoded_size;

                    NYA_Color svg_color = parameters->as_texture_load.svg_color;
                    if (svg_color.r == 0.0F && svg_color.g == 0.0F && svg_color.b == 0.0F && svg_color.a == 0.0F) {
                        svg_color = (NYA_Color){ 1.0F, 1.0F, 1.0F, 1.0F };
                    }

                    char replacement[8];
                    (void)snprintf(
                        replacement,
                        sizeof(replacement),
                        "#%02X%02X%02X",
                        (u32)(nya_clamp(svg_color.r, 0.0F, 1.0F) * 255.0F),
                        (u32)(nya_clamp(svg_color.g, 0.0F, 1.0F) * 255.0F),
                        (u32)(nya_clamp(svg_color.b, 0.0F, 1.0F) * 255.0F)
                    );

                    NYA_ConstCString needle        = "currentColor";
                    u64              needle_size   = strlen(needle);
                    u64              replaced_size = strlen(replacement);

                    // The replacement is shorter than the keyword, so the original size is always
                    // enough room and the output can be built in one pass with no growth check.
                    u8* rewritten = nya_arena_alloc(&scratch, encoded_size + 1);

                    u64 read  = 0;
                    u64 write = 0;

                    while (read < encoded_size) {
                        if (read + needle_size <= encoded_size && nya_memcmp(&encoded[read], needle, needle_size) == 0) {
                            nya_memcpy(&rewritten[write], replacement, replaced_size);

                            read  += needle_size;
                            write += replaced_size;
                            continue;
                        }

                        rewritten[write++] = encoded[read++];
                    }

                    if (write != encoded_size) {
                        svg_source      = rewritten;
                        svg_source_size = write;
                    }

                    surface = IMG_LoadSizedSVG_IO(SDL_IOFromConstMem(svg_source, svg_source_size), (int)requested_width, (int)requested_height);
                }

                if (surface == nullptr) surface = IMG_Load_IO(SDL_IOFromConstMem(encoded, encoded_size), true);
                _nya_asset_unload_raw(asset); // the encoded bytes are done with once decoded

                if (surface == nullptr) {
                    NYA_Error decode = nya_error(NYA_ERROR_CORRUPT, "could not decode image '%s': %s", parameters->handle, SDL_GetError());
                    _nya_asset_fail(asset, &decode);
                    break;
                }

                NYA_Error upload = _nya_asset_stage_texture(surface, &pending_uploads, asset);
                SDL_DestroySurface(surface);

                if (!upload.ok) {
                    _nya_asset_fail(asset, &upload);
                    break;
                }

                asset->type              = NYA_ASSET_TYPE_TEXTURE;
                asset->status            = NYA_ASSET_STATUS_LOADED;
                asset->as_texture.filter = parameters->as_texture_load.filter;
            } break;

            case NYA_ASSET_TYPE_SOUND: {
                NYA_Error result = _nya_asset_load_raw(parameters->source != nullptr ? (NYA_AssetHandle)parameters->source : parameters->handle, parameters->external, asset);
                if (!result.ok) {
                    _nya_asset_fail(asset, &result);
                    break;
                }

                // MIX_Audio keeps a reference to the stream for a non predecoded sound, so the
                // encoded bytes stay alive until the asset is unloaded.
                SDL_IOStream* stream = SDL_IOFromConstMem(asset->as_text.data, asset->as_text.size);
                MIX_Audio*    audio  = MIX_LoadAudio_IO(system->mixer, stream, parameters->as_sound.predecode, true);

                if (audio == nullptr) {
                    NYA_Error decode = nya_error(NYA_ERROR_CORRUPT, "could not decode audio '%s': %s", parameters->handle, SDL_GetError());
                    _nya_asset_fail(asset, &decode);
                    break;
                }

                asset->as_sound.audio = audio;
                asset->type           = NYA_ASSET_TYPE_SOUND;
                asset->status         = NYA_ASSET_STATUS_LOADED;
            } break;

            case NYA_ASSET_TYPE_FONT: {
                NYA_Error result = _nya_asset_load_raw(parameters->source != nullptr ? (NYA_AssetHandle)parameters->source : parameters->handle, parameters->external, asset);
                if (!result.ok) {
                    _nya_asset_fail(asset, &result);
                    break;
                }

                // A face has one size baked in, so a zero point size is a caller mistake rather
                // than a default worth guessing at.
                f32 point_size = parameters->as_font.point_size > 0.0F ? parameters->as_font.point_size : 16.0F;

                // TTF_Font reads glyphs out of the file lazily, so the bytes must outlive the open.
                SDL_IOStream* stream = SDL_IOFromConstMem(asset->as_text.data, asset->as_text.size);
                TTF_Font*     font   = TTF_OpenFontIO(stream, true, point_size);

                if (font == nullptr) {
                    NYA_Error decode = nya_error(NYA_ERROR_CORRUPT, "could not open font '%s': %s", parameters->handle, SDL_GetError());
                    _nya_asset_fail(asset, &decode);
                    break;
                }

                /*
                 * Light hinting, the right pairing for an anti-aliased rasteriser. This was NORMAL when
                 * the atlas thresholded coverage into a hard mask — with nothing to smooth what falls
                 * between pixels, a stem crossing a boundary snapped to one side and grid-fitting hard
                 * fixed that lopsidedness. The atlas keeps its anti-aliasing now, which removes the
                 * reason: normal hinting distorts outlines to force stems onto whole pixels, costing
                 * shape for a crispness the coverage already provides, and letterforms came out stiff
                 * and unevenly weighted. Light hinting straightens stems vertically and leaves the
                 * horizontal outlines alone — what every desktop UI renderer does for the same reason.
                 */
                TTF_SetFontHinting(font, TTF_HINTING_LIGHT);

                asset->as_font.font = font;
                asset->type         = NYA_ASSET_TYPE_FONT;
                asset->status       = NYA_ASSET_STATUS_LOADED;
            } break;

            case NYA_ASSET_TYPE_SHADER_VERTEX:
            case NYA_ASSET_TYPE_SHADER_FRAGMENT: {
                SDL_GPUShaderFormat format;
                NYA_AssetHandle     compiled_shader_handle = _nya_asset_pick_correct_compiled_shader(parameters->handle, &format);

                // Checked, like every other loader in this switch. Unchecked, a missing .spv reached
                // SDL_CreateGPUShader as a null pointer with a size of zero, which fails, which the
                // assertion below then turned into a dead process — for one shader that had not been
                // compiled yet.
                NYA_Error result = _nya_asset_load_raw(compiled_shader_handle, parameters->external, asset);
                if (!result.ok) {
                    _nya_asset_fail(asset, &result);
                    break;
                }

                SDL_GPUShaderStage stage;
                if (parameters->type == NYA_ASSET_TYPE_SHADER_VERTEX) {
                    stage = SDL_GPU_SHADERSTAGE_VERTEX;
                } else {
                    stage = SDL_GPU_SHADERSTAGE_FRAGMENT;
                }

                SDL_GPUShader* shader = SDL_CreateGPUShader(
                    render_system->gpu_device,
                    &(SDL_GPUShaderCreateInfo){
                        .code_size            = asset->as_text.size,
                        .code                 = asset->as_text.data,
                        .entrypoint           = "main",
                        .format               = format,
                        .stage                = stage,
                        .num_samplers         = parameters->as_shader.num_samplers,
                        .num_storage_textures = parameters->as_shader.num_storage_textures,
                        .num_storage_buffers  = parameters->as_shader.num_storage_buffers,
                        .num_uniform_buffers  = parameters->as_shader.num_uniform_buffers,
                    }
                );
                _nya_asset_unload_raw(asset); // unload raw text code

                if (shader == nullptr) {
                    NYA_Error compile = nya_error(NYA_ERROR_CORRUPT, "could not create shader '%s': %s", parameters->handle, SDL_GetError());
                    _nya_asset_fail(asset, &compile);
                    break;
                }

                // The requested type, not always the vertex one. This said SHADER_VERTEX
                // unconditionally, so every fragment shader was recorded as a vertex shader. The
                // stage handed to SDL above was always right, and every consumer of asset->type
                // happens to treat the two identically, which is the only reason it did not show.
                asset->type                      = parameters->type;
                asset->status                    = NYA_ASSET_STATUS_LOADED;
                asset->as_shader.compiled_handle = compiled_shader_handle;
                asset->as_shader.format          = format;
                asset->as_shader.shader          = shader;
            } break;

            case NYA_ASSET_TYPE_GRAPHICS_PIPELINE: {
                NYA_Asset* vertex_shader_asset   = nya_asset_get(parameters->as_graphics_pipeline.vertex_shader_handle);
                NYA_Asset* fragment_shader_asset = nya_asset_get(parameters->as_graphics_pipeline.fragment_shader_handle);
                nya_assert(vertex_shader_asset != nullptr);
                nya_assert(fragment_shader_asset != nullptr);

                /*
                 * That the shaders *exist* was the only thing checked here, and existing is not the same
                 * as having loaded. A shader that failed — no compiled binary in the format this backend
                 * wants, or one SDL refused — leaves an asset in the dictionary whose `shader` is null.
                 * That null went straight into SDL_CreateGPUGraphicsPipeline, which returns null, which
                 * the assertion below turned into an abort naming neither shader — the real cause was
                 * already in the log from the shader's own failure, several lines up, with nothing tying
                 * the two together. Checked here so the pipeline's failure names the shader that caused
                 * it, the one thing needed to tell a missing DXIL build from a rejected one.
                 */
                if (vertex_shader_asset->status != NYA_ASSET_STATUS_LOADED || vertex_shader_asset->as_shader.shader == nullptr) {
                    NYA_Error missing = nya_error(NYA_ERROR_NOT_OK, "its vertex shader '%s' did not load",
                                                  parameters->as_graphics_pipeline.vertex_shader_handle);

                    _nya_asset_fail(asset, &missing);
                    break;
                }

                if (fragment_shader_asset->status != NYA_ASSET_STATUS_LOADED || fragment_shader_asset->as_shader.shader == nullptr) {
                    NYA_Error missing = nya_error(NYA_ERROR_NOT_OK, "its fragment shader '%s' did not load",
                                                  parameters->as_graphics_pipeline.fragment_shader_handle);

                    _nya_asset_fail(asset, &missing);
                    break;
                }

                /*
                 * Straight alpha over the destination, matching what the fragment shaders in this
                 * tree emit. Built here rather than inline below because the whole struct has to be
                 * zeroed when blending is off: SDL reads these fields regardless, and a stale
                 * factor with enable_blend false is the kind of thing that works on one backend.
                 */
                SDL_GPUColorTargetBlendState blend_state = { 0 };

                if (parameters->as_graphics_pipeline.blend == NYA_BLEND_ADDITIVE) {
                    blend_state = (SDL_GPUColorTargetBlendState){
                        .enable_blend          = true,
                        .src_color_blendfactor = SDL_GPU_BLENDFACTOR_SRC_ALPHA,
                        // ONE, so this only ever adds. Overlapping glows saturate toward white rather
                        // than stacking into a dark blob the way alpha would.
                        .dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE,
                        .color_blend_op        = SDL_GPU_BLENDOP_ADD,
                        .src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE,
                        .dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE,
                        .alpha_blend_op        = SDL_GPU_BLENDOP_ADD,
                    };
                } else if (parameters->as_graphics_pipeline.blend == NYA_BLEND_MULTIPLY) {
                    blend_state = (SDL_GPUColorTargetBlendState){
                        .enable_blend = true,
                        // DST_COLOR / ZERO: the result is source times destination. A light map drawn
                        // this way darkens what it covers and leaves the bright parts alone.
                        .src_color_blendfactor = SDL_GPU_BLENDFACTOR_DST_COLOR,
                        .dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ZERO,
                        .color_blend_op        = SDL_GPU_BLENDOP_ADD,
                        // Alpha left alone. Multiplying it too would make a light map eat the
                        // target's opacity, which matters the moment it is a render texture.
                        .src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ZERO,
                        .dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE,
                        .alpha_blend_op        = SDL_GPU_BLENDOP_ADD,
                    };
                } else if (parameters->as_graphics_pipeline.blend != NYA_BLEND_NONE) {
                    blend_state = (SDL_GPUColorTargetBlendState){
                        .enable_blend           = true,
                        .src_color_blendfactor  = SDL_GPU_BLENDFACTOR_SRC_ALPHA,
                        .dst_color_blendfactor  = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA,
                        .color_blend_op         = SDL_GPU_BLENDOP_ADD,
                        // The destination is the opaque swapchain, so its alpha is meaningless to the
                        // compositor. ONE / ONE_MINUS_SRC_ALPHA rather than mirroring the colour
                        // factors keeps it sane anyway, for the day this renders into a texture that
                        // is later composited.
                        .src_alpha_blendfactor  = SDL_GPU_BLENDFACTOR_ONE,
                        .dst_alpha_blendfactor  = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA,
                        .alpha_blend_op         = SDL_GPU_BLENDOP_ADD,
                    };
                }

                /*
                 * The three layouts, chosen once rather than with a boolean per field. This was a pair
                 * of `is_2d ? a : b` ternaries repeated across three fields, fine while there were two
                 * layouts and no longer fine once there are three — the instanced one differs in the
                 * buffer *count* as well as the tables, and a ternary per field has no way to keep the
                 * three answers agreeing with each other.
                 */
                const SDL_GPUVertexBufferDescription* buffer_descriptions = &vertex_buffer_description;
                const SDL_GPUVertexAttribute*         attributes          = vertex_attributes;

                u32 buffer_description_count = 1;
                u32 attribute_count          = (u32)nya_carray_length(vertex_attributes);

                switch (parameters->as_graphics_pipeline.vertex_layout) {
                    case NYA_VERTEX_LAYOUT_2D: {
                        buffer_descriptions = &vertex_buffer_description_2d;
                        attributes          = vertex_attributes_2d;
                        attribute_count     = (u32)nya_carray_length(vertex_attributes_2d);
                    } break;

                    case NYA_VERTEX_LAYOUT_3D_INSTANCED: {
                        buffer_descriptions      = vertex_buffer_descriptions_3d_instanced;
                        buffer_description_count = (u32)nya_carray_length(vertex_buffer_descriptions_3d_instanced);
                        attributes               = vertex_attributes_3d_instanced;
                        attribute_count          = (u32)nya_carray_length(vertex_attributes_3d_instanced);
                    } break;

                    case NYA_VERTEX_LAYOUT_3D_SKINNED: {
                        buffer_descriptions = &vertex_buffer_description_3d_skinned;
                        attributes          = vertex_attributes_3d_skinned;
                        attribute_count     = (u32)nya_carray_length(vertex_attributes_3d_skinned);
                    } break;

                    case NYA_VERTEX_LAYOUT_3D:
                    case NYA_VERTEX_LAYOUT_COUNT:
                    default: break;
                }

                SDL_GPUGraphicsPipelineCreateInfo pipelineCreateInfo = {
          .target_info = {
            // Declared whenever the pipeline tests or writes depth. A pipeline that says it has a
            // depth target and a pass that has none is a validation failure at draw time, and the
            // reverse — a pass with depth and a pipeline without — is the ordinary 2D case and fine.
            .has_depth_stencil_target  = parameters->as_graphics_pipeline.depth_test || parameters->as_graphics_pipeline.depth_write,
            .depth_stencil_format      = render_system->depth_format,
            .num_color_targets = 1,
            .color_target_descriptions = (SDL_GPUColorTargetDescription[]){
              {
                // The named format when there is one; the window's otherwise. See color_format.
                .format = parameters->as_graphics_pipeline.color_format != 0
                            ? parameters->as_graphics_pipeline.color_format
                            : SDL_GetGPUSwapchainTextureFormat(render_system->gpu_device, parameters->as_graphics_pipeline.window->sdl_window),
                .blend_state = blend_state,
              },
            },
          },
          .primitive_type                                = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST,
          .vertex_shader                                 = vertex_shader_asset->as_shader.shader,
          .fragment_shader                               = fragment_shader_asset->as_shader.shader,
          // Must match every target this pipeline draws into. A pipeline built for one sample count
          // cannot render to a target with another, which is why there is a single value for the
          // whole renderer rather than one per pipeline.
          .multisample_state.sample_count                = parameters->as_graphics_pipeline.single_sampled ? SDL_GPU_SAMPLECOUNT_1
                                                                                                            : render_system->sample_count,
          .rasterizer_state.fill_mode                    = SDL_GPU_FILLMODE_FILL,
          // Counter-clockwise is front, matching the winding the 3D primitives are emitted in.
          .rasterizer_state.cull_mode                    = parameters->as_graphics_pipeline.cull_back_faces  ? SDL_GPU_CULLMODE_BACK
                                                           : parameters->as_graphics_pipeline.cull_front_faces ? SDL_GPU_CULLMODE_FRONT
                                                                                                               : SDL_GPU_CULLMODE_NONE,
          .rasterizer_state.front_face                   = SDL_GPU_FRONTFACE_COUNTER_CLOCKWISE,
          /*
           * LESS, not LESS_OR_EQUAL: a fragment at exactly the stored depth is the same surface drawn
           * twice, and letting the second one through is what makes coplanar geometry flicker as the
           * camera moves. The whole block is written whether or not depth testing is on, because SDL
           * reads these fields regardless — the same reason the blend state above is zeroed rather
           * than left stale when blending is off.
           */
          .depth_stencil_state = {
            .compare_op         = SDL_GPU_COMPAREOP_LESS,
            .enable_depth_test  = parameters->as_graphics_pipeline.depth_test,
            .enable_depth_write = parameters->as_graphics_pipeline.depth_write,
          },
          .vertex_input_state.num_vertex_buffers         = buffer_description_count,
          .vertex_input_state.vertex_buffer_descriptions = buffer_descriptions,
          // Every attribute the chosen layout declares, not a hardcoded count. This used to say two
          // while NYA_Vertex3D carried four, so a shader reading TEXCOORD0 got whatever was in the
          // register — no textured or text pipeline could work until it matched.
          .vertex_input_state.num_vertex_attributes      = attribute_count,
          .vertex_input_state.vertex_attributes          = attributes,
        };
                SDL_GPUGraphicsPipeline* pipeline = SDL_CreateGPUGraphicsPipeline(render_system->gpu_device, &pipelineCreateInfo);

                /*
                 * A failure here is reported, not asserted. This used to be a bare
                 * `nya_assert(pipeline != nullptr)`, which aborts with neither the pipeline's name nor
                 * SDL's reason for refusing it — and pipeline creation is exactly where the backends
                 * disagree: a shader Vulkan accepts can be rejected by D3D12 over a sample count, a
                 * target format or a vertex layout the other one tolerated, so the failure shows up on
                 * one machine and one driver with nothing to go on. Failing the asset instead puts the
                 * handle and SDL_GetError in the log and lets everything else load — what draws through
                 * this pipeline then draws nothing, a scene missing a pass rather than a program that
                 * will not start, and the log says which pass.
                 */
                if (pipeline == nullptr) {
                    NYA_Error refused = nya_error(NYA_ERROR_NOT_OK, "SDL would not create the graphics pipeline: %s", SDL_GetError());

                    _nya_asset_fail(asset, &refused);
                    break;
                }

                asset->type                          = NYA_ASSET_TYPE_GRAPHICS_PIPELINE;
                asset->status                        = NYA_ASSET_STATUS_LOADED;
                asset->as_graphics_pipeline.pipeline = pipeline;
            } break;

            default: {
                NYA_Error unsupported =
                    nya_error(NYA_ERROR_NOT_SUPPORTED, "no loader for asset type %d ('%s')", parameters->type, parameters->handle);
                _nya_asset_fail(asset, &unsupported);
            } break;
        }

#ifdef NYA_ASSET_HOT_RELOAD
        /*
         * The watch baseline, taken once the asset is loaded and therefore knows what it depends on.
         * Only the filesystem loader used to set this, so anything not built from a file of its own
         * kept a baseline of zero — a graphics pipeline in particular, assembled from shaders already
         * loaded. Any nonzero timestamp then looked newer than zero, so every pipeline queued itself
         * for reload on the first frame and rebuilt once for nothing.
         */
        if (asset->status == NYA_ASSET_STATUS_LOADED && !asset->from_blob) {
            _nya_asset_get_modification_time(asset, &asset->source_modification_time);
        }
#endif
    }

    // One copy pass for everything the loop staged. Assets are already marked loaded above, which is
    // safe because this runs on NYA_EVENT_FRAME_ENDED: nothing samples a texture between here and
    // the next frame's rendering.
    _nya_asset_flush_uploads(&pending_uploads);

    nya_array_clear(system->loading_queue);
}

void _nya_asset_unloading_process(NYA_Event* event) {
    nya_unused(event);

    NYA_AssetSystem*  system        = &nya_app_get()->asset_system;
    NYA_RenderSystem* render_system = &nya_app_get()->render_system;

    nya_array_foreach (system->unloading_queue, handle) {
        nya_log_debug("Unloading asset: %s", *handle);

        NYA_Asset* asset = nya_dict_get(system->assets, *handle);
        if (asset == nullptr) continue;

        asset->queued_for_unload = false;

        // Reacquired between queueing and now. The acquire removes it from the queue, but a second
        // entry from an earlier round could still be sitting here.
        if (atomic_load(&asset->reference_count) > 0) continue;
        if (asset->status == NYA_ASSET_STATUS_UNLOADED) continue;

        switch (asset->type) {
            case NYA_ASSET_TYPE_TEXT: {
                _nya_asset_unload_raw(asset);
                asset->status = NYA_ASSET_STATUS_UNLOADED;
            } break;

            case NYA_ASSET_TYPE_MESH: {
                // The FBX bytes were released at the end of the load; what is left is the three arrays
                // built from them, which were allocated together and go together.
                /*
                 * The GPU textures go first, while the array that names them is still there. Once each,
                 * which is what the deduplicated array buys: parts refer to textures by index precisely
                 * so this loop can release every distinct one exactly once, where releasing through the
                 * parts would double-free an image two materials share.
                 *
                 * Ordering matters and got this wrong once: the free below nulls `textures`, so running
                 * this loop after it dereferenced a null array with a still non-zero count — a segfault
                 * on the ordinary quit path. It survived every test because the headless suite has no
                 * GPU and therefore no textures to release, and survived every manual run because those
                 * were killed rather than quit.
                 */
                for (u32 i = 0; i < asset->as_mesh.texture_count; i++) {
                    SDL_ReleaseGPUTexture(render_system->gpu_device, asset->as_mesh.textures[i]);
                }

                /*
                 * The retained vertex buffer, if anything ever drew this mesh. Created by the renderer
                 * on first use rather than here — see NYA_Asset.as_mesh.gpu_vertices — but released
                 * here, because unload is the one place that knows the asset is going away. A hot
                 * reload runs unload then load, so without this each reload would strand a buffer the
                 * size of the model in VRAM.
                 */
                if (asset->as_mesh.gpu_vertices != nullptr) {
                    SDL_ReleaseGPUBuffer(render_system->gpu_device, asset->as_mesh.gpu_vertices);

                    asset->as_mesh.gpu_vertices     = nullptr;
                    asset->as_mesh.gpu_vertex_count = 0;
                }

                if (asset->as_mesh.positions != nullptr) {
                    // `allocated`, not the counts: the arena frees by extent and the two can differ. See
                    // the note on NYA_Asset.as_mesh.allocated.
                    u32 reserved = asset->as_mesh.allocated;

                    nya_arena_free(system->allocator, asset->as_mesh.positions, reserved * sizeof(f32x3));
                    nya_arena_free(system->allocator, asset->as_mesh.normals, reserved * sizeof(f32x3));
                    nya_arena_free(system->allocator, asset->as_mesh.uvs, reserved * sizeof(f32x2));
                    nya_arena_free(system->allocator, asset->as_mesh.parts, asset->as_mesh.part_capacity * sizeof(NYA_MeshPart));
                    nya_arena_free(system->allocator, asset->as_mesh.textures, asset->as_mesh.part_capacity * sizeof(SDL_GPUTexture*));

                    asset->as_mesh.positions = nullptr;
                    asset->as_mesh.normals   = nullptr;
                    asset->as_mesh.uvs       = nullptr;
                    asset->as_mesh.parts     = nullptr;
                    asset->as_mesh.textures  = nullptr;
                }

                asset->as_mesh.vertex_count  = 0;
                asset->as_mesh.allocated     = 0;
                asset->as_mesh.part_count    = 0;
                asset->as_mesh.part_capacity = 0;
                asset->as_mesh.texture_count = 0;
                asset->status               = NYA_ASSET_STATUS_UNLOADED;
            } break;

            case NYA_ASSET_TYPE_SHADER_VERTEX:
            case NYA_ASSET_TYPE_SHADER_FRAGMENT: {
                SDL_ReleaseGPUShader(render_system->gpu_device, asset->as_shader.shader);
                asset->as_shader.shader = nullptr;

                // The +1 matches the allocation in _nya_asset_pick_correct_compiled_shader, which
                // reserves room for the terminator. Freeing one byte short hands the arena a block
                // that does not line up with the one it gave out.
                nya_arena_free(system->allocator, (void*)asset->as_shader.compiled_handle, strlen(asset->as_shader.compiled_handle) + 1);

                // Cleared rather than left pointing at freed memory: a reload loads the shader
                // again and picks a fresh handle, and nothing should be able to read this one in
                // between.
                asset->as_shader.compiled_handle = nullptr;

                asset->status = NYA_ASSET_STATUS_UNLOADED;
            } break;

            case NYA_ASSET_TYPE_GRAPHICS_PIPELINE: {
                SDL_ReleaseGPUGraphicsPipeline(render_system->gpu_device, asset->as_graphics_pipeline.pipeline);
                asset->status = NYA_ASSET_STATUS_UNLOADED;
            } break;

            case NYA_ASSET_TYPE_TEXTURE: {
                // The encoded bytes were already released once the pixels reached the GPU.
                SDL_ReleaseGPUTexture(render_system->gpu_device, asset->as_texture.texture);
                asset->as_texture = (typeof(asset->as_texture)){ 0 };
                asset->status     = NYA_ASSET_STATUS_UNLOADED;
            } break;

            case NYA_ASSET_TYPE_SOUND: {
                // Destroy the audio before the bytes: a non predecoded sound still reads from them.
                MIX_DestroyAudio(asset->as_sound.audio);
                asset->as_sound.audio = nullptr;
                _nya_asset_unload_raw(asset);
                asset->status = NYA_ASSET_STATUS_UNLOADED;
            } break;

            case NYA_ASSET_TYPE_FONT: {
                // Same ordering: the face reads glyphs out of the file lazily.
                TTF_CloseFont(asset->as_font.font);
                asset->as_font.font = nullptr;
                _nya_asset_unload_raw(asset);
                asset->status = NYA_ASSET_STATUS_UNLOADED;
            } break;

            default: {
                // Includes anything that never finished loading, which has nothing to release.
                asset->status = NYA_ASSET_STATUS_UNLOADED;
            } break;
        }
    }

    nya_array_clear(system->unloading_queue);
}

#ifdef NYA_ASSET_HOT_RELOAD
void _nya_asset_reload_process(NYA_Event* event) {
    nya_unused(event);
    NYA_AssetSystem* system = &nya_app_get()->asset_system;

    NYA_ArrayᐸNYA_AssetHandleᐳ postponed = nya_array_create_on_stack(system->allocator, NYA_AssetHandle);

    nya_array_foreach (system->reload_queue, handle) {
        NYA_Asset* asset = nya_dict_get(system->assets, *handle);
        nya_assert(asset != nullptr);

        // Nothing queues a blob asset, but a pipeline can be pulled in behind a shader that was
        // itself blob backed, so the queue is not only what nya_asset_get put there.
        if (asset->from_blob) continue;

        u64 file_modification_time = 0;
        _nya_asset_get_modification_time(asset, &file_modification_time);

        if (asset->reload_grace_frames > 0) {
            nya_log_debug("Postponing reload of asset (grace period): %s", *handle);
            asset->reload_grace_frames--;
            nya_array_push_back(&postponed, *handle);
            continue;
        }

        if (file_modification_time != asset->source_modification_time) {
            nya_log_debug("Postponing reload of asset (still written to): %s", *handle);
            asset->source_modification_time = file_modification_time;
            nya_array_push_back(&postponed, *handle);
        } else {
            nya_log_debug("Reloading asset: %s", *handle);

            /*
             * A pipeline holds the shader modules its shaders created, so those have to be rebuilt
             * from the new bytes before the pipeline is, or it binds the same modules again and the
             * reload changes nothing visible. Queued ahead of the pipeline because both queues are
             * drained in order on the next frame — unloading first, then loading — so the shaders are
             * recreated by the time the pipeline asks them for their SDL_GPUShader.
             *
             * Nothing else notices a shader change on its own: reload detection lives in
             * nya_asset_get, and once a pipeline exists the game fetches the pipeline rather than
             * the shaders behind it, so a shader asset is never polled.
             */
            if (asset->type == NYA_ASSET_TYPE_GRAPHICS_PIPELINE) {
                NYA_AssetHandle shader_handles[] = {
                    asset->load_parameters.as_graphics_pipeline.vertex_shader_handle,
                    asset->load_parameters.as_graphics_pipeline.fragment_shader_handle,
                };

                for (u64 i = 0; i < sizeof(shader_handles) / sizeof(shader_handles[0]); i++) {
                    NYA_Asset* shader = nya_dict_get(system->assets, shader_handles[i]);
                    if (shader == nullptr || shader->status != NYA_ASSET_STATUS_LOADED || shader->from_blob) continue;

                    nya_log_debug("Also reloading the shader behind it: %s", shader_handles[i]);
                    nya_array_push_back(system->unloading_queue, shader->handle);
                    nya_array_push_back(system->loading_queue, shader->load_parameters);
                }
            }

            nya_array_push_back(system->unloading_queue, *handle);
            nya_array_push_back(system->loading_queue, asset->load_parameters);

            if (asset->type == NYA_ASSET_TYPE_SHADER_VERTEX || asset->type == NYA_ASSET_TYPE_SHADER_FRAGMENT) {
                nya_dict_foreach_value(system->assets, other_asset) {
                    if (other_asset->type != NYA_ASSET_TYPE_GRAPHICS_PIPELINE) continue;

                    if (other_asset->load_parameters.as_graphics_pipeline.vertex_shader_handle == asset->handle ||
                        other_asset->load_parameters.as_graphics_pipeline.fragment_shader_handle == asset->handle) {
                        if (!nya_array_contains(system->reload_queue, other_asset->handle)) {
                            nya_array_push_back(&postponed, other_asset->handle);
                            nya_log_debug("Also marking graphics pipeline for reload due to shader modification: %s", other_asset->handle);
                        }
                    }
                }
            }
        }
    }

    nya_array_clear(system->reload_queue);

    nya_array_foreach (&postponed, handle) nya_array_push_back(system->reload_queue, *handle);
    nya_array_destroy_on_stack(&postponed);
}
#endif // NYA_ASSET_HOT_RELOAD

NYA_INTERNAL NYA_AssetHandle _nya_asset_pick_correct_compiled_shader(NYA_AssetHandle source_shader, OUT SDL_GPUShaderFormat* out_format) {
    nya_assert(nya_string_contains(source_shader, "/shader/source/"));

    NYA_AssetSystem* system = &nya_app_get()->asset_system;

    NYA_String* compiled_shader_path = nya_string_from(system->allocator, source_shader);
    nya_string_replace(compiled_shader_path, "/shader/source/", "/shader/compiled/");
    nya_string_strip_suffix(compiled_shader_path, ".hlsl");

    switch (NYA_OS_CURRENT) {
        case NYA_OS_WINDOWS: {
            nya_string_extend(compiled_shader_path, ".dxil");
            *out_format = SDL_GPU_SHADERFORMAT_DXIL;
        } break;

        case NYA_OS_LINUX: {
            nya_string_extend(compiled_shader_path, ".spv");
            *out_format = SDL_GPU_SHADERFORMAT_SPIRV;
        } break;

        case NYA_OS_MAC: {
            nya_string_extend(compiled_shader_path, ".msl");
            *out_format = SDL_GPU_SHADERFORMAT_MSL;
        } break;

        default: {
            nya_log_panic("Unsupported OS for picking compiled shader: %d", NYA_OS_CURRENT);
        } break;
    }

    NYA_CString handle = nya_arena_alloc(system->allocator, compiled_shader_path->length + 1);
    nya_memcpy(handle, compiled_shader_path->items, compiled_shader_path->length);
    handle[compiled_shader_path->length] = '\0';

    nya_string_destroy(compiled_shader_path);

    return handle;
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * ENUMERATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/** Suffix test over C strings, since nya_string_ends_with wants an NYA_String and both callers have neither. */
NYA_INTERNAL b8 _nya_asset_path_has_suffix(NYA_ConstCString path, NYA_ConstCString suffix) {
    if (suffix == nullptr || suffix[0] == '\0') return true;
    if (path == nullptr) return false;

    u64 path_length   = strlen(path);
    u64 suffix_length = strlen(suffix);

    if (suffix_length > path_length) return false;

    return nya_memcmp(path + path_length - suffix_length, suffix, suffix_length) == 0;
}

#ifndef NYA_ASSET_PREFER_BLOB
/*
 * The disk walk's helpers, compiled only when there is a disk walk. A build with NYA_ASSET_PREFER_BLOB
 * reads the baked index instead and never calls these, and an unused static function is a warning —
 * the compiler correctly pointing out that the guard belongs here rather than only around the call.
 */
typedef struct {
    NYA_ArrayᐸNYA_Stringᐳ* paths;
    NYA_ConstCString       suffix;
} _NYA_AssetEnumerateContext;

NYA_INTERNAL b8 _nya_asset_enumerate_collect(NYA_ConstCString path, const NYA_DirectoryEntry* entry, void* user_data) {
    _NYA_AssetEnumerateContext* context = user_data;

    if (entry->type != NYA_FILE_TYPE_FILE) return true;
    if (!_nya_asset_path_has_suffix(path, context->suffix)) return true;

    NYA_String* copy = nya_string_from(context->paths->arena, path);

    // The handles the loader takes are spelled with a leading "./", and so are the blob's, so a path
    // from a disk walk has to match — otherwise the two builds hand back names that differ by two
    // characters and only one of them loads.
    if (!nya_string_starts_with(copy, "./")) nya_string_extend_front(copy, "./");

    nya_array_push_back(context->paths, *copy);

    return true;
}

NYA_INTERNAL s32 _nya_asset_enumerate_compare(const NYA_String* a, const NYA_String* b) {
    u64 shortest = a->length < b->length ? a->length : b->length;

    for (u64 i = 0; i < shortest; i++) {
        if (a->items[i] != b->items[i]) return a->items[i] < b->items[i] ? -1 : 1;
    }

    if (a->length == b->length) return 0;

    return a->length < b->length ? -1 : 1;
}
#endif // NYA_ASSET_PREFER_BLOB

NYA_ArrayᐸNYA_Stringᐳ* nya_asset_enumerate(NYA_Arena* arena, NYA_ConstCString suffix) {
    nya_assert(arena != nullptr);

    NYA_ArrayᐸNYA_Stringᐳ* paths = nya_array_create(arena, NYA_String);

#ifdef NYA_ASSET_PREFER_BLOB
    // The baked index. Already sorted, since the generator sorts before it writes.
    for (u64 i = 0; i < NYA_ASSET_BLOB_HEADER_COUNT; i++) {
        NYA_ConstCString path = NYA_ASSET_BLOB_HEADER[i].path;

        if (!_nya_asset_path_has_suffix(path, suffix)) continue;

        NYA_String* copy = nya_string_from(arena, path);
        nya_array_push_back(paths, *copy);
    }
#else
    _NYA_AssetEnumerateContext context = { .paths = paths, .suffix = suffix };

    // A missing assets directory is not fatal: a test or a headless tool may have no assets at all,
    // and an empty list is the honest answer rather than a reason to stop.
    NYA_Error walked = nya_filesystem_walk(arena, "./assets", _nya_asset_enumerate_collect, &context);

    if (!walked.ok) nya_log_warn("Could not enumerate assets: %s", walked.message);

    nya_array_sort(paths, _nya_asset_enumerate_compare);
#endif

    return paths;
}

/*
 * ─────────────────────────────────────────────────────────
 * THE BAKED INDEX
 * ─────────────────────────────────────────────────────────
 */

u64 nya_asset_blob_count(void) {
#ifdef NYA_ASSET_PREFER_BLOB
    return NYA_ASSET_BLOB_HEADER_COUNT;
#else
    // No blob in this build, so the index is empty rather than absent. A caller iterating it does
    // nothing, which is the right behaviour for a tool that runs in both.
    return 0;
#endif
}

const NYA_AssetBlobHeader* nya_asset_blob_at(u64 index) {
#ifdef NYA_ASSET_PREFER_BLOB
    if (index >= NYA_ASSET_BLOB_HEADER_COUNT) return nullptr;

    return &NYA_ASSET_BLOB_HEADER[index];
#else
    nya_unused(index);

    return nullptr;
#endif
}

const NYA_AssetBlobHeader* nya_asset_blob_find(NYA_ConstCString path) {
    if (path == nullptr) return nullptr;

#ifdef NYA_ASSET_PREFER_BLOB
    /*
     * Linear, deliberately: the loader's own lookup is the hot one and already exists; this is for
     * tooling that walks the whole index anyway, so a map built at startup would cost every build a
     * hash table to serve a path nothing takes.
     */
    for (u64 i = 0; i < NYA_ASSET_BLOB_HEADER_COUNT; i++) {
        if (nya_string_equals(NYA_ASSET_BLOB_HEADER[i].path, path)) return &NYA_ASSET_BLOB_HEADER[i];
    }
#endif

    return nullptr;
}
