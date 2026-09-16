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
typedef struct NYA_AssetBlobHeader     NYA_AssetBlobHeader;
typedef enum NYA_BlendMode NYA_BlendMode;
typedef enum NYA_VertexLayout          NYA_VertexLayout;
typedef struct NYA_AssetLoadParameters NYA_AssetLoadParameters;
typedef struct NYA_AssetSystem         NYA_AssetSystem;
typedef struct NYA_MeshPart            NYA_MeshPart;
typedef struct NYA_VertexSkinned3D     NYA_VertexSkinned3D;

/** One material's worth of a model: a run of triangles, a texture and a colour. Must be contiguous in
 * the index buffer, since each material needs its own texture bound per draw call. */
/** A vertex that can be skinned: NYA_Vertex3D plus who moves it. Separate type rather than extra bytes
 * on every vertex, since a static prop (of which there are thousands) pays nothing for skinning it
 * doesn't use — the same reasoning that keeps NYA_VERTEX_LAYOUT_3D and _3D_INSTANCED apart. */
struct NYA_VertexSkinned3D {
    f32x3     position;
    NYA_Color color;
    f32x3     normals;
    f32x2     uv;

    /** Which bones move this vertex, indices into the skeleton's palette. u32 rather than u8x4 to avoid
     * a second vertex attribute format for four bytes saved per vertex. */
    u32 bones[NYA_SKELETON_WEIGHTS_PER_VERTEX];

    /** How much each of them moves it. Normalised at load; see core_skeleton.h. */
    f32 weights[NYA_SKELETON_WEIGHTS_PER_VERTEX];
};

struct NYA_MeshPart {
    /** Where this part's vertices start in NYA_Asset.as_mesh, and how many. Vertices not indices — the
     * mesh is fully de-indexed, so a run of vertices is a run of triangles. Count must be a multiple of
     * three. */
    u32 first_vertex;
    u32 vertex_count;

    /** Index into NYA_Asset.as_mesh.textures this part samples, or -1 for none. Index rather than pointer
     * so parts sharing a material share the texture without either owning it. */
    s32 texture;

    /** The material's flat base colour, multiplied into the vertex colour. White when the material names
     * none. Not the whole material — no specular exponent or index of refraction; this shading model has
     * no use for them. */
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

    /** Owns every decoded sound. SDL_mixer needs a mixer before loading anything; held here rather than
     * per caller since audio outlives the track playing it. */
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

/** Which vertex struct a graphics pipeline reads. Baked into the pipeline at build time rather than
 * decided at bind time. Two exist because the 2D batch's vertex is a third the size of the general one. */
/** How a pipeline's output combines with what the target already holds. Baked into the pipeline at
 * creation — two blend modes means two pipelines — so this is a short enum of what a 2D game actually
 * draws, not the full set of factors and operations. */
enum NYA_BlendMode {
    /** Replace. What opaque geometry wants, and what costs least. */
    NYA_BLEND_NONE = 0,

    /** Straight alpha over the destination: SRC_ALPHA / ONE_MINUS_SRC_ALPHA. Value 1 deliberately, so the
     * older `.blend = true` still selects it. Needed for anything with a soft edge — glyph, fade,
     * translucent panel — or it draws as opaque pixel boxes instead. */
    NYA_BLEND_ALPHA = 1,

    /** Adds light rather than covering: SRC_ALPHA / ONE. For sparks, muzzle flashes, glows. Never
     * darkens — overlaps saturate toward white instead of stacking dark the way alpha would. */
    NYA_BLEND_ADDITIVE = 2,

    /** Multiplies into the destination: DST_COLOR / ZERO. What a light map is — darkens what a mostly-dark
     * texture covers, leaves bright parts alone. How 2D lighting works without a deferred pass; see
     * nya_render2d_lights_apply. */
    NYA_BLEND_MULTIPLY = 3,

    NYA_BLEND_MODE_COUNT,
};

enum NYA_VertexLayout {
    /** NYA_Vertex2D: position, uv, packed byte colour. Twenty bytes. What the 2D batch uses. First, so a
     * zeroed struct means this — it used to be second, behind a layout called STANDARD that silently fed
     * a 2D shader sixty-four byte strides and drew its geometry off screen. The layout actually used
     * should be the default. */
    NYA_VERTEX_LAYOUT_2D,

    /** NYA_Vertex3D: position, colour, normal, uv. Thirty-six bytes; see that struct for the packing. */
    NYA_VERTEX_LAYOUT_3D,

