/**
 * @file core_tilemap.h
 *
 * Tiled maps: load a `.tmj`, draw it, collide with it, read what the designer put in it.
 *
 * ```c
 * NYA_Tilemap* map = nullptr;
 * NYA_EXPECT(nya_tilemap_load(world->allocator, NYA_ASSET_MAPS_DEMO_TOPDOWN_TMJ, &map));
 *
 * u32 bodies = nya_tilemap_collision_build(map, "collision");   // static bodies for the solid cells
 *
 * const NYA_TilemapObject* spawn = nya_tilemap_object_find(map, "player_spawn");
 * if (spawn != nullptr) player->position.xy = spawn->position;
 *
 * // ... inside a layer's on_render, with the camera set ...
 * nya_tilemap_draw(window, map);
 * ```
 *
 * `.tmj` is Tiled's own JSON export, read as-is — no import step, no intermediate format. Files go in
 * `assets/maps` and are picked up by the asset index automatically, giving an `NYA_ASSET_MAPS_*` handle.
 *
 * Reads finite orthogonal and isometric maps, tile layers with plain integer data, object groups with
 * their custom properties, and embedded tilesets. Not read, and fails loudly rather than half-working:
 * infinite maps, external `.tsx` tilesets, base64 or compressed layer data, hexagonal or staggered
 * orientations — a map using one fails to load with a message naming it.
 *
 * Everything is in world pixels, including isometric maps: `nya_tilemap_draw` does the diamond
 * projection itself, so an isometric map is drawn through a plain NYA_Camera2DTopDown and an entity on
 * it uses the same units as the physics solver. NYA_Camera2DIsometric is the other way to do this — the
 * camera projects instead, and everything upstream, entities included, lives in tile coordinates. Pick
 * one per game; mixing them double-projects. See render_camera.h.
 * */
#pragma once

#include "nyangine/base/base_arena.h"
#include "nyangine/base/base_attributes.h"
#include "nyangine/base/base_error.h"
#include "nyangine/base/base_types.h"
#include "nyangine/core/core_types.h"
#include "nyangine/math/math_vector.h"
#include "nyangine/renderer/render_color.h"

typedef struct NYA_Window NYA_Window;

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * CONSTANTS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/**
 * The bits Tiled packs into a tile's global id above the index itself.
 *
 * A `.tmj` stores each cell as one integer: low twenty-nine bits are the tile, top three say how it
 * was flipped. Unmasked, a flipped tile reads as a number in the billions — looks like corruption
 * rather than a missed mask.
 * */
#define NYA_TILEMAP_FLIP_HORIZONTAL 0x80000000u
#define NYA_TILEMAP_FLIP_VERTICAL   0x40000000u
#define NYA_TILEMAP_FLIP_DIAGONAL   0x20000000u
#define NYA_TILEMAP_GID_MASK        0x1FFFFFFFu

/** Most tilesets one map may reference. Tiled allows any number; a map needing more is unusual. */
#ifndef NYA_TILEMAP_MAX_TILESETS
#define NYA_TILEMAP_MAX_TILESETS 8
#endif

/** Most custom properties one object may carry. */
#ifndef NYA_TILEMAP_MAX_PROPERTIES
#define NYA_TILEMAP_MAX_PROPERTIES 16
#endif

/** Frames one animated tile may have. Tiled allows any number; water and torches use two to eight. */
#ifndef NYA_TILEMAP_MAX_ANIMATION_FRAMES
#define NYA_TILEMAP_MAX_ANIMATION_FRAMES 16
#endif

/** Animated tiles one tileset may define. */
#ifndef NYA_TILEMAP_MAX_ANIMATIONS
#define NYA_TILEMAP_MAX_ANIMATIONS 64
#endif

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * TYPES
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

typedef enum NYA_TilemapOrientation  NYA_TilemapOrientation;
typedef enum NYA_TilemapAutoTile     NYA_TilemapAutoTile;
typedef struct NYA_TilemapAnimationFrame NYA_TilemapAnimationFrame;
typedef struct NYA_TilemapAnimation      NYA_TilemapAnimation;
typedef enum NYA_TilemapLayerKind    NYA_TilemapLayerKind;
typedef struct NYA_TilemapTileset    NYA_TilemapTileset;
typedef struct NYA_TilemapProperty   NYA_TilemapProperty;
typedef struct NYA_TilemapObject     NYA_TilemapObject;
typedef struct NYA_TilemapLayer      NYA_TilemapLayer;
typedef struct NYA_Tilemap           NYA_Tilemap;

