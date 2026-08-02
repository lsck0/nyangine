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

    NYA_DictᐸNYA_Assetᐳ*                assets;
    NYA_ArrayᐸNYA_AssetLoadParametersᐳ* loading_queue;
    NYA_ArrayᐸNYA_AssetHandleᐳ*         unloading_queue;

#ifdef NYA_ASSET_BACKEND_FS
    NYA_ArrayᐸNYA_AssetHandleᐳ* reload_queue;
#endif // NYA_ASSET_BACKEND_FS
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
        } as_sound;

        struct {
            NYA_AssetHandle     compiled_handle;
            SDL_GPUShaderFormat format;
            SDL_GPUShader*      shader;
        } as_shader;

        struct {
            SDL_GPUGraphicsPipeline* pipeline;
        } as_graphics_pipeline;
    };

    atomic u64 reference_count;

#ifdef NYA_ASSET_BACKEND_FS
    u64 source_modification_time;

    /** wait this many frames before actually doing the reload */
    u64 reload_grace_frames;
#endif // NYA_ASSET_BACKEND_FS
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
NYA_API void       nya_asset_acquire(NYA_AssetHandle handle);
NYA_API void       nya_asset_release(NYA_AssetHandle handle);
NYA_API void       nya_asset_load(NYA_AssetLoadParameters parameters);
NYA_API void       nya_asset_unload(NYA_Asset* asset);
