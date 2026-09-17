#pragma once

#include "SDL3/SDL_gpu.h"

#include "nyangine/base/base.h"
#include "nyangine/base/base_types.h"
#include "nyangine/renderer/render_camera.h"
#include "nyangine/renderer/render_color.h"
// the 3D batch embeds a light and a material by value, so their definitions are needed here.
// render3d.h includes nothing from this file.
#include "nyangine/renderer/render3d.h"
// here rather than at the bottom: the 3D batch holds an NYA_OcclusionBuffer pointer.
#include "nyangine/renderer/render_occlusion.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * TYPES
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

typedef struct NYA_RenderOptions      NYA_RenderOptions;
typedef struct NYA_RenderFrameStats   NYA_RenderFrameStats;
typedef struct NYA_RenderSystem       NYA_RenderSystem;
typedef struct NYA_RenderSystemWindow NYA_RenderSystemWindow;
typedef struct NYA_Vertex3D             NYA_Vertex3D;
typedef struct NYA_Render3DInstance     NYA_Render3DInstance;
typedef struct NYA_Render3DSortKey      NYA_Render3DSortKey;
typedef struct NYA_Render3DStream        NYA_Render3DStream;
typedef struct NYA_Render3DObject        NYA_Render3DObject;
typedef struct NYA_Render3DFrustum       NYA_Render3DFrustum;
typedef struct NYA_Render3DSegment       NYA_Render3DSegment;
typedef struct NYA_Render2DBatch          NYA_Render2DBatch;
typedef struct NYA_Render2DDrawRange      NYA_Render2DDrawRange;
typedef struct NYA_Render2DDraw           NYA_Render2DDraw;
typedef struct NYA_Render3DBatch          NYA_Render3DBatch;
typedef struct NYA_RenderTexture      NYA_RenderTexture;
typedef struct NYA_RenderTextureOptions NYA_RenderTextureOptions;
typedef enum NYA_RenderTextureDepth     NYA_RenderTextureDepth;
typedef struct NYA_Vertex2D           NYA_Vertex2D;
typedef enum NYA_TextureFilter        NYA_TextureFilter;
/** The handles the built in pipelines are registered under. Shared by every window. */
#define NYA_RENDER2D_PIPELINE_SHAPES   "nya_shape_pipeline"
#define NYA_RENDER2D_PIPELINE_TEXTURED "nya_shape_textured_pipeline"

/** Text out of a coverage atlas, which is every font that is not a distance field. */
#define NYA_RENDER2D_PIPELINE_TEXT     "nya_text_pipeline"

/** Text out of a distance-field atlas. Selected per atlas, not per draw; see nya_font_sdf_set. */
#define NYA_RENDER2D_PIPELINE_TEXT_SDF "nya_text_sdf_pipeline"

typedef enum NYA_Render2DFlushReason      NYA_Render2DFlushReason;

/** What forced a draw call. */
enum NYA_Render2DFlushReason {
    /** A different pipeline: shapes to textured, or in and out of a custom shader. */
    NYA_RENDER2D_FLUSH_PIPELINE,

    /** A different texture. The usual cause, and the one an atlas fixes. */
    NYA_RENDER2D_FLUSH_TEXTURE,

    /** A different sampler, meaning a texture wanted another filter. */
    NYA_RENDER2D_FLUSH_SAMPLER,

    /** The render target, camera or scissor changed. Structural rather than avoidable. */
    NYA_RENDER2D_FLUSH_STATE,

    /** The vertex or index buffer filled. Raise NYA_RENDER2D_MAX_VERTICES if this dominates. */
    NYA_RENDER2D_FLUSH_FULL,

    /** The frame ended with work queued. Every frame has exactly one of these. */
    NYA_RENDER2D_FLUSH_FRAME_END,

    NYA_RENDER2D_FLUSH_REASON_COUNT,
};

/**
 * The most uniform bytes a custom shader may be given. Pushed inline per draw call, so small: a blur
 * needs sixteen bytes. Anything larger wants a storage buffer.
 * */
#ifndef NYA_RENDER2D_MAX_UNIFORM_BYTES
#define NYA_RENDER2D_MAX_UNIFORM_BYTES 128
#endif


/**
 * How a texture is sampled when not drawn at its own size. Set once on the texture asset, since a pixel
 * art sheet wants the same answer everywhere. Declared here because core_asset.h includes this header.
 * */
enum NYA_TextureFilter {
    // blends neighbouring texels, for photographic art, gradients and scaled UI. the default, and wrong for pixel
    // art: it blurs and bleeds the neighbouring tile at a sheet edge.
    NYA_TEXTURE_FILTER_LINEAR,

    /** Nearest texel, no blending. What pixel art and tile sheets want. */
    NYA_TEXTURE_FILTER_NEAREST,

