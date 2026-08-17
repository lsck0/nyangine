/**
 * @file render2d.c
 *
 * The 2D batch. See render2d.h for what it is and how to use it.
 *
 * This file is the real implementation; render2d_headless.c is the same public surface doing
 * nothing, and nyangine.c includes whichever the build calls for. They were one file behind a single
 * #if, which put every function in the subsystem seventeen hundred lines from its own stub — the
 * arrangement that let a set of camera functions be edited on one side only and still compile,
 * because nothing referenced them.
 *
 * The two must keep the same public surface. Nothing enforces that but the linker, so when adding a
 * function here, add its stub there.
 * */
#include "assets/shader/uniforms.h"

#include "nyangine/nyangine.h"

#include "nyangine/renderer/render_internal.h"

#include "assets/assets.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API DECLARATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */


/**
 * The ASCII range baked into every atlas up front, inclusive.
 *
 * Not the whole atlas any more — see NYA_RENDER2D_GLYPH_CAPACITY. These are baked eagerly because
 * essentially every string contains them, and because it keeps their lookup a subtraction rather
 * than a hash.
 * */
#define NYA_RENDER2D_GLYPH_FIRST 32
#define NYA_RENDER2D_GLYPH_LAST  126
#define NYA_RENDER2D_GLYPH_ASCII (NYA_RENDER2D_GLYPH_LAST - NYA_RENDER2D_GLYPH_FIRST + 1)

/**
 * Glyphs one atlas can hold in total: the ASCII block plus whatever else gets asked for.
 *
 * The atlas grew from a fixed ASCII grid into this because ASCII is not text. A byte-indexed atlas
 * draws `é` as two gaps and `日` as three, which is not a rendering limitation a game can work
 * around — it is the renderer refusing to display most of the languages it might be translated into.
 *
 * Five hundred and twelve covers Latin, Latin-1 Supplement, Latin Extended-A and a comfortable
 * margin, which is every European language. It does not cover CJK, and could not: a grid of fixed
 * cells large enough for those is megabytes, and that case wants a real packer and eviction.
 * */
#ifndef NYA_RENDER2D_GLYPH_CAPACITY
#define NYA_RENDER2D_GLYPH_CAPACITY 512
#endif

/** Cells across the atlas texture. Rows follow from the capacity. */
#define NYA_RENDER2D_GLYPH_COLUMNS 16

/** Fonts whose atlases are held at once. Small: a game uses a handful of faces, not hundreds. */
#define NYA_RENDER2D_FONT_CACHE_MAX 8

/** Longest derived font asset handle: a path, an '@', and a point size. */
#define NYA_RENDER2D_FONT_HANDLE_MAX 256

typedef struct NYA_Glyph      NYA_Glyph;
typedef struct NYA_FontAtlas  NYA_FontAtlas;

/** One glyph's place in the atlas, and what it does to the pen. All in pixels except the uvs. */
struct NYA_Glyph {
    f32 u0, v0, u1, v1;

    f32 width, height;

    /** Where the quad sits relative to the pen, which is not the glyph's own top left. */
    f32 bearing_x, bearing_y;

    /** How far the pen moves after drawing it. */
    f32 advance;
};

struct NYA_FontAtlas {
    /**
     * The path the face was loaded from. Null means the slot is free.
     *
     * The *path*, not an asset handle: a face carries no size, so one file at two point sizes is two
     * assets, and the asset key is derived below rather than invented by the caller.
     * */
    NYA_ConstCString path;

    /** Point size this atlas was rasterised at. Part of the cache key, with the path. */
    f32 point_size;

    /**
     * The asset handle, built from the path and the point size — "./assets/fonts/x.ttf@19".
     *
     * Owned here because the asset system keeps the pointer it is given, and the caller only ever
     * passes a path. This is what removes the need for a game to invent handles like "neat_font"
     * just to have the same face at a second size.
     * */
    char handle[NYA_RENDER2D_FONT_HANDLE_MAX];

    /**
     * The TTF_Font the glyphs were rasterised from, purely to notice that it is no longer the one
     * the asset holds.
     *
     * A reload replaces the asset's TTF_Font with a new one built from the new file. Nothing else
     * about the asset changes identity, so comparing this pointer is how the atlas learns that the
     * glyphs it baked belong to a font that no longer exists.
     * */
    TTF_Font* source_font;

    SDL_GPUTexture* texture;

    /** Baseline to baseline. What to add to y for the next line. */
    f32 line_height;

    /** Top of the line box to the baseline, and baseline to the deepest descender (positive). */
    f32 ascent;
    f32 descent;

    /*
     * ── The lazily baked glyph table ──
     *
     * Slots 0..NYA_RENDER2D_GLYPH_ASCII-1 are ASCII, in order, so that lookup for the overwhelmingly
     * common case is `codepoint - 32` and nothing else. Everything past that is filled in as it is
     * first asked for, which is what makes the atlas cover a language without knowing in advance
     * which language.
     */

    NYA_Glyph glyphs[NYA_RENDER2D_GLYPH_CAPACITY];

    /** What each slot holds. Only meaningful at or past the ASCII block. */
    u32 codepoints[NYA_RENDER2D_GLYPH_CAPACITY];

    /** Slots used. Starts at the ASCII block's size and grows as glyphs are baked. */
    u32 glyph_count;

    /**
     * The CPU side of the atlas, kept alive after the initial upload.
     *
     * Held rather than freed because a glyph baked later has to be blitted *somewhere* before it can
     * be uploaded, and re-rasterising the whole atlas to add one character would be far worse. Costs
     * about a megabyte per face, which for the handful of faces a game uses is the right trade.
     * */
    SDL_Surface* surface;

    /** Reused for every upload rather than created per glyph. Sized for the whole atlas. */
    SDL_GPUTransferBuffer* transfer_buffer;

    s32 atlas_width;
    s32 atlas_height;
    s32 cell_width;
    s32 cell_height;

    /** Set when a glyph is baked and cleared by the upload. See _nya_render2d_atlas_upload. */
    b8 upload_pending;
};

/** Appends one vertex with explicit uv. Callers reserve first, so this never checks for space. */
NYA_INTERNAL void _nya_render2d_vertex(NYA_Render2DBatch* batch, f32 x, f32 y, f32 u, f32 v, NYA_Color color);


/**
 * Appends one triangle, by offsets relative to the first vertex of the shape being built.
 *
 * Relative rather than absolute so a shape's winding reads the same wherever it lands in the batch —
 * a quad is always 0,1,2 / 0,2,3, and the base is added here.
 * */
NYA_INTERNAL void _nya_render2d_triangle_indices(NYA_Render2DBatch* batch, u32 base, u32 a, u32 b, u32 c);

/** Packs a float colour into the four normalized bytes NYA_Vertex2D stores. */
NYA_INTERNAL void _nya_render2d_pack_color(NYA_Color color, OUT u8 out_rgba[4]);

/**
 * Flushes if the pending draw needs a different pipeline or texture, then makes room for `count`.
 *
 * Returns false when the batch cannot draw at all. Every draw call bails on that rather than half
 * emitting, so a shape is either wholly queued or not queued.
 * */
/**
 * Flushes, recording why.
 *
 * The reason is set immediately before rather than passed into nya_render2d_flush, because that function
 * is public and the reason is not something a caller should have to name.
 * */
NYA_INTERNAL void _nya_render2d_flush_for(NYA_Window* window, NYA_Render2DFlushReason reason);

/** The body both nya_render2d_textf variants share, so the varargs are unpacked in exactly one place. */
NYA_INTERNAL void _nya_render2d_textf_va(NYA_Window* window, NYA_ConstCString font_path, f32 point_size, f32 x, f32 y, NYA_Color color, NYA_ConstCString format, va_list arguments);

NYA_INTERNAL b8 _nya_render2d_prepare(NYA_Window* window, NYA_ConstCString pipeline, SDL_GPUTexture* texture, SDL_GPUSampler* sampler, u32 vertex_count, u32 index_count);

/**
 * The next laid-out line, and how far to advance to reach the one after it.
 *
 * Returns false at the end of the string. `out_length` is the line's bytes and `out_skip` is what to add
 * to the cursor — the two differ by the separator, so a line broken at a space reports the space in the
 * skip and not in the length, and a trailing space never widens a right-aligned line.
 * */
NYA_INTERNAL b8 _nya_render2d_wrap_line(
    NYA_FontAtlas*   atlas,
    TTF_Font*        face,
    NYA_ConstCString cursor,
    f32              max_width,
    OUT u64*         out_length,
    OUT u64*         out_skip,
    OUT f32*         out_width
);

/** Lays a box out, drawing when `window` is non-null and only measuring when it is not. */
NYA_INTERNAL f32x2 _nya_render2d_text_box_layout(NYA_Window* window, NYA_ConstCString text, NYA_Render2DTextBox params);

/** Queues one axis aligned textured quad. The shared tail of every rect, texture and glyph draw. */
NYA_INTERNAL void _nya_render2d_quad(NYA_Render2DBatch* batch, f32 x, f32 y, f32 width, f32 height, f32 u0, f32 v0, f32 u1, f32 v1, NYA_Color color);

/**
 * Queues a quad from four already positioned corners, in the order top left, top right, bottom
 * right, bottom left.
 *
 * The general case behind _nya_render2d_quad, which exists so a rotated sprite costs the caller the
 * trigonometry and nothing else — the batch has no per shape transform, so rotation has to be baked
 * into the vertices as they are built.
 * */
NYA_INTERNAL void _nya_render2d_quad_corners(NYA_Render2DBatch* batch, const f32x2 corners[4], f32 u0, f32 v0, f32 u1, f32 v1, NYA_Color color);

/**
 * The four corners of a rectangle centred on `center` and turned by `rotation`, in the order
 * _nya_render2d_quad_corners expects: top left, top right, bottom right, bottom left before the turn.
 *
 * Shared by the fill and the outline so the two cannot disagree about where the shape is.
 * */
NYA_INTERNAL void _nya_render2d_rect_rotated_corners(f32x2 center, f32x2 size, f32 rotation, OUT f32x2 out_corners[4]);

/*
 * Closes and reopens the render pass around work that needs a copy pass.
 *
 * SDL_GPU forbids a copy pass while a render pass is open, and both the vertex upload and the glyph
 * atlas upload are copy passes that happen mid frame. Rather than each doing the dance itself, they
 * bracket themselves with these — resume always reopens on whatever the current target is, with LOAD
 * rather than CLEAR, so nothing already drawn is lost.
 */
NYA_INTERNAL void _nya_render2d_pass_suspend(NYA_Window* window);

/**
 * Pushes the batch's scissor state onto the current render pass, or clears it.
 *
 * Called both when the scissor changes and every time the pass reopens, because a render pass starts
 * with no scissor and a suspend for a copy pass therefore loses it.
 * */
NYA_INTERNAL void _nya_render2d_apply_scissor(NYA_Window* window);
NYA_INTERNAL void _nya_render2d_range_close(NYA_Window* window);
NYA_INTERNAL s32  _nya_render2d_range_compare(const void* a, const void* b);
NYA_INTERNAL f32_4x4 _nya_render2d_range_projection(const NYA_Render2DDrawRange* range);
NYA_INTERNAL void _nya_render2d_range_apply_scissor(NYA_Window* window, const NYA_Render2DDrawRange* range);
NYA_INTERNAL void _nya_render2d_pass_resume(NYA_Window* window);

/**
 * Builds the glyph atlas for a font asset, or returns the one already built. Null on failure.
 *
 * `window` may be null, and is only used to borrow an open render pass for the upload. Measuring
 * calls it that way: a layout pass runs before anything is drawn, so there is no pass to borrow and
 * the upload gets its own command buffer instead.
 * */
NYA_INTERNAL NYA_FontAtlas* _nya_render2d_font_atlas(NYA_Window* window, NYA_ConstCString font_path, f32 point_size);

/**
 * The glyph for a codepoint, rasterising it into a free cell if it is not there yet.
 *
 * Null when the atlas is full or the face has no such glyph. ASCII resolves by subtraction; anything
 * else is a scan of what has been baked, which is short because a game uses one language at a time.
 * */
NYA_INTERNAL const NYA_Glyph* _nya_render2d_glyph(NYA_FontAtlas* atlas, u32 codepoint);

/** Rasterises one codepoint into `slot`'s cell of the atlas surface and fills in its NYA_Glyph. */
NYA_INTERNAL void _nya_render2d_glyph_bake(NYA_FontAtlas* atlas, TTF_Font* font, u32 codepoint, u32 slot);

/**
 * The face an atlas was built from, or null.
 *
 * Resolved through the asset system on every call rather than cached on the atlas, for the reason
 * _nya_render2d_font_atlas re-resolves: a hot reload replaces the TTF_Font, and a stored pointer would
 * outlive it. Callers take it once per string, not once per glyph.
 * */
NYA_INTERNAL TTF_Font* _nya_render2d_atlas_font(const NYA_FontAtlas* atlas) __attr_no_discard;

/**
 * The spacing adjustment between two glyphs, in pixels. Zero for the first glyph of a run.
 *
 * Kerning is the difference between text that is *set* and text that is merely placed: without it "AV"
 * and "To" carry the same gap as "nn", because an advance describes one glyph in isolation and cannot
 * know what follows. Most faces ship a few hundred pairs covering exactly the combinations that look
 * wrong without them.
 *
 * Applied identically by the draw, the measure and the wrap — a measure that disagreed with the draw
 * would lay a panel out against a width the text does not have.
 * */
NYA_INTERNAL f32 _nya_render2d_kerning(TTF_Font* font, u32 previous, u32 current) __attr_no_discard;

/**
 * Pushes the atlas surface to its texture, if anything has been baked since the last time.
 *
 * The whole surface rather than the one cell that changed. A cell upload would need its own region
 * arithmetic and a second transfer buffer to be worth it, and this runs on the frames where a string
 * first shows a character nobody has drawn before — which for a game is a handful of frames per run.
 * */
NYA_INTERNAL void _nya_render2d_atlas_upload(NYA_Window* window, NYA_FontAtlas* atlas);


/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * STATE
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/**
 * Glyph atlases, keyed by font handle.
 *
 * A module global rather than a field on the font asset, so that adding text rendering did not put
 * GPU state into the asset system. The cost is that it does not follow an asset being unloaded; with
 * eight slots and fonts that live as long as the game, that has not been worth solving.
 * */
NYA_INTERNAL NYA_FontAtlas _nya_render2d_font_cache[NYA_RENDER2D_FONT_CACHE_MAX] = { 0 };

/**
 * The font nya_render2d_text and the measurements use, set by nya_render2d_font_set.
 *
 * Immediate mode state, and deliberately not reset per frame: a game that uses one face sets it once
 * at startup. Null until something sets it, which makes every text call a no-op rather than a crash.
 * */
NYA_INTERNAL NYA_ConstCString _nya_render2d_current_font = nullptr;

/** Point size of the current font. Paired with _nya_render2d_current_font; the two are one setting. */
NYA_INTERNAL f32 _nya_render2d_current_font_size = 0.0F;

/**
 * The atlas for _nya_render2d_current_font, resolved once rather than looked up per call.
 *
 * Every text call and every metric used to walk the cache doing a strcmp per entry. Cleared by
 * nya_render2d_font_set, so it cannot outlive the font it belongs to.
 * */
NYA_INTERNAL NYA_FontAtlas* _nya_render2d_current_atlas = nullptr;

/**
 * The 2x2 a camera applies to a world offset, before the view centre is added.
 *
 * The one place the two 2D projections differ. Everything downstream — the view matrix at flush, the
 * screen/world inverse, the world/screen forward — works on these four numbers and never asks which
 * kind produced them, which is what keeps the isometric camera from being a second copy of the
 * camera path.
 *
 * Written as `[a b; c d]`, matching the two rows of the view matrix it becomes.
 * */
NYA_INTERNAL void _nya_render2d_camera_basis(const NYA_Camera2D* camera, OUT f32* out_a, OUT f32* out_b, OUT f32* out_c, OUT f32* out_d);

/** The world (or tile) point the camera centres on, whichever kind it is. */
NYA_INTERNAL f32x2 _nya_render2d_camera_position(const NYA_Camera2D* camera) __attr_no_discard;

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PUBLIC API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

void nya_render2d_shutdown(void) {
    SDL_GPUDevice* gpu_device = nya_app_get()->render_system.gpu_device;

    /*
     * The atlases are keyed by font handle, not by window, so nothing per-window frees them — and
     * before this they were simply never released, at shutdown or otherwise.
     */
    for (u32 i = 0; i < NYA_RENDER2D_FONT_CACHE_MAX; i++) {
        NYA_FontAtlas* atlas = &_nya_render2d_font_cache[i];

        if (atlas->texture != nullptr) SDL_ReleaseGPUTexture(gpu_device, atlas->texture);
        if (atlas->transfer_buffer != nullptr) SDL_ReleaseGPUTransferBuffer(gpu_device, atlas->transfer_buffer);

        // The CPU side is kept alive for the whole run so that a glyph can be baked into it later,
        // so this is the one place it is freed.
        if (atlas->surface != nullptr) SDL_DestroySurface(atlas->surface);

        *atlas = (NYA_FontAtlas){ 0 };
    }

    // Cleared too: they point into the cache that was just emptied.
    _nya_render2d_current_font  = nullptr;
    _nya_render2d_current_atlas = nullptr;
}

