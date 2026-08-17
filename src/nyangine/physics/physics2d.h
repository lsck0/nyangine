/**
 * @file physics2d.h
 *
 * 2D rigid body physics, as a property an entity can have.
 *
 * Box2D v3 owns the simulation; this owns exactly one world and the mapping between it and the
 * entity table. A body is attached to an entity rather than created on its own, so there is no
 * second identity to keep in step — the entity's handle is the body's handle, and despawning the
 * entity destroys the body.
 *
 * ```c
 * NYA_EntityHandle crate = nya_entity_spawn(.name = "crate", .position = { 200, 0, 0 });
 * nya_physics2d_body_attach(crate, .shape = NYA_PHYSICS2D_SHAPE_BOX, .size = { 32, 32 });
 *
 * // ... the world steps, and the entity's transform follows it ...
 * NYA_Entity* entity = nya_entity_get(crate);
 * nya_render2d_rect_rotated(window, entity->position.xy, entity->physics2d.size, angle, colour);
 * ```
 *
 * ## Units, and the two of them
 *
 * Everything in this API is in **world units**, which is what an entity transform is in and what
 * the renderer draws in — pixels, y down, positive y toward the bottom of the screen. Box2D is
 * metric and is tuned for bodies between roughly 0.1 and 10 metres, which a pixel sized body is
 * emphatically not: solver tolerances are absolute, so a 32 unit crate simulated as 32 metres
 * jitters and sleeps wrong. Conversion happens at this boundary and nowhere else, through
 * nya_physics2d_pixels_per_meter.
 *
 * The y axis points **down** here and up in most Box2D material, which costs nothing: Box2D has no
 * opinion about which way is up, it only integrates the gravity vector it is given. So the default
 * gravity is positive y, and a positive rotation reads as clockwise on screen — the same sense
 * NYA_Render2DTexture.rotation already uses.
 *
 * ## The step is the engine's, not the game's
 *
 * nya_system_physics2d_update runs once per fixed tick, from the same loop that updates entities, and
 * writes each body's transform back onto its entity before any callback runs. A game therefore
 * never steps the world and never reads a b2BodyId; it reads `entity->position` like it would for
 * anything else, and an entity with a body simply stops integrating its own velocity.
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
 *
 * Pick it so that the things in the game land in the range Box2D's solver is tuned for: a character
 * around one to two metres, a crate around half of one. At the default a 32x32 pixel crate is a
 * one metre crate, which is exactly right.
 *
 * Changeable at runtime through nya_physics2d_pixels_per_meter_set, but only sensibly before anything
 * is created — existing bodies keep the size they were built at and would silently change scale
 * relative to their entities.
 * */
#ifndef NYA_PHYSICS2D_PIXELS_PER_METER
#define NYA_PHYSICS2D_PIXELS_PER_METER 32.0F
#endif

/**
 * Solver iterations per step.
 *
 * Box2D v3's soft step solver: more sub steps means stiffer stacks and less overlap, at a linear
 * cost. Four is upstream's recommendation and holds a dozen high stack of crates without visible
 * sink.
 * */
#ifndef NYA_PHYSICS2D_SUB_STEPS
#define NYA_PHYSICS2D_SUB_STEPS 4
#endif

/**
 * Most points one chain shape may be given.
 *
 * The points are converted to metres into a stack buffer of this size before they are handed over,
 * because the caller's array is in world units and Box2D copies what it is given. Terrain longer
 * than this is more than one chain, which is what a chunked world would want anyway.
 * */
#ifndef NYA_PHYSICS2D_CHAIN_MAX_POINTS
#define NYA_PHYSICS2D_CHAIN_MAX_POINTS 1024
#endif

/**
 * Contacts inspected when answering nya_physics2d_grounded.
 *
 * A stack buffer, so this is the whole cost of the call. A body resting on terrain has one or two;
 * anything with more than sixteen is wedged in a crevice and the seventeenth will not change the
 * answer.
 * */
#ifndef NYA_PHYSICS2D_MAX_CONTACTS_PER_BODY
#define NYA_PHYSICS2D_MAX_CONTACTS_PER_BODY 16
#endif

/**
 * How close to straight up a contact normal must point to count as ground, as a dot product with up.
 *
 * 0.7 is about forty-five degrees: a body resting on a slope steeper than that is against a wall
 * rather than standing on a floor, and a character controller that thought otherwise would let the
 * player jump up a cliff. Raise it toward 1 for "only flat ground counts".
 * */
#ifndef NYA_PHYSICS2D_GROUND_NORMAL_MIN
#define NYA_PHYSICS2D_GROUND_NORMAL_MIN 0.7F
#endif

/** Earth gravity, in world units per second squared, pointing down the screen. */
#define NYA_PHYSICS2D_GRAVITY_DEFAULT ((f32x2){ 0.0F, 9.81F * NYA_PHYSICS2D_PIXELS_PER_METER })

