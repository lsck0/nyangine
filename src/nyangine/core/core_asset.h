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
typedef enum NYA_BlendMode NYA_BlendMode;
typedef enum NYA_VertexLayout          NYA_VertexLayout;
typedef struct NYA_AssetLoadParameters NYA_AssetLoadParameters;
typedef struct NYA_AssetSystem         NYA_AssetSystem;
typedef struct NYA_MeshPart            NYA_MeshPart;
typedef struct NYA_VertexSkinned3D     NYA_VertexSkinned3D;

/**
 * One material's worth of a model: a run of triangles, a texture and a colour.
 *
 * The unit a textured model is drawn in. An FBX may hold several materials over one mesh, and each needs
 * its own texture bound — which is per draw call, so the run has to be contiguous in the index buffer
 * for the draw to be one range rather than a scatter.
 * */
/**
 * A vertex that can be skinned: NYA_Vertex3D plus who moves it.
 *
 * A separate type rather than twenty spare bytes on every vertex in the engine. A static prop pays
 * nothing for skinning it does not use, which matters because props are what there are thousands of
 * — the same reasoning that keeps NYA_VERTEX_LAYOUT_3D and _3D_INSTANCED apart.
 * */
struct NYA_VertexSkinned3D {
    f32x3     position;
    NYA_Color color;
    f32x3     normals;
    f32x2     uv;

    /**
     * Which bones move this vertex, as indices into the skeleton's palette.
     *
     * u32 rather than something narrower: a u8x4 would fit and would need its own vertex attribute
     * format, and the four bytes saved per vertex are not worth a second way for the layout to be
     * wrong.
     * */
    u32 bones[NYA_SKELETON_WEIGHTS_PER_VERTEX];

    /** How much each of them moves it. Normalised at load; see core_skeleton.h. */
    f32 weights[NYA_SKELETON_WEIGHTS_PER_VERTEX];
};

struct NYA_MeshPart {
    /**
     * Where this part's vertices start in NYA_Asset.as_mesh, and how many there are.
     *
     * Vertices rather than indices, because the mesh is fully de-indexed: every corner of every triangle
     * is its own vertex, so a run of vertices *is* a run of triangles and there is no indirection between
     * them. A count that is not a multiple of three would be a malformed part.
     * */
    u32 first_vertex;
    u32 vertex_count;

    /**
     * Which of NYA_Asset.as_mesh.textures this part samples, or -1 for none.
     *
     * An index rather than a pointer so that two parts sharing a material share the texture without
     * either of them owning it. See the note on as_mesh.textures.
     * */
    s32 texture;

    /**
     * The material's flat base colour, multiplied into the vertex colour.
     *
     * White when the material names none. It is not the whole material: this shading model has no use
     * for a specular exponent or an index of refraction, and reading them in order to ignore them would
     * suggest they do something.
     * */
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

/**
 * Which vertex struct a graphics pipeline reads.
 *
 * A pipeline bakes in the layout of the buffer it will be fed, so this has to be decided when the
 * pipeline is built rather than when it is bound. Two, because the 2D batch wants a vertex a third
 * the size of the general one and there is no reason for either to carry the other's fields.
 * */
/**
 * How a pipeline's output combines with what the target already holds.
 *
 * A property of the pipeline rather than of a draw, because it is baked in at creation — two blend
 * modes is two pipelines, which is exactly why this is a short enum rather than the full set of
 * factors and operations. Each of these earns its place by being something a 2D game actually draws.
 * */
enum NYA_BlendMode {
    /** Replace. What opaque geometry wants, and what costs least. */
    NYA_BLEND_NONE = 0,

    /**
     * Straight alpha over the destination: SRC_ALPHA / ONE_MINUS_SRC_ALPHA.
     *
     * One, deliberately, so that the `.blend = true` this replaced still selects it. Anything with a
     * soft edge needs it: an anti-aliased glyph, a fade, a translucent panel. Without it those draw
     * their background as opaque pixels, so text sits in rectangular boxes of whatever the colour
     * buffer happened to hold.
     * */
    NYA_BLEND_ALPHA = 1,

    /**
     * Adds light rather than covering: SRC_ALPHA / ONE.
     *
     * What a spark, a muzzle flash or a glow is. Never darkens, so overlapping ones saturate toward
     * white instead of stacking into a dark blob the way alpha would.
     * */
    NYA_BLEND_ADDITIVE = 2,

    /**
     * Multiplies into the destination: DST_COLOR / ZERO.
     *
     * What a light map is. Drawing a mostly-dark texture over a finished scene darkens everything it
     * covers and leaves the bright parts alone, which is how 2D lighting is done without a deferred
     * pass — see nya_render2d_lights_apply.
     * */
    NYA_BLEND_MULTIPLY = 3,