enum NYA_TilemapOrientation {
    /** A square grid seen from directly above. Tile (x, y) sits at (x * tile_width, y * tile_height). */
    NYA_TILEMAP_ORTHOGONAL = 0,

    /**
     * The same grid seen along a diagonal, so each tile draws as a diamond.
     *
     * Tile (x, y) sits at ((x - y) * tile_width / 2, (x + y) * tile_height / 2), which is what makes
     * a row further down the screen read as further away. `tile_height` is the diamond's full height,
     * usually half its width.
     * */
    NYA_TILEMAP_ISOMETRIC,

    NYA_TILEMAP_ORIENTATION_COUNT,
};

enum NYA_TilemapLayerKind {
    /** A grid of tile indices. `tiles` is `width * height` entries, row major, zero for empty. */
    NYA_TILEMAP_LAYER_TILES = 0,

    /** A set of placed objects: spawns, triggers, regions. `objects` rather than `tiles`. */
    NYA_TILEMAP_LAYER_OBJECTS,

    NYA_TILEMAP_LAYER_KIND_COUNT,
};

/**
 * One sheet the map draws from, and the range of global ids that come out of it.
 *
 * A map may use several; which one a tile belongs to is decided by `first_gid` — the tileset with the
 * largest `first_gid` not greater than the id owns it, and the local index is the difference. Why tile
 * ids in a `.tmj` are not simply frame numbers.
 * */
struct NYA_TilemapTileset {
    /** The lowest global id this tileset provides. Ids below it belong to an earlier one. */
    u32 first_gid;

    NYA_ConstCString name;

    /**
     * The texture asset handle, resolved against the map's own directory.
     *
     * Tiled writes the image path relative to the `.tmj`, so `tileset.png` in a map under `assets/maps`
     * becomes the handle `./assets/maps/tileset.png`. Queued for loading by nya_tilemap_load.
     * */
    NYA_ConstCString texture;

    u32 tile_width;
    u32 tile_height;

    /** Cells across the sheet, and how many it holds. From the file, not from the loaded texture. */
    u32 columns;
    u32 tile_count;

    /** Gap between cells and border around them, both in pixels. Usually zero. */
    u32 spacing;
    u32 margin;

    /** Animated tiles this sheet defines, if any. See NYA_TilemapAnimation. */
    const NYA_TilemapAnimation* animations;
    u32                         animation_count;
};

/** One frame of an animated tile: which cell of the sheet, and for how long. */
struct NYA_TilemapAnimationFrame {
    /** The tileset-local id to draw. Add the tileset's `first_gid` for a global one. */
    u32 local_id;

    /** Seconds. Tiled stores milliseconds; converted on load so nothing downstream has to remember. */
    f32 duration_s;
};

/**
 * An animation attached to one tile of a tileset.
 *
 * Tiled hangs these off the tileset rather than off the map, and so does this: the animation is a
 * property of the *artwork*, so every placement of that tile animates without the map saying so.
 * That is why a map with animated water needs no per-cell state at all — the cell still holds one
 * global id, and what it resolves to depends only on the clock.
 * */
struct NYA_TilemapAnimation {
    /** The tileset-local id this animation belongs to: the id a map cell actually holds. */
    u32 local_id;

    NYA_TilemapAnimationFrame frames[NYA_TILEMAP_MAX_ANIMATION_FRAMES];
    u32                       frame_count;

    /** The frames' durations added up. Precomputed because resolving a frame divides by it. */
    f32 total_duration_s;
};

/** One custom property from the editor. Tiled's `int`, `float`, `bool`, `string` and `color`. */
struct NYA_TilemapProperty {
    NYA_ConstCString name;

