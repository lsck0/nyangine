/**
 * @file core_world.h
 *
 * ```c
 * NYA_EntityHandle crate = nya_entity_spawn(.name = "crate");   // into the current world
 *
 * NYA_World* menu_backdrop = nya_world_create();
 * NYA_World* gameplay      = nya_world_set(menu_backdrop);      // returns the previous one
 * ```
 * */
#pragma once

#include "nyangine/base/base_arena.h"
#include "nyangine/base/base_attributes.h"
#include "nyangine/core/core_entity.h"
#include "nyangine/physics/physics2d.h"
#include "nyangine/physics/physics3d.h"
#include "nyangine/core/core_sim.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * TYPES
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

typedef struct NYA_World NYA_World;

struct NYA_World {
    /** Owns this struct and everything the game hangs off `user_data`; destroyed with the world. */
    NYA_Arena* allocator;

    NYA_EntitySystem  entity_system;
    NYA_Physics2DSystem physics2d_system;
    NYA_Physics3DSystem physics3d_system;
    NYA_SimSystem     sim_system;

    /**
     * The game's root pointer for this world; the engine stores it and never looks inside it. Same
     * contract as NYA_Entity.user_data, but freed with the world provided it was allocated from
     * `allocator` — the arrangement to prefer, since "destroy the world" then means the whole thing.
     * */
    void* user_data;
};

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * FUNCTIONS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/*
 * ─────────────────────────────────────────────────────────
 * LIFETIME
 * ─────────────────────────────────────────────────────────
 */

/**
 * Builds a world and brings its three systems up. Does **not** make it current.
 * */
NYA_API NYA_World* nya_world_create(void) __attr_no_discard;

/**
 * Tears a world down: every entity despawned, every rigid body destroyed, then the arena released.
 * */
NYA_API void nya_world_destroy(NYA_World* world);

/*
 * ─────────────────────────────────────────────────────────
 * THE CURRENT WORLD
 * ─────────────────────────────────────────────────────────
 */

/**
 * The world every entity and physics call operates on.
 * */
NYA_API NYA_World* nya_world(void) __attr_no_discard;

/** Whether there is a current world. For teardown paths, which run after the app has released it. */
NYA_API b8 nya_world_exists(void) __attr_no_discard;

/**
 * Makes `world` current and returns whichever one was.
 * */
NYA_API NYA_World* nya_world_set(NYA_World* world);

/*
 * ─────────────────────────────────────────────────────────
 * GAME STATE
 * ─────────────────────────────────────────────────────────
 */

/**
 * The current world's `user_data`, and the way a game finds its own state after a hot reload.
 *
 * ```c
 * GameState* state = nya_world_user_data();
 * if (state == nullptr) {
 *     state = nya_arena_alloc(nya_world()->allocator, sizeof(GameState));
 *     *state = (GameState){ 0 };
 *     nya_world_user_data_set(state);
 *     build_the_level(state);          // only on a genuinely fresh start
 * }
 * ```
 * */
NYA_API void* nya_world_user_data(void) __attr_no_discard;
NYA_API void  nya_world_user_data_set(void* user_data);