    NYA_TEXTURE_FILTER_COUNT,
};

/** What a zero NYA_RenderOptions.msaa_samples means. */
#ifndef NYA_RENDER_MSAA_SAMPLES_DEFAULT
#define NYA_RENDER_MSAA_SAMPLES_DEFAULT 4
#endif

/**
 * What a game can change about rendering while it runs. Zeroed is the default look.
 * */
struct NYA_RenderOptions {
    /**
     * Samples per pixel: 1 turns multisampling off, then 2, 4 or 8. Zero means NYA_RENDER_MSAA_SAMPLES_DEFAULT, and a
     * count the device cannot do falls back to the next lower one. At n samples the window holds n colour and n depth
     * images of its size, 3.5 MB each at 1280x720, where one sample holds a depth image alone. Render textures pay
     * the same again.
     * */
    u32 msaa_samples;
};

/**
 * What one frame of a window asked of the GPU, 2D and 3D together. See nya_render_frame_stats.
 * */
struct NYA_RenderFrameStats {
    u32 draw_calls;

    /** Render passes begun, each reopening after a copy or a target change included. */
    u32 passes;

    /** Copies to GPU buffers and textures, and the bytes they moved. */
    u32 uploads;
    u64 upload_bytes;
};

/*
 * ─────────────────────────────────────────────────────────
 * SYSTEM STRUCT
 * ─────────────────────────────────────────────────────────
 */

struct NYA_RenderSystem {
    SDL_GPUDevice* gpu_device;

    /**
     * Owns what the renderer keeps for the life of the process, such as per-window vertex staging. Not the
     * frame allocator, which resets every frame.
     * */
    NYA_Arena* allocator;

    /**
     * The samplers textured draws read through, one per NYA_TextureFilter, shared by every texture with that
     * filter. CLAMP_TO_EDGE, since REPEAT would wrap a uv just outside a sub-rectangle to the far side of the
     * texture.
     * */
    SDL_GPUSampler* samplers[NYA_TEXTURE_FILTER_COUNT];

    /**
     * Samples per pixel for windows and render textures, from `options` as the device allows. One value for
     * everything, so each pipeline needs one multisampled build. Changed only at nya_render_begin, so a frame never
     * mixes two counts.
     * */
    SDL_GPUSampleCount sample_count;

    /** Whether sample_count and depth_format have been settled by the first window. */
    b8 sample_count_decided;

    /** What nya_render_options_set asked for. Applied at the next nya_render_begin. */
    NYA_RenderOptions options;

    /** `options.msaa_samples` as last applied, so an unchanged request is not revalidated every frame. */
    u32 applied_msaa_samples;

    /**
     * The depth format of every depth buffer and depth-testing pipeline, negotiated once for the same reason
     * as the sample count. D24_UNORM first, D32_FLOAT as the fallback every backend has.
     * */
    SDL_GPUTextureFormat depth_format;
};

/**
 * Whether a render texture carries a depth buffer.
 * */
enum NYA_RenderTextureDepth {
    /** The default, and what a 3D scene needs. Costs width * height * 4 * the renderer's sample count. */
    NYA_RENDER_TEXTURE_DEPTH_ATTACHED = 0,

    /**
     * No depth buffer, for a target only render2d draws into. 2D pipelines declare no depth target, so a post
     * chain's ping-pong target would carry an unreachable 33 MB at 1080p and 4x.
     * */
    NYA_RENDER_TEXTURE_DEPTH_NONE,

    NYA_RENDER_TEXTURE_DEPTH_COUNT,
};

/**
 * Anything about a render texture that is not its size.
 * */
struct NYA_RenderTextureOptions {
    NYA_RenderTextureDepth depth;

    /**
     * Also record the 3D scene's normals and distances. See NYA_RENDER3D_NORMAL_FORMAT, which says what it costs.
     * Needs the depth buffer.
     * */
    b8 normals;

    /**
     * No multisampled companion, whatever NYA_RenderOptions.msaa_samples says. For targets that only ever take
     * fullscreen passes, where there are no edges to smooth: a 1280x720 target then costs 3.5 MB instead of 17.5 MB
     * at 4x. Pipelines drawing into it are built single sampled on first use.
     * */
    b8 single_sampled;
};

/**
 * An offscreen texture that can be drawn into and then drawn with, at the swapchain's format so the same
 * pipelines serve both.
 * */
struct NYA_RenderTexture {
    /** The resolved, single-sampled image that gets sampled when the texture is drawn. */
    SDL_GPUTexture* texture;

    /**
     * The multisampled surface rendered into, resolved onto `texture` when the pass ends. Null without
     * multisampling, in which case `texture` is drawn into directly.
     * */
    SDL_GPUTexture* msaa_texture;

