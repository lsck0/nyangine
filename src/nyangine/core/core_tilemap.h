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
 * ## The format is Tiled's, not ours
 *
 * `.tmj` is Tiled's own JSON export, read as-is. There is no import step and no intermediate format,
 * because the moment there is one, the file on disk and the file the designer edits are two different
 * things and someone has to remember to run the converter.
 *
 * `.tmj` files go in `assets/maps` and are picked up by the asset index automatically — the build
 * walks everything under `assets/` that is not a `.c`, `.h` or `.keep`, so a new map is a
 * `./build build assets` away from having an `NYA_ASSET_MAPS_*` handle.
 *
 * What is read: finite orthogonal and isometric maps, tile layers with plain (uncompressed) integer
 * data, object groups with their custom properties, and embedded tilesets. What is not, and says so
 * loudly rather than half-working: infinite maps, external `.tsx` tilesets, base64 or compressed
 * layer data, and hexagonal or staggered orientations. Each is a real feature rather than a missing
 * branch, and a map using one fails to load with a message naming it.
 *
 * ## Everything is in world pixels
 *
 * Including isometric maps. `nya_tilemap_draw` does the diamond projection itself and emits ordinary
 * world coordinates, so an isometric map is drawn through a plain NYA_Camera2DTopDown and an entity
 * standing on it has a position in the same units the physics solver uses.
 *
 * NYA_Camera2DIsometric is the *other* way to do this — the camera projects and everything upstream
 * of it, entities included, lives in tile coordinates. That is the better fit for a game which is
 * isometric all the way down. This one is the better fit for a game where the map happens to be
 * drawn isometrically, and mixing them double-projects. Pick one per game; see render_camera.h.
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
 * A `.tmj` stores each cell as one integer whose low twenty-nine bits are the tile and whose top
 * three say how it was flipped in the editor. Reading the integer without masking gives tile numbers
 * in the billions for every flipped tile, which looks like a corrupt file rather than a missed mask.
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

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * TYPES
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

typedef enum NYA_TilemapOrientation  NYA_TilemapOrientation;
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
 * A map may use several. Which one a tile belongs to is decided by `first_gid`: the tileset with the
 * largest `first_gid` not greater than the id owns it, and the local index is the difference. That
 * indirection is the whole reason tile ids in a `.tmj` are not simply frame numbers.
 * */
struct NYA_TilemapTileset {
    /** The lowest global id this tileset provides. Ids below it belong to an earlier one. */
    u32 first_gid;

    NYA_ConstCString name;

    /**
     * The texture asset handle, resolved against the map's own directory.
     *
     * Tiled writes the image path relative to the `.tmj`, so a map in `assets/maps` referring to
     * `tileset.png` becomes `./assets/maps/tileset.png` — which is exactly the handle the asset index
     * generated for it. Queued for loading by nya_tilemap_load.
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
};

/** One custom property from the editor. Tiled's `int`, `float`, `bool`, `string` and `color`. */
struct NYA_TilemapProperty {
    NYA_ConstCString name;

