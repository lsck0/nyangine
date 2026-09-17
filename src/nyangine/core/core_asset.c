#include "SDL3/SDL_gpu.h"

#include "nyangine/nyangine.h"

// the FBX reader, linked as an archive; see vendor_ufbx.h.
#include "ufbx.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API DECLRARATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/*
 * The filesystem is always there, and the embedded blob is an optional layer in front of it.
 * NYA_ASSET_PREFER_BLOB consults src/generated/assets.c first; anything missing still comes off disk.
 * NYA_ASSET_HOT_RELOAD watches disk files only, since a blob asset has no file.
 */
#ifdef NYA_ASSET_PREFER_BLOB
#include "generated/assets.c"

NYA_INTERNAL NYA_Error _nya_asset_load_raw_from_blob(NYA_AssetHandle path, OUT NYA_Asset* out_asset);
#endif // NYA_ASSET_PREFER_BLOB

/**
 * Handles the memo in front of the asset dictionary holds, least recently used dropped first. gnyame's menu
 * looks up 27 distinct handles over a run, and the generated index has 1428, so this leaves room for a busy
 * scene while a cold handle only costs a dictionary lookup.
 * */
#ifndef NYA_ASSET_LOOKUP_CAPACITY
#define NYA_ASSET_LOOKUP_CAPACITY 256
#endif

/** The longest handle the memo keeps. Longer ones go to the dictionary every time. */
#ifndef NYA_ASSET_LOOKUP_HANDLE_MAX
#define NYA_ASSET_LOOKUP_HANDLE_MAX 128
#endif

/** Handle text to the NYA_Asset* the dictionary returned, which may be null. */
NYA_INTERNAL NYA_Cache* _nya_asset_lookup = nullptr;

/** The memo's tag. A rehash moves every NYA_Asset*, so a new generation makes every older entry stale. */
NYA_INTERNAL u64 _nya_asset_lookup_generation = 1;

/** Drops every memoised handle lookup. */
NYA_INTERNAL void _nya_asset_lookup_invalidate(void);

/**
 * Stores a dictionary answer in the memo. Cold and out of line: the NYA_Error it checks would give nya_asset_get a
 * kilobyte frame and a stack protector on every hit.
 * */
__attr_cold NYA_INTERNAL void _nya_asset_lookup_remember(NYA_AssetHandle handle, u64 handle_length, NYA_Asset* asset);

/** The last NYA_Asset.generation handed out. Never reused, so an unload that zeroes an asset cannot repeat one. */
NYA_INTERNAL u64 _nya_asset_generation_last = 0;

NYA_INTERNAL NYA_Error _nya_asset_load_raw_from_filesystem(NYA_AssetHandle path, OUT NYA_Asset* out_asset);
NYA_INTERNAL void      _nya_asset_unload_raw_from_filesystem(NYA_Asset* asset);

/** Blob first when there is one, disk otherwise, and always disk for an external asset. */
NYA_INTERNAL NYA_Error _nya_asset_load_raw(NYA_AssetHandle handle, b8 external, OUT NYA_Asset* out_asset);

NYA_INTERNAL void      _nya_asset_fail(NYA_Asset* asset, const NYA_Error* error);
/**
 * A texture with its pixels staged, waiting for the frame's copy pass. The transfer buffers outlive
 * that pass, so they are released together after the single submit.
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
 * Turns FBX bytes into NYA_Asset.as_mesh. Takes the pending-upload list so the material's texture goes
 * through the frame's single copy pass; see _nya_asset_flush_uploads.
 * */
NYA_INTERNAL NYA_Error _nya_asset_build_mesh(NYA_AssetHandle handle, const u8* data, u64 size, NYA_Arrayᐸ_NYA_AssetPendingUploadᐳ* pending,
                                             OUT NYA_Asset* out_asset);

/** Runs every staged upload in one copy pass, then releases the transfer buffers. */
NYA_INTERNAL void _nya_asset_flush_uploads(NYA_Arrayᐸ_NYA_AssetPendingUploadᐳ* pending);
NYA_INTERNAL void      _nya_asset_unload_raw(NYA_Asset* asset);
NYA_INTERNAL void      _nya_asset_cancel_queued_unload(NYA_Asset* asset);

/** Copies a caller supplied handle into the asset system's own memory. */
NYA_INTERNAL NYA_AssetHandle _nya_asset_intern(NYA_AssetHandle handle) __attr_no_discard;

#ifdef NYA_ASSET_HOT_RELOAD
#define _NYA_ASSET_RELOAD_GRACE_FRAMES 5

/**
 * The shortest interval between two stats of one asset's file under hot reload. nya_asset_get is on every
 * draw path, and a stat per call would be tens of thousands of syscalls a frame.
 * */
#define _NYA_ASSET_STAT_INTERVAL_NS nya_time_ms_to_ns(100)
NYA_INTERNAL b8 _nya_asset_get_modification_time(NYA_Asset* asset, OUT u64* out_modification_time);

/*
 * Not NYA_INTERNAL
 *
 * Registered with nya_callback, which stores the symbol name; main.c re-resolves it with dlsym after a
 * hot reload. NYA_INTERNAL is hidden and static, which -rdynamic does not export, so the first reload
 * would assert "Could not find symbol". Ignore clang-tidy's misc-use-internal-linkage here.
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

/**
 * Samples per second a skeleton clip is baked at, at minimum. A uniform grid makes sampling a division
 * instead of a search, and thirty is above what this art style resolves.
 * */
#define NYA_ASSET_SKELETON_BAKE_RATE 30.0F

/*
 * The immediate 3D layout. Colour arrives as four halves and uv as two; the input assembler expands both,
 * so the shaders read `float4` and `float2`. See NYA_Vertex3D for why colour is HALF4.
 */
NYA_INTERNAL SDL_GPUVertexAttribute vertex_attributes[] = {
    { .location = 0, .format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3, .offset = nya_offsetof(NYA_Vertex3D, position), .buffer_slot = 0 },
    { .location = 1, .format = SDL_GPU_VERTEXELEMENTFORMAT_HALF4,  .offset = nya_offsetof(NYA_Vertex3D, color),    .buffer_slot = 0 },
    { .location = 2, .format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3, .offset = nya_offsetof(NYA_Vertex3D, normals),  .buffer_slot = 0 },
    { .location = 3, .format = SDL_GPU_VERTEXELEMENTFORMAT_HALF2,  .offset = nya_offsetof(NYA_Vertex3D, uv),       .buffer_slot = 0 },
};

NYA_INTERNAL SDL_GPUVertexBufferDescription vertex_buffer_description = {
    .slot               = 0,
    .input_rate         = SDL_GPU_VERTEXINPUTRATE_VERTEX,
    .instance_step_rate = 0,
    .pitch              = sizeof(NYA_Vertex3D),
};

/*
 * The retained mesh layout: vertices in buffer 0, a per-instance transform in buffer 1. Locations 4 to 7
 * are the model matrix's columns, since the engine's matrices are column-major. Rows would silently
 * transpose every model.
 */
