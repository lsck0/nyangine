#include "gnyame/gnyame.h"

#include "assets/assets.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * ON CREATE
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

void gny_layer_game_on_create(NYA_Window* window) {
    NYA_EXPECT(nya_asset_load((NYA_AssetLoadParameters){
      .type   = NYA_ASSET_TYPE_SHADER_VERTEX,
      .handle = NYA_ASSET_SHADER_SAMPLE_VERT,
      .as_shader = {
          .num_uniform_buffers = 1,
      },
  }), "while queueing the sample vertex shader");

    NYA_EXPECT(nya_asset_load((NYA_AssetLoadParameters){
      .type   = NYA_ASSET_TYPE_SHADER_FRAGMENT,
      .handle = NYA_ASSET_SHADER_SAMPLE_FRAG,
      .as_shader = {
          .num_uniform_buffers = 1,
      },
  }), "while queueing the sample fragment shader");

    NYA_EXPECT(nya_asset_load((NYA_AssetLoadParameters){
      .type   = NYA_ASSET_TYPE_GRAPHICS_PIPELINE,
      .handle = "sample_pipeline",
      .as_graphics_pipeline = {
          .window                 = window,
          .vertex_shader_handle   = NYA_ASSET_SHADER_SAMPLE_VERT,
          .fragment_shader_handle = NYA_ASSET_SHADER_SAMPLE_FRAG,
      },
  }), "while queueing the sample pipeline");
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * ON DESTROY
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

void gny_layer_game_on_destroy(NYA_Window* window) {
    nya_unused(window);
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * ON EVENT
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

void gny_layer_game_on_event(NYA_Window* window, NYA_Event* event) {
    nya_unused(window, event);
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * ON UPDATE
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

void gny_layer_game_on_update(NYA_Window* window, f32 delta_time_s) {
    nya_unused(window, delta_time_s);
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * ON RENDER
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

void gny_layer_game_on_render(NYA_Window* window) {

    NYA_Asset* gp_asset = nya_asset_get("sample_pipeline");
    nya_asset_with(gp_asset) {
        SDL_BindGPUGraphicsPipeline(window->render_system.render_pass, gp_asset->as_graphics_pipeline.pipeline);

        // The app's own clock, not a timestamp captured in a global here.
        //
        // A DLL global is reinitialised every time the library is reloaded, so start_time went back
        // to zero on each hot reload and the elapsed time jumped to the whole unix epoch — which the
        // shader saw as one enormous constant, and the triangle stopped moving. NYA_FrameStats lives
        // in NYA_App, which the executable owns and no reload touches.
        //
        // frame_stats.uptime_ns rather than nya_app_uptime_ns: it is sampled once at the top of the
        // frame, so every layer drawing this frame agrees on what time it is.
        f32 now = (f32)nya_time_ns_to_s(nya_app_get()->frame_stats.uptime_ns);
        SDL_PushGPUVertexUniformData(window->render_system.render_commands, 0, &now, sizeof(now));
        SDL_PushGPUFragmentUniformData(window->render_system.render_commands, 0, &now, sizeof(now));

        SDL_DrawGPUPrimitives(window->render_system.render_pass, 3, 1, 0, 0);
    }
}
