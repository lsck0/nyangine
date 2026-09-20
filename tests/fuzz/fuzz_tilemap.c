/**
 * The tilemap loader, fed whatever. A .tmj is JSON a level designer exported and a modder edited, so
 * everything in it is somebody else's number: layer sizes, gids, base64 tile data, object shapes.
 *
 * The input is written next to the real maps rather than into a scratch directory, because a tileset
 * names its image relative to the map and a map somewhere else would fail at the first tileset and
 * never reach the layer parsing this target exists for. The file is removed again afterwards.
 **/

#include "nyangine/nyangine.c"
#include "nyangine/nyangine.h"

#include "SDL3/SDL_init.h"

#define FUZZ_TARGET "tilemap"

/** Where the input is put so relative tileset images resolve the way the demo map's do. */
#define FUZZ_TILEMAP_PATH "./assets/maps/nya_fuzz_tilemap.tmj"

static void fuzz_setup(void) {
    // no real audio device: nya_system_asset_init opens one, and the driver's own allocations would
    // be reported against this target. Same as the core asset tests.
    SDL_SetHintWithPriority(SDL_HINT_AUDIO_DRIVER, "dummy", SDL_HINT_OVERRIDE);

    _NYA_APP_INSTANCE = (NYA_App){ .initialized = true };

    b8 sdl_ok = SDL_Init(0);
    nya_assert(sdl_ok, "SDL_Init failed: %s", SDL_GetError());

    // the asset system registers an end-of-frame hook, so events come up first. By hand rather than
    // through nya_app_init, which wants a window.
    nya_system_callback_init();
    NYA_EXPECT(nya_system_events_init());
    nya_system_asset_init();
}

#define FUZZ_SETUP fuzz_setup

static void fuzz_once(const u8* data, u64 size) {
    NYA_Arena* arena = nya_arena_create(.name = "fuzz_tilemap");
    defer      nya_arena_destroy(arena);

    NYA_String* content = nya_string_create_with_capacity(arena, size + 1);
    for (u64 i = 0; i < size; i++) nya_string_push_back(content, data[i]);

    if (!nya_file_write(FUZZ_TILEMAP_PATH, content).ok) return;

    // removed however this returns: a map left in the asset tree would be indexed into the generated
    // asset header by the next build.
    defer (void)nya_filesystem_delete(FUZZ_TILEMAP_PATH);

    NYA_Tilemap* map = nullptr;
    if (!nya_tilemap_load(arena, FUZZ_TILEMAP_PATH, &map).ok || map == nullptr) return;

    /*
     * What the loader promises whoever draws or queries the map, and what nothing downstream checks
     * again: a layer covers the map, and every tile lookup inside it is in bounds.
     */
    nya_assert(map->width > 0 && map->height > 0, "a loaded map has no size");
    nya_assert(map->tile_width > 0 && map->tile_height > 0, "a loaded map has no tile size");

    for (u32 layer = 0; layer < map->layer_count; layer++) {
        // corners and centre rather than every cell: the bound is the same check at every index, and
        // a map fuzzing at a thousand inputs a second should not walk a million of them.
        (void)nya_tilemap_tile_at(map, layer, 0, 0);
        (void)nya_tilemap_tile_at(map, layer, (s32)map->width - 1, (s32)map->height - 1);
        (void)nya_tilemap_tile_at(map, layer, (s32)map->width / 2, (s32)map->height / 2);

        // outside the map on every side: the lookup has to answer rather than index past its rows.
        (void)nya_tilemap_tile_at(map, layer, -1, -1);
        (void)nya_tilemap_tile_at(map, layer, (s32)map->width, (s32)map->height);
    }

    (void)nya_tilemap_to_object(arena, map);
}

#include "fuzz/fuzz.h"