    /**
     * NYA_Vertex3DDepth: position alone. Twelve bytes, and what the immediate shadow pass uploads.
     *
     * A shadow pass writes depth and reads nothing else, so the uv, normal and colour it was being handed
     * were twenty-four bytes per vertex uploaded across PCIe and discarded by the input assembler — three
     * times over, once per cascade. Only the *immediate* shadow pipeline uses it: the instanced one draws
     * out of a buffer uploaded once at registration, which the camera pass reads too and so must stay wide.
     * */
    NYA_VERTEX_LAYOUT_3D_DEPTH,

    /** NYA_Vertex3D in buffer 0, NYA_Render3DInstance in buffer 1, stepped per *instance*. What the
     * retained mesh path draws with — buffer 1 carries a model matrix and tint, letting one upload of a
     * model be drawn a hundred times in one draw call, which the immediate batch structurally cannot do.
     * The matrix arrives as four FLOAT4 attributes at locations 4 through 7 (a vertex attribute is at
     * most four components); the shader reassembles it. */
    NYA_VERTEX_LAYOUT_3D_INSTANCED,

    /** NYA_VertexSkinned3D. The 3D layout plus bone indices and weights. See nya_render3d_skinned_mesh. */
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

    /** A 3D model, read with ufbx. See nya_render3d_mesh. Holds triangles on the CPU, nothing on the
     * GPU — deliberate, since the 3D batch already owns the vertex buffer, pipeline and upload; a mesh
     * with its own would cost a draw call per model and opt out of batching. */
    NYA_ASSET_TYPE_MESH,
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

    /** Could not be loaded, and will not be retried. A baked asset failing is a build problem; an
     * external one failing is ordinary — the file came from outside the game and may have moved. Either
     * way the engine keeps running. */
    NYA_ASSET_STATUS_FAILED,

    NYA_ASSET_STATUS_COUNT,
};

struct NYA_AssetLoadParameters {
    NYA_AssetType   type;
    NYA_AssetHandle handle;

    /** The file to read, when it is not the handle itself. A handle is normally the path, so a file
     * could only be loaded once — wrong when load parameters matter, e.g. one .ttf at two point sizes is
     * two assets that can't share a path key. Null keeps the old behaviour: handle is the path. */
    NYA_ConstCString source;

    /** Load from the filesystem regardless of build backend. Default is a *baked* asset resolved against
     * the embedded blob; external is one that didn't exist at build time — a dropped file, a mod, a save
     * thumbnail — so it must be read at runtime. Without this, a dropped file resolves in development
     * (which reads any path) but fails in release (blob lookup misses). Also means the engine cannot vouch
     * for the contents — treat a load failure as ordinary, not a build error. */
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

            /** How the output combines with the target. See NYA_BlendMode. Defaults to NYA_BLEND_NONE.
             * ALPHA is deliberately value 1 so the older `.blend = true` spelling still selects it without
             * any call site changing. */
            NYA_BlendMode blend;

            /** Which vertex struct the vertex shader is written against. Defaults to NYA_VERTEX_LAYOUT_2D.
             * Getting this wrong is neither a compile nor validation error — the shader reads whatever
             * bytes the stride lands on and geometry ends up in nonsense positions — so the default is
             * chosen to usually be right. */
            NYA_VertexLayout vertex_layout;

            /** Whether this pipeline reads the depth buffer and refuses fragments behind what is there.
             * Off for 2D, which keeps painter's-order drawing working — on would let a HUD element lose to
             * the world drawn before it. On for 3D, where geometry occludes itself without sorting. */
            b8 depth_test;

            /** Whether this pipeline writes the depth it passed. Usually matches `depth_test`; separate
             * because transparent 3D geometry must test against the opaque pass but not write, or the
             * nearest transparent surface would hide the ones behind it. */
            b8 depth_write;

            /** Discard back faces, front decided by counter-clockwise winding. Off by default — the 2D
             * batch emits both windings and would lose half its triangles. On for closed 3D geometry,
             * halving fragment work for free. */
            b8 cull_back_faces;

            /** Discard *front* faces instead. For a shadow pass, and little else — recording the far side
             * of each object moves the depth away from the tested surface, the cheapest defence against
             * shadow acne. Wrong for open geometry, which then casts nothing (usually right for a floor).
             * Ignored when `cull_back_faces` is also set. */
            b8 cull_front_faces;

            /** The colour target's format. Zero means the window's swapchain format. A pipeline compiled
             * for the swapchain is rejected at bind time against any other target, so only needed for an
             * offscreen target of unusual format — the shadow map's R32_FLOAT. */
            SDL_GPUTextureFormat color_format;