    NYA_BLEND_MODE_COUNT,
};

enum NYA_VertexLayout {
    /**
     * NYA_Vertex2D: position, uv, packed byte colour. Twenty bytes. What the 2D batch uses.
     *
     * First, and therefore what a zeroed struct means. It used to be second, behind a layout called
     * STANDARD — which named a 3D vertex "standard" and then made it the default for every pipeline,
     * so a 2D shader that simply did not mention the field was silently fed sixty-four byte strides
     * and drew its geometry off screen. The layout the engine actually draws with is the one a
     * caller should get for free.
     * */
    NYA_VERTEX_LAYOUT_2D,

    /** NYA_Vertex3D: position, colour, normal, uv, all floats. Sixty-four bytes. */
    NYA_VERTEX_LAYOUT_3D,

    /**
     * NYA_Vertex3D in buffer 0 and NYA_Render3DInstance in buffer 1, stepped per *instance*.
     *
     * The layout the retained mesh path draws with. Buffer 1 carries a model matrix and a tint, which is
     * what lets one upload of a model be drawn a hundred times with one draw call — the thing the
     * immediate batch structurally cannot do, because it has no per-primitive anything.
     *
     * The matrix arrives as four FLOAT4 attributes at locations 4 through 7, because a vertex attribute
     * is at most four components and there is no matrix element format. The shader reassembles it.
     * */
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

    /**
     * A 3D model, read with ufbx. See nya_render3d_mesh.
     *
     * Holds triangles on the CPU and nothing on the GPU, which is deliberate and is what
     * render3d.h means by "a loader belongs on top of this, not inside it": the 3D batch already owns
     * the vertex buffer, the pipeline and the upload, so a mesh that brought its own would be a second
     * draw call per model and would opt out of every bit of batching the renderer does.
     * */
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
     * The file to read, when it is not the handle itself.
     *
     * A handle is normally the path, which means one file can be loaded exactly once — and that is
     * wrong for anything whose load parameters matter. A font is the case that forced it: a face
     * carries no size, so one .ttf at two point sizes is two assets, and they cannot both be keyed
     * on the same path.
     *
     * Null keeps the old behaviour, where the handle is the path.
     * */
    NYA_ConstCString source;

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

            /**
             * How the output combines with what is already in the target. See NYA_BlendMode.
             *
             * NYA_BLEND_NONE by default, which is what opaque geometry wants and what costs least.
             *
             * Typed as an enum whose ALPHA is one, so the older `.blend = true` spelling still means
             * exactly what it always did — the flag became a mode without a single call site having
             * to change, which is why the enum is numbered that way round rather than alphabetically.
             * */
            NYA_BlendMode blend;

            /**
             * Which vertex struct the vertex shader is written against.
             *
             * Defaults to NYA_VERTEX_LAYOUT_2D, which is what everything the engine draws for itself
             * uses. Getting this wrong is not a compile error and not a validation error either —
             * the shader reads whatever bytes the stride lands on, and the result is geometry in
             * nonsense positions, so the default is the one that is usually right.
             * */
            NYA_VertexLayout vertex_layout;

            /**
             * Whether this pipeline reads the depth buffer and refuses fragments behind what is there.
             *
             * Off for everything 2D, which is what makes the painter's order a 2D batch relies on
             * still work — turning it on for the UI would let a HUD element drawn second lose to the
             * world drawn first. On for 3D, where the whole point is that geometry occludes itself
             * without being sorted.
             * */
            b8 depth_test;

            /**
             * Whether this pipeline writes the depth it passed.
             *
             * Almost always the same answer as `depth_test`, and separate because of the one case
             * where it is not: transparent 3D geometry tests against the opaque pass but must not
             * write, or the nearest transparent surface hides the ones behind it that should show
             * through.
             * */
            b8 depth_write;

            /**
             * Discard back faces, deciding front by counter-clockwise winding.
             *
             * Off by default, because the 2D batch emits both windings and would lose half its
             * triangles. On for closed 3D geometry, where it halves the fragment work for free.
             * */
            b8 cull_back_faces;

            /**
             * Discard *front* faces instead. For a shadow pass, and for very little else.
             *
             * Recording the far side of each object rather than the side facing the light moves the depth
             * in the map away from the surface being tested by the thickness of the object, which is the
             * cheapest defence against shadow acne there is. Wrong for open geometry, which then casts
             * nothing at all — usually the right answer for a floor.
             *
             * Ignored when `cull_back_faces` is also set; culling both would draw nothing.
             * */
            b8 cull_front_faces;

