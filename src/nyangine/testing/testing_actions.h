/**
 * @file testing_actions.h
 *
 * The atomic actions and faults a nyangine world supports, as one registrable set, plus the invariants
 * that hold over it. What a deterministic simulation composes random sequences out of.
 *
 * ```c
 * NYA_SimulationRun* run = nya_simulation_create(.seed = seed, .step_count = 20000);
 *
 * nya_simulation_actions_add(run);          // the engine's actions, faults and invariants
 * nya_simulation_action_add(run, "spawn_a_robot", 10, spawn_a_robot);   // the game's own, on top
 *
 * u32 failures = nya_simulation_run(run);
 *
 * nya_simulation_actions_remove();
 * nya_simulation_destroy(run);
 * ```
 *
 * ## What is here
 *
 * Entities: spawn, despawn, deferred despawn, flags, parent, unparent, move, teleport.
 * Physics: attach and detach a 2D or 3D body, impulse, teleport, change collision layers, change
 * gravity, freeze and unfreeze the world.
 * Frame: one simulated tick through the real update order, the spatial index rebuild, the queries.
 * Storage: write, read back and delete a save file.
 * Reflection: fill a described type with shaped data and round trip it through an object.
 *
 * Faults: a save file with a flipped byte, a save file cut off mid-write, a save file that vanished,
 * a clock that jumped forward, and a world destroyed and rebuilt at an arbitrary point, which is the
 * crash-only restart path taken at a moment nobody chose.
 *
 * Invariants: the entity count agrees with the live slots, every live handle resolves to itself, each
 * solver's body count agrees with the entities carrying a body, nothing is its own ancestor, a radius
 * query returns exactly what a linear scan does, and the simulated clock never goes backwards.
 *
 * ## Why a singleton
 *
 * The action set owns one world, and the engine has exactly one current world at a time
 * (nya_world_set). Threading a context pointer through every action would let a caller register two
 * sets against two worlds, which the engine cannot honour anyway. Bringing the state up and down is
 * nya_simulation_actions_add and nya_simulation_actions_remove, which are a pair like any other.
 * */
#pragma once

#ifdef NYA_TESTING

#include "nyangine/base/base_attributes.h"
#include "nyangine/base/base_reflection.h"
#include "nyangine/base/base_types.h"
#include "nyangine/testing/testing_simulation.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * CONSTANTS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/**
 * Entities one simulated world may hold. Far below NYA_ENTITY_MAX on purpose: a simulation that fills
 * the table spends the rest of the run being refused spawns, which tests the refusal and nothing else.
 * */
#ifndef NYA_SIMULATION_ENTITY_MAX
#define NYA_SIMULATION_ENTITY_MAX 512
#endif

/** Entity kinds the spawn action picks between, so the kind bitsets are exercised. */
#define NYA_SIMULATION_KIND_COUNT 4

/** Where a simulated save file goes, under the save root. */
#define NYA_SIMULATION_SAVE_FILE "simulation.sav"

/** Collision layers the action set registers, by name. */
#define NYA_SIMULATION_LAYER_GROUND  "simulation_ground"
#define NYA_SIMULATION_LAYER_ACTOR   "simulation_actor"
#define NYA_SIMULATION_LAYER_DEBRIS  "simulation_debris"
#define NYA_SIMULATION_LAYER_TRIGGER "simulation_trigger"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * FUNCTIONS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/**
 * Brings a world up and registers the whole set against `run`. Call before nya_simulation_run.
 *
 * The weights are chosen so a run spends most of its time ticking and mutating a world that exists,
 * and only occasionally tears one down: a simulation that restarts every tenth step never reaches a
 * state deep enough to be interesting.
 * */
NYA_API void nya_simulation_actions_add(NYA_SimulationRun* run);

/**
 * Registers the reflection round trip over `types`, which the caller supplies because the engine
 * cannot name the generated table: src/generated/reflection.h declares the game's described types
 * next to the engine's, and the engine does not know the game.
 *
 * ```c
 * nya_simulation_actions_reflect_add(run, NYA_REFLECT_TYPES, NYA_REFLECT_TYPE_COUNT);
 * ```
 *
 * The action fills a described type with nya_simulation_fill and sends it through
 * nya_reflect_to_object and back, which is the path a save file, a property panel and an undo snapshot
 * all take. Without this call the action is simply not registered.
 * */
NYA_API void nya_simulation_actions_reflect_add(NYA_SimulationRun* run, const NYA_TypeReflection* const* types, u32 count);

/** Tears the world and the save file down. A no-op when nothing was brought up. */
NYA_API void nya_simulation_actions_remove(void);

#endif // NYA_TESTING
