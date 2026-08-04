#pragma once

#include "SDL3/SDL_gpu.h"

#include "nyangine/base/base.h"
#include "nyangine/base/base_arena.h"
#include "nyangine/base/base_array.h"
#include "nyangine/base/base_dict.h"
#include "nyangine/base/base_hset.h"
#include "nyangine/base/base_string.h"
#include "nyangine/core/core_event.h"
#include "nyangine/core/core_window.h"
#include "nyangine/renderer/renderer.h"

#include "../../../assets/assets.h"
#include "SDL3_image/SDL_image.h"
#include "SDL3_mixer/SDL_mixer.h"
#include "SDL3_ttf/SDL_ttf.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * TYPES
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

typedef NYA_CString                    NYA_AssetHandle;
typedef enum NYA_AssetLoadStatus       NYA_AssetStatus;
typedef enum NYA_AssetType             NYA_AssetType;
typedef struct NYA_Asset               NYA_Asset;
typedef struct NYA_AssetBlobHeader     NYA_AssetBlobHeader;
typedef struct NYA_AssetLoadParameters NYA_AssetLoadParameters;
typedef struct NYA_AssetSystem         NYA_AssetSystem;
nya_derive_array(NYA_AssetHandle);
nya_derive_array(NYA_AssetLoadParameters);
nya_derive_dict(NYA_Asset);

/*
 * ─────────────────────────────────────────────────────────
 * SYSTEM STRUCT
 * ─────────────────────────────────────────────────────────
 */

struct NYA_AssetSystem {
    NYA_Arena* allocator;

    /**
     * Owns every decoded sound. SDL_mixer needs a mixer before it can load anything, and audio
     * outlives the track playing it, so the system holds one rather than each caller making its own.
     * */
    MIX_Mixer* mixer;

    NYA_DictᐸNYA_Assetᐳ*                assets;
    NYA_ArrayᐸNYA_AssetLoadParametersᐳ* loading_queue;
    NYA_ArrayᐸNYA_AssetHandleᐳ*         unloading_queue;

#ifdef NYA_ASSET_HOT_RELOAD
    NYA_ArrayᐸNYA_AssetHandleᐳ* reload_queue;
#endif // NYA_ASSET_HOT_RELOAD
};

/*
 * ─────────────────────────────────────────────────────────
 * ASSET STRUCTS
 * ─────────────────────────────────────────────────────────
 */

struct NYA_AssetBlobHeader {
    NYA_ConstCString path;
    u64              start;
    u64              size;
};

enum NYA_AssetType {
    // raw data
    NYA_ASSET_TYPE_TEXT,

    // processed data on cpu ram
    NYA_ASSET_TYPE_SOUND,
    NYA_ASSET_TYPE_FONT,

    // processed data on gpu vram
    NYA_ASSET_TYPE_TEXTURE,
    NYA_ASSET_TYPE_SHADER_VERTEX,
    NYA_ASSET_TYPE_SHADER_FRAGMENT,
    NYA_ASSET_TYPE_SHADER_COMPUTE,
    NYA_ASSET_TYPE_BUFFER_VERTEX,
    NYA_ASSET_TYPE_BUFFER_INDEX,
    NYA_ASSET_TYPE_BUFFER_UNIFORM,

    // things that are made up of other assets
    NYA_ASSET_TYPE_GRAPHICS_PIPELINE,
    NYA_ASSET_TYPE_COMPUTE_PIPELINE,

    NYA_ASSET_TYPE_COUNT,
};

enum NYA_AssetLoadStatus {
    NYA_ASSET_STATUS_UNLOADED,
    NYA_ASSET_STATUS_LOADING,
    NYA_ASSET_STATUS_LOADED,

    /**
     * Could not be loaded, and will not be retried.
     *
     * A baked asset failing is a build problem; an external one failing is ordinary, because the
     * file came from outside the game and may have moved. Either way the engine keeps running.
     * */
    NYA_ASSET_STATUS_FAILED,

    NYA_ASSET_STATUS_COUNT,
};

struct NYA_AssetLoadParameters {
    NYA_AssetType   type;
    NYA_AssetHandle handle;

    /**
     * Load from the filesystem regardless of which backend this build uses.
     *
     * The default is a *baked* asset: the handle names something the build system indexed, and a
     * shipping build resolves it against the embedded blob. An external asset is one that did not
     * exist at build time — a file the player dropped on the window, a mod, a save thumbnail — so
     * there is nothing in the blob to find and the path has to be read at runtime.
     *
     * This is what makes such a load behave the same in a development build and a shipped one.
     * Without it a dropped file resolves in development, where the filesystem backend happily reads
     * any path, and fails in release, where the blob lookup misses.
     *
     * Being external also means the engine cannot vouch for the contents. The handle is a path from
     * outside the game, so treat a failure to load as ordinary rather than as a build error.
     * */
    b8 external;

    union {
        struct {
            u32 num_samplers;
            u32 num_storage_textures;
            u32 num_storage_buffers;
            u32 num_uniform_buffers;
        } as_shader;

        struct {
            NYA_Window*     window;
            NYA_AssetHandle vertex_shader_handle;
            NYA_AssetHandle fragment_shader_handle;
        } as_graphics_pipeline;