    /**
     * The depth buffer for passes into this texture, so a 3D scene rendered offscreen still occludes itself.
     * Null for NYA_RENDER_TEXTURE_DEPTH_NONE, which only a 2D target may use; nya_render3d_begin asserts that.
     * */
    SDL_GPUTexture* depth_texture;

    /**
     * The scene normal buffer and its multisampled companion, null unless NYA_RenderTextureOptions.normals. See
     * NYA_RENDER3D_NORMAL_FORMAT.
     * */
    SDL_GPUTexture* normal_texture;
    SDL_GPUTexture* normal_msaa_texture;

    u32 width;
    u32 height;

    /** What `msaa_texture` and `depth_texture` were built with; one without a multisampled companion. */
    SDL_GPUSampleCount sample_count;

    /** What it was made with, so a caller can tell whether it still fits. See nya_render_texture_is_current. */
    NYA_RenderTextureOptions options;
};

// after NYA_RenderTexture, which the post chain is built from, and before the window state, which holds its options.
#include "nyangine/renderer/render_post.h"
// the window holds the decal and output options and what they allocate.
#include "nyangine/renderer/render3d_decal.h"
#include "nyangine/renderer/render_output.h"

/** The 2D shape batch for one window. Only render2d.c touches it. */
/** Bytes of custom fragment uniform a deferred range can carry inline. */
#define NYA_RENDER2D_RANGE_UNIFORM_MAX 256

/** Ranges recorded before the batch is forced to draw. */
#define NYA_RENDER2D_MAX_RANGES 512

/** How many draw calls back a range looks for one with its state to join. Bounds the merge at a few compares per range. */
#define NYA_RENDER2D_MERGE_LOOKBACK 16

/**
 * One state's worth of the batch, recorded instead of issued. A state change closes a range, and
 * nya_render2d_flush sorts, merges and issues the ranges, so draw order can differ from declaration order: a
 * dropdown declared inside its panel still paints over later panels. Without layers the sort is stable
 * and nothing changes.
 * */
struct NYA_Render2DDrawRange {
    /** Painted low to high. See nya_render2d_layer_set. */
    s32 layer;

    /** Declaration order, breaking ties inside a layer. */
    u32 sequence;

    u32 first_index;
    u32 index_count;

    /** Already resolved through shader_override. */
    NYA_CString pipeline;

    SDL_GPUTexture* texture;
    SDL_GPUSampler* sampler;

    /** The custom shader's second texture, bound at t1. Null for none. */
    SDL_GPUTexture* shader_texture;

    /**
     * A copy of the custom fragment uniform, since the caller's struct is usually gone by replay. A larger
     * uniform forces an immediate draw instead of being truncated.
     * */
    u8  uniform[NYA_RENDER2D_RANGE_UNIFORM_MAX];
    u32 uniform_size;

    /** Snapshotted because the projection depends on them and both change mid-frame. */
    u32 target_width;
    u32 target_height;

    NYA_Camera2D camera;

    b8  scissor_active;
    s32 scissor_x, scissor_y, scissor_width, scissor_height;

    /** What the vertices cover, in the camera's space, so a merge can tell whether moving the range changes the picture. */
    NYA_Rectf bounds;

    /** The next range in the same draw call, or U32_MAX. Set by nya_render2d_ranges_merge. */
    u32 next;
};

/**
 * Sorted ranges that share every piece of state and draw as one call. A range may join an earlier draw call only
 * when nothing painted in between overlaps it, so the picture is the one the ranges would paint one by one.
 * */
struct NYA_Render2DDraw {
    /** The chain of ranges in paint order, through NYA_Render2DDrawRange.next. The first one's state is the draw's. */
    u32 first_range;
    u32 last_range;

    /** Into the index stream written by nya_render2d_draws_indices_write. */
    u32 first_index;
    u32 index_count;

    /** The union of its ranges' bounds. */
    NYA_Rectf bounds;
};

struct NYA_Render2DBatch {
    SDL_GPUBuffer*         vertex_buffer;
    SDL_GPUTransferBuffer* transfer_buffer;

    /*
     * Indices. A quad is four vertices instead of six and a circle one per segment instead of three, for the
     * price of a second buffer.
     */
    SDL_GPUBuffer*         index_buffer;
    SDL_GPUTransferBuffer* index_transfer_buffer;

    u32* indices;
    u32  index_count;

    /**
     * CPU staging, copied into the transfer buffer on flush. A mapping held open across a frame could be read
     * by the GPU at the same time.
     * */
    NYA_Vertex2D* vertices;
    u32           vertex_count;

    /* Recorded but not yet drawn. See NYA_Render2DDrawRange. */

    NYA_Render2DDrawRange* ranges;
    u32                    range_count;

    /** The draw calls the ranges merge into at flush, as many as there are ranges at most. */
    NYA_Render2DDraw* draws;

