/**
 * @file physics2d.h
 *
 * ```c
 * NYA_EntityHandle crate = nya_entity_spawn(.name = "crate", .position = { 200, 0, 0 });
 * nya_physics2d_body_attach(crate, .shape = NYA_PHYSICS2D_SHAPE_BOX, .size = { 32, 32 });
 *
 * // ... the world steps, and the entity's transform follows it ...
 * NYA_Entity* entity = nya_entity_get(crate);
 * nya_render2d_rect_rotated(window, entity->position.xy, entity->physics2d.size, angle, colour);
 * ```
 * */
#pragma once

#include "box2d/box2d.h"

#include "nyangine-std/base/base_attributes.h"
#include "nyangine-std/base/base_handle.h"
#include "nyangine-std/base/base_types.h"
#include "nyangine-core/physics/physics_layer.h"
#include "nyangine-core/physics/physics_types.h"
#include "nyangine-std/math/math_vector.h"

/* entities hold an NYA_Physics2DBody, so core_entity.h cannot be included here. only the pointer is needed. */
typedef struct NYA_Entity NYA_Entity;

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * CONSTANTS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/**
 * World units per metre, the scale the whole module converts through.
 * */
#ifndef NYA_PHYSICS2D_PIXELS_PER_METER
#define NYA_PHYSICS2D_PIXELS_PER_METER 32.0F
#endif

/**
 * Solver iterations per step.
 * */
#ifndef NYA_PHYSICS2D_SUB_STEPS
#define NYA_PHYSICS2D_SUB_STEPS 4
#endif

/**
 * Most points one chain shape may be given.
 * */
#ifndef NYA_PHYSICS2D_CHAIN_MAX_POINTS
#define NYA_PHYSICS2D_CHAIN_MAX_POINTS 1024
#endif

/**
 * Contacts inspected when answering nya_physics2d_grounded.
 * */
#ifndef NYA_PHYSICS2D_MAX_CONTACTS_PER_BODY
#define NYA_PHYSICS2D_MAX_CONTACTS_PER_BODY 16
#endif

/**
 * Shapes a body may have and still be refiltered whole by nya_physics2d_layers_set. A stack buffer,
 * and the only cost of the call. This API attaches one shape per body, so the only way past it is a
 * chain, whose segments are shapes; a chain longer than this keeps its old layers on the remainder
 * and says so.
 * */
#ifndef NYA_PHYSICS2D_MAX_SHAPES_PER_BODY
#define NYA_PHYSICS2D_MAX_SHAPES_PER_BODY NYA_PHYSICS2D_CHAIN_MAX_POINTS
#endif

/**
 * How close to straight up a contact normal must point to count as ground, as a dot product with up.
 * */
#ifndef NYA_PHYSICS2D_GROUND_NORMAL_MIN
#define NYA_PHYSICS2D_GROUND_NORMAL_MIN 0.7F
#endif

/** Earth gravity, in world units per second squared, pointing down the screen. */
#define NYA_PHYSICS2D_GRAVITY_DEFAULT ((f32x2){ 0.0F, 9.81F * NYA_PHYSICS2D_PIXELS_PER_METER })

/** Hits kept per step. Overflow is dropped and logged once per step. */
#ifndef NYA_PHYSICS2D_MAX_HITS
#define NYA_PHYSICS2D_MAX_HITS 256
#endif

/**
 * How fast two things have to be closing before a contact counts as a hit, in world units per second.
 * */
#ifndef NYA_PHYSICS2D_HIT_THRESHOLD
#define NYA_PHYSICS2D_HIT_THRESHOLD (4.0F * NYA_PHYSICS2D_PIXELS_PER_METER)
#endif

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * TYPES
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

typedef enum NYA_Physics2DShape         NYA_Physics2DShape;
typedef struct NYA_Physics2DBody        NYA_Physics2DBody;
typedef struct NYA_Physics2DBodyOptions NYA_Physics2DBodyOptions;
typedef struct NYA_Physics2DSystem      NYA_Physics2DSystem;

/*
 * NYA_PhysicsBodyType, NYA_PhysicsHitKind and NYA_PhysicsHit are shared with the 3D solver in physics_types.h,
 * since an entity has one on_collision both solvers deliver to.
 */

/**
 * Which way a surface admits contacts. See nya_physics2d_one_way_set.
 * */
