#include "nyangine/nyangine.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * INTERNALS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/**
 * Rebuilds both targets if the window size changed. False when there is nothing usable to draw into.
 *
 * Recreated rather than resized, because a GPU texture has no resize, and the old one is destroyed
 * first — a window dragged to a new size produces one of these per frame.
 */
NYA_INTERNAL b8 _nya_post_targets_ensure(NYA_Window* window, NYA_PostChain* chain) {
    const u32 width  = window->screen_width;
    const u32 height = window->screen_height;

    // Minimised or mid resize. The GPU will not make a target of no size, and the caller falls back
    // to drawing straight to the window.
    if (width == 0 || height == 0) return false;

    /*
     * Keyed on the recorded size, not on targets[0].texture.
     *
     * A headless build has no GPU texture but does report an honest width and height, because that is
     * state game logic reads back. Testing the pointer would make every chain permanently unusable
     * there — and in a real build the pointer tells us nothing anyway, since nya_render_texture_create
     * asserts rather than returning a null texture.
     */
    if (chain->width == width && chain->height == height) return true;

    nya_post_chain_destroy(chain);

    chain->targets[0] = nya_render_texture_create(window, width, height);
    chain->targets[1] = nya_render_texture_create(window, width, height);
    chain->width      = width;
    chain->height     = height;

    return true;
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
     *
     * render2d silently drops a batch whose pipeline is not loaded, so drawing one anyway means the
     * scene goes into a target that is never blitted and the window shows nothing. That is not
     * hypothetical — it is what happened on Windows, where the asset load lands a frame later than on
     * Linux. Counting first means the last *surviving* pass is the one that reaches the window.
     */
    // Cast because the asset API takes a mutable handle while only reading it; every call site in the
    // tree does the same. See nya_render2d_texture.
    u32 usable = 0;
    for (u32 i = 0; i < pass_count; i++) {
        if (passes[i].pipeline != nullptr && nya_asset_status((NYA_CString)passes[i].pipeline) == NYA_ASSET_STATUS_LOADED) usable++;
    }

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

    *chain = (NYA_PostChain){ 0 };
}
