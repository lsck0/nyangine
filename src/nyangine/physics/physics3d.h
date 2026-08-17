/**
 * @file physics3d.h
 *
 * 3D rigid body physics, as a property an entity can have.
 *
 * Box3D owns the simulation; this owns exactly one world and the mapping between it and the entity
 * table. Deliberately the same shape as physics2d.h, function for function, because the two are
 * the same idea over a different number of axes and a game moving between them should not have to
 * relearn anything:
 *
 * ```c
 * NYA_EntityHandle cube = nya_entity_spawn(.name = "cube", .position = { 0, 4, 0 });
 * nya_physics3d_body_attach(cube, .shape = NYA_PHYSICS3D_SHAPE_BOX, .size = { 1, 1, 1 });
 *
 * // ... the world steps, and the entity's transform follows it ...
 * NYA_Entity* entity = nya_entity_get(cube);
 * nya_render3d_cube(window, entity->position, entity->physics3d.size, entity->rotation, colour);
 * ```
 *
 * ## The differences that are real
 *
 * **Rotation is a quaternion, not an angle.** A 2D body has one angular degree of freedom and this
 * one has three, so `entity->rotation` is written in full rather than rebuilt from a roll — and
 * nya_physics3d_angular_velocity is an f32x3 rather than a scalar. That is the whole of why there is
 * no nya_physics3d_rotation returning a float: there is no single number to return.
 *
 * **Y is up, and gravity is negative.** The 2D world is the screen, where y grows downward because
 * that is what pixels, texture rows and mouse coordinates do. A 3D scene has no such constraint and
 * every 3D convention, Box3D's included, puts y up — so NYA_PHYSICS3D_GRAVITY_DEFAULT points along
 * negative y. The two worlds genuinely disagree about which way is down, which is fine because
 * nothing is ever simulated in both.
 *
 * **The default scale is one.** A 2D world is measured in pixels and has to convert, at 32 units per
 * metre. A 3D scene has no pixel size at all — the camera decides how big a metre looks — so the
 * natural unit *is* the metre and nya_physics3d_units_per_meter defaults to 1. The knob exists
 * anyway, because a game whose art is authored at some other scale should change one number rather
 * than every dimension it passes in.
 *
 * ## The step is the engine's, not the game's
 *
 * nya_system_physics3d_update runs once per fixed tick, from the same loop that updates entities and
 * steps the 2D world, and writes each body's transform back onto its entity before any callback
 * runs. Both worlds step every tick; a scene that only uses one is paying for an empty solver, which
 * is a handful of nanoseconds.
 * */
#pragma once

#include "box3d/box3d.h"

#include "nyangine/base/base_attributes.h"
#include "nyangine/base/base_types.h"
#include "nyangine/physics/physics_types.h"
#include "nyangine/core/core_types.h"
#include "nyangine/math/math_quaternion.h"
#include "nyangine/math/math_vector.h"

/* Physics is a property of an entity, and entities hold an NYA_Physics3DBody, so including
 * core_entity.h here would be a cycle. Only the pointer is needed. */
typedef struct NYA_Entity NYA_Entity;

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * CONSTANTS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/**
 * World units per metre. One, unlike the 2D world's thirty-two.
 *
 * See the file header: a 3D scene has no pixel scale to convert through, so the natural unit is the
 * metre and the conversion is the identity. Changing it is for art authored at some other scale, and
 * only sensibly before anything is created — existing bodies keep the size they were built at.
 * */
#ifndef NYA_PHYSICS3D_UNITS_PER_METER
#define NYA_PHYSICS3D_UNITS_PER_METER 1.0F
#endif

/** Solver iterations per step. Same trade as the 2D world's: stiffer stacks at a linear cost. */
#ifndef NYA_PHYSICS3D_SUB_STEPS
#define NYA_PHYSICS3D_SUB_STEPS 4
#endif

/** Earth gravity, in world units per second squared, pointing down — which in 3D is negative y. */
#define NYA_PHYSICS3D_GRAVITY_DEFAULT ((f32x3){ 0.0F, -9.81F * NYA_PHYSICS3D_UNITS_PER_METER, 0.0F })

/** Hits kept per step. Same ceiling and same reasoning as NYA_PHYSICS2D_MAX_HITS. */
#ifndef NYA_PHYSICS3D_MAX_HITS
#define NYA_PHYSICS3D_MAX_HITS 256
#endif

/**
 * How fast two things have to be closing before a contact counts as a hit, in world units per second.
 *
 * Four metres per second, the same speed as the 2D world's threshold — which is a different *number*
 * only because the two scales differ. A body that has fallen roughly a metre.
 * */