typedef enum NYA_Physics2DOneWay {
    /** Solid from every direction. The default. */
    NYA_PHYSICS2D_ONE_WAY_NONE = 0,

    NYA_PHYSICS2D_ONE_WAY_UP,
    NYA_PHYSICS2D_ONE_WAY_DOWN,
    NYA_PHYSICS2D_ONE_WAY_LEFT,
    NYA_PHYSICS2D_ONE_WAY_RIGHT,

    NYA_PHYSICS2D_ONE_WAY_COUNT,
} NYA_Physics2DOneWay;

enum NYA_Physics2DShape {
    /** An axis aligned box in the body's own frame, `size` being its full width and height. */
    NYA_PHYSICS2D_SHAPE_BOX = 0,

    NYA_PHYSICS2D_SHAPE_CIRCLE,

    /** Two half circles joined by a rectangle: `radius` wide, `length` between the cap centres. */
    NYA_PHYSICS2D_SHAPE_CAPSULE,

    /**
     * An open polyline, for terrain.
     * */
    NYA_PHYSICS2D_SHAPE_CHAIN,

    NYA_PHYSICS2D_SHAPE_COUNT,
};

/*
 * ─────────────────────────────────────────────────────────
 * SYSTEM STRUCT
 * ─────────────────────────────────────────────────────────
 */

struct NYA_Physics2DSystem {
    b2WorldId world;

    /**
     * False between deinit and the next init; every entry point checks this first.
     * */
    b8 initialized;

    /** Set false to freeze the world without unwinding it. Bodies keep their state. */
    b8 enabled;

    f32   pixels_per_meter;
    f32x2 gravity;
    u32   sub_step_count;

    /** Bodies currently attached to an entity. */
    u32 body_count;

    /** Box2D's contact recycle distance at init, in metres. */
    f32 contact_recycle_distance;

    /**
     * Whether contact recycling is switched off.
     *
     * Box2D skips pre-solve for a contact that has not moved: b2UpdateContact is skipped when both bodies stay
     * within the recycle distance, which is every step for something resting on a platform. Without this a body
     * standing on a one-way surface is never let through.
     * */
    b8 contact_recycling_suspended;

    /** Seconds the last step spent inside Box2D. */
    f32 last_step_time_s;

    /**
     * Steps taken since the world was created. What per-step caches key on.
     * */
    u64 step_count;

    /*
     * Hits from the last step, copied out of Box2D's event buffer (valid only until the next step) and converted
     * to world units and entity handles.
     */
    NYA_PhysicsHit hits[NYA_PHYSICS2D_MAX_HITS];
    u32            hit_count;

    /** World units per second. See NYA_PHYSICS2D_HIT_THRESHOLD. */
    f32 hit_threshold;
};

/*
 * ─────────────────────────────────────────────────────────
 * BODY STRUCT
 * ─────────────────────────────────────────────────────────
 */

/**
 * What an entity carries when it is simulated. Zeroed, and `attached` false, when it is not.
 * */
struct NYA_Physics2DBody {
    b2BodyId id;

    NYA_PhysicsBodyType type;
    NYA_Physics2DShape    shape;

    /** Full width and height in world units, for a box. */
    f32x2 size;

    /** World units, for a circle or capsule. */
    f32 radius;

    /** Distance between the cap centres, world units, for a capsule. */
    f32 length;

    b8 attached;

    /*
     * Cached grounded state, computed on demand and kept for the tick. Checking contact normals for every body
     * every frame would cost too much for the few anyone asks about.
     */

    b8 grounded;

    /** The step `grounded` was computed on, plus one. Zero means never. */
    u64 grounded_step;

    /*
     * Mirrored from the shapes so a reader does not have to walk them, and so the two sides of a pair
     * can be compared without asking Box2D twice.
     */

    /** Which layers this body is in. See physics_layer.h. */
    NYA_PhysicsLayerMask layers;

    /** Which layers it will meet. Both sides have to agree for a contact to survive. */
    NYA_PhysicsLayerMask collides_with;

    /** Which way this surface lets bodies through, if any. See nya_physics2d_one_way_set. */
    NYA_Physics2DOneWay one_way;

    /**
     * Seconds left in which this body ignores every one-way surface. See nya_physics2d_drop_through.
     * */
    f32 drop_through_s;
};

/**
 * What a body is created as. Everything except the shape's dimensions has a usable default.
 * */
struct NYA_Physics2DBodyOptions {
    NYA_PhysicsBodyType type;
    NYA_Physics2DShape    shape;

    /** BOX: full width and height, world units. */
    f32x2 size;

    /** CIRCLE and CAPSULE: radius, world units. */
    f32 radius;

