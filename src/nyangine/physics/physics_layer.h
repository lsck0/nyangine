/**
 * @file physics_layer.h
 *
 * Named collision layers, shared by both solvers.
 *
 * A body is in one or more layers and collides with a mask of layers. Both sides have to agree: a
 * contact survives only when A is in something B collides with and B is in something A collides with.
 * That is Box2D's and Box3D's own rule, and this module is a thin naming layer over their filters, so
 * the broadphase rejects the pair before any narrowphase or callback work happens.
 *
 * Overview:
 *   nya_physics_layer            the bit for a name, registering it on first use
 *   nya_physics_layer_find       the bit for a name, without registering
 *   nya_physics_layer_name       the name of a layer index
 *   nya_physics_layer_count      how many are registered
 *   nya_physics_layers           ORs several names into one mask
 *
 * ```c
 * // once, wherever the game sets its world up.
 * NYA_PhysicsLayerMask terrain = nya_physics_layer("terrain");
 * NYA_PhysicsLayerMask player  = nya_physics_layer("player");
 * NYA_PhysicsLayerMask debris  = nya_physics_layer("debris");
 *
 * // debris piles up on the ground and rattles against itself, and never blocks the player.
 * nya_physics2d_body_attach(chunk, .size = { 4, 4 }, .layers = debris, .collides_with = terrain | debris);
 * ```
 *
 * Registration is by name because a layer is late bound like every other name in the engine: a save
 * file, a tilemap or a reloaded dll names "terrain", not bit 3, so the bit a name maps to may change
 * between runs without anything holding a stale number.
 *
 * Layers are per world. Two worlds can register different sets, and a world's registry dies with it.
 * The bit a name gets is the order it was first registered in, so a world that registers the same
 * names in the same order gets the same bits, which is what a replayed simulation needs.
 *
 * Rejected: a `groupIndex` passthrough. Both libraries have one, and it overrides the mask entirely,
 * which makes "why did these not collide" answerable only by reading two fields that disagree. Layers
 * are the whole vocabulary here.
 * */
#pragma once

#include "nyangine/base/base_attributes.h"
#include "nyangine/base/base_types.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * CONSTANTS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/**
 * Layers a world can hold. Sixty-four, because b2Filter and b3Filter carry their categories and masks
 * as one `uint64_t` each; a sixty-fifth layer would need a second word on both sides of the vendor
 * boundary and a filter callback per pair instead of a bitwise and in the broadphase.
 * */
#define NYA_PHYSICS_LAYER_MAX 64

/**
 * Bytes a layer name may take, terminator included. Names are copied rather than pointed at, since a
 * caller's string is usually a literal but may be a tilemap property read off disk.
 * */
#ifndef NYA_PHYSICS_LAYER_NAME_MAX
#define NYA_PHYSICS_LAYER_NAME_MAX 32
#endif

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * TYPES
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

typedef struct NYA_PhysicsLayerSystem NYA_PhysicsLayerSystem;

/**
 * A set of layers, one bit each. Both a body's own layers and the layers it collides with are one of
 * these, because "which layers am I in" and "which layers do I meet" are the same kind of question.
 * */
typedef u64 NYA_PhysicsLayerMask;

/** No layer at all. A body whose `collides_with` is this meets nothing. */
#define NYA_PHYSICS_LAYER_NONE ((NYA_PhysicsLayerMask)0)

/** Every layer, registered or not. What a body collides with unless it narrows it. */
#define NYA_PHYSICS_LAYER_ALL ((NYA_PhysicsLayerMask)U64_MAX)

/**
 * Bit zero, registered as "default" when a world comes up, and what a body is in when it names none.
 * A game that never touches layers therefore has every body in one layer meeting every other, which
 * is what the engine did before layers existed.
 * */
#define NYA_PHYSICS_LAYER_DEFAULT ((NYA_PhysicsLayerMask)1)

/** The name bit zero is registered under. */
#define NYA_PHYSICS_LAYER_DEFAULT_NAME "default"

/**
 * One world's layer names, indexed by bit. Transparent like every system struct; nothing here needs
 * hiding, and the debug overlay reads it directly.
 * */
struct NYA_PhysicsLayerSystem {
    /** Registered names, `names[i]` being the name of bit `i`. Empty past `count`. */
    char names[NYA_PHYSICS_LAYER_MAX][NYA_PHYSICS_LAYER_NAME_MAX];

    /** Layers handed out. Always at least one, since bit zero is the default layer. */
    u32 count;

    /** False between deinit and the next init. */
    b8 initialized;
};

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

/** Empties the registry and registers the default layer as bit zero. */
NYA_API void nya_system_physics_layer_init(void);
NYA_API void nya_system_physics_layer_deinit(void);

/*
 * ─────────────────────────────────────────────────────────
 * LAYERS
 * ─────────────────────────────────────────────────────────
 */

/**
 * The bit for `name`, registering it on first use.
 *
 * Idempotent: the same name always gives the same bit within a world. Registering past
 * NYA_PHYSICS_LAYER_MAX names is an operating error, not a crash: it logs and returns
 * NYA_PHYSICS_LAYER_NONE, so a body built from it collides with nothing instead of silently joining
 * whatever layer happened to be last.
 * */
NYA_API NYA_PhysicsLayerMask nya_physics_layer(NYA_ConstCString name) __attr_no_discard;

/**
 * The bit for `name` without registering anything. False when nothing has that name.
 *
 * For code that must not invent a layer: a tilemap naming a layer the game never declared is a
 * content bug, and inventing bit 7 for it would hide that.
 * */
NYA_API b8 nya_physics_layer_find(NYA_ConstCString name, OUT NYA_PhysicsLayerMask* out_layer) __attr_no_discard;

/** The name registered for bit `index`, or nullptr past what is registered. */
NYA_API NYA_ConstCString nya_physics_layer_name(u32 index) __attr_no_discard;

/** How many layers are registered, the default layer included. */
NYA_API u32 nya_physics_layer_count(void) __attr_no_discard;

/**
 * ORs several named layers into one mask, registering each on first use.
 *
 * ```c
 * .collides_with = nya_physics_layers("terrain", "player")
 * ```
 * */
#define nya_physics_layers(...) _nya_physics_layers((NYA_ConstCString[]){ __VA_ARGS__, nullptr })

/**
 * Whether two bodies described this way could ever meet. The same rule both solvers apply, exposed so
 * a test, an editor or a query can answer it without stepping a world.
 * */
NYA_API b8 nya_physics_layer_mask_overlaps(NYA_PhysicsLayerMask layers_a, NYA_PhysicsLayerMask collides_with_a, NYA_PhysicsLayerMask layers_b,
                                           NYA_PhysicsLayerMask collides_with_b) __attr_no_discard;

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * INTERNAL
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/** Behind nya_physics_layers. Reads `names` until the nullptr terminator the macro appends. */
NYA_API NYA_PhysicsLayerMask _nya_physics_layers(NYA_ConstCString const* names);
