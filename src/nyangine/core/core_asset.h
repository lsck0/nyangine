#pragma once

#include "nyangine/core/core_skeleton.h"

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
#include "generated/assets.h"
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
typedef struct NYA_AssetBlobExpanded   NYA_AssetBlobExpanded;
typedef struct NYA_AssetBlobHeader     NYA_AssetBlobHeader;
typedef enum NYA_BlendMode NYA_BlendMode;
typedef enum NYA_VertexLayout          NYA_VertexLayout;
typedef struct NYA_AssetLoadParameters NYA_AssetLoadParameters;
typedef struct NYA_AssetSystem         NYA_AssetSystem;
typedef struct NYA_MeshPart            NYA_MeshPart;
typedef struct NYA_VertexSkinned3D     NYA_VertexSkinned3D;

/** One material's worth of a model: a run of triangles, a texture and a colour. Must be contiguous in
 * the index buffer, since each material needs its own texture bound per draw call. */
/**
 * A vertex that can be skinned: NYA_Vertex3D's layout plus bone indices and weights, 44 bytes. A separate type, so
 * static props do not pay for skinning.
 * */
struct NYA_VertexSkinned3D {
    f32 position[3];
    f16 uv[2];
    f32 normals[3];
    f16 color[4];

    /** Palette indices of the bones moving this vertex, UBYTE4. */
    u8 bones[NYA_SKELETON_WEIGHTS_PER_VERTEX];

    /** How much each bone moves it, UBYTE4_NORM. Sums to exactly 255. */
    u8 weights[NYA_SKELETON_WEIGHTS_PER_VERTEX];
};

static_assert(sizeof(NYA_VertexSkinned3D) == 44, "the skinned layout in core_asset.c describes a 44 byte vertex");
static_assert(NYA_SKELETON_MAX_BONES <= 256, "a skinned vertex indexes bones with a byte");

struct NYA_MeshPart {
    /**
     * Where this part's vertices start in NYA_Asset.as_mesh, and how many. The mesh is de-indexed, so a run of
     * vertices is a run of triangles; the count is a multiple of three.
     * */
    u32 first_vertex;
    u32 vertex_count;

    /** Index into NYA_Asset.as_mesh.textures, or -1 for none, so parts can share a texture. */
    s32 texture;

    /** The material's base colour, multiplied into the vertex colour. White when the material names none. */
    NYA_Color base_color;
};
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

    /** Owns every decoded sound. A mixer must exist before loading, and audio outlives the track playing it. */
    MIX_Mixer* mixer;

    NYA_DictᐸNYA_Assetᐳ*                assets;
    NYA_ArrayᐸNYA_AssetLoadParametersᐳ* loading_queue;
    NYA_ArrayᐸNYA_AssetHandleᐳ*         unloading_queue;

    /**
     * Expanded copies of compressed blob entries, one per entry, shared by every asset reading those bytes (several
     * font sizes read one file). Null until the first compressed entry loads.
     * */
    NYA_AssetBlobExpanded* blob_expanded;

#ifdef NYA_ASSET_HOT_RELOAD
    NYA_ArrayᐸNYA_AssetHandleᐳ* reload_queue;
#endif // NYA_ASSET_HOT_RELOAD
};

/*
 * ─────────────────────────────────────────────────────────
 * ASSET STRUCTS
 * ─────────────────────────────────────────────────────────
 */

struct NYA_AssetBlobExpanded {
    u8* data;
    u32 references;
};

struct NYA_AssetBlobHeader {
    NYA_ConstCString path;

    /** The entry's bytes inside the executable, `compressed_size` of them. */
    const u8* data;

    /** The asset's real size, whatever form it is stored in. */
    u64 size;

    /**
     * Bytes the entry occupies in the blob; equal to `size` when stored verbatim. Entries are only kept compressed
     * when that is smaller, so PNG and OGG stay verbatim and load without a copy.
     * */
    u64 compressed_size;
};

/** Which vertex struct a graphics pipeline reads, baked in when the pipeline is built. */
/**
 * How a pipeline's output combines with the target. Baked into the pipeline, so this lists what a 2D game
 * draws rather than every blend factor.
 * */
enum NYA_BlendMode {
    /** Replace. For opaque geometry, and the cheapest. */
    NYA_BLEND_NONE = 0,

    /**
     * Straight alpha: SRC_ALPHA / ONE_MINUS_SRC_ALPHA. Value 1, so `.blend = true` selects it. Needed for anything
     * with a soft edge.
     * */
    NYA_BLEND_ALPHA = 1,