            /** Force one sample per pixel, whatever the renderer is using. A pipeline's sample count must
             * match its target; the renderer's multisampled default is wrong for the shadow map, where
             * averaging two depths would describe neither surface. */
            b8 single_sampled;
        } as_graphics_pipeline;

        struct {
            /**
             * How this image is sampled. Defaults to linear; pixel art wants nearest.
             * */
            NYA_TextureFilter filter;

            /**
             * Rasterize a vector image at this size instead of its natural one.
             * */
            u32 width;
            u32 height;

            /**
             * What `currentColor` rasterises to, for an SVG that uses it.
             * */
            NYA_Color svg_color;
        } as_texture_load;

        struct {
            /**
             * How the model's embedded texture is filtered. Zero is NYA_TEXTURE_FILTER_LINEAR.
             * */
            NYA_TextureFilter filter;
        } as_mesh_load;

        struct {
            /** Point size. A font file carries no size of its own, so one face per size. */
            f32 point_size;
        } as_font;

        struct {
            /**
             * Decode the whole thing up front rather than streaming it.
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

            /** Carried from the load parameters, because the draw path picks the sampler from it. */
            NYA_TextureFilter filter;
        } as_texture;

        struct {
            TTF_Font* font;
        } as_font;

        /**
         * Triangles, flattened and de-indexed by ufbx into one array per attribute.
         * */
        struct {
            f32x3* positions;
            f32x3* normals;

            /** Parallel to the other two. Zeroed for a model with no UV set, which samples one texel. */
            f32x2* uvs;

            /**
             * How many vertices the three arrays hold.
             * */
            u32 vertex_count;

            /**
             * How many elements each array was allocated for, which is not always how many were written.
             * */
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
             * ── skinning ──
             *
             * Present only when the file carried a skin deformer. A mesh is either skinned or it is
             * not: `skinned_vertices` and `vertices` are the same geometry in two layouts and only
             * one of them is filled, because a static mesh should not pay for the wider vertex and a
             * skinned one has no use for the narrower.
             */

            /** Null unless the file was rigged. Owned by this asset. */
            NYA_Skeleton* skeleton;

            /** Parallel to `vertices` when skinned, and null otherwise. */
            NYA_VertexSkinned3D* skinned_vertices;
        } as_mesh;
    };

    atomic u64 reference_count;

    /**
     * Already sitting in the unloading queue.
     * */
    b8 queued_for_unload;

    /**
     * Came out of the embedded blob rather than off disk.
     * */
    b8 from_blob;

#ifdef NYA_ASSET_HOT_RELOAD
    u64 source_modification_time;

    /** wait this many frames before actually doing the reload */
    u64 reload_grace_frames;

    /**
     * Uptime at which this asset's file may next be stat'd. See _NYA_ASSET_STAT_INTERVAL_NS.
     *
     * Per asset rather than global, so one asset checked often does not starve the rest.
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

/*
 * Reference counting.
 */

/** Errors rather than asserting if the handle is unknown: a typo'd handle should not end the process. */
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

/**
 * Sets a window's icon from an asset, without the asset system taking it on.
 * */
NYA_API NYA_Error nya_asset_set_window_icon(NYA_WindowHandle window, NYA_AssetHandle handle) __attr_no_discard;

/** NYA_ASSET_STATUS_FAILED for anything that could not be loaded, so a caller can react without a hook. */
NYA_API NYA_AssetStatus nya_asset_status(NYA_AssetHandle handle) __attr_no_discard;

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
 * ── the baked index, directly ──
 *
 * nya_asset_enumerate answers "what assets are there", which is what a picker asks. These answer
 * "what is baked into *this binary*, and how big is it" — which is a different question, and one
 * only the blob can answer: a disk walk knows paths but not what was actually shipped, and neither
 * knows an entry's size without opening the file.
 *
 * For tooling rather than for gameplay: a bundle report, a size breakdown, a check that something
 * made it into the build. All three return nothing in a build without NYA_ASSET_PREFER_BLOB, so a
 * caller compiles everywhere and simply finds an empty index where there is no blob.
 */

/** How many assets are baked in. Zero when this build has no blob. */
NYA_API u64 nya_asset_blob_count(void) __attr_no_discard;

/** The baked entry at `index`, or null past the end. Carries the path, its offset and its size. */
NYA_API const NYA_AssetBlobHeader* nya_asset_blob_at(u64 index) __attr_no_discard;

/** The baked entry for `path`, or null. The path is spelled as the index spells it, leading "./" and all. */
NYA_API const NYA_AssetBlobHeader* nya_asset_blob_find(NYA_ConstCString path) __attr_no_discard;
