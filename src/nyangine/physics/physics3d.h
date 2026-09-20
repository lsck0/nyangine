/**
 * @file physics3d.h
 *
 * ```c
 * NYA_EntityHandle cube = nya_entity_spawn(.name = "cube", .position = { 0, 4, 0 });
 * nya_physics3d_body_attach(cube, .shape = NYA_PHYSICS3D_SHAPE_BOX, .size = { 1, 1, 1 });
 *
 * // ... the world steps, and the entity's transform follows it ...
 * NYA_Entity* entity = nya_entity_get(cube);
 * nya_render3d_cube(window, entity->position, entity->physics3d.size, entity->rotation, colour);
 * ```
 * */
#pragma once

#include "box3d/box3d.h"

#include "nyangine/base/base_attributes.h"
#include "nyangine/base/base_types.h"
#include "nyangine/physics/physics_layer.h"
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
 * */
#ifndef NYA_PHYSICS3D_UNITS_PER_METER
#define NYA_PHYSICS3D_UNITS_PER_METER 1.0F
#endif

/** Solver iterations per step. Same trade as the 2D world's: stiffer stacks at a linear cost. */
#ifndef NYA_PHYSICS3D_SUB_STEPS
#define NYA_PHYSICS3D_SUB_STEPS 4
#endif

/** Earth gravity in world units per second squared, pointing down (negative y). */
#define NYA_PHYSICS3D_GRAVITY_DEFAULT ((f32x3){ 0.0F, -9.81F * NYA_PHYSICS3D_UNITS_PER_METER, 0.0F })

/** Hits kept per step. Same ceiling and same reasoning as NYA_PHYSICS2D_MAX_HITS. */
#ifndef NYA_PHYSICS3D_MAX_HITS
#define NYA_PHYSICS3D_MAX_HITS 256
#endif

/**
 * How fast two things have to be closing before a contact counts as a hit, in world units per second.
 * */
#ifndef NYA_PHYSICS3D_HIT_THRESHOLD
#define NYA_PHYSICS3D_HIT_THRESHOLD (4.0F * NYA_PHYSICS3D_UNITS_PER_METER)
#endif

/**
 * How close to straight up a contact normal must point to count as ground, as a dot product with up.
 * */
#ifndef NYA_PHYSICS3D_GROUND_NORMAL_MIN
#define NYA_PHYSICS3D_GROUND_NORMAL_MIN 0.7F
#endif

/** Contacts inspected when answering nya_physics3d_grounded. A stack buffer; this is the whole cost. */
#ifndef NYA_PHYSICS3D_MAX_CONTACTS_PER_BODY
#define NYA_PHYSICS3D_MAX_CONTACTS_PER_BODY 16
#endif

/**
 * Shapes a body may have and still be refiltered whole by nya_physics3d_layers_set. This API attaches
 * exactly one shape per body, so four is slack rather than a limit anything reaches, and the bound is
 * asserted rather than logged.
 * */