    /** Additive: SRC_ALPHA / ONE, for sparks and glows. Overlaps saturate toward white. */
    NYA_BLEND_ADDITIVE = 2,

    /** Multiply: DST_COLOR / ZERO. A light map darkens what it covers; see nya_render2d_lights_apply. */
    NYA_BLEND_MULTIPLY = 3,

    NYA_BLEND_MODE_COUNT,
};

enum NYA_VertexLayout {
    /**
     * NYA_Vertex2D: position, uv, packed colour, twenty bytes. The 2D batch's layout, and first so a zeroed
     * struct selects it.
     * */
    NYA_VERTEX_LAYOUT_2D,

    /** NYA_Vertex3D: position, colour, normal, uv, 36 bytes. */
    NYA_VERTEX_LAYOUT_3D,


    /**
     * NYA_Vertex3D in buffer 0 and NYA_Render3DInstance in buffer 1, stepped per instance, so one upload draws
     * many copies in one call. The matrix arrives as four FLOAT4 attributes at locations 4 to 7.
     * */
    NYA_VERTEX_LAYOUT_3D_INSTANCED,

    /** NYA_VertexSkinned3D: the 3D layout plus bone indices and weights. */
    NYA_VERTEX_LAYOUT_3D_SKINNED,

    NYA_VERTEX_LAYOUT_COUNT,
};

enum NYA_AssetType {
    // raw data
    NYA_ASSET_TYPE_TEXT,

    // processed data on cpu ram
    NYA_ASSET_TYPE_SOUND,
    NYA_ASSET_TYPE_FONT,

    // processed data on gpu vram
    NYA_ASSET_TYPE_TEXTURE,

    /** A `.cube` colour lookup table, as a 3D texture. See nya_lut_parse and effect_lut.frag.hlsl. */
    NYA_ASSET_TYPE_LUT,

    /** A 3D model read with ufbx. See nya_render3d_mesh. */
    NYA_ASSET_TYPE_MESH,
    NYA_ASSET_TYPE_SHADER_VERTEX,
    NYA_ASSET_TYPE_SHADER_FRAGMENT,
    NYA_ASSET_TYPE_SHADER_COMPUTE,
    NYA_ASSET_TYPE_BUFFER_VERTEX,
    NYA_ASSET_TYPE_BUFFER_INDEX,
    NYA_ASSET_TYPE_BUFFER_UNIFORM,

    // made of other assets
    NYA_ASSET_TYPE_GRAPHICS_PIPELINE,
    NYA_ASSET_TYPE_COMPUTE_PIPELINE,

    NYA_ASSET_TYPE_COUNT,
};

enum NYA_AssetLoadStatus {
    NYA_ASSET_STATUS_UNLOADED,
    NYA_ASSET_STATUS_LOADING,
    NYA_ASSET_STATUS_LOADED,

    /**
     * Could not be loaded, and will not be retried. For a baked asset that is a build problem; an external file
     * may simply have moved. The engine keeps running either way.
     * */
    NYA_ASSET_STATUS_FAILED,

    NYA_ASSET_STATUS_COUNT,
};

struct NYA_AssetLoadParameters {
    NYA_AssetType   type;
    NYA_AssetHandle handle;

    /**
     * The file to read, when it is not the handle. One .ttf at two point sizes is two assets, which cannot both
     * use the path as their handle. Null means the handle is the path.
     * */
    NYA_ConstCString source;

    /**
     * Load from the filesystem in every build. Baked assets resolve against the embedded blob; an external one
     * (a dropped file, a mod, a save thumbnail) did not exist at build time. Its contents are not the engine's,
     * so a failure is ordinary.
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

            /** How the output combines with the target. See NYA_BlendMode. Defaults to NYA_BLEND_NONE. */
            NYA_BlendMode blend;

            /**
             * Which vertex struct the vertex shader expects. Defaults to NYA_VERTEX_LAYOUT_2D. A mismatch produces no
             * error, only garbage positions.
             * */
            NYA_VertexLayout vertex_layout;

            /** Whether this pipeline depth tests. Off for 2D, which draws in painter's order; on for 3D. */
            b8 depth_test;

            /**
             * Whether this pipeline writes depth. Separate from `depth_test` because transparent geometry tests against
             * the opaque pass without writing.
             * */
            b8 depth_write;

            /**
             * Discard back faces (front is counter-clockwise). Off by default: the 2D batch emits both windings. On for
             * closed 3D geometry.
             * */
            b8 cull_back_faces;