/**
 * The projection a range draws through, built from its own target and camera.
 *
 * Per range rather than per flush because both change mid frame: a render texture has a different
 * size from the window it is composited into, and a world camera is set and cleared around the HUD.
 * The batch used to hold the only copy, which was fine when a flush drew exactly one state.
 *
 * Top is 0 and bottom is the height, which is what makes y grow downward from the top left.
 * */
NYA_INTERNAL f32_4x4 _nya_render2d_range_projection(const NYA_Render2DDrawRange* range) {
    f32_4x4 projection = nya_matrix_orthographic(0.0F, (f32)range->target_width, 0.0F, (f32)range->target_height);

    /*
     * The camera is a view matrix folded in ahead of the projection: world → screen pixels, then
     * screen pixels → clip space. Skipped entirely when no camera is set, which is the UI case and
     * the case for everything that existed before cameras did.
     *
     * The 2x2 is a rotation scaled by the zoom, and the translation puts the camera's position at
     * the centre of the target rather than at its corner — which is what makes zooming feel like it
     * happens around what you are looking at.
     */
    if (range->camera.kind == NYA_CAMERA2D_KIND_NONE) return projection;

    // Both kinds collapse to the same four numbers here, which is the whole reason the batch stores a
    // tagged camera rather than a top-down one: the flush does not care which it is.
    f32 a, b, c, d;
    _nya_render2d_camera_basis(&range->camera, &a, &b, &c, &d);

    f32x2 position = _nya_render2d_camera_position(&range->camera);

    f32 center_x = (f32)range->target_width * 0.5F;
    f32 center_y = (f32)range->target_height * 0.5F;

    f32 px = position[0];
    f32 py = position[1];

    f32_4x4 view = nya_matrix_create(
        (f32x4){ a, b, 0.0F, center_x - ((a * px) + (b * py)) },
        (f32x4){ c, d, 0.0F, center_y - ((c * px) + (d * py)) },
        (f32x4){ 0.0F, 0.0F, 1.0F, 0.0F },
        (f32x4){ 0.0F, 0.0F, 0.0F, 1.0F }
    );

    return projection * view;
}

/** The range's own clip rectangle. The batch's current one is not it: ranges are replayed out of order. */
NYA_INTERNAL void _nya_render2d_range_apply_scissor(NYA_Window* window, const NYA_Render2DDrawRange* range) {
    NYA_RenderSystemWindow* render = &window->render_system;

    if (render->render_pass == nullptr) return;

    if (!range->scissor_active) {
        // The whole target, which is what a pass starts as. SDL has no "disable", so the way to stop
        // clipping is to clip to everything.
        SDL_SetGPUScissor(render->render_pass,
                          &(SDL_Rect){ .x = 0, .y = 0, .w = (s32)range->target_width, .h = (s32)range->target_height });
        return;
    }

    SDL_SetGPUScissor(
        render->render_pass,
        &(SDL_Rect){ .x = range->scissor_x, .y = range->scissor_y, .w = range->scissor_width, .h = range->scissor_height }
    );
}

/**
 * Records everything queued since the last range as one draw, and starts the next.
 *
 * Does not touch the staging arrays: the geometry stays where it is and the range simply remembers
 * which slice of it belongs to this state. That is what lets several ranges share one upload.
 * */
NYA_INTERNAL void _nya_render2d_range_close(NYA_Window* window) {
    NYA_Render2DBatch* batch = &window->render_system.draw_batch;

    if (batch->index_count <= batch->range_first_index) return;
    if (batch->range_count >= NYA_RENDER2D_MAX_RANGES) return;

    NYA_Render2DDrawRange* range = &batch->ranges[batch->range_count];

    *range = (NYA_Render2DDrawRange){
        .layer       = batch->layer,
        .sequence    = batch->range_sequence,
        .first_index = batch->range_first_index,
        .index_count = batch->index_count - batch->range_first_index,

        // Resolved now, so replaying makes no decisions: shader mode is the same vertices through a
        // different pipeline, and which one it was is part of *this* range rather than of the batch.
        .pipeline = (NYA_CString)(batch->shader_override != nullptr ? batch->shader_override : batch->pipeline),

        .texture = batch->texture,
        .sampler = batch->sampler,

        .target_width  = batch->target_width,
        .target_height = batch->target_height,
        .camera        = batch->camera,

        .scissor_active = batch->scissor_active,
        .scissor_x      = batch->scissor_x,
        .scissor_y      = batch->scissor_y,
        .scissor_width  = batch->scissor_width,
        .scissor_height = batch->scissor_height,
    };

    // Copied, not borrowed. See NYA_Render2DDrawRange.uniform: the caller's struct is usually a stack
    // local that is gone by the time this is replayed.
    if (batch->shader_uniform_size > 0 && batch->shader_uniform_size <= NYA_RENDER2D_RANGE_UNIFORM_MAX) {
        nya_memcpy(range->uniform, batch->shader_uniform, batch->shader_uniform_size);
        range->uniform_size = batch->shader_uniform_size;
    }

    batch->range_count++;
    batch->range_sequence++;
    batch->range_first_index = batch->index_count;
}

/**
 * Layer first, declaration order second.
 *
 * The tie break is what makes this a *stable* sort by another name: two draws in the same layer keep
 * the order they were issued in, so a frame that never sets a layer renders exactly as it did before
 * ranges existed.
 * */
NYA_INTERNAL s32 _nya_render2d_range_compare(const void* a, const void* b) {
    const NYA_Render2DDrawRange* left  = a;
    const NYA_Render2DDrawRange* right = b;

    if (left->layer != right->layer) return left->layer < right->layer ? -1 : 1;
    if (left->sequence != right->sequence) return left->sequence < right->sequence ? -1 : 1;

    return 0;
}

void nya_render2d_layer_set(NYA_Window* window, s32 layer) {
    nya_assert(window != nullptr);

    NYA_Render2DBatch* batch = &window->render_system.draw_batch;

    if (layer == batch->layer) return;

    // Closes the range rather than drawing it: what has been queued belongs to the layer that was
    // set when it was queued, and the whole point is that it is issued in layer order later.
    _nya_render2d_range_close(window);

    batch->layer = layer;
}

s32 nya_render2d_layer(NYA_Window* window) {
    nya_assert(window != nullptr);

    return window->render_system.draw_batch.layer;
}

void nya_render2d_flush(NYA_Window* window) {
    // Timed per call rather than per frame on purpose: the run count is the draw call count, which is
    // the number the batching documentation tells you to act on, and the total is what it costs.
    nya_perf_time_this_function();

    nya_assert(window != nullptr);

    NYA_RenderSystemWindow* render = &window->render_system;
    NYA_Render2DBatch*          batch  = &render->draw_batch;

    // Whatever is still open becomes the last range, so the loop below is the only thing that draws.
    _nya_render2d_range_close(window);

    if (batch->range_count == 0) {
        batch->vertex_count      = 0;
        batch->index_count       = 0;
        batch->range_first_index = 0;
        return;
    }

    // No pass to draw into: the window is occluded or minimised. Drop what was queued rather than
    // holding it for a frame that may never come, which would draw stale geometry once it returned.
    if (render->render_pass == nullptr) {
        batch->vertex_count      = 0;
        batch->index_count       = 0;
        batch->range_count       = 0;
        batch->range_first_index = 0;
        batch->range_sequence    = 0;
        return;
    }

    SDL_GPUDevice* gpu_device  = nya_app_get()->render_system.gpu_device;
    u32            upload_size = (u32)(batch->vertex_count * sizeof(NYA_Vertex2D));

    // Mapped and unmapped around the copy rather than held open, so the driver is free to move the
    // transfer buffer between frames.
    void* mapped = SDL_MapGPUTransferBuffer(gpu_device, batch->transfer_buffer, true);
    nya_assert(mapped != nullptr, "SDL_MapGPUTransferBuffer() failed: %s", SDL_GetError());
    nya_memcpy(mapped, batch->vertices, upload_size);
    SDL_UnmapGPUTransferBuffer(gpu_device, batch->transfer_buffer);

    u32   index_upload_size = (u32)(batch->index_count * sizeof(u32));
    void* mapped_indices    = SDL_MapGPUTransferBuffer(gpu_device, batch->index_transfer_buffer, true);
    nya_assert(mapped_indices != nullptr, "SDL_MapGPUTransferBuffer() failed for indices: %s", SDL_GetError());
    nya_memcpy(mapped_indices, batch->indices, index_upload_size);
    SDL_UnmapGPUTransferBuffer(gpu_device, batch->index_transfer_buffer);

    _nya_render2d_pass_suspend(window);

    SDL_GPUCopyPass* copy_pass = SDL_BeginGPUCopyPass(render->render_commands);
    SDL_UploadToGPUBuffer(
        copy_pass,
        &(SDL_GPUTransferBufferLocation){ .transfer_buffer = batch->transfer_buffer, .offset = 0 },
        &(SDL_GPUBufferRegion){ .buffer = batch->vertex_buffer, .offset = 0, .size = upload_size },
        true
    );
    SDL_UploadToGPUBuffer(
        copy_pass,
        &(SDL_GPUTransferBufferLocation){ .transfer_buffer = batch->index_transfer_buffer, .offset = 0 },
        &(SDL_GPUBufferRegion){ .buffer = batch->index_buffer, .offset = 0, .size = index_upload_size },
        true
    );
    SDL_EndGPUCopyPass(copy_pass);

    _nya_render2d_pass_resume(window);

    /*
     * Built here rather than cached, because it depends on the *target* size and that changes both
     * on a window resize and every time drawing moves in or out of a render texture. It is four
     * multiplies; caching it would mean invalidating it correctly.
     *
     * Top is 0 and bottom is the height, which is what makes y grow downward from the top left.
     */
    /*
     * Sorted, then issued.
     *
     * The buffers are bound once for all of them: every range indexes into the same upload and
     * differs only in where its slice starts, which is the reason recording ranges costs a draw call
     * each and no more than it did before.
     */
    qsort(batch->ranges, batch->range_count, sizeof(NYA_Render2DDrawRange), _nya_render2d_range_compare);

    SDL_BindGPUVertexBuffers(render->render_pass, 0, &(SDL_GPUBufferBinding){ .buffer = batch->vertex_buffer, .offset = 0 }, 1);
    SDL_BindGPUIndexBuffer(render->render_pass, &(SDL_GPUBufferBinding){ .buffer = batch->index_buffer, .offset = 0 }, SDL_GPU_INDEXELEMENTSIZE_32BIT);

    for (u32 i = 0; i < batch->range_count; i++) {
        const NYA_Render2DDrawRange* range = &batch->ranges[i];

        if (range->index_count == 0) continue;

        NYA_Asset* pipeline_asset = range->pipeline != nullptr ? nya_asset_get((NYA_CString)range->pipeline) : nullptr;

        // Still loading, which is normal for the first frames of a run. Skipped rather than holding
        // the whole flush, so one unloaded pipeline does not take the rest of the frame with it.
        if (pipeline_asset == nullptr || pipeline_asset->status != NYA_ASSET_STATUS_LOADED) continue;

        // Rebuilt per range because the target and the camera are part of the range, not of the
        // frame: a render texture and the window it is composited into have different projections.
        f32_4x4 range_projection = _nya_render2d_range_projection(range);

        _nya_render2d_range_apply_scissor(window, range);

        SDL_BindGPUGraphicsPipeline(render->render_pass, pipeline_asset->as_graphics_pipeline.pipeline);
        SDL_PushGPUVertexUniformData(render->render_commands, 0, &range_projection, sizeof(range_projection));

        // Only when a custom shader asked for it. The built in pipelines declare no fragment uniforms,
        // and pushing to a slot a pipeline does not declare is a validation error rather than a no-op.
        if (range->uniform_size > 0) {
            SDL_PushGPUFragmentUniformData(render->render_commands, 0, range->uniform, range->uniform_size);
        }

        if (range->texture != nullptr) {
            SDL_BindGPUFragmentSamplers(
                render->render_pass,
                0,
                &(SDL_GPUTextureSamplerBinding){ .texture = range->texture, .sampler = range->sampler },
                1
            );
        }

        SDL_DrawGPUIndexedPrimitives(render->render_pass, range->index_count, 1, range->first_index, 0, 0);

        // Counted per range, because a range *is* a draw call — which is the number the batching
        // documentation tells you to act on.
        batch->frame_flushes++;
    }

    batch->frame_vertices += batch->vertex_count;
    batch->frame_indices  += batch->index_count;

    // Consumed here rather than at the call site, so a flush nobody attributed still lands somewhere
    // — the frame end case, which is what an unattributed flush always is.
    batch->frame_flush_reasons[batch->pending_flush_reason % NYA_RENDER2D_FLUSH_REASON_COUNT]++;
    batch->pending_flush_reason = NYA_RENDER2D_FLUSH_FRAME_END;

    batch->vertex_count      = 0;
    batch->index_count       = 0;
    batch->range_count       = 0;
    batch->range_first_index = 0;
    batch->range_sequence    = 0;
}

void _nya_render2d_textf_va(NYA_Window* window, NYA_ConstCString font_path, f32 point_size, f32 x, f32 y, NYA_Color color, NYA_ConstCString format, va_list arguments) {
    char text[NYA_RENDER2D_TEXT_MAX];

    // Truncated rather than grown. vsnprintf always terminates, so an over-long line is a short line
    // rather than a buffer overrun, and a HUD string past this length is a bug worth seeing.
    (void)vsnprintf(text, sizeof(text), format, arguments);

    nya_render2d_text_with_font(window, font_path, point_size, text, x, y, color);
}

void _nya_render2d_flush_for(NYA_Window* window, NYA_Render2DFlushReason reason) {
    window->render_system.draw_batch.pending_flush_reason = reason;
    nya_render2d_flush(window);
}

/*
 * ─────────────────────────────────────────────────────────
 * SHAPES
 * ─────────────────────────────────────────────────────────
 */

void nya_render2d_rect(NYA_Window* window, f32 x, f32 y, f32 width, f32 height, NYA_Color color) {
    nya_assert(window != nullptr);

    // Two triangles sharing the diagonal. Not indexed: an index buffer saves two vertices per quad
    // and costs a second buffer to upload and keep in step, the wrong trade at this size.
    if (!_nya_render2d_prepare(window, NYA_RENDER2D_PIPELINE_SHAPES, nullptr, nullptr, 4, 6)) return;

    NYA_Render2DBatch* batch = &window->render_system.draw_batch;

    // Zeroed uvs: the shape fragment shader ignores them, and they exist only so this and a textured
    // draw share one vertex layout and one batch.
    _nya_render2d_quad(batch, x, y, width, height, 0.0F, 0.0F, 0.0F, 0.0F, color);
}

void nya_render2d_rect_outline(NYA_Window* window, f32 x, f32 y, f32 width, f32 height, f32 thickness, NYA_Color color) {
    nya_assert(window != nullptr);

    if (thickness <= 0.0F) return;

    // Clamped so an outline thicker than the rectangle fills it, rather than drawing four bars that
    // overlap and double blend into a darker patch in the middle.
    f32 horizontal = nya_min(thickness, height * 0.5F);
    f32 vertical   = nya_min(thickness, width * 0.5F);

    // Top and bottom span the full width; left and right are inset by those, so each corner is
    // covered exactly once. Overlapping them would show through at any alpha below one.
    nya_render2d_rect(window, x, y, width, horizontal, color);
    nya_render2d_rect(window, x, y + height - horizontal, width, horizontal, color);
    nya_render2d_rect(window, x, y + horizontal, vertical, height - (horizontal * 2.0F), color);
    nya_render2d_rect(window, x + width - vertical, y + horizontal, vertical, height - (horizontal * 2.0F), color);
}

void nya_render2d_rect_rotated(NYA_Window* window, f32x2 center, f32x2 size, f32 rotation, NYA_Color color) {
    nya_assert(window != nullptr);

    if (size.x <= 0.0F || size.y <= 0.0F) return;

    if (!_nya_render2d_prepare(window, NYA_RENDER2D_PIPELINE_SHAPES, nullptr, nullptr, 4, 6)) return;

    NYA_Render2DBatch* batch = &window->render_system.draw_batch;

    f32x2 corners[4];
    _nya_render2d_rect_rotated_corners(center, size, rotation, corners);

    _nya_render2d_quad_corners(batch, corners, 0.0F, 0.0F, 0.0F, 0.0F, color);
}