    /** Where the range being built started, and the counter that keeps the sort stable. */
    u32 range_first_index;
    u32 range_first_vertex;
    u32 range_sequence;

    /** Painted low to high. Snapshotted into each range as it closes. */
    s32 layer;

    /*
     * Per frame counters, reset by nya_render_begin. `flushes` is the number to watch: each one is a draw call
     * forced by a state change.
     */
    u32 frame_flushes;
    u32 frame_vertices;
    u32 frame_indices;

    /**
     * Draw calls this frame by cause, so ten texture swaps (fixable with an atlas) can be told apart from ten
     * target changes. Indexed by NYA_Render2DFlushReason.
     * */
    u32 frame_flush_reasons[NYA_RENDER2D_FLUSH_REASON_COUNT];

    /** Why the next flush happens. Set by whatever forces it. */
    u32 pending_flush_reason;

    /**
     * Whether the next window pass resolves multisampling. Only the frame's last pass does; resolving on every
     * reopen would resolve once per draw call. Render texture passes always resolve.
     * */
    b8 resolve_pending;

    /**
     * Draws that did nothing this frame: no pipeline, no texture, no pass, or a shape too large. Silent draws
     * look like nothing was asked for, so they are counted.
     * */
    u32 frame_dropped_draws;

    /*
     * Batch state. A draw needing anything different flushes first, because a draw call has one pipeline and
     * one texture.
     */

    /**
     * The pipeline the queued vertices want, null when nothing is queued. NYA_CString rather than
     * NYA_AssetHandle, since core_asset.h includes this file.
     * */
    NYA_CString pipeline;

    /** Bound at t0 for a textured draw, null for an untextured one. */
    SDL_GPUTexture* texture;

    /** The sampler for that texture. A change flushes. */
    SDL_GPUSampler* sampler;

    /**
     * Set by nya_render2d_shader_begin and used instead of the pipeline a draw would pick. Null for the normal
     * one.
     * */
    NYA_CString shader_override;

    /*
     * Custom shader uniforms, pushed to fragment slot 0 of the overriding pipeline (vertex slot 0 is the
     * projection). Stored inline, since the push happens at flush, after the caller's data is gone.
     */
    u8  shader_uniform[NYA_RENDER2D_MAX_UNIFORM_BYTES];
    u32 shader_uniform_size;

    /** Bound at t1 beside the drawn texture, for a custom shader that samples a second image. */
    SDL_GPUTexture* shader_texture;

    /*
     * Render target. The swapchain until nya_render_texture_begin, held here because the projection has to
     * match the target size.
     */
    /** Where the finished pixels go: the swapchain image or a render texture's resolved side. */
    SDL_GPUTexture* target_texture;

    /** What the pass draws into when multisampling is on. */
    SDL_GPUTexture* target_msaa;

    /** Samples per pixel of whatever the pass draws into, which picks each pipeline's build. */
    SDL_GPUSampleCount target_sample_count;

    /**
     * The depth buffer for the current target. A render texture has its own, and a reopened pass must attach
     * the right one.
     * */
    SDL_GPUTexture* target_depth;

    /** The current target's normal buffer and its multisampled side, null for the window. */
    SDL_GPUTexture* target_normal;
    SDL_GPUTexture* target_normal_msaa;

    /**
     * Whether a pass attached the normal buffer since the target began, meaning 3D was drawn into it. The first one
     * clears it; nya_render_texture_end resolves it only if one did.
     * */
    b8 target_normal_written;

    u32 target_width;
    u32 target_height;

    /** The render texture nya_render_texture_end has to restore the swapchain from. */
    b8 target_is_texture;

    /* View */

    /**
     * The camera for the queued vertices. Part of the per-flush projection, so a change flushes; applying it to
     * vertices as they are built would make camera changes retroactive.
     * */
    /* Scissor. Render pass state, so a change flushes, and it is reapplied whenever the pass reopens. */
    b8  scissor_active;
    s32 scissor_x, scissor_y, scissor_width, scissor_height;

    /** The camera for the queued vertices, or NONE for screen pixels. NONE, the UI case, skips the view matrix. */
    NYA_Camera2D camera;
};

/**
 * The 3D mesh batch for one window. Only render3d.c touches it. Separate from the 2D batch because the
 * vertices, pipelines and depth state differ; they share the render pass and buffer lifetimes.
 * */
/** The instances queued for one retained mesh this pass. Grouped per mesh because the draw call is per mesh. */
/** One transparent triangle's place in the queue: its distance and where its indices start. */
struct NYA_Render3DSortKey {
    /**
     * Squared distance from the eye to the centroid. Squared because only order matters. The centroid, because a
     * nearest-vertex key flickers for triangles that share an edge.
     * */
    f32 depth;

    /** Index of the triangle's first element in the transparent stream. */
    u32 first;
};

