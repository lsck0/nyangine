/**
 * Animated tiles and auto-tiling: the two things a tile layer does that a static grid cannot.
 *
 * Both are pure functions of what is already loaded, so this writes its own `.tmj` fixture rather
 * than adding to the generated maps under assets/maps — a fixture that exists to carry one animated
 * tile and one blob of wall is clearer beside the assertions than three directories away.
 *
 * The blob case is where the real risk is. Its 47 pieces come from collapsing 256 raw neighbour
 * combinations, the collapse is four lines, and nothing about the result is checkable by eye — so
 * most of the auto-tiling half is properties of the collapse rather than specific indices.
 *
 * Headless: nothing here draws.
 **/

#include "nyangine/nyangine.c"
#include "nyangine/nyangine.h"

#include "SDL3/SDL_init.h"

#define FIXTURE "./_test_tilemap_autotile.tmj"

/**
 * A four-by-four map with one tile layer, plus a tileset whose tile 0 animates over three frames.
 *
 * Written out rather than embedded as a byte array so the JSON is readable: everything asserted
 * below is visible here, including the frame durations.
 * */
static NYA_ConstCString FIXTURE_JSON =
    "{"
    "  \"width\": 4, \"height\": 4, \"tilewidth\": 16, \"tileheight\": 16,"
    "  \"orientation\": \"orthogonal\", \"infinite\": false,"
    "  \"tilesets\": [ {"
    "    \"firstgid\": 1, \"name\": \"sheet\", \"image\": \"tileset.png\","
    "    \"tilewidth\": 16, \"tileheight\": 16, \"columns\": 8, \"tilecount\": 64,"
    "    \"tiles\": ["
    "      { \"id\": 0, \"animation\": ["
    "        { \"tileid\": 0, \"duration\": 100 },"
    "        { \"tileid\": 1, \"duration\": 200 },"
    "        { \"tileid\": 2, \"duration\": 100 } ] },"
    "      { \"id\": 5, \"properties\": [] }"
    "    ]"
    "  } ],"
    "  \"layers\": [ {"
    "    \"type\": \"tilelayer\", \"name\": \"ground\", \"visible\": true, \"opacity\": 1,"
    "    \"width\": 4, \"height\": 4,"
    "    \"data\": [ 0,0,0,0,  0,10,10,0,  0,10,10,0,  0,0,0,0 ]"
    "  } ]"
    "}";

static void write_fixture(void) {
    FILE* file = fopen(FIXTURE, "wb");
    nya_assert(file != nullptr, "could not create the fixture at " FIXTURE);
    (void)fwrite(FIXTURE_JSON, 1, strlen(FIXTURE_JSON), file);
    (void)fclose(file);
}

/** A predicate over a fixed 5x5 pattern, for exercising the mask without a map. */
static b8 pattern_filled(s32 x, s32 y, void* user_data) {
    const char* rows = user_data;

    if (x < 0 || y < 0 || x >= 5 || y >= 5) return false;

    return rows[(y * 5) + x] == '#';
}