#ifndef NYA_PHYSICS3D_HIT_THRESHOLD
#define NYA_PHYSICS3D_HIT_THRESHOLD (4.0F * NYA_PHYSICS3D_UNITS_PER_METER)
#endif

/**
 * How close to straight up a contact normal must point to count as ground, as a dot product with up.
 *
 * The same forty-five degrees the 2D world uses, against a different up vector: positive y here,
 * negative y there.
 * */
#ifndef NYA_PHYSICS3D_GROUND_NORMAL_MIN
#define NYA_PHYSICS3D_GROUND_NORMAL_MIN 0.7F
#endif

/** Contacts inspected when answering nya_physics3d_grounded. A stack buffer; this is the whole cost. */
#ifndef NYA_PHYSICS3D_MAX_CONTACTS_PER_BODY
#define NYA_PHYSICS3D_MAX_CONTACTS_PER_BODY 16
#endif

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * TYPES
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

typedef enum NYA_Physics3DShape         NYA_Physics3DShape;
typedef struct NYA_Physics3DBody        NYA_Physics3DBody;
typedef struct NYA_Physics3DBodyOptions NYA_Physics3DBodyOptions;
typedef struct NYA_Physics3DSystem      NYA_Physics3DSystem;

/*
 * NYA_PhysicsBodyType, NYA_PhysicsHitKind and NYA_PhysicsHit are physics_types.h's, shared with the
 * 2D solver.
 */

enum NYA_Physics3DShape {
    /**
     * An axis aligned box in the body's own frame, `size` being its full extents.
     *
     * Built as a convex hull, which is what Box3D calls a box — there is no separate box primitive,
     * and a hull of eight points is what one is.
     * */
    NYA_PHYSICS3D_SHAPE_BOX = 0,

    NYA_PHYSICS3D_SHAPE_SPHERE,

    /**
     * Two hemispheres joined by a cylinder, upright in the body's own frame.
     *
     * `radius` wide, `length` between the cap centres — the same parameterisation the 2D capsule
     * uses, and upright for the same reason: a capsule is nearly always a character.
     * */
    NYA_PHYSICS3D_SHAPE_CAPSULE,

    /**
     * An arbitrary triangle mesh, from `vertices` and `indices`. **Static bodies only.**
     *
     * The 3D counterpart of the 2D chain, and it exists for the same reason: a landscape is not a box,
     * and approximating one with boxes gives a staircase that a crate visibly catches on. Box3D builds a
     * BVH over the triangles, so a mesh of a few thousand is a normal thing to collide against.
     *
     * Static only, and not by choice here — a triangle soup has no interior, so it has no volume and no
     * inertia tensor, and there is nothing for a dynamic body to be. Every solver draws this line;
     * a dynamic concave shape is a compound of convex hulls instead.
     *
     * Wound counter-clockwise seen from the side a body should be pushed out toward, which for ground is
     * from above. Backwards winding does not make the surface invisible the way it does in the renderer;
     * it makes bodies fall through it, which is a great deal harder to see.
     *
     * The arrays are read during the attach and not kept — Box3D copies them into a structure of its own,
     * which the body owns and releases when it is detached. A caller can build them in scratch memory.
     * */
    NYA_PHYSICS3D_SHAPE_MESH,

    NYA_PHYSICS3D_SHAPE_COUNT,
};

/*
 * ─────────────────────────────────────────────────────────
 * SYSTEM STRUCT
 * ─────────────────────────────────────────────────────────
 */

struct NYA_Physics3DSystem {
    b3WorldId world;

    /** False between deinit and the next init, and what every entry point checks first. */
    b8 initialized;

    /** Set false to freeze the world without unwinding it. Bodies keep their state. */
    b8 enabled;

    f32   units_per_meter;
    f32x3 gravity;
    u32   sub_step_count;

    u32 body_count;

    f32 last_step_time_s;
    u64 step_count;

    NYA_PhysicsHit hits[NYA_PHYSICS3D_MAX_HITS];
    u32            hit_count;

    f32 hit_threshold;
};

/*
 * ─────────────────────────────────────────────────────────
 * BODY STRUCT
 * ─────────────────────────────────────────────────────────
 */

/** What an entity carries when the 3D solver simulates it. Zeroed, and `attached` false, when it does not. */
struct NYA_Physics3DBody {
    b3BodyId id;

    NYA_PhysicsBodyType type;
    NYA_Physics3DShape  shape;

    /** Full extents in world units, for a box. Kept here because Box3D hands back a hull, not a box. */
    f32x3 size;

    /** World units, for a sphere or capsule. */
    f32 radius;

    /** Distance between the cap centres, world units, for a capsule. */
    f32 length;

