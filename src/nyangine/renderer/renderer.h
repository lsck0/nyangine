#pragma once

#include "SDL3/SDL_gpu.h"

#include "nyangine/base/base.h"
#include "nyangine/base/base_types.h"
#include "nyangine/core/core_event.h"
#include "nyangine/renderer/render_camera.h"
#include "nyangine/renderer/render_color.h"
// The 3D batch embeds an NYA_Render3DLight and an NYA_Render3DMaterial by value, so their
// definitions have to be complete here. render3d.h deliberately includes nothing from this file.
#include "nyangine/renderer/render3d.h"
// Here rather than beside render_sort.h and render_lod.h at the bottom: the 3D batch holds an
// NYA_OcclusionBuffer pointer, and a pointer to an undeclared type is not a type.
#include "nyangine/renderer/render_occlusion.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * TYPES
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

typedef struct NYA_RenderSystem       NYA_RenderSystem;
typedef struct NYA_RenderSystemWindow NYA_RenderSystemWindow;
typedef struct NYA_Vertex3D             NYA_Vertex3D;
typedef struct NYA_Render3DInstance     NYA_Render3DInstance;
typedef struct NYA_Render3DSortKey      NYA_Render3DSortKey;
typedef struct NYA_Render3DStream        NYA_Render3DStream;
typedef struct NYA_Render2DBatch          NYA_Render2DBatch;
typedef struct NYA_Render2DDrawRange      NYA_Render2DDrawRange;
typedef struct NYA_Render3DBatch          NYA_Render3DBatch;
typedef struct NYA_RenderTexture      NYA_RenderTexture;
typedef struct NYA_Vertex2D           NYA_Vertex2D;
typedef enum NYA_TextureFilter        NYA_TextureFilter;
/** The handles the built in pipelines are registered under. Shared by every window. */
#define NYA_RENDER2D_PIPELINE_SHAPES   "nya_shape_pipeline"
#define NYA_RENDER2D_PIPELINE_TEXTURED "nya_shape_textured_pipeline"

/** Text out of a distance-field atlas. Selected per atlas, not per draw; see nya_font_sdf_set. */
#define NYA_RENDER2D_PIPELINE_TEXT_SDF "nya_text_sdf_pipeline"

typedef enum NYA_Render2DFlushReason      NYA_Render2DFlushReason;

/**
 * What forced a draw call.
 *
 * The batch merges consecutive draws that agree on pipeline, texture, sampler and target; anything
 * that disagrees ends the run. Knowing *which* turns "this frame costs ten draw calls" into a thing
 * you can act on — texture swaps mean reach for an atlas, target changes mean restructure the frame.
 * */
enum NYA_Render2DFlushReason {
    /** A different pipeline: shapes to textured, or in and out of a custom shader. */
    NYA_RENDER2D_FLUSH_PIPELINE,

    /** A different texture. The usual cause, and the one an atlas fixes. */
    NYA_RENDER2D_FLUSH_TEXTURE,

    /** A different sampler, which means a texture asked for a different filter. */
    NYA_RENDER2D_FLUSH_SAMPLER,

    /** The render target changed, or the camera or scissor did. Structural rather than avoidable. */
    NYA_RENDER2D_FLUSH_STATE,

    /** The vertex or index buffer filled. Raise NYA_RENDER2D_MAX_VERTICES if this dominates. */
    NYA_RENDER2D_FLUSH_FULL,

    /** The frame ended with work queued. Every frame has exactly one of these. */
    NYA_RENDER2D_FLUSH_FRAME_END,

    NYA_RENDER2D_FLUSH_REASON_COUNT,
};

/**
 * The most uniform bytes a custom shader may be given. Small on purpose: this is per draw call state
 * pushed inline into the command buffer, not a buffer to stream data through — a blur needs a texel size
 * and a direction, which is sixteen bytes. Wanting kilobytes here means wanting a storage buffer instead.
 * */
#ifndef NYA_RENDER2D_MAX_UNIFORM_BYTES
#define NYA_RENDER2D_MAX_UNIFORM_BYTES 128
#endif


/**
 * How a texture is sampled when it is not drawn at exactly its own size. A property of the image rather
 * than of the draw, so it is set once on the texture asset: a pixel art tile sheet wants the same answer
 * everywhere it appears, and a photographic background wants the other one everywhere. Lives here rather
 * than in core_asset.h, which is where it is *used*, because that header already includes this one and
 * the reverse would be a cycle.
 * */
enum NYA_TextureFilter {
    // Blends neighbouring texels. Right for photographic art, gradients and scaled up UI. The default,
    // and wrong for pixel art in two ways: it blurs, and at a sheet edge it samples pixels belonging to
    // the *next* tile, a seam that appears and disappears as the camera scrolls to fractional offsets.
    NYA_TEXTURE_FILTER_LINEAR,

    /** Nearest texel, no blending. What pixel art and tile sheets want. */
    NYA_TEXTURE_FILTER_NEAREST,