    /**
     * The value, as every type it could be read as.
     *
     * All four are filled from whatever the file held, so `1` reads correctly through both `as_integer`
     * and `as_real` — Tiled writes a float of one as `1` and there is no telling it from an int after.
     * */
    s64              as_integer;
    f64              as_real;
    b8               as_boolean;
    NYA_ConstCString as_string;
};

/**
 * A placed object: a spawn point, a trigger volume, a region.
 *
 * The half of a map that is not tiles, and usually the half a game actually reads. Positions are in
 * world pixels, already projected for an isometric map.
 * */
struct NYA_TilemapObject {
    u32 id;

    /** The editor's Name field. What nya_tilemap_object_find matches on. */
    NYA_ConstCString name;

    /** The editor's Type/Class field. For "every object of kind spawn". */
    NYA_ConstCString type;

    /** World pixels. Tiled anchors a rectangle at its top left and a point object at the point. */
    f32x2 position;

    /** Zero for a point object, which is the usual shape of a spawn marker. */
    f32x2 size;

    /** Degrees clockwise, as the editor reports it. */
    f32 rotation;

    const NYA_TilemapProperty* properties;
    u32                        property_count;
};

struct NYA_TilemapLayer {
    NYA_TilemapLayerKind kind;

    NYA_ConstCString name;

    /**
     * Whether the editor had this layer's eye open.
     *
     * nya_tilemap_draw skips a hidden layer — what makes a collision layer work: authored as tiles,
     * marked invisible, and read by nya_tilemap_collision_build rather than drawn.
     * */
    b8 visible;

    /** Multiplied into every tile's tint. One unless the editor says otherwise. */
    f32 opacity;

    /* ── TILES ── */

    u32 width;
    u32 height;

    /**
     * `width * height` global ids, row major from the top left. Zero means no tile.
     *
     * Still carries the flip bits — see NYA_TILEMAP_GID_MASK. nya_tilemap_tile_at masks them off;
     * reading this array directly must do so itself.
     * */
    const u32* tiles;

    /* ── OBJECTS ── */

    const NYA_TilemapObject* objects;
    u32                      object_count;
};

struct NYA_Tilemap {
    /** Everything the map owns comes from here, including every string. */
    NYA_Arena* allocator;

    /**
     * Where the map's tile (0, 0) sits in the world, in world units. Zero until something sets it.
     *
     * Lives on the map rather than as a parameter to every call, so drawing, collision and coordinate
     * conversions can't disagree about it — passing it separately is how a map ends up drawn in one
     * place and collided with in another. Set it straight after loading; nya_tilemap_tile_to_world and
     * everything built on it honours this.
     * */
    f32x2 origin;

    NYA_TilemapOrientation orientation;

    /** Tiles across and down. */
    u32 width;
    u32 height;

    /** One cell in pixels. For an isometric map this is the diamond's full width and height. */
    u32 tile_width;
    u32 tile_height;

    const NYA_TilemapTileset* tilesets;
    u32                       tileset_count;

    const NYA_TilemapLayer* layers;
    u32                     layer_count;

    /**
     * Seconds the map's tile animations have been running. Advanced by nya_tilemap_animate.
     *
     * One clock for the whole map rather than one per cell, which is what makes animated tiles free:
     * every placement of the same tile shows the same frame, so a lake of two hundred water tiles
     * costs one modulo at draw time and no state at all. A map that wants tiles out of phase with
     * each other wants two tiles, not two clocks.
     * */
    f32 animation_time_s;
};

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * FUNCTIONS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/*
 * ─────────────────────────────────────────────────────────
 * LOADING
 * ─────────────────────────────────────────────────────────
 */

/**
 * Reads a `.tmj` and queues every tileset texture it names.
 *
 * Everything comes from `arena` — freeing the map is freeing the arena, so pass the world's and it
 * dies with the level. Textures are queued rather than loaded, so the first frames after a load draw
 * nothing before the map appears.
 *
 * Returns NYA_ERROR_INVALID_ARGUMENT, naming the feature, for anything this does not read: an infinite
 * map, an external tileset, compressed layer data, a hexagonal orientation — rather than a silent
 * partial load missing half its tiles.
 * */
NYA_API NYA_Error nya_tilemap_load(NYA_Arena* arena, NYA_ConstCString asset_handle, OUT NYA_Tilemap** out_map) __attr_no_discard;

