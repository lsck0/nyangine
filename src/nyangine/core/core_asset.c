#include "SDL3/SDL_gpu.h"

#include "nyangine/nyangine.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API DECLRARATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/*
 * There is no longer a choice of backend. The filesystem is always there, and the embedded blob is
 * an optional layer in front of it.
 *
 * NYA_ASSET_PREFER_BLOB bakes assets/assets.c into the binary and consults it first; anything not in it
 * still comes off disk, so a shipped build can read files that did not exist when it was built
 * without a second code path. NYA_ASSET_HOT_RELOAD watches the files that did come off disk and
 * reloads them when they change — orthogonal to the blob, because an asset served out of the blob
 * has no file to watch.
 *
 * Neither is required, and both together is a sensible combination: prefer the baked copy, but pick
 * up edits to anything overriding it.
 */
#ifdef NYA_ASSET_PREFER_BLOB
#include "../../../assets/assets.c"

NYA_INTERNAL NYA_Error _nya_asset_load_raw_from_blob(NYA_AssetHandle path, OUT NYA_Asset* out_asset);
#endif // NYA_ASSET_PREFER_BLOB

NYA_INTERNAL NYA_Error _nya_asset_load_raw_from_filesystem(NYA_AssetHandle path, OUT NYA_Asset* out_asset);
NYA_INTERNAL void      _nya_asset_unload_raw_from_filesystem(NYA_Asset* asset);

/** Blob first when there is one, disk otherwise, and always disk for an external asset. */
NYA_INTERNAL NYA_Error _nya_asset_load_raw(NYA_AssetHandle handle, b8 external, OUT NYA_Asset* out_asset);
NYA_INTERNAL void      _nya_asset_fail(NYA_Asset* asset, const NYA_Error* error);
/**
 * A texture that has had its pixels staged and is waiting for the frame's copy pass.
 *
 * The transfer buffer has to outlive the copy pass that reads it, which is why these are collected
 * and released together after the single submit rather than each upload cleaning up after itself.
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

/** Runs every staged upload in one copy pass, then releases the transfer buffers. */
NYA_INTERNAL void _nya_asset_flush_uploads(NYA_Arrayᐸ_NYA_AssetPendingUploadᐳ* pending);
NYA_INTERNAL void      _nya_asset_unload_raw(NYA_Asset* asset);
NYA_INTERNAL void      _nya_asset_cancel_queued_unload(NYA_Asset* asset);

/** Copies a caller supplied handle into the asset system's own memory. See the note at its definition. */
NYA_INTERNAL NYA_AssetHandle _nya_asset_intern(NYA_AssetHandle handle) __attr_no_discard;

#ifdef NYA_ASSET_HOT_RELOAD
#define _NYA_ASSET_RELOAD_GRACE_FRAMES 5
NYA_INTERNAL b8 _nya_asset_get_modification_time(NYA_Asset* asset, OUT u64* out_modification_time);

void _nya_asset_reload_process(NYA_Event* event);
#endif // NYA_ASSET_HOT_RELOAD

void _nya_asset_loading_process(NYA_Event* event);
void _nya_asset_unloading_process(NYA_Event* event);

NYA_INTERNAL NYA_AssetHandle _nya_asset_pick_correct_compiled_shader(NYA_AssetHandle source_shader, OUT SDL_GPUShaderFormat* out_format);

SDL_GPUVertexAttribute vertex_attributes[] = {
    {
     .location    = 0,
     .format      = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3,
     .offset      = nya_offsetof(NYA_Vertex,           position),
     .buffer_slot = 0,
     },
    {
     .location    = 1,
     .format      = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4,
     .offset      = nya_offsetof(NYA_Vertex,                                 color),
     .buffer_slot = 0,
     },
    {
     .location    = 2,
     .format      = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3,
     .offset      = nya_offsetof(NYA_Vertex,                                                normals),
     .buffer_slot = 0,
     },
    {
     .location    = 3,
     .format      = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2,
     .offset      = nya_offsetof(NYA_Vertex,uv),
     .buffer_slot = 0,
     },
};

