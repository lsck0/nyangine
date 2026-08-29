/**
 * The tilemap's editing and writing half: tile_set, layer_resize, to_object and save.
 *
 * tests/nyangine/core/test_tilemap.c covers loading and the two projections. This covers the API an
 * editor drives, which was the untested half — and the one assertion worth making about it is that a
 * map survives a save/load round trip, because that is the whole claim nya_tilemap_to_object makes.
 *
 * Headless: nothing here draws.
 **/

#include "nyangine/nyangine.c"
#include "nyangine/nyangine.h"

#include "generated/assets.h"

#include "SDL3/SDL_init.h"

#define SAVE_PATH "./.test_tilemap_roundtrip.tmj"

s32 main(void) {
    // No real audio device, for the reason test_tilemap.c gives: the ALSA driver leaks inside the
    // library itself and LeakSanitizer reports it against this test.
    SDL_SetHintWithPriority(SDL_HINT_AUDIO_DRIVER, "dummy", SDL_HINT_OVERRIDE);

    _NYA_APP_INSTANCE = (NYA_App){ .initialized = true };
    b8 sdl_ok         = SDL_Init(0);
    nya_assert(sdl_ok, "SDL_Init failed: %s", SDL_GetError());

    nya_system_callback_init();
    NYA_EXPECT(nya_system_events_init());
    nya_system_asset_init();

    NYA_World* world = nya_world_create();
    (void)nya_world_set(world);

    defer nya_world_destroy(world);
    defer nya_system_asset_deinit();
    defer nya_system_events_deinit();
    defer nya_system_callback_deinit();

    NYA_Arena* arena = nya_arena_create(.name = "test_tilemap_editing");
    defer nya_arena_destroy(arena);

    NYA_Tilemap* map = nullptr;
    NYA_EXPECT(nya_tilemap_load(arena, NYA_ASSET_MAPS_DEMO_TOPDOWN_TMJ, &map));

    const u32 ground = nya_tilemap_layer_find(map, "ground");
    nya_check(ground != NYA_TILEMAP_LAYER_NONE, "the fixture should have a 'ground' layer");

    // ── Writing a tile is readable back, and reports that it wrote.
    {
        const u32 original = nya_tilemap_tile_at(map, ground, 3, 4);

        nya_check(nya_tilemap_tile_set(map, ground, 3, 4, 2), "an in-bounds write should report true");
        nya_check(nya_tilemap_tile_at(map, ground, 3, 4) == 2, "the written gid should read back");

        nya_check(nya_tilemap_tile_set(map, ground, 3, 4, 0), "clearing a tile is still a write");
        nya_check(nya_tilemap_tile_at(map, ground, 3, 4) == 0, "a cleared tile reads as empty");

        (void)nya_tilemap_tile_set(map, ground, 3, 4, original);
    }

    // ── Off the map does nothing and says so, rather than asserting. A brush dragged past the edge
    //    is ordinary, so this is the contract that keeps an editor from having to bounds check.
    {
        nya_check(!nya_tilemap_tile_set(map, ground, -1, 0, 1), "negative x should be refused");
        nya_check(!nya_tilemap_tile_set(map, ground, 0, -1, 1), "negative y should be refused");
        nya_check(!nya_tilemap_tile_set(map, ground, (s32)map->width, 0, 1), "x past the edge should be refused");
        nya_check(!nya_tilemap_tile_set(map, ground, 0, (s32)map->height, 1), "y past the edge should be refused");
        nya_check(nya_tilemap_tile_at(map, ground, -1, -1) == 0, "reading off the map is empty, not a crash");
    }

    // ── Resizing is anchored top-left: existing tiles keep their coordinates.
    {
        NYA_Tilemap* resizable = nullptr;
        NYA_EXPECT(nya_tilemap_load(arena, NYA_ASSET_MAPS_DEMO_TOPDOWN_TMJ, &resizable));
        const u32 layer = nya_tilemap_layer_find(resizable, "ground");

        (void)nya_tilemap_tile_set(resizable, layer, 1, 1, 3);
        (void)nya_tilemap_tile_set(resizable, layer, 2, 2, 4);

        NYA_EXPECT(nya_tilemap_layer_resize(resizable, layer, 40, 24));
        nya_check(nya_tilemap_tile_at(resizable, layer, 1, 1) == 3, "growing must not move an existing tile");
        nya_check(nya_tilemap_tile_at(resizable, layer, 2, 2) == 4, "nor the second one");
        nya_check(nya_tilemap_tile_at(resizable, layer, 30, 20) == 0, "new space starts empty");
        nya_check(nya_tilemap_tile_set(resizable, layer, 30, 20, 1), "and is writable");

        // Shrinking keeps what still fits and drops the rest.
        NYA_EXPECT(nya_tilemap_layer_resize(resizable, layer, 5, 5));
        nya_check(nya_tilemap_tile_at(resizable, layer, 1, 1) == 3, "shrinking keeps what still fits");
        nya_check(!nya_tilemap_tile_set(resizable, layer, 30, 20, 1), "and what no longer fits is off the map");
    }

    // ── The document round trips: save, load, and the tiles are the same.
    {
        NYA_Tilemap* edited = nullptr;
        NYA_EXPECT(nya_tilemap_load(arena, NYA_ASSET_MAPS_DEMO_TOPDOWN_TMJ, &edited));
        const u32 layer = nya_tilemap_layer_find(edited, "ground");

        // A recognisable pattern, so a round trip that silently zeroes everything cannot pass.
        for (s32 i = 0; i < 8; i++) (void)nya_tilemap_tile_set(edited, layer, i, i, (u32)(i % 4) + 1);

        NYA_Object* document = nya_tilemap_to_object(arena, edited);
        nya_check(document != nullptr, "to_object should produce a document");

        NYA_EXPECT(nya_tilemap_save(edited, SAVE_PATH));
        nya_check(nya_filesystem_exists(SAVE_PATH), "save should write the file");

        NYA_Tilemap* reloaded = nullptr;
        NYA_EXPECT(nya_tilemap_load(arena, SAVE_PATH, &reloaded));

        nya_check(reloaded->width == edited->width && reloaded->height == edited->height,
                  "dimensions should survive the round trip");
        nya_check(reloaded->orientation == edited->orientation, "orientation should survive");
        nya_check(reloaded->tile_width == edited->tile_width, "tile size should survive");
        // Tile layers survive; object layers are deliberately not written back. Asserting the
        // documented contract rather than the total, so this test fails if that ever silently changes.
        u32 edited_tile_layers = 0;
        for (u32 i = 0; i < edited->layer_count; i++) {
            if (edited->layers[i].kind == NYA_TILEMAP_LAYER_TILES) edited_tile_layers++;
        }
        u32 reloaded_tile_layers = 0;
        for (u32 i = 0; i < reloaded->layer_count; i++) {
            if (reloaded->layers[i].kind == NYA_TILEMAP_LAYER_TILES) reloaded_tile_layers++;
        }
        nya_check(reloaded_tile_layers == edited_tile_layers, "every tile layer should survive: %u -> %u",
                  edited_tile_layers, reloaded_tile_layers);
        nya_check(reloaded->layer_count < edited->layer_count,
                  "the fixture has an object layer, which save is documented to drop");

        const u32 reloaded_layer = nya_tilemap_layer_find(reloaded, "ground");
        nya_check(reloaded_layer != NYA_TILEMAP_LAYER_NONE, "the layer should survive by name");

        for (s32 i = 0; i < 8; i++) {
            const u32 expected = (u32)(i % 4) + 1;
            const u32 actual   = nya_tilemap_tile_at(reloaded, reloaded_layer, i, i);
            nya_check(actual == expected, "tile (%d,%d) round tripped as %u, expected %u", i, i, actual, expected);
        }

        NYA_EXPECT(nya_filesystem_delete(SAVE_PATH));
    }

    // ── An unknown layer name is reported, not guessed at.
    {
        nya_check(nya_tilemap_layer_find(map, "no such layer") == NYA_TILEMAP_LAYER_NONE,
                  "a missing layer should report NONE");
    }

    return nya_check_failures() == 0 ? 0 : 1;
}
