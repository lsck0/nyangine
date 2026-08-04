/**
 * @file core_entity.h
 *
 * Entities: one flat struct per thing in the world, addressed by generational handle.
 *
 * Deliberately not an ECS. There are no components, no archetypes and no queries — every entity is
 * the same fat struct and unused fields simply sit there. For a game of this size that trades a
 * little memory for the ability to read a whole entity in one place, and it can be replaced later
 * without changing how anything refers to an entity, because references are handles rather than
 * pointers or indices.
 *
 * ```c
 * NYA_EntityHandle player = nya_entity_spawn(.name = "player", .position = { 0, 1, 0 });
 *
 * NYA_Entity* entity = nya_entity_get(player);   // null once it is despawned
 * if (entity) entity->velocity.y -= 9.81F * delta_time_s;
 *
 * nya_entity_despawn_deferred(player);           // applied at the simulation barrier
 * ```
 *
 * **Borrow pointers, hold handles.** `NYA_Entity*` stays valid for as long as that entity lives,
 * because the table never moves, but it says nothing about whether the entity still exists. Anything
 * that outlives the current scope — a deferred command, a field on another entity — stores a handle.
 * */
#pragma once

#include "nyangine/core/core_callback.h"
#include "nyangine/core/core_event.h"
#include "nyangine/core/core_types.h"
#include "nyangine/math/math_quaternion.h"
#include "nyangine/math/math_vector.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * CONSTANTS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/**
 * Entity slots, allocated up front.
 *
 * Fixed rather than growable so an NYA_Entity* stays put.
 *
 * Handles already cover identity and lifetime, so a reallocating array would be safe *across*
 * frames. What it would not be safe for is a single callback body: on_update is handed a raw
 * NYA_Entity*, and code as ordinary as "spawn a bullet, then set my own cooldown" would be a use
 * after free whenever that spawn crossed a growth boundary. Silent, and only sometimes.
 *
 * Override with -DNYA_ENTITY_MAX=<n> per game. Costs sizeof(NYA_Entity) x n up front, allocated
 * once from the entity arena. If a hard ceiling is the wrong shape, the answer is chunked blocks
 * rather than one reallocating array: those grow without ever moving what already exists.
 * */
#ifndef NYA_ENTITY_MAX
#define NYA_ENTITY_MAX 8192
#endif

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * TYPES
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

typedef enum NYA_EntityFlag           NYA_EntityFlag;
typedef struct NYA_Entity             NYA_Entity;
typedef struct NYA_EntitySystem       NYA_EntitySystem;
typedef struct NYA_EntitySpawnOptions NYA_EntitySpawnOptions;

enum NYA_EntityFlag {
    NYA_ENTITY_FLAG_NONE = 0,

    /** Cleared to leave an entity in the world but skip its update. */
    NYA_ENTITY_FLAG_ACTIVE = 1 << 0,

    /** Cleared to keep simulating an entity that is not drawn. */
    NYA_ENTITY_FLAG_VISIBLE = 1 << 1,

    /** Never moves. A hint for whatever ends up doing spatial partitioning. */
    NYA_ENTITY_FLAG_STATIC = 1 << 2,

    /**
     * Despawn has been requested and is waiting on the simulation barrier.
     *
     * Set by nya_entity_despawn_deferred. The entity is still fully valid until the barrier runs,
     * so anything mid update sees a consistent world; this flag is how it can tell the difference.
     * */
    NYA_ENTITY_FLAG_DESPAWNING = 1 << 3,
};

/*
 * ─────────────────────────────────────────────────────────
 * SYSTEM STRUCT
 * ─────────────────────────────────────────────────────────
 */

struct NYA_EntitySystem {
    NYA_Arena* allocator;

    NYA_Entity* entities;
    b8*         occupied;
    u32*        generations;

    /**
     * Indices of free slots, most recently freed first.
     *
     * A stack rather than a scan for the first empty slot: with NYA_ENTITY_MAX slots that scan is
     * what turns spawning a few thousand entities from linear into quadratic.
     * */
    u32* free_slots;
    u32  free_count;

    u32 count;

    /** Highest slot ever occupied. Iteration stops here rather than at NYA_ENTITY_MAX. */
    u32 high_water_mark;
};

/*
 * ─────────────────────────────────────────────────────────
 * ENTITY STRUCT
 * ─────────────────────────────────────────────────────────
 */

struct NYA_Entity {
    NYA_EntityHandle handle;
    NYA_EntityFlag   flags;

    /**
     * What kind of thing this is. Game defined; core never interprets it.
     *
     * The tag half of a tagged fat struct: code switches on it to decide which of the fields below
     * actually mean anything for this entity. Same arrangement as NYA_SimRecord.type, and for the
     * same reason — the engine has no business enumerating what a game contains.
     * */
    u32 type;

    /** Not owned. Point it at a literal or something that outlives the entity. */
    NYA_ConstCString name;

    /* ── transform ── */

