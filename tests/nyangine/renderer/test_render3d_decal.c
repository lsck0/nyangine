/**
 * Decals on the CPU: draping onto a probe, staging, the remembered grids, the ceiling and the release.
 **/

#include "nyangine/nyangine.c"
#include "nyangine/nyangine.h"

#include "SDL3/SDL_init.h"

/** Probes answered, so a test can tell a remembered grid from a draped one. */
static u32 probe_calls = 0;

/** Flat ground at y = 0, with nothing past x = 1, where a decal must be cut off. */
static b8 flat_ground_probe(f32x3 origin, f32x3 direction, void* user_data, OUT f32x3* out_point, OUT f32x3* out_normal) {
  nya_unused(user_data);
  probe_calls++;

  if (origin.x > 1.0F || origin.y < 0.0F || origin.y + direction.y > 0.0F) return false;

  *out_point  = (f32x3){ origin.x, 0.0F, origin.z };
  *out_normal = (f32x3){ 0.0F, 1.0F, 0.0F };
  return true;
}

static NYA_Render3DDecal decal_at(f32 x) {
  return (NYA_Render3DDecal){
    .texture = "decal_texture",
    .center  = { x, 0.2F, 0.0F },
    .size    = { 1.0F, 1.0F, 1.0F },
    .color   = NYA_COLOR_WHITE,
    .columns = 2,
    .rows    = 2,
    .cell    = 3,
  };
}

s32 main(void) {
  SDL_SetHintWithPriority(SDL_HINT_AUDIO_DRIVER, "dummy", SDL_HINT_OVERRIDE);

  _NYA_APP_INSTANCE = (NYA_App){ .initialized = true };
  b8 sdl_ok         = SDL_Init(0);
  nya_assert(sdl_ok, "SDL_Init failed: %s", SDL_GetError());

  // the setters queue their pipelines through the asset registry.
  nya_system_callback_init();
  NYA_EXPECT(nya_system_events_init());
  nya_system_asset_init();

  defer nya_system_asset_deinit();
  defer nya_system_events_deinit();
  defer nya_system_callback_deinit();

  NYA_Window window = { .screen_width = 320, .screen_height = 200 };

  NYA_Render3DDecalsGPU* decals = &window.render_system.decals_gpu;

  nya_render3d_begin(&window, (NYA_Camera3DPerspective){ .position = { 0.0F, 5.0F, 5.0F } });

  // ── Off, a decal is ignored and nothing is allocated.
  {
    nya_render3d_decal_probe_set(&window, nya_callback(flat_ground_probe), nullptr);
    nya_render3d_decal(&window, decal_at(0.0F));

    nya_check(decals->count == 0 && decals->arena == nullptr, "a decal while decals are off should cost nothing");
    nya_check(probe_calls == 0, "a decal while decals are off should not probe");
  }

  nya_render3d_decals_set(&window, (NYA_Render3DDecals){ .enabled = true, .lift = -1.0F });
  nya_check(nya_render3d_decals(&window).lift == 0.0F, "a negative lift should clamp to zero");

  // ── A decal drapes onto the ground, lifted, with its sheet cell's uvs.
  {
    nya_render3d_decal(&window, decal_at(0.0F));

    nya_check(decals->count == 1, "one decal should be staged, got %u", decals->count);
    nya_check(probe_calls == NYA_RENDER3D_DECAL_VERTICES, "every grid vertex should probe once, got %u", probe_calls);

    for (u32 i = 0; i < NYA_RENDER3D_DECAL_VERTICES; i++) {
      NYA_Vertex3D vertex = decals->vertices[i];

      nya_check(fabsf(vertex.position[1] - NYA_RENDER3D_DECAL_LIFT) < 0.0001F, "vertex %u should sit a lift above the ground, got %f", i,
                (f64)vertex.position[1]);
      nya_check(vertex.uv[0] >= 0.5F && vertex.uv[1] >= 0.5F, "cell 3 of a 2x2 sheet is the lower right quarter");
    }

    // the corners of the grid span the box and the whole cell.
    NYA_Vertex3D first = decals->vertices[0];
    NYA_Vertex3D last  = decals->vertices[NYA_RENDER3D_DECAL_VERTICES - 1];

    nya_check(first.position[0] == -0.5F && last.position[0] == 0.5F, "the grid should span the box across x");
    nya_check(first.uv[0] == 0.5F && last.uv[0] == 1.0F && last.uv[1] == 1.0F, "the grid should span the cell");
  }

  // ── The same box is remembered, not probed again, until the probe is set again.
  {
    u32 calls = probe_calls;

    nya_render3d_decal(&window, decal_at(0.0F));
    nya_check(probe_calls == calls, "a remembered box should not probe again");

    nya_render3d_decal_probe_set(&window, nya_callback(flat_ground_probe), nullptr);
    nya_render3d_decal(&window, decal_at(0.0F));
    nya_check(probe_calls == calls + NYA_RENDER3D_DECAL_VERTICES, "setting the probe again should forget the grids");
  }

  // ── Where the probe finds nothing the normal is zero, which the shader cuts the decal at.
  {
    u32 base = decals->count * NYA_RENDER3D_DECAL_VERTICES;

    nya_render3d_decal(&window, decal_at(1.0F));

    u32 landed = 0;
    for (u32 i = 0; i < NYA_RENDER3D_DECAL_VERTICES; i++) {
      const NYA_Vertex3D* vertex = &decals->vertices[base + i];
      if (vertex->normals[1] > 0.0F) landed++;
    }

    // the grid's columns at x = 0.5, 0.75 and 1.0 land; the two past the edge do not.
    nya_check(landed == 3 * (NYA_RENDER3D_DECAL_GRID + 1), "only the columns over ground should land, got %u", landed);
  }

  // ── Nothing is staged outside a scene, and past the ceiling a decal is dropped and counted.
  {
    u32 staged = decals->count;

    nya_render3d_end(&window);
    nya_render3d_decal(&window, decal_at(0.0F));
    nya_render3d_begin(&window, (NYA_Camera3DPerspective){ .position = { 0.0F, 5.0F, 5.0F } });

    nya_check(decals->count == staged, "outside a scene no decal should be staged");

    while (decals->frame_count < NYA_RENDER3D_DECAL_MAX) nya_render3d_decal(&window, decal_at(0.0F));

    nya_render3d_decal(&window, decal_at(0.0F));
    nya_check(nya_render3d_frame_stats(&window).dropped_draws == 1, "a decal past the ceiling should be counted as dropped");
  }

  // ── Switching decals off releases what they held and keeps the probe.
  {
    nya_render3d_decals_set(&window, (NYA_Render3DDecals){ 0 });

    nya_check(decals->arena == nullptr && decals->vertices == nullptr && decals->count == 0, "off should release the staging");
    nya_check(decals->probe != 0, "off should keep the probe");
  }

  return nya_check_failures() == 0 ? 0 : 1;
}