    NYA_TEXTURE_FILTER_COUNT,
};

/*
 * ─────────────────────────────────────────────────────────
 * SYSTEM STRUCT
 * ─────────────────────────────────────────────────────────
 */

struct NYA_RenderSystem {
    SDL_GPUDevice* gpu_device;

    /**
     * Owns anything the renderer keeps for the lifetime of the process, which today is the shape batch's
     * CPU side vertex staging, one buffer per window. Its own arena rather than the app's frame allocator,
     * since that one is reset every frame and these allocations have to outlive the frame that made them —
     * same shape as the entity, input and callback systems, which each own one for the same reason.
     * */
    NYA_Arena* allocator;

    /**
     * The samplers textured draws read through, one per NYA_TextureFilter, both clamped to the edge. Two
     * rather than one per texture: a sampler says how to read rather than what is being read, so every
     * texture wanting the same filter can share one, and a change between draws costs a draw call, since a
     * draw call binds one. CLAMP_TO_EDGE on both — REPEAT would wrap a uv landing a hair outside a
     * sub-rectangle round to the far side of the texture, a stray line of some unrelated tile or glyph
     * along an edge.
     * */
    SDL_GPUSampler* samplers[NYA_TEXTURE_FILTER_COUNT];

    /**
     * Samples per pixel, decided once at startup and used by every pipeline and every render target —
     * anti-aliasing for shapes and text, without which a line's edge is a hard staircase, since the batch
     * rasterizes geometry with no coverage information of its own. Four is the usual sweet spot; the
     * driver is asked first and this falls back to one when it says no, which is why it is stored rather
     * than a constant. One value for everything on purpose: a pipeline bakes its sample count in, so a
     * pipeline built for four samples cannot draw into a single-sampled target — mixing them would mean a
     * second copy of every pipeline.
     * */
    SDL_GPUSampleCount sample_count;

    /** Whether sample_count has been settled. See nya_system_renderer_for_window_init. */
    b8 sample_count_decided;

    /**
     * The depth format every window's depth buffer and every depth-testing pipeline is built at.
     * Negotiated once, like the sample count and for the same reason: a pipeline bakes its depth format
     * in, so two formats would mean two copies of every 3D pipeline. D24_UNORM is asked for first,
     * D32_FLOAT is the fallback that every backend has.
     * */
    SDL_GPUTextureFormat depth_format;
};

/**
 * An offscreen texture that can be drawn into and then drawn *with*. The engine's counterpart to raylib's
 * RenderTexture2D. Created at the swapchain's own format on purpose: a graphics pipeline is compiled
 * against the format of the target it writes to, so a render texture in some other format would need its
 * own copy of every pipeline that draws into it. Matching the swapchain means one set of pipelines serves
 * both.
 * */
struct NYA_RenderTexture {
    /**
     * The resolved, single-sampled image. This is what gets sampled when the texture is drawn.
     * */
    SDL_GPUTexture* texture;

    /**
     * The multisampled surface actually rendered into, resolved onto `texture` when the pass ends. Null
     * when the device does not support multisampling, in which case `texture` is drawn into directly —
     * every path here has to cope with both, which is why this is checked rather than assumed.
     * */
    SDL_GPUTexture* msaa_texture;

    /**
     * The depth buffer for passes that draw into this texture. Present for the same reason the window has
     * one and created with it, so a 3D scene rendered offscreen — into a bloom or CRT pass, say — occludes
     * itself exactly as it would on the swapchain. Without it, 3D geometry draws in submission order, a
     * bug that only appears once someone adds a post effect to a scene that already worked.
     * */
    SDL_GPUTexture* depth_texture;

    u32 width;
    u32 height;
};

/**
 * The 2D shape batch for one window. See render2d.h; nothing outside that file touches this.
 *
 * Per window rather than per renderer because the pipeline is built against a specific swapchain
 * format and the vertex buffer is filled and drained inside one window's frame.
 * */
/** Bytes of custom fragment uniform a deferred range can carry inline. See NYA_Render2DDrawRange.uniform. */
#define NYA_RENDER2D_RANGE_UNIFORM_MAX 256

/** Ranges recorded before the batch is forced to draw. Past this, a draw happens early. */
#define NYA_RENDER2D_MAX_RANGES 512

/**
 * One draw call's worth of the batch, recorded rather than issued. The 2D batch used to draw the moment
 * any of pipeline, texture or sampler changed, which made draw order and *declaration* order the same
 * thing — a UI needs them apart, since a dropdown is declared inside the panel that owns it and has to
 * paint over the panels declared after it. So a state change now closes a range instead of drawing one,
 * and nya_render2d_flush sorts what has accumulated and issues it; with no layers in play the sort is
 * stable on `sequence` and the output is exactly what it always was.
 * */
struct NYA_Render2DDrawRange {
    /** Painted low to high. See nya_render2d_layer_set. */
    s32 layer;

    /** Declaration order, which breaks ties inside a layer and keeps the sort stable. */
    u32 sequence;