SDL_GPUVertexBufferDescription vertex_buffer_description = {
    .slot               = 0,
    .input_rate         = SDL_GPU_VERTEXINPUTRATE_VERTEX,
    .instance_step_rate = 0,
    .pitch              = sizeof(NYA_Vertex),
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

    // The decoders are libraries in their own right and each needs bringing up once. A failure here
    // is not fatal: a build with no audio device still runs, it just cannot load sounds.
    if (!TTF_Init()) nya_warn("TTF_Init() failed, fonts will not load: %s", SDL_GetError());

    if (!MIX_Init()) {
        nya_warn("MIX_Init() failed, sounds will not load: %s", SDL_GetError());
    } else {
        app->asset_system.mixer = MIX_CreateMixerDevice(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, nullptr);
        if (app->asset_system.mixer == nullptr) nya_warn("MIX_CreateMixerDevice() failed, sounds will not load: %s", SDL_GetError());
    }

    app->asset_system.assets          = nya_dict_create(app->asset_system.allocator, NYA_Asset);
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

    nya_info("Asset system initialized.");
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

    nya_info("Asset system deinitialized.");
}

/*
 * ─────────────────────────────────────────────────────────
 * ASSET FUNCTIONS
 * ─────────────────────────────────────────────────────────
 */

NYA_Asset* nya_asset_get(NYA_AssetHandle handle) {
    nya_assert(handle != nullptr);

    NYA_AssetSystem* system = &nya_app_get()->asset_system;
    NYA_Asset*       asset  = nya_dict_get(system->assets, handle);

#ifdef NYA_ASSET_HOT_RELOAD
    // Only assets that came off disk have a file behind them to have changed. One served out of the
    // blob is part of the executable, so there is nothing to stat and nothing that could differ.
    if (asset != nullptr && asset->status == NYA_ASSET_STATUS_LOADED && !asset->from_blob) {
        u64 file_modification_time = 0;
        _nya_asset_get_modification_time(asset, &file_modification_time);

        if (file_modification_time > asset->source_modification_time) { /**/
            if (nya_array_contains(system->reload_queue, asset->handle)) return asset;

            asset->reload_grace_frames = _NYA_ASSET_RELOAD_GRACE_FRAMES;
            nya_array_push_back(system->reload_queue, asset->handle);
            nya_info("Asset marked for reload due to modification: %s", asset->handle);
        }
    }
#endif // NYA_ASSET_HOT_RELOAD

    return asset;
}

NYA_Error nya_asset_acquire(NYA_AssetHandle handle) {
    if (handle == nullptr) return nya_error(NYA_ERROR_INVALID_ARGUMENT, "asset handle is null.");

    NYA_AssetSystem* system = &nya_app_get()->asset_system;

    NYA_Asset* asset = nya_dict_get(system->assets, handle);
    if (asset == nullptr) return nya_error(NYA_ERROR_NOT_FOUND, "cannot acquire '%s': it was never loaded.", handle);
    if (asset->status == NYA_ASSET_STATUS_FAILED) return nya_error(NYA_ERROR_NOT_OK, "cannot acquire '%s': it failed to load.", handle);

    // Taking a reference cancels a pending unload. The asset is still intact at this point, since
    // the queue is not processed until the end of the frame, so this is a plain revival rather than
    // a resurrection of something already torn down.
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
        nya_warn("Released asset '%s', which was never loaded. This release has no matching acquire.", handle);
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
        nya_warn("Released asset '%s' more times than it was acquired.", handle);
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

    if (parameters.handle == nullptr) return nya_error(NYA_ERROR_INVALID_ARGUMENT, "asset handle is null.");

    NYA_Asset* asset = nya_dict_get(system->assets, parameters.handle);

    // Already known. A previous failure is terminal, so say so rather than silently doing nothing.
    if (asset != nullptr && asset->status == NYA_ASSET_STATUS_FAILED) {
        return nya_error(NYA_ERROR_NOT_OK, "asset '%s' previously failed to load.", parameters.handle);
    }
    if (asset != nullptr && asset->status != NYA_ASSET_STATUS_UNLOADED) return NYA_OK;

    /*
     * Every handle the system is about to keep is copied into its own memory first.
     *
     * Handles are the string literals generated into assets.h, so a handle passed in by the game
     * lives in the game's DLL. Hot reloading dlcloses that DLL and unmaps the .rodata those literals
     * sit in, which leaves the dict holding keys that point into nothing — and since a dict hashes
     * its keys by content, the next lookup faults while hashing rather than merely missing.
     *
     * The asset system outlives every DLL that talks to it, so the strings have to as well.
     * */
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
    nya_array_push_back(system->loading_queue, parameters);

    nya_info("Queuing asset for loading: %s", parameters.handle);

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

    // Still in use by someone else. Not an error, and not something to assert over: sharing an
    // asset between two systems is the normal case, and whichever finishes first should not be able
    // to pull it out from under the other.
    if (atomic_load(&asset->reference_count) > 0) return false;

    if (asset->queued_for_unload) return true; // already on its way out
    if (asset->status == NYA_ASSET_STATUS_UNLOADED) return true;

    asset->queued_for_unload = true;
    nya_array_push_back(system->unloading_queue, asset->handle);

    nya_info("Queuing asset for unloading: %s", asset->handle);
    return true;
}

