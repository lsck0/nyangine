#include "nyangine-core/nyangine.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API DECLARATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/**
 * What one saved slot turns into. Indexed by NYA_SceneEntity.slot, which is why it is sized by the
 * table rather than by the file: a scene's slots are sparse, and a file that names slot 8000 with
 * three entities in it is ordinary. NYA_ENTITY_MAX of these is 128 KB out of a scratch arena that is
 * released before the load returns.
 * */
typedef struct {
    /** Where the record was spawned, or NYA_ENTITY_HANDLE_NONE when the file did not use this id. */
    NYA_EntityHandle spawned;

    u32 parent_id;
    b8  parented;
} _NYA_SceneSlot;

/**
 * A live slot's place in the document being written. Sized by the table for the same reason
 * _NYA_SceneSlot is: a world's live slots are sparse.
 * */
typedef struct {
    u32 id;
    b8  assigned;
} _NYA_SceneIndex;

/**
 * Copies `text` into `out`, truncated on a character boundary, and says so when it had to cut.
 * */
NYA_INTERNAL void _nya_scene_text_copy(OUT char* out, u64 capacity, NYA_ConstCString text, NYA_ConstCString what);

/** The arena's own copy of `text`, or null when it is empty. Null is what "no asset" means downstream. */
NYA_INTERNAL NYA_ConstCString _nya_scene_text_intern(NYA_Arena* arena, NYA_ConstCString text) __attr_no_discard;

/** Turns one finding from nya_reflect_check into a warning naming the entity it came from. */
NYA_INTERNAL void _nya_scene_report(NYA_ConstCString path, NYA_ConstCString found, NYA_ConstCString expected, void* user_data);

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PUBLIC API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/*
 * ─────────────────────────────────────────────────────────
 * ONE ENTITY
 * ─────────────────────────────────────────────────────────
 */

void nya_scene_entity_from(OUT NYA_SceneEntity* out_record, const NYA_Entity* entity) {
    nya_assert(out_record != nullptr);
    nya_assert(entity != nullptr);

    const NYA_EntityVisual* visual = &entity->visual;

    *out_record = (NYA_SceneEntity){
        // The live slot, which is unique within a world. nya_scene_to_object renumbers a whole scene
        // into its own 0..n-1; see NYA_SceneEntity.id.
        .id = entity->handle.index,

        // By slot, not by handle: the parent's generation counter belongs to the table this entity is
        // leaving, and the file's own hierarchy only has to agree with itself.
        .parent_id = entity->parent.index,
        .parented  = nya_entity_is_valid(entity->parent),

        .type  = entity->type,
        .flags = entity->flags,

        // DESPAWNING is a request waiting for the simulation barrier, not something an entity *is*.
        // Written out it would come back as an entity that removes itself on the first tick after a load.
        .state = (NYA_EntityState)(entity->state & ~NYA_ENTITY_STATE_DESPAWNING),

        .position = entity->position,
        .rotation = entity->rotation,
        .scale    = entity->scale,

        .velocity         = entity->velocity,
        .angular_velocity = entity->angular_velocity,

        .visual = {
            .kind = visual->kind,

            .source = (f32x4){ visual->sprite.source_x, visual->sprite.source_y, visual->sprite.source_width, visual->sprite.source_height },

            .origin          = visual->sprite.origin,
            .sprite_scale    = visual->sprite.scale,
            .sprite_rotation = visual->sprite.rotation,
            .flip_x          = visual->sprite.flip_x,
            .flip_y          = visual->sprite.flip_y,
            .tint            = visual->sprite.tint,

            .frame_width  = visual->atlas.frame_width,
            .frame_height = visual->atlas.frame_height,
            .columns      = visual->atlas.columns,
            .rows         = visual->atlas.rows,
            .spacing      = visual->atlas.spacing,
            .margin       = visual->atlas.margin,

            .size  = visual->size,
            .color = visual->color,

            .z_order       = visual->z_order,
            .y_sorted      = visual->y_sorted,
            .y_sort_anchor = visual->y_sort_anchor,
        },

        .light = entity->light,
    };

    _nya_scene_text_copy(out_record->name, sizeof(out_record->name), entity->name, "an entity name");
    _nya_scene_text_copy(out_record->visual.sprite, sizeof(out_record->visual.sprite), visual->sprite.texture, "a sprite texture");
    _nya_scene_text_copy(out_record->visual.atlas, sizeof(out_record->visual.atlas), visual->atlas.texture, "an atlas texture");
}