#ifndef NYA_PHYSICS3D_MAX_SHAPES_PER_BODY
#define NYA_PHYSICS3D_MAX_SHAPES_PER_BODY 4
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
     * */
    NYA_PHYSICS3D_SHAPE_BOX = 0,

    NYA_PHYSICS3D_SHAPE_SPHERE,

    /**
     * Two hemispheres joined by a cylinder, upright in the body's own frame.
     * */
    NYA_PHYSICS3D_SHAPE_CAPSULE,

    /** An arbitrary triangle mesh, from `vertices` and `indices`. Static bodies only. */
    NYA_PHYSICS3D_SHAPE_MESH,

    /**
     * A regular grid of heights on the xz plane, from `heights`. Static bodies only.
     *
     * Cheaper than the same terrain as a MESH: a heightfield finds the cell under a point without walking
     * a BVH. `b3SolveContacts_Mesh` was 4.3% of a release profile with the terrain as a mesh.
     *
     * Heights are world units, row-major with x varying fastest, `height_count_x * height_count_z` of
     * them. `height_scale` is the world size of one cell on x and z; y is unused since heights are
     * absolute.
     * */
    NYA_PHYSICS3D_SHAPE_HEIGHTFIELD,

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
     * */
    void* mesh;

    /**
     * Box3D's quantised copy of a HEIGHTFIELD grid, owned by this body. Null for other shapes.
     *
     * Separate from `mesh` because the two need different destructors.
     * */
    void* height_field;

    /*
     * Mirrored from the shapes, as in the 2D body and for the same reason.
     */

    /** Which layers this body is in. See physics_layer.h. */
    NYA_PhysicsLayerMask layers;

    /** Which layers it will meet. Both sides have to agree for a contact to survive. */
    NYA_PhysicsLayerMask collides_with;

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

    /**
     * HEIGHTFIELD: one height per grid point, world units, row-major with x varying fastest.
     *
     * Not copied or owned. Box3D quantises them at creation, so the array can go once
     * nya_physics3d_body_attach returns.
     * */
    const f32* heights;

    /** HEIGHTFIELD: grid points along each axis. Cells are one fewer on each. */
    u32 height_count_x;
    u32 height_count_z;

    /** HEIGHTFIELD: world size of one cell, on x and z. Both must be positive. */
    f32x2 height_cell_size;

    /**
     * Which layers this body is in, as a mask from nya_physics_layer. NYA_PHYSICS_LAYER_DEFAULT unless
     * given. See the 2D field of the same name.
     * */
    NYA_PhysicsLayerMask layers;

    /**
     * Which layers this body will meet. NYA_PHYSICS_LAYER_ALL unless given, and taken literally:
     * NYA_PHYSICS_LAYER_NONE is a body that meets nothing.
     * */
    NYA_PhysicsLayerMask collides_with;

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
    .gravity_scale = 1.0F, .layers = NYA_PHYSICS_LAYER_DEFAULT, .collides_with = NYA_PHYSICS_LAYER_ALL
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

/** Moves a body without simulating the move: no sweep, no contacts along the way, and no interpolation from where it was. */
NYA_API void nya_physics3d_teleport(NYA_Entity* entity, f32x3 position, NYA_Quaternion rotation);

/**
 * Whether the body is resting on something that could hold it up.
 * */
NYA_API b8 nya_physics3d_grounded(const NYA_Entity* entity) __attr_no_discard;

NYA_API b8   nya_physics3d_awake(const NYA_Entity* entity) __attr_no_discard;
NYA_API void nya_physics3d_wake(NYA_Entity* entity);

/*
 * ─────────────────────────────────────────────────────────
 * COLLISION LAYERS
 * ─────────────────────────────────────────────────────────
 */

/**
 * Moves an attached body onto other layers, which takes effect on the next step. See the 2D call of
 * the same name; every shape on the body is refiltered.
 * */
NYA_API void nya_physics3d_layers_set(NYA_Entity* entity, NYA_PhysicsLayerMask layers, NYA_PhysicsLayerMask collides_with);

/** Which layers this body is in. NYA_PHYSICS_LAYER_NONE for an entity with no body. */
NYA_API NYA_PhysicsLayerMask nya_physics3d_layers(const NYA_Entity* entity) __attr_no_discard;

/** Which layers this body meets. NYA_PHYSICS_LAYER_NONE for an entity with no body. */
NYA_API NYA_PhysicsLayerMask nya_physics3d_collides_with(const NYA_Entity* entity) __attr_no_discard;

/*
 * ─────────────────────────────────────────────────────────
 * HITS
 * ─────────────────────────────────────────────────────────
 */

/**
 * The 3D hits from the step just taken, and how many there are.
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
 * The masked overload sees only bodies in `layers`. The plain one is that call with
 * NYA_PHYSICS_LAYER_ALL.
 * */
NYA_API NYA_EntityHandle nya_physics3d_raycast(f32x3 origin, f32x3 direction, OUT f32x3* out_point, OUT f32x3* out_normal)
    __attr_no_discard __attr_overloaded;

NYA_API NYA_EntityHandle nya_physics3d_raycast(f32x3 origin, f32x3 direction, NYA_PhysicsLayerMask layers, OUT f32x3* out_point,
                                               OUT f32x3* out_normal) __attr_no_discard __attr_overloaded;