void nya_render2d_rect_rotated_outline(NYA_Window* window, f32x2 center, f32x2 size, f32 rotation, f32 thickness, NYA_Color color) {
    nya_assert(window != nullptr);

    if (thickness <= 0.0F) return;
    if (size.x <= 0.0F || size.y <= 0.0F) return;

    f32x2 corners[4];
    _nya_render2d_rect_rotated_corners(center, size, rotation, corners);

    // Four separate lines rather than one closed polyline, so this reads as the outline of the same
    // four corners the fill uses and stays correct if the corner order ever changes.
    for (u32 i = 0; i < 4; i++) nya_render2d_line(window, corners[i], corners[(i + 1) % 4], thickness, color);
}

void nya_render2d_polyline(NYA_Window* window, const f32x2* points, u32 count, f32 thickness, NYA_Color color) {
    nya_assert(window != nullptr);

    if (points == nullptr || count < 2) return;
    if (thickness <= 0.0F) return;

    for (u32 i = 0; i + 1 < count; i++) nya_render2d_line(window, points[i], points[i + 1], thickness, color);
}

void nya_render2d_line(NYA_Window* window, f32x2 from, f32x2 to, f32 thickness, NYA_Color color) {
    nya_assert(window != nullptr);

    if (thickness <= 0.0F) return;

    f32x2 along  = to - from;
    f32   length = sqrtf((along[0] * along[0]) + (along[1] * along[1]));

    // A zero length line has no direction to be perpendicular to, so the normal below would be a
    // division by zero and the quad would come out as NaNs.
    if (length <= 0.0F) return;

    // Perpendicular of the unit direction, scaled to half the thickness: the line is a quad
    // straddling the segment rather than sitting to one side of it.
    f32x2 normal = (f32x2){ -along[1] / length, along[0] / length } * (thickness * 0.5F);

    if (!_nya_render2d_prepare(window, NYA_RENDER2D_PIPELINE_SHAPES, nullptr, nullptr, 4, 6)) return;

    NYA_Render2DBatch* batch = &window->render_system.draw_batch;

    f32x2 corners[4] = { from - normal, from + normal, to + normal, to - normal };

    _nya_render2d_quad_corners(batch, corners, 0.0F, 0.0F, 0.0F, 0.0F, color);
}

void nya_render2d_triangle(NYA_Window* window, f32x2 a, f32x2 b, f32x2 c, NYA_Color color) {
    nya_assert(window != nullptr);

    if (!_nya_render2d_prepare(window, NYA_RENDER2D_PIPELINE_SHAPES, nullptr, nullptr, 3, 3)) return;

    NYA_Render2DBatch* batch = &window->render_system.draw_batch;

    u32 base = batch->vertex_count;

    _nya_render2d_vertex(batch, a[0], a[1], 0.0F, 0.0F, color);
    _nya_render2d_vertex(batch, b[0], b[1], 0.0F, 0.0F, color);
    _nya_render2d_vertex(batch, c[0], c[1], 0.0F, 0.0F, color);

    _nya_render2d_triangle_indices(batch, base, 0, 1, 2);
}

void nya_render2d_circle(NYA_Window* window, f32x2 center, f32 radius, NYA_Color color) {
    nya_assert(window != nullptr);

    if (radius <= 0.0F) return;

    /*
     * Segments scale with the radius rather than being fixed.
     *
     * A 3 pixel dot subdivided 64 ways spends 192 vertices on something a hexagon would cover, and
     * the batch is a fixed budget those come out of. Roughly one segment per two pixels of
     * circumference, floored at 8 so the smallest circle is still convincingly round.
     */
    /*
     * The cap scales with the radius rather than being flat.
     *
     * A fixed ceiling of sixty-four is right for a UI dot and visibly polygonal on anything large —
     * a two hundred pixel circle drawn with sixty-four segments has straight edges about ten pixels
     * long. The constant is the ceiling for a *small* circle; a big one is allowed proportionally
     * more, up to a hard limit that keeps one shape from eating the batch.
     */
    u32 ceiling  = (u32)nya_clamp((f32)NYA_RENDER2D_CIRCLE_SEGMENTS * (radius / 64.0F), (f32)NYA_RENDER2D_CIRCLE_SEGMENTS, 512.0F);
    u32 segments = (u32)(radius * 1.5F);
    segments     = nya_clamp(segments, 8U, ceiling);

    // A centre plus one vertex per rim point, rather than three vertices per segment — the whole
    // point of indexing here, since every rim vertex is shared by two triangles and the centre by
    // all of them. Sixty-four segments drops from 192 vertices to 65.
    if (!_nya_render2d_prepare(window, NYA_RENDER2D_PIPELINE_SHAPES, nullptr, nullptr, segments + 1, segments * 3)) return;

    NYA_Render2DBatch* batch = &window->render_system.draw_batch;
    // Tau, i.e. a full turn. M_PI comes from <math.h>, which base_basic.h includes.
    f32            step  = (2.0F * (f32)M_PI) / (f32)segments;

    u32 base = batch->vertex_count;

    _nya_render2d_vertex(batch, center[0], center[1], 0.0F, 0.0F, color);

    for (u32 i = 0; i < segments; i++) {
        f32 angle = step * (f32)i;
        _nya_render2d_vertex(batch, center[0] + (cosf(angle) * radius), center[1] + (sinf(angle) * radius), 0.0F, 0.0F, color);
    }

    // Still a triangle *list*, not a fan primitive: one primitive type means one pipeline for every
    // shape here. The last segment wraps back to the first rim vertex to close the circle.
    for (u32 i = 0; i < segments; i++) {
        u32 next = (i + 1) % segments;
        _nya_render2d_triangle_indices(batch, base, 0, 1 + i, 1 + next);
    }
}

/*
 * ─────────────────────────────────────────────────────────
 * CAMERA
 * ─────────────────────────────────────────────────────────
 */

void nya_render2d_camera_set(NYA_Window* window, NYA_Camera2DTopDown camera) {
    nya_assert(window != nullptr);

    NYA_Render2DBatch* batch = &window->render_system.draw_batch;

    // A zoom of zero collapses the world to a point and divides by zero on the way back out, so it
    // is corrected rather than propagated. A caller wanting nothing drawn should not draw.
    if (camera.zoom <= 0.0F) camera.zoom = 1.0F;

    // Flushed before the change, so what was queued is drawn through the camera it was queued under.
    // A boundary, not a draw: the clip rectangle and the camera are both recorded per range, so
    // what is already queued keeps the state it was queued under and can still be reordered by
    // layer. Flushing here instead would make every scissored panel a barrier to layering.
    _nya_render2d_range_close(window);

    batch->camera = (NYA_Camera2D){ .kind = NYA_CAMERA2D_KIND_TOP_DOWN, .as_top_down = camera };
}

void nya_render2d_camera_isometric_set(NYA_Window* window, NYA_Camera2DIsometric camera) {
    nya_assert(window != nullptr);

    NYA_Render2DBatch* batch = &window->render_system.draw_batch;

    if (camera.zoom <= 0.0F) camera.zoom = 1.0F;

    // A tile with no size projects every tile coordinate onto the same point, which is the isometric
    // spelling of a zero zoom and is corrected the same way. 64x32 is the classic 2:1 diamond.
    if (camera.tile_width <= 0.0F) camera.tile_width = 64.0F;
    if (camera.tile_height <= 0.0F) camera.tile_height = 32.0F;

    _nya_render2d_flush_for(window, NYA_RENDER2D_FLUSH_STATE);

    batch->camera = (NYA_Camera2D){ .kind = NYA_CAMERA2D_KIND_ISOMETRIC, .as_isometric = camera };
}

void nya_render2d_camera_reset(NYA_Window* window) {
    nya_assert(window != nullptr);

    NYA_Render2DBatch* batch = &window->render_system.draw_batch;

    _nya_render2d_flush_for(window, NYA_RENDER2D_FLUSH_STATE);

    batch->camera = (NYA_Camera2D){ .kind = NYA_CAMERA2D_KIND_NONE };
}

NYA_Camera2D nya_render2d_camera_get(NYA_Window* window) {
    nya_assert(window != nullptr);

    return window->render_system.draw_batch.camera;
}

NYA_Camera2DTopDown nya_render2d_camera_top_down_get(NYA_Window* window) {
    nya_assert(window != nullptr);

    NYA_Camera2D camera = window->render_system.draw_batch.camera;

    // The identity, spelled out, rather than the zeroed struct the batch holds when no camera is set
    // — a zoom of zero would be a surprising thing to hand back and then pass straight back in. An
    // isometric camera answers the identity too: it is not a top-down camera and there is no
    // top-down camera that means the same thing, so the honest answer is "none".
    if (camera.kind != NYA_CAMERA2D_KIND_TOP_DOWN) return (NYA_Camera2DTopDown){ .zoom = 1.0F };

    return camera.as_top_down;
}

f32x2 nya_render2d_screen_to_world(NYA_Window* window, f32x2 screen) {
    nya_assert(window != nullptr);

    NYA_Render2DBatch* batch = &window->render_system.draw_batch;
    if (batch->camera.kind == NYA_CAMERA2D_KIND_NONE) return screen;

    f32 a, b, c, d;
    _nya_render2d_camera_basis(&batch->camera, &a, &b, &c, &d);

    f32x2 position = _nya_render2d_camera_position(&batch->camera);

    f32 center_x = (f32)batch->target_width * 0.5F;
    f32 center_y = (f32)batch->target_height * 0.5F;

    /*
     * Inverting the 2x2 the view matrix applies, written out.
     *
     * A general matrix inverse for four numbers would be more code and more to get wrong, and the
     * determinant is the one thing worth naming: for a top-down camera it is the zoom squared, and
     * for an isometric one it is half the tile area times the zoom squared. Neither can be zero,
     * because both setters correct a zero zoom and a zero tile size before they store anything.
     */
    f32 determinant = (a * d) - (b * c);

    f32 dx = screen[0] - center_x;
    f32 dy = screen[1] - center_y;

    return (f32x2){
        (((d * dx) - (b * dy)) / determinant) + position[0],
        ((((-c) * dx) + (a * dy)) / determinant) + position[1],
    };
}

f32x2 nya_render2d_world_to_screen(NYA_Window* window, f32x2 world) {
    nya_assert(window != nullptr);

    NYA_Render2DBatch* batch = &window->render_system.draw_batch;
    if (batch->camera.kind == NYA_CAMERA2D_KIND_NONE) return world;

    f32 a, b, c, d;
    _nya_render2d_camera_basis(&batch->camera, &a, &b, &c, &d);

    f32x2 position = _nya_render2d_camera_position(&batch->camera);

    f32 center_x = (f32)batch->target_width * 0.5F;
    f32 center_y = (f32)batch->target_height * 0.5F;

    f32 dx = world[0] - position[0];
    f32 dy = world[1] - position[1];

    return (f32x2){ (a * dx) + (b * dy) + center_x, (c * dx) + (d * dy) + center_y };
}

/*
 * ─────────────────────────────────────────────────────────
 * TEXTURES
 * ─────────────────────────────────────────────────────────
 */


void nya_render2d_texture(NYA_Window* window, NYA_ConstCString texture_handle, f32 x, f32 y, NYA_Color tint) {
    // Cast because nya_asset_get takes a mutable handle while only reading it; every caller here
    // passes a literal.
    NYA_Asset* asset = nya_asset_get((NYA_CString)texture_handle);
    if (asset == nullptr || asset->status != NYA_ASSET_STATUS_LOADED) return;

    f32 width  = (f32)asset->as_texture.width;
    f32 height = (f32)asset->as_texture.height;

    nya_render2d_texture_rect(window, texture_handle, 0.0F, 0.0F, width, height, x, y, width, height, tint);
}

void nya_render2d_texture_ex(NYA_Window* window, NYA_ConstCString texture_handle, NYA_Render2DTexture params) {
    nya_assert(window != nullptr);

    NYA_Asset* asset = nya_asset_get((NYA_CString)texture_handle);
    if (asset == nullptr || asset->status != NYA_ASSET_STATUS_LOADED || asset->as_texture.texture == nullptr) {
        window->render_system.draw_batch.frame_dropped_draws++;
        return;
    }

    f32 texture_width  = (f32)asset->as_texture.width;
    f32 texture_height = (f32)asset->as_texture.height;
    if (texture_width <= 0.0F || texture_height <= 0.0F) return;

    // Zero means "not specified" throughout, which is what makes a partially filled struct a complete
    // call. See NYA_Render2DTexture.
    f32 source_width  = params.source_width > 0.0F ? params.source_width : texture_width;
    f32 source_height = params.source_height > 0.0F ? params.source_height : texture_height;

    f32 width  = params.width > 0.0F ? params.width : source_width;
    f32 height = params.height > 0.0F ? params.height : source_height;

    NYA_Color tint = params.tint;
    if (tint.r == 0.0F && tint.g == 0.0F && tint.b == 0.0F && tint.a == 0.0F) tint = (NYA_Color){ 1.0F, 1.0F, 1.0F, 1.0F };

    if (!_nya_render2d_prepare(window, NYA_RENDER2D_PIPELINE_TEXTURED, asset->as_texture.texture, _nya_render_sampler_for(asset->as_texture.filter), 4, 6)) return;

    f32 u0 = params.source_x / texture_width;
    f32 v0 = params.source_y / texture_height;
    f32 u1 = (params.source_x + source_width) / texture_width;
    f32 v1 = (params.source_y + source_height) / texture_height;

    // Mirroring is a uv swap, not a negative size. See NYA_Render2DTexture.flip_x.
    if (params.flip_x) { f32 swap = u0; u0 = u1; u1 = swap; }
    if (params.flip_y) { f32 swap = v0; v0 = v1; v1 = swap; }

    /*
     * Corners in the sprite's own space first, measured from the pivot, then rotated, then moved to
     * where the pivot goes. Doing it in that order is what makes `rotation` a rotation about `origin`
     * rather than about the target's origin.
     */
    f32 left   = -params.origin[0];
    f32 top    = -params.origin[1];
    f32 right  = left + width;
    f32 bottom = top + height;

    f32x2 corners[4] = {
        { left, top },
        { right, top },
        { right, bottom },
        { left, bottom },
    };

    if (params.rotation != 0.0F) {
        f32 c = cosf(params.rotation);
        f32 s = sinf(params.rotation);

        for (u32 i = 0; i < 4; i++) {
            f32 cx = corners[i][0];
            f32 cy = corners[i][1];

            // Clockwise on screen, because y grows downward here — the same matrix that reads as
            // counter clockwise in a y-up convention.
            corners[i] = (f32x2){ (cx * c) - (cy * s), (cx * s) + (cy * c) };
        }
    }

    for (u32 i = 0; i < 4; i++) corners[i] += (f32x2){ params.x, params.y };

    _nya_render2d_quad_corners(&window->render_system.draw_batch, corners, u0, v0, u1, v1, tint);
}

void nya_render2d_texture_rect(
    NYA_Window*      window,
    NYA_ConstCString texture_handle,
    f32              source_x,
    f32              source_y,
    f32              source_width,
    f32              source_height,
    f32              destination_x,
    f32              destination_y,
    f32              destination_width,
    f32              destination_height,
    NYA_Color        tint
) {
    nya_assert(window != nullptr);

    // Missing or still loading. Assets load asynchronously, so this is the normal state for the
    // first frames after a load rather than an error worth reporting every frame.
    // Cast because nya_asset_get takes a mutable handle while only reading it; every caller here
    // passes a literal.
    NYA_Asset* asset = nya_asset_get((NYA_CString)texture_handle);
    if (asset == nullptr || asset->status != NYA_ASSET_STATUS_LOADED || asset->as_texture.texture == nullptr) {
        window->render_system.draw_batch.frame_dropped_draws++;
        return;
    }

    f32 texture_width  = (f32)asset->as_texture.width;
    f32 texture_height = (f32)asset->as_texture.height;
    if (texture_width <= 0.0F || texture_height <= 0.0F) return;

    if (!_nya_render2d_prepare(window, NYA_RENDER2D_PIPELINE_TEXTURED, asset->as_texture.texture, _nya_render_sampler_for(asset->as_texture.filter), 4, 6)) return;

    // Source pixels to normalized uv, which is what the sampler wants.
    f32 u0 = source_x / texture_width;
    f32 v0 = source_y / texture_height;
    f32 u1 = (source_x + source_width) / texture_width;
    f32 v1 = (source_y + source_height) / texture_height;

    _nya_render2d_quad(&window->render_system.draw_batch, destination_x, destination_y, destination_width, destination_height, u0, v0, u1, v1, tint);
}

/*
 * ─────────────────────────────────────────────────────────
 * TEXT
 * ─────────────────────────────────────────────────────────
 */

