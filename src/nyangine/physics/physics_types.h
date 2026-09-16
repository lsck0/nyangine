/**
 * @file physics_types.h
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
 * */
enum NYA_PhysicsHitKind {
    /** Two solid bodies struck each other hard enough to clear the world's hit threshold. */
    NYA_PHYSICS_HIT_IMPACT = 0,

    /**
     * Something entered a sensor. `a` is the sensor, `b` is what walked into it.
     * */
    NYA_PHYSICS_HIT_SENSOR_ENTER,

    /**
     * Something left a sensor, or was destroyed while inside one.
     * */
    NYA_PHYSICS_HIT_SENSOR_EXIT,

    NYA_PHYSICS_HIT_KIND_COUNT,
};

/**
 * Two bodies that met, during the step just taken.
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
     * */
    f32x3 point;

    /** Unit vector, pointing from A's surface toward B. Zero for a sensor event. */
    f32x3 normal;

    /**
     * How fast they were closing along that normal, in world units per second. Always positive.
     * */
    f32 approach_speed;
};