            /**
             * The colour target's format. Zero means the window's swapchain format.
             *
             * A pipeline is compiled against the format it will draw into, and one built for the swapchain
             * is rejected at bind time when the pass targets something else. Only needed by a pipeline that
             * renders into an offscreen target of an unusual format — the shadow map's R32_FLOAT.
             * */
            SDL_GPUTextureFormat color_format;

            /**
             * Force one sample per pixel, whatever the renderer is using.
             *
             * A pipeline's sample count has to match every target it draws into, and the renderer's
             * multisampled default is wrong for a target that is deliberately not: averaging two depths in
             * a shadow map produces a distance describing neither surface, so it is single sampled and this
             * is how a pipeline agrees with it.
             * */
            b8 single_sampled;
        } as_graphics_pipeline;

        struct {
            /**
             * How this image is sampled. Defaults to linear; pixel art wants nearest.
             *
             * See NYA_TextureFilter. A batch draws with one sampler at a time, so mixing filters
             * across consecutive sprites costs a draw call at each change — group by filter the way
             * you would group by texture.
             * */
            NYA_TextureFilter filter;

            /**
             * Rasterize a vector image at this size instead of its natural one.
             *
             * Only meaningful for SVG, and the whole reason to use one: a vector icon has a size
             * written into the file, and loading it at that size throws away the only advantage it
             * has over a PNG. Ask for the size you will draw it at and it is sharp there.
             *
             * Zero on either axis means the file's own size. A raster format ignores both.
             * */
            u32 width;
            u32 height;

            /**
             * What `currentColor` rasterises to, for an SVG that uses it.
             *
             * Most icon sets stroke with `currentColor`, which is a CSS inheritance keyword — with
             * no document around the file there is nothing to inherit from, and the rasteriser
             * resolves it to black. A black icon cannot be recoloured afterwards either, because a
             * draw tint is a multiply and anything times zero is zero. It could only be made visible
             * by putting something light behind it.
             *
             * Substituted in the source before rasterising, so the texture comes out in this colour
             * and the alpha channel still carries the shape. All-zero means white, which is the
             * useful default: white multiplied by a draw tint is the tint, so a white icon is one
             * that can be drawn in any colour.
             *
             * Ignored by every raster format, and by an SVG that names its own colours.
             * */
            NYA_Color svg_color;
        } as_texture_load;

        struct {
            /**
             * How the model's embedded texture is filtered. Zero is NYA_TEXTURE_FILTER_LINEAR.
             *
             * A load parameter rather than something read out of the FBX, because the file does not say:
             * FBX has no notion of the sampling a renderer should use. A gradient atlas wants linear and
             * a pixel-art atlas wants nearest, and only the game knows which it authored.
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

            /** Carried from the load parameters, because the draw path picks the sampler from it. */
            NYA_TextureFilter filter;
        } as_texture;

        struct {
            TTF_Font* font;
        } as_font;

        /**
         * Triangles, flattened and de-indexed by ufbx into one array per attribute.
         *
         * Positions and normals are parallel: `positions[i]` has `normals[i]`, and `indices` selects
         * into both. Normals come from the file rather than being computed per face, so a model
         * exported with smooth shading arrives smooth — which is the one thing the batch's own
         * primitives cannot express, since _nya_render3d_quad derives a flat normal per quad.
         *
         * All three live in the asset system's arena and are freed together on unload.
         * */
        struct {
            f32x3* positions;
            f32x3* normals;

            /** Parallel to the other two. Zeroed for a model with no UV set, which samples one texel. */
            f32x2* uvs;

            /**
             * How many vertices the three arrays hold.
             *
             * There is no index array. The mesh is de-indexed — one vertex per triangle corner — so an
             * index buffer here would be the identity permutation, four bytes a vertex to say that vertex
             * `i` is vertex `i`. There was one, briefly, and nothing ever read it: nya_render3d_mesh
             * writes the batch's indices as `base + i` because that is all they can be.
             *
             * The cost of de-indexing is duplicated positions at shared corners, which is the trade
             * described above the arrays. Welding would make a real index buffer worth having again.
             * */
            u32 vertex_count;

            /**
             * How many elements each array was allocated for, which is not always how many were written.
             *
             * The arena tracks the size of every allocation and keeps a free list keyed on it, so freeing
             * a different extent than was reserved corrupts that list rather than merely leaking. The
             * counts above come from what the triangulator actually produced and the reservation comes
             * from what ufbx said it would produce; those agree for a well formed file and there is no
             * reason to make a mesh's teardown depend on them agreeing.
             * */
            u32 allocated;