/*
 * ─────────────────────────────────────────────────────────
 * DRAWING
 * ─────────────────────────────────────────────────────────
 */

/**
 * Draws every visible layer, in order, culled to what the camera can see.
 *
 * Called from a layer's on_render with the camera already set. Only tiles inside the view are emitted
 * — a thousand by thousand map costs the same as a screenful, which is what makes a large map viable
 * without chunking it.
 *
 * One draw call per tileset per layer; a map drawn from one sheet is one call however many tiles are
 * on screen.
 * */
NYA_API void nya_tilemap_draw(NYA_Window* window, const NYA_Tilemap* map);

/**
 * One layer, whether or not it is visible.
 *
 * For drawing something *between* two layers — entities between the ground and the treetops. Ignoring
 * `visible` is deliberate: a caller naming a layer has already decided it wants it.
 * */
NYA_API void nya_tilemap_layer_draw(NYA_Window* window, const NYA_Tilemap* map, u32 layer_index);

/** The index of the layer called `name`, or NYA_TILEMAP_LAYER_NONE. */
NYA_API u32 nya_tilemap_layer_find(const NYA_Tilemap* map, NYA_ConstCString name) __attr_no_discard;

/*
 * ─────────────────────────────────────────────────────────
 * ANIMATED TILES
 * ─────────────────────────────────────────────────────────
 */

/**
 * Advances the map's animation clock. Call once per frame with the frame's delta.
 *
 * ```c
 * nya_tilemap_animate(map, delta_time_s);   // in on_update
 * nya_tilemap_draw(window, map);            // in on_render
 * ```
 *
 * The whole of what animated tiles cost per frame. Nothing is stored per cell and nothing is
 * rebuilt — drawing resolves each tile's frame from this clock, so a lake of two hundred water tiles
 * and a lake of one cost the same.
 *
 * Safe to skip: a map that is never animated draws its tiles' first frames, which is what the editor
 * shows and what a static map holds anyway.
 * */
NYA_API void nya_tilemap_animate(NYA_Tilemap* map, f32 delta_time_s);

/**
 * The global id `gid` currently resolves to, following its animation if it has one.
 *
 * The identity for a tile with no animation, which is almost all of them — so this is what drawing
 * calls unconditionally rather than branching on whether the map animates. Flip bits are preserved:
 * a flipped animated tile stays flipped through every frame.
 * */
NYA_API u32 nya_tilemap_tile_frame(const NYA_Tilemap* map, u32 gid) __attr_no_discard;

/** The animation for a global id, or null. For a game that wants to read the frames itself. */
NYA_API const NYA_TilemapAnimation* nya_tilemap_animation_for(const NYA_Tilemap* map, u32 gid) __attr_no_discard;

/** Answered by nya_tilemap_layer_find when no layer has that name. */
#define NYA_TILEMAP_LAYER_NONE 0xFFFFFFFFu

/*
 * ─────────────────────────────────────────────────────────
 * COORDINATES
 * ─────────────────────────────────────────────────────────
 */

/**
 * Where a tile coordinate sits in the world, in pixels. Fractional coordinates work.
 *
 * The orthogonal answer is a multiply; the isometric one is the diamond projection — the function that
 * makes an isometric map usable with an ordinary camera, see the file header. Returns the tile's top
 * left for an orthogonal map and the top corner of its diamond for an isometric one, matching where
 * each is drawn from. Includes NYA_Tilemap.origin, so a map placed away from the world origin needs
 * nothing else.
 * */
NYA_API f32x2 nya_tilemap_tile_to_world(const NYA_Tilemap* map, f32x2 tile) __attr_no_discard;

/**
 * The inverse: which tile a world point falls in. Fractional, so floor it for an index.
 *
 * What a mouse needs. For an isometric map this is the inverse of the diamond projection — not obvious
 * by eye, which is why it's a function rather than a comment.
 * */
NYA_API f32x2 nya_tilemap_world_to_tile(const NYA_Tilemap* map, f32x2 world) __attr_no_discard;