void nya_scene_entity_to(const NYA_SceneEntity* record, NYA_Arena* arena, OUT NYA_EntitySpawnOptions* out_options) {
    nya_assert(record != nullptr);
    nya_assert(arena != nullptr);
    nya_assert(out_options != nullptr);

    const NYA_SceneVisual* visual = &record->visual;

    *out_options = (NYA_EntitySpawnOptions){
        .name  = _nya_scene_text_intern(arena, record->name),
        .state = record->state,
        .type  = record->type,
        .flags = record->flags,

        .position = record->position,
        .rotation = record->rotation,
        .scale    = record->scale,

        .velocity         = record->velocity,
        .angular_velocity = record->angular_velocity,

        .visual = {
            .kind = visual->kind,

            .sprite = {
                .texture       = _nya_scene_text_intern(arena, visual->sprite),
                .source_x      = visual->source[0],
                .source_y      = visual->source[1],
                .source_width  = visual->source[2],
                .source_height = visual->source[3],
                .origin        = visual->origin,
                .scale         = visual->sprite_scale,
                .rotation      = visual->sprite_rotation,
                .flip_x        = visual->flip_x,
                .flip_y        = visual->flip_y,
                .tint          = visual->tint,
            },

            .atlas = {
                .texture      = _nya_scene_text_intern(arena, visual->atlas),
                .frame_width  = visual->frame_width,
                .frame_height = visual->frame_height,
                .columns      = visual->columns,
                .rows         = visual->rows,
                .spacing      = visual->spacing,
                .margin       = visual->margin,
            },

            .size  = visual->size,
            .color = visual->color,

            .z_order       = visual->z_order,
            .y_sorted      = visual->y_sorted,
            .y_sort_anchor = visual->y_sort_anchor,
        },

        .light = record->light,
    };
}

/*
 * ─────────────────────────────────────────────────────────
 * A WHOLE WORLD
 * ─────────────────────────────────────────────────────────
 */

NYA_Object* nya_scene_to_object(NYA_Arena* arena, NYA_World* world) {
    nya_assert(arena != nullptr);

    if (world == nullptr) return nullptr;

    // The entity calls all operate on the current world, so the world being written becomes it for the
    // length of the walk and the previous one is put back. Cheaper and less surprising than a second
    // set of per-world entity accessors.
    NYA_World* previous = nya_world_set(world);
    defer      (void)nya_world_set(previous);

    NYA_Object* root = nya_object_create(arena);

    nya_object_add(root, NYA_SAVE_VERSION_KEY, (NYA_Value){ .type = NYA_TYPE_U32, .as_u32 = NYA_SCENE_VERSION });

    NYA_ArrayᐸNYA_Valueᐳ* entities = nya_array_create(arena, NYA_Value);

    // Two passes, because a record names its parent and a parent may be written after its child.
    // Sized by the table rather than by the count, since live slots are sparse.
    _NYA_SceneIndex* index = nya_arena_alloc(arena, sizeof(_NYA_SceneIndex) * NYA_ENTITY_MAX);
    nya_memset(index, 0, sizeof(_NYA_SceneIndex) * NYA_ENTITY_MAX);

    u32 next_id = 0;

    nya_entity_foreach (entity) {
        index[entity->handle.index] = (_NYA_SceneIndex){ .id = next_id, .assigned = true };
        next_id++;
    }

    nya_entity_foreach (entity) {
        NYA_SceneEntity record = { 0 };
        nya_scene_entity_from(&record, entity);

        record.id = index[record.id].id;

        // A parent that is not in the scene cannot be referred to, so the child is written as a root
        // rather than as a reference to nothing. nya_entity_is_valid already made this all but
        // impossible; it is here so the document cannot be wrong even if it becomes possible.
        if (record.parented && index[record.parent_id].assigned) {
            record.parent_id = index[record.parent_id].id;
        } else {
            record.parented  = false;
            record.parent_id = 0;
        }

        NYA_Object* written = nya_reflect_to_object(arena, nya_reflect_of(NYA_SceneEntity), &record);
        if (written == nullptr) continue;

        nya_array_push_back(entities, ((NYA_Value){ .type = NYA_TYPE_OBJECT, .as_object = *written }));
    }

    nya_object_add(root, NYA_SCENE_ENTITIES_KEY, (NYA_Value){ .type = NYA_TYPE_ARRAY, .as_array = *entities });

    return root;
}