/**
 * Geometry a caller built and handed to the renderer to keep. See nya_render3d_mesh_register. Generated
 * geometry such as terrain changes rarely, and the immediate path would upload it again for every pass. One
 * part, no texture: a caller with several materials registers several meshes.
 * */
typedef struct NYA_Render3DRegisteredMesh NYA_Render3DRegisteredMesh;

struct NYA_Render3DRegisteredMesh {
    SDL_GPUBuffer* vertices;
    u32            vertex_count;

    /**
     * The staged copy, held until a frame can run it; null once uploaded. Registration usually happens outside
     * rendering, where there is no command buffer.
     * */
    SDL_GPUTransferBuffer* pending_upload;

    /** Bytes the pending copy will move. Meaningless once `pending_upload` is null. */
    u32 pending_size;

    /** Computed at registration; the vertices cannot change without re-registering. */
    f32x3 bounds_min;
    f32x3 bounds_max;
};

typedef struct NYA_Render3DMeshGroup NYA_Render3DMeshGroup;

struct NYA_Render3DMeshGroup {
    /**
     * The asset handle, compared by pointer. Handles are literals from the generated index, and two different
     * pointers with equal text would only cost an extra draw call.
     * */
    NYA_ConstCString handle;

    /** Where this group's run starts in the shared instance array, and its length. */
    u32 first_instance;
    u32 instance_count;

    /**
     * Whether this mesh was tinted translucent and draws after the opaque ones. Per group, since a group only
     * collects consecutive draws with the same tint.
     * */
    b8 transparent;

    /** Squared distance to the nearest instance, for ordering transparent groups. */
    f32 depth;
};

/**
 * One run of CPU-staged geometry. There are two: opaque geometry is sorted by the depth buffer, while
 * translucent geometry must be drawn back to front because blending is not commutative. Separate streams
 * make that sort possible without breaking batching.
 * */
struct NYA_Render3DStream {
    NYA_Vertex3D* vertices;
    u32           vertex_count;

    /** Sixteen bits, which NYA_RENDER3D_MAX_VERTICES fits, since each pass uploads its own list of them. */
    u16* indices;
    u32  index_count;

    NYA_Render3DObject* objects;
    u32                 object_count;
};

/**
 * A run of a stream's indices seen by the same passes: one culled primitive, or neighbours that agree. It ends where
 * the next object starts.
 * */
struct NYA_Render3DObject {
    u32 first_index;

    /** One bit per pass that sees it: the camera in bit zero, cascade `c` in bit `c + 1`. */
    u8 passes;
};

/** The six inward-facing clip planes of a view-projection. See _nya_render3d_frustum_build. */
struct NYA_Render3DFrustum {
    f32x4 planes[6];
};

/** A run of the uploaded index buffer. */
typedef struct {
    u32 first;
    u32 count;
} NYA_Render3DIndexRange;

/**
 * What the scene recorded between two state changes: the shading, the pipeline choice and the geometry drawn with
 * them. Recorded once and drawn by every pass, so the shadow cascades and the camera share one upload.
 * */
struct NYA_Render3DSegment {
    /** Where this segment's objects start in each stream. They end where the next segment's start. */
    u32 opaque_objects;
    u32 transparent_objects;

    /** The instanced groups and decals recorded with this state. */
    u32 first_group;
    u32 group_count;
    u32 first_decal;
    u32 decal_count;

    /** Written by the playback: each stream's visible indices, per pass. */
    NYA_Render3DIndexRange opaque[NYA_RENDER3D_PASSES];
    NYA_Render3DIndexRange transparent[NYA_RENDER3D_PASSES];

    SDL_GPUTexture*  texture;
    SDL_GPUSampler*  sampler;
    NYA_ConstCString decal_texture;

    /** A posed mesh drawn instead of the geometry above: its handle and its palette, in the frame arena. */
    NYA_ConstCString                    skinned;
    const struct NYA_ShaderSkinUniform* skin;

    NYA_Render3DMaterial material;
    NYA_Render3DBlend    blend;
    NYA_Render3DDepth    depth;
    b8                   casts_shadow;
};

struct NYA_Render3DBatch {
    SDL_GPUBuffer*         vertex_buffer;
    SDL_GPUTransferBuffer* transfer_buffer;
    SDL_GPUBuffer*         index_buffer;
    SDL_GPUTransferBuffer* index_transfer_buffer;

    /**
     * CPU staging, uploaded once when the scene plays back. Both streams share one GPU buffer, opaque at offset zero
     * and transparent after it.
     * */
    NYA_Render3DStream opaque;
    NYA_Render3DStream transparent;

    /** What was recorded, in order. See NYA_Render3DSegment. */
    NYA_Render3DSegment*            segments;
    struct NYA_ShaderMesh3DUniform* segment_uniforms;
    u32                             segment_count;