NYA_INTERNAL SDL_GPUVertexAttribute vertex_attributes_3d_instanced[] = {
    { .location = 0, .format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3, .offset = nya_offsetof(NYA_Vertex3D, position), .buffer_slot = 0 },
    { .location = 1, .format = SDL_GPU_VERTEXELEMENTFORMAT_HALF4, .offset = nya_offsetof(NYA_Vertex3D, color), .buffer_slot = 0 },
    { .location = 2, .format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3, .offset = nya_offsetof(NYA_Vertex3D, normals), .buffer_slot = 0 },
    { .location = 3, .format = SDL_GPU_VERTEXELEMENTFORMAT_HALF2, .offset = nya_offsetof(NYA_Vertex3D, uv), .buffer_slot = 0 },

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
 * The 2D batch layout. See NYA_Vertex2D. Same semantics as 3D, with two position components, no normal,
 * and colour as four normalized bytes.
 */
/* The skinned 3D layout: the 3D one plus bone indices and weights at locations 4 and 5. */
NYA_INTERNAL SDL_GPUVertexAttribute vertex_attributes_3d_skinned[] = {
    { .location = 0, .format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3,      .offset = nya_offsetof(NYA_VertexSkinned3D, position), .buffer_slot = 0 },
    { .location = 1, .format = SDL_GPU_VERTEXELEMENTFORMAT_HALF4,       .offset = nya_offsetof(NYA_VertexSkinned3D, color),    .buffer_slot = 0 },
    { .location = 2, .format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3,      .offset = nya_offsetof(NYA_VertexSkinned3D, normals),  .buffer_slot = 0 },
    { .location = 3, .format = SDL_GPU_VERTEXELEMENTFORMAT_HALF2,       .offset = nya_offsetof(NYA_VertexSkinned3D, uv),       .buffer_slot = 0 },
    { .location = 4, .format = SDL_GPU_VERTEXELEMENTFORMAT_UBYTE4,      .offset = nya_offsetof(NYA_VertexSkinned3D, bones),    .buffer_slot = 0 },
    { .location = 5, .format = SDL_GPU_VERTEXELEMENTFORMAT_UBYTE4_NORM, .offset = nya_offsetof(NYA_VertexSkinned3D, weights),  .buffer_slot = 0 },
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
     // UBYTE4_NORM: NORM makes the input assembler divide by 255. without it everything saturates white.
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

    // not fatal: a build with no audio device runs, it just cannot load sounds.
    if (!TTF_Init()) nya_log_warn("TTF_Init() failed, fonts will not load: %s", SDL_GetError());

    if (!MIX_Init()) {
        nya_log_warn("MIX_Init() failed, sounds will not load: %s", SDL_GetError());
    } else {
        app->asset_system.mixer = MIX_CreateMixerDevice(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, nullptr);
        if (app->asset_system.mixer == nullptr) nya_log_warn("MIX_CreateMixerDevice() failed, sounds will not load: %s", SDL_GetError());
    }

    app->asset_system.assets          = nya_dict_create(app->asset_system.allocator, NYA_Asset);

    // a fresh dictionary shares nothing with the old memo.
    _nya_asset_lookup = nya_cache_create(
        app->asset_system.allocator,
        NYA_Asset*,
        .name         = "asset_lookup",
        .capacity     = NYA_ASSET_LOOKUP_CAPACITY,
        .key_size_max = NYA_ASSET_LOOKUP_HANDLE_MAX,
        .eviction     = NYA_CACHE_EVICTION_LEAST_RECENT,
    );
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

    // shaped text lives in this arena and belongs to the faces unloaded below.
    nya_text_run_cache_destroy();

    nya_dict_foreach_value(app->asset_system.assets, asset) {
        if (asset && asset->status != NYA_ASSET_STATUS_UNLOADED) { /**/
            nya_array_push_back(app->asset_system.unloading_queue, asset->handle);
        }
    }
    _nya_asset_unloading_process(nullptr);

    // after the assets: a MIX_Audio outlives neither its mixer nor its bytes.
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

    nya_cache_destroy(_nya_asset_lookup);
    _nya_asset_lookup = nullptr;

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

/** Invalidates the memo. Called wherever the dictionary is created or inserted into. */
NYA_INTERNAL void _nya_asset_lookup_invalidate(void) {
    _nya_asset_lookup_generation++;
}

void _nya_asset_lookup_remember(NYA_AssetHandle handle, u64 handle_length, NYA_Asset* asset) {
    void* slot = nullptr;

    // cannot fail: the caller checked the key fits, and a least recent cache always makes room.
    NYA_EXPECT(nya_cache_insert(_nya_asset_lookup, handle, handle_length, _nya_asset_lookup_generation, &slot));
    *(NYA_Asset**)slot = asset;
}

NYA_Asset* nya_asset_get(NYA_AssetHandle handle) {
    nya_assert(handle != nullptr);

    NYA_AssetSystem* system = &nya_app_get()->asset_system;

    // null before the asset system is up: a sprite atlas can be described during static setup.
    if (system->assets == nullptr) return nullptr;

    u64 handle_length = strlen(handle);

    // a cache key is never empty, and a longer handle than the memo keeps is not worth a copy.
    b8 memoizable = handle_length > 0 && handle_length <= NYA_ASSET_LOOKUP_HANDLE_MAX;

    NYA_Asset** memo  = memoizable ? nya_cache_get(_nya_asset_lookup, handle, handle_length, _nya_asset_lookup_generation) : nullptr;
    NYA_Asset*  asset = nullptr;

    if (memo != nullptr) {
        asset = *memo;
    } else {
        asset = nya_dict_get(system->assets, handle);
        if (memoizable) _nya_asset_lookup_remember(handle, handle_length, asset);
    }

#ifdef NYA_ASSET_HOT_RELOAD
    // only disk assets have a file that can change.
    if (asset != nullptr && asset->status == NYA_ASSET_STATUS_LOADED && !asset->from_blob) {
        // uptime, sampled once per frame, so every lookup in a frame agrees and clock changes do not matter.
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

    // a reference cancels a pending unload; the queue has not run yet, so the asset is intact.
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

    // checked: fetch_sub on zero wraps, and the asset would never unload.
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

    // a failure is terminal, so say so.
    if (asset != nullptr && asset->status == NYA_ASSET_STATUS_FAILED) {
        return nya_error(NYA_ERROR_NOT_OK, "asset '%s' previously failed to load", parameters.handle);
    }
    if (asset != nullptr && asset->status != NYA_ASSET_STATUS_UNLOADED) return NYA_OK;

    // Handles are copied first. They are literals in the game DLL's .rodata, which a hot reload unmaps, and the
    // asset system outlives the DLL.
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

    // the insert may have rehashed, moving every NYA_Asset* the memo holds.
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

    // shared assets are normal; whoever finishes first must not pull it from the other.
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

    // a scratch asset, never registered: this is a read, not a load.
    NYA_Asset raw = { 0 };
    NYA_TRY(_nya_asset_load_raw(handle, false, &raw));
    defer _nya_asset_unload_raw(&raw);

    // copied: filesystem bytes are freed by the defer above, so only a copy is safe to return in both cases.
    u8* copy = nya_arena_alloc(arena, raw.as_text.size);
    nya_memcpy(copy, raw.as_text.data, raw.as_text.size);

    *out_data = copy;
    *out_size = raw.as_text.size;

    return NYA_OK;
}

NYA_Error nya_asset_set_window_icon(NYA_WindowHandle window, NYA_AssetHandle handle) {
    if (handle == nullptr) return nya_error(NYA_ERROR_INVALID_ARGUMENT, "asset handle is null");

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

        const u8* stored = (const u8*)NYA_ASSET_BLOB + asset_header.start;

        // stored verbatim (LZ4 could not shrink it): pointed at directly, no allocation and no copy.
        if (asset_header.compressed_size == asset_header.size) {
            out_asset->as_text.data = (u8*)stored;
            out_asset->as_text.size = asset_header.size;
            out_asset->raw.data     = (u8*)stored;
            out_asset->raw.size     = asset_header.size;
            out_asset->raw_owned    = false;

            return NYA_OK;
        }

        // compressed: expanded once and shared, reference counted, by every asset reading this entry.
        NYA_AssetSystem* system = &nya_app_get()->asset_system;

        if (system->blob_expanded == nullptr) {
            u64 bytes             = NYA_ASSET_BLOB_HEADER_COUNT * sizeof(NYA_AssetBlobExpanded);
            system->blob_expanded = nya_arena_alloc(system->allocator, bytes);
            nya_memset(system->blob_expanded, 0, bytes);
        }

        NYA_AssetBlobExpanded* expanded = &system->blob_expanded[asset_header_index];

        if (expanded->references == 0) {
            u8* data = nya_arena_alloc(system->allocator, asset_header.size);
            if (data == nullptr) {
                return nya_error(NYA_ERROR_OUT_OF_MEMORY, "could not allocate " FMTu64 " bytes to decompress '%s'", asset_header.size, path);
            }

            if (!nya_decompress(stored, asset_header.compressed_size, data, asset_header.size)) {
                nya_arena_free(system->allocator, data, asset_header.size);
                return nya_error(NYA_ERROR_CORRUPT, "the embedded blob entry for '%s' did not decompress", path);
            }

            expanded->data = data;
        }

        expanded->references++;

        out_asset->as_text.data   = expanded->data;
        out_asset->as_text.size   = asset_header.size;
        out_asset->raw.data       = expanded->data;
        out_asset->raw.size       = asset_header.size;
        out_asset->raw_owned      = false;
        out_asset->raw_shared     = true;
        out_asset->raw_blob_index = (u32)asset_header_index;

        return NYA_OK;
    }

    // a build problem, not fatal: one missing texture should not take the game down.
    return nya_error(NYA_ERROR_NOT_FOUND, "asset not in the embedded blob: %s. Was it indexed at build time?", path);
}
#endif // NYA_ASSET_PREFER_BLOB

NYA_INTERNAL NYA_Error _nya_asset_load_raw_from_filesystem(NYA_CString path, OUT NYA_Asset* out_asset) {
    nya_assert(path != nullptr);

    // an external asset is a path from outside the game, and the file may have moved.
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
    out_asset->raw.data     = data;
    out_asset->raw.size     = size;

    // freed on unload.
    out_asset->raw_owned = true;

#ifdef NYA_ASSET_HOT_RELOAD
    out_asset->source_modification_time = modification_time;
#else
    nya_unused(modification_time); // nothing is watching, so nothing needs the timestamp.
#endif

    return NYA_OK;
}

NYA_INTERNAL void _nya_asset_unload_raw_from_filesystem(NYA_Asset* asset) {
    nya_assert(asset != nullptr);

    NYA_App*   app   = nya_app_get();
    NYA_Arena* arena = app->asset_system.allocator;

    nya_arena_free(arena, asset->raw.data, asset->raw.size);

    asset->raw.data = nullptr;
    asset->raw.size = 0;
}

/**
 * Marks an asset as unloadable and says why, once.
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
 * Copies a decoded surface into GPU memory through a transfer buffer. Converted to RGBA32 first so the
 * texture format is fixed.
 * */
/*
 * Staging is per texture; the copy pass is per command buffer, so all textures in a frame share one submit
 * in _nya_asset_flush_uploads.
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

    // computed wide: the transfer buffer size is a u32, which four bytes per pixel overflows around 32k square.
    u64 size_wide = (u64)width * (u64)height * 4ULL;
    if (size_wide == 0 || size_wide > U32_MAX) {
        if (converted) SDL_DestroySurface(rgba);
        return nya_error(NYA_ERROR_NOT_SUPPORTED, "image is %ux%u, which does not fit a single GPU transfer buffer", width, height);
    }
    u32 size = (u32)size_wide;

    SDL_GPUTexture* texture = nya_gpu_texture_create(
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

    SDL_GPUTransferBuffer* transfer = nya_gpu_transfer_buffer_create(
        render_system->gpu_device,
        &(SDL_GPUTransferBufferCreateInfo){ .usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD, .size = size }
    );

    if (transfer == nullptr) {
        nya_gpu_texture_release(render_system->gpu_device, texture);
        if (converted) SDL_DestroySurface(rgba);
        return nya_error(NYA_ERROR_OUT_OF_MEMORY, "SDL_CreateGPUTransferBuffer() failed: %s", SDL_GetError());
    }

    void* mapped = SDL_MapGPUTransferBuffer(render_system->gpu_device, transfer, false);
    if (mapped == nullptr) {
        nya_gpu_transfer_buffer_release(render_system->gpu_device, transfer);
        nya_gpu_texture_release(render_system->gpu_device, texture);
        if (converted) SDL_DestroySurface(rgba);
        return nya_error(NYA_ERROR_OUT_OF_MEMORY, "SDL_MapGPUTransferBuffer() failed: %s", SDL_GetError());
    }

    /*
     * Row by row: a surface's pitch may exceed its width, so one bulk copy would read padding and run off the
     * last row.
     */
    u32 row_bytes = width * 4;
    for (u32 row = 0; row < height; row++) {
        nya_memcpy((u8*)mapped + ((u64)row * row_bytes), (const u8*)rgba->pixels + ((u64)row * (u64)rgba->pitch), row_bytes);
    }

    SDL_UnmapGPUTransferBuffer(render_system->gpu_device, transfer);

    if (converted) SDL_DestroySurface(rgba);

    // handed over before the copy: the asset is only marked loaded after the flush.
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
        // nothing was copied, so the textures are undefined. the transfer buffers are still released.
        nya_log_error("SDL_AcquireGPUCommandBuffer() failed, dropping " FMTu64 " texture uploads: %s", pending->length, SDL_GetError());
        nya_array_foreach (pending, upload) nya_gpu_transfer_buffer_release(render_system->gpu_device, upload->transfer);
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

    // released after the submit, once the commands referencing them are recorded.
    nya_array_foreach (pending, upload) nya_gpu_transfer_buffer_release(render_system->gpu_device, upload->transfer);

    nya_array_clear(pending);
}


/*
 * ─────────────────────────────────────────────────────────
 * SKINNING EXTRACTION
 * ─────────────────────────────────────────────────────────
 */

/** ufbx's matrix is three rows of an affine transform. */
NYA_INTERNAL ufbx_matrix _nya_asset_node_world_at(ufbx_anim* anim, ufbx_node* node, f64 time, ufbx_matrix* worlds, u32* computed_frame, u32 frame);

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
 * The skinned geometry's transform to world at bind time, the same through every cluster. Skinned vertices and
 * root bones are moved through it, so a rigged file comes out y up and in metres like a static one.
 * */
NYA_INTERNAL ufbx_matrix _nya_asset_skin_geometry_bind(const ufbx_skin_deformer* skin) {
    nya_assert(skin != nullptr && skin->clusters.count > 0);

    return ufbx_matrix_mul(&skin->clusters.data[0]->bind_to_world, &skin->clusters.data[0]->geometry_to_bone);
}

/**
 * Builds the skeleton and bakes every clip, or returns null when the file is not rigged. `out_bone_nodes`
 * receives the node behind each bone, so another skinned mesh can map its clusters by node.
 * */
NYA_INTERNAL NYA_Skeleton* _nya_asset_mesh_skeleton(NYA_Arena* arena, ufbx_scene* scene, OUT ufbx_node** out_bone_nodes) {
    ufbx_skin_deformer* skin = _nya_asset_find_skin(scene);

    if (skin == nullptr || skin->clusters.count == 0) return nullptr;

    u32 cluster_count = (u32)skin->clusters.count;

    if (cluster_count > NYA_SKELETON_MAX_BONES) {
        nya_log_warn("The model has %u bones and the palette holds %d; the deepest ones are dropped.", cluster_count, NYA_SKELETON_MAX_BONES);
    }

    /*
     * Each cluster's parent is its nearest ancestor node that is also a cluster. Helper nodes and the armature
     * root have no cluster.
     */
    s32* cluster_parent = nya_arena_alloc(arena, (u64)cluster_count * sizeof(s32));
    u32* cluster_depth  = nya_arena_alloc(arena, (u64)cluster_count * sizeof(u32));
    u32* order          = nya_arena_alloc(arena, (u64)cluster_count * sizeof(u32));
    defer nya_arena_free(arena, cluster_parent, (u64)cluster_count * sizeof(s32));
    defer nya_arena_free(arena, cluster_depth, (u64)cluster_count * sizeof(u32));
    defer nya_arena_free(arena, order, (u64)cluster_count * sizeof(u32));

    for (u32 i = 0; i < cluster_count; i++) {
        cluster_parent[i] = -1;

        ufbx_node* node = skin->clusters.data[i]->bone_node;
        if (node == nullptr) continue;

        for (ufbx_node* ancestor = node->parent; ancestor != nullptr && cluster_parent[i] < 0; ancestor = ancestor->parent) {
            for (u32 j = 0; j < cluster_count; j++) {
                if (skin->clusters.data[j]->bone_node == ancestor) {
                    cluster_parent[i] = (s32)j;
                    break;
                }
            }
        }
    }

    // bounded by the cluster count; a node tree has no cycles.
    u32 max_depth = 0;
    for (u32 i = 0; i < cluster_count; i++) {
        u32 depth = 0;
        for (s32 at = cluster_parent[i]; at >= 0 && depth < cluster_count; at = cluster_parent[at]) depth++;

        cluster_depth[i] = depth;
        max_depth        = nya_max(max_depth, depth);
    }

    /*
     * Ordered by depth, so parents come first. nya_skeleton_model_transforms composes in one forward pass, and
     * exporters list clusters in vertex group order. Truncating to the palette then drops leaves.
     */
    u32 ordered = 0;
    for (u32 depth = 0; depth <= max_depth; depth++) {
        for (u32 i = 0; i < cluster_count; i++) {
            if (cluster_depth[i] == depth) order[ordered++] = i;
        }
    }
    nya_assert(ordered == cluster_count);

    u32 bone_count = nya_min(cluster_count, (u32)NYA_SKELETON_MAX_BONES);

    ufbx_matrix geometry_bind         = _nya_asset_skin_geometry_bind(skin);
    ufbx_matrix geometry_bind_inverse = ufbx_matrix_invert(&geometry_bind);

    NYA_Skeleton* skeleton = nya_arena_alloc(arena, sizeof(NYA_Skeleton));

    *skeleton = (NYA_Skeleton){
        .bones      = nya_arena_alloc(arena, bone_count * sizeof(NYA_SkeletonBone)),
        .bone_count = bone_count,
    };

    for (u32 i = 0; i < bone_count; i++) {
        u32                cluster_index = order[i];
        ufbx_skin_cluster* cluster       = skin->clusters.data[cluster_index];

        out_bone_nodes[i] = cluster->bone_node;

        NYA_SkeletonBone* bone = &skeleton->bones[i];

        *bone = (NYA_SkeletonBone){
            .parent = -1,

            // geometry_to_bone is the inverse bind, taken from the space the vertices are moved into.
            .inverse_bind = _nya_asset_matrix_from_ufbx(ufbx_matrix_mul(&cluster->geometry_to_bone, &geometry_bind_inverse)),
            .rest         = { .scale = { 1.0F, 1.0F, 1.0F } },
        };

        NYA_ConstCString name = cluster->bone_node != nullptr ? cluster->bone_node->name.data : "bone";
        (void)snprintf(bone->name, sizeof(bone->name), "%s", name);

        // a parent has a smaller depth, so it is already placed.
        s32 parent_cluster = cluster_parent[cluster_index];
        for (u32 b = 0; b < i && parent_cluster >= 0; b++) {
            if (order[b] == (u32)parent_cluster) bone->parent = (s32)b;
        }

        nya_assert(bone->parent < (s32)i, "bone '%s' is not ordered after its parent", bone->name);
    }

    /*
     * The rest pose comes from the bind matrices, not the nodes. A bone's node chain runs through non-bone
     * nodes such as the armature, which local transforms would skip.
     */
    for (u32 i = 0; i < bone_count; i++) {
        ufbx_matrix geometry_to_bone = skin->clusters.data[order[i]]->geometry_to_bone;
        ufbx_matrix bone_to_geometry = ufbx_matrix_invert(&geometry_to_bone);

        s32 parent = skeleton->bones[i].parent;

        // a child relative to its parent's bind transform, a root to the space the vertices are moved into.
        ufbx_matrix outer = parent >= 0 ? skin->clusters.data[order[parent]]->geometry_to_bone : geometry_bind;
        ufbx_matrix local = ufbx_matrix_mul(&outer, &bone_to_geometry);

        skeleton->bones[i].rest = _nya_asset_bone_transform(ufbx_matrix_to_transform(&local));
    }

    // clips
    if (scene->anim_stacks.count > 0) {
        // one world transform per scene node per frame, shared by bones with common ancestors.
        u64          node_count     = scene->nodes.count;
        ufbx_matrix* node_worlds    = nya_arena_alloc(arena, node_count * sizeof(ufbx_matrix));
        u32*         computed_frame = nya_arena_alloc(arena, node_count * sizeof(u32));
        defer nya_arena_free(arena, node_worlds, node_count * sizeof(ufbx_matrix));
        defer nya_arena_free(arena, computed_frame, node_count * sizeof(u32));

        u32 frame_serial = 0;

        skeleton->clips      = nya_arena_alloc(arena, scene->anim_stacks.count * sizeof(NYA_SkeletonClip));
        skeleton->clip_count = (u32)scene->anim_stacks.count;

        for (u64 c = 0; c < scene->anim_stacks.count; c++) {
            ufbx_anim_stack* stack = scene->anim_stacks.data[c];

            NYA_SkeletonClip* clip = &skeleton->clips[c];

            f32 duration = (f32)(stack->time_end - stack->time_begin);

            if (duration < 0.0F) duration = 0.0F;

            /* Baked on a uniform grid so sampling is a division, not a search. */
            u32 frames = (u32)ceilf(duration * NYA_ASSET_SKELETON_BAKE_RATE) + 1;
            f32 rate   = duration > 0.0F ? (f32)(frames - 1) / duration : NYA_ASSET_SKELETON_BAKE_RATE;

            *clip = (NYA_SkeletonClip){
                .duration_s  = duration,
                .frame_count = frames,
                .frame_rate  = rate,
                .frames      = nya_arena_alloc(arena, (u64)frames * bone_count * sizeof(NYA_BoneTransform)),
            };

            (void)snprintf(clip->name, sizeof(clip->name), "%s", stack->name.data);

            for (u32 f = 0; f < frames; f++) {
                f64 time = stack->time_begin + ((f64)f / (f64)rate);

                // serials start at one, so a zeroed computed_frame caches nothing.
                frame_serial++;

                if (time > stack->time_end) time = stack->time_end;

                /*
                 * World first, then made relative. A bone's node parent is not its bone parent, so node-local transforms
                 * would apply the geometry transform twice.
                 */
                ufbx_matrix world[NYA_SKELETON_MAX_BONES];

                nya_memset(computed_frame, 0, node_count * sizeof(u32));

                for (u32 b = 0; b < bone_count; b++) {
                    ufbx_node* node = out_bone_nodes[b];

                    world[b] = node != nullptr ? _nya_asset_node_world_at(stack->anim, node, time, node_worlds, computed_frame, frame_serial)
                                               : ufbx_identity_matrix;
                }

                for (u32 b = 0; b < bone_count; b++) {
                    NYA_BoneTransform* out = &clip->frames[((u64)f * bone_count) + b];

                    s32 parent = skeleton->bones[b].parent;

                    ufbx_matrix local;

                    if (parent >= 0) {
                        // between two bones the geometry constant cancels.
                        ufbx_matrix parent_inverse = ufbx_matrix_invert(&world[parent]);

                        local = ufbx_matrix_mul(&parent_inverse, &world[b]);
                    } else {
                        // a root bone's world transform, since the vertices sit in bind time world space.
                        local = world[b];
                    }

                    *out = _nya_asset_bone_transform(ufbx_matrix_to_transform(&local));
                }
            }
        }
    }

    return skeleton;
}


/**
 * A node's world transform at `time`. ufbx_evaluate_transform gives parent-relative transforms and there
 * is no evaluated scene (UFBX_NO_SCENE_EVALUATION), so this walks the real node chain.
 * */
NYA_INTERNAL ufbx_matrix _nya_asset_node_world_at(ufbx_anim* anim, ufbx_node* node, f64 time, ufbx_matrix* worlds, u32* computed_frame, u32 frame) {
    // walks up to the first ancestor known this frame, then composes back down.
    ufbx_node* chain[256];
    u32        depth = 0;

    ufbx_node* step = node;
    while (step != nullptr && computed_frame[step->typed_id] != frame) {
        nya_assert(depth < nya_carray_length(chain), "a node hierarchy deeper than %d", (s32)nya_carray_length(chain));
        chain[depth++] = step;
        step           = step->parent;
    }

    ufbx_matrix world = step != nullptr ? worlds[step->typed_id] : ufbx_identity_matrix;

    for (u32 i = depth; i > 0; i--) {
        ufbx_node*     at     = chain[i - 1];
        ufbx_transform local  = ufbx_evaluate_transform(anim, at, time);
        ufbx_matrix    matrix = ufbx_transform_to_matrix(&local);

        world                        = ufbx_matrix_mul(&world, &matrix);
        worlds[at->typed_id]         = world;
        computed_frame[at->typed_id] = frame;
    }

    return world;
}

/**
 * The four strongest influences on one vertex, normalised. ufbx sorts weights by influence. `cluster_bone`
 * maps this mesh's clusters to skeleton bones, -1 for a dropped bone, since each skinned mesh numbers its
 * clusters differently.
 * */
NYA_INTERNAL void _nya_asset_mesh_vertex_weights(ufbx_mesh* mesh, const s32* cluster_bone, u32 index, OUT u32* out_bones, OUT f32* out_weights) {
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

        // a dropped bone is skipped, so its weight does not go to an unrelated bone.
        if (weight.cluster_index >= skin->clusters.count || cluster_bone[weight.cluster_index] < 0) continue;

        out_bones[taken]   = (u32)cluster_bone[weight.cluster_index];
        out_weights[taken] = (f32)weight.weight;

        total += (f32)weight.weight;
        taken++;
    }

    /* A vertex with no influence is pinned to bone zero. Zero weights would collapse it to the origin. */
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

    /* ufbx triangulates and converts to y-up metres, since exporters disagree on both. */
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
     * Counted per instance and per material, as the write loop iterates. num_triangles is after triangulation;
     * the face count under-allocates for quads.
     */
    u32 total = 0;
    u32 parts = 0;

    for (u64 i = 0; i < scene->meshes.count; i++) {
        ufbx_mesh* mesh = scene->meshes.data[i];

        u64 instances = mesh->instances.count > 0 ? mesh->instances.count : 1;

        total += (u32)(mesh->num_triangles * instances) * 3;

        // at least one part per instance, so a mesh with no material still draws.
        parts += (u32)(instances * (mesh->material_parts.count > 0 ? mesh->material_parts.count : 1));
    }

    if (total == 0 || parts == 0) return nya_error(NYA_ERROR_NOT_OK, "the FBX has no triangles");

    /* No welding: faces meeting at a hard edge need the same position with different normals. */
    f32x3* positions = nya_arena_alloc(arena, total * sizeof(f32x3));
    f32x3* normals   = nya_arena_alloc(arena, total * sizeof(f32x3));
    f32x2* uvs       = nya_arena_alloc(arena, total * sizeof(f32x2));

    // skinning, only when a mesh carries a deformer.
    ufbx_node*    bone_nodes[NYA_SKELETON_MAX_BONES] = { 0 };
    NYA_Skeleton* skeleton                           = _nya_asset_mesh_skeleton(arena, scene, bone_nodes);
    ufbx_skin_deformer* reference_skin               = _nya_asset_find_skin(scene);

    u32* bone_indices = nullptr;
    f32* bone_weights = nullptr;

    ufbx_matrix skin_to_world     = ufbx_identity_matrix;
    ufbx_matrix skin_to_world_dir = ufbx_identity_matrix;

    if (skeleton != nullptr) {
        skin_to_world     = _nya_asset_skin_geometry_bind(reference_skin);
        skin_to_world_dir = ufbx_matrix_for_normals(&skin_to_world);

        bone_indices = nya_arena_alloc(arena, (u64)total * NYA_SKELETON_WEIGHTS_PER_VERTEX * sizeof(u32));
        bone_weights = nya_arena_alloc(arena, (u64)total * NYA_SKELETON_WEIGHTS_PER_VERTEX * sizeof(f32));

        nya_memset(bone_indices, 0, (u64)total * NYA_SKELETON_WEIGHTS_PER_VERTEX * sizeof(u32));
        nya_memset(bone_weights, 0, (u64)total * NYA_SKELETON_WEIGHTS_PER_VERTEX * sizeof(f32));
    }

    NYA_MeshPart* mesh_parts = nya_arena_alloc(arena, parts * sizeof(NYA_MeshPart));

    /*
     * Textures are deduplicated by the ufbx texture pointer, since materials often share an image. `sources` is
     * the key; `textures` is what the asset owns.
     */
    SDL_GPUTexture** textures = nya_arena_alloc(arena, parts * sizeof(SDL_GPUTexture*));
    const void**     sources  = nya_arena_alloc(arena, parts * sizeof(const void*));

    u32 texture_count = 0;
    u32 written       = 0;
    u32 part_count    = 0;

    for (u64 m = 0; m < scene->meshes.count; m++) {
        ufbx_mesh* mesh = scene->meshes.data[m];

        /*
         * This mesh's clusters mapped to bones by node, and the transform into the reference skin's geometry space,
         * where the inverse binds apply.
         */
        ufbx_skin_deformer* mesh_skin        = mesh->skin_deformers.count > 0 ? mesh->skin_deformers.data[0] : nullptr;
        u64                 mesh_clusters    = mesh_skin != nullptr ? mesh_skin->clusters.count : 0;
        s32*                cluster_bone     = nullptr;
        ufbx_matrix         to_reference     = ufbx_identity_matrix;
        ufbx_matrix         to_reference_dir = ufbx_identity_matrix;

        if (skeleton != nullptr && mesh_clusters > 0) {
            cluster_bone = nya_arena_alloc(arena, mesh_clusters * sizeof(s32));

            b8 space_found = mesh_skin == reference_skin;

            for (u64 c = 0; c < mesh_clusters; c++) {
                cluster_bone[c] = -1;

                for (u32 b = 0; b < skeleton->bone_count; b++) {
                    if (bone_nodes[b] != mesh_skin->clusters.data[c]->bone_node) continue;

                    cluster_bone[c] = (s32)b;

                    if (!space_found) {
                        // the same bone in both skins: reference from bone from this mesh.
                        ufbx_skin_cluster* reference = nullptr;
                        for (u64 r = 0; r < reference_skin->clusters.count; r++) {
                            if (reference_skin->clusters.data[r]->bone_node == bone_nodes[b]) reference = reference_skin->clusters.data[r];
                        }

                        if (reference != nullptr) {
                            ufbx_matrix bone_to_reference = ufbx_matrix_invert(&reference->geometry_to_bone);
                            to_reference                  = ufbx_matrix_mul(&bone_to_reference, &mesh_skin->clusters.data[c]->geometry_to_bone);
                            to_reference_dir              = ufbx_matrix_for_normals(&to_reference);
                            space_found                   = true;
                        }
                    }
                    break;
                }
            }
        }

        u64  corners  = (u64)mesh->max_face_triangles * 3;
        u32* triangle = nya_arena_alloc(arena, corners * sizeof(u32));

        u64 instances = mesh->instances.count > 0 ? mesh->instances.count : 1;

        for (u64 n = 0; n < instances; n++) {
            /*
             * The node transform is applied: vertex positions are in the mesh's own space. pill.fbx is a capsule only
             * because its node stretches it.
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

                /* The material for this run and its faces, grouped by ufbx. */
                ufbx_material* material = nullptr;

                if (mesh->material_parts.count > 0 && g < mesh->materials.count) material = mesh->materials.data[g];

                if (material != nullptr) {
                    ufbx_vec3 diffuse = material->fbx.diffuse_color.value_vec3;

                    part->base_color = (NYA_Color){ (f32)diffuse.x, (f32)diffuse.y, (f32)diffuse.z, 1.0F };

                    ufbx_texture* texture = material->fbx.diffuse_color.texture;

                    // pbr.base_color as the fallback, for exporters that write a modern material graph.
                    if (texture == nullptr) texture = material->pbr.base_color.texture;

                    // any texture, embedded or not; exporters often leave images beside the model.
                    if (texture != nullptr) {
                        // already decoded for an earlier part.
                        s32 existing = -1;

                        for (u32 i = 0; i < texture_count; i++) {
                            if (sources[i] == texture->content.data) existing = (s32)i;
                        }

                        if (existing >= 0) {
                            part->texture = existing;
                        } else {
                            /*
                             * The texture embedded in the FBX first, the file it names second. Both are normal: embedding is an exporter
                             * option, and without it the image sits beside the model, usually in `<model>.fbm`.
                             */
                            NYA_Arena* texture_arena = nya_arena_create(.name = "fbx_texture");
                            defer      nya_arena_destroy(texture_arena);

                            const u8* image      = texture->content.data;
                            u64       image_size = texture->content.size;

                            if (image_size == 0) {
                                // resolved against the model's directory through the asset system, so bundled and loose builds agree.
                                NYA_ConstCString relative =
                                    texture->relative_filename.length > 0 ? texture->relative_filename.data : texture->filename.data;

                                if (relative != nullptr && relative[0] != '\0') {
                                    // everything up to the model's last slash.
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
                                // only warn again when there were bytes to decode.
                                if (image_size > 0) {
                                    nya_log_warn("Could not decode the texture '%s' from '%s' (%s); that part will draw untextured.",
                                             texture->name.data, handle, SDL_GetError());
                                }
                            } else {
                                /*
                                 * Staged into a scratch asset: as_texture and as_mesh share a union, and out_asset already holds the
                                 * triangles.
                                 */
                                NYA_Asset staged = { 0 };

                                NYA_Error upload = _nya_asset_stage_texture(surface, pending, &staged);
                                SDL_DestroySurface(surface);

                                if (!upload.ok) {
                                    nya_log_warn("Could not upload a texture embedded in an FBX (%s); that part will draw untextured.",
                                             (NYA_ConstCString)upload.message);
                                } else {
                                    sources[texture_count]  = texture->content.data;
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

                    // triangulated face by face, or quad-modelled meshes get holes.
                    u32 triangles = ufbx_triangulate_face(triangle, corners, mesh, mesh->faces.data[face_index]);

                    for (u32 corner = 0; corner < triangles * 3 && written < total; corner++) {
                        u32 vertex = triangle[corner];

                        ufbx_vec3 position = ufbx_get_vertex_vec3(&mesh->vertex_position, vertex);
                        ufbx_vec3 normal   = ufbx_get_vertex_vec3(&mesh->vertex_normal, vertex);

                        /*
                         * The UV, or zero without a UV set; `exists` has to be checked, or ufbx reads a null index array. V is
                         * flipped because FBX puts the origin bottom left.
                         */
                        ufbx_vec2 uv = { 0 };

                        if (mesh->vertex_uv.exists) {
                            uv   = ufbx_get_vertex_vec2(&mesh->vertex_uv, vertex);
                            uv.y = 1.0 - uv.y;
                        }

                        /*
                         * The node transform is baked in for static meshes only. A skinned vertex goes through the reference
                         * skin's bind below, which is what its inverse bind expects.
                         */
                        if (skeleton == nullptr) {
                            position = ufbx_transform_position(&to_world, position);
                            normal   = ufbx_transform_direction(&normal_world, normal);
                        }

                        if (skeleton != nullptr && cluster_bone != nullptr) {
                            position = ufbx_transform_position(&to_reference, position);
                            normal   = ufbx_transform_direction(&to_reference_dir, normal);

                            _nya_asset_mesh_vertex_weights(mesh, cluster_bone, vertex, &bone_indices[(u64)written * NYA_SKELETON_WEIGHTS_PER_VERTEX],
                                                           &bone_weights[(u64)written * NYA_SKELETON_WEIGHTS_PER_VERTEX]);
                        } else if (skeleton != nullptr) {
                            // an unskinned mesh in a rigged file rides the root bone instead of collapsing under zero weights.
                            bone_indices[(u64)written * NYA_SKELETON_WEIGHTS_PER_VERTEX] = 0;
                            bone_weights[(u64)written * NYA_SKELETON_WEIGHTS_PER_VERTEX] = 1.0F;
                        }

                        if (skeleton != nullptr) {
                            position = ufbx_transform_position(&skin_to_world, position);
                            normal   = ufbx_transform_direction(&skin_to_world_dir, normal);
                        }

                        positions[written] = (f32x3){ (f32)position.x, (f32)position.y, (f32)position.z };

                        // renormalised: non-uniform scale changes a normal's length.
                        normals[written] = nya_vector_normalize((f32x3){ (f32)normal.x, (f32)normal.y, (f32)normal.z });

                        uvs[written] = (f32x2){ (f32)uv.x, (f32)uv.y };

                        written++;
                    }
                }

                part->vertex_count = written - part->first_vertex;

                // empty runs are dropped; they would bind a texture and draw nothing.
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
     * The skeleton and the skinned vertices, built here because the final vertex count is only known after the
     * loop drops empty runs.
     */
    out_asset->as_mesh.skeleton = skeleton;

    if (skeleton != nullptr && written > 0) {
        NYA_VertexSkinned3D* skinned = nya_arena_alloc(arena, (u64)written * sizeof(NYA_VertexSkinned3D));

        for (u32 v = 0; v < written; v++) {
            skinned[v] = (NYA_VertexSkinned3D){
                .position = { positions[v].x, positions[v].y, positions[v].z },
                .uv       = { (f16)uvs[v].x, (f16)uvs[v].y },
                .normals  = { normals[v].x, normals[v].y, normals[v].z },
                .color    = { 1.0F, 1.0F, 1.0F, 1.0F },
            };

            u32 quantised = 0;

            for (u32 w = 0; w < NYA_SKELETON_WEIGHTS_PER_VERTEX; w++) {
                const f32 weight = bone_weights[((u64)v * NYA_SKELETON_WEIGHTS_PER_VERTEX) + w];

                skinned[v].bones[w]   = (u8)bone_indices[((u64)v * NYA_SKELETON_WEIGHTS_PER_VERTEX) + w];
                skinned[v].weights[w] = (u8)((weight * 255.0F) + 0.5F);

                quantised += skinned[v].weights[w];
            }

            // four rounded weights miss 255 by at most two steps, which the strongest influence absorbs unnoticed.
            nya_assert(quantised >= 253 && quantised <= 257 && skinned[v].weights[0] >= 2);
            skinned[v].weights[0] = (u8)(skinned[v].weights[0] + 255 - quantised);
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

    out_asset->from_blob  = false;
    out_asset->raw_owned  = false;
    out_asset->raw_shared = false;

    // takes the handle because a shader loads its compiled artifact, whose handle differs. external assets come
    // from outside the game and always load from disk.
    if (external) return _nya_asset_load_raw_from_filesystem(handle, out_asset);

#ifdef NYA_ASSET_PREFER_BLOB
    // the blob is a cache in front of the filesystem, so a handle it lacks falls through to disk.
    NYA_Error blob_result = _nya_asset_load_raw_from_blob(handle, out_asset);
    if (blob_result.ok) {
        out_asset->from_blob = true;
        return NYA_OK;
    }
    if (blob_result.kind != NYA_ERROR_NOT_FOUND) return blob_result;
#endif

    return _nya_asset_load_raw_from_filesystem(handle, out_asset);
}

/** Takes an asset back out of the unloading queue when something acquires it first. */
/*
 * A copy of a handle the asset system can keep. Interned once per asset, since a repeat load returns early.
 * Freed with the arena at shutdown.
 */
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

#ifdef NYA_ASSET_PREFER_BLOB
    if (asset->raw_shared) {
        NYA_AssetSystem*       system   = &nya_app_get()->asset_system;
        NYA_AssetBlobExpanded* expanded = &system->blob_expanded[asset->raw_blob_index];

        nya_assert(expanded->references > 0, "a shared blob entry released more often than it was taken");
        nya_assert(expanded->data == asset->raw.data);

        expanded->references--;
        if (expanded->references == 0) {
            nya_arena_free(system->allocator, expanded->data, asset->raw.size);
            expanded->data = nullptr;
        }

        asset->raw_shared = false;
        asset->raw.data   = nullptr;
        asset->raw.size   = 0;
        return;
    }
#endif

    // a verbatim blob entry points into the executable's .rodata and is not freed.
    if (!asset->raw_owned) return;

    _nya_asset_unload_raw_from_filesystem(asset);
}

#ifdef NYA_ASSET_HOT_RELOAD
NYA_INTERNAL b8 _nya_asset_get_modification_time(NYA_Asset* asset, OUT u64* out_modification_time) {
    NYA_Error result;

    switch (asset->type) {
        /* Everything backed by one file on disk. A type missing here reports no timestamp and never hot reloads. */
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
             * Watched through the compiled shader artifacts, not the .hlsl: until the build compiles a source edit, the
             * running shaders have not changed.
             */
            NYA_AssetSystem* system = &nya_app_get()->asset_system;

            NYA_Asset* vertex   = nya_dict_get(system->assets, asset->load_parameters.as_graphics_pipeline.vertex_shader_handle);
            NYA_Asset* fragment = nya_dict_get(system->assets, asset->load_parameters.as_graphics_pipeline.fragment_shader_handle);

            // mid load or gone: no timestamp keeps the pipeline out of the reload queue until its shaders exist.
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

void nya_asset_load_queued(void) {
    // a window made without the asset system, as some renderer tests do, has nothing queued.
    if (nya_app_get()->asset_system.loading_queue == nullptr) return;

    _nya_asset_loading_process(nullptr);
}

void _nya_asset_loading_process(NYA_Event* event) {
    // runs on NYA_EVENT_FRAME_ENDED, outside every frame_* timer. decoding is the classic invisible spike.
    nya_perf_time_this_function();

    nya_unused(event);

    NYA_AssetSystem*  system        = &nya_app_get()->asset_system;
    NYA_RenderSystem* render_system = &nya_app_get()->render_system;

    // every texture decoded here uploads in one copy pass at the end.
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

                /* All three decode from memory, so blob, disk and dropped files take the same route. */

            case NYA_ASSET_TYPE_MESH: {
                NYA_Error result = _nya_asset_load_raw(parameters->source != nullptr ? (NYA_AssetHandle)parameters->source : parameters->handle, parameters->external, asset);
                if (!result.ok) {
                    _nya_asset_fail(asset, &result);
                    break;
                }

                /*
                 * Built into a scratch asset, because as_mesh and as_text share a union: writing the mesh into `asset`
                 * would overwrite the file bytes that unloading then frees.
                 */
                NYA_Asset built_mesh = { 0 };

                NYA_Error built = _nya_asset_build_mesh(parameters->handle, asset->as_text.data, asset->as_text.size, &pending_uploads, &built_mesh);

                // the FBX bytes are freed either way; the triangles are what the asset is now.
                _nya_asset_unload_raw(asset);

                if (!built.ok) {
                    _nya_asset_fail(asset, &built);
                    break;
                }

                asset->as_mesh = built_mesh.as_mesh;

                // carried from the load parameters; see as_mesh_load.filter.
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
                 * A requested size only matters for a vector image, so SVG is tried first. IMG_LoadSizedSVG_IO fails cleanly
                 * on anything else, which is simpler than sniffing XML declarations and comments.
                 */
                SDL_Surface* surface = nullptr;

                u32 requested_width  = parameters->as_texture_load.width;
                u32 requested_height = parameters->as_texture_load.height;

                if (requested_width > 0 && requested_height > 0) {
                    /*
                     * `currentColor` becomes white before rasterising. After rasterising it is black, and a tint (a multiply)
                     * cannot recolour black.
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

                    // the replacement is shorter than the keyword, so the original size is enough room.
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
                _nya_asset_unload_raw(asset); // the encoded bytes are not needed once decoded.

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

                // MIX_Audio reads the stream lazily for a sound that is not predecoded.
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

                // a face has one size baked in, so zero is a caller mistake.
                f32 point_size = parameters->as_font.point_size > 0.0F ? parameters->as_font.point_size : 16.0F;

                // TTF_Font reads glyphs lazily, so the bytes must outlive the open.
                SDL_IOStream* stream = SDL_IOFromConstMem(asset->as_text.data, asset->as_text.size);
                TTF_Font*     font   = TTF_OpenFontIO(stream, true, point_size);

                if (font == nullptr) {
                    NYA_Error decode = nya_error(NYA_ERROR_CORRUPT, "could not open font '%s': %s", parameters->handle, SDL_GetError());
                    _nya_asset_fail(asset, &decode);
                    break;
                }

                /*
                 * Light hinting straightens stems vertically and leaves horizontal outlines alone. Normal hinting distorts
                 * letterforms to force pixel alignment, which antialiased coverage does not need.
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

                // checked: a missing .spv would reach SDL_CreateGPUShader as a null pointer.
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
                _nya_asset_unload_raw(asset); // the source text is not needed once compiled.

                if (shader == nullptr) {
                    NYA_Error compile = nya_error(NYA_ERROR_CORRUPT, "could not create shader '%s': %s", parameters->handle, SDL_GetError());
                    _nya_asset_fail(asset, &compile);
                    break;
                }

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
                 * The shaders must have loaded, not just exist. A failed shader leaves a null `shader`, and this error names
                 * it, which tells a missing build from a rejected one.
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
                 * Straight alpha, matching the fragment shaders. The whole struct is zeroed when blending is off, since SDL
                 * reads the fields regardless.
                 */
                SDL_GPUColorTargetBlendState blend_state = { 0 };

                if (parameters->as_graphics_pipeline.blend == NYA_BLEND_ADDITIVE) {
                    blend_state = (SDL_GPUColorTargetBlendState){
                        .enable_blend          = true,
                        .src_color_blendfactor = SDL_GPU_BLENDFACTOR_SRC_ALPHA,
                        // ONE: overlapping glows saturate toward white.
                        .dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE,
                        .color_blend_op        = SDL_GPU_BLENDOP_ADD,
                        .src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE,
                        .dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE,
                        .alpha_blend_op        = SDL_GPU_BLENDOP_ADD,
                    };
                } else if (parameters->as_graphics_pipeline.blend == NYA_BLEND_MULTIPLY) {
                    blend_state = (SDL_GPUColorTargetBlendState){
                        .enable_blend = true,
                        // source times destination: a light map darkens what it covers.
                        .src_color_blendfactor = SDL_GPU_BLENDFACTOR_DST_COLOR,
                        .dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ZERO,
                        .color_blend_op        = SDL_GPU_BLENDOP_ADD,
                        // alpha untouched, or a light map would eat a render texture's opacity.
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
                        // the swapchain's alpha is unused, but this stays correct for a texture composited later.
                        .src_alpha_blendfactor  = SDL_GPU_BLENDFACTOR_ONE,
                        .dst_alpha_blendfactor  = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA,
                        .alpha_blend_op         = SDL_GPU_BLENDOP_ADD,
                    };
                }

                /* The layout tables, chosen once. The instanced layout also differs in buffer count. */
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
            // declared whenever the pipeline tests or writes depth. a depth pipeline in a pass without depth fails
            // validation; the reverse is fine.
            .has_depth_stencil_target  = parameters->as_graphics_pipeline.depth_test || parameters->as_graphics_pipeline.depth_write,
            .depth_stencil_format      = render_system->depth_format,
            .num_color_targets = 1,
            .color_target_descriptions = (SDL_GPUColorTargetDescription[]){
              {
                // the named format, else the window's. see color_format.
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
          // must match every target drawn into, so the renderer has one sample count.
          .multisample_state.sample_count                = parameters->as_graphics_pipeline.single_sampled ? SDL_GPU_SAMPLECOUNT_1
                                                                                                            : render_system->sample_count,
          .rasterizer_state.fill_mode                    = SDL_GPU_FILLMODE_FILL,
          // counter-clockwise front, matching the 3D primitives.
          .rasterizer_state.cull_mode                    = parameters->as_graphics_pipeline.cull_back_faces  ? SDL_GPU_CULLMODE_BACK
                                                           : parameters->as_graphics_pipeline.cull_front_faces ? SDL_GPU_CULLMODE_FRONT
                                                                                                               : SDL_GPU_CULLMODE_NONE,
          .rasterizer_state.front_face                   = SDL_GPU_FRONTFACE_COUNTER_CLOCKWISE,
          /*
           * LESS, not LESS_OR_EQUAL, so coplanar geometry does not flicker. Written even with depth off, since SDL
           * reads the fields.
           */
          .depth_stencil_state = {
            .compare_op         = SDL_GPU_COMPAREOP_LESS,
            .enable_depth_test  = parameters->as_graphics_pipeline.depth_test,
            .enable_depth_write = parameters->as_graphics_pipeline.depth_write,
          },
          .vertex_input_state.num_vertex_buffers         = buffer_description_count,
          .vertex_input_state.vertex_buffer_descriptions = buffer_descriptions,
          // every attribute of the layout; a short count leaves shaders reading garbage.
          .vertex_input_state.num_vertex_attributes      = attribute_count,
          .vertex_input_state.vertex_attributes          = attributes,
        };
                SDL_GPUGraphicsPipeline* pipeline = SDL_CreateGPUGraphicsPipeline(render_system->gpu_device, &pipelineCreateInfo);

                /*
                 * Reported, not asserted. Backends disagree about pipelines, so this can fail on one driver only; the log
                 * names the pipeline and SDL's reason, and everything else still loads.
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

        if (asset->status == NYA_ASSET_STATUS_LOADED) asset->generation = ++_nya_asset_generation_last;

#ifdef NYA_ASSET_HOT_RELOAD
        /*
         * The watch baseline, taken once the asset has loaded. A pipeline has no file of its own, and a baseline of
         * zero would queue it for reload on the first frame.
         */
        if (asset->status == NYA_ASSET_STATUS_LOADED && !asset->from_blob) {
            _nya_asset_get_modification_time(asset, &asset->source_modification_time);
        }
#endif
    }

    // one copy pass for everything staged. safe to mark loaded first: nothing samples before the next frame.
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

        // reacquired between queueing and now; an earlier round may have left a second entry.
        if (atomic_load(&asset->reference_count) > 0) continue;
        if (asset->status == NYA_ASSET_STATUS_UNLOADED) continue;

        switch (asset->type) {
            case NYA_ASSET_TYPE_TEXT: {
                _nya_asset_unload_raw(asset);
                asset->status = NYA_ASSET_STATUS_UNLOADED;
            } break;

            case NYA_ASSET_TYPE_MESH: {
                // the FBX bytes are gone; the three arrays were allocated together and are freed together.
                /*
                 * GPU textures first, once each through the deduplicated array. Releasing through the parts would free a
                 * shared image twice.
                 */
                for (u32 i = 0; i < asset->as_mesh.texture_count; i++) {
                    nya_gpu_texture_release(render_system->gpu_device, asset->as_mesh.textures[i]);
                }

                /*
                 * The retained vertex buffer, if anything drew the mesh. The renderer creates it; unload releases it, or
                 * every hot reload would strand one in VRAM.
                 */
                if (asset->as_mesh.gpu_vertices != nullptr) {
                    nya_gpu_buffer_release(render_system->gpu_device, asset->as_mesh.gpu_vertices);

                    asset->as_mesh.gpu_vertices     = nullptr;
                    asset->as_mesh.gpu_vertex_count = 0;
                }

                if (asset->as_mesh.positions != nullptr) {
                    // `allocated`, not the counts: the arena frees by extent. see NYA_Asset.as_mesh.allocated.
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

                // +1 for the terminator reserved in _nya_asset_pick_correct_compiled_shader.
                nya_arena_free(system->allocator, (void*)asset->as_shader.compiled_handle, strlen(asset->as_shader.compiled_handle) + 1);

                // cleared so nothing reads the freed handle before a reload picks a new one.
                asset->as_shader.compiled_handle = nullptr;

                asset->status = NYA_ASSET_STATUS_UNLOADED;
            } break;

            case NYA_ASSET_TYPE_GRAPHICS_PIPELINE: {
                SDL_ReleaseGPUGraphicsPipeline(render_system->gpu_device, asset->as_graphics_pipeline.pipeline);
                asset->status = NYA_ASSET_STATUS_UNLOADED;
            } break;

            case NYA_ASSET_TYPE_TEXTURE: {
                // the encoded bytes were released once the pixels reached the GPU.
                nya_gpu_texture_release(render_system->gpu_device, asset->as_texture.texture);
                asset->as_texture = (typeof(asset->as_texture)){ 0 };
                asset->status     = NYA_ASSET_STATUS_UNLOADED;
            } break;

            case NYA_ASSET_TYPE_SOUND: {
                // audio before the bytes: a streamed sound still reads them.
                MIX_DestroyAudio(asset->as_sound.audio);
                asset->as_sound.audio = nullptr;
                _nya_asset_unload_raw(asset);
                asset->status = NYA_ASSET_STATUS_UNLOADED;
            } break;

            case NYA_ASSET_TYPE_FONT: {
                // same ordering: the face reads glyphs lazily.
                TTF_CloseFont(asset->as_font.font);
                asset->as_font.font = nullptr;
                _nya_asset_unload_raw(asset);
                asset->status = NYA_ASSET_STATUS_UNLOADED;
            } break;

            default: {
                // anything that never finished loading has nothing to release.
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

        // a pipeline can be queued behind a blob-backed shader.
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
             * The shaders are rebuilt before the pipeline, or it binds the old modules again. Both queues drain in
             * order next frame, unloads first.
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

    // by what the device accepts, not by OS: Windows runs Vulkan as often as Direct3D 12, and a Vulkan device
    // handed DXIL rejects every shader.
    SDL_GPUShaderFormat formats = SDL_GetGPUShaderFormats(nya_app_get()->render_system.gpu_device);

    if (formats & SDL_GPU_SHADERFORMAT_SPIRV) {
        nya_string_extend(compiled_shader_path, ".spv");
        *out_format = SDL_GPU_SHADERFORMAT_SPIRV;
    } else if (formats & SDL_GPU_SHADERFORMAT_DXIL) {
        nya_string_extend(compiled_shader_path, ".dxil");
        *out_format = SDL_GPU_SHADERFORMAT_DXIL;
    } else if (formats & SDL_GPU_SHADERFORMAT_MSL) {
        nya_string_extend(compiled_shader_path, ".msl");
        *out_format = SDL_GPU_SHADERFORMAT_MSL;
    } else {
        nya_log_panic("The GPU device accepts none of the compiled shader formats (SPIR-V, DXIL, MSL).");
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

/** Suffix test over C strings. */
NYA_INTERNAL b8 _nya_asset_path_has_suffix(NYA_ConstCString path, NYA_ConstCString suffix) {
    if (suffix == nullptr || suffix[0] == '\0') return true;
    if (path == nullptr) return false;

    u64 path_length   = strlen(path);
    u64 suffix_length = strlen(suffix);

    if (suffix_length > path_length) return false;

    return nya_memcmp(path + path_length - suffix_length, suffix, suffix_length) == 0;
}

#ifndef NYA_ASSET_PREFER_BLOB
/* Disk walk helpers, only compiled when there is a disk walk, or they are unused statics. */
typedef struct {
    NYA_ArrayᐸNYA_Stringᐳ* paths;
    NYA_ConstCString       suffix;
} _NYA_AssetEnumerateContext;

NYA_INTERNAL b8 _nya_asset_enumerate_collect(NYA_ConstCString path, const NYA_DirectoryEntry* entry, void* user_data) {
    _NYA_AssetEnumerateContext* context = user_data;

    if (entry->type != NYA_FILE_TYPE_FILE) return true;
    if (!_nya_asset_path_has_suffix(path, context->suffix)) return true;

    NYA_String* copy = nya_string_from(context->paths->arena, path);

    // handles start with "./", as the blob's do, so both builds name assets identically.
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
    // the baked index, already sorted by the generator.
    for (u64 i = 0; i < NYA_ASSET_BLOB_HEADER_COUNT; i++) {
        NYA_ConstCString path = NYA_ASSET_BLOB_HEADER[i].path;

        if (!_nya_asset_path_has_suffix(path, suffix)) continue;

        NYA_String* copy = nya_string_from(arena, path);
        nya_array_push_back(paths, *copy);
    }
#else
    _NYA_AssetEnumerateContext context = { .paths = paths, .suffix = suffix };

    // a missing assets directory is an empty list, not an error.
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
    // no blob in this build, so the index is empty.
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
    /* Linear: this is for tooling that walks the whole index anyway. */
    for (u64 i = 0; i < NYA_ASSET_BLOB_HEADER_COUNT; i++) {
        if (nya_string_equals(NYA_ASSET_BLOB_HEADER[i].path, path)) return &NYA_ASSET_BLOB_HEADER[i];
    }
#endif

    return nullptr;
}