NYA_Error nya_scene_from_object(NYA_World* world, const NYA_Object* object) {
    if (world == nullptr) return nya_error(NYA_ERROR_INVALID_ARGUMENT, "no world to load into");
    if (object == nullptr) return nya_error(NYA_ERROR_INVALID_ARGUMENT, "no document to load");

    u32 version = nya_save_version(object);

    // Refused rather than loaded for what it can: a version bump means a field's meaning changed, and
    // nothing in the document says which one. See NYA_SCENE_VERSION.
    if (version > NYA_SCENE_VERSION) {
        return nya_error(NYA_ERROR_NOT_SUPPORTED, "the scene is version " FMTu32 ", newer than the " FMTu32 " this build understands",
                         version, (u32)NYA_SCENE_VERSION);
    }

    NYA_Value* entities = nya_object_get(object, NYA_SCENE_ENTITIES_KEY);
    if (entities == nullptr) return nya_error(NYA_ERROR_PARSE, "the scene has no '%s'", NYA_SCENE_ENTITIES_KEY);
    if (entities->type != NYA_TYPE_ARRAY) return nya_error(NYA_ERROR_PARSE, "the scene's '%s' is not a list", NYA_SCENE_ENTITIES_KEY);

    NYA_World* previous = nya_world_set(world);
    defer      (void)nya_world_set(previous);

    NYA_Arena* scratch = nya_arena_create(.name = "scene_from_object");
    defer      nya_arena_destroy(scratch);

    _NYA_SceneSlot* slots = nya_arena_alloc(scratch, sizeof(_NYA_SceneSlot) * NYA_ENTITY_MAX);
    nya_memset(slots, 0, sizeof(_NYA_SceneSlot) * NYA_ENTITY_MAX);

    // Emptied before the first spawn, so a failure part way through leaves a world that is short of
    // entities rather than one holding two overlapping scenes. See the header.
    nya_entity_clear();

    u32 index    = 0;
    u32 refused  = 0;

    nya_array_foreach (&entities->as_array, element) {
        u32 at = index;
        index++;

        if (element->type != NYA_TYPE_OBJECT) {
            nya_log_warn("Scene entity " FMTu32 " is not an object; skipping it.", at);
            refused++;
            continue;
        }

        // Fresh per record, so a field the document omits comes back as the zero the engine would
        // have spawned with rather than as the previous entity's value.
        NYA_SceneEntity record = { 0 };

        (void)nya_reflect_check(nya_reflect_of(NYA_SceneEntity), &element->as_object, _nya_scene_report, &at);

        NYA_Error read = nya_reflect_from_object(nya_reflect_of(NYA_SceneEntity), &record, &element->as_object);
        if (!read.ok) {
            nya_log_warn("Scene entity " FMTu32 " could not be read; skipping it.", at);
            refused++;
            continue;
        }

        if (record.id >= NYA_ENTITY_MAX) {
            nya_log_warn("Scene entity " FMTu32 " calls itself " FMTu32 ", past the " FMTu32 " entities this build holds; skipping it.",
                         at, record.id, (u32)NYA_ENTITY_MAX);
            refused++;
            continue;
        }

        if (nya_entity_is_valid(slots[record.id].spawned)) {
            nya_log_warn("Scene entity " FMTu32 " calls itself " FMTu32 ", which another entity already answers to; skipping it.", at,
                         record.id);
            refused++;
            continue;
        }

        // The world's own arena, so the name and the asset handles live exactly as long as the entity
        // that borrows them. See nya_scene_entity_to.
        NYA_EntitySpawnOptions options = { 0 };
        nya_scene_entity_to(&record, world->allocator, &options);

        NYA_EntityHandle spawned = nya_entity_spawn_with_options(options);

        if (!nya_entity_is_valid(spawned)) {
            nya_log_warn("Scene entity " FMTu32 " could not be spawned; skipping it.", at);
            refused++;
            continue;
        }

        slots[record.id] = (_NYA_SceneSlot){ .spawned = spawned, .parent_id = record.parent_id, .parented = record.parented };
    }

    // Second pass, because a child may be listed before its parent and parenting needs both to exist.
    // nya_entity_parent_set keeps the world transform and derives the local one, which is why the
    // local transform is not in the record at all: it is a value with one source of truth.
    for (u32 id = 0; id < NYA_ENTITY_MAX; id++) {
        if (!slots[id].parented) continue;
        if (!nya_entity_is_valid(slots[id].spawned)) continue;

        u32 parent_id = slots[id].parent_id;

        if (parent_id >= NYA_ENTITY_MAX || !nya_entity_is_valid(slots[parent_id].spawned)) {
            nya_log_warn("Scene entity " FMTu32 " names " FMTu32 " as its parent, which the scene does not hold; leaving it a root.", id,
                         parent_id);
            continue;
        }

        (void)nya_entity_parent_set(slots[id].spawned, slots[parent_id].spawned);
    }

    if (refused > 0) nya_log_warn("The scene held " FMTu32 " entit%s this build could not load.", refused, refused == 1 ? "y" : "ies");

    return NYA_OK;
}