    f32x3          position;
    NYA_Quaternion rotation;
    f32x3          scale;

    /* ── motion ── */

    f32x3 velocity;
    f32x3 angular_velocity;

    /**
     * Whatever the game needs to hang off this entity.
     *
     * Not owned and not freed on despawn. The engine has no business knowing what a player or a
     * projectile is, and this is the seam where that stays true.
     * */
    void* user_data;

    /* ── per entity callbacks, by handle so they survive a hot reload ── */

    NYA_CallbackHandle on_spawn;
    NYA_CallbackHandle on_despawn;
    NYA_CallbackHandle on_update;
};

typedef void (*NYA_EntityOnSpawnFn)(NYA_Entity* entity);
typedef void (*NYA_EntityOnDespawnFn)(NYA_Entity* entity);
typedef void (*NYA_EntityOnUpdateFn)(NYA_Entity* entity, f32 delta_time_s);

/**
 * What an entity starts as. Everything is optional.
 *
 * Scale defaults to 1 and rotation to identity rather than to zero, because a zeroed transform
 * produces an entity of no size facing nowhere, which is never what anyone meant.
 * */
struct NYA_EntitySpawnOptions {
    NYA_ConstCString name;
    NYA_EntityFlag   flags;
    u32              type;

    f32x3          position;
    NYA_Quaternion rotation;
    f32x3          scale;
    f32x3          velocity;
    f32x3          angular_velocity;

    void* user_data;

    NYA_CallbackHandle on_spawn;
    NYA_CallbackHandle on_despawn;
    NYA_CallbackHandle on_update;
};

#define _NYA_ENTITY_DEFAULT_OPTIONS                                                                                                                  \
    .flags = NYA_ENTITY_FLAG_ACTIVE | NYA_ENTITY_FLAG_VISIBLE, .scale = { 1.0F, 1.0F, 1.0F }, .rotation = { 0.0F, 0.0F, 0.0F, 1.0F }

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * FUNCTIONS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/*
 * ─────────────────────────────────────────────────────────
 * SYSTEM FUNCTIONS
 * ─────────────────────────────────────────────────────────
 */

NYA_API void nya_system_entity_init(void);
NYA_API void nya_system_entity_deinit(void);

/** Runs on_update for every active entity, and integrates velocity into the transform. */
NYA_API void nya_system_entity_update(f32 delta_time_s);

/*
 * ─────────────────────────────────────────────────────────
 * LIFETIME
 * ─────────────────────────────────────────────────────────
 */

#define nya_entity_spawn(...) nya_entity_spawn_with_options((NYA_EntitySpawnOptions){ _NYA_ENTITY_DEFAULT_OPTIONS, __VA_ARGS__ })

/** NYA_ENTITY_HANDLE_NONE when the table is full. */
NYA_API NYA_EntityHandle nya_entity_spawn_with_options(NYA_EntitySpawnOptions options) __attr_no_discard;

/**
 * Removes an entity immediately.
 *
 * Only safe outside iteration. Anything running during an update should use the deferred form,
 * which is the whole reason the simulation has a barrier.
 * */
NYA_API void nya_entity_despawn(NYA_EntityHandle entity);

/**
 * Marks an entity to be removed at the next simulation barrier.
 *
 * What game code should reach for. The entity stays valid for the rest of the tick, so nothing
 * iterating the world has the ground moved under it, and NYA_ENTITY_FLAG_DESPAWNING says what is
 * about to happen. Despawning twice is harmless.
 * */
NYA_API void nya_entity_despawn_deferred(NYA_EntityHandle entity);

/** Null once the entity is gone. Never store the result; store the handle. */
NYA_API NYA_Entity* nya_entity_get(NYA_EntityHandle entity) __attr_no_discard;
NYA_API b8          nya_entity_is_valid(NYA_EntityHandle entity) __attr_no_discard;

NYA_API u32 nya_entity_count(void) __attr_no_discard;

/** Removes every entity. Runs on_despawn for each. */
NYA_API void nya_entity_clear(void);

/*
 * ─────────────────────────────────────────────────────────
 * ITERATION
 * ─────────────────────────────────────────────────────────
 */

/** Null for an empty slot, so a loop over slots has to check. Stops being useful past the high water mark. */
NYA_API NYA_Entity* nya_entity_at_slot(u32 index) __attr_no_discard;
NYA_API u32         nya_entity_slot_count(void) __attr_no_discard;

/**
 * Walks every live entity.
 *
 * Spawning during iteration is safe but the new entity may or may not be visited this pass;
 * despawning during iteration is only safe through nya_entity_despawn_deferred.
 * */
#define nya_entity_foreach(entity_name)                                                                                                              \
    for (u32 _nya_entity_slot = 0; _nya_entity_slot < nya_entity_slot_count(); _nya_entity_slot++)                                                   \
        for (NYA_Entity* entity_name = nya_entity_at_slot(_nya_entity_slot); entity_name != nullptr; entity_name = nullptr)