            /**
             * Discard front faces instead, for a shadow pass: recording far sides moves the depth away from lit surfaces,
             * the cheapest defence against acne. Open geometry then casts nothing. Ignored when `cull_back_faces` is set.
             * */
            b8 cull_front_faces;

            /**
             * The colour target format; zero means the swapchain's. Needed for offscreen targets of another format, such
             * as the shadow map.
             * */
            SDL_GPUTextureFormat color_format;

            /**
             * Force one sample per pixel. The sample count must match the target, and averaging depths in a shadow map
             * describes neither surface.
             * */
            b8 single_sampled;
        } as_graphics_pipeline;

        struct {
            /**
             * How this image is sampled. Defaults to linear; pixel art wants nearest.
             * */
            NYA_TextureFilter filter;

            /** Rasterize a vector image at this size instead of its natural one. */
            u32 width;
            u32 height;

            /** What `currentColor` rasterises to in an SVG. */
            NYA_Color svg_color;
        } as_texture_load;

        struct {
            /**
             * How the model's embedded texture is filtered. Zero is NYA_TEXTURE_FILTER_LINEAR.
             * */
            NYA_TextureFilter filter;
        } as_mesh_load;

        struct {
            /** Point size. A font file has no size, so there is one face per size. */
            f32 point_size;
        } as_font;

        struct {
            /** Decode the whole sound up front rather than streaming. */
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
            /**
             * One per kind of target: [0] single sampled, [1] at the renderer's sample count, and [2] and [3] the same
             * with the scene normal buffer as a second colour target. Built on first use and rebuilt when the sample
             * count or depth format they were built for changes. Reach them through nya_asset_graphics_pipeline.
             * */
            struct {
                SDL_GPUGraphicsPipeline* pipeline;
                SDL_GPUSampleCount       sample_count;
                SDL_GPUTextureFormat     depth_format;

                /** Tried, even if SDL refused, so a refusal is logged once. */
                b8 built;
            } variants[4];
        } as_graphics_pipeline;

        struct {
            SDL_GPUTexture* texture;
            u32             width;
            u32             height;

            /** Carried from the load parameters, because the draw path picks the sampler from it. */
            NYA_TextureFilter filter;
        } as_texture;

        struct {
            TTF_Font* font;
        } as_font;

        struct {
            /** RGBA8, `size` texels along each of three axes. */
            SDL_GPUTexture* texture;
            u32             size;
        } as_lut;

        /** Triangles, de-indexed by ufbx into one array per attribute. */
        struct {
            f32x3* positions;
            f32x3* normals;

            /** Parallel to the other two. Zeroed for a model without UVs. */
            f32x2* uvs;

            /**
             * How many vertices the three arrays hold.
             * */
            u32 vertex_count;

            /** Elements each array was allocated for, which can exceed how many were written. */
            u32 allocated;

            /**
             * One entry per material, each naming a contiguous run of `indices`.
             * */
            NYA_MeshPart* parts;
            u32           part_count;

            /**
             * The same vertices on the GPU, uploaded once and kept.
             * */
            SDL_GPUBuffer* gpu_vertices;

            /** How many vertices `gpu_vertices` holds. Zero while it is null. */
            u32 gpu_vertex_count;

            /**
             * The model's axis-aligned bounds, computed once on first request. See nya_render3d_mesh_bounds.
             * */
            f32x3 bounds_min;
            f32x3 bounds_max;
            b8    bounds_valid;

            /**
             * How many parts were reserved, which is at least `part_count`.
             * */
            u32 part_capacity;

            /**
             * Every distinct texture the parts refer to, owned by this asset.
             * */
            SDL_GPUTexture** textures;
            u32              texture_count;

            /** Carried from the load parameters, because the draw path picks the sampler from it. */
            NYA_TextureFilter filter;

            /*
             * Skinning, only when the file has a skin deformer. `skinned_vertices` and `vertices` are the same geometry in
             * two layouts, and only one is filled.
             */

            /** Null unless the file was rigged. Owned by this asset. */
            NYA_Skeleton* skeleton;

            /** Parallel to `vertices` when skinned, and null otherwise. */
            NYA_VertexSkinned3D* skinned_vertices;
        } as_mesh;
    };

    atomic u64 reference_count;

    /**
     * Which load this is, unique across the run and set each time the asset finishes loading. Whatever a
     * consumer derives from an asset (a glyph atlas) is tagged with it, so a reload reads as stale.
     * */
    u64 generation;

    /**
     * Already sitting in the unloading queue.
     * */
    b8 queued_for_unload;

    /**
     * Came out of the embedded blob. Decides whether there is a file to watch for hot reload; ownership is
     * `raw_owned`.
     * */
    b8 from_blob;

    /**
     * `raw.data` is an allocation this asset owns, and `_nya_asset_unload_raw` frees it. True for
     * anything read off disk; false for a blob entry, which is either `.rodata` or a shared expansion.
     * */
    b8 raw_owned;

    /** `raw.data` is a shared expansion of blob entry `raw_blob_index`, released on unload. */
    b8  raw_shared;
    u32 raw_blob_index;

    /**
     * The encoded bytes, kept outside the union: as_font.font and as_sound.audio share storage with as_text.data
     * and overwrite it once decoded.
     * */
    struct {
        u8* data;
        u64 size;
    } raw;