void nya_render2d_font_set(NYA_ConstCString font_path, f32 point_size) {
    if (point_size <= 0.0F) point_size = NYA_RENDER2D_FONT_DEFAULT_SIZE;
    if (font_path == _nya_render2d_current_font && point_size == _nya_render2d_current_font_size) return;

    _nya_render2d_current_font      = font_path;
    _nya_render2d_current_font_size = point_size;

    // Dropped rather than resolved here: the atlas may not be buildable yet, and every consumer
    // already builds on demand. This only has to stop the old one being handed out.
    _nya_render2d_current_atlas = nullptr;
}

NYA_ConstCString nya_render2d_font_get(void) {
    return _nya_render2d_current_font;
}

f32 nya_render2d_font_size_get(void) {
    return _nya_render2d_current_font_size;
}

void nya_render2d_nine_slice(NYA_Window* window, NYA_ConstCString texture_handle, NYA_NineSlice params) {
    nya_assert(window != nullptr);

    NYA_Asset* asset = nya_asset_get((NYA_CString)texture_handle);

    // Missing or still loading, which is the normal state for the first frames after a load.
    if (asset == nullptr || asset->status != NYA_ASSET_STATUS_LOADED || asset->type != NYA_ASSET_TYPE_TEXTURE) return;

    f32 texture_width  = (f32)asset->as_texture.width;
    f32 texture_height = (f32)asset->as_texture.height;

    if (texture_width <= 0.0F || texture_height <= 0.0F) return;
    if (params.width <= 0.0F || params.height <= 0.0F) return;

    /*
     * An all-zero tint means white, and this has to be done *here*.
     *
     * nya_render2d_texture_ex makes that substitution and nya_render2d_texture_rect does not — it passes
     * the colour straight to the quad. This routes through the second one, so a caller who left the tint
     * alone got an alpha of zero and an invisible panel, while the field's own documentation promised
     * white. The default use of the call was the broken one.
     */
    NYA_Color tint = params.tint;

    if (tint.r == 0.0F && tint.g == 0.0F && tint.b == 0.0F && tint.a == 0.0F) tint = (NYA_Color){ 1.0F, 1.0F, 1.0F, 1.0F };

    f32 left   = nya_max(params.left, 0.0F);
    f32 right  = nya_max(params.right, 0.0F);
    f32 top    = nya_max(params.top, 0.0F);
    f32 bottom = nya_max(params.bottom, 0.0F);

    /*
     * Borders scaled down when the destination is smaller than they are.
     *
     * Without this a panel narrower than its own two corners draws them overlapping and gives the edge
     * patch a negative width, which comes out as an inside-out quad rather than as nothing. A panel
     * animating open from zero passes through exactly that state on its way, so it is the common case
     * and not the pathological one.
     *
     * Scaled proportionally, so a lopsided border stays lopsided as it shrinks.
     */
    f32 horizontal = left + right;
    f32 vertical   = top + bottom;

    if (horizontal > params.width && horizontal > 0.0F) {
        f32 shrink  = params.width / horizontal;
        left       *= shrink;
        right      *= shrink;
    }

    if (vertical > params.height && vertical > 0.0F) {
        f32 shrink  = params.height / vertical;
        top        *= shrink;
        bottom     *= shrink;
    }

    /*
     * The grid, as three source spans and three destination spans per axis.
     *
     * The source borders are unscaled — they name the authored image — while the destination ones are
     * the possibly shrunk values above. That difference is the entire point of a nine-slice: the corners
     * are drawn at whatever size the destination gives them and the *middle* absorbs the rest.
     */
    f32 source_x[4]   = { 0.0F, params.left, texture_width - params.right, texture_width };
    f32 source_y[4]   = { 0.0F, params.top, texture_height - params.bottom, texture_height };
    f32 destination_x[4] = { params.x, params.x + left, params.x + params.width - right, params.x + params.width };
    f32 destination_y[4] = { params.y, params.y + top, params.y + params.height - bottom, params.y + params.height };

    for (u32 row = 0; row < 3; row++) {
        for (u32 column = 0; column < 3; column++) {
            // The centre patch, skipped for a frame. See NYA_NineSlice.hollow.
            if (params.hollow && row == 1 && column == 1) continue;

            f32 source_width       = source_x[column + 1] - source_x[column];
            f32 source_height      = source_y[row + 1] - source_y[row];
            f32 destination_width  = destination_x[column + 1] - destination_x[column];
            f32 destination_height = destination_y[row + 1] - destination_y[row];

            // A zero border collapses its row or column to nothing, which is a valid three-slice rather
            // than an error — see NYA_NineSlice. Skipped so it costs no vertices.
            if (source_width <= 0.0F || source_height <= 0.0F) continue;
            if (destination_width <= 0.0F || destination_height <= 0.0F) continue;

            nya_render2d_texture_rect(
                window,
                texture_handle,
                source_x[column],
                source_y[row],
                source_width,
                source_height,
                destination_x[column],
                destination_y[row],
                destination_width,
                destination_height,
                tint
            );
        }
    }
}

void nya_render2d_text(NYA_Window* window, NYA_ConstCString text, f32 x, f32 y, NYA_Color color) {
    nya_render2d_text_with_font(window, _nya_render2d_current_font, _nya_render2d_current_font_size, text, x, y, color);
}

void nya_render2d_textf(NYA_Window* window, f32 x, f32 y, NYA_Color color, NYA_ConstCString format, ...) {
    va_list arguments;
    va_start(arguments, format);
    _nya_render2d_textf_va(window, _nya_render2d_current_font, _nya_render2d_current_font_size, x, y, color, format, arguments);
    va_end(arguments);
}

void nya_render2d_textf_with_font(NYA_Window* window, NYA_ConstCString font_path, f32 point_size, f32 x, f32 y, NYA_Color color, NYA_ConstCString format, ...) {
    va_list arguments;
    va_start(arguments, format);
    _nya_render2d_textf_va(window, font_path, point_size, x, y, color, format, arguments);
    va_end(arguments);
}

void nya_render2d_text_with_font(NYA_Window* window, NYA_ConstCString font_path, f32 point_size, NYA_ConstCString text, f32 x, f32 y, NYA_Color color) {
    nya_assert(window != nullptr);

    if (text == nullptr || text[0] == '\0') return;

    NYA_FontAtlas* atlas = _nya_render2d_font_atlas(window, font_path, point_size);
    if (atlas == nullptr) return;

    f32 pen_x = x;
    f32 pen_y = y;

    // Taken once for the whole string. Kerning needs the face, and resolving it per glyph would be an
    // asset lookup per character.
    TTF_Font* face = _nya_render2d_atlas_font(atlas);

    u32 previous = 0;

    /*
     * Two passes over the string: resolve every glyph, then emit the quads.
     *
     * Resolving may rasterise a glyph nobody has drawn before, which dirties the atlas surface — and
     * the texture has to carry that *before* any quad referencing it is queued. Emitting as we
     * resolve would put a quad in the batch pointing at a cell the GPU has not received yet, which
     * draws as a blank rectangle for exactly one frame and is maddening to reproduce.
     *
     * Decoding twice costs nothing worth measuring: these are strings, not files.
     */
    for (NYA_ConstCString cursor = text; *cursor != '\0';) {
        u32 codepoint = 0;
        cursor       += nya_utf8_next(cursor, &codepoint);

        if (codepoint == '\n') continue;

        (void)_nya_render2d_glyph(atlas, codepoint);
    }

    _nya_render2d_atlas_upload(window, atlas);

    for (NYA_ConstCString cursor = text; *cursor != '\0';) {
        u32 codepoint = 0;
        cursor       += nya_utf8_next(cursor, &codepoint);

        if (codepoint == '\n') {
            pen_x    = x;
            pen_y   += atlas->line_height;
            previous = 0;
            continue;
        }

        // Before the glyph is placed, because kerning moves *this* glyph relative to the one before it.
        pen_x += _nya_render2d_kerning(face, previous, codepoint);

        previous = codepoint;

        const NYA_Glyph* glyph = _nya_render2d_glyph(atlas, codepoint);

        // Not in the atlas and not bakeable — a full atlas, or a face with no such glyph. Advanced by
        // a space rather than skipped, so unsupported text is visibly wrong instead of silently
        // shortening the line.
        if (glyph == nullptr) {
            pen_x += atlas->glyphs[0].advance;
            continue;
        }

        // A space has an advance and no pixels. Skipping the quad rather than queueing an empty one
        // keeps six vertices per space out of the batch.
        if (glyph->width > 0.0F && glyph->height > 0.0F) {
            /*
             * Nearest, and the glyphs are rasterised without anti-aliasing to match.
             *
             * Linear here was blurring text that is drawn at exactly one texel per pixel: a glyph
             * landing on a fractional coordinate gets sampled between texels, and every stem picks
             * up a soft edge on both sides. Point sampling a hard-edged glyph keeps the stems the
             * width the rasteriser made them.
             */
            if (!_nya_render2d_prepare(window, NYA_RENDER2D_PIPELINE_TEXTURED, atlas->texture, _nya_render_sampler_for(NYA_TEXTURE_FILTER_NEAREST), 4, 6)) return;

            /*
             * Snapped to whole pixels.
             *
             * Point sampling only stays crisp if the quad lands on the pixel grid — a glyph at x =
             * 12.4 samples the texel boundary and drops or doubles a column, which is exactly the
             * shimmer that makes small text look broken while the caller is animating anything.
             * Rounding the destination, not the advance, so the run's total width is unchanged.
             */
            _nya_render2d_quad(
                &window->render_system.draw_batch,
                roundf(pen_x + glyph->bearing_x),
                roundf(pen_y + glyph->bearing_y),
                glyph->width,
                glyph->height,
                glyph->u0,
                glyph->v0,
                glyph->u1,
                glyph->v1,
                color
            );
        }

        pen_x += glyph->advance;
    }
}

f32x2 nya_render2d_text_box(NYA_Window* window, NYA_ConstCString text, NYA_Render2DTextBox params) {
    nya_assert(window != nullptr);

    return _nya_render2d_text_box_layout(window, text, params);
}

f32x2 nya_render2d_text_box_measure(NYA_ConstCString text, NYA_Render2DTextBox params) {
    // Null window means "lay out but do not emit". One function for both, because a measure that took a
    // different path from the draw would be a measure that eventually disagreed with it.
    return _nya_render2d_text_box_layout(nullptr, text, params);
}

f32x2 nya_render2d_text_measure(NYA_ConstCString text) {
    return nya_render2d_text_measure_with_font(_nya_render2d_current_font, _nya_render2d_current_font_size, text);
}

f32x2 nya_render2d_text_measure_with_font(NYA_ConstCString font_path, f32 point_size, NYA_ConstCString text) {
    if (text == nullptr) return f32x2_zero;

    /*
     * Builds the atlas if it is not there yet, with a null window.
     *
     * Measuring has to work before anything has been drawn — that is the whole point of it, since a
     * layout pass runs first. The builder takes a null window to mean "there is no render pass to
     * borrow", and uploads on its own command buffer instead.
     */
    NYA_FontAtlas* atlas = _nya_render2d_font_atlas(nullptr, font_path, point_size);
    if (atlas == nullptr) return f32x2_zero;

    // An empty string still occupies a line, which is what a caller stacking rows expects. Its width
    // is zero.
    if (text[0] == '\0') return (f32x2){ 0.0F, atlas->line_height };

    f32 widest  = 0.0F;
    f32 current = 0.0F;
    f32 height  = atlas->line_height;

    TTF_Font* face = _nya_render2d_atlas_font(atlas);

    u32 previous = 0;

    for (NYA_ConstCString cursor = text; *cursor != '\0';) {
        u32 codepoint = 0;
        cursor       += nya_utf8_next(cursor, &codepoint);

        if (codepoint == '\n') {
            widest   = nya_max(widest, current);
            current  = 0.0F;
            height  += atlas->line_height;
            previous = 0;
            continue;
        }

        // The same adjustment the draw applies, so a measured width is the width that gets drawn.
        current += _nya_render2d_kerning(face, previous, codepoint);

        previous = codepoint;

        /*
         * Resolving here bakes the glyph if it is new, which is what makes measuring a string agree
         * with drawing it.
         *
         * A measure that reported a space's width for every accented character would lay out a
         * translated menu against widths that are wrong by a third — and the atlas has to be built
         * eventually anyway, so doing it at layout time is doing it once rather than twice.
         *
         * No upload: measuring needs the advance, which is metrics, not pixels. The next draw's
         * upload carries whatever this baked.
         */
        const NYA_Glyph* glyph = _nya_render2d_glyph(atlas, codepoint);

        current += glyph != nullptr ? glyph->advance : atlas->glyphs[0].advance;
    }

    return (f32x2){ nya_max(widest, current), height };
}

f32 nya_render2d_text_width(NYA_ConstCString text) {
    return nya_render2d_text_measure(text)[0];
}

f32 nya_render2d_text_height(NYA_ConstCString text) {
    return nya_render2d_text_measure(text)[1];
}

f32 nya_render2d_font_line_height(void) {
    NYA_FontAtlas* atlas = _nya_render2d_current_atlas != nullptr ? _nya_render2d_current_atlas : _nya_render2d_font_atlas(nullptr, _nya_render2d_current_font, _nya_render2d_current_font_size);
    return atlas != nullptr ? atlas->line_height : 0.0F;
}

f32 nya_render2d_font_ascent(void) {
    NYA_FontAtlas* atlas = _nya_render2d_current_atlas != nullptr ? _nya_render2d_current_atlas : _nya_render2d_font_atlas(nullptr, _nya_render2d_current_font, _nya_render2d_current_font_size);
    return atlas != nullptr ? atlas->ascent : 0.0F;
}

f32 nya_render2d_font_descent(void) {
    NYA_FontAtlas* atlas = _nya_render2d_current_atlas != nullptr ? _nya_render2d_current_atlas : _nya_render2d_font_atlas(nullptr, _nya_render2d_current_font, _nya_render2d_current_font_size);
    return atlas != nullptr ? atlas->descent : 0.0F;
}

f32 nya_render2d_font_height(void) {
    NYA_FontAtlas* atlas = _nya_render2d_current_atlas != nullptr ? _nya_render2d_current_atlas : _nya_render2d_font_atlas(nullptr, _nya_render2d_current_font, _nya_render2d_current_font_size);
    return atlas != nullptr ? atlas->ascent + atlas->descent : 0.0F;
}

/*
 * ─────────────────────────────────────────────────────────
 * SHADERS
 * ─────────────────────────────────────────────────────────
 */

void nya_render2d_shader_begin(NYA_Window* window, NYA_ConstCString pipeline_handle) {
    nya_assert(window != nullptr);

    // Flushed first, so shapes queued before this are drawn with the pipeline they were queued for
    // rather than being retroactively shaded by whatever comes next.
    _nya_render2d_flush_for(window, NYA_RENDER2D_FLUSH_PIPELINE);

    window->render_system.draw_batch.shader_override = (NYA_CString)pipeline_handle;
}

void nya_render2d_shader_set_uniform(NYA_Window* window, const void* data, u32 size) {
    nya_assert(window != nullptr);
    nya_assert(data != nullptr || size == 0);
    nya_assert(size <= NYA_RENDER2D_MAX_UNIFORM_BYTES, "%u uniform bytes, past NYA_RENDER2D_MAX_UNIFORM_BYTES (%d)", size, NYA_RENDER2D_MAX_UNIFORM_BYTES);

    NYA_Render2DBatch* batch = &window->render_system.draw_batch;

    // A uniform is per draw call, so anything already queued was queued under the previous value and
    // has to go out before this one replaces it.
    _nya_render2d_flush_for(window, NYA_RENDER2D_FLUSH_STATE);

    // Copied, not referenced: the caller's struct is usually a compound literal that stops existing
    // at the end of the statement, and the push does not happen until the flush.
    if (size > 0) nya_memcpy(batch->shader_uniform, data, size);
    batch->shader_uniform_size = size;
}

void nya_render2d_shader_end(NYA_Window* window) {
    nya_assert(window != nullptr);

    _nya_render2d_flush_for(window, NYA_RENDER2D_FLUSH_PIPELINE);

    window->render_system.draw_batch.shader_override    = nullptr;
    // Cleared with the shader, so the next custom pipeline cannot silently inherit these.
    window->render_system.draw_batch.shader_uniform_size = 0;
}

/*
 * ─────────────────────────────────────────────────────────
 * SCISSOR
 * ─────────────────────────────────────────────────────────
 */

