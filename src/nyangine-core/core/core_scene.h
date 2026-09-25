/**
 * @file core_scene.h
 *
 * A whole world to a file and back: every entity's identity, transform, hierarchy, motion, flags,
 * type, name, appearance and light. What a save slot holds and what an editor opens.
 *
 * ```c
 * NYA_EXPECT(nya_scene_save(nya_world(), "slot0.nya", NYA_SAVE_FLAGS_DATA));
 *
 * // Later, or in another run. Everything the world holds is despawned first.
 * NYA_EXPECT(nya_scene_load(nya_world(), "slot0.nya", NYA_SAVE_FLAGS_DATA));
 * ```
 *
 * ─────────────────────────────────────────────────────────
 * WHAT A SCENE DOES NOT CARRY, AND WHY
 * ─────────────────────────────────────────────────────────
 *
 * Roughly half of NYA_Entity is state that exists only within one run of one process, and none of it
 * can be written to a file honestly:
 *
 * - `user_data` is a raw pointer into the game's own memory.
 * - the eight `on_*` fields are NYA_CallbackHandle, and a callback is keyed by the address of the
 *   function it was registered with, so the handle a save wrote is meaningless the next time the
 *   program is laid out in memory, let alone the next time it is built.
 * - `physics2d` and `physics3d` are live bodies owned by a solver that no longer exists.
 * - `move_tween` is a running interpolation, `physics` owns the transform while attached, and the
 *   animator borrows an NYA_SpriteAnimation the game owns as static data.
 *
 * So a scene is written from NYA_SceneEntity, a type whose every field can come out of a file,
 * rather than from NYA_Entity with nine `@skip` annotations on it. Three reasons, in order:
 *
 * 1. A parsed type that can only exist if the input was good beats a validated one. Nothing
 *    downstream of NYA_SceneEntity has to remember which of NYA_Entity's fields a file was allowed
 *    to touch, because the ones it was not allowed to touch are not in the type.
 * 2. Ownership. A record owns its text in `char` arrays of its own, so a loaded scene does not leave
 *    entity names and texture handles pointing into a parse arena the caller is about to release.
 *    `NYA_Entity.name` is a borrowed `NYA_ConstCString` and reflection would copy the pointer.
 * 3. The file's schema is the record, not the live struct. Reordering or renaming a field of
 *    NYA_Entity for a runtime reason then cannot silently change what every existing save means.
 *
 * The cost is one total conversion each way, nya_scene_entity_from and nya_scene_entity_to, which is
 * the same bargain a DTO makes at any other boundary.
 *
 * **Behaviour is the game's to re-attach.** A loaded entity comes back with its `type`, its `flags`
 * and its name, and with no callbacks, no rigid body and no animation. The game walks the world after
 * a load, switches on `type`, and binds the callbacks and attaches the bodies that kind of thing has:
 *
 * ```c
 * NYA_EXPECT(nya_scene_load(nya_world(), "slot0.nya", NYA_SAVE_FLAGS_DATA));
 *
 * nya_entity_foreach (entity) {
 *     switch (entity->type) {
 *         case GNY_ENTITY_ROBOT: gny_entity_robot_attach(entity); break;
 *         case GNY_ENTITY_BOX:   gny_entity_box_attach(entity); break;
 *         default:               break;
 *     }
 * }
 * ```
 * */
#pragma once

#include "nyangine-std/base/base_arena.h"
#include "nyangine-std/base/base_attributes.h"
#include "nyangine-std/base/base_error.h"
#include "nyangine-std/base/base_object.h"
#include "nyangine-core/core/core_entity.h"
#include "nyangine-core/core/core_world.h"
#include "nyangine-std/serde/serde_types.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * CONSTANTS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/**
 * The version written into every scene, and what a loader checks before trusting the shape. Bumped
 * when the meaning of a field changes; a field merely added needs no bump, since a record left out of
 * an older file keeps the value the fresh record had.
 * */
#define NYA_SCENE_VERSION 1

/** Where the entities live in the document. One string, so a rename is one edit. */
#define NYA_SCENE_ENTITIES_KEY "entities"

/**
 * Longest entity name a scene carries, terminator included. Entity names are debug labels and
 * lookup keys, not prose; the longest in the tree is under twenty bytes. A longer one is truncated on
 * a character boundary and the save says so.
 * */
#define NYA_SCENE_NAME_MAX 64

/**
 * Longest asset handle a scene carries, terminator included. Handles are paths under assets/, and the
 * deepest in the tree is around fifty bytes.
 * */
#define NYA_SCENE_ASSET_MAX 128

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * TYPES
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

typedef struct NYA_SceneVisual NYA_SceneVisual;
typedef struct NYA_SceneEntity NYA_SceneEntity;

// @reflect
/**
 * The persistable part of NYA_EntityVisual: flat, owning its own text, and carrying no animator.
 *
 * The animator is left out on purpose. It holds a borrowed pointer to an NYA_SpriteAnimation the game
 * owns as static data, and a frame index into an animation nothing in the file names is not state
 * worth restoring. A loaded entity is not mid-animation; the game starts it playing again from
 * `type`, which is where the animation table lives anyway.
 * */
struct NYA_SceneVisual {
    NYA_EntityVisualKind kind;

    /* SPRITE and ANIMATION */

    /** The texture asset, or empty. */
    char sprite[NYA_SCENE_ASSET_MAX]; // @hint(asset)

    /** The part of the texture to draw, in its pixels: x, y, width, height. Zero width or height is the whole thing. */
    f32x4 source;

    /** Where the pivot sits within the sprite, as a fraction of its size. */
    f32x2 origin;