/**
 * Hits kept per step. Anything past this is dropped, oldest first in the sense that it is never copied.
 *
 * A ceiling rather than a growable buffer, because this is per tick data with a hard deadline: a
 * pile collapsing produces a burst of contacts and there are only so many an audio or damage
 * response can do anything with. Overflow is logged once per step rather than silently truncating.
 * */
#ifndef NYA_PHYSICS2D_MAX_HITS
#define NYA_PHYSICS2D_MAX_HITS 256
#endif

/**
 * How fast two things have to be closing before a contact counts as a hit, in world units per second.
 *
 * The whole point of the hit list: every resting crate generates contacts every step, and almost
 * none of them are events. At the default scale this is about four metres per second, which is a
 * body that has fallen roughly a metre — audible as an impact rather than as a settle.
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

enum NYA_Physics2DShape {
    /** An axis aligned box in the body's own frame, `size` being its full width and height. */
    NYA_PHYSICS2D_SHAPE_BOX = 0,

    NYA_PHYSICS2D_SHAPE_CIRCLE,

    /** Two half circles joined by a rectangle: `radius` wide, `length` between the cap centres. */
    NYA_PHYSICS2D_SHAPE_CAPSULE,

    /**
     * An open polyline, for terrain.
     *
     * One sided and infinitely thin, so nothing tunnels through the wrong face and nothing has an
     * interior to be caught inside. Only valid on a static body — a chain has no volume and
     * therefore no mass to give a dynamic one.
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
     * False between deinit and the next init, and what every entry point checks first.
     *
     * Teardown destroys the world and every body in it at once, and entity teardown runs afterwards
     * and detaches bodies one at a time. Without this the second pass would hand freed ids back to
     * Box2D, which asserts rather than shrugging.
     * */
    b8 initialized;

    /** Set false to freeze the world without unwinding it. Bodies keep their state. */
    b8 enabled;

    f32   pixels_per_meter;
    f32x2 gravity;
    u32   sub_step_count;

    /** Bodies currently attached to an entity. */
    u32 body_count;

    /** Seconds the last step spent inside Box2D. For an overlay, and for noticing a stack that costs. */
    f32 last_step_time_s;

    /**
     * Steps taken since the world was created. What per-step caches key on.
     *
     * Not the simulation tick: that only advances in the application's own update loop, so anything
     * keyed on it goes stale the moment the solver is stepped by something else — a test, a tool, a
     * rewind. This counts the thing it is actually measuring.
     * */
    u64 step_count;

    /*
     * ── Hits from the last step ──
     *
     * Copied out of Box2D's transient event buffer rather than pointed at, and converted to world
     * units and entity handles on the way. Upstream's buffer is only valid until the next step and
     * speaks in metres and shape ids, neither of which anything outside this file should have to
     * know about.
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
 *
 * The dimensions are kept alongside the id because they are what a renderer needs and Box2D does
 * not hand them back in the form they went in as — reading a box's extents means walking its shape
 * list and inspecting a polygon's vertices, per frame, to recover a number the caller already had.
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
     * ── Cached grounded state ──
     *
     * Answering "is this standing on something" means asking Box2D for the body's contacts and
     * looking at their normals, which is far too much to do per body per frame for a world of
     * hundreds — and far too little to bother precomputing for the handful anyone actually asks
     * about. So it is computed on demand and remembered for the tick it was computed in.
     */

    b8 grounded;

    /**
     * The step `grounded` was computed on, plus one. Zero means "never computed".
     *
     * Plus one so that a zeroed NYA_Physics2DBody — which is what a detached one is — cannot look like
     * a valid answer for step zero.
     * */
    u64 grounded_step;
};

/**
 * What a body is created as. Everything except the shape's dimensions has a usable default.
 *
 * `density` is per square metre, so it interacts with the scale: at the default pixels per metre a
 * 32x32 box of density 1 weighs one kilogram. Zero density on a dynamic body is legal and gives it
 * the minimum mass Box2D will accept rather than an infinite one.
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

    /**
     * Collides and reports, but never resolves. A trigger volume.
     *
     * What a pickup is. The overlap arrives as an NYA_PHYSICS_HIT_SENSOR_ENTER through the same
     * on_collision the impacts come through, so a coin is an ordinary entity with an ordinary
     * callback:
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
     * Nothing has to be done to the *other* body for this to work: every shape this API creates
     * enables sensor events, so anything can be seen by a sensor. Box2D's own default is off on both
     * sides, which is a footgun — a coin with `is_sensor` set and a player without would produce no
     * events at all, and look exactly like a coin that was never reached.
     * */
    b8 is_sensor;

    /** Continuous collision against static geometry, for something small and fast. Costs more. */
    b8 is_bullet;

    /** Never sleeps. Only for a body something is measuring every tick; sleeping is what makes a big world cheap. */
    b8 never_sleep;

    /**
     * Stops this body from asking for its impacts to be measured.
     *
     * Not the same as never appearing in nya_physics2d_hits: the solver measures a pair as soon as
     * *either* side asked, so a body that opts out is still reported when it strikes one that did
     * not. Silencing an impact entirely means both bodies setting this.
     *
     * Reported by default, because an empty hit list with no way to tell why is a bad afternoon.
     * Set it for the bodies whose impacts nothing reacts to — debris, decoration — since the
     * measurement is not free.
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
 *
 * Called from the fixed timestep loop with the fixed step, which is the only thing Box2D's solver
 * is stable under; handing it a variable frame time makes a stack of crates behave differently at
 * different frame rates. A game does not call this.
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
 *
 * The entity's `position` and `rotation` seed the body, and its `velocity` seeds the body's linear
 * velocity — so spawning something with an initial throw is one spawn call and one attach, in that
 * order, rather than a third call to set velocity afterwards.
 *
 * From here on the body owns the transform: nya_system_entity_update stops integrating this
 * entity's velocity, because two things writing one position is a fight the frame rate decides.
 *
 * False when the entity handle does not resolve, when it already has a body, or when the shape's
 * dimensions do not describe anything — all logged.
 * */
