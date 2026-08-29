/**
 * Tilemaps: reading Tiled's own JSON, and the two projections it can be in.
 *
 * The maps under assets/maps are the fixtures. They are generated rather than hand written, so what
 * this asserts about them is checkable against the generator — a twenty by twelve map, grass
 * everywhere, a pond, a wall along the bottom, and two objects with properties.
 *
 * The isometric half is where the real risk is: the diamond projection and its inverse are four
 * lines each and neither is verifiable by eye, so most of this file is round trips through them.
 *
 * Headless throughout. Drawing is stubbed, but loading, projecting and collision are not — the
 * collision half spawns real bodies into a real Box2D world.
 **/

#include "nyangine/nyangine.c"
#include "nyangine/nyangine.h"

#include "generated/assets.h"

#include "SDL3/SDL_init.h"

enum { KIND_TERRAIN = 7 };

/** Two tile coordinates are the same cell. Projections are float arithmetic, so not exact equality. */
static b8 same_tile(f32x2 a, f32x2 b) {
  return fabsf(a.x - b.x) < 0.001F && fabsf(a.y - b.y) < 0.001F;
}

s32 main(void) {
  // No real audio device; nya_system_asset_init opens one otherwise. Same reason as test_asset.c —
  // and on CI the difference is not academic: the real ALSA driver leaks inside the library itself,
  // which LeakSanitizer reports against this test.
  SDL_SetHintWithPriority(SDL_HINT_AUDIO_DRIVER, "dummy", SDL_HINT_OVERRIDE);

  _NYA_APP_INSTANCE = (NYA_App){ .initialized = true };
  b8 sdl_ok         = SDL_Init(0);
  nya_assert(sdl_ok, "SDL_Init failed: %s", SDL_GetError());

  // The asset system registers an end-of-frame hook, so the event system has to be up first — the
  // same build-up-by-hand the other core tests do rather than a full nya_app_init, which would want
  // a window.
  nya_system_callback_init();
  NYA_EXPECT(nya_system_events_init());
  nya_system_asset_init();

  NYA_World* world = nya_world_create();
  (void)nya_world_set(world);

  defer nya_world_destroy(world);
  defer nya_system_asset_deinit();
  defer nya_system_events_deinit();
  defer nya_system_callback_deinit();

  NYA_Arena* arena = nya_arena_create(.name = "test_tilemap");
  defer nya_arena_destroy(arena);

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: an orthogonal map loads with its layers, tilesets and objects
  // ─────────────────────────────────────────────────────────────────────────────
  {
    NYA_Tilemap* map = nullptr;
    NYA_EXPECT(nya_tilemap_load(arena, NYA_ASSET_MAPS_DEMO_TOPDOWN_TMJ, &map));

    nya_assert(map->orientation == NYA_TILEMAP_ORTHOGONAL, "the file says orthogonal");
    nya_assert(map->width == 20 && map->height == 12, "got %ux%u", map->width, map->height);
    nya_assert(map->tile_width == 32 && map->tile_height == 32, "got %ux%u tiles", map->tile_width, map->tile_height);

    nya_assert(map->tileset_count == 1, "one embedded tileset, got " FMTu32, map->tileset_count);
    nya_assert(map->tilesets[0].first_gid == 1, "ids start at one");
    nya_assert(map->tilesets[0].columns == 4, "four tiles across");

    // The image path is relative to the .tmj, and resolving it against the map's directory is what
    // turns it into the handle the asset index generated. Getting this wrong loads no texture at all.
    nya_assert(nya_string_equals(map->tilesets[0].texture, NYA_ASSET_MAPS_TILESET_PNG), "the tileset resolved to '%s'",
               map->tilesets[0].texture);

    nya_assert(map->layer_count == 3, "two tile layers and one object group, got " FMTu32, map->layer_count);

    u32 ground = nya_tilemap_layer_find(map, "ground");
    nya_assert(ground != NYA_TILEMAP_LAYER_NONE, "the ground layer is there");
    nya_assert(map->layers[ground].visible, "and is visible");

    u32 collision = nya_tilemap_layer_find(map, "collision");
    nya_assert(collision != NYA_TILEMAP_LAYER_NONE, "the collision layer is there");
    nya_assert(!map->layers[collision].visible, "and is hidden, which is what stops it being drawn");

    nya_assert(nya_tilemap_layer_find(map, "nope") == NYA_TILEMAP_LAYER_NONE, "a missing layer answers NONE");

    printf("  PASSED\n");
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: tile contents, and reading off the edge
  // ─────────────────────────────────────────────────────────────────────────────
  {
    NYA_Tilemap* map = nullptr;
    NYA_EXPECT(nya_tilemap_load(arena, NYA_ASSET_MAPS_DEMO_TOPDOWN_TMJ, &map));

    u32 ground = nya_tilemap_layer_find(map, "ground");

    nya_assert(nya_tilemap_tile_at(map, ground, 0, 0) == 2, "the top left is grass");
    nya_assert(nya_tilemap_tile_at(map, ground, 7, 5) == 3, "the pond is water");
    nya_assert(nya_tilemap_tile_at(map, ground, 0, 11) == 4, "the bottom row is wall");

    // Off the map answers empty rather than asserting: a query around a position routinely runs off
    // the edge, and making every caller clamp first is how one of them forgets.
    nya_assert(nya_tilemap_tile_at(map, ground, -1, 0) == 0, "left of the map is empty");
    nya_assert(nya_tilemap_tile_at(map, ground, 0, -1) == 0, "above it is empty");
    nya_assert(nya_tilemap_tile_at(map, ground, 20, 0) == 0, "right of it is empty");
    nya_assert(nya_tilemap_tile_at(map, ground, 0, 12) == 0, "below it is empty");
    nya_assert(nya_tilemap_tile_at(map, 99, 0, 0) == 0, "a layer that does not exist is empty");

    printf("  PASSED\n");
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: objects and their custom properties
  // ─────────────────────────────────────────────────────────────────────────────
  {
    NYA_Tilemap* map = nullptr;
    NYA_EXPECT(nya_tilemap_load(arena, NYA_ASSET_MAPS_DEMO_TOPDOWN_TMJ, &map));

    const NYA_TilemapObject* spawn = nya_tilemap_object_find(map, "player_spawn");
    nya_assert(spawn != nullptr, "the spawn marker is there");
    nya_assert(nya_string_equals(spawn->type, "spawn"), "with its class, got '%s'", spawn->type);
    nya_assert(spawn->position.x == 64.0F, "at tile two, got %f", (f64)spawn->position.x);

    const NYA_TilemapProperty* facing = nya_tilemap_object_property(spawn, "facing");
    nya_assert(facing != nullptr && nya_string_equals(facing->as_string, "right"), "a string property reads back");

    const NYA_TilemapObject* chest = nya_tilemap_object_find(map, "chest");
    nya_assert(chest != nullptr, "and so is the chest");
    nya_assert(chest->size.x == 32.0F && chest->size.y == 32.0F, "a rectangle object has a size");

    // Filled in as every type at once, because Tiled writes a float of one as `1` and there is no
    // way to tell it from an int afterwards.
    const NYA_TilemapProperty* gold = nya_tilemap_object_property(chest, "gold");
    nya_assert(gold != nullptr, "the gold property is there");
    nya_assert(gold->as_integer == 25, "as an integer, got " FMTs64, gold->as_integer);
    nya_assert(gold->as_real == 25.0, "and as a real, got %f", gold->as_real);

    nya_assert(nya_tilemap_object_find(map, "nope") == nullptr, "a missing object answers null");
    nya_assert(nya_tilemap_object_property(chest, "nope") == nullptr, "and so does a missing property");

    printf("  PASSED\n");
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: orthogonal coordinates round trip
  // ─────────────────────────────────────────────────────────────────────────────
  {
    NYA_Tilemap* map = nullptr;
    NYA_EXPECT(nya_tilemap_load(arena, NYA_ASSET_MAPS_DEMO_TOPDOWN_TMJ, &map));

    nya_assert(same_tile(nya_tilemap_tile_to_world(map, (f32x2){ 0.0F, 0.0F }), (f32x2){ 0.0F, 0.0F }), "the origin is the origin");
    nya_assert(same_tile(nya_tilemap_tile_to_world(map, (f32x2){ 3.0F, 2.0F }), (f32x2){ 96.0F, 64.0F }), "and a cell is a multiply");

    f32x2 tile = { 5.25F, 7.5F };
    nya_assert(same_tile(nya_tilemap_world_to_tile(map, nya_tilemap_tile_to_world(map, tile)), tile), "world_to_tile inverts tile_to_world");

    printf("  PASSED\n");
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: isometric coordinates round trip
  // ─────────────────────────────────────────────────────────────────────────────
  {
    NYA_Tilemap* map = nullptr;
    NYA_EXPECT(nya_tilemap_load(arena, NYA_ASSET_MAPS_DEMO_ISOMETRIC_TMJ, &map));

    nya_assert(map->orientation == NYA_TILEMAP_ISOMETRIC, "the file says isometric");
    nya_assert(map->tile_width == 32 && map->tile_height == 16, "a 2:1 diamond, got %ux%u", map->tile_width, map->tile_height);

    // Stepping one tile along +x moves half a tile right and half a tile down; along +y it moves half
    // a tile *left* and half down. That asymmetry is the whole projection.
    nya_assert(same_tile(nya_tilemap_tile_to_world(map, (f32x2){ 0.0F, 0.0F }), (f32x2){ 0.0F, 0.0F }), "the origin is the origin");
    nya_assert(same_tile(nya_tilemap_tile_to_world(map, (f32x2){ 1.0F, 0.0F }), (f32x2){ 16.0F, 8.0F }), "+x goes right and down");
    nya_assert(same_tile(nya_tilemap_tile_to_world(map, (f32x2){ 0.0F, 1.0F }), (f32x2){ -16.0F, 8.0F }), "+y goes left and down");

    // The inverse is not obvious by eye, which is exactly why it is a function. Several points,
    // including a fractional one and one in the negative quadrant.
    f32x2 samples[] = { { 0.0F, 0.0F }, { 3.0F, 5.0F }, { 12.5F, 0.25F }, { -4.0F, 9.0F } };

    for (u32 i = 0; i < nya_carray_length(samples); i++) {
      f32x2 world = nya_tilemap_tile_to_world(map, samples[i]);
      nya_assert(same_tile(nya_tilemap_world_to_tile(map, world), samples[i]), "the isometric projection round trips at sample " FMTu32, i);
    }

    printf("  PASSED\n");
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: collision merges runs rather than making one body per tile
  // ─────────────────────────────────────────────────────────────────────────────
  {
    NYA_Tilemap* map = nullptr;
    NYA_EXPECT(nya_tilemap_load(arena, NYA_ASSET_MAPS_DEMO_TOPDOWN_TMJ, &map));

    nya_assert(nya_physics2d_body_count() == 0, "no bodies before the map builds any");

    u32 built = nya_tilemap_collision_build(map, "collision", KIND_TERRAIN);

    /*
     * The solid rows are three full-width rows: the shelf and two rows of wall.
     *
     * Merged, that is three bodies. One per tile would be sixty — and, worse, fifty-seven internal
     * edges for a sliding body to catch on, which is the reason the merge exists at all.
     */
    nya_assert(built == 3, "three merged runs, got " FMTu32, built);
    nya_assert(nya_physics2d_body_count() == 3, "and three bodies in the solver");

    u32 terrain = 0;
    nya_entity_foreach_kind (KIND_TERRAIN, entity) {
      terrain++;
      nya_assert(entity->physics2d.attached, "each collider has a body");
      nya_assert(entity->physics2d.size.x == 20.0F * 32.0F, "spanning the full width, got %f", (f64)entity->physics2d.size.x);
    }

    nya_assert(terrain == 3, "and each is findable by kind, got " FMTu32, terrain);

    // A layer that is not there is a warning and no bodies, not a crash — a map is content and may
    // simply not have one.
    nya_assert(nya_tilemap_collision_build(map, "nope", KIND_TERRAIN) == 0, "a missing layer builds nothing");

    nya_entity_clear();
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: an isometric map refuses to build box colliders
  // ─────────────────────────────────────────────────────────────────────────────
  {
    NYA_Tilemap* map = nullptr;
    NYA_EXPECT(nya_tilemap_load(arena, NYA_ASSET_MAPS_DEMO_ISOMETRIC_TMJ, &map));

    // A diamond is not a box, and boxing one is wrong in a way nobody sees until something walks into
    // a corner. Refused with a warning rather than answered badly.
    nya_assert(nya_tilemap_collision_build(map, "collision", KIND_TERRAIN) == 0, "isometric collision is refused");
    nya_assert(nya_physics2d_body_count() == 0, "and builds nothing");

    printf("  PASSED\n");
  }

  printf("PASSED: test_tilemap\n");
  return 0;
}