    f32x2 sprite_scale;  // @hint(scale)
    f32   sprite_rotation;

    b8 flip_x;
    b8 flip_y;

    NYA_Color tint; // @hint(color)

    /* ANIMATION: the sheet the frames are cut from */

    char atlas[NYA_SCENE_ASSET_MAX]; // @hint(asset)

    u32 frame_width;
    u32 frame_height;
    u32 columns;
    u32 rows;
    u32 spacing;
    u32 margin;

    /* CUBE */

    f32x3     size; // @hint(scale)
    NYA_Color color; // @hint(color)

    /* draw order */

    f32 z_order;
    b8  y_sorted;
    f32 y_sort_anchor;
};

// @reflect
/**
 * One entity as a file holds it. Every field here can come out of a document, which is the whole
 * point of the type; see the note at the top of this file for what NYA_Entity carries that cannot.
 * */
struct NYA_SceneEntity {
    /**
     * The scene's own name for this entity, which is all its hierarchy needs. Not a handle: a
     * handle's generation counter is about one run of one table.
     *
     * nya_scene_entity_from fills it with the entity's slot, which is unique within a live world.
     * nya_scene_to_object then renumbers a whole scene to 0..n-1 in the order it writes them, so the
     * document does not depend on which slots the table happened to hand out, and the same world
     * saved twice is the same bytes twice.
     * */
    u32 id;

    /** The `id` of this entity's parent. Only meaningful when `parented`. */
    u32 parent_id;

    /**
     * Whether `parent_id` names anything. A flag rather than a reserved id, so no legal id has to be
     * kept out of use to mean "none".
     * */
    b8 parented;

    /** Truncated on a character boundary if the entity's name is longer. See NYA_SCENE_NAME_MAX. */
    char name[NYA_SCENE_NAME_MAX];

    /** Game defined; the engine never interprets it. What the game switches on to re-attach behaviour. */
    u32 type;

    /** Game defined. See NYA_Entity.flags. */
    u64 flags;

    /** Never carries NYA_ENTITY_STATE_DESPAWNING: a scene holds entities, not pending removals. */
    NYA_EntityState state;

    /* the world transform, which is what physics writes and everything reads */

    f32x3          position; // @hint(position)
    NYA_Quaternion rotation;
    f32x3          scale; // @hint(scale)

    f32x3 velocity;
    f32x3 angular_velocity;

    NYA_SceneVisual visual;

    /** What it emits; zeroed emits nothing. See NYA_Light2D. */
    NYA_Light2D light;
};

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * FUNCTIONS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/*
 * ─────────────────────────────────────────────────────────
 * ONE ENTITY
 * ─────────────────────────────────────────────────────────
 */

/**
 * The record for a live entity. Total: every entity produces a record, and a record is produced from
 * nothing else. Writes nothing and allocates nothing.
 * */
NYA_API void nya_scene_entity_from(OUT NYA_SceneEntity* out_record, const NYA_Entity* entity);

/**
 * The inverse: the spawn options a record describes. `arena` holds the name and the asset handles,
 * which the entity borrows rather than owns, so it has to outlive the entity; the world's own
 * allocator is the arrangement that makes that true by construction.
 *
 * Does not spawn. An editor that wants to change something before the entity exists edits the options
 * it gets back, and nya_scene_from_object is the caller that does spawn.
 * */
NYA_API void nya_scene_entity_to(const NYA_SceneEntity* record, NYA_Arena* arena, OUT NYA_EntitySpawnOptions* out_options);

/*
 * ─────────────────────────────────────────────────────────
 * A WHOLE WORLD
 * ─────────────────────────────────────────────────────────
 */

/**
 * Every live entity of `world` as a document: a version and a list of records, each written through
 * NYA_SceneEntity's own description.
 *
 * Canonical. The records are renumbered 0..n-1 as they are written, so the same world produces the
 * same bytes however its slots were handed out, and a document loaded into a *fresh* world and
 * written again is the document it came from. Loading into a world that has held entities reuses
 * freed slots, so the order of the list that comes back out follows the table rather than the file;
 * what the world holds is the same either way.
 *
 * Everything comes from `arena`. Null only when `world` is null.
 * */
NYA_API NYA_Object* nya_scene_to_object(NYA_Arena* arena, NYA_World* world) __attr_no_discard;

/**
 * The inverse, into `world`, which is emptied first: loading a scene replaces what a world holds
 * rather than merging into it, because a save is a whole world and a half-loaded one is nobody's
 * intent. on_despawn runs for everything that was there.
 *
 * A record the document cannot supply is named and skipped rather than taken as evidence the file is
 * ruined, so one bad entity costs that entity. A version newer than NYA_SCENE_VERSION is refused
 * outright, since a field whose meaning changed cannot be recognised by inspection.
 * */
NYA_API NYA_Error nya_scene_from_object(NYA_World* world, const NYA_Object* object) __attr_no_discard;

/**
 * nya_scene_to_object written under the save root, atomically. See core_save.h for `relative` and for
 * NYA_SAVE_FLAGS_DATA, which is what a save slot wants.
 * */
NYA_API NYA_Error nya_scene_save(NYA_World* world, NYA_ConstCString relative, NYA_SerdeFlags flags) __attr_no_discard;

/**
 * The inverse. NYA_ERROR_NOT_FOUND when there is no such file, which is the ordinary first-run case;
 * NYA_ERROR_CORRUPT when the file exists and its checksum does not match what it holds.
 * */
NYA_API NYA_Error nya_scene_load(NYA_World* world, NYA_ConstCString relative, NYA_SerdeFlags flags) __attr_no_discard;