NYA_Error nya_asset_set_window_icon(NYA_WindowHandle window, NYA_AssetHandle handle) {
    if (handle == nullptr) return nya_error(NYA_ERROR_INVALID_ARGUMENT, "asset handle is null.");

    // A bare NYA_Asset used as a scratch buffer, never entered into the registry: this is a read,
    // not a load. _nya_asset_unload_raw frees it only when the bytes came off disk, since blob bytes
    // are part of the executable.
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

    // A baked asset that is not in the blob is a build problem, but still not worth killing the
    // process for: the caller gets to decide, and one missing texture should not take the game down.
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
 * The alternative was nya_panic, which turned one bad file into a dead process. A missing texture
 * should cost you a texture. The status is terminal so the loader does not retry it every frame.
 * */
NYA_INTERNAL void _nya_asset_fail(NYA_Asset* asset, const NYA_Error* error) {
    nya_assert(asset != nullptr);
    nya_assert(error != nullptr);

    nya_warn("Asset '%s' failed to load: %s", asset->handle, (NYA_ConstCString)error->message);

    asset->status = NYA_ASSET_STATUS_FAILED;

    nya_event_dispatch((NYA_Event){
        .type           = NYA_EVENT_ASSET_LOAD_FAILED,
        .as_asset_event = { .asset_handle = asset->handle },
    });
}

/**
 * Copies a decoded surface into GPU memory.
 *
 * The GPU cannot read the surface directly: the pixels have to go through a transfer buffer and be
 * copied in on a command buffer. Everything is converted to RGBA32 first so the texture format is
 * known regardless of what the file happened to contain.
 * */
/*
 * Staging and submission are separate because a copy pass is per command buffer, not per texture.
 *
 * Every texture used to acquire its own command buffer, open a copy pass for its single upload,
 * close it and submit — so loading twenty textures in a frame meant twenty submissions, each with
 * the driver side synchronisation that implies. The pixel work below is per texture and unavoidable;
 * the command recording is not, and now happens once for the whole batch in _nya_asset_flush_uploads.
 */
NYA_INTERNAL NYA_Error _nya_asset_stage_texture(SDL_Surface* surface, NYA_Arrayᐸ_NYA_AssetPendingUploadᐳ* pending, OUT NYA_Asset* out_asset) {
    nya_assert(surface != nullptr);
    nya_assert(pending != nullptr);
    nya_assert(out_asset != nullptr);

    NYA_RenderSystem* render_system = &nya_app_get()->render_system;
    if (render_system->gpu_device == nullptr) return nya_error(NYA_ERROR_NOT_SUPPORTED, "no GPU device; cannot upload a texture.");

    SDL_Surface* rgba      = surface;
    b8           converted = false;
    if (surface->format != SDL_PIXELFORMAT_RGBA32) {
        rgba = SDL_ConvertSurface(surface, SDL_PIXELFORMAT_RGBA32);
        if (rgba == nullptr) return nya_error(NYA_ERROR_NOT_OK, "could not convert image to RGBA32: %s", SDL_GetError());
        converted = true;
    }

    u32 width  = (u32)rgba->w;
    u32 height = (u32)rgba->h;
    u32 size   = width * height * 4;

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
    nya_memcpy(mapped, rgba->pixels, size);
    SDL_UnmapGPUTransferBuffer(render_system->gpu_device, transfer);

    if (converted) SDL_DestroySurface(rgba);

    // The texture handle is handed over now even though nothing has been copied into it yet. Nothing
    // can sample it before the frame ends, because the asset is only marked loaded after the flush.
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
        // Nothing has been copied, so the textures are undefined rather than wrong. Releasing the
        // transfer buffers is still the right thing; the textures belong to their assets now and are
        // freed on unload like any other.
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

    // Released only after the submit: SDL keeps the buffer alive until the copy has actually run, but
    // the release has to come after the commands that reference it have been recorded and handed over.
    nya_array_foreach (pending, upload) SDL_ReleaseGPUTransferBuffer(render_system->gpu_device, upload->transfer);

    nya_array_clear(pending);
}

NYA_INTERNAL NYA_Error _nya_asset_load_raw(NYA_AssetHandle handle, b8 external, OUT NYA_Asset* out_asset) {
    nya_assert(handle != nullptr);
    nya_assert(out_asset != nullptr);

    out_asset->from_blob = false;

    // Takes the handle rather than reading it off the parameters, because a shader loads the
    // *compiled* artifact whose handle is derived from the one that was requested.
    //
    // An external asset is a path from outside the game — something dropped on the window, or a
    // file the player picked — so it was never a candidate for the blob and goes straight to disk.
    if (external) return _nya_asset_load_raw_from_filesystem(handle, out_asset);

#ifdef NYA_ASSET_PREFER_BLOB
    // The blob is a cache in front of the filesystem rather than a replacement for it. A handle it
    // does not carry is not an error: the asset may have been added since the build, or deliberately
    // shipped loose beside the executable, and either way disk is the answer.
    NYA_Error blob_result = _nya_asset_load_raw_from_blob(handle, out_asset);
    if (blob_result.kind == NYA_ERROR_NONE) {
        out_asset->from_blob = true;
        return NYA_OK;
    }
    if (blob_result.kind != NYA_ERROR_NOT_FOUND) return blob_result;
#endif

    return _nya_asset_load_raw_from_filesystem(handle, out_asset);
}

/** Takes an asset back out of the unloading queue, for when something acquires it before the queue runs. */
/*
 * A handle the asset system can keep, copied out of whoever supplied it.
 *
 * Interned once per asset: nya_asset_load returns early for a handle it already knows, so a repeat
 * load does not allocate a second copy. Freed with the asset system's arena at shutdown.
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
        case NYA_ASSET_TYPE_TEXT: {
            result = nya_filesystem_last_modified(asset->handle, out_modification_time);
        } break;

        case NYA_ASSET_TYPE_SHADER_VERTEX:
        case NYA_ASSET_TYPE_SHADER_FRAGMENT: {
            result = nya_filesystem_last_modified(asset->as_shader.compiled_handle, out_modification_time);
        } break;

        case NYA_ASSET_TYPE_GRAPHICS_PIPELINE: {
            /*
             * Watched through the compiled artifacts its shaders were built from, not the .hlsl
             * those were compiled out of.
             *
             * Editing the source changes nothing a running process can use: compiling is a build
             * step, and until it runs the .spv on disk is still what the shader modules hold. This
             * used to watch load_parameters.*_shader_handle, which is the source path — so touching
             * a shader rebuilt the pipeline out of unchanged shader modules, which looks exactly
             * like hot reload doing nothing.
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
            if (result.kind == NYA_ERROR_NONE) {
                result = nya_filesystem_last_modified(fragment->as_shader.compiled_handle, &fragment_shader_modification_time);
            }

            if (result.kind == NYA_ERROR_NONE) *out_modification_time = nya_max(vertex_shader_modification_time, fragment_shader_modification_time);
        } break;

        default: {
            *out_modification_time = 0;
            return true;
        } break;
    }

    if (result.kind != NYA_ERROR_NONE) nya_warn("Loaded asset missing from filesystem: %s", asset->handle);
    return result.kind == NYA_ERROR_NONE;
}
#endif // NYA_ASSET_HOT_RELOAD

void _nya_asset_loading_process(NYA_Event* event) {
    nya_unused(event);

    NYA_AssetSystem*  system        = &nya_app_get()->asset_system;
    NYA_RenderSystem* render_system = &nya_app_get()->render_system;

    // Every texture decoded in this pass stages into here and is uploaded in a single copy pass at
    // the end, rather than each one opening and submitting a command buffer of its own.
    NYA_Arrayᐸ_NYA_AssetPendingUploadᐳ pending_uploads = nya_array_create_on_stack(system->allocator, _NYA_AssetPendingUpload);
    defer                              nya_array_destroy_on_stack(&pending_uploads);

    nya_array_foreach (system->loading_queue, parameters) {
        nya_info("Loading asset: %s", parameters->handle);

        NYA_Asset* asset = nya_dict_get(system->assets, parameters->handle);
        nya_assert(asset != nullptr);

        switch (parameters->type) {
            case NYA_ASSET_TYPE_TEXT: {
                NYA_Error result = _nya_asset_load_raw(parameters->handle, parameters->external, asset);
                if (result.kind != NYA_ERROR_NONE) {
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

            case NYA_ASSET_TYPE_TEXTURE: {
                NYA_Error result = _nya_asset_load_raw(parameters->handle, parameters->external, asset);
                if (result.kind != NYA_ERROR_NONE) {
                    _nya_asset_fail(asset, &result);
                    break;
                }

                u8* encoded      = asset->as_text.data;
                u64 encoded_size = asset->as_text.size;

                SDL_Surface* surface = IMG_Load_IO(SDL_IOFromConstMem(encoded, encoded_size), true);
                _nya_asset_unload_raw(asset); // the encoded bytes are done with once decoded

                if (surface == nullptr) {
                    NYA_Error decode = nya_error(NYA_ERROR_CORRUPT, "could not decode image '%s': %s", parameters->handle, SDL_GetError());
                    _nya_asset_fail(asset, &decode);
                    break;
                }

                NYA_Error upload = _nya_asset_stage_texture(surface, &pending_uploads, asset);
                SDL_DestroySurface(surface);

                if (upload.kind != NYA_ERROR_NONE) {
                    _nya_asset_fail(asset, &upload);
                    break;
                }

                asset->type   = NYA_ASSET_TYPE_TEXTURE;
                asset->status = NYA_ASSET_STATUS_LOADED;
            } break;

            case NYA_ASSET_TYPE_SOUND: {
                NYA_Error result = _nya_asset_load_raw(parameters->handle, parameters->external, asset);
                if (result.kind != NYA_ERROR_NONE) {
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
                NYA_Error result = _nya_asset_load_raw(parameters->handle, parameters->external, asset);
                if (result.kind != NYA_ERROR_NONE) {
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

                asset->as_font.font = font;
                asset->type         = NYA_ASSET_TYPE_FONT;
                asset->status       = NYA_ASSET_STATUS_LOADED;
            } break;

            case NYA_ASSET_TYPE_SHADER_VERTEX:
            case NYA_ASSET_TYPE_SHADER_FRAGMENT: {
                SDL_GPUShaderFormat format;
                NYA_AssetHandle     compiled_shader_handle = _nya_asset_pick_correct_compiled_shader(parameters->handle, &format);
                _nya_asset_load_raw(compiled_shader_handle, parameters->external, asset);

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
                nya_assert(shader != nullptr, "Failed to compile shader: %s", parameters->handle);
                _nya_asset_unload_raw(asset); // unload raw text code

                asset->type                      = NYA_ASSET_TYPE_SHADER_VERTEX;
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

                SDL_GPUGraphicsPipelineCreateInfo pipelineCreateInfo = {
          .target_info = {
            .num_color_targets = 1,
            .color_target_descriptions = (SDL_GPUColorTargetDescription[]){
              { .format = SDL_GetGPUSwapchainTextureFormat(render_system->gpu_device, parameters->as_graphics_pipeline.window->sdl_window),},
            },
          },
          .primitive_type                                = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST,
          .vertex_shader                                 = vertex_shader_asset->as_shader.shader,
          .fragment_shader                               = fragment_shader_asset->as_shader.shader,
          .rasterizer_state.fill_mode                    = SDL_GPU_FILLMODE_FILL,
          .rasterizer_state.cull_mode                    = SDL_GPU_CULLMODE_NONE,
          .vertex_input_state.num_vertex_buffers         = 1,
          .vertex_input_state.vertex_buffer_descriptions = &vertex_buffer_description,
          .vertex_input_state.num_vertex_attributes      = 2,
          .vertex_input_state.vertex_attributes          = vertex_attributes,
        };
                SDL_GPUGraphicsPipeline* pipeline = SDL_CreateGPUGraphicsPipeline(render_system->gpu_device, &pipelineCreateInfo);
                nya_assert(pipeline != nullptr);

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
         *
         * Only the filesystem loader used to set this, so anything not built from a file of its own
         * kept a baseline of zero — a graphics pipeline, in particular, which is assembled from
         * shaders that were already loaded. Any nonzero timestamp then looked newer than zero, so
         * every pipeline queued itself for reload on the first frame and rebuilt once for nothing.
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
        nya_info("Unloading asset: %s", *handle);

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

            case NYA_ASSET_TYPE_SHADER_VERTEX:
            case NYA_ASSET_TYPE_SHADER_FRAGMENT: {
                SDL_ReleaseGPUShader(render_system->gpu_device, asset->as_shader.shader);
                nya_arena_free(system->allocator, (void*)asset->as_shader.compiled_handle, strlen(asset->as_shader.compiled_handle));
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
            nya_info("Postponing reload of asset (grace period): %s", *handle);
            asset->reload_grace_frames--;
            nya_array_push_back(&postponed, *handle);
            continue;
        }

        if (file_modification_time != asset->source_modification_time) {
            nya_info("Postponing reload of asset (still written to): %s", *handle);
            asset->source_modification_time = file_modification_time;
            nya_array_push_back(&postponed, *handle);
        } else {
            nya_info("Reloading asset: %s", *handle);

            /*
             * A pipeline holds the shader modules its shaders created, so those have to be rebuilt
             * from the new bytes before the pipeline is, or it binds the same modules again and the
             * reload changes nothing visible.
             *
             * Queued ahead of the pipeline because both queues are drained in order on the next
             * frame — unloading first, then loading — so the shaders are recreated by the time the
             * pipeline asks them for their SDL_GPUShader.
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

                    nya_info("Also reloading the shader behind it: %s", shader_handles[i]);
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
                            nya_info("Also marking graphics pipeline for reload due to shader modification: %s", other_asset->handle);
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
            nya_panic("Unsupported OS for picking compiled shader: %d", NYA_OS_CURRENT);
        } break;
    }

    NYA_CString handle = nya_arena_alloc(system->allocator, compiled_shader_path->length + 1);
    nya_memcpy(handle, compiled_shader_path->items, compiled_shader_path->length);
    handle[compiled_shader_path->length] = '\0';

    nya_string_destroy(compiled_shader_path);

    return handle;
}