void nya_render2d_procedural(NYA_Window* window, NYA_ConstCString pipeline_handle, u32 vertex_count, const void* uniform_data, u32 uniform_size) {
    nya_assert(window != nullptr);

    NYA_RenderSystemWindow* render = &window->render_system;
    NYA_Render2DBatch*          batch  = &render->draw_batch;

    if (pipeline_handle == nullptr || vertex_count == 0) return;

    NYA_Asset* asset = nya_asset_get((NYA_CString)pipeline_handle);
    if (asset == nullptr || asset->status != NYA_ASSET_STATUS_LOADED || asset->as_graphics_pipeline.pipeline == nullptr) {
        batch->frame_dropped_draws++;
        return;
    }

    // Whatever is queued was queued for a different pipeline and has to go out first, or it would be
    // drawn with this one.
    _nya_render2d_flush_for(window, NYA_RENDER2D_FLUSH_PIPELINE);

    if (render->render_pass == nullptr || render->render_commands == nullptr) {
        batch->frame_dropped_draws++;
        return;
    }

    SDL_BindGPUGraphicsPipeline(render->render_pass, asset->as_graphics_pipeline.pipeline);

    if (uniform_data != nullptr && uniform_size > 0) {
        SDL_PushGPUVertexUniformData(render->render_commands, 0, uniform_data, uniform_size);
        SDL_PushGPUFragmentUniformData(render->render_commands, 0, uniform_data, uniform_size);
    }

    SDL_DrawGPUPrimitives(render->render_pass, (Uint32)vertex_count, 1, 0, 0);

    batch->frame_flushes++;
    batch->frame_flush_reasons[NYA_RENDER2D_FLUSH_PIPELINE]++;

    /*
     * The batch's cached pipeline is cleared, not left pointing at what it was.
     *
     * The batch skips rebinding when the pipeline it wants is the one it believes is bound. This
     * draw bound a different one behind its back, so without clearing the cache the next shape would
     * be drawn through this pipeline — which is the exact failure the raw SDL calls in a layer used
     * to produce, only later and harder to attribute.
     */
    batch->pipeline = nullptr;
    batch->texture  = nullptr;
    batch->sampler  = nullptr;
}

void nya_render2d_scissor_begin(NYA_Window* window, f32 x, f32 y, f32 width, f32 height) {
    nya_assert(window != nullptr);

    NYA_Render2DBatch* batch = &window->render_system.draw_batch;

    // Flushed first: what was queued was queued unclipped, and clipping is pass state that would
    // otherwise apply to it retroactively.
    // A boundary, not a draw: the clip rectangle and the camera are both recorded per range, so
    // what is already queued keeps the state it was queued under and can still be reordered by
    // layer. Flushing here instead would make every scissored panel a barrier to layering.
    _nya_render2d_range_close(window);

    /*
     * Clamped to the target, and to a non-negative size.
     *
     * SDL_GPU rejects a scissor that leaves the target, so a panel scrolled half off the left edge —
     * which is ordinary in a UI — would otherwise be a hard failure rather than a smaller clip.
     */
    f32 left   = nya_max(0.0F, x);
    f32 top    = nya_max(0.0F, y);
    f32 right  = nya_min((f32)batch->target_width, x + width);
    f32 bottom = nya_min((f32)batch->target_height, y + height);

    batch->scissor_x      = (s32)left;
    batch->scissor_y      = (s32)top;
    batch->scissor_width  = (s32)nya_max(0.0F, right - left);
    batch->scissor_height = (s32)nya_max(0.0F, bottom - top);
    batch->scissor_active = true;

    _nya_render2d_apply_scissor(window);
}

void nya_render2d_scissor_end(NYA_Window* window) {
    nya_assert(window != nullptr);

    NYA_Render2DBatch* batch = &window->render_system.draw_batch;

    // A boundary, not a draw: the clip rectangle and the camera are both recorded per range, so
    // what is already queued keeps the state it was queued under and can still be reordered by
    // layer. Flushing here instead would make every scissored panel a barrier to layering.
    _nya_render2d_range_close(window);

    batch->scissor_active = false;

    _nya_render2d_apply_scissor(window);
}

/*
 * ─────────────────────────────────────────────────────────
 * RENDER TEXTURES
 * ─────────────────────────────────────────────────────────
 */

NYA_RenderTexture nya_render_texture_create(NYA_Window* window, u32 width, u32 height) {
    nya_assert(window != nullptr);
    nya_assert(width > 0 && height > 0, "a render texture needs a non-zero size");

    SDL_GPUDevice* gpu_device = nya_app_get()->render_system.gpu_device;

    // The swapchain's own format, so the pipelines built for the window also draw into this. See
    // NYA_RenderTexture.
    SDL_GPUTextureFormat format = SDL_GetGPUSwapchainTextureFormat(gpu_device, window->sdl_window);

    SDL_GPUTexture* texture = SDL_CreateGPUTexture(
        gpu_device,
        &(SDL_GPUTextureCreateInfo){
            .type                 = SDL_GPU_TEXTURETYPE_2D,
            .format               = format,
            // Both, and that is the whole point of the type: COLOR_TARGET to be drawn into, SAMPLER
            // to be drawn with afterwards.
            .usage                = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER,
            .width                = width,
            .height               = height,
            .layer_count_or_depth = 1,
            .num_levels           = 1,
        }
    );
    nya_assert(texture != nullptr, "SDL_CreateGPUTexture() failed for a render texture: %s", SDL_GetError());

    /*
     * A multisampled companion, for the same reason the window has one: the pipelines are built for
     * the renderer's sample count and cannot draw into a single-sampled target.
     *
     * Drawing goes into this one and resolves onto the sampled texture above as the pass ends, so
     * nya_render2d_render_texture still reads the resolved image and nothing else has to know.
     */
    SDL_GPUTexture* msaa_texture = nullptr;

    if (nya_app_get()->render_system.sample_count != SDL_GPU_SAMPLECOUNT_1) {
        msaa_texture = SDL_CreateGPUTexture(
            gpu_device,
            &(SDL_GPUTextureCreateInfo){
                .type                 = SDL_GPU_TEXTURETYPE_2D,
                .format               = format,
                .usage                = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET,
                .width                = width,
                .height               = height,
                .layer_count_or_depth = 1,
                .num_levels           = 1,
                .sample_count         = nya_app_get()->render_system.sample_count,
            }
        );
        nya_assert(msaa_texture != nullptr, "SDL_CreateGPUTexture() failed for a render texture's MSAA buffer: %s", SDL_GetError());
    }

    // Same format and same sample count as the window's, because the pipelines that draw here are
    // the very same objects and both are baked into them.
    SDL_GPUTexture* depth_texture = SDL_CreateGPUTexture(
        gpu_device,
        &(SDL_GPUTextureCreateInfo){
            .type                 = SDL_GPU_TEXTURETYPE_2D,
            .format               = nya_app_get()->render_system.depth_format,
            .usage                = SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET,
            .width                = width,
            .height               = height,
            .layer_count_or_depth = 1,
            .num_levels           = 1,
            .sample_count         = nya_app_get()->render_system.sample_count,
        }
    );
    nya_assert(depth_texture != nullptr, "SDL_CreateGPUTexture() failed for a render texture's depth buffer: %s", SDL_GetError());

    return (NYA_RenderTexture){
        .texture       = texture,
        .msaa_texture  = msaa_texture,
        .depth_texture = depth_texture,
        .width         = width,
        .height        = height,
    };
}

void nya_render_texture_destroy(NYA_RenderTexture* render_texture) {
    if (render_texture == nullptr) return;
    if (render_texture->texture == nullptr) return;

    SDL_GPUDevice* gpu_device = nya_app_get()->render_system.gpu_device;

    /*
     * No wait. SDL_ReleaseGPUTexture frees "as soon as it is safe to do so", so it already defers
     * past any frame still reading the texture — the SDL_WaitForGPUIdle that used to be here was a
     * full pipeline stall buying nothing, which a game resizing a render texture would pay every
     * time it did so.
     */
    SDL_ReleaseGPUTexture(gpu_device, render_texture->texture);
    if (render_texture->msaa_texture != nullptr) SDL_ReleaseGPUTexture(gpu_device, render_texture->msaa_texture);
    if (render_texture->depth_texture != nullptr) SDL_ReleaseGPUTexture(gpu_device, render_texture->depth_texture);

    *render_texture = (NYA_RenderTexture){ 0 };
}

void nya_render_texture_begin(NYA_Window* window, NYA_RenderTexture* render_texture, NYA_Color clear) {
    nya_assert(window != nullptr);
    nya_assert(render_texture != nullptr);
    nya_assert(render_texture->texture != nullptr, "the render texture was destroyed, or never created");

    NYA_RenderSystemWindow* render = &window->render_system;
    NYA_Render2DBatch*          batch  = &render->draw_batch;

    nya_assert(!batch->target_is_texture, "nya_render_texture_begin does not nest; end the current target first");

    if (render->render_pass == nullptr) return;

    // Everything queued belongs to the previous target, and its projection. Drawn before the switch,
    // or it would come out at the new target's scale.
    _nya_render2d_flush_for(window, NYA_RENDER2D_FLUSH_STATE);

    SDL_EndGPURenderPass(render->render_pass);

    render->render_pass = SDL_BeginGPURenderPass(
        render->render_commands,
        &(SDL_GPUColorTargetInfo){
            .texture         = render_texture->msaa_texture != nullptr ? render_texture->msaa_texture : render_texture->texture,
            .resolve_texture = render_texture->msaa_texture != nullptr ? render_texture->texture : nullptr,
            .clear_color     = (SDL_FColor){ .r = clear.r, .g = clear.g, .b = clear.b, .a = clear.a },
            // CLEAR, unlike the reopen after a flush: this is the start of drawing into this target,
            // and a render texture holds whatever was left in it from the last frame otherwise.
            .load_op         = SDL_GPU_LOADOP_CLEAR,
            .store_op        = render_texture->msaa_texture != nullptr ? SDL_GPU_STOREOP_RESOLVE_AND_STORE : SDL_GPU_STOREOP_STORE,
        },
        1,
        // Cleared to the far plane along with the colour, because this is the start of drawing into
        // this target and last frame's depth would occlude this frame's geometry.
        &(SDL_GPUDepthStencilTargetInfo){
            .texture          = render_texture->depth_texture,
            .clear_depth      = 1.0F,
            .load_op          = SDL_GPU_LOADOP_CLEAR,
            .store_op         = SDL_GPU_STOREOP_STORE,
            .stencil_load_op  = SDL_GPU_LOADOP_DONT_CARE,
            .stencil_store_op = SDL_GPU_STOREOP_DONT_CARE,
        }
    );
    nya_assert(render->render_pass != nullptr, "SDL_BeginGPURenderPass() failed for a render texture: %s", SDL_GetError());

    batch->target_texture    = render_texture->texture;
    batch->target_msaa       = render_texture->msaa_texture;
    batch->target_depth      = render_texture->depth_texture;
    batch->target_width      = render_texture->width;
    batch->target_height     = render_texture->height;
    batch->target_is_texture = true;
}

void nya_render_texture_end(NYA_Window* window) {
    nya_assert(window != nullptr);

    NYA_RenderSystemWindow* render = &window->render_system;
    NYA_Render2DBatch*          batch  = &render->draw_batch;

    if (!batch->target_is_texture) return;
    if (render->render_pass == nullptr) return;

    // Drawn while the texture is still the target; after the switch these vertices would land on the
    // window at the texture's coordinates.
    _nya_render2d_flush_for(window, NYA_RENDER2D_FLUSH_STATE);

    SDL_EndGPURenderPass(render->render_pass);
    render->render_pass = nullptr;

    batch->target_texture    = render->swapchain_texture;
    batch->target_msaa       = render->msaa_texture;
    batch->target_depth      = render->depth_texture;
    batch->target_width      = window->screen_width;
    batch->target_height     = window->screen_height;
    batch->target_is_texture = false;

    // LOAD, not CLEAR: whatever was drawn to the window before the render texture is still wanted.
    _nya_render2d_pass_resume(window);
}

void nya_render2d_render_texture(NYA_Window* window, const NYA_RenderTexture* render_texture, f32 x, f32 y, f32 width, f32 height, NYA_Color tint) {
    nya_assert(window != nullptr);
    nya_assert(render_texture != nullptr);

    if (render_texture->texture == nullptr) return;

    NYA_Render2DBatch* batch = &window->render_system.draw_batch;

    nya_assert(render_texture->texture != batch->target_texture, "a render texture cannot be drawn while it is the target being drawn into");

    // Zero means natural size, which is the common case and saves the caller repeating the
    // dimensions it just created the texture with.
    f32 destination_width  = width > 0.0F ? width : (f32)render_texture->width;
    f32 destination_height = height > 0.0F ? height : (f32)render_texture->height;

    if (!_nya_render2d_prepare(window, NYA_RENDER2D_PIPELINE_TEXTURED, render_texture->texture, _nya_render_sampler_for(NYA_TEXTURE_FILTER_LINEAR), 4, 6)) return;

    _nya_render2d_quad(batch, x, y, destination_width, destination_height, 0.0F, 0.0F, 1.0F, 1.0F, tint);
}

void nya_render2d_target_size(NYA_Window* window, OUT u32* out_width, OUT u32* out_height) {
    nya_assert(window != nullptr);
    nya_assert(out_width != nullptr && out_height != nullptr);

    const NYA_Render2DBatch* batch = &window->render_system.draw_batch;

    *out_width  = batch->target_width;
    *out_height = batch->target_height;
}

u32 nya_render2d_pending_vertex_count(NYA_Window* window) {
    nya_assert(window != nullptr);
    return window->render_system.draw_batch.vertex_count;
}

NYA_Render2DFrameStats nya_render2d_frame_stats(NYA_Window* window) {
    nya_assert(window != nullptr);

    NYA_Render2DBatch* batch = &window->render_system.draw_batch;

    NYA_Render2DFrameStats stats = {
        .draw_calls    = batch->frame_flushes,
        .vertices      = batch->frame_vertices,
        .indices       = batch->frame_indices,
        .dropped_draws = batch->frame_dropped_draws,
    };

    for (u32 i = 0; i < NYA_RENDER2D_FLUSH_REASON_COUNT; i++) stats.draw_calls_by_reason[i] = batch->frame_flush_reasons[i];

    return stats;
}

