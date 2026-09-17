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
                         { .pipeline = "no_such_pipeline", .texture = "no_such_table" },
                     },
                     3);
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

        nya_check(nya_asset_graphics_pipeline(nya_asset_get("no_such_pipeline"), SDL_GPU_SAMPLECOUNT_4, false) == nullptr, "nothing loaded, nothing to bind");
    }
    // ── The cartoon options: zero is off, and out of range values from a config file are clamped.
    {
        NYA_PostInk ink = nya_post_ink(&window);
        nya_check(!ink.enabled && ink.width == 0.0F, "a fresh window has no ink");

        nya_post_ink_set(&window, (NYA_PostInk){ .enabled = true, .width = 400.0F, .crease = -3.0F, .color = { 2.0F, -1.0F, 0.5F, 1.0F } });
        ink = nya_post_ink(&window);
        nya_check(ink.enabled && ink.width == 16.0F && ink.crease == 0.0F, "ink width and crease clamp, got %f and %f", (f64)ink.width,
                  (f64)ink.crease);
        nya_check(ink.color.r == 1.0F && ink.color.g == 0.0F && ink.color.b == 0.5F, "ink colour clamps per channel");

        nya_post_ambient_occlusion_set(&window, (NYA_PostAmbientOcclusion){ .enabled = true, .strength = 3.0F, .band = -1.0F });
        NYA_PostAmbientOcclusion occlusion = nya_post_ambient_occlusion(&window);
        nya_check(occlusion.strength == 1.0F && occlusion.band == 0.0F, "occlusion strength and band clamp to [0, 1]");

        nya_post_antialias_set(&window, (NYA_PostAntialias){ .enabled = true, .subpixel = 5.0F });
        nya_check(nya_post_antialias(&window).subpixel == 1.0F, "the subpixel amount clamps to one");

        nya_post_depth_of_field_set(&window, (NYA_PostDepthOfField){ .focus = (NYA_PostFocus)7, .radius = 400.0F, .band_offset = -3.0F });
        NYA_PostDepthOfField depth_of_field = nya_post_depth_of_field(&window);
        nya_check(depth_of_field.focus == NYA_POST_FOCUS_OFF, "an unknown focus reads as off");
        nya_check(depth_of_field.radius == NYA_POST_DEPTH_OF_FIELD_RADIUS_MAX && depth_of_field.band_offset == -0.5F, "radius and band offset clamp");

        nya_post_speed_lines_set(&window, (NYA_PostSpeedLines){ .amount = 3.0F, .center_x = -2.0F, .density = -4.0F });
        NYA_PostSpeedLines lines = nya_post_speed_lines(&window);
        nya_check(lines.amount == 1.0F && lines.center_x == -0.5F && lines.density == 0.0F, "speed lines clamp");
        nya_post_speed_lines_set(&window, (NYA_PostSpeedLines){ 0 });

        // a headless window has no swapchain, so HDR is kept as asked and never presented.
        nya_render_output_set(&window, (NYA_RenderOutput){ .hdr = true, .peak = 4.0F });
        nya_check(nya_render_output(&window).hdr && nya_render_output(&window).peak == 4.0F, "the output reads back as given");
        nya_check(!nya_render_output_hdr_active(&window), "a headless window never presents in HDR");

        nya_post_debug_view_set(&window, (NYA_PostDebugView)99);
        nya_check(nya_post_debug_view(&window) == NYA_POST_DEBUG_VIEW_NONE, "an unknown debug view reads as none");

        nya_post_debug_view_set(&window, NYA_POST_DEBUG_VIEW_CASCADES);
        nya_check(nya_post_debug_view(&window) == NYA_POST_DEBUG_VIEW_CASCADES, "a known view is kept");
    }

    // ── Targets follow the options: the normal buffer and the half resolution occlusion exist only while needed.
    {
        NYA_PostChain chain = { 0 };
        defer         nya_post_chain_destroy(&chain);

        window.render_system.post_ink               = (NYA_PostInk){ 0 };
        window.render_system.post_ambient_occlusion = (NYA_PostAmbientOcclusion){ 0 };
        window.render_system.post_antialias         = (NYA_PostAntialias){ 0 };
        window.render_system.post_debug_view        = NYA_POST_DEBUG_VIEW_NONE;

        nya_check(nya_post_begin(&window, &chain), "all off");
        nya_post_end(&window, &chain, nullptr, 0);
        nya_check(!chain.targets[0].options.normals && chain.half.width == 0, "nothing on means no normal buffer and no half target");

        // antialiasing reads only the image.
        nya_post_antialias_set(&window, (NYA_PostAntialias){ .enabled = true });
        nya_check(nya_post_begin(&window, &chain), "antialias on");
        nya_post_end(&window, &chain, nullptr, 0);
        nya_check(!chain.targets[0].options.normals && chain.half.width == 0, "antialiasing alone needs neither buffer");

        nya_post_ink_set(&window, (NYA_PostInk){ .enabled = true });
        nya_check(nya_post_begin(&window, &chain), "ink on");
        nya_post_end(&window, &chain, nullptr, 0);
        nya_check(chain.targets[0].options.normals && chain.half.width == 0, "ink needs the normal buffer and no half target");

        nya_post_ambient_occlusion_set(&window, (NYA_PostAmbientOcclusion){ .enabled = true });
        nya_check(nya_post_begin(&window, &chain), "occlusion on");
        nya_post_end(&window, &chain, nullptr, 0);
        nya_check(chain.targets[0].options.normals && chain.half.width == 160 && chain.half.height == 100, "occlusion adds a half target, got %ux%u",
                  chain.half.width, chain.half.height);

        nya_post_ink_set(&window, (NYA_PostInk){ 0 });
        nya_post_ambient_occlusion_set(&window, (NYA_PostAmbientOcclusion){ 0 });
        nya_check(nya_post_begin(&window, &chain), "both off again");
        nya_post_end(&window, &chain, nullptr, 0);
        nya_check(!chain.targets[0].options.normals && chain.half.width == 0, "turning them off releases both");

        // tilt shift reads only the image; distance focus reads the normal buffer. both blur at half resolution.
        nya_post_depth_of_field_set(&window, (NYA_PostDepthOfField){ .focus = NYA_POST_FOCUS_TILT_SHIFT });
        nya_check(nya_post_begin(&window, &chain), "tilt shift on");
        nya_post_end(&window, &chain, nullptr, 0);
        nya_check(!chain.targets[0].options.normals && chain.blur.width == 160 && chain.half.width == 0, "tilt shift adds only a blur target");

        nya_post_depth_of_field_set(&window, (NYA_PostDepthOfField){ .focus = NYA_POST_FOCUS_DISTANCE });
        nya_check(nya_post_begin(&window, &chain), "distance focus on");
        nya_post_end(&window, &chain, nullptr, 0);
        nya_check(chain.targets[0].options.normals && chain.blur.width == 160, "distance focus adds the normal buffer too");

        nya_post_depth_of_field_set(&window, (NYA_PostDepthOfField){ 0 });
        nya_check(nya_post_begin(&window, &chain), "depth of field off");
        nya_post_end(&window, &chain, nullptr, 0);
        nya_check(!chain.targets[0].options.normals && chain.blur.width == 0, "turning it off releases the blur and the normal buffer");

        nya_post_antialias_set(&window, (NYA_PostAntialias){ 0 });
    }

    return nya_check_failures() == 0 ? 0 : 1;
}