#define nya_physics2d_body_attach(entity, ...)                                                                                                         \
    nya_physics2d_body_attach_with_options(entity, (NYA_Physics2DBodyOptions){ _NYA_PHYSICS_BODY_DEFAULT_OPTIONS, __VA_ARGS__ })

NYA_API b8 nya_physics2d_body_attach_with_options(NYA_EntityHandle entity, NYA_Physics2DBodyOptions options);

/**
 * Destroys the body and leaves the entity in the world.
 *
 * The entity keeps whatever transform the last step gave it and goes back to integrating its own
 * velocity. Called for you on despawn; call it directly to turn something from simulated into
 * scripted without respawning it. Harmless on an entity that has no body.
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
 *
 * For a respawn or a teleport. Anything that should collide on the way there is a velocity or an
 * impulse instead.
 * */
NYA_API void nya_physics2d_teleport(NYA_Entity* entity, f32x2 position, f32 rotation);

/**
 * The body's rotation about the screen's z axis, in radians, clockwise.
 *
 * A 2D body has one degree of angular freedom and the entity carries a full quaternion, so this is
 * the direct read that avoids converting one to Euler angles and picking an axis back out.
 * */
NYA_API f32 nya_physics2d_rotation(const NYA_Entity* entity) __attr_no_discard;

/**
 * Whether the body is resting on something that could hold it up.
 *
 * True when any contact's normal points up within NYA_PHYSICS2D_GROUND_NORMAL_MIN — so a crate on flat
 * terrain is grounded, one wedged against a vertical wall is not, and one in mid air is not.
 *
 * Computed on first ask each tick and remembered until the next one, so calling it repeatedly in a
 * frame costs one contact query rather than one per call. A sleeping body keeps whatever it last
 * answered, which is correct: it went to sleep resting on something and has not moved since.
 *
 * False, without complaint, for an entity with no body — "not standing on anything" is a truthful
 * answer for something the solver has never heard of.
 * */
NYA_API b8 nya_physics2d_grounded(const NYA_Entity* entity) __attr_no_discard;

/** False once the solver has put the body to rest. Asleep bodies cost nothing until touched. */
NYA_API b8   nya_physics2d_awake(const NYA_Entity* entity) __attr_no_discard;
NYA_API void nya_physics2d_wake(NYA_Entity* entity);

/*
 * ─────────────────────────────────────────────────────────
 * HITS
 * ─────────────────────────────────────────────────────────
 */

/**
 * The hits from the step just taken, and how many there are — impacts and sensor overlaps together.
 *
 * One list rather than two, because everything downstream of it wants the same thing: walk what
 * happened this tick and react. Filter on `kind`; the example below does, and code that does not
 * will find itself playing an impact sound at zero gain every time something walks into a trigger.
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
 * **Read it during the tick that produced it.** The list is refilled by the next
 * nya_system_physics2d_update, which runs at the top of every tick — so a layer's on_update and an
 * entity's on_update both see the current one, and anything that stashes the pointer across a frame
 * is reading the next tick's contacts. Copy what has to outlive the tick.
 *
 * Never null; `count` is zero on a quiet tick and on a paused world.
 * */
NYA_API const NYA_PhysicsHit* nya_physics2d_hits(OUT u32* out_count) __attr_no_discard;

/**
 * The closing speed a contact needs before it appears in that list, in world units per second.
 *
 * Raise it so that only real impacts get through, lower it to hear scrapes. Takes effect on the next
 * step; contacts already reported are not revisited.
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
 *
 * What a click on the world is. Overlapping bodies resolve to whichever the broadphase reports
 * first, which is stable within a frame and not otherwise ordered — for a picker that has to be
 * exact, walk the candidates yourself.
 * */
NYA_API NYA_EntityHandle nya_physics2d_entity_at(f32x2 point) __attr_no_discard;