/**
 * The tile at a cell of a layer, with the flip bits already masked off. Zero for empty or off the map.
 *
 * Negative and out-of-range coordinates answer zero rather than asserting: a query around a position
 * routinely runs off the edge, and a caller should not have to clamp first.
 * */
NYA_API u32 nya_tilemap_tile_at(const NYA_Tilemap* map, u32 layer_index, s32 x, s32 y) __attr_no_discard;

/*
 * ─────────────────────────────────────────────────────────
 * OBJECTS
 * ─────────────────────────────────────────────────────────
 */

/** The first object called `name`, across every object layer. Null when there is none. */
NYA_API const NYA_TilemapObject* nya_tilemap_object_find(const NYA_Tilemap* map, NYA_ConstCString name) __attr_no_discard;

/** The property called `name` on an object, or null. */
NYA_API const NYA_TilemapProperty* nya_tilemap_object_property(const NYA_TilemapObject* object, NYA_ConstCString name) __attr_no_discard;

/*
 * ─────────────────────────────────────────────────────────
 * COLLISION
 * ─────────────────────────────────────────────────────────
 */

/**
 * Spawns a static 2D body for every non-zero cell of a tile layer. Returns how many were made.
 *
 * The layer is read for *presence*, not for which tile it holds — a collision layer is authored by
 * painting any tile wherever the map should be solid, and marked invisible so it is not drawn.
 *
 * ```c
 * u32 solid = nya_tilemap_collision_build(map, "collision");
 * ```
 *
 * Runs of adjacent solid cells along a row are merged into one wide box rather than one body per tile.
 * Not a micro-optimisation: a twenty by twelve map has two hundred and forty cells, and a hundred
 * separate one-tile boxes gives the solver a hundred bodies and a hundred internal edges for a sliding
 * body to catch on. Merging leaves a handful of long ones.
 *
 * Orthogonal maps only. An isometric map's cells are diamonds, not boxes, and boxing them is wrong in
 * a way that is not visible until something walks into a corner — that wants a polygon per cell or a
 * hand-authored object layer, and gets a log rather than a bad answer.
 *
 * The bodies are entities: `nya_entity_despawn` removes them, destroying the world takes them all, and
 * they are spawned with `type` set to `entity_type` so a game can find them again.
 * */
NYA_API u32 nya_tilemap_collision_build(const NYA_Tilemap* map, NYA_ConstCString layer_name, u32 entity_type);

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * EDITING
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/**
 * Writes one tile. Zero clears it.
 *
 * `gid` keeps the flip bits — see NYA_TILEMAP_GID_MASK — so a value passed straight back from
 * nya_tilemap_tile_at round trips, flips and all, which is what an editor's copy and paste needs.
 *
 * Off the map does nothing rather than asserting, matching nya_tilemap_tile_at: a brush dragged past
 * the edge is ordinary. Returns whether anything was written, so an editor can tell an ignored stroke
 * from an applied one.
 * */
NYA_API b8 nya_tilemap_tile_set(NYA_Tilemap* map, u32 layer_index, s32 x, s32 y, u32 gid);

/**
 * Resizes a tile layer, keeping whatever still fits.
 *
 * Anchored at the top left, so tiles keep their coordinates and growing only adds empty space —
 * reanchoring on the centre would move every existing tile, which is rarely what "make it bigger" means.
 * */
NYA_API NYA_Error nya_tilemap_layer_resize(NYA_Tilemap* map, u32 layer_index, u32 width, u32 height);

/*
 * ─────────────────────────────────────────────────────────
 * AUTO-TILING
 * ─────────────────────────────────────────────────────────
 *
 * Choosing which *variant* of a tile to draw from what its neighbours are — the difference between a
 * wall drawn as a row of identical blocks and one that grows corners, ends and junctions on its own.
 *
 * The input is a boolean per cell ("is this the same material as me") and the output is an index
 * into a lookup the *game* supplies, because which artwork sits at which index is a property of the
 * sheet and not of the algorithm. Nothing here reads a tileset.
 */

/** How many neighbours an auto-tile rule looks at. Decides the range of nya_tilemap_autotile_mask. */
enum NYA_TilemapAutoTile {
    /**
     * The four edge neighbours: north, east, south, west. Sixteen cases.
     *
     * Bit 0 north, bit 1 east, bit 2 south, bit 3 west — so a cell with neighbours above and to the
     * right is 0b0011 = 3. The usual choice, and the one a 16-tile "blob" sheet is drawn for.
     * */
    NYA_TILEMAP_AUTOTILE_EDGES = 0,

