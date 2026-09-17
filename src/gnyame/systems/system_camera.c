/**
 * @file system_camera.c
 *
 * Renders the world once per camera entity: insets into their textures, the primary into the window,
 * then the insets composited on top.
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

    if (nya_render_texture_is_current(&view->target, width, height)) return true;

    // Recreated rather than resized, because a GPU texture has no resize, and after a change of MSAA. Freeing first
    // matters: a viewport that animates would otherwise leak one target per frame.
    nya_render_texture_destroy(&view->target);

    view->target = nya_render_texture_create(window, width, height);

    return view->target.texture != nullptr;
}

void _gny_camera_render_primary(NYA_Window* window, NYA_Camera2DTopDown camera) {
    GNY_World* world = gny_world();

    nya_perf_time_this_scope("gny_world_pass");

    // always through the offscreen target, bloom or not: the light pass multiplies whatever is in the target, and
    // drawing straight to the window would darken the background layer behind the world too, so toggling bloom
    // changed the brightness of the whole screen. Only fails without a target size, and the world still draws.
    // no depth: nothing in the 2D world tests it, and at 4x it is 14 MB.
    world->post.scene = (NYA_RenderTextureOptions){ .depth = NYA_RENDER_TEXTURE_DEPTH_NONE };

    if (!nya_post_begin(window, &world->post)) {
        gny_world_draw(window, camera);
        return;
    }

    gny_world_draw(window, camera);

    NYA_PostPass passes[2] = { 0 };
    u32          pass_count = 0;

    if (world->bloom_enabled) {
        passes[pass_count++] = (NYA_PostPass){
            .pipeline = GNY_PIPELINE_BLOOM,
            .uniform =
                &(NYA_ShaderBloomUniform){
                    // the 2D world's numbers; the 3D scene runs the same pipeline with its own. See GNY_BLOOM_2D_THRESHOLD.
                    .texel_x   = GNY_BLOOM_2D_SPREAD / (f32)world->post.width,
                    .texel_y   = GNY_BLOOM_2D_SPREAD / (f32)world->post.height,
                    .threshold = GNY_BLOOM_2D_THRESHOLD,
                    .intensity = GNY_BLOOM_2D_INTENSITY,
                },
            .uniform_size = sizeof(NYA_ShaderBloomUniform),
        };
    }

    // greyed out behind the pause menu. with bloom on that is two passes, and only then does the chain hold a second
    // target.
    if (nya_layer_get(GNY_WINDOW_MAIN, GNY_LAYER_PAUSE_MENU_ID) != nullptr) passes[pass_count++] = (NYA_PostPass){ .pipeline = GNY_PIPELINE_GRAYSCALE };

    // zero passes puts the captured world back on the window unchanged.
    nya_post_end(window, &world->post, passes, pass_count);
}
