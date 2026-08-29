/**
 * @file core_world.h
 *
 * The simulated scene: entities, physics, and the deferred work that ties them together.
 *
 * The split this file exists to draw is between the *process* and the *scene*. NYA_App owns what
 * belongs to the program — the windows, the GPU device, the assets on disk, the input devices, the
 * job pool. NYA_World owns what belongs to the thing being simulated, and there can be more than
 * one of those, sequentially or side by side.
 *
 * ```c
 * NYA_EntityHandle crate = nya_entity_spawn(.name = "crate");   // into the current world
 *
 * NYA_World* menu_backdrop = nya_world_create();
 * NYA_World* gameplay      = nya_world_set(menu_backdrop);      // returns the previous one
 * ```
 *
 * A world is a unit of lifetime: creating one brings its three systems up in the order they depend
 * on each other, and destroying one takes every entity and every rigid body with it, in reverse order
 * — physics has to outlive entities, because despawning an entity destroys the body it carries. That
 * ordering is now written down in exactly one place instead of being re-derived at each call site.
 *
 * The world is allocated from its own arena and reached through NYA_App, which lives in the
 * executable rather than the game's shared library, so it survives a reload intact while the library
 * is closed, reopened and reinitialised. `user_data` is the game's own root pointer and follows the
 * same rule — allocate it from `allocator` and it outlives the reload too.
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
 *
 * nya_app_init creates one and makes it current, so a game that only ever has one world never calls
 * this. Call it to build a second — a level to fade to, a headless world to run a simulation in —
 * and nya_world_set to switch.
 * */
NYA_API NYA_World* nya_world_create(void) __attr_no_discard;

/**
 * Tears a world down: every entity despawned, every rigid body destroyed, then the arena released.
 *
 * Destroying the current world leaves none current, and nya_world asserts rather than handing back
 * a dangling pointer — so set another one first if the program is going to keep running.
 * */
NYA_API void nya_world_destroy(NYA_World* world);

/*
 * ─────────────────────────────────────────────────────────
 * THE CURRENT WORLD
 * ─────────────────────────────────────────────────────────
 */

/**
 * The world every entity and physics call operates on.
 *
 * Never null once nya_app_init has run: the app creates a world before anything can spawn into one,
 * so the entity and physics APIs never have to answer "what if there is no world".
 * */
NYA_API NYA_World* nya_world(void) __attr_no_discard;

/** Whether there is a current world. For teardown paths, which run after the app has released it. */
NYA_API b8 nya_world_exists(void) __attr_no_discard;

/**
 * Makes `world` current and returns whichever one was.
 *
 * Nothing is destroyed and nothing is copied — this is a pointer swap, so the outgoing world keeps
 * every entity and body it had and resumes exactly where it left off when it is set back. Null is
 * legal and means "no current world", which only a teardown path should want.
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
 * Null until something sets it, which is what makes "fresh start or reload?" answerable without the
 * game tracking it separately:
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
 *
 * Allocated from the world's arena rather than the game's own, so destroying the world frees it.
 * */
NYA_API void* nya_world_user_data(void) __attr_no_discard;
NYA_API void  nya_world_user_data_set(void* user_data);