    /**
     * All eight, with the corners. **Forty-seven** cases, not 256.
     *
     * A corner only matters when both edges beside it are filled — otherwise the corner is hidden
     * behind the gap that edge leaves — so the 256 raw combinations collapse to 47 distinct pieces.
     * `nya_tilemap_autotile_mask` returns the collapsed index directly, in the standard order a
     * 47-tile sheet is drawn in, so a lookup table has 47 entries rather than 256.
     * */
    NYA_TILEMAP_AUTOTILE_BLOB,

    NYA_TILEMAP_AUTOTILE_COUNT,
};

/** Whether a cell counts as filled, for auto-tiling. Called with whatever `user_data` was passed. */
typedef b8 (*NYA_TilemapAutoTileFilledFn)(s32 x, s32 y, void* user_data);

/**
 * The variant index for one cell, from what its neighbours are.
 *
 * ```c
 * // A wall sheet whose sixteen variants are laid out in the standard edge order.
 * u32 variant = nya_tilemap_autotile_mask(is_wall, world, x, y, NYA_TILEMAP_AUTOTILE_EDGES);
 * nya_tilemap_tile_set(map, walls, x, y, wall_first_gid + variant);
 * ```
 *
 * Below `16` for `EDGES` and below `47` for `BLOB`. Cells outside the map are whatever `filled`
 * says about them — which is the caller's choice to make, and the reason it takes a predicate rather
 * than a layer: a map whose edges should look walled answers true out of bounds, and one that should
 * look open answers false.
 * */
NYA_API u32 nya_tilemap_autotile_mask(NYA_TilemapAutoTileFilledFn filled, void* user_data, s32 x, s32 y, NYA_TilemapAutoTile kind)
    __attr_no_discard;

/**
 * Auto-tiles a whole layer in place, from what is already in it.
 *
 * A cell counts as filled when it is non-zero. Every non-zero cell is replaced by
 * `lookup[nya_tilemap_autotile_mask(...)]`, and empty cells are left empty — so this turns a layer
 * painted as a solid blob into one drawn with its corners and ends.
 *
 * `lookup` is global ids, `lookup_length` entries: 16 for `EDGES`, 47 for `BLOB`. A shorter table is
 * refused rather than read past. `out_of_bounds` decides what the world outside the map looks like —
 * true for a map whose edges should read as solid.
 *
 * ⚠ **Reads a snapshot, not the layer it is writing.** Auto-tiling in place off the live layer would
 * have each cell decided partly by the *new* values of the cells before it, so the same map would
 * come out differently depending on which corner the walk started from.
 * */
NYA_API NYA_Error nya_tilemap_autotile_layer(
    NYA_Tilemap*        map,
    u32                 layer_index,
    const u32*          lookup,
    u32                 lookup_length,
    NYA_TilemapAutoTile kind,
    b8                  out_of_bounds
);

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * WRITING
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/**
 * The map as a document, in the Tiled JSON shape nya_tilemap_load reads.
 *
 * Round trips through nya_tilemap_load — an editor saves, the game loads, no second format needed.
 *
 * **Tile layers only. Object layers are read by the loader and are not written back**, so saving a map
 * that had one loses it. That is deliberate — writing out a half understood layer is worse than plainly
 * omitting it — but it means this is not yet a lossless editor save for a map with spawn markers on it.
 *
 * Also does not round trip what the loader ignores: editor state, custom layer properties and per-tile
 * animation, all dropped on load. Safe for maps this engine authored; an editor opening someone else's
 * `.tmj` and saving it will quietly simplify it.
 * */
NYA_API NYA_Object* nya_tilemap_to_object(NYA_Arena* arena, const NYA_Tilemap* map) __attr_no_discard;

/** The same, written to `path`. See nya_tilemap_to_object for what is preserved. */
NYA_API NYA_Error nya_tilemap_save(const NYA_Tilemap* map, NYA_ConstCString path);
