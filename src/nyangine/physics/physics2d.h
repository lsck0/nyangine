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

#include "nyangine/base/base_attributes.h"
#include "nyangine/base/base_types.h"
#include "nyangine/physics/physics_types.h"
#include "nyangine/core/core_types.h"
#include "nyangine/math/math_vector.h"

/* Physics is a property of an entity, and entities hold an NYA_Physics2DBody, so including
 * core_entity.h here would be a cycle. Only the pointer is needed. */
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
 * How close to straight up a contact normal must point to count as ground, as a dot product with up.
 * */
#ifndef NYA_PHYSICS2D_GROUND_NORMAL_MIN
#define NYA_PHYSICS2D_GROUND_NORMAL_MIN 0.7F
#endif

/** Earth gravity, in world units per second squared, pointing down the screen. */
#define NYA_PHYSICS2D_GRAVITY_DEFAULT ((f32x2){ 0.0F, 9.81F * NYA_PHYSICS2D_PIXELS_PER_METER })

/**
 * Hits kept per step. Anything past this is dropped, and overflow is logged once per step rather
 * than silently truncated.
 * */
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
 * NYA_PhysicsBodyType, NYA_PhysicsHitKind and NYA_PhysicsHit are physics_types.h's, shared with the
 * 3D solver. What a static body is does not depend on how many axes it has, and an entity has one
 * on_collision that both solvers deliver to — see that file for why the hit is one type with a
 * dimension tag rather than two types.
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

    /**
     * Box2D's contact recycle distance, as it was at init, in **metres**.
     * */
    f32 contact_recycle_distance;

    /**
     * Whether contact recycling is currently switched off.
     *
     * ⚠ **This exists because Box2D skips the pre-solve callback for a contact that has not moved.**
     * `b2UpdateContact` is where pre-solve is invoked, and the step loop `continue`s past it entirely
     * when both bodies are within the recycle distance of where they were — which is every step for
     * something resting on a platform. So a body standing still on a one-way surface never gets its
     * contact re-examined, and "let me through" is never heard.
     * */
    b8 contact_recycling_suspended;

    /** Seconds the last step spent inside Box2D. For an overlay, and for noticing a stack that costs. */
    f32 last_step_time_s;

    /**
     * Steps taken since the world was created. What per-step caches key on.
     * */
    u64 step_count;

    /*
     * Hits from the last step. Copied out of Box2D's transient event buffer rather than pointed at,
     * and converted to world units and entity handles on the way — upstream's buffer is only valid
     * until the next step and speaks in metres and shape ids.
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
     * Cached grounded state: asking Box2D for a body's contacts and checking their normals is too
     * much to do per body per frame for a world of hundreds, and too little to bother precomputing
     * for the handful anyone actually asks about — so it is computed on demand and remembered for
     * the tick it was computed in.
     */

    b8 grounded;

    /**
     * The step `grounded` was computed on, plus one. Zero means "never computed".
     * */
    u64 grounded_step;

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

    /**
     * CHAIN: the polyline, in world units **relative to the entity's position**.
     *
     * Copied during the call, so the caller's array does not have to outlive it.
     * */
    const f32x2* points;
    u32          point_count;

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
     * ⚠ Every shape this API creates enables sensor events on **both** sides, unlike Box2D's default of
     * off on both — a coin with `is_sensor` and a player without would produce no events at all, and
     * look exactly like a coin that was never reached.
     * */
    b8 is_sensor;

    /** Continuous collision against static geometry, for something small and fast. Costs more. */
    b8 is_bullet;

    /** Never sleeps. Only for a body something is measuring every tick; sleeping is what makes a big world cheap. */
    b8 never_sleep;

    /**
     * Stops this body from asking for its impacts to be measured.
     * */
    b8 ignore_hits;
};

// clang-format off
#define _NYA_PHYSICS_BODY_DEFAULT_OPTIONS                                                                                                            \
    .type = NYA_PHYSICS_BODY_DYNAMIC, .shape = NYA_PHYSICS2D_SHAPE_BOX, .density = 1.0F, .friction = 0.6F, .restitution = 0.05F, .gravity_scale = 1.0F
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
 * Moves a body without simulating the move: no sweep, no contacts along the way.
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
 * ⚠ **Velocity-based means a body that has already stopped inside the surface stays inside it.**
 * Nothing pushes it out — a one-way surface has no interior to expel from. In practice this is what
 * is wanted: something that gets there was moving the passable way, and it goes on through.
 *
 * ⚠ **The callback runs inside `b2World_Step`.** It reads body user data and the body's velocity and
 * writes nothing, which is what makes it safe; this world is stepped single-threaded (the enqueue
 * hooks in `nya_system_physics2d_init` are deliberately left unset), so it is not a threading
 * question today, and anything added to it must stay read-only if that changes.
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
 * The hits from the step just taken — impacts and sensor overlaps together.
 *
 * One list rather than two, because everything downstream wants the same thing: walk what happened this
 * tick and react. ⚠ Filter on `kind`, or you will play an impact sound at zero gain every time
 * something walks into a trigger.
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
 * ⚠ **Read it during the tick that produced it.** The list is refilled at the top of every tick, so a
 * stashed pointer reads the *next* tick's contacts. Copy what has to outlive the tick.
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
 * The entity whose body covers `point`, or NYA_ENTITY_HANDLE_NONE.
 * */
/**
 * The nearest body along `origin + direction`, or NYA_ENTITY_HANDLE_NONE.
 * */
NYA_API NYA_EntityHandle nya_physics2d_raycast(f32x2 origin, f32x2 direction, OUT f32x2* out_point, OUT f32x2* out_normal)
    __attr_no_discard;

NYA_API NYA_EntityHandle nya_physics2d_entity_at(f32x2 point) __attr_no_discard;