NYA_ConstCString nya_render2d_flush_reason_name(NYA_Render2DFlushReason reason) {
    switch (reason) {
        case NYA_RENDER2D_FLUSH_PIPELINE:  return "pipeline";
        case NYA_RENDER2D_FLUSH_TEXTURE:   return "texture";
        case NYA_RENDER2D_FLUSH_SAMPLER:   return "sampler";
        case NYA_RENDER2D_FLUSH_STATE:     return "state";
        case NYA_RENDER2D_FLUSH_FULL:      return "batch full";
        case NYA_RENDER2D_FLUSH_FRAME_END: return "frame end";

        case NYA_RENDER2D_FLUSH_REASON_COUNT:
        default:                       return "unknown";
    }
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

b8 _nya_render2d_wrap_line(
    NYA_FontAtlas*   atlas,
    TTF_Font*        face,
    NYA_ConstCString cursor,
    f32              max_width,
    OUT u64*         out_length,
    OUT u64*         out_skip,
    OUT f32*         out_width
) {
    if (cursor == nullptr || *cursor == '\0') return false;

    f32 width = 0.0F;

    u64 offset = 0;

    // Zero at the start of every line, so the first glyph after a wrap kerns against nothing — which is
    // what it now sits next to.
    u32 previous = 0;

    /*
     * The last place a break would be legal, remembered as we go.
     *
     * Zero means "no space seen yet on this line", which is what makes a single over-long word fall
     * through to the character break below rather than wrapping to a width of nothing.
     */
    u64 break_length = 0;
    u64 break_skip   = 0;
    f32 break_width  = 0.0F;

    for (;;) {
        u32 codepoint = 0;

        u64 step = (u64)nya_utf8_next(cursor + offset, &codepoint);
        if (step == 0) step = 1;

        // End of string: the rest of it is the line.
        if (codepoint == 0) {
            *out_length = offset;
            *out_skip   = offset;
            *out_width  = width;

            return true;
        }

        // An explicit break always wins over the wrapping below, and consumes the newline itself.
        if (codepoint == '\n') {
            *out_length = offset;
            *out_skip   = offset + step;
            *out_width  = width;

            return true;
        }

        const NYA_Glyph* glyph = _nya_render2d_glyph(atlas, codepoint);

        // The kerning is part of this glyph's cost, so the fit test below accounts for it exactly as the
        // draw will apply it.
        f32 advance = _nya_render2d_kerning(face, previous, codepoint);

        advance += glyph != nullptr ? glyph->advance : atlas->glyphs[0].advance;

        /*
         * Too wide, and there is somewhere legal to break: break there.
         *
         * The space itself goes into the skip and not the length, so it is neither drawn nor counted in
         * the line's width — a trailing space on a right-aligned line would otherwise push the text left
         * by a space for no visible reason.
         */
        if (max_width > 0.0F && offset > 0 && (width + advance) > max_width) {
            if (break_length > 0) {
                *out_length = break_length;
                *out_skip   = break_skip;
                *out_width  = break_width;

                return true;
            }

            /*
             * Nowhere legal to break, so break here — mid-word.
             *
             * A word longer than the box has to go somewhere, and the alternatives are worse: letting it
             * overflow puts text outside the panel that was sized to hold it, and refusing to draw it
             * loses the text entirely. An ugly break is the one failure a reader can still work with.
             */
            *out_length = offset;
            *out_skip   = offset;
            *out_width  = width;

            return true;
        }

        width   += advance;
        offset  += step;
        previous = codepoint;

        // Recorded *after* the space is consumed, so the break point is the text up to it.
        if (codepoint == ' ') {
            break_length = offset - step;
            break_skip   = offset;
            break_width  = width - advance;
        }
    }
}

f32x2 _nya_render2d_text_box_layout(NYA_Window* window, NYA_ConstCString text, NYA_Render2DTextBox params) {
    if (text == nullptr || text[0] == '\0') return f32x2_zero;

    NYA_ConstCString font_path  = params.font_path != nullptr ? params.font_path : _nya_render2d_current_font;
    f32              point_size = params.point_size > 0.0F ? params.point_size : _nya_render2d_current_font_size;

    /*
     * A measure needs an atlas too, and building one needs a window.
     *
     * _nya_render2d_font_atlas takes a window because rasterising creates a GPU texture. A pure measure
     * therefore has to reach the atlas some other way — it uses the same lookup with a null window, which
     * finds an already-built atlas and declines to build a new one. Measuring a font nothing has drawn yet
     * answers zero, which is the same answer nya_render2d_text_measure has always given.
     */
    NYA_FontAtlas* atlas = _nya_render2d_font_atlas(window, font_path, point_size);
    if (atlas == nullptr) return f32x2_zero;

    f32 line_height = atlas->line_height * (params.line_spacing > 0.0F ? params.line_spacing : 1.0F);

    // Once for the whole box, and handed to both the wrap and the emit so they cannot disagree.
    TTF_Font* face = _nya_render2d_atlas_font(atlas);

    /*
     * Every glyph resolved before any quad is emitted.
     *
     * The same two-pass rule nya_render2d_text_with_font documents at length: resolving may rasterise a
     * glyph, which dirties the atlas surface, and a quad queued before the upload points at a cell the
     * GPU has not received. Here it matters more, because the wrapping pass *also* resolves glyphs to
     * measure them — so without this the first frame of a wrapped string would be blank rectangles.
     */
    if (window != nullptr) {
        for (NYA_ConstCString scan = text; *scan != '\0';) {
            u32 codepoint = 0;

            u64 step = (u64)nya_utf8_next(scan, &codepoint);
            if (step == 0) step = 1;

            scan += step;

            if (codepoint == '\n') continue;

            (void)_nya_render2d_glyph(atlas, codepoint);
        }

        /*
         * The ellipsis glyph too, before the upload rather than when it is drawn.
         *
         * It is emitted below only when the text was truncated, and it was resolved at that point — after
         * this upload. Baking a glyph dirties the atlas *surface*, so a full stop that the text itself did
         * not contain was drawn from a cell the GPU had not received: three blank rectangles for one
         * frame, and again every time the atlas is rebuilt. Exactly the trap the two-pass rule exists for.
         *
         * Resolved unconditionally, because whether the text truncates is not known until the layout runs
         * and one glyph is not worth a second upload to find out.
         */
        if (params.ellipsis) (void)_nya_render2d_glyph(atlas, '.');

        _nya_render2d_atlas_upload(window, atlas);
    }

    f32 widest = 0.0F;
    f32 pen_y  = params.y;

    u32 line_index = 0;

    NYA_ConstCString cursor = text;

    for (;;) {
        u64 length = 0;
        u64 skip   = 0;
        f32 width  = 0.0F;

        if (!_nya_render2d_wrap_line(atlas, face, cursor, params.width, &length, &skip, &width)) break;

        b8 last_allowed = params.max_lines > 0 && line_index + 1 >= params.max_lines;

        // Something is left over after this line, and this line is the last one allowed.
        b8 truncated = last_allowed && cursor[skip] != '\0';

        widest = nya_max(widest, width);

        /*
         * Alignment is a horizontal offset, computed against the box width.
         *
         * A zero box width makes centre and right align against `x` itself, which is what a centred
         * title without a wrap width means: the line is centred *on the point*. Any other reading would
         * need a width the caller did not give.
         */
        f32 pen_x = params.x;

        switch (params.align) {
            case NYA_TEXT_ALIGN_CENTER: pen_x += (params.width - width) * 0.5F; break;
            case NYA_TEXT_ALIGN_RIGHT: pen_x += params.width - width; break;

            case NYA_TEXT_ALIGN_LEFT:
            case NYA_TEXT_ALIGN_COUNT:
            default: break;
        }

        if (window != nullptr) {
            u32 previous = 0;

            for (u64 offset = 0; offset < length;) {
                u32 codepoint = 0;

                u64 step = (u64)nya_utf8_next(cursor + offset, &codepoint);
                if (step == 0) step = 1;

                offset += step;

                pen_x += _nya_render2d_kerning(face, previous, codepoint);

                previous = codepoint;

                const NYA_Glyph* glyph = _nya_render2d_glyph(atlas, codepoint);

                if (glyph == nullptr) {
                    pen_x += atlas->glyphs[0].advance;
                    continue;
                }

                if (glyph->width > 0.0F && glyph->height > 0.0F) {
                    if (!_nya_render2d_prepare(window, NYA_RENDER2D_PIPELINE_TEXTURED, atlas->texture,
                                               _nya_render_sampler_for(NYA_TEXTURE_FILTER_NEAREST), 4, 6)) {
                        return (f32x2){ widest, (f32)(line_index + 1) * line_height };
                    }

                    // Snapped to whole pixels, for the reason the unwrapped draw gives: point sampling
                    // only stays crisp on the pixel grid.
                    _nya_render2d_quad(
                        &window->render_system.draw_batch,
                        roundf(pen_x + glyph->bearing_x),
                        roundf(pen_y + glyph->bearing_y),
                        glyph->width,
                        glyph->height,
                        glyph->u0,
                        glyph->v0,
                        glyph->u1,
                        glyph->v1,
                        params.color
                    );
                }

                pen_x += glyph->advance;
            }

            /*
             * The ellipsis, appended after the line rather than fitted inside it.
             *
             * Strictly it should displace enough trailing characters to make room, which needs a second
             * backward pass over the line. Appending overhangs the box by up to three dots' width, which
             * at any realistic box size is less wrong than the alternative and is a great deal less code.
             */
            if (truncated && params.ellipsis) {
                for (u32 dot = 0; dot < 3; dot++) {
                    const NYA_Glyph* glyph = _nya_render2d_glyph(atlas, '.');
                    if (glyph == nullptr) break;

                    if (glyph->width > 0.0F && glyph->height > 0.0F) {
                        if (!_nya_render2d_prepare(window, NYA_RENDER2D_PIPELINE_TEXTURED, atlas->texture,
                                                   _nya_render_sampler_for(NYA_TEXTURE_FILTER_NEAREST), 4, 6)) {
                            break;
                        }

                        _nya_render2d_quad(
                            &window->render_system.draw_batch,
                            roundf(pen_x + glyph->bearing_x),
                            roundf(pen_y + glyph->bearing_y),
                            glyph->width,
                            glyph->height,
                            glyph->u0,
                            glyph->v0,
                            glyph->u1,
                            glyph->v1,
                            params.color
                        );
                    }

                    pen_x += glyph->advance;
                }
            }
        }

        pen_y += line_height;
        line_index++;

        cursor += skip;

        if (*cursor == '\0') break;
        if (last_allowed) break;
    }

    return (f32x2){ widest, (f32)line_index * line_height };
}

void _nya_render2d_vertex(NYA_Render2DBatch* batch, f32 x, f32 y, f32 u, f32 v, NYA_Color color) {
    nya_assert(batch->vertex_count < NYA_RENDER2D_MAX_VERTICES, "_nya_render2d_prepare was not called, or lied");

    NYA_Vertex2D* vertex = &batch->vertices[batch->vertex_count];

    // No z. The projection puts everything on the same plane and nothing here is depth tested, so
    // carrying a per vertex depth would be four bytes a frame spent on a constant.
    vertex->x = x;
    vertex->y = y;
    vertex->u = u;
    vertex->v = v;
    _nya_render2d_pack_color(color, vertex->color);

    batch->vertex_count++;
}

void _nya_render2d_quad(NYA_Render2DBatch* batch, f32 x, f32 y, f32 width, f32 height, f32 u0, f32 v0, f32 u1, f32 v1, NYA_Color color) {
    f32 right  = x + width;
    f32 bottom = y + height;

    // Four corners and six indices, not six vertices. The two triangles share the diagonal, so the
    // duplicated pair is exactly what indexing exists to remove.
    u32 base = batch->vertex_count;

    _nya_render2d_vertex(batch, x, y, u0, v0, color);
    _nya_render2d_vertex(batch, right, y, u1, v0, color);
    _nya_render2d_vertex(batch, right, bottom, u1, v1, color);
    _nya_render2d_vertex(batch, x, bottom, u0, v1, color);

    _nya_render2d_triangle_indices(batch, base, 0, 1, 2);
    _nya_render2d_triangle_indices(batch, base, 0, 2, 3);
}

void _nya_render2d_quad_corners(NYA_Render2DBatch* batch, const f32x2 corners[4], f32 u0, f32 v0, f32 u1, f32 v1, NYA_Color color) {
    // Same two triangles and the same shared diagonal as the axis aligned case, so a rotated sprite
    // and an unrotated one rasterize identically at zero rotation.
    u32 base = batch->vertex_count;

    _nya_render2d_vertex(batch, corners[0][0], corners[0][1], u0, v0, color);
    _nya_render2d_vertex(batch, corners[1][0], corners[1][1], u1, v0, color);
    _nya_render2d_vertex(batch, corners[2][0], corners[2][1], u1, v1, color);
    _nya_render2d_vertex(batch, corners[3][0], corners[3][1], u0, v1, color);

    _nya_render2d_triangle_indices(batch, base, 0, 1, 2);
    _nya_render2d_triangle_indices(batch, base, 0, 2, 3);
}

void _nya_render2d_rect_rotated_corners(f32x2 center, f32x2 size, f32 rotation, OUT f32x2 out_corners[4]) {
    f32 half_width  = size.x * 0.5F;
    f32 half_height = size.y * 0.5F;

    f32 sine   = sinf(rotation);
    f32 cosine = cosf(rotation);

    // The ordinary rotation matrix, in a coordinate space whose y points down — which is what makes
    // a positive angle read as clockwise on screen rather than counter clockwise. Same sense as
    // NYA_Render2DTexture.rotation and as a 2D rigid body's angle, so all three agree.
    f32x2 across = { cosine * half_width, sine * half_width };
    f32x2 down   = { -sine * half_height, cosine * half_height };

    out_corners[0] = center - across - down;
    out_corners[1] = center + across - down;
    out_corners[2] = center + across + down;
    out_corners[3] = center - across + down;
}



void _nya_render2d_triangle_indices(NYA_Render2DBatch* batch, u32 base, u32 a, u32 b, u32 c) {
    nya_assert(batch->index_count + 3 <= NYA_RENDER2D_MAX_INDICES, "_nya_render2d_prepare was not called, or lied");

    batch->indices[batch->index_count++] = base + a;
    batch->indices[batch->index_count++] = base + b;
    batch->indices[batch->index_count++] = base + c;
}

void _nya_render2d_pack_color(NYA_Color color, OUT u8 out_rgba[4]) {
    // Clamped before scaling: a component outside 0..1 wraps rather than saturating once it is cast
    // to a byte, so an over-bright colour would come out dark instead of white.
    out_rgba[0] = (u8)(nya_clamp(color.r, 0.0F, 1.0F) * 255.0F + 0.5F);
    out_rgba[1] = (u8)(nya_clamp(color.g, 0.0F, 1.0F) * 255.0F + 0.5F);
    out_rgba[2] = (u8)(nya_clamp(color.b, 0.0F, 1.0F) * 255.0F + 0.5F);
    out_rgba[3] = (u8)(nya_clamp(color.a, 0.0F, 1.0F) * 255.0F + 0.5F);
}

b8 _nya_render2d_prepare(NYA_Window* window, NYA_ConstCString pipeline, SDL_GPUTexture* texture, SDL_GPUSampler* sampler, u32 vertex_count, u32 index_count) {
    NYA_Render2DBatch* batch = &window->render_system.draw_batch;

    // Nothing to draw into. Cheaper to answer here than to accumulate all frame and throw it away at
    // flush, and it makes an occluded window cost almost nothing.
    // Counted rather than merely refused: a draw that quietly does nothing is indistinguishable from
    // one that worked, and the first frames after an asset load legitimately hit this.
    if (window->render_system.render_pass == nullptr) {
        batch->frame_dropped_draws++;
        return false;
    }

    if (batch->vertices == nullptr) {
        batch->frame_dropped_draws++;
        return false;
    }

    // One shape bigger than the entire buffer. Flushing would not help and emitting part of it would
    // draw a torn shape, so it is refused and said out loud — the fix is a larger
    // NYA_RENDER2D_MAX_VERTICES, which the caller cannot guess without being told.
    if (vertex_count > NYA_RENDER2D_MAX_VERTICES || index_count > NYA_RENDER2D_MAX_INDICES) {
        nya_warn(
            "a single shape needs %u vertices and %u indices, past NYA_RENDER2D_MAX_VERTICES (%d) or NYA_RENDER2D_MAX_INDICES (%d)",
            vertex_count,
            index_count,
            NYA_RENDER2D_MAX_VERTICES,
            NYA_RENDER2D_MAX_INDICES
        );

        batch->frame_dropped_draws++;
        return false;
    }

    /*
     * A draw call has one pipeline and one texture, so anything queued under different state has to
     * go out first.
     *
     * Pipeline handles are compared by pointer rather than by content because they are the
     * NYA_RENDER2D_PIPELINE_* literals, which are the same pointer every time. A custom pipeline arrives
     * through shader_override instead, which is not consulted here at all — switching it flushes on
     * its own.
     */
    if (batch->index_count > batch->range_first_index) {
        // Attributed most-specific first: a pipeline change usually brings a texture change with it,
        // and reporting it as a texture swap would send someone off to build an atlas that would not
        // have helped.
        if (batch->pipeline != pipeline) batch->pending_flush_reason = NYA_RENDER2D_FLUSH_PIPELINE;
        else if (batch->texture != texture) batch->pending_flush_reason = NYA_RENDER2D_FLUSH_TEXTURE;
        else if (batch->sampler != sampler) batch->pending_flush_reason = NYA_RENDER2D_FLUSH_SAMPLER;

        /*
         * A state change closes a range rather than drawing one.
         *
         * This is the whole change: the geometry stays in the staging arrays and is issued later, in
         * layer order, instead of the moment the state moved. See NYA_Render2DDrawRange.
         */
        if (batch->pipeline != pipeline || batch->texture != texture || batch->sampler != sampler) {
            _nya_render2d_range_close(window);
        }
    }

    /*
     * Out of room, or out of ranges: this one has to actually draw.
     *
     * Deferring is bounded by the staging arrays every pending range shares — once they are full,
     * nothing more can be recorded and the only way forward is to issue what is held. A frame that
     * hits this loses cross-layer ordering for the geometry on either side of it, which is why the
     * bounds are generous rather than tight.
     */
    if (batch->vertex_count + vertex_count > NYA_RENDER2D_MAX_VERTICES ||
        batch->index_count + index_count > NYA_RENDER2D_MAX_INDICES ||
        batch->range_count + 1 >= NYA_RENDER2D_MAX_RANGES) {
        batch->pending_flush_reason = NYA_RENDER2D_FLUSH_FULL;
        nya_render2d_flush(window);
    }

    batch->pipeline = (NYA_CString)pipeline;
    batch->texture  = texture;
    batch->sampler  = sampler;

    // The flush may have found no pipeline loaded and cleared the batch without drawing, which is
    // not a reason to refuse: the vertices are wanted next frame once it finishes loading.
    return true;
}

void _nya_render2d_pass_suspend(NYA_Window* window) {
    NYA_RenderSystemWindow* render = &window->render_system;

    if (render->render_pass == nullptr) return;

    SDL_EndGPURenderPass(render->render_pass);
    render->render_pass = nullptr;
}

void _nya_render2d_apply_scissor(NYA_Window* window) {
    NYA_RenderSystemWindow* render = &window->render_system;
    NYA_Render2DBatch*          batch  = &render->draw_batch;

    if (render->render_pass == nullptr) return;

    if (!batch->scissor_active) {
        // The whole target, which is what a pass starts as. SDL has no "disable", so the way to stop
        // clipping is to clip to everything.
        SDL_SetGPUScissor(render->render_pass, &(SDL_Rect){ .x = 0, .y = 0, .w = (s32)batch->target_width, .h = (s32)batch->target_height });
        return;
    }

    SDL_SetGPUScissor(
        render->render_pass,
        &(SDL_Rect){ .x = batch->scissor_x, .y = batch->scissor_y, .w = batch->scissor_width, .h = batch->scissor_height }
    );
}

void _nya_render2d_pass_resume(NYA_Window* window) {
    NYA_RenderSystemWindow* render = &window->render_system;
    NYA_Render2DBatch*          batch  = &render->draw_batch;

    if (render->render_commands == nullptr) return;

    /*
     * Resuming a shadow pass: back onto the light-space depth map.
     *
     * Mid-pass flushes during the shadow pass — e.g. a material change — suspend the pass to upload
     * their vertices, and this resume has to point them back at the shadow map rather than at the
     * window. Without this branch, everything drawn after the first shadow flush lands in the scene's
     * colour buffer at light-space coordinates.
     *
     * `active` is what tells them apart: it is false during the 2D flush that begins a 3D pass, so
     * that flush correctly lands on the window; it is set true immediately afterwards, so every
     * subsequent 3D flush correctly lands on the shadow map.
     */
    if (render->mesh_batch.shadow_pass_active && render->mesh_batch.active) {
        render->render_pass = SDL_BeginGPURenderPass(
            render->render_commands,
            &(SDL_GPUColorTargetInfo){
                .texture = render->mesh_batch.shadow_color,
                .load_op = SDL_GPU_LOADOP_LOAD,
                .store_op = SDL_GPU_STOREOP_STORE,
            },
            1,
            &(SDL_GPUDepthStencilTargetInfo){
                .texture          = render->mesh_batch.shadow_depth,
                .load_op          = SDL_GPU_LOADOP_LOAD,
                .store_op         = SDL_GPU_STOREOP_STORE,
                .stencil_load_op  = SDL_GPU_LOADOP_DONT_CARE,
                .stencil_store_op = SDL_GPU_STOREOP_DONT_CARE,
            }
        );

        nya_assert(render->render_pass != nullptr, "SDL_BeginGPURenderPass() failed while resuming a shadow pass: %s", SDL_GetError());
        return;
    }

    if (batch->target_texture == nullptr) return;

    /*
     * Multisampling is resolved once a frame, on the last pass, not on every reopen.
     *
     * A frame opens a pass per flush — every flush suspends to run its copy pass — so resolving as
     * each one ends resolved the entire target once per draw call: fourteen full resolves in a
     * frame with fourteen draws. Only the last one produces pixels anyone sees; the rest are thrown
     * away by the next pass's LOAD.
     *
     * Intermediate passes therefore target the multisample texture alone, with no resolve texture
     * attached at all, and STORE. Attaching a resolve texture and asking for a plain STORE is not a
     * combination to rely on — if there is a resolve target, the store op is expected to resolve.
     *
     * The earlier attempt at this kept the resolve target attached throughout and put the single
     * resolve in a final *empty* pass, which segfaults inside the AMD Vulkan driver at
     * BeginRenderPass. There is no empty pass here: nya_render_end guarantees the resolving pass has
     * something to draw before it opens, so the driver never sees that shape.
     *
     * Render textures are exempt and always resolve. They are ended explicitly and read back the
     * moment they are, so there is no later pass to defer to.
     */
    b8 resolving = batch->target_msaa != nullptr && (batch->target_is_texture || batch->resolve_pending);

    render->render_pass = SDL_BeginGPURenderPass(
        render->render_commands,
        &(SDL_GPUColorTargetInfo){
            .texture         = batch->target_msaa != nullptr ? batch->target_msaa : batch->target_texture,
            .resolve_texture = resolving ? batch->target_texture : nullptr,
            // Always LOAD. This reopens a pass in the middle of drawing a target, so clearing here
            // would wipe everything queued before whatever forced the suspend. RESOLVE_AND_STORE
            // rather than RESOLVE also keeps the multisample contents, which is what makes the LOAD
            // find anything.
            .load_op  = SDL_GPU_LOADOP_LOAD,
            .store_op = resolving ? SDL_GPU_STOREOP_RESOLVE_AND_STORE : SDL_GPU_STOREOP_STORE,
        },
        1,
        // LOAD, not CLEAR, for exactly the reason the colour target does: this reopens a pass in the
        // middle of drawing a target, and clearing here would throw away the depth of everything
        // drawn before whatever forced the suspend.
        &(SDL_GPUDepthStencilTargetInfo){
            .texture          = batch->target_depth,
            .load_op          = SDL_GPU_LOADOP_LOAD,
            .store_op         = SDL_GPU_STOREOP_STORE,
            .stencil_load_op  = SDL_GPU_LOADOP_DONT_CARE,
            .stencil_store_op = SDL_GPU_STOREOP_DONT_CARE,
        }
    );
    nya_assert(render->render_pass != nullptr, "SDL_BeginGPURenderPass() failed while resuming: %s", SDL_GetError());

    // A fresh pass clips to nothing, so whatever the batch was clipping to has to go back on.
    _nya_render2d_apply_scissor(window);
}

NYA_FontAtlas* _nya_render2d_font_atlas(NYA_Window* window, NYA_ConstCString font_path, f32 point_size) {
    if (font_path == nullptr) return nullptr;
    if (point_size <= 0.0F) point_size = NYA_RENDER2D_FONT_DEFAULT_SIZE;

    /*
     * The asset handle is derived from the path and the size, and the asset is queued if it is not
     * there yet.
     *
     * A face carries no point size, so one .ttf at two sizes is two textures and therefore two
     * assets — and an asset is keyed by one handle, so the two cannot both be keyed on the path. A
     * game used to resolve that by inventing a handle ("neat_font") and loading the file a second
     * time under it, which put an asset system detail in the caller's lap and made the font argument
     * to every draw call a name that meant nothing on its own.
     *
     * Derived here instead. A caller passes NYA_ASSET_FONTS_ALDRICH_TTF and a size, and gets that
     * face at that size — loading it on first use if nobody has.
     */
    char derived[NYA_RENDER2D_FONT_HANDLE_MAX];
    (void)snprintf(derived, sizeof(derived), "%s@%.0f", font_path, (f64)point_size);

    /*
     * The asset is resolved first, on every call, and that is deliberate.
     *
     * The cache used to answer from a hit without ever touching the asset system, which meant a font
     * was looked up exactly once per process — so hot reload could not see it (nothing polled it)
     * and could not have helped if it had (the atlas holds glyphs baked from a TTF_Font the reload
     * replaces). Fonts silently did not hot reload while every other asset type did.
     *
     * The lookup is cheap: nya_asset_get rate limits its stat, so this is a dictionary hit on all
     * but a handful of calls a second.
     */
    NYA_Asset* asset = nya_asset_get(derived);

    if (asset == nullptr) {
        // Queued, not loaded: the asset system resolves it over the next frames, and every caller
        // here already copes with there being no atlas yet by drawing nothing.
        NYA_EXPECT(nya_asset_load((NYA_AssetLoadParameters){
          .type    = NYA_ASSET_TYPE_FONT,
          .handle  = derived,
          .source  = font_path,
          .as_font = { .point_size = point_size },
      }), "while queueing a font");

        return nullptr;
    }

    if (asset->status != NYA_ASSET_STATUS_LOADED) return nullptr;
    if (asset->as_font.font == nullptr) return nullptr;

    // The common case: the current font, already resolved and still built from the same face.
    if (font_path == _nya_render2d_current_font && point_size == _nya_render2d_current_font_size && _nya_render2d_current_atlas != nullptr &&
        _nya_render2d_current_atlas->source_font == asset->as_font.font) {
        return _nya_render2d_current_atlas;
    }

    for (u32 i = 0; i < NYA_RENDER2D_FONT_CACHE_MAX; i++) {
        if (_nya_render2d_font_cache[i].path == nullptr) continue;
        if (_nya_render2d_font_cache[i].point_size != point_size || strcmp(_nya_render2d_font_cache[i].path, font_path) != 0) continue;

        if (_nya_render2d_font_cache[i].source_font == asset->as_font.font) {
            if (font_path == _nya_render2d_current_font && point_size == _nya_render2d_current_font_size) _nya_render2d_current_atlas = &_nya_render2d_font_cache[i];
            return &_nya_render2d_font_cache[i];
        }

        /*
         * Reloaded. The slot is freed and falls through to be rebuilt below.
         *
         * Flushed first: queued vertices reference the texture about to be released, and releasing
         * it while a draw call still names it is a use after free inside the driver. Without a
         * window there is no batch to flush and no pass to upload into, so the stale atlas is
         * returned for now — measurement calls arrive that way, and being one frame behind on a text
         * width is not worth a rebuild path that cannot flush.
         */
        if (window == nullptr) return &_nya_render2d_font_cache[i];

        nya_debug("font '%s' reloaded; rebuilding its glyph atlas", derived);

        _nya_render2d_flush_for(window, NYA_RENDER2D_FLUSH_STATE);

        // All three, not just the texture. The atlas now owns a CPU surface and a transfer buffer as
        // well, and a reload that released only the texture leaked both — once per reload, which on a
        // hot-reloading development build is once every time a line changes.
        SDL_GPUDevice* device = nya_app_get()->render_system.gpu_device;
        NYA_FontAtlas* stale  = &_nya_render2d_font_cache[i];

        SDL_ReleaseGPUTexture(device, stale->texture);
        if (stale->transfer_buffer != nullptr) SDL_ReleaseGPUTransferBuffer(device, stale->transfer_buffer);
        if (stale->surface != nullptr) SDL_DestroySurface(stale->surface);

        if (_nya_render2d_current_atlas == stale) _nya_render2d_current_atlas = nullptr;

        *stale = (NYA_FontAtlas){ 0 };
        break;
    }

    NYA_FontAtlas* slot = nullptr;
    for (u32 i = 0; i < NYA_RENDER2D_FONT_CACHE_MAX; i++) {
        if (_nya_render2d_font_cache[i].path == nullptr) {
            slot = &_nya_render2d_font_cache[i];
            break;
        }
    }

    // Full rather than evicting. Eviction would have to know that no queued vertex still references
    // the texture it is about to release, and a game that needs more than eight faces at once is
    // better served by raising the constant than by a cache that quietly drops one.
    if (slot == nullptr) {
        nya_warn("no free glyph atlas slot for '%s'; raise NYA_RENDER2D_FONT_CACHE_MAX (%d)", derived, NYA_RENDER2D_FONT_CACHE_MAX);
        return nullptr;
    }

    TTF_Font*      font       = asset->as_font.font;
    SDL_GPUDevice* gpu_device = nya_app_get()->render_system.gpu_device;

    /*
     * A fixed grid rather than a packer.
     *
     * Every cell is as large as the tallest and widest glyph, so a 95 glyph atlas wastes the
     * difference. For one face that is a few hundred kilobytes and it makes the layout arithmetic a
     * multiply instead of a rectangle packer — the right trade until an atlas holds enough faces for
     * the waste to matter.
     */
    s32 line_skip = TTF_GetFontLineSkip(font);
    s32 ascent    = TTF_GetFontAscent(font);
    // SDL reports the descent below the baseline as negative; flipped so ascent + descent is the ink
    // height, which is what a caller doing arithmetic with the two expects.
    s32 descent   = -TTF_GetFontDescent(font);

    /*
     * A cell is the size of a *rendered glyph surface*, not of the glyph's ink.
     *
     * TTF_RenderGlyph_Blended hands back a surface the full height of the font with the glyph already
     * sitting on its baseline, and the full width of the advance with the glyph already at its left
     * bearing. Measured, not assumed: every glyph in this face comes back 23 pixels tall at 24pt,
     * whether it is an 'x' or a 'd'.
     *
     * That is why the glyphs below carry no bearing. Positioning them by `ascent - max_y` as well
     * applies the baseline offset a second time, which lifts every ascender and drops everything at
     * x-height — text that looks like the letters are on different lines.
     */
    s32 cell_width  = 1;
    s32 cell_height = nya_max(TTF_GetFontHeight(font), line_skip);

    for (s32 character = NYA_RENDER2D_GLYPH_FIRST; character <= NYA_RENDER2D_GLYPH_LAST; character++) {
        s32 min_x = 0, max_x = 0, min_y = 0, max_y = 0, advance = 0;
        if (!TTF_GetGlyphMetrics(font, (u32)character, &min_x, &max_x, &min_y, &max_y, &advance)) continue;

        // The wider of the two: the advance is what the surface is normally sized to, but a glyph
        // whose ink overhangs its advance would otherwise be clipped by the cell.
        cell_width = nya_max(cell_width, nya_max(advance, max_x));
    }

    /*
     * Widened past what ASCII needs, because the cells are sized once and filled forever.
     *
     * A glyph baked later has to fit the cell chosen now, and a `W` with an umlaut over it or a `Æ`
     * is wider than anything in ASCII. Half again is measured rather than guessed: across the Latin
     * Extended blocks the widest glyph in a typical face is around a third wider than the widest
     * ASCII one, and a cell too small clips the new glyph rather than failing to bake it — which
     * looks like a font bug and is invisible until someone plays in the language that has it.
     */
    cell_width  = (cell_width * 3) / 2;
    cell_height = (cell_height * 3) / 2;

    // A one pixel gutter, so linear filtering at a cell edge cannot bleed the neighbouring glyph in.
    // Without it the right edge of every glyph picks up a sliver of the next one at small sizes.
    cell_width  += 2;
    cell_height += 2;

    const s32 columns      = NYA_RENDER2D_GLYPH_COLUMNS;
    s32       rows         = (NYA_RENDER2D_GLYPH_CAPACITY + columns - 1) / columns;
    s32       atlas_width  = cell_width * columns;
    s32       atlas_height = cell_height * rows;

    SDL_Surface* atlas_surface = SDL_CreateSurface(atlas_width, atlas_height, SDL_PIXELFORMAT_RGBA32);
    if (atlas_surface == nullptr) {
        nya_warn("could not allocate a glyph atlas for '%s': %s", derived, SDL_GetError());
        return nullptr;
    }

    // Transparent, not black: the untouched space between glyphs has to blend away rather than draw
    // as a box.
    SDL_ClearSurface(atlas_surface, 0.0F, 0.0F, 0.0F, 0.0F);

    *slot = (NYA_FontAtlas){
        .path         = font_path,
        .point_size   = point_size,
        .line_height  = (f32)line_skip,
        .ascent       = (f32)ascent,
        .descent      = (f32)descent,
        .surface      = atlas_surface,
        .atlas_width  = atlas_width,
        .atlas_height = atlas_height,
        .cell_width   = cell_width,
        .cell_height  = cell_height,
        // The ASCII block occupies the first slots, so a lookup for it is a subtraction. Everything
        // baked later appends past this.
        .glyph_count = NYA_RENDER2D_GLYPH_ASCII,
    };

    // Copied, because the asset system keeps the pointer it was handed and `derived` is a local.
    (void)snprintf(slot->handle, sizeof(slot->handle), "%s", derived);

    // ASCII up front, because essentially every string contains it. Everything else is baked the
    // first time it is asked for; see _nya_render2d_glyph.
    for (u32 index = 0; index < NYA_RENDER2D_GLYPH_ASCII; index++) {
        slot->codepoints[index] = NYA_RENDER2D_GLYPH_FIRST + index;
        _nya_render2d_glyph_bake(slot, font, NYA_RENDER2D_GLYPH_FIRST + index, index);
    }


    SDL_GPUTexture* texture = SDL_CreateGPUTexture(
        gpu_device,
        &(SDL_GPUTextureCreateInfo){
            .type                 = SDL_GPU_TEXTURETYPE_2D,
            .format               = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM,
            .usage                = SDL_GPU_TEXTUREUSAGE_SAMPLER,
            .width                = (u32)atlas_width,
            .height               = (u32)atlas_height,
            .layer_count_or_depth = 1,
            .num_levels           = 1,
        }
    );
    nya_assert(texture != nullptr, "SDL_CreateGPUTexture() failed for a glyph atlas: %s", SDL_GetError());

    slot->transfer_buffer = SDL_CreateGPUTransferBuffer(
        gpu_device,
        &(SDL_GPUTransferBufferCreateInfo){ .usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD, .size = (u32)(atlas_width * atlas_height * 4) }
    );
    nya_assert(slot->transfer_buffer != nullptr, "SDL_CreateGPUTransferBuffer() failed for a glyph atlas: %s", SDL_GetError());

    slot->texture = texture;

    // The ASCII block is already in the surface; this is what puts it on the GPU. Every later bake
    // marks the atlas dirty again and the next draw re-uploads.
    slot->upload_pending = true;
    _nya_render2d_atlas_upload(window, slot);

    // What a later lookup compares against to notice a reload. Set with the texture, so the two can
    // never disagree about which face the glyphs came from.
    slot->source_font = font;

    if (font_path == _nya_render2d_current_font && point_size == _nya_render2d_current_font_size) _nya_render2d_current_atlas = slot;

    nya_info("Built a glyph atlas for '%s' (%dx%d, %u of %d slots used).", derived, atlas_width, atlas_height, slot->glyph_count,
             NYA_RENDER2D_GLYPH_CAPACITY);

    return slot;
}

/*
 * ─────────────────────────────────────────────────────────
 * CAMERA INTERNALS
 * ─────────────────────────────────────────────────────────
 */

void _nya_render2d_camera_basis(const NYA_Camera2D* camera, OUT f32* out_a, OUT f32* out_b, OUT f32* out_c, OUT f32* out_d) {
    switch (camera->kind) {
        case NYA_CAMERA2D_KIND_TOP_DOWN: {
            // A rotation scaled by the zoom. Positive rotation reads clockwise on screen, which is
            // the same sense NYA_Render2DTexture.rotation and the 2D solver already use.
            f32 c = cosf(camera->as_top_down.rotation) * camera->as_top_down.zoom;
            f32 s = sinf(camera->as_top_down.rotation) * camera->as_top_down.zoom;

            *out_a = c;
            *out_b = -s;
            *out_c = s;
            *out_d = c;
        } break;

        case NYA_CAMERA2D_KIND_ISOMETRIC: {
            /*
             * The classic tile-to-screen map, as a matrix.
             *
             * Stepping one tile along +x moves half a tile right and half a tile down; stepping one
             * along +y moves half a tile *left* and half a tile down. That is what makes a square
             * grid draw as diamonds and what makes rows further down the screen read as further away.
             *
             * Halves because the widths are the full diamond, which is what the art is authored to.
             */
            f32 half_width  = camera->as_isometric.tile_width * 0.5F * camera->as_isometric.zoom;
            f32 half_height = camera->as_isometric.tile_height * 0.5F * camera->as_isometric.zoom;

            *out_a = half_width;
            *out_b = -half_width;
            *out_c = half_height;
            *out_d = half_height;
        } break;

        case NYA_CAMERA2D_KIND_NONE:
        default: {
            // The identity. Not reachable through the callers, which all check the kind first, but a
            // basis of zeroes would be a division by zero in the inverse rather than a visible bug.
            *out_a = 1.0F;
            *out_b = 0.0F;
            *out_c = 0.0F;
            *out_d = 1.0F;
        } break;
    }
}

f32x2 _nya_render2d_camera_position(const NYA_Camera2D* camera) {
    switch (camera->kind) {
        case NYA_CAMERA2D_KIND_TOP_DOWN:  return camera->as_top_down.position;
        case NYA_CAMERA2D_KIND_ISOMETRIC: return camera->as_isometric.position;
        case NYA_CAMERA2D_KIND_NONE:
        default:                          return f32x2_zero;
    }
}

/*
 * ─────────────────────────────────────────────────────────
 * LIGHTS
 * ─────────────────────────────────────────────────────────
 */

void nya_render2d_lights_apply(NYA_Window* window, const NYA_Light2D* lights, const f32x2* positions, u32 count, NYA_Color ambient) {
    nya_perf_time_this_function();

    nya_assert(window != nullptr);

    NYA_RenderSystemWindow* render = &window->render_system;
    NYA_Render2DBatch*      batch  = &render->draw_batch;

    NYA_Asset* asset = nya_asset_get(NYA_RENDER2D_PIPELINE_LIGHT);
    if (asset == nullptr || asset->status != NYA_ASSET_STATUS_LOADED || asset->as_graphics_pipeline.pipeline == nullptr) {
        // Still loading, which is normal for the first frames. Dropped rather than drawn black: an
        // unlit frame is better than a frame that is entirely dark because the light map is missing.
        batch->frame_dropped_draws++;
        return;
    }

    // The scene this darkens has to be *in* the target before the multiply happens, so anything the
    // batch is still holding goes out first.
    _nya_render2d_flush_for(window, NYA_RENDER2D_FLUSH_PIPELINE);

    if (render->render_pass == nullptr || render->render_commands == nullptr) {
        batch->frame_dropped_draws++;
        return;
    }

    struct NYA_ShaderLight2DUniform uniform = {
        .ambient_r     = ambient.r,
        .ambient_g     = ambient.g,
        .ambient_b     = ambient.b,
        .target_width  = (f32)batch->target_width,
        .target_height = (f32)batch->target_height,
    };

    u32 kept = nya_min(count, (u32)NYA_SHADER_LIGHT2D_MAX);

    for (u32 i = 0; i < kept; i++) {
        /*
         * World to target pixels, through the same camera the scene was drawn with.
         *
         * The lights arrive in world coordinates because that is where the entities carrying them
         * are, and the shader works in target pixels because a fullscreen pass has no camera. Doing
         * the conversion here rather than in the shader means the camera matrix does not have to be
         * a uniform, and means a light's radius is in world units like everything else.
         */
        f32x2 screen = nya_render2d_world_to_screen(window, positions[i]);

        // The radius has to cross the same boundary, and a zoomed camera scales it — a torch does
        // not get smaller when the camera pulls back, it covers fewer pixels.
        f32x2 edge   = nya_render2d_world_to_screen(window, positions[i] + (f32x2){ lights[i].radius, 0.0F });
        f32   radius = nya_vector_length(edge - screen);

        uniform.lights[i][0] = screen.x;
        uniform.lights[i][1] = screen.y;
        uniform.lights[i][2] = radius;
        uniform.lights[i][3] = lights[i].intensity;

        uniform.colors[i][0] = lights[i].color.r;
        uniform.colors[i][1] = lights[i].color.g;
        uniform.colors[i][2] = lights[i].color.b;
        uniform.colors[i][3] = 0.0F;
    }

    uniform.count = (f32)kept;

    SDL_BindGPUGraphicsPipeline(render->render_pass, asset->as_graphics_pipeline.pipeline);

    // Fragment only. The fullscreen vertex shader builds its triangle from SV_VertexID and declares
    // no uniform buffer at all, so pushing to it would be a validation error rather than a no-op.
    SDL_PushGPUFragmentUniformData(render->render_commands, 0, &uniform, sizeof(uniform));

    // Three vertices, one oversized triangle. See procedural.vert.hlsl for why it is not two.
    SDL_DrawGPUPrimitives(render->render_pass, 3, 1, 0, 0);

    batch->frame_flushes++;
    batch->frame_flush_reasons[NYA_RENDER2D_FLUSH_PIPELINE]++;

    // Cleared, not left pointing at what it was: this bound a pipeline behind the batch's back, and
    // the batch skips rebinding when it believes the one it wants is already bound.
    batch->pipeline = nullptr;
}

/*
 * ─────────────────────────────────────────────────────────
 * GLYPHS
 * ─────────────────────────────────────────────────────────
 */

void _nya_render2d_glyph_bake(NYA_FontAtlas* atlas, TTF_Font* font, u32 codepoint, u32 slot) {
    NYA_Glyph* glyph = &atlas->glyphs[slot];

    s32 min_x = 0, max_x = 0, min_y = 0, max_y = 0, advance = 0;
    if (!TTF_GetGlyphMetrics(font, codepoint, &min_x, &max_x, &min_y, &max_y, &advance)) {
        // The face has no such glyph. Left with a zero advance and no ink, so it draws as nothing
        // rather than as whatever the cell happened to hold.
        *glyph = (NYA_Glyph){ 0 };
        return;
    }

    *glyph = (NYA_Glyph){ .advance = (f32)advance };

    s32 cell_x = (s32)(slot % NYA_RENDER2D_GLYPH_COLUMNS) * atlas->cell_width;
    s32 cell_y = (s32)(slot / NYA_RENDER2D_GLYPH_COLUMNS) * atlas->cell_height;

    /*
     * Blended, even though the atlas is not anti-aliased.
     *
     * TTF_RenderGlyph_Solid looks like the obvious choice and is wrong twice over: it returns an
     * 8-bit palettised surface whose background blits as opaque black, so every glyph came out in a
     * dark box, and it returns a surface cropped to the glyph rather than the full line box — which
     * the zero bearings below depend on. The anti-aliasing comes off afterwards, by thresholding.
     */
    SDL_Surface* glyph_surface = TTF_RenderGlyph_Blended(font, codepoint, (SDL_Color){ 255, 255, 255, 255 });
    if (glyph_surface == nullptr) return;

    defer SDL_DestroySurface(glyph_surface);

    // Clipped rather than allowed to spill into the neighbouring cell. A glyph wider than the cell is
    // a face whose Latin Extended block is far wider than its ASCII; better a clipped glyph than a
    // sliver of it appearing inside an unrelated character.
    s32 width  = nya_min(glyph_surface->w, atlas->cell_width - 2);
    s32 height = nya_min(glyph_surface->h, atlas->cell_height - 2);

    if (width <= 0 || height <= 0) return;

    // Cleared first, because a slot may be re-baked after a font reload and the old ink would
    // otherwise show through wherever the new glyph is thinner.
    SDL_FillSurfaceRect(
        atlas->surface, &(SDL_Rect){ .x = cell_x, .y = cell_y, .w = atlas->cell_width, .h = atlas->cell_height },
        SDL_MapSurfaceRGBA(atlas->surface, 0, 0, 0, 0)
    );

    // Copied, not blended: the glyph's alpha has to land in the atlas verbatim for the threshold
    // below to have anything to threshold.
    SDL_SetSurfaceBlendMode(glyph_surface, SDL_BLENDMODE_NONE);
    SDL_BlitSurface(
        glyph_surface, &(SDL_Rect){ .x = 0, .y = 0, .w = width, .h = height }, atlas->surface,
        &(SDL_Rect){ .x = cell_x + 1, .y = cell_y + 1, .w = width, .h = height }
    );

    /*
     * The coverage is kept. RGB is forced to white so the vertex colour alone decides the text colour.
     *
     * This used to threshold the alpha to fully on or fully off, turning every cell into a hard mask. The
     * reasoning was that the nearest sampler expects one, because point sampling a coverage ramp "keeps
     * the soft edge and just makes it blocky as well" — and that is the wrong diagnosis of a real problem.
     *
     * The blur being fought came from *linear* sampling at a half-texel offset. Nearest was the fix for
     * that, and it is a complete one: the glyph quad is snapped to whole pixels and drawn at exactly its
     * texel size, so one output pixel maps to one texel and nearest reproduces the cell exactly — coverage
     * and all. Thresholding on top threw away the anti-aliasing to solve a problem the sampler change had
     * already solved, and left every curve and diagonal jagged.
     *
     * RGB is still overwritten. TTF_RenderGlyph_Blended puts the requested colour in rgb and the coverage
     * in alpha, so the channels are already white — but a texel at zero coverage is not guaranteed to
     * carry a defined colour, and a texel whose rgb drifts toward black darkens the tint when it is
     * multiplied through.
     */
    for (s32 pixel_y = cell_y; pixel_y < cell_y + atlas->cell_height && pixel_y < atlas->atlas_height; pixel_y++) {
        u8* row = (u8*)atlas->surface->pixels + ((size_t)pixel_y * (size_t)atlas->surface->pitch);

        for (s32 pixel_x = cell_x; pixel_x < cell_x + atlas->cell_width && pixel_x < atlas->atlas_width; pixel_x++) {
            u8* pixel = row + ((size_t)pixel_x * 4);

            // RGBA32 is byte order dependent; SDL_PIXELFORMAT_RGBA32 is defined so that alpha is the
            // last byte on either endianness, which is what makes this indexable rather than masked.
            pixel[0] = 255;
            pixel[1] = 255;
            pixel[2] = 255;
        }
    }

    *glyph = (NYA_Glyph){
        .u0     = (f32)(cell_x + 1) / (f32)atlas->atlas_width,
        .v0     = (f32)(cell_y + 1) / (f32)atlas->atlas_height,
        .u1     = (f32)(cell_x + 1 + width) / (f32)atlas->atlas_width,
        .v1     = (f32)(cell_y + 1 + height) / (f32)atlas->atlas_height,
        .width  = (f32)width,
        .height = (f32)height,
        // Both zero: the surface is already the whole line box with the glyph placed inside it, so
        // the quad goes at the pen and the pen is the top left of the line.
        .bearing_x = 0.0F,
        .bearing_y = 0.0F,
        .advance   = (f32)advance,
    };

    atlas->upload_pending = true;
}

TTF_Font* _nya_render2d_atlas_font(const NYA_FontAtlas* atlas) {
    if (atlas == nullptr) return nullptr;

    // Cast because nya_asset_get takes a mutable handle while only reading it, and the atlas keeps its
    // own copy. The same cast every other lookup in this file makes.
    NYA_Asset* asset = nya_asset_get((NYA_AssetHandle)atlas->handle);
    if (asset == nullptr || asset->status != NYA_ASSET_STATUS_LOADED) return nullptr;

    return asset->as_font.font;
}

f32 _nya_render2d_kerning(TTF_Font* font, u32 previous, u32 current) {
    // No previous glyph means the start of a run, which nothing kerns against. Zero rather than a lookup
    // against codepoint zero, which some faces do have a pair for.
    if (font == nullptr || previous == 0) return 0.0F;

    s32 kerning = 0;

    // Failure is not an error: a face with no kern table simply has no pairs, which is the common case for
    // a monospace or a simple display font.
    if (!TTF_GetGlyphKerning(font, previous, current, &kerning)) return 0.0F;

    return (f32)kerning;
}

const NYA_Glyph* _nya_render2d_glyph(NYA_FontAtlas* atlas, u32 codepoint) {
    // ASCII by subtraction, which is the overwhelmingly common case and the reason the block is baked
    // in order at the front rather than appended like everything else.
    if (codepoint >= NYA_RENDER2D_GLYPH_FIRST && codepoint <= NYA_RENDER2D_GLYPH_LAST) {
        return &atlas->glyphs[codepoint - NYA_RENDER2D_GLYPH_FIRST];
    }

    // A linear scan of what has been baked past ASCII. Short in practice: a game shows one language
    // at a time, and one language is a few dozen accented characters. A map would cost an allocation
    // and a hash to save a scan that is usually under fifty comparisons.
    for (u32 i = NYA_RENDER2D_GLYPH_ASCII; i < atlas->glyph_count; i++) {
        if (atlas->codepoints[i] == codepoint) return &atlas->glyphs[i];
    }

    if (atlas->glyph_count >= NYA_RENDER2D_GLYPH_CAPACITY) {
        // Full rather than evicting, for the same reason the font cache is: eviction would have to
        // know that no queued vertex still references the cell it is about to overwrite.
        nya_warn("glyph atlas for '%s' is full at %d glyphs; raise NYA_RENDER2D_GLYPH_CAPACITY", atlas->path, NYA_RENDER2D_GLYPH_CAPACITY);
        return nullptr;
    }

    NYA_Asset* asset = nya_asset_get(atlas->handle);
    if (asset == nullptr || asset->status != NYA_ASSET_STATUS_LOADED || asset->as_font.font == nullptr) return nullptr;

    u32 slot = atlas->glyph_count++;

    atlas->codepoints[slot] = codepoint;
    _nya_render2d_glyph_bake(atlas, asset->as_font.font, codepoint, slot);

    return &atlas->glyphs[slot];
}

void _nya_render2d_atlas_upload(NYA_Window* window, NYA_FontAtlas* atlas) {
    if (!atlas->upload_pending) return;
    if (atlas->texture == nullptr || atlas->surface == nullptr) return;

    SDL_GPUDevice* gpu_device = nya_app_get()->render_system.gpu_device;

    u32 upload_size = (u32)(atlas->atlas_width * atlas->atlas_height * 4);

    void* mapped = SDL_MapGPUTransferBuffer(gpu_device, atlas->transfer_buffer, false);
    nya_memcpy(mapped, atlas->surface->pixels, upload_size);
    SDL_UnmapGPUTransferBuffer(gpu_device, atlas->transfer_buffer);

    /*
     * This runs inside a render pass — the first frame that draws text, and every later frame that
     * draws a character nobody has drawn before — and a copy pass cannot open while one is. Hence
     * the suspend, the same way a vertex flush does it.
     *
     * Uploading on its own command buffer instead would work and would avoid breaking the pass, but
     * it would also mean a second submission and a fence to know when it landed. This happens a
     * handful of times per run, so the suspend is cheaper in every sense.
     */
    b8 borrowed_pass = window != nullptr && window->render_system.render_pass != nullptr;
    if (borrowed_pass) _nya_render2d_pass_suspend(window);

    SDL_GPUCommandBuffer* command_buffer = borrowed_pass ? window->render_system.render_commands : SDL_AcquireGPUCommandBuffer(gpu_device);

    SDL_GPUCopyPass* copy_pass = SDL_BeginGPUCopyPass(command_buffer);
    SDL_UploadToGPUTexture(
        copy_pass, &(SDL_GPUTextureTransferInfo){ .transfer_buffer = atlas->transfer_buffer, .offset = 0 },
        &(SDL_GPUTextureRegion){ .texture = atlas->texture, .w = (u32)atlas->atlas_width, .h = (u32)atlas->atlas_height, .d = 1 }, false
    );
    SDL_EndGPUCopyPass(copy_pass);

    if (borrowed_pass) {
        _nya_render2d_pass_resume(window);
    } else {
        SDL_SubmitGPUCommandBuffer(command_buffer);
    }

    atlas->upload_pending = false;
}