        struct {
            /** Point size. A font file carries no size of its own, so one face per size. */
            f32 point_size;
        } as_font;

        struct {
            /**
             * Decode the whole thing up front rather than streaming it.
             *
             * Right for a sound effect that has to start instantly and will play many times; wrong
             * for a music track, where it means holding the entire decoded stream in memory.
             * */
            b8 predecode;
        } as_sound;
    };
};

struct NYA_Asset {
    NYA_AssetType           type;
    NYA_AssetHandle         handle;
    NYA_AssetStatus         status;
    NYA_AssetLoadParameters load_parameters;

    union {
        struct {
            u8* data;
            u64 size;
        } as_text;

        struct {
            MIX_Audio* audio;
        } as_sound;

        struct {
            NYA_AssetHandle     compiled_handle;
            SDL_GPUShaderFormat format;
            SDL_GPUShader*      shader;
        } as_shader;

        struct {
            SDL_GPUGraphicsPipeline* pipeline;
        } as_graphics_pipeline;

        struct {
            SDL_GPUTexture* texture;
            u32             width;
            u32             height;
        } as_texture;

        struct {
            TTF_Font* font;
        } as_font;
    };

    atomic u64 reference_count;

    /**
     * Already sitting in the unloading queue.
     *
     * Without it a second unload request before the queue is processed enqueues the asset twice,
     * and the second pass releases GPU handles that were freed on the first.
     * */
    b8 queued_for_unload;

    /**
     * Came out of the embedded blob rather than off disk.
     *
     * Recorded per asset rather than inferred from the build, because with NYA_ASSET_PREFER_BLOB the two
     * sources coexist: the blob is consulted first and anything missing from it still comes off
     * disk. It decides both whether the bytes are owned (blob bytes live in the executable and must
     * not be freed) and whether the file is worth watching for changes.
     * */
    b8 from_blob;

#ifdef NYA_ASSET_HOT_RELOAD
    u64 source_modification_time;

    /** wait this many frames before actually doing the reload */
    u64 reload_grace_frames;
#endif // NYA_ASSET_HOT_RELOAD
};

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * FUNCTIONS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/*
 * ─────────────────────────────────────────────────────────
 * SYSTEM FUNCTIONS
 * ─────────────────────────────────────────────────────────
 */

NYA_API void nya_system_asset_init(void);
NYA_API void nya_system_asset_deinit(void);

/*
 * ─────────────────────────────────────────────────────────
 * ASSET FUNCTIONS
 * ─────────────────────────────────────────────────────────
 */

#define nya_asset_with(asset) if ((asset) && (asset)->status == NYA_ASSET_STATUS_LOADED)

NYA_API NYA_Asset* nya_asset_get(NYA_AssetHandle handle);

/*
 * Reference counting.
 *
 * An asset is unloaded when the last holder releases it, not when someone decides it is time. Every
 * acquire must be matched by exactly one release; both are safe to call from any thread.
 */

/** Errors rather than asserting if the handle is unknown: a typo'd handle should not end the process. */
NYA_API NYA_Error nya_asset_acquire(NYA_AssetHandle handle) __attr_no_discard;

/** Drops a reference and queues the asset for unloading if that was the last one. */
NYA_API void nya_asset_release(NYA_AssetHandle handle);

NYA_API u64 nya_asset_reference_count(NYA_AssetHandle handle) __attr_no_discard;

/**
 * Queues an asset for loading. The load itself happens at the end of the frame.
 *
 * Only the queueing is reported here; whether the bytes are actually readable is not known until
 * the load runs. Watch NYA_EVENT_ASSET_LOAD_FAILED, or check nya_asset_status, for that.
 * */
NYA_API NYA_Error nya_asset_load(NYA_AssetLoadParameters parameters) __attr_no_discard;

/**
 * Queues an asset for unloading, but only if nothing holds a reference to it.
 *
 * Returns false when it is still referenced, which is a normal answer rather than a failure: it
 * means somebody else is still using the thing you were done with. This used to assert on a
 * non-zero count, which turned "two systems share a texture" into a crash — and since assertions
 * are enabled in shipping builds, a crash in a player's hands.
 * */
NYA_API b8 nya_asset_unload(NYA_AssetHandle handle);

/**
 * Sets a window's icon from an asset, without the asset system taking it on.
 *
 * The bytes are read the same way any asset is — out of the blob when there is one, off disk
 * otherwise — handed to SDL, and released immediately. Nothing is registered, reference counted or
 * watched, because there is nothing to manage: SDL converts the image into its own surface and the
 * decoded copy belongs to the window from then on.
 *
 * Synchronous, unlike nya_asset_load, so it can be called right after nya_window_create rather than
 * waiting a frame for a queue to drain.
 *
 * Lives here rather than in core_window because the window system sits below the asset system and
 * cannot ask it for bytes.
 * */
NYA_API NYA_Error nya_asset_set_window_icon(NYA_WindowHandle window, NYA_AssetHandle handle) __attr_no_discard;

/** NYA_ASSET_STATUS_FAILED for anything that could not be loaded, so a caller can react without a hook. */
NYA_API NYA_AssetStatus nya_asset_status(NYA_AssetHandle handle) __attr_no_discard;
