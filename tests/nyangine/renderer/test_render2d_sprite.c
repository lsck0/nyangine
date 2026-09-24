/**
 * Sprites, atlases and image lists.
 **/

#include "nyangine/nyangine.c"
#include "nyangine/nyangine.h"

#include "SDL3/SDL_init.h"

s32 main(void) {
  _NYA_APP_INSTANCE = (NYA_App){ .initialized = true };
  b8 sdl_ok         = SDL_Init(0);
  nya_assert(sdl_ok, "SDL_Init failed: %s", SDL_GetError());

  // TEST: sprites, atlases and image lists
  {
    /* Arithmetic only, since no texture loads headless. That is worth asserting too: an atlas over a missing texture reports no frames, and a sprite over one has no size. */
    NYA_SpriteAtlas atlas = nya_sprite_atlas_grid("does/not/exist.png", 32, 32);
    nya_assert(nya_sprite_atlas_frame_count(&atlas) == 0, "an atlas over an unloaded texture has no frames");

    // centred, unflipped and untinted, and still pointing at the sheet while it has no grid to cut.
    NYA_Sprite cell = nya_sprite_from_atlas(&atlas, 3);
    nya_assert(cell.texture == atlas.texture, "a sprite from an atlas draws the atlas's texture");
    nya_assert(cell.origin.x == 0.5F && cell.origin.y == 0.5F, "and turns about its middle, got " FMTf32x2, FMTf32x2_ARG(cell.origin));
    nya_assert(!cell.flip_x && !cell.flip_y && cell.rotation == 0.0F, "unflipped and unturned");
    nya_assert(cell.tint.r == 1.0F && cell.tint.g == 1.0F && cell.tint.b == 1.0F && cell.tint.a == 1.0F, "and untinted");
    nya_assert(cell.source_width == 0.0F && cell.source_height == 0.0F, "an unloaded sheet has no frame rectangle to cut");

    // A rectangle sprite carries its own size, so it needs no texture to be measured.
    NYA_Sprite sprite = nya_sprite_from_rect("does/not/exist.png", 4.0F, 8.0F, 16.0F, 24.0F);

    f32x2 size = nya_sprite_size(&sprite);
    nya_assert(size.x == 16.0F && size.y == 24.0F, "a rectangle sprite measures its source, got " FMTf32x2, FMTf32x2_ARG(size));

    // Scale multiplies it, and a zeroed scale reads as one rather than as nothing.
    sprite.scale = (f32x2){ 2.0F, 0.5F };
    size         = nya_sprite_size(&sprite);
    nya_assert(size.x == 32.0F && size.y == 12.0F, "scale must multiply the source size, got " FMTf32x2, FMTf32x2_ARG(size));

    NYA_Sprite bare = nya_sprite_from_rect("x", 0.0F, 0.0F, 10.0F, 10.0F);
    bare.scale      = f32x2_zero;
    size            = nya_sprite_size(&bare);
    nya_assert(size.x == 10.0F && size.y == 10.0F, "a zeroed scale means one, got " FMTf32x2, FMTf32x2_ARG(size));

    // An image list knows its length without any texture having loaded, unlike an atlas.
    static NYA_ConstCString frames[] = { "a.png", "b.png", "c.png" };

    NYA_SpriteList list = nya_sprite_list(frames, 3);
    nya_assert(nya_sprite_list_frame_count(&list) == 3);

    NYA_Sprite from_list = nya_sprite_from_list(&list, 1);
    nya_assert(from_list.texture == frames[1], "frame one is the second image");
    nya_assert(from_list.source_width == 0.0F, "a list frame is a whole image, so it has no source rect");

    // past the end draws nothing rather than clamping and hiding an animation that ran off its table.
    nya_sprite_set_frame_from_list(&from_list, &list, 99);
    nya_assert(from_list.texture == nullptr, "an out of range frame points at no texture");
  }

  nya_log_info("PASSED: test_sprite");

  return EXIT_SUCCESS;
}