    /**
     * The value, as every type it could be read as.
     *
     * All four are filled from whatever the file held, so a property written as `1` reads correctly
     * through `as_integer` and through `as_real` — which matters because Tiled writes a float of one
     * as `1` and there is no way to tell it from an int afterwards.
     * */
    s64              as_integer;
    f64              as_real;
    b8               as_boolean;
    NYA_ConstCString as_string;
};

/**
 * A placed object: a spawn point, a trigger volume, a region.
 *
 * The half of a map that is not tiles, and usually the half a game actually reads — where the player
 * starts, where the door is, which room this is. Positions are in world pixels, already projected for
 * an isometric map.
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
     * nya_tilemap_draw skips a hidden layer, which is what makes a collision layer work: it is
     * authored as tiles, marked invisible, and read by nya_tilemap_collision_build rather than drawn.
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
     * Still carrying the flip bits — see NYA_TILEMAP_GID_MASK. nya_tilemap_tile_at masks them off;
     * anything reading this array directly must do so itself.
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
     * On the map rather than a parameter to every call, because it is a property of *this map's
     * placement* and every one of drawing, collision and the coordinate conversions has to agree
     * about it. Passing it separately is how a map ends up drawn in one place and collided with in
     * another — which looks like broken physics rather than a missing argument.
     *
     * Set it straight after loading; it is honoured by nya_tilemap_tile_to_world and therefore by
     * everything built on that.
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
 * Everything comes from `arena`, so freeing the map is freeing the arena — pass the world's and the
 * map dies with the level. The textures are queued rather than loaded, like every other asset, so the
 * first frames after a load draw nothing and then the map appears.
 *
 * NYA_ERROR_INVALID_ARGUMENT, with a message naming the feature, for a map using something this does
 * not read: an infinite map, an external tileset, compressed layer data, a hexagonal orientation. A
 * silent partial load would be a map missing half its tiles for no visible reason.
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
 * Called from a layer's on_render with the camera already set, like any other world drawing. Only the
 * tiles inside the view are emitted — a thousand by thousand map is the same cost as a screenful,
 * which is what makes a large map viable without chunking it.
 *
 * One draw call per tileset per layer, since a draw call holds one texture. A map drawn from one
 * sheet is one call however many tiles are on screen.
 * */
NYA_API void nya_tilemap_draw(NYA_Window* window, const NYA_Tilemap* map);

/**
 * One layer, whether or not it is visible.
 *
 * For drawing something *between* two layers — entities between the ground and the treetops, which is
 * the usual reason a map has more than one. Ignoring `visible` is deliberate: a caller naming a layer
 * has already decided it wants it.
 * */
NYA_API void nya_tilemap_layer_draw(NYA_Window* window, const NYA_Tilemap* map, u32 layer_index);

/** The index of the layer called `name`, or NYA_TILEMAP_LAYER_NONE. */
NYA_API u32 nya_tilemap_layer_find(const NYA_Tilemap* map, NYA_ConstCString name) __attr_no_discard;

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
 * The orthogonal answer is a multiply. The isometric one is the diamond projection, and is the
 * function that makes an isometric map usable with an ordinary camera — see the file header.
 *
 * The point returned is the tile's **top left** for an orthogonal map and the **top corner of its
 * diamond** for an isometric one, matching where each is drawn from.
 *
 * Includes NYA_Tilemap.origin, so a map placed away from the world origin needs nothing else.
 * */
NYA_API f32x2 nya_tilemap_tile_to_world(const NYA_Tilemap* map, f32x2 tile) __attr_no_discard;

/**
 * The inverse: which tile a world point falls in. Fractional, so floor it for an index.
 *
 * What a mouse needs. For an isometric map this is the inverse of the diamond projection, which is
 * genuinely not obvious by eye and is exactly why it is a function rather than a comment.
 * */
NYA_API f32x2 nya_tilemap_world_to_tile(const NYA_Tilemap* map, f32x2 world) __attr_no_discard;

/**
 * The tile at a cell of a layer, with the flip bits already masked off. Zero for empty or off the map.
 *
 * Negative coordinates and coordinates past the edge answer zero rather than asserting: a query
 * around a position routinely runs off the edge, and a caller should not have to clamp first.
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
 * painting any tile wherever the map should be solid, and is marked invisible so it is not drawn.
 *
 * ```c
 * u32 solid = nya_tilemap_collision_build(map, "collision");
 * ```
 *
 * Runs of adjacent solid cells along a row are merged into one wide box rather than one body per
 * tile. That is not a micro-optimisation: a twenty by twelve map has two hundred and forty cells and
 * a floor of a hundred separate one-tile boxes gives the solver a hundred bodies and, worse, a
 * hundred internal edges for a sliding body to catch on. Merging leaves a handful of long ones.
 *
 * Orthogonal maps only. An isometric map's cells are diamonds, and boxing them is wrong in a way
 * that is not visible until something walks into a corner — that wants a polygon per cell or a
 * hand-authored object layer, and gets a log rather than a bad answer.
 *
 * The bodies are entities, so `nya_entity_despawn` removes them and destroying the world takes them
 * all. They are spawned with `type` set to `entity_type`, so a game can find them again.
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
 * `gid` is a global id as the tilesets number them, and it keeps the flip bits — see
 * NYA_TILEMAP_GID_MASK. Passing a value straight back from nya_tilemap_tile_at therefore round trips,
 * flips and all, which is what a copy and paste in an editor needs.
 *
 * Off the map does nothing rather than asserting, matching nya_tilemap_tile_at: a brush dragged past
 * the edge is ordinary, and making every caller clamp first is how one of them forgets.
 *
 * Returns whether anything was written, so an editor can tell an ignored stroke from an applied one.
 * */
NYA_API b8 nya_tilemap_tile_set(NYA_Tilemap* map, u32 layer_index, s32 x, s32 y, u32 gid);

/**
 * Resizes a tile layer, keeping whatever still fits.
 *
 * Anchored at the top left, so tiles keep their coordinates and growing only adds empty space. The
 * alternative — reanchoring on the centre — moves every existing tile, which is almost never what
 * "make the map bigger" means.
 * */
NYA_API NYA_Error nya_tilemap_layer_resize(NYA_Tilemap* map, u32 layer_index, u32 width, u32 height);

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * WRITING
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/**
 * The map as a document, in the Tiled JSON shape nya_tilemap_load reads.
 *
 * Round trips through nya_tilemap_load, which is the property that matters: an editor saves, the game
 * loads, and neither needs a second format. What it does **not** round trip is anything the loader
 * ignores — Tiled writes editor state, custom properties on layers and per-tile animation that this
 * engine has no field for, and a load followed by a save drops all of it.
 *
 * That makes this safe for maps this engine authored and lossy for maps Tiled authored. An editor
 * that opens someone else's `.tmj` and saves it will quietly simplify it.
 * */
NYA_API NYA_Object* nya_tilemap_to_object(NYA_Arena* arena, const NYA_Tilemap* map) __attr_no_discard;

/** The same, written to `path`. See nya_tilemap_to_object for what is preserved. */
NYA_API NYA_Error nya_tilemap_save(const NYA_Tilemap* map, NYA_ConstCString path);