    u32 first_index;
    u32 index_count;

    /** Already resolved through shader_override, so replaying needs no second decision. */
    NYA_CString pipeline;

    SDL_GPUTexture* texture;
    SDL_GPUSampler* sampler;

    /**
     * A copy of the custom fragment uniform, not a pointer to the caller's. The uniform used to be pushed
     * during the same call that set it, so borrowing it was safe; deferring breaks that, since the
     * caller's struct is routinely a stack local gone by the time the range is replayed. Copied, therefore,
     * and a uniform larger than this forces an immediate draw rather than being truncated.
     * */
    u8  uniform[NYA_RENDER2D_RANGE_UNIFORM_MAX];
    u32 uniform_size;

    /** Snapshotted because the projection is built from them and both change mid frame. */
    u32 target_width;
    u32 target_height;

    NYA_Camera2D camera;

    b8  scissor_active;
    s32 scissor_x, scissor_y, scissor_width, scissor_height;
};

struct NYA_Render2DBatch {
    SDL_GPUBuffer*         vertex_buffer;
    SDL_GPUTransferBuffer* transfer_buffer;

    /*
     * ── Indices ──
     *
     * Every shape here is triangles built from a small number of distinct corners, and without an
     * index buffer the shared ones are duplicated: a quad costs six vertices rather than four, and a
     * circle three per segment rather than one. Indexing cuts a quad's vertex traffic by a third and
     * a circle's by two thirds, at the price of a second buffer to upload alongside the first.
     */
    SDL_GPUBuffer*         index_buffer;
    SDL_GPUTransferBuffer* index_transfer_buffer;

    u32* indices;
    u32  index_count;

    /**
     * CPU side staging, filled by the draw calls and copied into the transfer buffer on flush. Kept
     * separate from the mapped transfer buffer rather than writing straight into it: a mapping held open
     * across a frame is a mapping held while the GPU may still be reading last frame's copy of the same
     * memory.
     * */
    NYA_Vertex2D* vertices;
    u32           vertex_count;

    /*
     * ── Deferred ranges ──
     *
     * What has been recorded but not yet drawn. See NYA_Render2DDrawRange.
     */

    NYA_Render2DDrawRange* ranges;
    u32                    range_count;

    /** Where the range being built started, and the order counter that keeps the sort stable. */
    u32 range_first_index;
    u32 range_sequence;

    /** Painted low to high. Batch state, snapshotted into each range as it closes. */
    s32 layer;

    /*
     * ── Per frame counters ──
     *
     * What the frame actually cost the batch, reset by nya_render_begin. A draw call is the unit
     * that matters here — vertices are cheap and state changes are not — so `flushes` is the number
     * to watch: it is exactly the count of draw calls the 2D batch issued, and every one of them is
     * a pipeline, texture, sampler or target change that could not be batched away.
     */
    u32 frame_flushes;
    u32 frame_vertices;
    u32 frame_indices;

    /**
     * Draw calls this frame, broken down by what forced each one. A count alone says the frame cost ten
     * draw calls; this says whether they were ten texture swaps that could have been an atlas, or ten
     * target changes that are structural. Indexed by NYA_Render2DFlushReason.
     * */
    u32 frame_flush_reasons[NYA_RENDER2D_FLUSH_REASON_COUNT];

    /** Why the next flush is happening. Set by whatever forces it, consumed when it does. */
    u32 pending_flush_reason;

    /**
     * The next window pass to open must resolve multisampling onto the swapchain. False for every pass but
     * the last of a frame — see _nya_render2d_pass_resume: a frame opens one pass per flush, and resolving
     * on each of them resolves the whole target once per draw call. Render texture passes ignore this and
     * always resolve, since they are ended explicitly and their contents are read immediately afterwards.
     * */
    b8 resolve_pending;

    /**
     * Draws asked for and not made this frame: every reason a draw silently does nothing — no pipeline
     * loaded yet, no texture, no render pass, a shape too large for the batch. Silence is right in release
     * and misleading in development, where "nothing appeared" and "nothing was asked for" look identical.
     * */
    u32 frame_dropped_draws;

    /*
     * ── Batch state ──
     *
     * What the queued vertices are waiting to be drawn with. A draw that needs anything different
     * flushes what is queued first, because one flush is one draw call and a draw call has exactly
     * one pipeline and one texture. That is the whole batching rule: shapes of the same kind
     * accumulate, a change costs a draw call.
     */

    /**
     * The pipeline asset the queued vertices want. Null when nothing is queued. NYA_CString rather than
     * NYA_AssetHandle, which is a typedef for exactly that: core_asset.h includes this file, so naming its
     * typedef here would be a cycle. Same type either way.
     * */
    NYA_CString pipeline;

    /** Bound at t0 for a textured draw, null for an untextured one. */
    SDL_GPUTexture* texture;

    /** The sampler that texture is read through. Part of the batch state, so a change flushes. */
    SDL_GPUSampler* sampler;

