#include "nyangine/nyangine.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API DECLARATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/**
 * An integer out of a bare JSON value, whatever width the parser chose for it.
 *
 * Separate from the keyed reader below because a tile layer's `data` is an array of numbers with no
 * keys at all, and it is by far the hottest thing here — one call per cell of the map.
 * */
NYA_INTERNAL s64 _nya_tilemap_integer_value(const NYA_Value* value);

/** An integer out of a JSON object's key, whatever width the parser chose. `fallback` when absent. */
NYA_INTERNAL s64 _nya_tilemap_integer(const NYA_Object* object, NYA_ConstCString key, s64 fallback);

/** A real, likewise. JSON has one number type, so an opacity of 1 arrives as an integer. */
NYA_INTERNAL f64 _nya_tilemap_real(const NYA_Object* object, NYA_ConstCString key, f64 fallback);

NYA_INTERNAL b8               _nya_tilemap_boolean(const NYA_Object* object, NYA_ConstCString key, b8 fallback);
NYA_INTERNAL NYA_ConstCString _nya_tilemap_string(NYA_Arena* arena, const NYA_Object* object, NYA_ConstCString key, NYA_ConstCString fallback);

/** The tileset a global id belongs to, or null. See NYA_TilemapTileset.first_gid. */
NYA_INTERNAL const NYA_TilemapTileset* _nya_tilemap_tileset_for(const NYA_Tilemap* map, u32 gid);

NYA_INTERNAL NYA_Error _nya_tilemap_parse_tilesets(NYA_Tilemap* map, const NYA_Object* root, NYA_ConstCString asset_handle);
NYA_INTERNAL NYA_Error _nya_tilemap_parse_layers(NYA_Tilemap* map, const NYA_Object* root);
NYA_INTERNAL NYA_Error _nya_tilemap_parse_objects(NYA_Tilemap* map, const NYA_Object* layer_object, OUT NYA_TilemapLayer* out_layer);

/** The half-open range of tile columns and rows that could touch the visible rectangle. */
NYA_INTERNAL void _nya_tilemap_visible_range(const NYA_Tilemap* map, const NYA_TilemapLayer* layer, f32x2 min, f32x2 max, OUT s32* out_x0,
                                             OUT s32* out_y0, OUT s32* out_x1, OUT s32* out_y1);

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PUBLIC API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/*
 * ─────────────────────────────────────────────────────────
 * LOADING
 * ─────────────────────────────────────────────────────────
 */

NYA_Error nya_tilemap_load(NYA_Arena* arena, NYA_ConstCString asset_handle, OUT NYA_Tilemap** out_map) {
    nya_assert(arena != nullptr);
    nya_assert(out_map != nullptr);

    *out_map = nullptr;

    u8* data = nullptr;
    u64 size = 0;

    // Cast because the asset API takes a mutable handle while only reading it; every call site
    // in the tree does the same. See nya_render2d_texture.
    NYA_TRY(nya_asset_read(arena, (NYA_CString)asset_handle, &data, &size));

    NYA_Object* root = nullptr;

    // JSONC rather than JSON: Tiled writes plain JSON, but a hand-edited map with a comment in it is
    // a thing people do, and the lenient parser reads strict JSON unchanged.
    NYA_TRY(nya_deserialize(arena, data, size, NYA_SERDE_FORMAT_JSONC, NYA_SERDE_NONE, &root));

    NYA_Tilemap* map = nya_arena_alloc(arena, sizeof(NYA_Tilemap));

    *map = (NYA_Tilemap){
        .allocator  = arena,
        .width      = (u32)_nya_tilemap_integer(root, "width", 0),
        .height     = (u32)_nya_tilemap_integer(root, "height", 0),
        .tile_width = (u32)_nya_tilemap_integer(root, "tilewidth", 0),
        .tile_height = (u32)_nya_tilemap_integer(root, "tileheight", 0),
    };

    /*
     * The unsupported cases are refused by name rather than half-read.
     *
     * Each of these produces a map that loads and is wrong: an infinite map has no `data` array and
     * comes out empty, an external tileset leaves every tile unresolvable, and compressed data parses
     * as a base64 string where an array was expected. A blank screen with no message is a far worse
     * afternoon than a refusal that says which feature to turn off in Tiled.
     */
    if (_nya_tilemap_boolean(root, "infinite", false)) {
        return nya_error(NYA_ERROR_INVALID_ARGUMENT, "tilemap '%s' is infinite; save it as a fixed size map", asset_handle);
    }

    NYA_ConstCString orientation = _nya_tilemap_string(arena, root, "orientation", "orthogonal");

    if (nya_string_equals(orientation, "orthogonal")) {
        map->orientation = NYA_TILEMAP_ORTHOGONAL;
    } else if (nya_string_equals(orientation, "isometric")) {
        map->orientation = NYA_TILEMAP_ISOMETRIC;
    } else {
        return nya_error(NYA_ERROR_INVALID_ARGUMENT, "tilemap '%s' is %s; only orthogonal and isometric are read", asset_handle, orientation);
    }

    if (map->width == 0 || map->height == 0 || map->tile_width == 0 || map->tile_height == 0) {
        return nya_error(NYA_ERROR_INVALID_ARGUMENT, "tilemap '%s' has no size", asset_handle);
    }

    NYA_TRY(_nya_tilemap_parse_tilesets(map, root, asset_handle));
    NYA_TRY(_nya_tilemap_parse_layers(map, root));

    nya_info("Loaded tilemap '%s': %ux%u tiles, %u tilesets, %u layers.", asset_handle, map->width, map->height, map->tileset_count,
             map->layer_count);

    *out_map = map;

    return NYA_OK;
}