    /** CAPSULE: distance between the cap centres, world units. */
    f32 length;

    /** CHAIN: the polyline, in world units relative to the entity's position. Copied during the call. */
    const f32x2* points;
    u32          point_count;

    /**
     * Which layers this body is in, as a mask from nya_physics_layer. NYA_PHYSICS_LAYER_DEFAULT unless
     * given, so a game that never names a layer behaves exactly as it did before layers existed.
     *
     * ```c
     * nya_physics2d_body_attach(bullet, .radius = 2, .shape = NYA_PHYSICS2D_SHAPE_CIRCLE,
     *                           .layers = nya_physics_layer("bullet"),
     *                           .collides_with = nya_physics_layers("terrain", "enemy"));
     * ```
     * */
    NYA_PhysicsLayerMask layers;

    /**
     * Which layers this body will meet. NYA_PHYSICS_LAYER_ALL unless given, and taken literally:
     * NYA_PHYSICS_LAYER_NONE is a body that meets nothing, not a body that meets everything.
     *
     * Filtering happens in the broadphase, so a pair the masks reject costs nothing: no manifold, no
     * pre-solve call, no hit event, no on_collision. That is the whole reason this is a filter rather
     * than an early return in a callback.
     * */
    NYA_PhysicsLayerMask collides_with;

    /** Kilograms per square metre. Ignored on a static or kinematic body, which have no mass. */
    f32 density;

    /** Coulomb friction, normally within [0, 1]. Mixed with the other surface's on contact. */
    f32 friction;

    /** Bounce, within [0, 1]. Zero is a dead stop; one would return all of the energy. */
    f32 restitution;

    /** Drag against linear and angular motion, per second. Zero is a vacuum. */
    f32 linear_damping;
    f32 angular_damping;

    /** Multiplies world gravity for this body alone. Zero floats; negative rises. */
    f32 gravity_scale;

    /** Stops the body from turning, without giving it infinite inertia. What a character wants. */
    b8 lock_rotation;

    /** Makes this a one-way surface at creation. Same meaning as nya_physics2d_one_way_set. */
    NYA_Physics2DOneWay one_way;

    /**
     * Collides and reports, but never resolves. A trigger volume.
     *
     * ```c
     * void coin_on_collision(NYA_Entity* coin, NYA_Entity* other, const NYA_PhysicsHit* hit) {
     *     if (hit->kind != NYA_PHYSICS_HIT_SENSOR_ENTER) return;
     *     if (other == nullptr || other->type != KIND_PLAYER) return;
     *
     *     nya_audio_play_sound_at(NYA_ASSET_SOUNDS_PICKUP_WAV, hit->point, (NYA_SoundParams){ 0 });
     *     nya_entity_despawn_deferred(coin->handle);
     * }
     * ```
     *
     * Every shape this API creates enables sensor events on both sides. Box2D defaults both off, and a sensor
     * coin touching a player without the flag would report nothing.
     * */
    b8 is_sensor;

    /** Continuous collision against static geometry, for something small and fast. Costs more. */
    b8 is_bullet;

    /** Never sleeps. Only for a body measured every tick; sleeping is what keeps a large world cheap. */
    b8 never_sleep;

    /**
     * Stops this body from asking for its impacts to be measured.
     * */
    b8 ignore_hits;
};

// clang-format off
#define _NYA_PHYSICS_BODY_DEFAULT_OPTIONS                                                                                                            \
    .type = NYA_PHYSICS_BODY_DYNAMIC, .shape = NYA_PHYSICS2D_SHAPE_BOX, .density = 1.0F, .friction = 0.6F, .restitution = 0.05F, .gravity_scale = 1.0F, \
    .layers = NYA_PHYSICS_LAYER_DEFAULT, .collides_with = NYA_PHYSICS_LAYER_ALL
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

NYA_API void nya_system_physics2d_init(void);
NYA_API void nya_system_physics2d_deinit(void);

/**
 * Steps the world once and writes every body's transform onto its entity.
 * */
NYA_API void nya_system_physics2d_update(f32 delta_time_s);

/*
 * ─────────────────────────────────────────────────────────
 * WORLD
 * ─────────────────────────────────────────────────────────
 */

/** World units per second squared, positive y being down the screen. */
NYA_API void  nya_physics2d_gravity_set(f32x2 gravity);
NYA_API f32x2 nya_physics2d_gravity(void) __attr_no_discard;

