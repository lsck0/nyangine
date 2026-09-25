/**
 * @file core_tilemap.h
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
 * */
#pragma once

#include "nyangine-std/base/base_arena.h"
#include "nyangine-std/base/base_attributes.h"
#include "nyangine-std/base/base_error.h"
#include "nyangine-std/base/base_types.h"
#include "nyangine-core/core/core_types.h"
#include "nyangine-std/math/math_vector.h"
#include "nyangine-core/renderer/render_color.h"

typedef struct NYA_Window NYA_Window;

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * CONSTANTS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/**
 * The bits Tiled packs into a tile's global id above the index itself.
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
 * */
struct NYA_TilemapTileset {
    /** The lowest global id this tileset provides. Ids below it belong to an earlier one. */
    u32 first_gid;

    NYA_ConstCString name;

    /**
     * The texture asset handle, resolved against the map's own directory.
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
     * */
    s64              as_integer;
    f64              as_real;
    b8               as_boolean;
    NYA_ConstCString as_string;
};

/**
 * A placed object: a spawn point, a trigger volume, a region.
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
     * */
    b8 visible;

    /** Multiplied into every tile's tint. One unless the editor says otherwise. */
    f32 opacity;

    /* ── TILES ── */

    u32 width;
    u32 height;

    /**
     * `width * height` global ids, row major from the top left. Zero means no tile.
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
 * */
NYA_API NYA_Error nya_tilemap_load(NYA_Arena* arena, NYA_ConstCString asset_handle, OUT NYA_Tilemap** out_map) __attr_no_discard;

/*
 * ─────────────────────────────────────────────────────────
 * DRAWING
 * ─────────────────────────────────────────────────────────
 */

/**
 * Draws every visible layer, in order, culled to what the camera can see.
 * */
NYA_API void nya_tilemap_draw(NYA_Window* window, const NYA_Tilemap* map);

/**
 * One layer, whether or not it is visible.
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
 * */
NYA_API void nya_tilemap_animate(NYA_Tilemap* map, f32 delta_time_s);

/**
 * The global id `gid` currently resolves to, following its animation if it has one.
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
 * */
NYA_API f32x2 nya_tilemap_tile_to_world(const NYA_Tilemap* map, f32x2 tile) __attr_no_discard;

/**
 * The inverse: which tile a world point falls in. Fractional, so floor it for an index.
 * */
NYA_API f32x2 nya_tilemap_world_to_tile(const NYA_Tilemap* map, f32x2 world) __attr_no_discard;

/**
 * The tile at a cell of a layer, with the flip bits already masked off. Zero for empty or off the map.
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
 * ```c
 * u32 solid = nya_tilemap_collision_build(map, "collision");
 * ```
 * */
NYA_API u32 nya_tilemap_collision_build(const NYA_Tilemap* map, NYA_ConstCString layer_name, u32 entity_type);

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * EDITING
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/**
 * Writes one tile. Zero clears it.
 * */
NYA_API b8 nya_tilemap_tile_set(NYA_Tilemap* map, u32 layer_index, s32 x, s32 y, u32 gid);

/**
 * Resizes a tile layer, keeping whatever still fits.
 * */
NYA_API NYA_Error nya_tilemap_layer_resize(NYA_Tilemap* map, u32 layer_index, u32 width, u32 height) __attr_no_discard;

/*
 * ─────────────────────────────────────────────────────────
 * AUTO-TILING
 * ─────────────────────────────────────────────────────────
 *
 * Picks a tile variant from its neighbours, so walls grow corners, ends and junctions on their own.
 *
 * The input is a boolean per cell ("same material as me") and the output an index into a lookup the
 * game supplies, since which artwork sits at which index belongs to the sheet. Nothing here reads a
 * tileset.
 */

/** How many neighbours an auto-tile rule looks at. Decides the range of nya_tilemap_autotile_mask. */
enum NYA_TilemapAutoTile {
    /**
     * The four edge neighbours: north, east, south, west. Sixteen cases.
     * */
    NYA_TILEMAP_AUTOTILE_EDGES = 0,

    /** All eight, with the corners. Forty-seven cases, not 256. */
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
 * */
NYA_API u32 nya_tilemap_autotile_mask(NYA_TilemapAutoTileFilledFn filled, void* user_data, s32 x, s32 y, NYA_TilemapAutoTile kind)
    __attr_no_discard;

/**
 * Auto-tiles a whole layer in place, from what is already in it.
 *
 * Reads a snapshot, not the layer being written. Otherwise cells would see their neighbours' new
 * values and the result would depend on walk order.
 * */
NYA_API NYA_Error nya_tilemap_autotile_layer(
    NYA_Tilemap*        map,
    u32                 layer_index,
    const u32*          lookup,
    u32                 lookup_length,
    NYA_TilemapAutoTile kind,
    b8                  out_of_bounds
) __attr_no_discard;

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * WRITING
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/**
 * The map as a document, in the Tiled JSON shape nya_tilemap_load reads.
 * */
NYA_API NYA_Object* nya_tilemap_to_object(NYA_Arena* arena, const NYA_Tilemap* map) __attr_no_discard;

/** The same, written to `path`. See nya_tilemap_to_object for what is preserved. */
NYA_API NYA_Error nya_tilemap_save(const NYA_Tilemap* map, NYA_ConstCString path) __attr_no_discard;