/*
 * ─────────────────────────────────────────────────────────
 * DRAWING
 * ─────────────────────────────────────────────────────────
 */

void nya_tilemap_draw(NYA_Window* window, const NYA_Tilemap* map) {
    nya_assert(window != nullptr);

    if (map == nullptr) return;

    for (u32 i = 0; i < map->layer_count; i++) {
        // The one place `visible` is honoured, and what makes an invisible collision layer work: it
        // is authored as tiles and read by nya_tilemap_collision_build rather than drawn.
        if (!map->layers[i].visible) continue;

        nya_tilemap_layer_draw(window, map, i);
    }
}

void nya_tilemap_layer_draw(NYA_Window* window, const NYA_Tilemap* map, u32 layer_index) {
    nya_perf_time_this_function();

    nya_assert(window != nullptr);

    if (map == nullptr || layer_index >= map->layer_count) return;

    const NYA_TilemapLayer* layer = &map->layers[layer_index];
    if (layer->kind != NYA_TILEMAP_LAYER_TILES || layer->tiles == nullptr) return;

    /*
     * The visible rectangle, from the camera, exactly the way nya_system_entity_render derives it.
     *
     * All four corners rather than two, because a rotated camera's view is not bounded by the world
     * points behind two opposite screen corners. With no camera this degenerates to the target in
     * screen pixels, which is right for a map drawn in screen space.
     */
    u32 target_width, target_height;
    nya_render2d_target_size(window, &target_width, &target_height);

    f32x2 corners[4] = {
        nya_render2d_screen_to_world(window, (f32x2){ 0.0F, 0.0F }),
        nya_render2d_screen_to_world(window, (f32x2){ (f32)target_width, 0.0F }),
        nya_render2d_screen_to_world(window, (f32x2){ 0.0F, (f32)target_height }),
        nya_render2d_screen_to_world(window, (f32x2){ (f32)target_width, (f32)target_height }),
    };

    f32x2 min = corners[0];
    f32x2 max = corners[0];

    for (u32 i = 1; i < 4; i++) {
        min.x = nya_min(min.x, corners[i].x);
        min.y = nya_min(min.y, corners[i].y);
        max.x = nya_max(max.x, corners[i].x);
        max.y = nya_max(max.y, corners[i].y);
    }

    s32 x0, y0, x1, y1;
    _nya_tilemap_visible_range(map, layer, min, max, &x0, &y0, &x1, &y1);

    NYA_Color tint = { 1.0F, 1.0F, 1.0F, layer->opacity };

    for (s32 y = y0; y < y1; y++) {
        for (s32 x = x0; x < x1; x++) {
            u32 raw = layer->tiles[(u32)y * layer->width + (u32)x];

            u32 gid = raw & NYA_TILEMAP_GID_MASK;
            if (gid == 0) continue;

            const NYA_TilemapTileset* tileset = _nya_tilemap_tileset_for(map, gid);
            if (tileset == nullptr) continue;

            u32 local = gid - tileset->first_gid;

            u32 column = tileset->columns > 0 ? local % tileset->columns : 0;
            u32 row    = tileset->columns > 0 ? local / tileset->columns : 0;

            f32 source_x = (f32)(tileset->margin + (column * (tileset->tile_width + tileset->spacing)));
            f32 source_y = (f32)(tileset->margin + (row * (tileset->tile_height + tileset->spacing)));

            f32x2 position = nya_tilemap_tile_to_world(map, (f32x2){ (f32)x, (f32)y });

            /*
             * Isometric tiles are drawn from the diamond's top corner and are usually *taller* than
             * the diamond — a wall tile sticks up out of its own cell. Anchoring by the top corner of
             * the cell would bury it; anchoring so the tile's bottom edge sits on the cell's bottom
             * corner is what Tiled does and what the art is drawn for.
             */
            if (map->orientation == NYA_TILEMAP_ISOMETRIC) {
                position.x -= (f32)tileset->tile_width * 0.5F;
                position.y -= (f32)tileset->tile_height - (f32)map->tile_height;
            }

            nya_render2d_texture_rect(
                window, tileset->texture, source_x, source_y, (f32)tileset->tile_width, (f32)tileset->tile_height, position.x, position.y,
                (f32)tileset->tile_width, (f32)tileset->tile_height, tint
            );
        }
    }
}

u32 nya_tilemap_layer_find(const NYA_Tilemap* map, NYA_ConstCString name) {
    if (map == nullptr || name == nullptr) return NYA_TILEMAP_LAYER_NONE;

    for (u32 i = 0; i < map->layer_count; i++) {
        if (map->layers[i].name != nullptr && nya_string_equals(map->layers[i].name, name)) return i;
    }

    return NYA_TILEMAP_LAYER_NONE;
}

/*
 * ─────────────────────────────────────────────────────────
 * COORDINATES
 * ─────────────────────────────────────────────────────────
 */

f32x2 nya_tilemap_tile_to_world(const NYA_Tilemap* map, f32x2 tile) {
    if (map == nullptr) return tile;

    f32 tile_width  = (f32)map->tile_width;
    f32 tile_height = (f32)map->tile_height;

    if (map->orientation == NYA_TILEMAP_ISOMETRIC) {
        // Stepping one tile along +x moves half a tile right and half a tile down; along +y it moves
        // half a tile *left* and half down. That is what turns a square grid into diamonds.
        return map->origin + (f32x2){
            (tile.x - tile.y) * tile_width * 0.5F,
            (tile.x + tile.y) * tile_height * 0.5F,
        };
    }

    return map->origin + (f32x2){ tile.x * tile_width, tile.y * tile_height };
}