    /** The most segments one playback held, for the ceiling. */
    u32 segment_count_worst;

    /** Every pass's visible indices, one list after another, as uploaded. */
    u16* pass_indices;

    /**
     * The stream primitives are writing into, chosen by colour. Kept as state because a shape emits many quads
     * with the same value.
     * */
    b8 transparent_active;

    /**
     * Scratch for sorting the transparent stream: a key per triangle and the reordered indices, written beside
     * the original since a triangle's three indices move together.
     * */
    NYA_Render3DSortKey* sort_keys;
    u16*                 sorted_indices;

    /** The radix sort's other buffer. */
    NYA_Render3DSortKey* sort_keys_scratch;

    /** Scratch for ordering one group's instances back to front. */
    NYA_Render3DInstance* sorted_instances;

    /**
     * The texture for the queued triangles, or null. Batch state like `material`: it selects pipeline and
     * binding, so a change flushes. Only nya_render3d_mesh sets it.
     * */
    SDL_GPUTexture* texture;
    SDL_GPUSampler* sampler;

    /** The frame's point lights, bounded by what one uniform block carries. */
    NYA_Render3DPointLight point_lights[NYA_RENDER3D_MAX_POINT_LIGHTS];
    u32                    point_light_count;

    /** The frame's fog. A fragment uniform, like `light`. */
    NYA_Render3DFog fog;

    /* Shadow pass. */

    /**
     * Light-space depth plus the depth buffer for its test. A colour target, because sampling a depth format is
     * unevenly supported. Created when a scene first casts shadows, released when they are turned off.
     * */
    SDL_GPUTexture* shadow_color;
    SDL_GPUTexture* shadow_depth;

    /** One texel bound in the atlas's place without shadows, since a declared sampler needs a texture. */
    SDL_GPUTexture* shadow_none;

    /** As the caller set them. Resolve through nya_render3d_shadow_options, which applies the defaults. */
    NYA_Render3DShadowOptions shadow_options;

    /** As the caller set it. See nya_render3d_shadow_set. */
    NYA_Render3DShadowFit shadow_fit;

    /** The fitted volume's strength and bias, for the uniform. */
    NYA_Render3DShadow shadow;

    /** One matrix per cascade and its reach, fitted when the scene first draws and read by the camera pass. */
    f32_4x4 shadow_view_projection[NYA_RENDER3D_SHADOW_CASCADES];
    f32     shadow_cascade_extent[NYA_RENDER3D_SHADOW_CASCADES];

    /** Cascades fitted this frame. Reset by nya_render3d_end. */
    u32 shadow_cascade_count;

    /** True once the cascades were drawn this frame, so a later playback loads the atlas instead of clearing it. */
    b8 shadow_valid;

    /**
     * Every pass's frustum, the camera's first and a cascade's after it, fitted the first time something is drawn.
     * Recording tests a draw against all of them once and keeps the answer as a pass mask.
     * */
    NYA_Render3DFrustum passes[NYA_RENDER3D_PASSES];
    u32                 pass_count;
    b8                  passes_ready;

    /** Whether what is drawn now goes into the shadow cascades. See nya_render3d_shadow_cast_set. */
    b8 casts_shadow;

    /** False outside nya_render3d_begin and nya_render3d_end. Nothing draws while false. */
    b8 active;

    /**
     * Whether a 3D camera has been set on this window. Never cleared. Separate from `active`, which is only true
     * during on_render: clicks arrive in on_event and are un-projected through last frame's camera.
     * */
    b8 camera_valid;

    /** The finished view-projection. Constant between a begin and an end. */
    f32_4x4 view_projection;

    /*
     * The camera as given. Rebuilding a ray from the basis is cheaper and better conditioned than inverting the
     * view-projection; see nya_render3d_screen_ray.
     */
    NYA_Camera3DPerspective  camera;
    NYA_Camera3DOrthographic camera_orthographic;
    b8                       camera_is_ortho;

    /** Both are fragment uniforms, so a change costs a draw call. */
    NYA_Render3DLight    light;
    NYA_Render3DMaterial material;

    /** The occlusion buffer the camera pass culls against, or null. */
    const NYA_OcclusionBuffer* occlusion;

    /* Retained mesh path, played back with the rest. See NYA_Render3DInstance. */

    /** Per-instance transforms, uploaded once per playback, and which passes see each. */
    NYA_Render3DInstance* instances;
    u8*                   instance_passes;
    u32                   instance_count;

    SDL_GPUBuffer*         instance_buffer;
    SDL_GPUTransferBuffer* instance_transfer_buffer;

    /** One entry per distinct mesh queued. */
    NYA_Render3DMeshGroup mesh_groups[NYA_RENDER3D_MAX_MESH_GROUPS];
    u32                   mesh_group_count;

