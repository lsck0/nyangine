#include "nyangine/nyangine.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API DECLARATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/**
 * An integer out of a bare JSON value, whatever width the parser chose for it.
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

/**
 * Reads a tileset's `tiles[]` array for the ones carrying an `animation`.
 * */
NYA_INTERNAL void _nya_tilemap_parse_animations(NYA_Tilemap* map, const NYA_Object* tileset_object, OUT NYA_TilemapTileset* out_tileset);
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

    // cast because the asset API takes a mutable handle it only reads.
    NYA_TRY(nya_asset_read(arena, (NYA_CString)asset_handle, &data, &size));

    NYA_Object* root = nullptr;

    // JSONC: Tiled writes JSON, people add comments by hand, and the lenient parser reads both.
    NYA_TRY(nya_deserialize(arena, data, size, NYA_SERDE_FORMAT_JSONC, NYA_SERDE_NONE, &root));

    NYA_Tilemap* map = nya_arena_alloc(arena, sizeof(NYA_Tilemap));

    *map = (NYA_Tilemap){
        .allocator  = arena,
        .width      = (u32)_nya_tilemap_integer(root, "width", 0),
        .height     = (u32)_nya_tilemap_integer(root, "height", 0),
        .tile_width = (u32)_nya_tilemap_integer(root, "tilewidth", 0),
        .tile_height = (u32)_nya_tilemap_integer(root, "tileheight", 0),
    };

    /* Unsupported cases are refused by name, not half read. */
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

    nya_log_info("Loaded tilemap '%s': %ux%u tiles, %u tilesets, %u layers.", asset_handle, map->width, map->height, map->tileset_count,
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
        // the one place `visible` is honoured: an invisible collision layer is read by nya_tilemap_collision_build,
        // not drawn.
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

    /* The visible rectangle from the camera, as nya_system_entity_render derives it. */
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

            // before the tileset lookup: an animation's frames may live elsewhere in the sheet.
            gid = nya_tilemap_tile_frame(map, gid) & NYA_TILEMAP_GID_MASK;

            const NYA_TilemapTileset* tileset = _nya_tilemap_tileset_for(map, gid);
            if (tileset == nullptr) continue;

            u32 local = gid - tileset->first_gid;

            u32 column = tileset->columns > 0 ? local % tileset->columns : 0;
            u32 row    = tileset->columns > 0 ? local / tileset->columns : 0;

            f32 source_x = (f32)(tileset->margin + (column * (tileset->tile_width + tileset->spacing)));
            f32 source_y = (f32)(tileset->margin + (row * (tileset->tile_height + tileset->spacing)));

            f32x2 position = nya_tilemap_tile_to_world(map, (f32x2){ (f32)x, (f32)y });

            /*
             * Isometric tiles are drawn from the diamond's top corner and are often taller than the cell. Anchoring the
             * tile's bottom edge on the cell's bottom corner is what Tiled does.
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
        // one tile along +x moves half right and half down; along +y half left and half down.
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

    // origin first, so this stays the exact inverse of tile_to_world.
    world -= map->origin;

    if (map->orientation == NYA_TILEMAP_ISOMETRIC) {
        /* The inverse of the diamond projection. */
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

    // off the map reads empty: queries around a position routinely cross the edge.
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
        // a diamond is not a box. an isometric map wants a polygon per cell or an authored object layer.
        nya_log_warn("Tilemap collision is orthogonal only; layer '%s' of an isometric map was skipped.", layer_name);
        return 0;
    }

    u32 layer_index = nya_tilemap_layer_find(map, layer_name);
    if (layer_index == NYA_TILEMAP_LAYER_NONE) {
        nya_log_warn("Tilemap has no layer called '%s'; no collision was built.", layer_name);
        return 0;
    }

    const NYA_TilemapLayer* layer = &map->layers[layer_index];
    if (layer->kind != NYA_TILEMAP_LAYER_TILES) return 0;

    f32 tile_width  = (f32)map->tile_width;
    f32 tile_height = (f32)map->tile_height;

    u32 built = 0;

    /* Runs along a row are merged into one wide box. */
    for (u32 y = 0; y < layer->height; y++) {
        u32 run_start = 0;
        u32 run       = 0;

        for (u32 x = 0; x <= layer->width; x++) {
            // one past the end, so a run at the right edge closes like any other.
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
                // offset by the map's origin, like everything the map places.
                .position = { map->origin.x + center, map->origin.y + ((f32)y * tile_height) + (tile_height * 0.5F), 0.0F },
                // active but not visible: geometry, not a drawing.
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

    nya_log_info("Built %u colliders from tilemap layer '%s'.", built, layer_name);

    return built;
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

s64 _nya_tilemap_integer_value(const NYA_Value* value) {
    /* Every numeric type, since the parser picks the C type. */
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

    // a key that is not a number is malformed, not missing, so it reads as zero rather than the fallback.
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

    // tolerates 0 and 1 from hand edits.
    return _nya_tilemap_integer(object, key, fallback ? 1 : 0) != 0;
}

NYA_ConstCString _nya_tilemap_string(NYA_Arena* arena, const NYA_Object* object, NYA_ConstCString key, NYA_ConstCString fallback) {
    NYA_Value* value = nya_object_get(object, (NYA_CString)key);
    if (value == nullptr || value->type != NYA_TYPE_STRING || value->as_string == nullptr) return fallback;

    // cloned into the map's arena: the caller may free the parsed tree sooner.
    return nya_string_to_cstring(arena, nya_string_from(arena, value->as_string));
}

/*
 * ─────────────────────────────────────────────────────────
 * ANIMATED TILES
 * ─────────────────────────────────────────────────────────
 */

void nya_tilemap_animate(NYA_Tilemap* map, f32 delta_time_s) {
    if (map == nullptr || delta_time_s <= 0.0F) return;

    map->animation_time_s += delta_time_s;

    /* Wrapped rather than left to grow. */
    if (map->animation_time_s > 3600.0F) map->animation_time_s -= 3600.0F;
}

const NYA_TilemapAnimation* nya_tilemap_animation_for(const NYA_Tilemap* map, u32 gid) {
    if (map == nullptr) return nullptr;

    u32 id = gid & NYA_TILEMAP_GID_MASK;
    if (id == 0) return nullptr;

    const NYA_TilemapTileset* tileset = _nya_tilemap_tileset_for(map, id);
    if (tileset == nullptr || tileset->animation_count == 0) return nullptr;

    u32 local = id - tileset->first_gid;

    // linear over at most NYA_TILEMAP_MAX_ANIMATIONS, and only for tilesets that have animations.
    for (u32 i = 0; i < tileset->animation_count; i++) {
        if (tileset->animations[i].local_id == local) return &tileset->animations[i];
    }

    return nullptr;
}

u32 nya_tilemap_tile_frame(const NYA_Tilemap* map, u32 gid) {
    const NYA_TilemapAnimation* animation = nya_tilemap_animation_for(map, gid);
    if (animation == nullptr || animation->frame_count == 0 || animation->total_duration_s <= 0.0F) return gid;

    const NYA_TilemapTileset* tileset = _nya_tilemap_tileset_for(map, gid & NYA_TILEMAP_GID_MASK);
    if (tileset == nullptr) return gid;

    f32 elapsed = fmodf(map->animation_time_s, animation->total_duration_s);

    for (u32 i = 0; i < animation->frame_count; i++) {
        if (elapsed < animation->frames[i].duration_s) {
            // flip bits carry over: every frame faces the way the tile was placed.
            return (tileset->first_gid + animation->frames[i].local_id) | (gid & ~NYA_TILEMAP_GID_MASK);
        }

        elapsed -= animation->frames[i].duration_s;
    }

    // a rounding edge where fmodf lands just under the total: the last frame.
    return (tileset->first_gid + animation->frames[animation->frame_count - 1].local_id) | (gid & ~NYA_TILEMAP_GID_MASK);
}

/*
 * ─────────────────────────────────────────────────────────
 * AUTO-TILING
 * ─────────────────────────────────────────────────────────
 */

/**
 * The 47 distinct blob cases, indexed by the raw 8-bit neighbour mask.
 * */
NYA_INTERNAL u8 _nya_tilemap_blob_case[256];
NYA_INTERNAL b8 _nya_tilemap_blob_case_built = false;

/** Bit positions in the raw 8-neighbour mask. Clockwise from north, edges and corners interleaved. */
enum {
    _NYA_TILEMAP_N  = 1U << 0,
    _NYA_TILEMAP_NE = 1U << 1,
    _NYA_TILEMAP_E  = 1U << 2,
    _NYA_TILEMAP_SE = 1U << 3,
    _NYA_TILEMAP_S  = 1U << 4,
    _NYA_TILEMAP_SW = 1U << 5,
    _NYA_TILEMAP_W  = 1U << 6,
    _NYA_TILEMAP_NW = 1U << 7,
};

/** Drops the corner bits whose two adjoining edges are not both set. The collapse, in one place. */
NYA_INTERNAL u32 _nya_tilemap_blob_canonical(u32 mask) {
    if ((mask & (_NYA_TILEMAP_N | _NYA_TILEMAP_E)) != (_NYA_TILEMAP_N | _NYA_TILEMAP_E)) mask &= ~(u32)_NYA_TILEMAP_NE;
    if ((mask & (_NYA_TILEMAP_E | _NYA_TILEMAP_S)) != (_NYA_TILEMAP_E | _NYA_TILEMAP_S)) mask &= ~(u32)_NYA_TILEMAP_SE;
    if ((mask & (_NYA_TILEMAP_S | _NYA_TILEMAP_W)) != (_NYA_TILEMAP_S | _NYA_TILEMAP_W)) mask &= ~(u32)_NYA_TILEMAP_SW;
    if ((mask & (_NYA_TILEMAP_W | _NYA_TILEMAP_N)) != (_NYA_TILEMAP_W | _NYA_TILEMAP_N)) mask &= ~(u32)_NYA_TILEMAP_NW;

    return mask;
}

NYA_INTERNAL void _nya_tilemap_blob_build(void) {
    if (_nya_tilemap_blob_case_built) return;

    // canonical masks in ascending order, numbered as met: the sheet order, and reproducible.
    u32 next = 0;

    for (u32 mask = 0; mask < 256; mask++) {
        if (_nya_tilemap_blob_canonical(mask) != mask) continue;

        _nya_tilemap_blob_case[mask] = (u8)next++;
    }

    // then non-canonical masks point at what they collapse to.
    for (u32 mask = 0; mask < 256; mask++) {
        u32 canonical = _nya_tilemap_blob_canonical(mask);
        if (canonical != mask) _nya_tilemap_blob_case[mask] = _nya_tilemap_blob_case[canonical];
    }

    nya_assert(next == 47, "the blob collapse should yield exactly 47 cases, got " FMTu32, next);

    _nya_tilemap_blob_case_built = true;
}

u32 nya_tilemap_autotile_mask(NYA_TilemapAutoTileFilledFn filled, void* user_data, s32 x, s32 y, NYA_TilemapAutoTile kind) {
    if (filled == nullptr) return 0;

    if (kind == NYA_TILEMAP_AUTOTILE_EDGES) {
        u32 mask = 0;

        if (filled(x, y - 1, user_data)) mask |= 1U << 0;   // north
        if (filled(x + 1, y, user_data)) mask |= 1U << 1;   // east
        if (filled(x, y + 1, user_data)) mask |= 1U << 2;   // south
        if (filled(x - 1, y, user_data)) mask |= 1U << 3;   // west

        return mask;
    }

    _nya_tilemap_blob_build();

    u32 mask = 0;

    if (filled(x, y - 1, user_data)) mask |= _NYA_TILEMAP_N;
    if (filled(x + 1, y - 1, user_data)) mask |= _NYA_TILEMAP_NE;
    if (filled(x + 1, y, user_data)) mask |= _NYA_TILEMAP_E;
    if (filled(x + 1, y + 1, user_data)) mask |= _NYA_TILEMAP_SE;
    if (filled(x, y + 1, user_data)) mask |= _NYA_TILEMAP_S;
    if (filled(x - 1, y + 1, user_data)) mask |= _NYA_TILEMAP_SW;
    if (filled(x - 1, y, user_data)) mask |= _NYA_TILEMAP_W;
    if (filled(x - 1, y - 1, user_data)) mask |= _NYA_TILEMAP_NW;

    return _nya_tilemap_blob_case[mask];
}

/** What nya_tilemap_autotile_layer hands its predicate: the snapshot, its size, and the edge answer. */
typedef struct {
    const u32* tiles;
    u32        width;
    u32        height;
    b8         out_of_bounds;
} _NYA_TilemapAutoTileContext;

NYA_INTERNAL b8 _nya_tilemap_autotile_filled(s32 x, s32 y, void* user_data) {
    const _NYA_TilemapAutoTileContext* context = user_data;

    if (x < 0 || y < 0 || (u32)x >= context->width || (u32)y >= context->height) return context->out_of_bounds;

    return (context->tiles[((u32)y * context->width) + (u32)x] & NYA_TILEMAP_GID_MASK) != 0;
}

NYA_Error nya_tilemap_autotile_layer(
    NYA_Tilemap*        map,
    u32                 layer_index,
    const u32*          lookup,
    u32                 lookup_length,
    NYA_TilemapAutoTile kind,
    b8                  out_of_bounds
) {
    if (map == nullptr || lookup == nullptr) return nya_error(NYA_ERROR_INVALID_ARGUMENT, "auto-tiling needs a map and a lookup");
    if (layer_index >= map->layer_count) return nya_error(NYA_ERROR_NOT_FOUND, "layer " FMTu32 " does not exist", layer_index);

    const NYA_TilemapLayer* layer = &map->layers[layer_index];

    if (layer->kind != NYA_TILEMAP_LAYER_TILES || layer->tiles == nullptr) {
        return nya_error(NYA_ERROR_INVALID_ARGUMENT, "layer '%s' is not a tile layer", layer->name);
    }

    // refused: a short table means the sheet does not match the rule, and reading past it draws garbage.
    u32 required = kind == NYA_TILEMAP_AUTOTILE_EDGES ? 16U : 47U;
    if (lookup_length < required) {
        return nya_error(NYA_ERROR_INVALID_ARGUMENT, "auto-tiling needs " FMTu32 " lookup entries, got " FMTu32, required, lookup_length);
    }

    /* A snapshot, since the walk reads and writes the layer. */
    NYA_Arena scratch    = nya_arena_create_on_stack(.name = "tilemap_autotile");
    defer     nya_arena_destroy_on_stack(&scratch);

    u64 cell_count = (u64)layer->width * (u64)layer->height;

    u32* snapshot = nya_arena_alloc(&scratch, cell_count * sizeof(u32));
    nya_memcpy(snapshot, layer->tiles, cell_count * sizeof(u32));

    _NYA_TilemapAutoTileContext context = {
        .tiles         = snapshot,
        .width         = layer->width,
        .height        = layer->height,
        .out_of_bounds = out_of_bounds,
    };

    for (u32 y = 0; y < layer->height; y++) {
        for (u32 x = 0; x < layer->width; x++) {
            u32 existing = snapshot[((u64)y * layer->width) + x];

            // empty stays empty: auto-tiling picks variants, not which cells are filled.
            if ((existing & NYA_TILEMAP_GID_MASK) == 0) continue;

            u32 variant = nya_tilemap_autotile_mask(_nya_tilemap_autotile_filled, &context, (s32)x, (s32)y, kind);

            // flip bits preserved.
            (void)nya_tilemap_tile_set(map, layer_index, (s32)x, (s32)y, lookup[variant] | (existing & ~NYA_TILEMAP_GID_MASK));
        }
    }

    return NYA_OK;
}

const NYA_TilemapTileset* _nya_tilemap_tileset_for(const NYA_Tilemap* map, u32 gid) {
    const NYA_TilemapTileset* best = nullptr;

    // the tileset with the largest first_gid not above this id. walked: at most NYA_TILEMAP_MAX_TILESETS, per
    // visible tile, where the branch predictor beats a search.
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

    /* The map's directory, as the asset handle spells it. */
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

        // after the tileset is filled in, since a warning names the sheet.
        _nya_tilemap_parse_animations(map, object, &parsed[i]);

        /* Queued here rather than left to the caller. */
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

/**
 * The most animations any tileset has had. NYA_TILEMAP_MAX_ANIMATIONS is per tileset, so the ceiling registry
 * watches the busiest, as with _nya_render2d_glyph_count_worst.
 * */
NYA_INTERNAL u32 _nya_tilemap_animation_count_worst = 0;

void _nya_tilemap_parse_animations(NYA_Tilemap* map, const NYA_Object* tileset_object, OUT NYA_TilemapTileset* out_tileset) {
    out_tileset->animations      = nullptr;
    out_tileset->animation_count = 0;

    NYA_Value* tiles = nya_object_get((NYA_Object*)tileset_object, "tiles");
    if (tiles == nullptr || tiles->type != NYA_TYPE_ARRAY) return;

    // counted first so the array is allocated once; the arena has no realloc.
    u32 animated = 0;

    for (u64 i = 0; i < tiles->as_array.length; i++) {
        NYA_Value* entry = &tiles->as_array.items[i];
        if (entry->type != NYA_TYPE_OBJECT) continue;

        NYA_Value* animation = nya_object_get(&entry->as_object, "animation");
        if (animation != nullptr && animation->type == NYA_TYPE_ARRAY && animation->as_array.length > 0) animated++;
    }

    if (animated == 0) return;

    if (animated > NYA_TILEMAP_MAX_ANIMATIONS) {
        nya_log_warn("tileset '%s' defines %u animated tiles; only %d are kept; raise NYA_TILEMAP_MAX_ANIMATIONS",
                     out_tileset->name != nullptr ? out_tileset->name : "(unnamed)", animated, NYA_TILEMAP_MAX_ANIMATIONS);
        animated = NYA_TILEMAP_MAX_ANIMATIONS;
    }

    NYA_TilemapAnimation* parsed = nya_arena_alloc(map->allocator, animated * sizeof(NYA_TilemapAnimation));

    u32 kept = 0;

    for (u64 i = 0; i < tiles->as_array.length && kept < animated; i++) {
        NYA_Value* entry = &tiles->as_array.items[i];
        if (entry->type != NYA_TYPE_OBJECT) continue;

        NYA_Value* frames = nya_object_get(&entry->as_object, "animation");
        if (frames == nullptr || frames->type != NYA_TYPE_ARRAY || frames->as_array.length == 0) continue;

        NYA_TilemapAnimation* animation = &parsed[kept];

        *animation = (NYA_TilemapAnimation){ .local_id = (u32)_nya_tilemap_integer(&entry->as_object, "id", 0) };

        for (u64 f = 0; f < frames->as_array.length && animation->frame_count < NYA_TILEMAP_MAX_ANIMATION_FRAMES; f++) {
            NYA_Value* frame = &frames->as_array.items[f];
            if (frame->type != NYA_TYPE_OBJECT) continue;

            // Tiled stores milliseconds; converted so nothing downstream has to remember.
            f32 duration_s = (f32)_nya_tilemap_integer(&frame->as_object, "duration", 100) / 1000.0F;

            // a zero duration would make the total zero; it gets the shortest duration Tiled can express.
            if (duration_s <= 0.0F) duration_s = 0.001F;

            animation->frames[animation->frame_count++] = (NYA_TilemapAnimationFrame){
                .local_id   = (u32)_nya_tilemap_integer(&frame->as_object, "tileid", 0),
                .duration_s = duration_s,
            };

            animation->total_duration_s += duration_s;
        }

        if (animation->frame_count == 0) continue;

        kept++;
    }

    out_tileset->animations      = parsed;
    out_tileset->animation_count = kept;

    if (kept > _nya_tilemap_animation_count_worst) _nya_tilemap_animation_count_worst = kept;

    // registered once, on the first tileset with animations.
    static b8 ceiling_registered = false;
    if (!ceiling_registered) {
        nya_ceiling_register("tilemap_animations", NYA_TILEMAP_MAX_ANIMATIONS, &_nya_tilemap_animation_count_worst);
        ceiling_registered = true;
    }
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
                // a string is base64, Tiled's other encoding. named, or the layer loads empty with no explanation.
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

            // unfilled cells stay empty, so a truncated file shows holes rather than garbage.
            for (u32 cell = available; cell < cells; cell++) tiles[cell] = 0;

            layer.tiles = tiles;
        } else if (nya_string_equals(type, "objectgroup")) {
            layer.kind = NYA_TILEMAP_LAYER_OBJECTS;
            NYA_TRY(_nya_tilemap_parse_objects(map, object, &layer));
        } else {
            // an image layer or a group: skipped, since neither carries anything a game reads.
            nya_log_debug("Skipping tilemap layer '%s' of unsupported type '%s'.", layer.name, type);
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
            // recent Tiled says "class", older "type"; both are read.
            .type     = _nya_tilemap_string(map->allocator, object, "class",
                                            _nya_tilemap_string(map->allocator, object, "type", nullptr)),
            .position = { (f32)_nya_tilemap_real(object, "x", 0.0), (f32)_nya_tilemap_real(object, "y", 0.0) },
            .size     = { (f32)_nya_tilemap_real(object, "width", 0.0), (f32)_nya_tilemap_real(object, "height", 0.0) },
            .rotation = (f32)_nya_tilemap_real(object, "rotation", 0.0),
        };

        // object coordinates in an isometric map are already projected, unlike tile coordinates, which are grid
        // indices.

        NYA_Value* properties = nya_object_get(object, "properties");
        if (properties == nullptr || properties->type != NYA_TYPE_ARRAY) continue;

        u32 property_count = (u32)nya_min(properties->as_array.length, (u64)NYA_TILEMAP_MAX_PROPERTIES);

        NYA_TilemapProperty* parsed_properties = nya_arena_alloc(map->allocator, property_count * sizeof(NYA_TilemapProperty));

        for (u32 j = 0; j < property_count; j++) {
            NYA_Value* property_entry = &properties->as_array.items[j];
            if (property_entry->type != NYA_TYPE_OBJECT) continue;

            NYA_Object* property = &property_entry->as_object;

            // filled as every type: Tiled writes a float of one as `1`, so the caller reads the type it meant.
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
        /* The four screen corners, turned back into tile coordinates. */
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

        // widened by two: an isometric tile can stick up into view from an off-screen origin.
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

    /* The one place this file writes through `const`. */
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

    // row by row, since the stride changes; a flat copy would shear the map.
    if (layer->tiles != nullptr) {
        u32 rows    = layer->height < height ? layer->height : height;
        u32 columns = layer->width < width ? layer->width : width;

        for (u32 row = 0; row < rows; row++) {
            nya_memcpy(tiles + ((u64)row * width), layer->tiles + ((u64)row * layer->width), sizeof(u32) * columns);
        }
    }

    /* The old array is not freed; an arena has no per-allocation free. */
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

    // the fields Tiled writes and this loader reads, including its version keys.
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

    // tilesets
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

    // layers
    NYA_ArrayᐸNYA_Valueᐳ* layers = nya_array_create(arena, NYA_Value);

    for (u32 i = 0; i < map->layer_count; i++) {
        const NYA_TilemapLayer* layer = &map->layers[i];

        // object layers are not written back: an editor that cannot author them yet would write a half-understood
        // layer. tile layers round trip.
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

        // raw gids with flip bits, row major, uncompressed: this loader does not read base64 or zlib.
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

    /* JSON explicitly, not nya_serde_save_file. */
    NYA_String* text = nya_serialize(scratch, root, NYA_SERDE_FORMAT_JSON, NYA_SERDE_PRETTY);
    if (text == nullptr) return nya_error(NYA_ERROR_NOT_OK, "could not serialize the map for '%s'", path);

    return nya_file_write_atomic(path, text);
}