f32x2 nya_tilemap_world_to_tile(const NYA_Tilemap* map, f32x2 world) {
    if (map == nullptr) return world;

    f32 tile_width  = (f32)map->tile_width;
    f32 tile_height = (f32)map->tile_height;

    // The origin comes off first, so the arithmetic below is the same whether or not the map is
    // placed away from the world origin — and stays the exact inverse of tile_to_world.
    world -= map->origin;

    if (map->orientation == NYA_TILEMAP_ISOMETRIC) {
        /*
         * The inverse of the diamond projection, written out.
         *
         * Solving the two equations above for x and y: adding them cancels y and subtracting cancels
         * x, which is why both terms appear in both results. Not obvious by eye, which is precisely
         * why this is a function and not a comment at a call site.
         */
        f32 half_width  = tile_width * 0.5F;
        f32 half_height = tile_height * 0.5F;

        return (f32x2){
            ((world.x / half_width) + (world.y / half_height)) * 0.5F,
            ((world.y / half_height) - (world.x / half_width)) * 0.5F,
        };
    }

    return (f32x2){ world.x / tile_width, world.y / tile_height };
}

u32 nya_tilemap_tile_at(const NYA_Tilemap* map, u32 layer_index, s32 x, s32 y) {
    if (map == nullptr || layer_index >= map->layer_count) return 0;

    const NYA_TilemapLayer* layer = &map->layers[layer_index];
    if (layer->kind != NYA_TILEMAP_LAYER_TILES || layer->tiles == nullptr) return 0;

    // Off the map answers empty rather than asserting: a query around a position routinely runs off
    // the edge, and making every caller clamp first is how one of them forgets.
    if (x < 0 || y < 0 || (u32)x >= layer->width || (u32)y >= layer->height) return 0;

    return layer->tiles[(u32)y * layer->width + (u32)x] & NYA_TILEMAP_GID_MASK;
}

/*
 * ─────────────────────────────────────────────────────────
 * OBJECTS
 * ─────────────────────────────────────────────────────────
 */

const NYA_TilemapObject* nya_tilemap_object_find(const NYA_Tilemap* map, NYA_ConstCString name) {
    if (map == nullptr || name == nullptr) return nullptr;

    for (u32 i = 0; i < map->layer_count; i++) {
        const NYA_TilemapLayer* layer = &map->layers[i];
        if (layer->kind != NYA_TILEMAP_LAYER_OBJECTS) continue;

        for (u32 j = 0; j < layer->object_count; j++) {
            if (layer->objects[j].name != nullptr && nya_string_equals(layer->objects[j].name, name)) return &layer->objects[j];
        }
    }

    return nullptr;
}

const NYA_TilemapProperty* nya_tilemap_object_property(const NYA_TilemapObject* object, NYA_ConstCString name) {
    if (object == nullptr || name == nullptr) return nullptr;

    for (u32 i = 0; i < object->property_count; i++) {
        if (object->properties[i].name != nullptr && nya_string_equals(object->properties[i].name, name)) return &object->properties[i];
    }

    return nullptr;
}

/*
 * ─────────────────────────────────────────────────────────
 * COLLISION
 * ─────────────────────────────────────────────────────────
 */

