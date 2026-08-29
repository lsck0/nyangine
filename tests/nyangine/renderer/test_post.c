/**
 * The post-processing chain's own bookkeeping: target lifetime, the ping-pong, and the fallbacks.
 *
 * Headless, so nothing is actually rasterised — nya_render_texture_create returns a stub. What is
 * testable here is everything around the draw: that a chain sizes itself to the window, that it
 * recreates rather than leaks on a resize, that begin refuses a zero-sized window instead of
 * asserting, and that destroy is safe to call twice. Those are the parts that had been hand written
 * twice in the game and were the ones that went wrong.
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

    return nya_check_failures() == 0 ? 0 : 1;
}