    /**
     * Box3D's copy of a MESH shape's triangles, owned by this body. Null for every other shape.
     *
     * Held here because it is the one shape whose data outlives the attach call and is not freed by
     * b3DestroyBody — b3CreateMeshShape takes a pointer to it and keeps it. Released by
     * nya_physics3d_body_detach, which is also why that function has to do its work even when the
     * solver has already been shut down underneath it.
     * */
    void* mesh;

    b8 attached;

    /* ── Cached grounded state ── Same arrangement as the 2D body: computed on demand, remembered
     * for the step it was computed in, because walking a body's contacts is far too much to do per
     * body per frame and far too little to precompute for the handful anyone asks about. */

    b8 grounded;

    /** The step `grounded` was computed on, plus one. Zero means "never computed". */
    u64 grounded_step;
};

/**
 * What a body is created as. Everything except the shape's dimensions has a usable default.
 *
 * `density` is per cubic metre here, where the 2D world's is per square metre — the one place the
 * two option structs mean genuinely different things by the same field name, and unavoidable, since
 * a 2D body has an area and a 3D body has a volume.
 * */
struct NYA_Physics3DBodyOptions {
    NYA_PhysicsBodyType type;
    NYA_Physics3DShape  shape;

    /** BOX: full extents, world units. */
    f32x3 size;

    /** SPHERE and CAPSULE: radius, world units. */
    f32 radius;

    /** CAPSULE: distance between the cap centres, world units. */
    f32 length;

    /** MESH: the triangle corners, world units, in the body's own frame. */
    const f32x3* vertices;

    /** MESH: three per triangle, counter-clockwise seen from the outside. */
    const u32* indices;

    u32 vertex_count;

    /** MESH: three times the triangle count, not the triangle count. */
    u32 index_count;

    /** Kilograms per cubic metre. Ignored on a static or kinematic body, which have no mass. */
    f32 density;

    f32 friction;
    f32 restitution;

    f32 linear_damping;
    f32 angular_damping;

    /** Multiplies world gravity for this body alone. Zero floats; negative rises. */
    f32 gravity_scale;

    /**
     * Stops the body from turning at all, on every axis.
     *
     * The 3D counterpart of the 2D body's `lock_rotation`, and a blunter instrument: Box3D can lock
     * each axis separately, which is what an upright character actually wants (free yaw, locked
     * pitch and roll). That finer control is deliberately not exposed yet — three booleans on this
     * struct with no caller is three booleans to get wrong.
     * */
    b8 lock_rotation;

    /** Collides and reports, but never resolves. A trigger volume. */
    b8 is_sensor;

    /** Continuous collision against static geometry, for something small and fast. Costs more. */
    b8 is_bullet;

    /** Never sleeps. Only for a body something is measuring every tick. */
    b8 never_sleep;

    /** Stops this body from asking for its impacts to be measured. See the 2D field of the same name. */
    b8 ignore_hits;
};

// clang-format off
#define _NYA_PHYSICS3D_BODY_DEFAULT_OPTIONS                                                                                                          \
    .type = NYA_PHYSICS_BODY_DYNAMIC, .shape = NYA_PHYSICS3D_SHAPE_BOX, .density = 1000.0F, .friction = 0.6F, .restitution = 0.05F,                   \
    .gravity_scale = 1.0F
// clang-format on

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

NYA_API void nya_system_physics3d_init(void);
NYA_API void nya_system_physics3d_deinit(void);

/** Steps the world once and writes every body's transform onto its entity. A game does not call this. */
NYA_API void nya_system_physics3d_update(f32 delta_time_s);

/*
 * ─────────────────────────────────────────────────────────
 * WORLD
 * ─────────────────────────────────────────────────────────
 */

/** World units per second squared. Negative y is down, unlike the 2D world. */
NYA_API void  nya_physics3d_gravity_set(f32x3 gravity);
NYA_API f32x3 nya_physics3d_gravity(void) __attr_no_discard;

/** See NYA_PHYSICS3D_UNITS_PER_METER. Set this before creating anything, not after. */
NYA_API void nya_physics3d_units_per_meter_set(f32 units_per_meter);
NYA_API f32  nya_physics3d_units_per_meter(void) __attr_no_discard;

NYA_API void nya_physics3d_enabled_set(b8 enabled);
NYA_API b8   nya_physics3d_enabled(void) __attr_no_discard;

NYA_API u32 nya_physics3d_body_count(void) __attr_no_discard;
NYA_API f32 nya_physics3d_last_step_time_s(void) __attr_no_discard;

/*
 * ─────────────────────────────────────────────────────────
 * BODIES
 * ─────────────────────────────────────────────────────────
 */