u32 nya_tilemap_collision_build(const NYA_Tilemap* map, NYA_ConstCString layer_name, u32 entity_type) {
    if (map == nullptr) return 0;

    if (map->orientation != NYA_TILEMAP_ORTHOGONAL) {
        // A diamond is not a box, and boxing one is wrong in a way nobody sees until something walks
        // into a corner. An isometric map wants a polygon per cell or a hand-authored object layer.
        nya_warn("Tilemap collision is orthogonal only; layer '%s' of an isometric map was skipped.", layer_name);
        return 0;
    }

    u32 layer_index = nya_tilemap_layer_find(map, layer_name);
    if (layer_index == NYA_TILEMAP_LAYER_NONE) {
        nya_warn("Tilemap has no layer called '%s'; no collision was built.", layer_name);
        return 0;
    }

    const NYA_TilemapLayer* layer = &map->layers[layer_index];
    if (layer->kind != NYA_TILEMAP_LAYER_TILES) return 0;

    f32 tile_width  = (f32)map->tile_width;
    f32 tile_height = (f32)map->tile_height;

    u32 built = 0;

    /*
     * Runs along a row are merged into one wide box.
     *
     * Not a micro-optimisation. One body per solid tile gives the solver hundreds of bodies for a
     * small map, and — worse — hundreds of internal edges between them, which a body sliding along
     * the floor catches on. Box2D calls this the ghost vertex problem, and merging is the cheap
     * two-thirds of the fix; the rest would be a chain shape per contour.
     */
    for (u32 y = 0; y < layer->height; y++) {
        u32 run_start = 0;
        u32 run       = 0;

        for (u32 x = 0; x <= layer->width; x++) {
            // One past the end deliberately, so a run reaching the right edge is closed by the same
            // code that closes one ending in the middle.
            b8 solid = x < layer->width && (layer->tiles[y * layer->width + x] & NYA_TILEMAP_GID_MASK) != 0;

            if (solid) {
                if (run == 0) run_start = x;
                run++;
                continue;
            }

            if (run == 0) continue;

            f32 width  = (f32)run * tile_width;
            f32 center = ((f32)run_start * tile_width) + (width * 0.5F);

            NYA_EntityHandle handle = nya_entity_spawn(
                .name     = "tilemap_collider",
                .type     = entity_type,
                // Offset by the map's origin, like everything else the map places — a collider in a
                // different place from the tile it was built from is the bug this field exists to
                // stop.
                .position = { map->origin.x + center, map->origin.y + ((f32)y * tile_height) + (tile_height * 0.5F), 0.0F },
                // Active but not visible: it is geometry, not something to draw. The map's own tiles
                // are what the player sees.
                .state = NYA_ENTITY_STATE_ACTIVE | NYA_ENTITY_STATE_STATIC
            );

            if (nya_entity_is_valid(handle)) {
                (void)nya_physics2d_body_attach(
                    handle, .type = NYA_PHYSICS_BODY_STATIC, .shape = NYA_PHYSICS2D_SHAPE_BOX, .size = { width, tile_height }
                );

                built++;
            }

            run = 0;
        }
    }

    nya_info("Built %u colliders from tilemap layer '%s'.", built, layer_name);

    return built;
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

s64 _nya_tilemap_integer_value(const NYA_Value* value) {
    /*
     * Every numeric type, because JSON has one and the parser picks a C type for it.
     *
     * A tile width of 32 comes back as an S64 and an opacity of 1 comes back as an S64 too, while a
     * hand-edited 1.0 comes back as an F64. A reader that accepted only its own idea of the type
     * would silently take the fallback for the other spelling.
     */
    switch (value->type) {
        case NYA_TYPE_S64: return value->as_s64;
        case NYA_TYPE_S32: return value->as_s32;
        case NYA_TYPE_U64: return (s64)value->as_u64;
        case NYA_TYPE_U32: return value->as_u32;
        case NYA_TYPE_F64: return (s64)value->as_f64;
        case NYA_TYPE_F32: return (s64)value->as_f32;
        case NYA_TYPE_B8:  return value->as_b8 ? 1 : 0;
        default:           return 0;
    }
}

s64 _nya_tilemap_integer(const NYA_Object* object, NYA_ConstCString key, s64 fallback) {
    NYA_Value* value = nya_object_get(object, (NYA_CString)key);
    if (value == nullptr) return fallback;

    // A key present but not a number is a malformed file rather than a missing field, and answering
    // the fallback for it would hide that. Zero is what a non-number reads as.
    if (value->type == NYA_TYPE_NULL) return fallback;

    return _nya_tilemap_integer_value(value);
}

f64 _nya_tilemap_real(const NYA_Object* object, NYA_ConstCString key, f64 fallback) {
    NYA_Value* value = nya_object_get(object, (NYA_CString)key);
    if (value == nullptr) return fallback;

    switch (value->type) {
        case NYA_TYPE_F64: return value->as_f64;
        case NYA_TYPE_F32: return (f64)value->as_f32;
        case NYA_TYPE_S64: return (f64)value->as_s64;
        case NYA_TYPE_S32: return (f64)value->as_s32;
        case NYA_TYPE_U64: return (f64)value->as_u64;
        case NYA_TYPE_U32: return (f64)value->as_u32;
        default:           return fallback;
    }
}

b8 _nya_tilemap_boolean(const NYA_Object* object, NYA_ConstCString key, b8 fallback) {
    NYA_Value* value = nya_object_get(object, (NYA_CString)key);
    if (value == nullptr) return fallback;

    if (value->type == NYA_TYPE_B8) return value->as_b8;

    // Tiled writes true booleans, but a hand-edited file may hold 0 or 1 and both mean what they
    // look like.
    return _nya_tilemap_integer(object, key, fallback ? 1 : 0) != 0;
}

NYA_ConstCString _nya_tilemap_string(NYA_Arena* arena, const NYA_Object* object, NYA_ConstCString key, NYA_ConstCString fallback) {
    NYA_Value* value = nya_object_get(object, (NYA_CString)key);
    if (value == nullptr || value->type != NYA_TYPE_STRING || value->as_string == nullptr) return fallback;

    // Cloned into the map's arena. The parsed object tree lives in the same arena today, but the map
    // is the thing whose lifetime is documented and a caller is entitled to free the tree sooner.
    return nya_string_to_cstring(arena, nya_string_from(arena, value->as_string));
}

const NYA_TilemapTileset* _nya_tilemap_tileset_for(const NYA_Tilemap* map, u32 gid) {
    const NYA_TilemapTileset* best = nullptr;

    // The tileset with the largest first_gid not greater than this id. Walked rather than binary
    // searched: there are at most NYA_TILEMAP_MAX_TILESETS of them and this is per visible tile, so
    // the branch predictor does better than a search would.
    for (u32 i = 0; i < map->tileset_count; i++) {
        if (map->tilesets[i].first_gid > gid) continue;
        if (best == nullptr || map->tilesets[i].first_gid > best->first_gid) best = &map->tilesets[i];
    }

    return best;
}

NYA_Error _nya_tilemap_parse_tilesets(NYA_Tilemap* map, const NYA_Object* root, NYA_ConstCString asset_handle) {
    NYA_Value* tilesets = nya_object_get(root, "tilesets");
    if (tilesets == nullptr || tilesets->type != NYA_TYPE_ARRAY) return NYA_OK;

    u32 count = (u32)nya_min(tilesets->as_array.length, (u64)NYA_TILEMAP_MAX_TILESETS);

    NYA_TilemapTileset* parsed = nya_arena_alloc(map->allocator, count * sizeof(NYA_TilemapTileset));

    /*
     * The map's own directory, kept byte for byte as the asset handle spelled it.
     *
     * Tiled writes the image path relative to the `.tmj`, and the handle for the result has to match
     * the one the asset index generated — which starts `./assets/`. nya_path_join normalises that
     * leading `./` away, and the resulting handle then matches nothing in the blob, so the tileset
     * silently never loads and the map draws blank.
     *
     * So this is a truncation at the last separator rather than a path operation. The handle is not
     * really a path; it is a key that happens to look like one.
     */
    u64 directory_length = 0;

    for (u64 i = 0; asset_handle[i] != '\0'; i++) {
        if (asset_handle[i] == '/' || asset_handle[i] == '\\') directory_length = i + 1;
    }

    for (u32 i = 0; i < count; i++) {
        NYA_Value* entry = &tilesets->as_array.items[i];
        if (entry->type != NYA_TYPE_OBJECT) continue;

        NYA_Object* object = &entry->as_object;

        if (nya_object_get(object, "source") != nullptr) {
            return nya_error(
                NYA_ERROR_INVALID_ARGUMENT, "tilemap '%s' uses an external tileset; re-export it with tilesets embedded", asset_handle
            );
        }

        NYA_ConstCString image = _nya_tilemap_string(map->allocator, object, "image", nullptr);
        if (image == nullptr) return nya_error(NYA_ERROR_INVALID_ARGUMENT, "tilemap '%s' has a tileset with no image", asset_handle);

        NYA_String* texture_path = nya_string_create(map->allocator);

        for (u64 c = 0; c < directory_length; c++) nya_string_push_back(texture_path, (u8)asset_handle[c]);

        nya_string_extend(texture_path, image);

        parsed[i] = (NYA_TilemapTileset){
            .first_gid   = (u32)_nya_tilemap_integer(object, "firstgid", 1),
            .name        = _nya_tilemap_string(map->allocator, object, "name", "tileset"),
            .texture     = nya_string_to_cstring(map->allocator, texture_path),
            .tile_width  = (u32)_nya_tilemap_integer(object, "tilewidth", map->tile_width),
            .tile_height = (u32)_nya_tilemap_integer(object, "tileheight", map->tile_height),
            .columns     = (u32)_nya_tilemap_integer(object, "columns", 0),
            .tile_count  = (u32)_nya_tilemap_integer(object, "tilecount", 0),
            .spacing     = (u32)_nya_tilemap_integer(object, "spacing", 0),
            .margin      = (u32)_nya_tilemap_integer(object, "margin", 0),
        };

        /*
         * Queued here rather than left to the caller.
         *
         * A map names its own textures and nothing else knows which they are, so making the caller
         * queue them would mean making the caller parse the file first. NEAREST because tile art is
         * pixel art: linear filtering samples across the cell boundary and pulls in a sliver of the
         * neighbouring tile, which shows up as a seam that flickers as the camera scrolls.
         */
        NYA_TRY(nya_asset_load((NYA_AssetLoadParameters){
            .type             = NYA_ASSET_TYPE_TEXTURE,
            .handle           = (NYA_CString)parsed[i].texture,
            .as_texture_load  = { .filter = NYA_TEXTURE_FILTER_NEAREST },
        }));
    }

    map->tilesets      = parsed;
    map->tileset_count = count;

    return NYA_OK;
}

NYA_Error _nya_tilemap_parse_layers(NYA_Tilemap* map, const NYA_Object* root) {
    NYA_Value* layers = nya_object_get(root, "layers");
    if (layers == nullptr || layers->type != NYA_TYPE_ARRAY) return NYA_OK;

    u32 count = (u32)layers->as_array.length;

    NYA_TilemapLayer* parsed = nya_arena_alloc(map->allocator, count * sizeof(NYA_TilemapLayer));

    u32 kept = 0;

    for (u32 i = 0; i < count; i++) {
        NYA_Value* entry = &layers->as_array.items[i];
        if (entry->type != NYA_TYPE_OBJECT) continue;

        NYA_Object* object = &entry->as_object;

        NYA_ConstCString type = _nya_tilemap_string(map->allocator, object, "type", "");

        NYA_TilemapLayer layer = {
            .name    = _nya_tilemap_string(map->allocator, object, "name", "layer"),
            .visible = _nya_tilemap_boolean(object, "visible", true),
            .opacity = (f32)_nya_tilemap_real(object, "opacity", 1.0),
        };

        if (nya_string_equals(type, "tilelayer")) {
            layer.kind   = NYA_TILEMAP_LAYER_TILES;
            layer.width  = (u32)_nya_tilemap_integer(object, "width", map->width);
            layer.height = (u32)_nya_tilemap_integer(object, "height", map->height);

            NYA_Value* data = nya_object_get(object, "data");

            if (data == nullptr || data->type != NYA_TYPE_ARRAY) {
                // A string here is base64, which is Tiled's other encoding. Named rather than
                // skipped, because the map otherwise loads with an empty layer and no explanation.
                return nya_error(
                    NYA_ERROR_INVALID_ARGUMENT, "tilemap layer '%s' is not a plain array; re-export with CSV or XML tile layer format", layer.name
                );
            }

            u32  cells = layer.width * layer.height;
            u32* tiles = nya_arena_alloc(map->allocator, cells * sizeof(u32));

            u32 available = (u32)nya_min(data->as_array.length, (u64)cells);

            for (u32 cell = 0; cell < available; cell++) {
                tiles[cell] = (u32)_nya_tilemap_integer_value(&data->as_array.items[cell]);
            }

            // Anything the file did not supply stays empty rather than uninitialised, which for a
            // truncated file is the difference between a hole and a screenful of garbage tiles.
            for (u32 cell = available; cell < cells; cell++) tiles[cell] = 0;

            layer.tiles = tiles;
        } else if (nya_string_equals(type, "objectgroup")) {
            layer.kind = NYA_TILEMAP_LAYER_OBJECTS;
            NYA_TRY(_nya_tilemap_parse_objects(map, object, &layer));
        } else {
            // An image layer or a group. Skipped rather than refused: neither carries anything a game
            // reads, and refusing would make a decorative layer break a map that works.
            nya_debug("Skipping tilemap layer '%s' of unsupported type '%s'.", layer.name, type);
            continue;
        }

        parsed[kept++] = layer;
    }

    map->layers      = parsed;
    map->layer_count = kept;

    return NYA_OK;
}

NYA_Error _nya_tilemap_parse_objects(NYA_Tilemap* map, const NYA_Object* layer_object, OUT NYA_TilemapLayer* out_layer) {
    NYA_Value* objects = nya_object_get(layer_object, "objects");
    if (objects == nullptr || objects->type != NYA_TYPE_ARRAY) return NYA_OK;

    u32 count = (u32)objects->as_array.length;

    NYA_TilemapObject* parsed = nya_arena_alloc(map->allocator, count * sizeof(NYA_TilemapObject));

    for (u32 i = 0; i < count; i++) {
        NYA_Value* entry = &objects->as_array.items[i];
        if (entry->type != NYA_TYPE_OBJECT) continue;

        NYA_Object* object = &entry->as_object;

        parsed[i] = (NYA_TilemapObject){
            .id   = (u32)_nya_tilemap_integer(object, "id", 0),
            .name = _nya_tilemap_string(map->allocator, object, "name", nullptr),
            // Tiled calls this "class" in recent versions and "type" in older ones. Both are read,
            // because a map exported by either has to load.
            .type     = _nya_tilemap_string(map->allocator, object, "class",
                                            _nya_tilemap_string(map->allocator, object, "type", nullptr)),
            .position = { (f32)_nya_tilemap_real(object, "x", 0.0), (f32)_nya_tilemap_real(object, "y", 0.0) },
            .size     = { (f32)_nya_tilemap_real(object, "width", 0.0), (f32)_nya_tilemap_real(object, "height", 0.0) },
            .rotation = (f32)_nya_tilemap_real(object, "rotation", 0.0),
        };

        // Object coordinates in an isometric map are already in Tiled's projected space, so they need
        // no conversion — unlike tile coordinates, which are grid indices. Worth stating, because the
        // two look like they should be treated alike and are not.

        NYA_Value* properties = nya_object_get(object, "properties");
        if (properties == nullptr || properties->type != NYA_TYPE_ARRAY) continue;

        u32 property_count = (u32)nya_min(properties->as_array.length, (u64)NYA_TILEMAP_MAX_PROPERTIES);

        NYA_TilemapProperty* parsed_properties = nya_arena_alloc(map->allocator, property_count * sizeof(NYA_TilemapProperty));

        for (u32 j = 0; j < property_count; j++) {
            NYA_Value* property_entry = &properties->as_array.items[j];
            if (property_entry->type != NYA_TYPE_OBJECT) continue;

            NYA_Object* property = &property_entry->as_object;

            // Filled in as every type at once. Tiled writes a float of one as `1`, which is
            // indistinguishable from an int afterwards — so rather than guess, both are available and
            // the caller reads the one it meant.
            parsed_properties[j] = (NYA_TilemapProperty){
                .name       = _nya_tilemap_string(map->allocator, property, "name", nullptr),
                .as_integer = _nya_tilemap_integer(property, "value", 0),
                .as_real    = _nya_tilemap_real(property, "value", 0.0),
                .as_boolean = _nya_tilemap_boolean(property, "value", false),
                .as_string  = _nya_tilemap_string(map->allocator, property, "value", nullptr),
            };
        }

        parsed[i].properties     = parsed_properties;
        parsed[i].property_count = property_count;
    }

    out_layer->objects      = parsed;
    out_layer->object_count = count;

    return NYA_OK;
}

void _nya_tilemap_visible_range(const NYA_Tilemap* map, const NYA_TilemapLayer* layer, f32x2 min, f32x2 max, OUT s32* out_x0, OUT s32* out_y0,
                                OUT s32* out_x1, OUT s32* out_y1) {
    if (map->orientation == NYA_TILEMAP_ISOMETRIC) {
        /*
         * The four screen corners, each turned back into a tile coordinate.
         *
         * An axis-aligned rectangle in world space is a *diamond* in tile space, so its tile bounds
         * are not the corners' min and max taken pairwise — they are the extent over all four. This
         * is the same reason the camera cull transforms four corners rather than two.
         */
        f32x2 tiles[4] = {
            nya_tilemap_world_to_tile(map, (f32x2){ min.x, min.y }),
            nya_tilemap_world_to_tile(map, (f32x2){ max.x, min.y }),
            nya_tilemap_world_to_tile(map, (f32x2){ min.x, max.y }),
            nya_tilemap_world_to_tile(map, (f32x2){ max.x, max.y }),
        };

        f32x2 low  = tiles[0];
        f32x2 high = tiles[0];

        for (u32 i = 1; i < 4; i++) {
            low.x  = nya_min(low.x, tiles[i].x);
            low.y  = nya_min(low.y, tiles[i].y);
            high.x = nya_max(high.x, tiles[i].x);
            high.y = nya_max(high.y, tiles[i].y);
        }

        // Widened by two, because an isometric tile is usually taller than its cell and one whose
        // origin is off screen can still be half on it — the same margin nya_system_entity_render
        // applies for the same reason.
        *out_x0 = nya_max((s32)floorf(low.x) - 2, 0);
        *out_y0 = nya_max((s32)floorf(low.y) - 2, 0);
        *out_x1 = nya_min((s32)ceilf(high.x) + 2, (s32)layer->width);
        *out_y1 = nya_min((s32)ceilf(high.y) + 2, (s32)layer->height);

        return;
    }

    f32x2 low  = nya_tilemap_world_to_tile(map, min);
    f32x2 high = nya_tilemap_world_to_tile(map, max);

    *out_x0 = nya_max((s32)floorf(low.x), 0);
    *out_y0 = nya_max((s32)floorf(low.y), 0);
    *out_x1 = nya_min((s32)ceilf(high.x) + 1, (s32)layer->width);
    *out_y1 = nya_min((s32)ceilf(high.y) + 1, (s32)layer->height);
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * EDITING
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

b8 nya_tilemap_tile_set(NYA_Tilemap* map, u32 layer_index, s32 x, s32 y, u32 gid) {
    if (map == nullptr || layer_index >= map->layer_count) return false;

    /*
     * The cast is the one place this file writes through `const`.
     *
     * The layer array is declared const because reading a map is overwhelmingly what happens to one,
     * and a game holding a NYA_Tilemap* should not be able to scribble on it by accident. The map
     * owns this memory — it was allocated out of `map->allocator` by the loader — so an editor
     * writing to it is writing to its own arena, not to something borrowed.
     */
    NYA_TilemapLayer* layer = (NYA_TilemapLayer*)&map->layers[layer_index];

    if (layer->kind != NYA_TILEMAP_LAYER_TILES || layer->tiles == nullptr) return false;
    if (x < 0 || y < 0 || (u32)x >= layer->width || (u32)y >= layer->height) return false;

    ((u32*)layer->tiles)[(u32)y * layer->width + (u32)x] = gid;

    return true;
}

NYA_Error nya_tilemap_layer_resize(NYA_Tilemap* map, u32 layer_index, u32 width, u32 height) {
    if (map == nullptr || layer_index >= map->layer_count) return nya_error(NYA_ERROR_INVALID_ARGUMENT, "no such layer");
    if (width == 0 || height == 0) return nya_error(NYA_ERROR_INVALID_ARGUMENT, "a layer cannot be empty");

    NYA_TilemapLayer* layer = (NYA_TilemapLayer*)&map->layers[layer_index];

    if (layer->kind != NYA_TILEMAP_LAYER_TILES) return nya_error(NYA_ERROR_INVALID_ARGUMENT, "not a tile layer");
    if (layer->width == width && layer->height == height) return NYA_OK;

    u32* tiles = nya_arena_alloc(map->allocator, sizeof(u32) * (u64)width * (u64)height);

    nya_memset(tiles, 0, sizeof(u32) * (u64)width * (u64)height);

    // Copied row by row, since the stride changes: a flat copy would shear the map diagonally.
    if (layer->tiles != nullptr) {
        u32 rows    = layer->height < height ? layer->height : height;
        u32 columns = layer->width < width ? layer->width : width;

        for (u32 row = 0; row < rows; row++) {
            nya_memcpy(tiles + ((u64)row * width), layer->tiles + ((u64)row * layer->width), sizeof(u32) * columns);
        }
    }

    /*
     * The old array is not freed, because an arena has no per-allocation free.
     *
     * Resizing repeatedly therefore grows the map's arena. That is acceptable for an editor, where a
     * resize is a deliberate act a person performs, and would not be for anything doing it per frame.
     */
    layer->tiles  = tiles;
    layer->width  = width;
    layer->height = height;

    return NYA_OK;
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * WRITING
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

NYA_Object* nya_tilemap_to_object(NYA_Arena* arena, const NYA_Tilemap* map) {
    nya_assert(arena != nullptr);

    if (map == nullptr) return nullptr;

    NYA_Object* root = nya_object_create(arena);

    // The fields Tiled writes and this loader reads. Its own version keys are included because a
    // reader that checks them should find what it expects rather than nothing.
    nya_object_set(root, "type", (NYA_Value){ .type = NYA_TYPE_STRING, .as_string = (NYA_CString) "map" });
    nya_object_set(root, "infinite", (NYA_Value){ .type = NYA_TYPE_B8, .as_b8 = false });
    nya_object_set(root, "renderorder", (NYA_Value){ .type = NYA_TYPE_STRING, .as_string = (NYA_CString) "right-down" });
    nya_object_set(root, "orientation",
                   (NYA_Value){ .type      = NYA_TYPE_STRING,
                                .as_string = (NYA_CString)(map->orientation == NYA_TILEMAP_ISOMETRIC ? "isometric" : "orthogonal") });

    nya_object_set(root, "width", (NYA_Value){ .type = NYA_TYPE_U32, .as_u32 = map->width });
    nya_object_set(root, "height", (NYA_Value){ .type = NYA_TYPE_U32, .as_u32 = map->height });
    nya_object_set(root, "tilewidth", (NYA_Value){ .type = NYA_TYPE_U32, .as_u32 = map->tile_width });
    nya_object_set(root, "tileheight", (NYA_Value){ .type = NYA_TYPE_U32, .as_u32 = map->tile_height });

    // ── tilesets ──
    NYA_ArrayᐸNYA_Valueᐳ* tilesets = nya_array_create(arena, NYA_Value);

    for (u32 i = 0; i < map->tileset_count; i++) {
        const NYA_TilemapTileset* tileset = &map->tilesets[i];

        NYA_Object* entry = nya_object_create(arena);

        nya_object_set(entry, "firstgid", (NYA_Value){ .type = NYA_TYPE_U32, .as_u32 = tileset->first_gid });
        nya_object_set(entry, "name", (NYA_Value){ .type = NYA_TYPE_STRING, .as_string = (NYA_CString)tileset->name });
        nya_object_set(entry, "image", (NYA_Value){ .type = NYA_TYPE_STRING, .as_string = (NYA_CString)tileset->texture });
        nya_object_set(entry, "tilewidth", (NYA_Value){ .type = NYA_TYPE_U32, .as_u32 = tileset->tile_width });
        nya_object_set(entry, "tileheight", (NYA_Value){ .type = NYA_TYPE_U32, .as_u32 = tileset->tile_height });
        nya_object_set(entry, "columns", (NYA_Value){ .type = NYA_TYPE_U32, .as_u32 = tileset->columns });
        nya_object_set(entry, "tilecount", (NYA_Value){ .type = NYA_TYPE_U32, .as_u32 = tileset->tile_count });
        nya_object_set(entry, "spacing", (NYA_Value){ .type = NYA_TYPE_U32, .as_u32 = tileset->spacing });
        nya_object_set(entry, "margin", (NYA_Value){ .type = NYA_TYPE_U32, .as_u32 = tileset->margin });

        nya_array_push_back(tilesets, ((NYA_Value){ .type = NYA_TYPE_OBJECT, .as_object = *entry }));
    }

    nya_object_set(root, "tilesets", (NYA_Value){ .type = NYA_TYPE_ARRAY, .as_array = *tilesets });

    // ── layers ──
    NYA_ArrayᐸNYA_Valueᐳ* layers = nya_array_create(arena, NYA_Value);

    for (u32 i = 0; i < map->layer_count; i++) {
        const NYA_TilemapLayer* layer = &map->layers[i];

        // Object layers are not written back. The loader reads them, but an editor that cannot yet
        // author one would drop whatever it had loaded — and silently writing out a half understood
        // layer is worse than plainly omitting it. Tile layers are what this can round trip.
        if (layer->kind != NYA_TILEMAP_LAYER_TILES) continue;

        NYA_Object* entry = nya_object_create(arena);

        nya_object_set(entry, "type", (NYA_Value){ .type = NYA_TYPE_STRING, .as_string = (NYA_CString) "tilelayer" });
        nya_object_set(entry, "name", (NYA_Value){ .type = NYA_TYPE_STRING, .as_string = (NYA_CString)layer->name });
        nya_object_set(entry, "visible", (NYA_Value){ .type = NYA_TYPE_B8, .as_b8 = layer->visible });
        nya_object_set(entry, "opacity", (NYA_Value){ .type = NYA_TYPE_F32, .as_f32 = layer->opacity });
        nya_object_set(entry, "width", (NYA_Value){ .type = NYA_TYPE_U32, .as_u32 = layer->width });
        nya_object_set(entry, "height", (NYA_Value){ .type = NYA_TYPE_U32, .as_u32 = layer->height });
        nya_object_set(entry, "x", (NYA_Value){ .type = NYA_TYPE_U32, .as_u32 = 0 });
        nya_object_set(entry, "y", (NYA_Value){ .type = NYA_TYPE_U32, .as_u32 = 0 });

        NYA_ArrayᐸNYA_Valueᐳ* data = nya_array_create(arena, NYA_Value);

        // Raw gids, flip bits included, in the row major order the loader expects. Uncompressed:
        // Tiled's base64 and zlib encodings are optional and this loader does not read them.
        for (u64 tile = 0; tile < (u64)layer->width * (u64)layer->height; tile++) {
            u32 gid = layer->tiles != nullptr ? layer->tiles[tile] : 0;

            nya_array_push_back(data, ((NYA_Value){ .type = NYA_TYPE_U32, .as_u32 = gid }));
        }

        nya_object_set(entry, "data", (NYA_Value){ .type = NYA_TYPE_ARRAY, .as_array = *data });

        nya_array_push_back(layers, ((NYA_Value){ .type = NYA_TYPE_OBJECT, .as_object = *entry }));
    }

    nya_object_set(root, "layers", (NYA_Value){ .type = NYA_TYPE_ARRAY, .as_array = *layers });

    return root;
}

NYA_Error nya_tilemap_save(const NYA_Tilemap* map, NYA_ConstCString path) {
    if (map == nullptr || path == nullptr) return nya_error(NYA_ERROR_INVALID_ARGUMENT, "no map or no path");

    NYA_Arena* scratch = nya_arena_create(.name = "tilemap_save");
    defer      nya_arena_destroy(scratch);

    NYA_Object* root = nya_tilemap_to_object(scratch, map);

    if (root == nullptr) return nya_error(NYA_ERROR_NOT_OK, "could not build the document");

    // Pretty, because a map file is something a person opens in a text editor and a diff has to be
    // readable — a tile layer on one line is a diff nobody can review.
    return nya_serde_save_file(root, path, NYA_SERDE_PRETTY);
}
