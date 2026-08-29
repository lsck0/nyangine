/**
 * @file system_camera.c
 *
 * Drawing the world once per camera, and putting the results where they belong.
 *
 * Order matters and is the whole of what this file decides:
 *
 * 1. every secondary camera renders the world into its own texture,
 * 2. the primary camera renders the world into the window, through the bloom pass,
 * 3. every secondary texture is composited into its viewport, on top.
 *
 * Secondaries go first because a render texture cannot be bound while the window's pass is open —
 * beginning one mid-frame ends the pass the primary was drawing into. Doing them all up front means
 * one target switch each rather than one per composite.
 * */
#include "gnyame/gnyame.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API DECLARATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/** Creates or resizes a secondary camera's target to match its viewport. False when it has none. */
NYA_INTERNAL b8 _gny_camera_target_ensure(NYA_Window* window, GNY_CameraView* view);

/** The primary camera's pass: the world into the window, through the bloom pipeline if it is on. */
NYA_INTERNAL void _gny_camera_render_primary(NYA_Window* window, NYA_Camera2DTopDown camera);

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PUBLIC API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

void gny_system_camera_render(NYA_Window* window) {
    nya_perf_time_this_function();

    /*
     * ── 1. Secondaries, into their own targets ──
     */
    nya_entity_foreach_kind (GNY_ENTITY_CAMERA, entity) {
        if (gny_entity_flag_check(entity, GNY_ENTITY_FLAG_CAMERA_PRIMARY)) continue;

        GNY_CameraView* view = gny_entity_camera_view(entity);
        if (view == nullptr) continue;
        if (!_gny_camera_target_ensure(window, view)) continue;

        // Opaque, unlike the primary's scene target: this one is composited as a panel over the
        // finished frame rather than over the background layer, so it wants its own backdrop.
        nya_render_texture_begin(window, &view->target, GNY_CAMERA_VIEW_CLEAR);
        gny_world_draw(window, gny_entity_camera_of(entity));
        nya_render_texture_end(window);
    }

    /*
     * ── 2. The primary, filling the window ──
     */
    _gny_camera_render_primary(window, gny_entity_camera_get());

    /*
     * ── 3. Secondaries composited on top ──
     */
    nya_entity_foreach_kind (GNY_ENTITY_CAMERA, entity) {
        if (gny_entity_flag_check(entity, GNY_ENTITY_FLAG_CAMERA_PRIMARY)) continue;

        GNY_CameraView* view = gny_entity_camera_view(entity);
        if (view == nullptr || view->target.texture == nullptr) continue;

        nya_render2d_render_texture(window, &view->target, view->viewport.x, view->viewport.y, view->viewport.width, view->viewport.height,
                                NYA_COLOR_WHITE);

        // A border, so the inset reads as a separate view rather than as part of the world behind it.
        nya_render2d_rect_outline(window, view->viewport.x, view->viewport.y, view->viewport.width, view->viewport.height, GNY_CAMERA_VIEW_BORDER_WIDTH,
                              GNY_CAMERA_VIEW_BORDER);
    }
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

b8 _gny_camera_target_ensure(NYA_Window* window, GNY_CameraView* view) {
    u32 width  = (u32)view->viewport.width;
    u32 height = (u32)view->viewport.height;

    if (width == 0 || height == 0) return false;

    if (view->target.texture != nullptr && view->target.width == width && view->target.height == height) return true;

    // Recreated rather than resized, because a GPU texture has no resize. Freeing first matters: a
    // viewport that animates would otherwise leak one target per frame.
    if (view->target.texture != nullptr) nya_render_texture_destroy(&view->target);

    view->target = nya_render_texture_create(window, width, height);

    return view->target.texture != nullptr;
}

void _gny_camera_render_primary(NYA_Window* window, NYA_Camera2DTopDown camera) {
    GNY_World* world = gny_world();

    if (!world->bloom_enabled) {
        // Straight to the window, no second pass and no offscreen target. What this did before there
        // was a post process, and the fallback if the pipeline failed to build.
        gny_world_draw(window, camera);
        return;
    }

    nya_perf_time_this_scope("gny_bloom_pass");

    // Returns false when the window has no size to make a target of; the world still has to be drawn.
    if (!nya_post_begin(window, &world->post)) {
        gny_world_draw(window, camera);
        return;
    }

    gny_world_draw(window, camera);

    nya_post_end(
        window, &world->post,
        (NYA_PostPass[]){
            {
                .pipeline = GNY_PIPELINE_BLOOM,
                .uniform =
                    &(NYA_ShaderBloomUniform){
                        // The 2D world's own numbers. The 3D scene runs the same pipeline with its own
                        // set, because the two are nowhere near equally bright; see GNY_BLOOM_2D_THRESHOLD.
                        .texel_x   = GNY_BLOOM_2D_SPREAD / (f32)world->post.width,
                        .texel_y   = GNY_BLOOM_2D_SPREAD / (f32)world->post.height,
                        .threshold = GNY_BLOOM_2D_THRESHOLD,
                        .intensity = GNY_BLOOM_2D_INTENSITY,
                    },
                .uniform_size = sizeof(NYA_ShaderBloomUniform),
            },
        },
        1
    );
}