    /**
     * Set by nya_render2d_shader_begin, and used in place of whichever pipeline a draw would otherwise
     * pick. Null means "use the normal one".
     * */
    NYA_CString shader_override;

    /*
     * ── Custom shader uniforms ──
     *
     * Bytes handed to the fragment stage at slot 0 for whatever pipeline is overriding. Vertex slot
     * 0 is the projection and is not available; a custom shader that needs to know its texel size,
     * a blur direction or a threshold gets it here.
     *
     * Stored inline rather than as a pointer because the caller's data is theirs the moment the call
     * returns, and the value is not pushed until the flush — which may be several draws later.
     */
    u8  shader_uniform[NYA_RENDER2D_MAX_UNIFORM_BYTES];
    u32 shader_uniform_size;

    /*
     * ── Render target ──
     *
     * What the render pass is drawing into, which is the swapchain until nya_render_texture_begin
     * points it somewhere else. Held here rather than read from the window because the projection
     * has to match the *target* size: drawing into a 256x256 render texture with the window's
     * projection puts everything in its top left corner.
     */
    /** Where the finished pixels end up: the swapchain image, or a render texture's resolved side. */
    SDL_GPUTexture* target_texture;

    /**
     * What the render pass actually draws into, when multisampling is on.
     *
     * Null means no multisampling and `target_texture` is drawn into directly. When it is set, the
     * pass renders here and resolves onto `target_texture` as it ends.
     * */
    SDL_GPUTexture* target_msaa;

    /**
     * The depth buffer attached alongside, whichever target is current. Tracked on the batch rather than
     * looked up from the window, for the same reason the colour target is: a render texture has its own,
     * and a pass reopened mid-frame has to reattach the one belonging to the target it is actually drawing
     * into.
     * */
    SDL_GPUTexture* target_depth;

    u32 target_width;
    u32 target_height;

    /** The render texture nya_render_texture_end has to restore the swapchain from. */
    b8 target_is_texture;

    /*
     * ── View ──
     */

    /**
     * The camera the queued vertices are to be seen through. Part of the projection, which is a per flush
     * uniform, so changing it flushes. Held rather than applied to the vertices as they are built, which
     * would bake the camera in and make a mid frame camera change retroactive.
     * */
    /*
     * ── Scissor ──
     *
     * A rectangle outside which nothing is drawn. Render pass state rather than anything carried in
     * the vertices, so changing it flushes — and it has to be reapplied every time the pass reopens,
     * because a suspend for a copy pass drops it along with everything else the pass held.
     */
    b8  scissor_active;
    s32 scissor_x, scissor_y, scissor_width, scissor_height;

    /**
     * The camera the queued vertices are to be seen through, or kind NONE for screen pixels. NONE is the
     * common case and the whole UI case — including the UI drawn over a 3D scene — and the flush skips
     * building and multiplying a view matrix rather than multiplying by identity.
     * */
    NYA_Camera2D camera;
};

/**
 * The 3D mesh batch for one window. See render3d.h; nothing outside that file touches this. A second batch
 * beside the 2D one rather than a mode on it, because the two carry different vertices — forty-eight bytes
 * against twenty — and are drawn by different pipelines with different depth state. What they share is
 * the render pass and the buffers' lifetime, and that is all they need to share.
 * */
/**
 * The instances queued for one retained mesh this pass, and the mesh they belong to. A group per distinct
 * mesh rather than a flat list of (mesh, instance) pairs, since the draw call is per mesh — vertex buffer
 * and textures bound once, instance count the only thing that varies. A flat list would have to be sorted
 * by mesh before it could be drawn, every pass, to recover exactly this.
 * */
/** One transparent triangle's place in the queue: how far away it is, and where its indices start. */
struct NYA_Render3DSortKey {
    /**
     * Squared distance from the eye to the triangle's centroid. Squared, since only the ordering matters
     * and a square root would be one per triangle to reach the same order. The centroid rather than the
     * nearest vertex: a nearest-vertex key sorts two triangles sharing an edge by whichever happens to own
     * the closer copy of it, which flickers as the camera moves.
     * */
    f32 depth;

    /** Index of the triangle's first element in the transparent stream's index array. */
    u32 first;
};

/**
 * Geometry a caller built itself and handed to the renderer to keep. See nya_render3d_mesh_register. The
 * retained path was reachable only through the asset system, serving models read off disk and nothing
 * else — while the case that needed it most was generated geometry. A terrain is thousands of triangles
 * that change when a seed does and never between, and pushing it through the immediate path meant
 * transforming and uploading every vertex again for the camera pass and each shadow cascade. One part and
 * no texture, unlike an asset mesh: a caller with several materials registers several meshes, the same
 * thing a multi-material model already costs.
 * */
typedef struct NYA_Render3DRegisteredMesh NYA_Render3DRegisteredMesh;

struct NYA_Render3DRegisteredMesh {
    /** Compared by pointer, like a mesh group's. Null means the slot is free. */
    NYA_ConstCString handle;

    SDL_GPUBuffer* vertices;
    u32            vertex_count;

