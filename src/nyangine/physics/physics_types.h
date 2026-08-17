/**
 * @file physics_types.h
 *
 * What the two solvers agree on: what a body type is, and what a collision looks like when it is
 * reported.
 *
 * There are two rigid body simulations in this engine — Box2D in physics2d.h and Box3D in
 * physics3d.h — and they share nothing at runtime. They do share a *vocabulary*, and this file
 * is it. Everything dimension-specific lives in the two headers that include this one.
 *
 * ## Why one hit type and not two
 *
 * An entity has one on_collision. It has to, because "what happens when this is struck" is a
 * property of the thing, not of which solver happens to be simulating it — a crate that plays a
 * sound and takes damage wants to say that once, and a game that puts a 2D HUD over a 3D scene would
 * otherwise be writing the same callback twice with two argument types.
 *
 * So NYA_PhysicsHit carries a `dimension` tag rather than being split in two. The cost is that a 2D
 * hit's `point` and `normal` are f32x3 with a zero z, which is not a fiction: the 2D world *is* the
 * z = 0 plane, it is where 2D entities already sit, and the z the solver never touches is the z that
 * was already zero.
 *
 * The tag is not decoration. A game running both at once needs to know which world an event came
 * out of before it can interpret the units, because the two scales are set independently — see
 * nya_physics2d_pixels_per_meter and nya_physics3d_units_per_meter.
 * */
#pragma once

#include "nyangine/base/base_types.h"
#include "nyangine/core/core_types.h"
#include "nyangine/math/math_vector.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * TYPES
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

typedef enum NYA_PhysicsDimension NYA_PhysicsDimension;
typedef enum NYA_PhysicsBodyType  NYA_PhysicsBodyType;
typedef enum NYA_PhysicsHitKind   NYA_PhysicsHitKind;
typedef struct NYA_PhysicsHit     NYA_PhysicsHit;

/** Which solver an event came out of. See the file header for why this is a tag and not two types. */
enum NYA_PhysicsDimension {
    NYA_PHYSICS_2D = 0,
    NYA_PHYSICS_3D,

    NYA_PHYSICS_DIMENSION_COUNT,
};

/**
 * How a body is moved. The same three in both solvers, because the distinction is not about axes.
 *
 * Shared rather than duplicated so that a helper written against "is this thing pushable" does not
 * have to switch on the dimension first.
 * */
enum NYA_PhysicsBodyType {
    /** Never moves and is never moved by a collision. Terrain, walls, platforms. */
    NYA_PHYSICS_BODY_STATIC = 0,

    /** Moves only where it is told to, and pushes what it meets without being pushed back. */
    NYA_PHYSICS_BODY_KINEMATIC = 1,

    /** Has mass and is moved by gravity, forces and collisions. The default. */
    NYA_PHYSICS_BODY_DYNAMIC = 2,
};

/**
 * What kind of meeting an NYA_PhysicsHit describes.
 *
 * The three are genuinely different events that happen to share a struct and a callback, and mixing
 * them up is the mistake this enum exists to prevent: an impact has a speed and a normal and a
 * pickup has neither, so a response written for one reads garbage off the other.
 * */
enum NYA_PhysicsHitKind {
    /** Two solid bodies struck each other hard enough to clear the world's hit threshold. */
    NYA_PHYSICS_HIT_IMPACT = 0,

    /**
     * Something entered a sensor. `a` is the sensor, `b` is what walked into it.
     *
     * What a coin, a checkpoint, a damage volume or a door trigger is. The bodies do not push each
     * other and nothing is resolved, so there is no impact speed to react to — the event *is* the
     * whole of it, and it fires once on the step the overlap begins rather than every step it lasts.
     * */
    NYA_PHYSICS_HIT_SENSOR_ENTER,

    /**
     * Something left a sensor, or was destroyed while inside one.
     *
     * The second half matters: a despawn inside a trigger produces an exit, so anything counting
     * what is currently inside a volume stays balanced instead of leaking one per despawn. The
     * entity that left may therefore already be gone by the time this is delivered, which is the
     * usual reason to check nya_entity_get rather than assume.
     * */
    NYA_PHYSICS_HIT_SENSOR_EXIT,

    NYA_PHYSICS_HIT_KIND_COUNT,
};

/**
 * Two bodies that met, during the step just taken.
 *
 * A *hit*, not a contact. Both solvers report every touching pair every step, which for a settled
 * pile is a constant stream saying nothing happened; an impact is filtered by closing speed before
 * it gets here, so one landing crate produces one of these and then goes quiet. Sensor overlaps are
 * not filtered at all, because entering a trigger is already an edge rather than a state.
 *
 * Both handles are resolved from the bodies' entities, so either can already have been despawned by
 * the time something walks the list — check with nya_entity_get rather than assuming.
 * */
struct NYA_PhysicsHit {
    /** Which solver produced this, and therefore what the units below mean. */
    NYA_PhysicsDimension dimension;

    /** Which of the three events this is, and therefore which fields below mean anything. */
    NYA_PhysicsHitKind kind;

    /** For a sensor event this is the sensor. For an impact the two sides are not ordered. */
    NYA_EntityHandle a;

    /** For a sensor event this is the visitor — what entered or left. */
    NYA_EntityHandle b;

    /**
     * Where they met, in the producing world's units. `z` is always zero for a 2D hit.
     *
     * The contact point for an impact. For a sensor event neither solver reports overlap geometry at
     * all, so this is the midpoint between the two bodies — close enough to put a pickup sparkle at,
     * and deliberately symmetric rather than pretending to be either body's position.
     * */
    f32x3 point;

    /** Unit vector, pointing from A's surface toward B. Zero for a sensor event. */
    f32x3 normal;

    /**
     * How fast they were closing along that normal, in world units per second. Always positive.
     *
     * The number to drive a response with: an impact's loudness, a dent's depth, whether it breaks.
     * Scale it against the producing world's threshold, which is the slowest speed that can appear
     * here — nya_physics2d_hit_threshold or nya_physics3d_hit_threshold, chosen on `dimension`.
     *
     * Zero for a sensor event, which has no closing speed — a sensor does not resolve, so neither
     * solver ever computes one.
     * */
    f32 approach_speed;
};