    /** Geometry registered by the game: NYA_Render3DRegisteredMesh values keyed by handle text. */
    NYA_Cache* registered_meshes;

    /** What the transparent stream does: blend or add. See nya_render3d_blend_set. */
    NYA_Render3DBlend blend;

    /** See nya_render3d_depth_set. Selects the overlay pipeline for gizmos. */
    NYA_Render3DDepth depth;

    /**
     * A copy of the opaque scene for refractive glass. Created when first needed. A copy because a shader may
     * not sample the target it writes to.
     * */
    SDL_GPUTexture* refraction_capture;
    u32             refraction_width;
    u32             refraction_height;

    /* Per frame counters, reset by nya_render_begin and read through nya_render3d_frame_stats. */

    u32 frame_draw_calls;
    u32 frame_vertices;
    u32 frame_indices;

    /** Primitives too large for an empty batch. */
    u32 frame_dropped_draws;

    /** Mesh copies drawn through the retained path this frame, and how many were frustum culled. */
    u32 frame_instances;
    u32 frame_culled;

    /** Of those that survived the frustum, how many the occlusion buffer hid. */
    u32 frame_occluded;

    /** Passes drawn: each cascade and the camera, per playback. */
    u32 frame_passes;
};

struct NYA_RenderSystemWindow {
    /**
     * The window's clear colour, opaque black by default. Alpha is what a compositor reads: a transparent window
     * cleared to zero alpha shows the desktop wherever nothing is drawn. See nya_render_clear_color_set.
     * */
    NYA_Color clear_color;

    SDL_GPURenderPass*    render_pass;
    SDL_GPUCommandBuffer* render_commands;
    SDL_GPUTexture*       swapchain_texture;

    /**
     * The format every pipeline and target of this window is built for: the swapchain's when it was claimed, which
     * is SDR. It stays when the output switches to HDR; see render_output.h.
     * */
    SDL_GPUTextureFormat color_format;

    /**
     * Whether the open pass, or the one the next resume opens, attaches the target's normal buffer. 3D draws set
     * it and 2D draws clear it, since their pipelines are built for different targets. See
     * _nya_render2d_pass_normals_set.
     * */
    b8 render_pass_normals;

    /* The window's multisampled colour buffer and the size and sample count it was built for. */
    SDL_GPUTexture*    msaa_texture;
    u32                msaa_width;
    u32                msaa_height;
    SDL_GPUSampleCount msaa_sample_count;

    /*
     * The window's depth buffer, attached to every window pass so 2D and 3D share one pass; the attachment is
     * fixed when a pass opens. A 2D-only game pays one unused texture.
     */
    SDL_GPUTexture*    depth_texture;
    u32                depth_width;
    u32                depth_height;
    SDL_GPUSampleCount depth_sample_count;

    NYA_Render2DBatch draw_batch;
    NYA_Render3DBatch mesh_batch;

    /** See nya_render3d_decals_set. */
    NYA_Render3DDecals    decals;
    NYA_Render3DDecalsGPU decals_gpu;

    /** See nya_render_output_set. */
    NYA_RenderOutput    output;
    NYA_RenderOutputGPU output_gpu;

    /* The scene post passes. See render_post.h. */

    NYA_PostInk              post_ink;
    NYA_PostAmbientOcclusion post_ambient_occlusion;
    NYA_PostAntialias        post_antialias;
    NYA_PostDepthOfField     post_depth_of_field;
    NYA_PostSpeedLines       post_speed_lines;
    NYA_PostBloom            post_bloom;
    NYA_PostDebugView        post_debug_view;

    /** This frame so far, and the last finished one. The draw calls are filled in when a frame finishes. */
    NYA_RenderFrameStats frame_stats;
    NYA_RenderFrameStats frame_stats_last;
};

/*
 * ─────────────────────────────────────────────────────────
 * RENDERING STRUCTS
 * ─────────────────────────────────────────────────────────
 */

/** One vertex of the immediate 3D batch, 36 bytes. */
struct NYA_Vertex3D {
    /** Three plain floats: an `f32x3` would be sixteen bytes. */
    f32 position[3];

    /** HALF2 is enough precision for texture coordinates. */
    f16 uv[2];

    f32 normals[3];

    /** HALF4, so emissive colours above one survive to the tonemap. UBYTE4_NORM would clamp them. */
    f16 color[4];
};

static_assert(sizeof(NYA_Vertex3D) == 36, "the 3D vertex layout in core_asset.c describes a 36 byte vertex");


/** Builds one from the wide types a caller has. */
/** Releases the shadow atlas, which the next pass creates again at the current options. A no-op headless. */
NYA_INTERNAL __attr_allow_unused void _nya_render3d_shadow_release(NYA_Window* window);