    /**
     * The staged copy, held until a frame exists to perform it in. Null once the upload has happened.
     * Registration is a *game* call — a surface is generated when a level loads or a seed changes, not
     * during rendering — so there is usually no command buffer open, and a GPU copy needs one. Creating the
     * buffers and filling the staging memory needs none of that, so all of it happens immediately and only
     * the copy waits for the next draw.
     *
     * The first version did the copy at registration regardless. Outside a frame that meant a copy pass
     * begun on a null command buffer, which fails quietly: the buffer existed, was never filled, and drew
     * as nothing at all.
     * */
    SDL_GPUTransferBuffer* pending_upload;

    /** Bytes the pending copy will move. Meaningless once `pending_upload` is null. */
    u32 pending_size;

    /** Computed once at registration, since the vertices cannot change without re-registering. */
    f32x3 bounds_min;
    f32x3 bounds_max;
};

typedef struct NYA_Render3DMeshGroup NYA_Render3DMeshGroup;

struct NYA_Render3DMeshGroup {
    /**
     * The asset handle, compared by pointer. Pointer equality rather than strcmp is sound here rather than
     * a shortcut: asset handles are interned string literals from the generated asset index, so two draws
     * of the same mesh pass literally the same pointer. Two *different* pointers holding equal text would
     * merely produce two groups drawing the same mesh — a wasted draw call, not a wrong picture.
     * */
    NYA_ConstCString handle;

    /** Where this group's run starts in the batch's shared instance array, and how long it is. */
    u32 first_instance;
    u32 instance_count;

    /**
     * Whether this mesh was tinted translucent, and therefore has to be drawn after the opaque ones. Per
     * group rather than per instance: an instance's tint is what decides it, and a group only ever collects
     * consecutive draws of one mesh — a caller alternating a translucent and an opaque tint on the same
     * model gets a group each, the same thing that already happens when they alternate two different models.
     * */
    b8 transparent;

    /** Squared distance from the eye to the nearest instance, for ordering the transparent groups. */
    f32 depth;
};

/**
 * One run of CPU-staged geometry: vertices and the indices into them. There are two of these, and the
 * split is the whole of transparency ordering: opaque geometry can be drawn in any order since the depth
 * buffer sorts it, while translucent geometry cannot, since blending is not commutative — two panes drawn
 * near-then-far give a different colour from far-then-near, and only the second is right. Keeping them in
 * separate streams is what makes sorting possible at all; sorting primitives before they are emitted would
 * fight the batching, since consecutive draws sharing state become one draw call, and reordering the calls
 * to fix blending reorders the state changes with them.
 * */
struct NYA_Render3DStream {
    NYA_Vertex3D* vertices;
    u32           vertex_count;

    u32* indices;
    u32  index_count;
};

struct NYA_Render3DBatch {
    SDL_GPUBuffer*         vertex_buffer;
    SDL_GPUTransferBuffer* transfer_buffer;
    SDL_GPUBuffer*         index_buffer;
    SDL_GPUTransferBuffer* index_transfer_buffer;

    /**
     * CPU side staging, filled by the draw calls and copied into the transfer buffer on flush. The two
     * share one GPU buffer and one capacity: opaque is uploaded at offset zero and transparent straight
     * after it, so the pair costs no more VRAM than the single stream did.
     * */
    NYA_Render3DStream opaque;
    NYA_Render3DStream transparent;

    /**
     * Which stream the primitives are currently writing into, decided by the colour they were given. State
     * rather than a parameter threaded through every emitter: a cube is six quads and a grid is dozens of
     * lines which are each six quads, so passing it down would mean touching every one of them to carry a
     * value that never changes within a primitive.
     * */
    b8 transparent_active;

    /**
     * Scratch for sorting the transparent stream: one key per triangle, and the reordered indices.
     * Allocated once with the streams rather than per flush. Sorting in place is not possible — a triangle
     * is three indices that have to move together — so the reordered run is written out beside the
     * original.
     * */
    NYA_Render3DSortKey* sort_keys;
    u32*                 sorted_indices;

    /** The radix sort's other half. It ping-pongs between this and `sort_keys`. */
    NYA_Render3DSortKey* sort_keys_scratch;

    /** Scratch for reordering one group's instances back to front. Same reasoning as `sorted_indices`. */
    NYA_Render3DInstance* sorted_instances;

    /**
     * The texture the queued triangles are to be drawn with, or null for the untextured pipeline. Batch
     * state exactly like `material` is: it selects the pipeline and the binding, both per draw call, so a
     * change to it has to flush first. nya_render3d_mesh does that; nothing else touches it, which is why
     * the primitives never leave it non-null.
     * */
    SDL_GPUTexture* texture;
    SDL_GPUSampler* sampler;

    /**
     * The frame's point lights. Frame state, like `light` and `material`. A fixed array on the batch
     * rather than an allocation, since the count is bounded by what one uniform block carries and the
     * whole set is copied into that block on every flush anyway.
     * */
    NYA_Render3DPointLight point_lights[NYA_RENDER3D_MAX_POINT_LIGHTS];
    u32                    point_light_count;