/** See NYA_PHYSICS2D_PIXELS_PER_METER. Set this before creating anything, not after. */
NYA_API void nya_physics2d_pixels_per_meter_set(f32 pixels_per_meter);
NYA_API f32  nya_physics2d_pixels_per_meter(void) __attr_no_discard;

/** False freezes the simulation; bodies keep their velocities and resume where they left off. */
NYA_API void nya_physics2d_enabled_set(b8 enabled);
NYA_API b8   nya_physics2d_enabled(void) __attr_no_discard;

NYA_API u32 nya_physics2d_body_count(void) __attr_no_discard;

/** Seconds the last step spent in the solver. Zero before the first step. */
NYA_API f32 nya_physics2d_last_step_time_s(void) __attr_no_discard;

/*
 * ─────────────────────────────────────────────────────────
 * BODIES
 * ─────────────────────────────────────────────────────────
 */

/**
 * Gives an entity a rigid body, built at the transform the entity already has.
 * */
#define nya_physics2d_body_attach(entity, ...)                                                                                                         \
    nya_physics2d_body_attach_with_options(entity, (NYA_Physics2DBodyOptions){ _NYA_PHYSICS_BODY_DEFAULT_OPTIONS, __VA_ARGS__ })

NYA_API b8 nya_physics2d_body_attach_with_options(NYA_EntityHandle entity, NYA_Physics2DBodyOptions options);

/**
 * Destroys the body and leaves the entity in the world.
 * */
NYA_API void nya_physics2d_body_detach(NYA_EntityHandle entity);

NYA_API b8 nya_physics2d_body_attached(const NYA_Entity* entity) __attr_no_discard;

/*
 * ─────────────────────────────────────────────────────────
 * FORCES AND STATE
 * ─────────────────────────────────────────────────────────
 */

/** An instantaneous change in momentum, at the centre of mass. What a jump or a hit is. */
NYA_API void nya_physics2d_apply_impulse(NYA_Entity* entity, f32x2 impulse);

/** A continuous push, applied for the tick it is called in. What a thruster is. */
NYA_API void nya_physics2d_apply_force(NYA_Entity* entity, f32x2 force);

/** An instantaneous change in angular momentum, in world units squared per second. */
NYA_API void nya_physics2d_apply_angular_impulse(NYA_Entity* entity, f32 impulse);

/** World units per second. Setting velocity fights the solver; prefer an impulse where either works. */
NYA_API void  nya_physics2d_velocity_set(NYA_Entity* entity, f32x2 velocity);
NYA_API f32x2 nya_physics2d_velocity(const NYA_Entity* entity) __attr_no_discard;

/** Radians per second, clockwise on screen. */
NYA_API void nya_physics2d_angular_velocity_set(NYA_Entity* entity, f32 radians_per_second);
NYA_API f32  nya_physics2d_angular_velocity(const NYA_Entity* entity) __attr_no_discard;

/**
 * Moves a body without simulating the move: no sweep, no contacts along the way, and no interpolation from where it was.
 * */
NYA_API void nya_physics2d_teleport(NYA_Entity* entity, f32x2 position, f32 rotation);

/**
 * The body's rotation about the screen's z axis, in radians, clockwise.
 * */
NYA_API f32 nya_physics2d_rotation(const NYA_Entity* entity) __attr_no_discard;

/**
 * Whether the body is resting on something that could hold it up.
 * */
NYA_API b8 nya_physics2d_grounded(const NYA_Entity* entity) __attr_no_discard;

/** False once the solver has put the body to rest. Asleep bodies cost nothing until touched. */
NYA_API b8   nya_physics2d_awake(const NYA_Entity* entity) __attr_no_discard;
NYA_API void nya_physics2d_wake(NYA_Entity* entity);

/*
 * ─────────────────────────────────────────────────────────
 * COLLISION LAYERS
 * ─────────────────────────────────────────────────────────
 */

/**
 * Moves an attached body onto other layers, which takes effect on the next step.
 *
 * ```c
 * // the player is untouchable for a moment after being hit, without being despawned or disabled.
 * nya_physics2d_layers_set(player, nya_physics_layer("player"), nya_physics_layer("terrain"));
 * ```
 *
 * Every shape on the body is refiltered, chain segments included, since a body's layers are a
 * property of the body and not of whichever shape happens to be first.
 * */
NYA_API void nya_physics2d_layers_set(NYA_Entity* entity, NYA_PhysicsLayerMask layers, NYA_PhysicsLayerMask collides_with);

/** Which layers this body is in. NYA_PHYSICS_LAYER_NONE for an entity with no body. */
NYA_API NYA_PhysicsLayerMask nya_physics2d_layers(const NYA_Entity* entity) __attr_no_discard;