/** `options` with zeroes defaulted, cascades clamped and the map size clamped to a power of two. */
NYA_INTERNAL __attr_allow_unused NYA_Render3DShadowOptions _nya_render3d_shadow_options_resolve(NYA_Render3DShadowOptions options) __attr_no_discard;

NYA_API NYA_Vertex3D nya_vertex3d(f32x3 position, NYA_Color color, f32x3 normal, f32x2 uv) __attr_no_discard;

/**
 * Sets what this window's colour target is cleared to each frame. Defaults to opaque black.
 *
 * ```c
 * // A desktop widget: shaped like what it draws, not like its window.
 * NYA_WindowHandle w = nya_window_create("pet", 240, 240, NYA_WINDOW_TRANSPARENT | NYA_WINDOW_BORDERLESS | NYA_WINDOW_ALWAYS_ON_TOP);
 * nya_render_clear_color_set(nya_window_get(w), (NYA_Color){ 0.0F, 0.0F, 0.0F, 0.0F });
 * ```
 *
 * Zero alpha only makes sense with NYA_WINDOW_TRANSPARENT. Without it the frame shows whatever the compositor
 * has behind the window.
 * */
NYA_API void nya_render_clear_color_set(NYA_Window* window, NYA_Color color);

/** What this window clears to. See nya_render_clear_color_set. */
NYA_API NYA_Color nya_render_clear_color(NYA_Window* window) __attr_no_discard;

/**
 * Changes how `window` renders, from its next nya_render_begin. Cheap when nothing changed, so a game can feed its
 * config every frame. Windows share pipelines, so the sample count is every window's.
 *
 * ```c
 * nya_render_options_set(window, (NYA_RenderOptions){ .msaa_samples = NYA_CONFIG.engine.renderer.msaa_samples });
 * ```
 * */
NYA_API void nya_render_options_set(NYA_Window* window, NYA_RenderOptions options);

/** The options in effect: `msaa_samples` is what the device took, not what was asked for. */
NYA_API NYA_RenderOptions nya_render_options_get(NYA_Window* window) __attr_no_discard;

/**
 * The last finished frame's draw calls, render passes and uploads, so a regression shows as a number that moved.
 * */
NYA_API NYA_RenderFrameStats nya_render_frame_stats(NYA_Window* window) __attr_no_discard;

/** The position as a vector. */
NYA_API f32x3 nya_vertex3d_position(NYA_Vertex3D vertex) __attr_no_discard;

/**
 * One drawn copy of a retained mesh: its transform and tint, 80 bytes. The per-instance half of
 * NYA_VERTEX_LAYOUT_3D_INSTANCED, for geometry uploaded once and drawn many times.
 * */
struct NYA_Render3DInstance {
    /**
     * Model to world, column-major like the engine's matrices. The shader reads it as four FLOAT4 attributes,
     * since no API has a matrix vertex format.
     * */
    f32_4x4 model;

    /** Multiplied onto the vertex colour, which already carries the part's material colour. */
    NYA_Color tint;
};

/**
 * The vertex the 2D batch uses, twenty bytes. The batch uploads every vertex every frame, so size is
 * per-frame bandwidth. Plain scalars rather than f32x2, which is eight-byte aligned and would pad this to
 * twenty-four.
 * */
struct NYA_Vertex2D {
    f32 x, y;
    f32 u, v;

    /** RGBA bytes, declared UBYTE4_NORM, so the shader still reads a `float4` in 0..1. */
    u8 color[4];
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

typedef struct NYA_Window NYA_Window;

NYA_API NYA_Error nya_system_renderer_init(void) __attr_no_discard;
NYA_API void      nya_system_renderer_deinit(void);
NYA_API void      nya_system_renderer_for_window_init(NYA_Window* window);
NYA_API void      nya_system_renderer_for_window_deinit(NYA_Window* window);
NYA_API void      nya_system_renderer_set_vsync(b8 enabled);

/*
 * ─────────────────────────────────────────────────────────
 * RENDERING FUNCTIONS
 * ─────────────────────────────────────────────────────────
 */

/**
 * Acquires a swapchain image and opens a render pass. False when there is nothing to draw into, which is
 * normal for a minimised or occluded window. The caller must not draw then: there is no pass and the command
 * buffer was cancelled.
 * */
NYA_API b8   nya_render_begin(NYA_Window* window) __attr_no_discard;
NYA_API void nya_render_end(NYA_Window* window);

// after NYA_Render3DSortKey, which it sorts.
#include "nyangine/renderer/render_sort.h"
#include "nyangine/renderer/render_lut.h"
#include "nyangine/renderer/render_lod.h"
#include "nyangine/renderer/render_gpu_memory.h"
#include "nyangine/renderer/render_text.h"
#include "nyangine/renderer/render_font.h"