    /*
     * ── the shadow pass ──
     */

    /**
     * Light-space depth, and the depth buffer that decides which surface got written. A colour target
     * rather than a sampled depth texture: sampling a depth format needs SDL_GPU_TEXTUREUSAGE_SAMPLER on
     * it, which backends support unevenly and which fights the multisampling every other target here uses.
     * R32_FLOAT plus a plain depth buffer for the test uses only paths the renderer already relies on, and
     * costs one texture. Created on the first shadow pass and kept, since the size never changes — it is
     * NYA_RENDER3D_SHADOW_MAP_SIZE, not the window's.
     * */
    SDL_GPUTexture* shadow_color;
    SDL_GPUTexture* shadow_depth;

    NYA_Render3DShadow shadow;

    /**
     * One matrix per cascade, and how far each reaches. Filled as each pass runs. Kept on the batch rather
     * than rebuilt per flush because a cascade's pass fills one of these and the *scene* pass reads all of
     * them — the two are separated by every draw call in the frame.
     * */
    f32_4x4 shadow_view_projection[NYA_RENDER3D_SHADOW_CASCADES];
    f32     shadow_cascade_extent[NYA_RENDER3D_SHADOW_CASCADES];

    /** How many cascades have run this frame. Reset by nya_render3d_end, like `shadow_valid`. */
    u32 shadow_cascade_count;

    /** Which cascade the pass currently open is filling. Meaningless outside one. */
    u32 shadow_cascade;

    /** True between nya_render3d_shadow_begin and nya_render3d_shadow_end. Selects the depth pipeline. */
    b8 shadow_pass_active;

    /** True once a pass has run. Cleared by nya_render3d_end, so a frame that skips it is unshadowed. */
    b8 shadow_valid;

    /** False outside nya_render3d_begin and nya_render3d_end. Nothing draws while it is. */
    b8 active;

    /**
     * Whether a 3D camera has ever been set on this window. Never cleared by nya_render3d_end. Separate
     * from `active` because the two answer different questions, and conflating them broke picking
     * outright: `active` is "are we between begin and end", true only during on_render, while a click
     * arrives during on_event, one phase *earlier* — a ray built then found `active` false, fell back to a
     * ray at the origin pointing along -z, and quietly hit nothing. The camera from the last frame is the
     * right one to un-project this frame's click through: it is the one the player was looking through
     * when they clicked.
     * */
    b8 camera_valid;

    /**
     * The camera the queued vertices are seen through, already combined. Kept as the finished matrix
     * rather than rebuilt per flush: it depends on the target size, which cannot change between a begin
     * and an end, so recomputing it would be recomputing a constant.
     * */
    f32_4x4 view_projection;

    /*
     * ── The camera, kept as it was given ──
     *
     * The view-projection above is enough to draw with and not enough to un-project with, and
     * inverting it is both slower and worse conditioned than rebuilding the basis. See
     * nya_render3d_screen_ray.
     */
    NYA_Camera3DPerspective  camera;
    NYA_Camera3DOrthographic camera_orthographic;
    b8                       camera_is_ortho;

    /** Both are fragment uniforms, so changing either costs a draw call. See render3d.h. */
    NYA_Render3DLight    light;
    NYA_Render3DMaterial material;

    /**
     * The six clip planes of `view_projection`, as (a, b, c, d) with the normals pointing inward. Rebuilt
     * whenever the matrix is, once per pass rather than once per draw — the planes are a property of the
     * camera, and a scene of ten thousand primitives would otherwise extract them ten thousand times to
     * answer the same question. Correct for the shadow pass without any special case, since the shadow
     * pass installs the *light's* matrix through the same function, so culling happens against the light's
     * volume — the right frustum for deciding what casts into the map, where the camera's would drop the
     * shadows of everything just off screen.
     * */
    f32x4 frustum[6];

    /**
     * The occlusion buffer this pass culls against, or null.
     *
     * A pointer to something the game owns rather than a buffer of its own: it is tens of kilobytes,
     * a scene may want one per camera or none at all, and which occluders go into it is a decision
     * only the game can make. Cleared on every begin like `light` and `material` are, so a buffer
     * built for last frame's camera cannot silently keep culling against it.
     * */
    const NYA_OcclusionBuffer* occlusion;

    /*
     * ── The retained mesh path ──
     *
     * Parallel to the immediate arrays above and drained by the same flush. See NYA_Render3DInstance.
     */

    /** Per-instance transforms, staged on the CPU and uploaded once per flush like the vertices are. */
    NYA_Render3DInstance* instances;
    u32                   instance_count;

    SDL_GPUBuffer*         instance_buffer;
    SDL_GPUTransferBuffer* instance_transfer_buffer;

    /** One entry per distinct mesh queued, naming its run of `instances`. */
    NYA_Render3DMeshGroup mesh_groups[NYA_RENDER3D_MAX_MESH_GROUPS];
    u32                   mesh_group_count;