s32 main(void) {
    // No real audio device; nya_system_asset_init opens one otherwise. Same reason as test_asset.c.
    SDL_SetHintWithPriority(SDL_HINT_AUDIO_DRIVER, "dummy", SDL_HINT_OVERRIDE);

    _NYA_APP_INSTANCE = (NYA_App){ .initialized = true };

    b8 sdl_ok = SDL_Init(0);
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

    NYA_Arena* arena = nya_arena_create(.name = "test_tilemap_autotile");
    defer nya_arena_destroy(arena);

    write_fixture();
    defer (void)remove(FIXTURE);

    NYA_Tilemap* map = nullptr;
    NYA_EXPECT(nya_tilemap_load(arena, FIXTURE, &map));
    nya_assert(map != nullptr);

    // ── The animation came off the tileset, with its durations in seconds.
    {
        nya_check(map->tileset_count == 1, "one tileset, got " FMTu32, map->tileset_count);
        nya_check(map->tilesets[0].animation_count == 1, "one animated tile, got " FMTu32, map->tilesets[0].animation_count);

        const NYA_TilemapAnimation* animation = &map->tilesets[0].animations[0];

        nya_check(animation->local_id == 0, "it belongs to local tile 0, got " FMTu32, animation->local_id);
        nya_check(animation->frame_count == 3, "three frames, got " FMTu32, animation->frame_count);

        // Milliseconds in the file, seconds in the struct. The conversion is the whole point of the field.
        nya_check(fabsf(animation->frames[1].duration_s - 0.2F) < 0.0001F, "the second frame runs 200 ms, got %f",
                  (f64)animation->frames[1].duration_s);
        nya_check(fabsf(animation->total_duration_s - 0.4F) < 0.0001F, "totalling 400 ms, got %f", (f64)animation->total_duration_s);

        // The entry with properties and no animation must not have been counted as one.
        nya_check(nya_tilemap_animation_for(map, 6) == nullptr, "a tile with properties but no animation is not animated");
    }

    // ── The clock picks the frame, and a tile with no animation is left alone.
    {
        // Global id 1 is local 0, the animated one. Frames are local 0, 1, 2 → global 1, 2, 3.
        nya_check(nya_tilemap_tile_frame(map, 1) == 1, "at rest it shows its first frame");

        nya_tilemap_animate(map, 0.15F);   // 0.15s: past the 0.1s first frame, inside the second
        nya_check(nya_tilemap_tile_frame(map, 1) == 2, "0.15s in should be the second frame, got " FMTu32,
                  nya_tilemap_tile_frame(map, 1));

        nya_tilemap_animate(map, 0.20F);   // 0.35s: inside the third
        nya_check(nya_tilemap_tile_frame(map, 1) == 3, "0.35s in should be the third, got " FMTu32, nya_tilemap_tile_frame(map, 1));

        nya_tilemap_animate(map, 0.10F);   // 0.45s: wrapped past 0.4s, back to the first
        nya_check(nya_tilemap_tile_frame(map, 1) == 1, "and it should loop, got " FMTu32, nya_tilemap_tile_frame(map, 1));

        // A tile with no animation resolves to itself, which is what the draw path relies on.
        nya_check(nya_tilemap_tile_frame(map, 10) == 10, "an unanimated tile is its own frame");
        nya_check(nya_tilemap_tile_frame(map, 0) == 0, "and so is an empty cell");
    }

    // ── The flip bits survive a frame change.
    {
        u32 flipped = 1 | NYA_TILEMAP_FLIP_HORIZONTAL;

        nya_tilemap_animate(map, 0.15F);   // 0.6s total → 0.2s into the loop, the second frame

        u32 resolved = nya_tilemap_tile_frame(map, flipped);

        nya_check((resolved & NYA_TILEMAP_GID_MASK) == 2, "the frame should advance, got " FMTu32, resolved & NYA_TILEMAP_GID_MASK);
        nya_check((resolved & NYA_TILEMAP_FLIP_HORIZONTAL) != 0, "and the tile should still be flipped");
    }

    // ── The edge mask: four bits, in the documented order.
    {
        // A plus shape centred at (2, 2), so that cell has all four edge neighbours and no others do.
        const char* plus = "....."
                           "..#.."
                           ".###."
                           "..#.."
                           ".....";

        nya_check(nya_tilemap_autotile_mask(pattern_filled, (void*)plus, 2, 2, NYA_TILEMAP_AUTOTILE_EDGES) == 0b1111,
                  "the centre of a plus has all four edges");

        // The top arm: only its south neighbour is filled, which is bit 2.
        nya_check(nya_tilemap_autotile_mask(pattern_filled, (void*)plus, 2, 1, NYA_TILEMAP_AUTOTILE_EDGES) == 0b0100,
                  "the top arm has only a southern neighbour");

        // The left arm: only east, bit 1.
        nya_check(nya_tilemap_autotile_mask(pattern_filled, (void*)plus, 1, 2, NYA_TILEMAP_AUTOTILE_EDGES) == 0b0010,
                  "the left arm has only an eastern neighbour");

        // Somewhere empty and surrounded by nothing.
        nya_check(nya_tilemap_autotile_mask(pattern_filled, (void*)plus, 0, 0, NYA_TILEMAP_AUTOTILE_EDGES) == 0,
                  "an isolated cell has no neighbours");
    }

    /*
     * ── The blob mask: 47 cases out of 256, and a corner only counts with both its edges.
     *
     * Asserted as properties rather than as specific indices. Which index a given neighbourhood gets
     * is a consequence of the enumeration order, and pinning those numbers here would mean this test
     * had to be rewritten to match any change to it rather than catching one.
     */
    {
        const char* solid = "#####"
                            "#####"
                            "#####"
                            "#####"
                            "#####";

        const char* corner_only = "....."
                                  "..#.."   // NE of (1, 2) is filled...
                                  ".#..."   // ...and (1, 2) itself
                                  "....."
                                  ".....";

        u32 all = nya_tilemap_autotile_mask(pattern_filled, (void*)solid, 2, 2, NYA_TILEMAP_AUTOTILE_BLOB);
        nya_check(all < 47, "every blob index is below 47, got " FMTu32, all);

        u32 isolated = nya_tilemap_autotile_mask(pattern_filled, (void*)corner_only, 1, 2, NYA_TILEMAP_AUTOTILE_BLOB);

        // (1, 2) has a north-east neighbour but neither the north nor the east edge, so that corner is
        // hidden behind the gap and the cell must read as fully isolated.
        nya_check(isolated == 0, "a corner with neither of its edges must collapse to isolated, got " FMTu32, isolated);

        // And the collapse actually reaches all 47: every index has to be produced by something, or a
        // sheet would have tiles nothing ever selects.
        b8 seen[47] = { 0 };
        for (u32 raw = 0; raw < 256; raw++) {
            // Reconstructing the neighbourhood from a raw mask is what the internal table does; this
            // walks the table it built instead, which is the thing being checked.
            seen[_nya_tilemap_blob_case[raw]] = true;
        }

        u32 reached = 0;
        for (u32 i = 0; i < 47; i++) reached += seen[i] ? 1 : 0;

        nya_check(reached == 47, "all 47 blob cases should be reachable, got " FMTu32, reached);
    }

    // ── Auto-tiling a layer rewrites its filled cells and leaves the empty ones alone.
    {
        u32 ground = nya_tilemap_layer_find(map, "ground");
        nya_check(ground != NYA_TILEMAP_LAYER_NONE, "the ground layer should be there");

        // Sixteen distinct ids, so which variant each cell got is readable straight out of the result.
        u32 lookup[16];
        for (u32 i = 0; i < 16; i++) lookup[i] = 100 + i;

        NYA_EXPECT(nya_tilemap_autotile_layer(map, ground, lookup, 16, NYA_TILEMAP_AUTOTILE_EDGES, false));

        // The fixture's filled cells are the 2x2 block at (1,1)-(2,2). Each has exactly two filled
        // edge neighbours, so each gets a different corner piece — and no cell may keep its old id.
        nya_check(nya_tilemap_tile_at(map, ground, 0, 0) == 0, "an empty cell stays empty");

        u32 top_left = nya_tilemap_tile_at(map, ground, 1, 1);
        nya_check(top_left >= 100 && top_left < 116, "a filled cell should be replaced from the lookup, got " FMTu32, top_left);

        // Top-left of the block: neighbours east (bit 1) and south (bit 2) → 0b0110 = 6.
        nya_check(top_left == 100 + 0b0110, "the block's top-left corner should pick variant 6, got " FMTu32, top_left - 100);

        // Bottom-right: neighbours north (bit 0) and west (bit 3) → 0b1001 = 9.
        nya_check(nya_tilemap_tile_at(map, ground, 2, 2) == 100 + 0b1001, "and its bottom-right variant 9, got " FMTu32,
                  nya_tilemap_tile_at(map, ground, 2, 2) - 100);
    }

    // ── The refusals.
    {
        u32 ground = nya_tilemap_layer_find(map, "ground");
        u32 lookup[16] = { 0 };

        NYA_Error short_table = nya_tilemap_autotile_layer(map, ground, lookup, 8, NYA_TILEMAP_AUTOTILE_EDGES, false);
        nya_check(!short_table.ok, "a lookup shorter than the rule needs must be refused rather than read past");

        NYA_Error blob_short = nya_tilemap_autotile_layer(map, ground, lookup, 16, NYA_TILEMAP_AUTOTILE_BLOB, false);
        nya_check(!blob_short.ok, "and sixteen entries is short for the blob rule, which needs 47");

        NYA_Error missing = nya_tilemap_autotile_layer(map, 99, lookup, 16, NYA_TILEMAP_AUTOTILE_EDGES, false);
        nya_check(!missing.ok, "a layer that does not exist is refused");

        NYA_Error no_lookup = nya_tilemap_autotile_layer(map, ground, nullptr, 16, NYA_TILEMAP_AUTOTILE_EDGES, false);
        nya_check(!no_lookup.ok, "and so is a null lookup");
    }

    return nya_check_failures() == 0 ? 0 : 1;
}
