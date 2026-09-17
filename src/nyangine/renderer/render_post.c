#include "nyangine/nyangine.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * INTERNALS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/**
 * Rebuilds the scene target if the window size or the renderer's sample count changed. False when there is
 * nothing usable to draw into.
 */
NYA_INTERNAL b8 _nya_post_targets_ensure(NYA_Window* window, NYA_PostChain* chain) {
    const u32 width  = window->screen_width;
    const u32 height = window->screen_height;

    // Minimised or mid resize. The GPU will not make a target of no size, and the caller falls back
    // to drawing straight to the window.
    if (width == 0 || height == 0) return false;

    if (nya_render_texture_is_current(&chain->targets[0], width, height) && chain->targets[0].options.depth == chain->scene.depth) return true;

    nya_render_texture_destroy(&chain->targets[0]);
    nya_render_texture_destroy(&chain->targets[1]);

    // the scene multisampled like the window, so a 3D scene occludes itself and its edges are smoothed as they would
    // be on the swapchain.
    chain->targets[0] = nya_render_texture_create_with(window, width, height, chain->scene);
    chain->width      = width;
    chain->height     = height;

    return true;
}

/**
 * Makes the between passes target when `needed` and releases it when not. Single sampled and without depth: it only
 * takes fullscreen quads, which have no edges to smooth and nothing to occlude.
 */
NYA_INTERNAL void _nya_post_intermediate_ensure(NYA_Window* window, NYA_PostChain* chain, b8 needed) {
    if (!needed) {
        nya_render_texture_destroy(&chain->targets[1]);
        return;
    }

    if (nya_render_texture_is_current(&chain->targets[1], chain->width, chain->height)) return;

    nya_render_texture_destroy(&chain->targets[1]);
    chain->targets[1] = nya_render_texture_create_with(
        window,
        chain->width,
        chain->height,
        (NYA_RenderTextureOptions){ .depth = NYA_RENDER_TEXTURE_DEPTH_NONE, .single_sampled = true }
    );
}

/** Draws `source` over the whole window through `pass`, into whatever target is currently bound. */
NYA_INTERNAL void _nya_post_draw_pass(NYA_Window* window, const NYA_RenderTexture* source, const NYA_PostPass* pass) {
    // Zeroed alpha means the caller left `tint` unset, which should be opaque white rather than invisible.
    NYA_Color tint = pass->tint;
    if (tint.a == 0) tint = NYA_COLOR_WHITE;

    nya_render2d_shader_begin(window, pass->pipeline);

    if (pass->uniform != nullptr && pass->uniform_size > 0) {
        nya_render2d_shader_set_uniform(window, pass->uniform, pass->uniform_size);
    }

    nya_render2d_render_texture(window, source, 0.0F, 0.0F, (f32)window->screen_width, (f32)window->screen_height, tint);

    nya_render2d_shader_end(window);
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PUBLIC API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

b8 nya_post_begin(NYA_Window* window, NYA_PostChain* chain) {
    nya_assert(window != nullptr);
    nya_assert(chain != nullptr);

    chain->capturing = false;

    if (!_nya_post_targets_ensure(window, chain)) return false;

    chain->scene_index = 0;
    chain->capturing   = true;

    // Transparent, not a colour: whatever was drawn to the window before this is underneath and the
    // chain composites over it. See the header.
    nya_render_texture_begin(window, &chain->targets[chain->scene_index], NYA_COLOR_TRANSPARENT);

    return true;
}

void nya_post_end(NYA_Window* window, NYA_PostChain* chain, const NYA_PostPass* passes, u32 pass_count) {
    nya_assert(window != nullptr);
    nya_assert(chain != nullptr);

    if (!chain->capturing) return;

    nya_render_texture_end(window);
    chain->capturing = false;

    /*
     * A pass whose pipeline has not finished loading is skipped rather than drawn.
     */
    // Cast because the asset API takes a mutable handle while only reading it; every call site in the
    // tree does the same. See nya_render2d_texture.
    u32 usable = 0;
    for (u32 i = 0; i < pass_count; i++) {
        if (passes[i].pipeline != nullptr && nya_asset_status((NYA_CString)passes[i].pipeline) == NYA_ASSET_STATUS_LOADED) usable++;
    }

    // only a chain of two or more passes has a between, and one pass costs no second target.
    _nya_post_intermediate_ensure(window, chain, usable > 1);

    // Nothing to run: put the captured scene back on the window so the frame is not simply lost.
    if (usable == 0) {
        nya_render2d_render_texture(window, &chain->targets[chain->scene_index], 0.0F, 0.0F, (f32)window->screen_width,
                                    (f32)window->screen_height, NYA_COLOR_WHITE);
        return;
    }

    u32 source = chain->scene_index;
    u32 run    = 0;

    for (u32 i = 0; i < pass_count; i++) {
        const NYA_PostPass* pass = &passes[i];

        if (pass->pipeline == nullptr || nya_asset_status((NYA_CString)pass->pipeline) != NYA_ASSET_STATUS_LOADED) continue;

        run++;

        // The last surviving pass draws to the window; the rest ping-pong into the other target.
        if (run == usable) {
            _nya_post_draw_pass(window, &chain->targets[source], pass);
            return;
        }

        const u32 destination = source ^ 1U;

        nya_render_texture_begin(window, &chain->targets[destination], NYA_COLOR_TRANSPARENT);
        _nya_post_draw_pass(window, &chain->targets[source], pass);
        nya_render_texture_end(window);

        source = destination;
    }
}

void nya_post_chain_destroy(NYA_PostChain* chain) {
    if (chain == nullptr) return;

    for (u32 i = 0; i < nya_carray_length(chain->targets); i++) nya_render_texture_destroy(&chain->targets[i]);

    // the scene options are the caller's choice, not state, so they survive.
    *chain = (NYA_PostChain){ .scene = chain->scene };
}