    /**
     * Geometry registered by the game rather than loaded as an asset. See NYA_Render3DRegisteredMesh. A
     * flat array rather than a dictionary: the count is small by construction — a scene has a handful of
     * generated meshes, not hundreds — and a linear scan over a few pointers beats hashing a string.
     * */
    NYA_Render3DRegisteredMesh registered_meshes[NYA_RENDER3D_MAX_REGISTERED_MESHES];

    /** Ink width in world units, and its colour. Zero width switches the outline pass off. */
    f32       outline_thickness;
    NYA_Color outline_color;

    /** What the transparent stream does: blend toward, or add to. See nya_render3d_blend_set. */
    NYA_Render3DBlend blend;

    /** See nya_render3d_depth_set. Selects the overlay pipeline, for gizmos. */
    NYA_Render3DDepth depth;

    /**
     * A copy of the opaque scene, for refractive glass to sample. See NYA_Render3DMaterial.refraction.
     * Created on the first frame something asks for refraction and resized when the target does, so a
     * scene with no glass in it never allocates one. It is a *copy* rather than the target itself because a
     * shader may not sample the render target it is writing to — the whole reason a capture exists rather
     * than the glass reading the framebuffer directly.
     * */
    SDL_GPUTexture* refraction_capture;
    u32             refraction_width;
    u32             refraction_height;

    /*
     * ── Per frame counters, reset by nya_render_begin and read through nya_render3d_frame_stats ──
     *
     * Both halves of that were missing. They were incremented on every draw, cleared by nothing and read
     * by nothing — three write-only numbers growing for the life of the window, under a comment that said
     * they were per frame.
     */

    u32 frame_draw_calls;
    u32 frame_vertices;
    u32 frame_indices;

    /** Primitives too large for an empty batch. See _nya_render3d_reserve. */
    u32 frame_dropped_draws;

    /** Mesh copies drawn through the retained path this frame, and how many were culled before that. */
    u32 frame_instances;
    u32 frame_culled;

    /** Of those that survived the frustum, how many the occlusion buffer hid. See render_occlusion.h. */
    u32 frame_occluded;
};

struct NYA_RenderSystemWindow {
    SDL_GPURenderPass*    render_pass;
    SDL_GPUCommandBuffer* render_commands;
    SDL_GPUTexture*       swapchain_texture;

    /*
     * The window's multisampled colour buffer, and the size it was built for.
     *
     * Rebuilt whenever the swapchain changes size, which is the only thing that invalidates it — a
     * resize otherwise leaves the pass rendering into a buffer of the wrong dimensions.
     */
    SDL_GPUTexture* msaa_texture;
    u32             msaa_width;
    u32             msaa_height;

    /*
     * The window's depth buffer, and the size it was built for. Attached to every window pass,
     * unconditionally, whether or not anything 3D is drawn — what lets 2D and 3D share one render pass: a
     * pass's depth attachment is fixed when it opens, so making it conditional would mean tearing the pass
     * down and rebuilding it the first time a frame drew a cube, losing the 2D content already in it. The
     * price is one texture on a purely 2D game, never cleared to anything but 1.0, and the 2D pipelines
     * neither test nor write it, so nothing else about the 2D path changes.
     */
    SDL_GPUTexture* depth_texture;
    u32             depth_width;
    u32             depth_height;

    NYA_Render2DBatch draw_batch;
    NYA_Render3DBatch mesh_batch;
};

/*
 * ─────────────────────────────────────────────────────────
 * RENDERING STRUCTS
 * ─────────────────────────────────────────────────────────
 */

/**
 * One vertex of the immediate 3D batch. **Thirty-six bytes, and every field is the narrowest thing
 * that still says what it meant.**
 *
 * It was sixty-four, and about half of that was nothing. `f32x3` is an `ext_vector_type(3)`, which is
 * *sixteen* bytes rather than twelve — the padding NYA_ShaderMesh3DUniform already documents — so two
 * of them wasted eight bytes before the four-float colour is counted. A vertex travels a long way:
 * the batch array on the CPU, the transfer buffer, the upload, the device buffer, and every registered
 * mesh keeps its own copy. Halving it halves all of that, and `VULKAN_UploadToBuffer` is 2.4% of a
 * release profile precisely because the scene is emitted once per shadow cascade and then again for
 * the camera.
 *
 * ## Why these formats and not narrower ones
 *
 * **Colour is HALF4, not the UBYTE4_NORM the 2D vertex uses.** Eight bits per channel is right for 2D
 * because that is what the swapchain stores, and wrong here: a vertex colour above one is how an
 * emissive surface is pushed past the bloom threshold, and gnyame's fire starts at 1.15 red for
 * exactly that reason. Normalized bytes clamp it to one and the flame stops glowing — a look change
 * that no test would catch. Halves keep everything up to 65504.
 *
 * **Normals stay full floats.** Octahedral in four bytes is the usual packing and would take this to
 * twenty-eight, but `mesh3d_edge` finds edges by taking `fwidth` of the interpolated normal, and a
 * derivative of a quantised value is a different thing from a derivative. Worth doing, worth measuring
 * first; see the TODO.
 *
 * Every format here still arrives at the shader as the `float3`/`float4`/`float2` it always did —
 * UBYTE4_NORM, HALF2 and HALF4 are expanded by the input assembler — so not one line of shader
 * changed for this. Build one with `nya_vertex3d`, which takes the wide types callers already hold.
 * */