/**
 * Gives an entity a 3D rigid body, built at the transform the entity already has.
 *
 * The entity's `position` and `rotation` seed the body and its `velocity` seeds the linear velocity,
 * exactly as in 2D. From here on the body owns the transform and nya_system_entity_update stops
 * integrating this entity's velocity.
 *
 * False when the entity handle does not resolve, when it already has a 3D body, or when the shape's
 * dimensions do not describe anything — all logged.
 * */
#define nya_physics3d_body_attach(entity, ...)                                                                                                       \
    nya_physics3d_body_attach_with_options(entity, (NYA_Physics3DBodyOptions){ _NYA_PHYSICS3D_BODY_DEFAULT_OPTIONS, __VA_ARGS__ })

NYA_API b8 nya_physics3d_body_attach_with_options(NYA_EntityHandle entity, NYA_Physics3DBodyOptions options);

/** Destroys the body and leaves the entity in the world. Called for you on despawn. */
NYA_API void nya_physics3d_body_detach(NYA_EntityHandle entity);

NYA_API b8 nya_physics3d_body_attached(const NYA_Entity* entity) __attr_no_discard;

/*
 * ─────────────────────────────────────────────────────────
 * FORCES AND STATE
 * ─────────────────────────────────────────────────────────
 */

/** An instantaneous change in momentum, at the centre of mass. What a jump or a hit is. */
NYA_API void nya_physics3d_apply_impulse(NYA_Entity* entity, f32x3 impulse);

/** A continuous push, applied for the tick it is called in. What a thruster is. */
NYA_API void nya_physics3d_apply_force(NYA_Entity* entity, f32x3 force);

/** An instantaneous change in angular momentum, about each axis. */
NYA_API void nya_physics3d_apply_angular_impulse(NYA_Entity* entity, f32x3 impulse);

NYA_API void  nya_physics3d_velocity_set(NYA_Entity* entity, f32x3 velocity);
NYA_API f32x3 nya_physics3d_velocity(const NYA_Entity* entity) __attr_no_discard;

/** Radians per second about each axis. Three components, where the 2D world has one. */
NYA_API void  nya_physics3d_angular_velocity_set(NYA_Entity* entity, f32x3 radians_per_second);
NYA_API f32x3 nya_physics3d_angular_velocity(const NYA_Entity* entity) __attr_no_discard;

/** Moves a body without simulating the move: no sweep, no contacts along the way. */
NYA_API void nya_physics3d_teleport(NYA_Entity* entity, f32x3 position, NYA_Quaternion rotation);

/**
 * Whether the body is resting on something that could hold it up.
 *
 * True when any contact's normal points up within NYA_PHYSICS3D_GROUND_NORMAL_MIN. False, without
 * complaint, for an entity with no 3D body.
 * */
NYA_API b8 nya_physics3d_grounded(const NYA_Entity* entity) __attr_no_discard;

NYA_API b8   nya_physics3d_awake(const NYA_Entity* entity) __attr_no_discard;
NYA_API void nya_physics3d_wake(NYA_Entity* entity);

/*
 * ─────────────────────────────────────────────────────────
 * HITS
 * ─────────────────────────────────────────────────────────
 */

/**
 * The 3D hits from the step just taken, and how many there are.
 *
 * Separate from nya_physics2d_hits, because they are separate worlds and a caller reacting to one
 * has no business being handed the other's. Every entry has `dimension` set to NYA_PHYSICS_3D, so
 * code that merges the two lists can still tell them apart afterwards.
 *
 * **Read it during the tick that produced it.** Never null; `count` is zero on a quiet tick.
 * */
NYA_API const NYA_PhysicsHit* nya_physics3d_hits(OUT u32* out_count) __attr_no_discard;

NYA_API void nya_physics3d_hit_threshold_set(f32 world_units_per_second);
NYA_API f32  nya_physics3d_hit_threshold(void) __attr_no_discard;

/*
 * ─────────────────────────────────────────────────────────
 * QUERIES
 * ─────────────────────────────────────────────────────────
 */

/**
 * The first entity a ray strikes, or NYA_ENTITY_HANDLE_NONE.
 *
 * What a click on a 3D scene is, and the 3D counterpart of nya_physics2d_entity_at — which takes a
 * point, because in 2D the screen *is* the world plane and a click already names a world position.
 * In 3D it names a line, so this takes one.
 *
 * `direction` need not be normalised; its length is how far the ray reaches. `out_point` and
 * `out_normal` are optional and are left alone when nothing is hit.
 *
 * Closest hit, not first reported, so a cube behind another cube cannot win.
 * */
NYA_API NYA_EntityHandle nya_physics3d_raycast(f32x3 origin, f32x3 direction, OUT f32x3* out_point, OUT f32x3* out_normal) __attr_no_discard;
