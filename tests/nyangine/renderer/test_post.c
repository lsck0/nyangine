/**
 * The post-processing chain's own bookkeeping: target lifetime, the ping-pong, and the fallbacks.
 **/

#include "nyangine/nyangine.c"
#include "nyangine/nyangine.h"

#include "SDL3/SDL_init.h"

s32 main(void) {
    SDL_SetHintWithPriority(SDL_HINT_AUDIO_DRIVER, "dummy", SDL_HINT_OVERRIDE);

    _NYA_APP_INSTANCE = (NYA_App){ .initialized = true };
    b8 sdl_ok         = SDL_Init(0);
    nya_assert(sdl_ok, "SDL_Init failed: %s", SDL_GetError());

    // The asset registry has to be up: nya_post_end asks nya_asset_status whether each pass's
    // pipeline finished loading, and that reads the registry's dict.
    nya_system_callback_init();
    NYA_EXPECT(nya_system_events_init());
    nya_system_asset_init();

    defer nya_system_asset_deinit();
    defer nya_system_events_deinit();
    defer nya_system_callback_deinit();

    // A window struct is all the chain reads: it wants a size and somewhere to draw.
    NYA_Window window = { .screen_width = 320, .screen_height = 200 };

    // ── A zeroed chain is a valid one, and destroying it is a no-op.
    {
        NYA_PostChain chain = { 0 };
        nya_post_chain_destroy(&chain);
        nya_check(chain.width == 0 && chain.height == 0, "a destroyed chain should be zeroed");
        nya_post_chain_destroy(&chain);
        nya_post_chain_destroy(nullptr);
    }

    // ── begin sizes the chain to the window.
    {
        NYA_PostChain chain = { 0 };
        defer         nya_post_chain_destroy(&chain);

        nya_check(nya_post_begin(&window, &chain), "a sized window should give a usable chain");
        nya_check(chain.width == 320 && chain.height == 200, "the chain should match the window, got %ux%u",
                  chain.width, chain.height);
        nya_check(chain.capturing, "begin should leave the chain capturing");

        nya_post_end(&window, &chain, nullptr, 0);
        nya_check(!chain.capturing, "end should stop the capture");
    }

    // ── A resize rebuilds the pair rather than keeping a stale size.
    {
        NYA_PostChain chain = { 0 };
        defer         nya_post_chain_destroy(&chain);

        nya_check(nya_post_begin(&window, &chain), "first frame");
        nya_post_end(&window, &chain, nullptr, 0);

        window.screen_width  = 640;
        window.screen_height = 480;

        nya_check(nya_post_begin(&window, &chain), "after a resize");
        nya_check(chain.width == 640 && chain.height == 480, "the chain should follow the window, got %ux%u",
                  chain.width, chain.height);
        nya_post_end(&window, &chain, nullptr, 0);

        window.screen_width  = 320;
        window.screen_height = 200;
    }

    // ── A zero-sized window is refused rather than asserted on. This is the branch the caller relies
    //    on to draw straight to the window while minimised or mid-resize.
    {
        NYA_PostChain chain = { 0 };
        defer         nya_post_chain_destroy(&chain);

        NYA_Window minimised = { .screen_width = 0, .screen_height = 0 };
        nya_check(!nya_post_begin(&minimised, &chain), "a zero-sized window should be refused");
        nya_check(!chain.capturing, "a refused begin must not leave the chain capturing");

        // And end on a chain that never began is a no-op rather than an unbalanced render pass.
        nya_post_end(&minimised, &chain, nullptr, 0);
    }

    // ── end without begin does nothing.
    {
        NYA_PostChain chain = { 0 };
        nya_post_end(&window, &chain, nullptr, 0);
        nya_check(!chain.capturing, "end on a fresh chain should stay not-capturing");
        nya_post_chain_destroy(&chain);
    }

    // ── Passes naming a pipeline that is not loaded are skipped, and the scene is still put back.
    //    This is the case that once cost the entire 3D scene on Windows.
    {
        NYA_PostChain chain = { 0 };
        defer         nya_post_chain_destroy(&chain);

        nya_check(nya_post_begin(&window, &chain), "begin");
        nya_post_end(&window, &chain,
                     (NYA_PostPass[]){
                         { .pipeline = "no_such_pipeline" },
                         { .pipeline = nullptr },
                     },
                     2);
        nya_check(!chain.capturing, "end should complete even when every pass is unusable");
    }

    // ── The scene target follows the caller's options, and a one pass chain holds no second target.
    {
        NYA_PostChain chain = { .scene = { .depth = NYA_RENDER_TEXTURE_DEPTH_NONE } };
        defer         nya_post_chain_destroy(&chain);

        nya_check(nya_post_begin(&window, &chain), "begin");
        nya_check(chain.targets[0].options.depth == NYA_RENDER_TEXTURE_DEPTH_NONE, "the scene target should be made as the chain asks");
        nya_post_end(&window, &chain, (NYA_PostPass[]){ { .pipeline = "no_such_pipeline" } }, 1);
        nya_check(chain.targets[1].width == 0, "no pass ran between targets, so there should be no second one");

        // a 3D scene after a 2D one asks for depth back, which rebuilds the target at the same size.
        chain.scene = (NYA_RenderTextureOptions){ .depth = NYA_RENDER_TEXTURE_DEPTH_ATTACHED };
        nya_check(nya_post_begin(&window, &chain), "begin with depth");
        nya_check(chain.targets[0].options.depth == NYA_RENDER_TEXTURE_DEPTH_ATTACHED, "a changed depth option should rebuild the scene target");
        nya_post_end(&window, &chain, nullptr, 0);

        nya_post_chain_destroy(&chain);
        nya_check(chain.scene.depth == NYA_RENDER_TEXTURE_DEPTH_ATTACHED, "destroying a chain should keep what the caller asked for");
    }

    // ── A render texture knows what it was made with, and whether it still fits.
    {
        NYA_RenderTexture target = nya_render_texture_create_with(&window, 64, 32, (NYA_RenderTextureOptions){ .single_sampled = true });
        defer             nya_render_texture_destroy(&target);

        nya_check(target.options.single_sampled, "the options should be kept on the texture");
        nya_check(nya_render_texture_is_current(&target, 64, 32), "a texture of the asked for size should be current");
        nya_check(!nya_render_texture_is_current(&target, 64, 64), "a resized window should make it stale");
    }

    // ── Render options are stored for the next frame, and a pipeline that never loaded has no build.
    {
        nya_render_options_set(&window, (NYA_RenderOptions){ .msaa_samples = 2 });
        nya_check(nya_app_get()->render_system.options.msaa_samples == 2, "the request should wait for the next nya_render_begin");
        nya_render_options_set(&window, (NYA_RenderOptions){ 0 });

        nya_check(nya_asset_graphics_pipeline(nya_asset_get("no_such_pipeline"), SDL_GPU_SAMPLECOUNT_4) == nullptr, "nothing loaded, nothing to bind");
    }

    return nya_check_failures() == 0 ? 0 : 1;
}