/** Which layers this body meets. NYA_PHYSICS_LAYER_NONE for an entity with no body. */
NYA_API NYA_PhysicsLayerMask nya_physics2d_collides_with(const NYA_Entity* entity) __attr_no_discard;

/*
 * ─────────────────────────────────────────────────────────
 * ONE-WAY SURFACES
 * ─────────────────────────────────────────────────────────
 */

/**
 * Makes this body's surface passable from one direction.
 *
 * ```c
 * // A ledge you jump up through and then stand on.
 * nya_physics2d_body_attach(ledge, (NYA_Physics2DBodyOptions){
 *     .type = NYA_PHYSICS_BODY_STATIC, .size = { 96, 8 }, .one_way = NYA_PHYSICS2D_ONE_WAY_UP,
 * });
 *
 * // Down on the stick, and the player falls off it.
 * if (nya_input_action_just_pressed("crouch")) nya_physics2d_drop_through(player, 0.25F);
 * ```
 *
 * Passage is decided by velocity, so a body that stopped inside the surface stays there; nothing pushes it
 * out. The callback runs inside b2World_Step and only reads, which is safe because the world steps on one
 * thread.
 * */
NYA_API void nya_physics2d_one_way_set(NYA_Entity* entity, NYA_Physics2DOneWay direction);

/** Which way this body lets bodies through. NONE for a solid one, or for an entity with no body. */
NYA_API NYA_Physics2DOneWay nya_physics2d_one_way(const NYA_Entity* entity) __attr_no_discard;

/**
 * Lets this body fall through every one-way surface for `seconds`.
 * */
NYA_API void nya_physics2d_drop_through(NYA_Entity* entity, f32 seconds);

/*
 * ─────────────────────────────────────────────────────────
 * HITS
 * ─────────────────────────────────────────────────────────
 */

/**
 * The hits from the step just taken, impacts and sensor overlaps together. Filter on `kind`.
 *
 * ```c
 * void layer_on_update(NYA_Window* window, f32 delta_time_s) {
 *     u32                   count;
 *     const NYA_PhysicsHit* hits = nya_physics2d_hits(&count);
 *
 *     for (u32 i = 0; i < count; i++) {
 *         if (hits[i].kind != NYA_PHYSICS_HIT_IMPACT) continue;
 *
 *         f32 loudness = hits[i].approach_speed / nya_physics2d_hit_threshold();
 *         nya_audio_play_sound_at(NYA_ASSET_SOUNDS_HIT_WAV, hits[i].point, (NYA_SoundParams){ .gain = loudness });
 *     }
 * }
 * ```
 *
 * The list is refilled every tick; copy anything that has to outlive it.
 * */
NYA_API const NYA_PhysicsHit* nya_physics2d_hits(OUT u32* out_count) __attr_no_discard;

/**
 * The closing speed a contact needs before it appears in that list, in world units per second.
 * */
NYA_API void nya_physics2d_hit_threshold_set(f32 world_units_per_second);
NYA_API f32  nya_physics2d_hit_threshold(void) __attr_no_discard;

/*
 * ─────────────────────────────────────────────────────────
 * QUERIES
 * ─────────────────────────────────────────────────────────
 */

/**
 * The nearest body along `origin + direction`, or NYA_ENTITY_HANDLE_NONE.
 *
 * The masked overload sees only bodies in `layers`, so a bullet trace can ignore the pickups and the
 * corpses it flies through. The plain one sees every layer and is that call with
 * NYA_PHYSICS_LAYER_ALL.
 * */
NYA_API NYA_EntityHandle nya_physics2d_raycast(f32x2 origin, f32x2 direction, OUT f32x2* out_point, OUT f32x2* out_normal)
    __attr_no_discard __attr_overloaded;

NYA_API NYA_EntityHandle nya_physics2d_raycast(f32x2 origin, f32x2 direction, NYA_PhysicsLayerMask layers, OUT f32x2* out_point,
                                               OUT f32x2* out_normal) __attr_no_discard __attr_overloaded;

/**
 * The entity whose body covers `point`, or NYA_ENTITY_HANDLE_NONE.
 * */
NYA_API NYA_EntityHandle nya_physics2d_entity_at(f32x2 point) __attr_no_discard __attr_overloaded;

NYA_API NYA_EntityHandle nya_physics2d_entity_at(f32x2 point, NYA_PhysicsLayerMask layers) __attr_no_discard __attr_overloaded;
