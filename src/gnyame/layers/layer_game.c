#include "gnyame/gnyame.h"

u64 start_time;

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * ON CREATE
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

void gny_layer_game_on_create(NYA_Window* window) {
    start_time = nya_clock_get_timestamp_ms();

    nya_asset_load((NYA_AssetLoadParameters){
      .type   = NYA_ASSET_TYPE_SHADER_VERTEX,
      .handle = NYA_ASSET_SHADER_SAMPLE_VERT,
      .as_shader = {
          .num_uniform_buffers = 1,
      },
  });

    nya_asset_load((NYA_AssetLoadParameters){
      .type   = NYA_ASSET_TYPE_SHADER_FRAGMENT,
      .handle = NYA_ASSET_SHADER_SAMPLE_FRAG,
      .as_shader = {
          .num_uniform_buffers = 1,
      },
  });

    nya_asset_load((NYA_AssetLoadParameters){
      .type   = NYA_ASSET_TYPE_GRAPHICS_PIPELINE,
      .handle = "sample_pipeline",
      .as_graphics_pipeline = {
          .window                 = window,
          .vertex_shader_handle   = NYA_ASSET_SHADER_SAMPLE_VERT,
          .fragment_shader_handle = NYA_ASSET_SHADER_SAMPLE_FRAG,
      },
  });
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

        f32 now = (f32)nya_time_ms_to_s(nya_clock_get_timestamp_ms() - start_time);
        SDL_PushGPUVertexUniformData(window->render_system.render_commands, 0, &now, sizeof(now));
        SDL_PushGPUFragmentUniformData(window->render_system.render_commands, 0, &now, sizeof(now));

        SDL_DrawGPUPrimitives(window->render_system.render_pass, 3, 1, 0, 0);
    }
}
