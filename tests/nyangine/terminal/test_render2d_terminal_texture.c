/**
 * Drawing a texture by its asset handle on a terminal, which used to be three no-ops.
 *
 * The reason given was sound at the time: a terminal has no sampler, and the pixels behind a handle
 * were nobody's to read because the loader handed them to the GPU and dropped them. A terminal build
 * creates no GPU device, so there is nothing to hand them to — the loader keeps them instead, and the
 * backend samples one texel per cell.
 **/

#include "nyangine-core/nyangine.c"
#include "nyangine-core/nyangine.h"

/** A cell well inside any grid, and the pixel it starts at. */
#define AT_COLUMN 2
#define AT_ROW    1
#define AT_X      ((f32)(AT_COLUMN * NYA_TERMINAL_CELL_WIDTH_PX))
#define AT_Y      ((f32)(AT_ROW * NYA_TERMINAL_CELL_HEIGHT_PX))

/** How many cells across and down the picture is drawn, so several cells sample it. */
#define DRAWN_COLUMNS 6
#define DRAWN_ROWS    4

#define DRAWN_WIDTH  ((f32)(DRAWN_COLUMNS * NYA_TERMINAL_CELL_WIDTH_PX))
#define DRAWN_HEIGHT ((f32)(DRAWN_ROWS * NYA_TERMINAL_CELL_HEIGHT_PX))

/** Cells whose background is not the cleared colour, over the drawn rectangle. */
static u32 painted_cells(u32 cleared) {
  u32 painted = 0;

  for (u16 row = AT_ROW; row < AT_ROW + DRAWN_ROWS; row++) {
    for (u16 column = AT_COLUMN; column < AT_COLUMN + DRAWN_COLUMNS; column++) {
      if (nya_terminal_cell_get(column, row).background != cleared) painted++;
    }
  }

  return painted;
}

s32 main(void) {
  _NYA_APP_INSTANCE = (NYA_App){ .initialized = true };

  nya_system_callback_init();
  NYA_EXPECT(nya_system_events_init());
  nya_system_asset_init();

  defer nya_system_asset_deinit();
  defer nya_system_events_deinit();
  defer nya_system_callback_deinit();

  if (!nya_terminal_open((NYA_TerminalOptions){ .detached = true }).ok) {
    nya_log_warn("No terminal here, skipping the texture tests.");

    return 0;
  }

  defer nya_terminal_close();

  NYA_Window* window = nya_render2d_terminal_window();
  nya_assert(window != nullptr, "the terminal backend has a window");

  const u32 cleared = nya_terminal_ink(0.0F, 0.0F, 0.0F);

  // TEST: the loader keeps the pixels where there is no device to upload them to
  {
    NYA_EXPECT(nya_asset_load((NYA_AssetLoadParameters){ .type = NYA_ASSET_TYPE_TEXTURE, .handle = NYA_ASSET_TEXTURES_DECALS_PNG }));

    NYA_Event ended = { .type = NYA_EVENT_FRAME_ENDED };
    _nya_asset_loading_process(&ended);

    const NYA_Asset* asset = nya_asset_get(NYA_ASSET_TEXTURES_DECALS_PNG);

    nya_check(asset != nullptr && asset->status == NYA_ASSET_STATUS_LOADED, "the texture loads without a GPU");

    if (asset != nullptr && asset->status == NYA_ASSET_STATUS_LOADED) {
      nya_check(asset->as_texture.texture == nullptr, "and uploaded nothing, since there is no device");
      nya_check(asset->as_texture.pixels != nullptr, "but kept the decoded pixels");
      nya_check(asset->as_texture.width > 0 && asset->as_texture.height > 0, "with a size, got %ux%u", asset->as_texture.width,
                asset->as_texture.height);
    }

    printf("  PASSED\n");
  }

  // TEST: drawing it paints cells, where it used to paint none at all
  {
    nya_render2d_terminal_frame_begin(window, NYA_COLOR_BLACK);

    nya_check(painted_cells(cleared) == 0, "nothing is painted before the draw");

    nya_render2d_texture_ex(window,
                            NYA_ASSET_TEXTURES_DECALS_PNG,
                            (NYA_Render2DTexture){ .x = AT_X, .y = AT_Y, .width = DRAWN_WIDTH, .height = DRAWN_HEIGHT });

    const u32 painted = painted_cells(cleared);
    nya_check(painted > 0, "the picture reaches the grid, got " FMTu32 " cells", painted);

    printf("  PASSED\n");
  }

  // TEST: a handle with nothing behind it draws nothing rather than asserting
  {
    nya_render2d_terminal_frame_begin(window, NYA_COLOR_BLACK);

    nya_render2d_texture(window, "./assets/textures/there_is_no_such_file.png", AT_X, AT_Y, NYA_COLOR_WHITE);
    nya_render2d_texture_rect(window, nullptr, 0, 0, 8, 8, AT_X, AT_Y, DRAWN_WIDTH, DRAWN_HEIGHT, NYA_COLOR_WHITE);

    nya_check(painted_cells(cleared) == 0, "a missing texture paints nothing");

    printf("  PASSED\n");
  }

  // TEST: a source rectangle cuts the sheet, so a sprite sheet works
  {
    const NYA_Asset* asset = nya_asset_get(NYA_ASSET_TEXTURES_DECALS_PNG);
    nya_check(asset != nullptr, "the sheet is still loaded");

    if (asset != nullptr) {
      const f32 half_width  = (f32)asset->as_texture.width * 0.5F;
      const f32 half_height = (f32)asset->as_texture.height * 0.5F;

      u32 painted[2] = { 0 };

      // The two left hand cells of the sheet. They are different pictures, so the cells differ too.
      for (u32 cell = 0; cell < 2; cell++) {
        nya_render2d_terminal_frame_begin(window, NYA_COLOR_BLACK);

        nya_render2d_texture_rect(window, NYA_ASSET_TEXTURES_DECALS_PNG, 0.0F, (f32)cell * half_height, half_width, half_height, AT_X, AT_Y,
                                  DRAWN_WIDTH, DRAWN_HEIGHT, NYA_COLOR_WHITE);

        painted[cell] = painted_cells(cleared);
      }

      nya_check(painted[0] > 0 || painted[1] > 0, "at least one cell of the sheet has something in it");
    }

    printf("  PASSED\n");
  }

  // TEST: the layer gate covers a picture too
  {
    nya_render2d_terminal_frame_begin(window, NYA_COLOR_BLACK);

    // A fill above it, then the picture below: the picture must not land.
    nya_render2d_layer_set(window, 5);
    nya_render2d_rect(window, AT_X, AT_Y, DRAWN_WIDTH, DRAWN_HEIGHT, (NYA_Color){ 0.0F, 1.0F, 0.0F, 1.0F });

    const u32 green = nya_terminal_ink(0.0F, 1.0F, 0.0F);

    nya_render2d_layer_set(window, 1);
    nya_render2d_texture_ex(window,
                            NYA_ASSET_TEXTURES_DECALS_PNG,
                            (NYA_Render2DTexture){ .x = AT_X, .y = AT_Y, .width = DRAWN_WIDTH, .height = DRAWN_HEIGHT });

    nya_check(painted_cells(green) == 0, "a picture under a covering layer does not land");

    printf("  PASSED\n");
  }

  nya_log_info("PASSED: test_render2d_terminal_texture");

  return nya_check_failures() == 0 ? 0 : 1;
}