NYA_Error nya_scene_save(NYA_World* world, NYA_ConstCString relative, NYA_SerdeFlags flags) {
    nya_assert(relative != nullptr);

    if (world == nullptr) return nya_error(NYA_ERROR_INVALID_ARGUMENT, "no world to save");

    NYA_Arena* scratch = nya_arena_create(.name = "scene_save");
    defer      nya_arena_destroy(scratch);

    NYA_Object* root = nya_scene_to_object(scratch, world);
    if (root == nullptr) return nya_error(NYA_ERROR_NOT_OK, "could not build the scene document");

    return nya_save_write(relative, root, flags);
}

NYA_Error nya_scene_load(NYA_World* world, NYA_ConstCString relative, NYA_SerdeFlags flags) {
    nya_assert(relative != nullptr);

    if (world == nullptr) return nya_error(NYA_ERROR_INVALID_ARGUMENT, "no world to load into");

    NYA_Arena* scratch = nya_arena_create(.name = "scene_load");
    defer      nya_arena_destroy(scratch);

    NYA_Object* root = nullptr;

    // Read whole before the world is touched: a missing or corrupt file leaves the world exactly as it
    // was, which is what lets a game offer "that slot is broken, keep playing" instead of an empty map.
    NYA_TRY(nya_save_read(scratch, relative, flags, &root));

    return nya_scene_from_object(world, root);
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

void _nya_scene_text_copy(OUT char* out, u64 capacity, NYA_ConstCString text, NYA_ConstCString what) {
    nya_assert(out != nullptr);
    nya_assert(capacity > 0);

    out[0] = '\0';

    if (text == nullptr) return;

    u64 length = strlen(text);

    if (length >= capacity) {
        length = capacity - 1;

        // A cut mid-character would store invalid UTF-8; back off the continuation bytes.
        while (length > 0 && ((u8)text[length] & 0xC0) == 0x80) length--;

        nya_log_warn("Scene: %s is longer than the " FMTu64 " bytes a scene carries and was cut: '%s'.", what, capacity - 1, text);
    }

    nya_memcpy(out, text, length);
    out[length] = '\0';
}

NYA_ConstCString _nya_scene_text_intern(NYA_Arena* arena, NYA_ConstCString text) {
    if (text == nullptr || text[0] == '\0') return nullptr;

    u64   length = strlen(text);
    char* copy   = nya_arena_alloc(arena, length + 1);

    nya_memcpy(copy, text, length);
    copy[length] = '\0';

    return copy;
}

void _nya_scene_report(NYA_ConstCString path, NYA_ConstCString found, NYA_ConstCString expected, void* user_data) {
    nya_log_warn("Scene entity " FMTu32 ": '%s' is %s, expected %s; leaving it at its default.", *(const u32*)user_data, path, found,
                 expected);
}