struct NYA_Vertex3D {
    /** Three plain floats, not an `f32x3`: the vector type would pad this out to sixteen bytes. */
    f32 position[3];

    /** HALF2. Texture coordinates do not need more than eleven bits of mantissa. */
    f16 uv[2];

    f32 normals[3];

    /** HALF4, so an emissive vertex colour above one survives to the tonemap. See the note above. */
    f16 color[4];
};

static_assert(sizeof(NYA_Vertex3D) == 36, "the 3D vertex layout in core_asset.c describes a 36 byte vertex");

/**
 * Builds one, from the wide types a caller actually has.
 *
 * The narrowing lives here rather than at nine call sites, and it is the only thing that knows the
 * storage is packed — which is what lets the fields get narrower again later without touching anyone.
 * */
NYA_API NYA_Vertex3D nya_vertex3d(f32x3 position, NYA_Color color, f32x3 normal, f32x2 uv) __attr_no_discard;

/**
 * The position back out as a vector, for the arithmetic that wants one — bounds, face normals, sorting.
 *
 * The field is three plain floats so the struct does not carry an `f32x3`'s padding, and a `f32[3]` does
 * not convert to a vector on its own. One place to widen it, rather than the same three lines wherever a
 * vertex is read.
 * */
NYA_API f32x3 nya_vertex3d_position(NYA_Vertex3D vertex) __attr_no_discard;

/**
 * One drawn copy of a retained mesh: where it is, and what colour it is tinted. The per-instance half of
 * NYA_VERTEX_LAYOUT_3D_INSTANCED, and the thing the renderer did not have: every primitive in the
 * immediate batch is baked into world space on the CPU precisely because there is nowhere to put a
 * per-primitive transform, and this is that place, for the one case where it pays — geometry uploaded once
 * and drawn many times. Eighty bytes per copy, against a low-poly character's several thousand vertices at
 * sixty-four bytes each. That ratio is the whole argument.
 * */
struct NYA_Render3DInstance {
    /**
     * Model to world, column-major to match the engine's matrices and the shader's `mul(m, v)`. Read by
     * the vertex shader as four FLOAT4 attributes, since a vertex attribute holds at most four components
     * and no graphics API has a matrix element format.
     * */
    f32_4x4 model;

    /** Multiplied onto the vertex colour, which already carries the mesh part's own material colour. */
    NYA_Color tint;
};

/**
 * The vertex the 2D batch uses. Twenty bytes, against NYA_Vertex3D's sixty-four. A separate type rather
 * than reusing NYA_Vertex3D, since three quarters of that one is dead weight here and the batch is the one
 * place where vertex size is a real cost — uploaded in full every frame, so a vertex's width is bandwidth
 * spent per frame rather than once. Where the sixty-four go: `position` is an f32x3 occupying sixteen
 * bytes rather than twelve, since a three-wide vector is aligned as if it were four; `normals` is another
 * sixteen that no 2D shader reads; `color` is four floats where four bytes carry the same information.
 * Plain scalars rather than f32x2 fields, deliberately — an f32x2 is eight-byte aligned, which would pad
 * this to twenty-four, while scalars are four-byte aligned and pack to exactly twenty, staying aligned
 * either way as the hardware asks.
 * */
struct NYA_Vertex2D {
    f32 x, y;
    f32 u, v;

    /**
     * RGBA bytes, not floats. Declared to the pipeline as UBYTE4_NORM, so the input assembler expands each
     * byte to a float in 0..1 before the shader sees it — the shader still reads a `float4` and nothing in
     * it changes. Eight bit colour channels are what the swapchain stores anyway.
     * */
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
 * Acquires a swapchain image and opens a render pass on it. Returns false when there is nothing to draw
 * into this frame, which is normal rather than an error: a minimised or occluded window has no swapchain
 * image, and on Wayland that happens routinely. **The caller must not draw when this returns false** —
 * there is no render pass, and the command buffer has already been cancelled.
 * */
NYA_API b8   nya_render_begin(NYA_Window* window) __attr_no_discard;
NYA_API void nya_render_end(NYA_Window* window);

// Last, and after NYA_RenderTexture above: the post chain is built out of a pair of them.
#include "nyangine/renderer/render_post.h"

// After NYA_Render3DSortKey above, which it sorts.
#include "nyangine/renderer/render_sort.h"
#include "nyangine/renderer/render_lod.h"
#include "nyangine/renderer/render_text.h"
#include "nyangine/renderer/render_font.h"