#ifdef NYA_ASSET_HOT_RELOAD
    u64 source_modification_time;

    /** frames to wait before reloading */
    u64 reload_grace_frames;

    /**
     * Uptime at which this asset's file may next be stat'd. Per asset, so a busy asset does not starve the rest.
     * See _NYA_ASSET_STAT_INTERVAL_NS.
     * */
    u64 next_stat_time_ns;
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

/**
 * Reads an asset's raw bytes into `arena`, right now, without registering it.
 * */
NYA_API NYA_Error nya_asset_read(NYA_Arena* arena, NYA_AssetHandle handle, OUT u8** out_data, OUT u64* out_size) __attr_no_discard;

/* Reference counting. */

/** Errors on an unknown handle rather than asserting. */
NYA_API NYA_Error nya_asset_acquire(NYA_AssetHandle handle) __attr_no_discard;

/** Drops a reference and queues the asset for unloading if that was the last one. */
NYA_API void nya_asset_release(NYA_AssetHandle handle);

NYA_API u64 nya_asset_reference_count(NYA_AssetHandle handle) __attr_no_discard;

/**
 * Queues an asset for loading. The load itself happens at the end of the frame.
 * */
NYA_API NYA_Error nya_asset_load(NYA_AssetLoadParameters parameters) __attr_no_discard;

/**
 * Queues an asset for unloading, but only if nothing holds a reference to it.
 * */
NYA_API b8 nya_asset_unload(NYA_AssetHandle handle);

/** Loads everything queued now rather than at the end of the frame. For startup, before a window shows. */
NYA_API void nya_asset_load_queued(void);

/**
 * Sets a window's icon from an asset, without the asset system taking it on.
 * */
NYA_API NYA_Error nya_asset_set_window_icon(NYA_WindowHandle window, NYA_AssetHandle handle) __attr_no_discard;

/** NYA_ASSET_STATUS_FAILED for anything that could not load, so a caller can react without a hook. */
NYA_API NYA_AssetStatus nya_asset_status(NYA_AssetHandle handle) __attr_no_discard;

/**
 * The pipeline to bind for a target of `sample_count`, built the first time a target of that kind asks, so a single
 * sampled render texture or a changed MSAA setting needs no second asset. Null while the asset is not loaded or when
 * SDL refuses the build. A `single_sampled` pipeline ignores `sample_count`.
 *
 * `normals` is for a pass that also attaches the scene normal buffer (NYA_RENDER3D_NORMAL_FORMAT) as a second colour
 * target. That build writes it only if the pipeline writes depth, so translucent geometry and the sky leave it alone.
 * */
NYA_API SDL_GPUGraphicsPipeline* nya_asset_graphics_pipeline(NYA_Asset* asset, SDL_GPUSampleCount sample_count, b8 normals) __attr_no_discard;

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * ENUMERATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/**
 * Every asset path, optionally filtered by suffix, sorted.
 * */
NYA_API NYA_ArrayᐸNYA_Stringᐳ* nya_asset_enumerate(NYA_Arena* arena, NYA_ConstCString suffix) __attr_no_discard;

/*
 * The baked index. nya_asset_enumerate lists what assets exist; these list what this binary ships and how big
 * each entry is, which only the blob knows. For tooling. Empty in a build without NYA_ASSET_PREFER_BLOB.
 */

/** How many assets are baked in. Zero when this build has no blob. */
NYA_API u64 nya_asset_blob_count(void) __attr_no_discard;

/** The baked entry at `index`, or null past the end. Carries the path, its offset and its size. */
NYA_API const NYA_AssetBlobHeader* nya_asset_blob_at(u64 index) __attr_no_discard;

/** The baked entry for `path`, or null. Spelled as the index spells it, with the leading "./". */
NYA_API const NYA_AssetBlobHeader* nya_asset_blob_find(NYA_ConstCString path) __attr_no_discard;