            /**
             * One entry per material, each naming a contiguous run of `indices`.
             *
             * A model is drawn part by part, and a part is the unit that has a texture — which is what
             * makes a multi-material model draw correctly rather than entirely in its first material.
             * There is always at least one, even for a file that names no material at all.
             * */
            NYA_MeshPart* parts;
            u32           part_count;

            /**
             * The same vertices on the GPU, uploaded once and kept.
             *
             * This is what makes a model cost a *draw call* rather than its vertex count. The immediate
             * batch re-uploads every vertex of every mesh on every flush — twice a frame once a shadow
             * pass exists — and that is the correct behaviour for geometry generated fresh each frame and
             * badly wrong for a model that has not changed since it was read off disk.
             *
             * Null until something draws the mesh. Created lazily by the renderer rather than by this
             * loader, because it needs a GPU copy pass and the loader has no frame to hang one on; see
             * _nya_render3d_mesh_upload. Released here on unload, beside the textures, which is what keeps
             * a hot reload from leaking one buffer per reload.
             *
             * The parts' `base_color` is *baked into* these vertices' colour, so a part needs no uniform
             * of its own and the instance tint is a plain multiply on top.
             * */
            SDL_GPUBuffer* gpu_vertices;

            /** How many vertices `gpu_vertices` holds. Zero while it is null. */
            u32 gpu_vertex_count;

            /**
             * The model's axis-aligned bounds, computed once on first request. See nya_render3d_mesh_bounds.
             *
             * Cached rather than recomputed because frustum culling asks for them once per drawn copy per
             * pass. Walking a few thousand vertices is nothing when something is being fitted to the model;
             * it is the whole saving back again when it happens twice a frame per instance.
             *
             * Zeroed by the load, so a hot reload recomputes them rather than keeping the old model's.
             * */
            f32x3 bounds_min;
            f32x3 bounds_max;
            b8    bounds_valid;

            /**
             * How many parts were reserved, which is at least `part_count`.
             *
             * Empty runs are dropped as they are found, so fewer parts can survive than were counted. The
             * arena frees by extent, so the reservation is what teardown needs. Same reasoning as
             * `allocated`.
             * */
            u32 part_capacity;

            /**
             * Every distinct texture the parts refer to, owned by this asset.
             *
             * Separate from the parts, and referred to by index, so that ownership is exact: two
             * materials in one file routinely point at the same image, and parts holding raw pointers
             * would release the same GPU texture twice on unload.
             *
             * Owned here rather than registered as NYA_ASSET_TYPE_TEXTURE assets because an FBX usually
             * carries its images *inside itself* as embedded blobs — both models in this tree do — so
             * there is no path to key a registry entry on without inventing one.
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
 *
 * The synchronous escape hatch from an otherwise asynchronous system, and it exists for the files
 * that are *parsed* rather than uploaded: a tilemap, a dialogue table, a level description. Those are
 * read once at a load boundary and turned into something else immediately, so queueing them would
 * mean a caller polling for a frame or two before it could do the one thing it wanted.
 *
 * Looks in the embedded blob first and the filesystem second, the same order a queued load does, so a
 * shipped build and a development build read the same bytes.
 *
 * The copy is the caller's and lives in `arena`. Nothing is added to the asset registry and nothing
 * is reference counted, which is why this is only right for something consumed on the spot — a
 * texture read this way would be re-read on every call.
 *
 * NYA_ERROR_NOT_FOUND when the handle names nothing, which for a path typed by hand is the usual
 * failure and is worth reporting rather than asserting.
 * */
NYA_API NYA_Error nya_asset_read(NYA_Arena* arena, NYA_AssetHandle handle, OUT u8** out_data, OUT u64* out_size) __attr_no_discard;

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

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * ENUMERATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/**
 * Every asset path, optionally filtered by suffix, sorted.
 *
 * What an editor's "pick a model" list is built from. `suffix` is matched on the end of the path, so
 * ".png" gives the textures and "/props/" does not — this is a suffix test, not a glob.
 *
 * **The source depends on the build**, deliberately. A build with NYA_ASSET_PREFER_BLOB reads the
 * baked index; every other build walks `./assets` on disk. That is the right way round for an editor:
 * a developer build sees a file the moment it is dropped into the folder, with no rebuild, and a
 * shipped build sees exactly what was baked into it and cannot be made to enumerate the player's
 * filesystem.
 *
 * Allocated in `arena`, which the caller owns. Never null; empty when nothing matches.
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
