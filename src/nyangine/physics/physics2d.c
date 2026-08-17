#include "nyangine/nyangine.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API DECLARATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/** World units to metres, and back. The whole of the unit boundary described in physics2d.h. */
NYA_INTERNAL b2Vec2 _nya_physics2d_to_meters(f32x2 world);
NYA_INTERNAL f32    _nya_physics2d_scalar_to_meters(f32 world);
NYA_INTERNAL f32x2  _nya_physics2d_to_world(b2Vec2 meters);

/** Null, and logged, when the handle does not resolve or the entity carries no body. */
NYA_INTERNAL NYA_Physics2DBody* _nya_physics2d_body_of(const NYA_Entity* entity, NYA_ConstCString operation);

/** Builds and attaches the shape described by `options`. False when the dimensions are nonsense. */
NYA_INTERNAL b8 _nya_physics2d_shape_create(b2BodyId body, const NYA_Entity* entity, const NYA_Physics2DBodyOptions* options);

/** b2OverlapResultFcn for nya_physics2d_entity_at: narrows the broadphase hit to a real point test. */
NYA_INTERNAL bool _nya_physics2d_point_query_callback(b2ShapeId shape, void* context);

/** Copies the step's hit events out of Box2D's transient buffer, in world units and entity handles. */
NYA_INTERNAL void _nya_physics2d_collect_hits(NYA_Physics2DSystem* system);

/**
 * Appends this step's sensor begin and end overlaps to the hit list.
 *
 * Separate from the contact events because Box2D reports them separately and they carry different
 * information — a sensor overlap has no point, no normal and no speed, so nothing about the impact
 * path applies to it. They share the list because everything downstream wants one walk over "what
 * happened this tick" rather than two.
 * */
NYA_INTERNAL void _nya_physics2d_collect_sensor_events(NYA_Physics2DSystem* system);

/** Writes one sensor overlap into the hit list. False when neither shape belongs to an entity. */
NYA_INTERNAL b8 _nya_physics2d_sensor_hit_write(NYA_Physics2DSystem* system, NYA_PhysicsHitKind kind, b2ShapeId sensor_shape, b2ShapeId visitor_shape);

/** Runs on_collision for both sides of every hit this step produced. */
NYA_INTERNAL void _nya_physics2d_dispatch_collisions(const NYA_Physics2DSystem* system);

typedef struct NYA_Physics2DPointQuery NYA_Physics2DPointQuery;

struct NYA_Physics2DPointQuery {
    b2Pos            point;
    NYA_EntityHandle result;
};

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PUBLIC API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/*
 * ─────────────────────────────────────────────────────────
 * SYSTEM FUNCTIONS
 * ─────────────────────────────────────────────────────────
 */

void nya_system_physics2d_init(void) {
    NYA_Physics2DSystem* system = &nya_world()->physics2d_system;

    *system = (NYA_Physics2DSystem){
        .initialized      = true,
        .enabled          = true,
        .pixels_per_meter = NYA_PHYSICS2D_PIXELS_PER_METER,
        .gravity          = NYA_PHYSICS2D_GRAVITY_DEFAULT,
        .sub_step_count   = NYA_PHYSICS2D_SUB_STEPS,
        .hit_threshold    = NYA_PHYSICS2D_HIT_THRESHOLD,
    };

    b2WorldDef world_def = b2DefaultWorldDef();
    world_def.gravity    = _nya_physics2d_to_meters(system->gravity);

    world_def.hitEventThreshold = _nya_physics2d_scalar_to_meters(system->hit_threshold);

    /*
     * Single threaded on purpose.
     *
     * b2WorldDef carries enqueueTask/finishTask hooks that would hand the solver's islands to the
     * job system, and that is the right thing to do once there is enough in the world to pay for
     * the handoff. It is not free: the callbacks run on the stepping thread's critical path, and a
     * world of a few hundred bodies spends more time dispatching than solving. Left for when a
     * profile says otherwise, rather than wired up speculatively.
     */
    world_def.workerCount = 1;

    system->world = b2CreateWorld(&world_def);

    nya_info("Physics2D system initialized (%.1f world units per metre, %u sub steps).", (f64)system->pixels_per_meter, system->sub_step_count);
}

void nya_system_physics2d_deinit(void) {
    NYA_Physics2DSystem* system = &nya_world()->physics2d_system;
    if (!system->initialized) return;

    // Takes every body with it, which is why `initialized` is cleared before anything else can run:
    // entity teardown happens afterwards and would otherwise detach ids this call has already freed.
    system->initialized = false;
    b2DestroyWorld(system->world);

    *system = (NYA_Physics2DSystem){ 0 };

    nya_info("Physics system deinitialized.");
}

void nya_system_physics2d_update(f32 delta_time_s) {
    nya_perf_time_this_function();

    NYA_Physics2DSystem* system = &nya_world()->physics2d_system;

    // Cleared first, so a tick that does not step reports no hits rather than the previous tick's.
    // A paused world producing the same impact over and over is the bug this prevents.
    system->hit_count = 0;

    if (!system->initialized || !system->enabled) return;
    if (delta_time_s <= 0.0F) return;

    system->step_count++;

    u64 started_ns = nya_clock_get_monotonic_ns();
    b2World_Step(system->world, delta_time_s, (int)system->sub_step_count);
    system->last_step_time_s = (f32)nya_time_ns_to_s(nya_clock_get_monotonic_ns() - started_ns);

    // Before the transforms are copied back, because Box2D's event buffer is only valid until the
    // next step and this is the one place that is guaranteed to be between two of them.
    _nya_physics2d_collect_hits(system);

    /*
     * The solver is authoritative, so its result is copied out rather than blended with anything.
     *
     * Only bodies that are awake are read back: a sleeping body has not moved, and the entity
     * already holds the transform it went to sleep at. That turns a world of settled crates from a
     * per tick copy of every transform into a check of a flag.
     */
    nya_entity_foreach (entity) {
        if (!entity->physics2d.attached) continue;
        if (!b2Body_IsAwake(entity->physics2d.id)) continue;

        b2Vec2 position = b2ToVec2(b2Body_GetPosition(entity->physics2d.id));
        f32x2  world    = _nya_physics2d_to_world(position);

        entity->position.x = world.x;
        entity->position.y = world.y;

        // Rotation about the screen's z axis, which is the only angular freedom a 2D body has.
        // nya_quaternion_from_euler takes pitch, yaw, roll — roll is the one.
        f32 angle        = b2Rot_GetAngle(b2Body_GetRotation(entity->physics2d.id));
        entity->rotation = nya_quaternion_from_euler(0.0F, 0.0F, angle);

        // Mirrored onto the entity so anything reading velocity gets the simulated value rather
        // than whatever it was spawned with. The entity's own integration is skipped for a body, so
        // this is a report and not an input.
        f32x2 velocity      = _nya_physics2d_to_world(b2Body_GetLinearVelocity(entity->physics2d.id));
        entity->velocity.x  = velocity.x;
        entity->velocity.y  = velocity.y;
        entity->velocity.z  = 0.0F;
        entity->angular_velocity.z = b2Body_GetAngularVelocity(entity->physics2d.id);
    }

    // Last, so a callback reads this tick's transforms rather than the ones the step started from.
    _nya_physics2d_dispatch_collisions(system);
}

/*
 * ─────────────────────────────────────────────────────────
 * WORLD
 * ─────────────────────────────────────────────────────────
 */

void nya_physics2d_gravity_set(f32x2 gravity) {
    NYA_Physics2DSystem* system = &nya_world()->physics2d_system;
    if (!system->initialized) return;

    system->gravity = gravity;
    b2World_SetGravity(system->world, _nya_physics2d_to_meters(gravity));
}

f32x2 nya_physics2d_gravity(void) {
    return nya_world()->physics2d_system.gravity;
}

void nya_physics2d_pixels_per_meter_set(f32 pixels_per_meter) {
    nya_assert(pixels_per_meter > 0.0F, "pixels_per_meter must be greater than 0.");

    NYA_Physics2DSystem* system = &nya_world()->physics2d_system;
    if (!system->initialized) return;

    if (system->body_count > 0) {
        nya_warn("Changing the physics scale with %u bodies already created; those keep their old size.", system->body_count);
    }

    system->pixels_per_meter = pixels_per_meter;

    // The stored gravity is in world units, so the metric value it converts to has moved.
    b2World_SetGravity(system->world, _nya_physics2d_to_meters(system->gravity));
}

f32 nya_physics2d_pixels_per_meter(void) {
    return nya_world()->physics2d_system.pixels_per_meter;
}

void nya_physics2d_enabled_set(b8 enabled) {
    nya_world()->physics2d_system.enabled = enabled;
}

b8 nya_physics2d_enabled(void) {
    return nya_world()->physics2d_system.enabled;
}

u32 nya_physics2d_body_count(void) {
    return nya_world()->physics2d_system.body_count;
}

f32 nya_physics2d_last_step_time_s(void) {
    return nya_world()->physics2d_system.last_step_time_s;
}

/*
 * ─────────────────────────────────────────────────────────
 * BODIES
 * ─────────────────────────────────────────────────────────
 */

b8 nya_physics2d_body_attach_with_options(NYA_EntityHandle entity_handle, NYA_Physics2DBodyOptions options) {
    NYA_Physics2DSystem* system = &nya_world()->physics2d_system;

    if (!system->initialized) {
        nya_log_error("Cannot attach a physics body: the physics system is not initialized.");
        return false;
    }

    NYA_Entity* entity = nya_entity_get(entity_handle);
    if (entity == nullptr) {
        nya_log_error("Cannot attach a physics body: entity handle %u/%u does not resolve.", entity_handle.index, entity_handle.generation);
        return false;
    }

    if (entity->physics2d.attached) {
        nya_log_error("Entity '%s' already has a physics body.", entity->name ? entity->name : "(unnamed)");
        return false;
    }

    if (options.shape == NYA_PHYSICS2D_SHAPE_CHAIN && options.type != NYA_PHYSICS_BODY_STATIC) {
        // A chain is a one sided line with no interior, so there is no area to compute a mass from.
        // Box2D rejects this too, but only from inside the solver and only in a debug build.
        nya_log_error("A chain shape is only valid on a static body; entity '%s' asked for one on a moving body.",
                      entity->name ? entity->name : "(unnamed)");
        return false;
    }

    f32 rotation = 0.0F;
    {
        f32 pitch, yaw;
        nya_quaternion_to_euler(entity->rotation, &pitch, &yaw, &rotation);
    }

    b2BodyDef body_def = b2DefaultBodyDef();

    body_def.type            = (b2BodyType)options.type;
    body_def.position        = b2ToPos(_nya_physics2d_to_meters((f32x2){ entity->position.x, entity->position.y }));
    body_def.rotation        = b2MakeRot(rotation);
    body_def.linearVelocity  = _nya_physics2d_to_meters((f32x2){ entity->velocity.x, entity->velocity.y });
    body_def.angularVelocity = entity->angular_velocity.z;
    body_def.linearDamping   = options.linear_damping;
    body_def.angularDamping  = options.angular_damping;
    body_def.gravityScale    = options.gravity_scale;
    body_def.isBullet        = options.is_bullet;
    body_def.enableSleep     = !options.never_sleep;
    body_def.name            = entity->name;

    // Not "infinite inertia": Box2D v3 locks the degree of freedom instead, so the body still
    // responds to linear collisions the way a mass of its size should.
    body_def.motionLocks.angularZ = options.lock_rotation;

    // The entity table never moves and the pointer outlives every step, so this is the cheapest
    // route from a shape the broadphase reported back to the entity that owns it. The handle is
    // read off the entity rather than stored, so it cannot go stale against a reused slot.
    body_def.userData = entity;

    b2BodyId body = b2CreateBody(system->world, &body_def);

    if (!_nya_physics2d_shape_create(body, entity, &options)) {
        b2DestroyBody(body);
        return false;
    }

    entity->physics2d = (NYA_Physics2DBody){
        .id       = body,
        .type     = options.type,
        .shape    = options.shape,
        .size     = options.size,
        .radius   = options.radius,
        .length   = options.length,
        .attached = true,
    };

    system->body_count++;

    return true;
}

void nya_physics2d_body_detach(NYA_EntityHandle entity_handle) {
    NYA_Physics2DSystem* system = &nya_world()->physics2d_system;

    NYA_Entity* entity = nya_entity_get(entity_handle);
    if (entity == nullptr) return;
    if (!entity->physics2d.attached) return;

    // Guarded rather than asserted: teardown destroys the world in one go and then unwinds the
    // entity table, so reaching here after the world is gone is the ordinary path, not a bug.
    if (system->initialized) {
        b2DestroyBody(entity->physics2d.id);
        nya_assert(system->body_count > 0, "Physics body count underflowed on detach.");
        system->body_count--;
    }

    entity->physics2d = (NYA_Physics2DBody){ 0 };
}

b8 nya_physics2d_body_attached(const NYA_Entity* entity) {
    return entity != nullptr && entity->physics2d.attached;
}

/*
 * ─────────────────────────────────────────────────────────
 * FORCES AND STATE
 * ─────────────────────────────────────────────────────────
 */

void nya_physics2d_apply_impulse(NYA_Entity* entity, f32x2 impulse) {
    NYA_Physics2DBody* body = _nya_physics2d_body_of(entity, "apply an impulse");
    if (body == nullptr) return;

    b2Body_ApplyLinearImpulseToCenter(body->id, _nya_physics2d_to_meters(impulse), true);
}

void nya_physics2d_apply_force(NYA_Entity* entity, f32x2 force) {
    NYA_Physics2DBody* body = _nya_physics2d_body_of(entity, "apply a force");
    if (body == nullptr) return;

    b2Body_ApplyForceToCenter(body->id, _nya_physics2d_to_meters(force), true);
}

void nya_physics2d_apply_angular_impulse(NYA_Entity* entity, f32 impulse) {
    NYA_Physics2DBody* body = _nya_physics2d_body_of(entity, "apply an angular impulse");
    if (body == nullptr) return;

    // Angular impulse is kg m^2 / s, so the world to metre conversion applies twice.
    f32 scale = nya_world()->physics2d_system.pixels_per_meter;
    b2Body_ApplyAngularImpulse(body->id, impulse / (scale * scale), true);
}

void nya_physics2d_velocity_set(NYA_Entity* entity, f32x2 velocity) {
    NYA_Physics2DBody* body = _nya_physics2d_body_of(entity, "set velocity");
    if (body == nullptr) return;

    b2Body_SetLinearVelocity(body->id, _nya_physics2d_to_meters(velocity));
}

f32x2 nya_physics2d_velocity(const NYA_Entity* entity) {
    NYA_Physics2DBody* body = _nya_physics2d_body_of(entity, "read velocity");
    if (body == nullptr) return f32x2_zero;

    return _nya_physics2d_to_world(b2Body_GetLinearVelocity(body->id));
}

void nya_physics2d_angular_velocity_set(NYA_Entity* entity, f32 radians_per_second) {
    NYA_Physics2DBody* body = _nya_physics2d_body_of(entity, "set angular velocity");
    if (body == nullptr) return;

    b2Body_SetAngularVelocity(body->id, radians_per_second);
}

f32 nya_physics2d_angular_velocity(const NYA_Entity* entity) {
    NYA_Physics2DBody* body = _nya_physics2d_body_of(entity, "read angular velocity");
    if (body == nullptr) return 0.0F;

    return b2Body_GetAngularVelocity(body->id);
}

void nya_physics2d_teleport(NYA_Entity* entity, f32x2 position, f32 rotation) {
    NYA_Physics2DBody* body = _nya_physics2d_body_of(entity, "teleport");
    if (body == nullptr) return;

    b2Body_SetTransform(body->id, b2ToPos(_nya_physics2d_to_meters(position)), b2MakeRot(rotation));

    // Written through immediately rather than waiting for the next step, so a teleport followed by
    // a read in the same tick sees where the thing now is. A sleeping body would otherwise not be
    // copied back at all.
    entity->position.x = position.x;
    entity->position.y = position.y;
    entity->rotation   = nya_quaternion_from_euler(0.0F, 0.0F, rotation);
}

f32 nya_physics2d_rotation(const NYA_Entity* entity) {
    NYA_Physics2DBody* body = _nya_physics2d_body_of(entity, "read rotation");
    if (body == nullptr) return 0.0F;

    return b2Rot_GetAngle(b2Body_GetRotation(body->id));
}

b8 nya_physics2d_grounded(const NYA_Entity* entity) {
    // No body means nothing is holding it up, which is a truthful answer rather than a misuse — a
    // caller iterating mixed entities should not have to filter first.
    if (entity == nullptr || !entity->physics2d.attached) return false;

    // The world owns physics now, not the app.
    if (!nya_world_exists() || !nya_world()->physics2d_system.initialized) return false;

    NYA_Physics2DBody* body = (NYA_Physics2DBody*)&entity->physics2d;

    // Remembered for the tick it was computed on. Asking twice in a frame is free, and asking about
    // an entity nobody cares about costs nothing at all — which is why this is not precomputed for
    // every body in the step.
    u64 step = nya_world()->physics2d_system.step_count + 1;
    if (body->grounded_step == step) return body->grounded;

    b2ContactData contacts[NYA_PHYSICS2D_MAX_CONTACTS_PER_BODY];

    int count = b2Body_GetContactData(body->id, contacts, NYA_PHYSICS2D_MAX_CONTACTS_PER_BODY);

    b8 grounded = false;

    for (int i = 0; i < count && !grounded; i++) {
        const b2ContactData* contact = &contacts[i];

        // A contact with no points is a pair the broadphase found and the narrowphase rejected.
        if (contact->manifold.pointCount == 0) continue;

        /*
         * The manifold normal points from shape A toward shape B, so which of the two we are decides
         * its sign. Being A and finding the normal pointing *down* the screen means the other shape
         * is below us — which in a y-down world is what standing on something looks like.
         */
        NYA_Entity* owner_a = b2Body_GetUserData(b2Shape_GetBody(contact->shapeIdA));

        f32 toward_other = owner_a == entity ? contact->manifold.normal.y : -contact->manifold.normal.y;

        grounded = toward_other >= NYA_PHYSICS2D_GROUND_NORMAL_MIN;
    }

    body->grounded      = grounded;
    body->grounded_step = step;

    return grounded;
}

b8 nya_physics2d_awake(const NYA_Entity* entity) {
    NYA_Physics2DBody* body = _nya_physics2d_body_of(entity, "read the sleep state");
    if (body == nullptr) return false;

    return b2Body_IsAwake(body->id);
}

void nya_physics2d_wake(NYA_Entity* entity) {
    NYA_Physics2DBody* body = _nya_physics2d_body_of(entity, "wake");
    if (body == nullptr) return;

    b2Body_SetAwake(body->id, true);
}

/*
 * ─────────────────────────────────────────────────────────
 * HITS
 * ─────────────────────────────────────────────────────────
 */

const NYA_PhysicsHit* nya_physics2d_hits(OUT u32* out_count) {
    nya_assert(out_count != nullptr);

    NYA_Physics2DSystem* system = &nya_world()->physics2d_system;

    *out_count = system->hit_count;
    return system->hits;
}

void nya_physics2d_hit_threshold_set(f32 world_units_per_second) {
    NYA_Physics2DSystem* system = &nya_world()->physics2d_system;
    if (!system->initialized) return;

    // Negative would mean every contact qualifies, including the resting ones this exists to filter
    // out, which is a stream of thousands per second rather than an obviously wrong setting.
    system->hit_threshold = nya_max(world_units_per_second, 0.0F);

    b2World_SetHitEventThreshold(system->world, _nya_physics2d_scalar_to_meters(system->hit_threshold));
}

f32 nya_physics2d_hit_threshold(void) {
    return nya_world()->physics2d_system.hit_threshold;
}

/*
 * ─────────────────────────────────────────────────────────
 * QUERIES
 * ─────────────────────────────────────────────────────────
 */

NYA_EntityHandle nya_physics2d_entity_at(f32x2 point) {
    NYA_Physics2DSystem* system = &nya_world()->physics2d_system;
    if (!system->initialized) return NYA_ENTITY_HANDLE_NONE;

    b2Vec2 meters = _nya_physics2d_to_meters(point);

    /*
     * A degenerate AABB rather than a point query, because Box2D v3 has no point query: the
     * broadphase works in boxes, and the exact test is b2Shape_TestPoint on each candidate. A zero
     * sized box is a legal AABB and reports every shape whose own box contains the point, which is
     * the smallest candidate set the tree can produce.
     */
    b2AABB aabb = { .lowerBound = meters, .upperBound = meters };

    NYA_Physics2DPointQuery query = { .point = b2ToPos(meters), .result = NYA_ENTITY_HANDLE_NONE };

    (void)b2World_OverlapAABB(system->world, b2Pos_zero, aabb, b2DefaultQueryFilter(), _nya_physics2d_point_query_callback, &query);

    return query.result;
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

b2Vec2 _nya_physics2d_to_meters(f32x2 world) {
    f32 scale = nya_world()->physics2d_system.pixels_per_meter;
    return (b2Vec2){ world.x / scale, world.y / scale };
}

f32 _nya_physics2d_scalar_to_meters(f32 world) {
    return world / nya_world()->physics2d_system.pixels_per_meter;
}

f32x2 _nya_physics2d_to_world(b2Vec2 meters) {
    f32 scale = nya_world()->physics2d_system.pixels_per_meter;
    return (f32x2){ meters.x * scale, meters.y * scale };
}

NYA_Physics2DBody* _nya_physics2d_body_of(const NYA_Entity* entity, NYA_ConstCString operation) {
    if (entity == nullptr) {
        nya_log_error("Cannot %s: the entity is null.", operation);
        return nullptr;
    }

    if (!entity->physics2d.attached) {
        nya_log_error("Cannot %s on entity '%s': it has no physics body.", operation, entity->name ? entity->name : "(unnamed)");
        return nullptr;
    }

    // Const in, mutable out. The accessors take a const NYA_Entity* because reading a velocity
    // should not demand a mutable entity, and every Box2D call below needs the id by value anyway.
    return (NYA_Physics2DBody*)&entity->physics2d;
}

b8 _nya_physics2d_shape_create(b2BodyId body, const NYA_Entity* entity, const NYA_Physics2DBodyOptions* options) {
    NYA_ConstCString name = entity->name ? entity->name : "(unnamed)";

    b2ShapeDef shape_def = b2DefaultShapeDef();

    shape_def.density              = options->density;
    shape_def.material.friction    = options->friction;
    shape_def.material.restitution = options->restitution;
    shape_def.isSensor             = options->is_sensor;
    shape_def.enableContactEvents  = true;

    /*
     * On for every shape, not just for the sensors.
     *
     * Box2D wants this set on *both* sides of a pair before it will report the overlap, and defaults
     * it off on both. A coin created with is_sensor and a player created without it therefore
     * produced no events whatsoever — which looks precisely like a coin the player never reached,
     * and is the one failure mode a trigger volume must not have.
     *
     * The cost is a flag on shapes that no sensor will ever touch. The solver still only does the
     * work for pairs where a sensor is actually involved, so what this buys is that a game never has
     * to know in advance which of its bodies might one day walk through a trigger.
     */
    shape_def.enableSensorEvents = true;

    // The solver only measures approach speed for pairs where at least one shape asked for it, which
    // is why this defaults on: a crate that opts out is still heard landing on terrain that did not.
    shape_def.enableHitEvents = !options->ignore_hits;

    switch (options->shape) {
        case NYA_PHYSICS2D_SHAPE_BOX: {
            if (options->size.x <= 0.0F || options->size.y <= 0.0F) {
                nya_log_error("Entity '%s' asked for a box body of size " FMTf32x2 "; both extents must be positive.", name,
                              FMTf32x2_ARG(options->size));
                return false;
            }

            // b2MakeBox takes half extents, and `size` is the full width and height, which is what
            // every draw call in the engine takes. Halving here is what keeps a body and the
            // rectangle drawn for it the same size.
            b2Polygon box = b2MakeBox(_nya_physics2d_scalar_to_meters(options->size.x * 0.5F), _nya_physics2d_scalar_to_meters(options->size.y * 0.5F));
            (void)b2CreatePolygonShape(body, &shape_def, &box);
            return true;
        }

        case NYA_PHYSICS2D_SHAPE_CIRCLE: {
            if (options->radius <= 0.0F) {
                nya_log_error("Entity '%s' asked for a circle body of radius %.3f; it must be positive.", name, (f64)options->radius);
                return false;
            }

            b2Circle circle = { .center = { 0.0F, 0.0F }, .radius = _nya_physics2d_scalar_to_meters(options->radius) };
            (void)b2CreateCircleShape(body, &shape_def, &circle);
            return true;
        }

        case NYA_PHYSICS2D_SHAPE_CAPSULE: {
            if (options->radius <= 0.0F || options->length <= 0.0F) {
                nya_log_error("Entity '%s' asked for a capsule body with radius %.3f and length %.3f; both must be positive.", name,
                              (f64)options->radius, (f64)options->length);
                return false;
            }

            // Upright, because a capsule is nearly always a character: the caps are above and below
            // the centre, `length` apart, and the body's rotation turns it from there.
            f32      half    = _nya_physics2d_scalar_to_meters(options->length * 0.5F);
            b2Capsule capsule = {
                .center1 = { 0.0F, -half },
                .center2 = { 0.0F, half },
                .radius  = _nya_physics2d_scalar_to_meters(options->radius),
            };
            (void)b2CreateCapsuleShape(body, &shape_def, &capsule);
            return true;
        }

        case NYA_PHYSICS2D_SHAPE_CHAIN: {
            if (options->points == nullptr || options->point_count < 4) {
                // Four is Box2D's floor for an open chain: the first and last segments are ghosts
                // that only exist to give the interior segments their neighbour normals, so a chain
                // of three points has exactly one real segment and no smoothing to do.
                nya_log_error("Entity '%s' asked for a chain body with %u points; an open chain needs at least 4.", name, options->point_count);
                return false;
            }

            if (options->point_count > NYA_PHYSICS2D_CHAIN_MAX_POINTS) {
                nya_log_error("Entity '%s' asked for a chain body with %u points, over the %d point limit.", name, options->point_count,
                              NYA_PHYSICS2D_CHAIN_MAX_POINTS);
                return false;
            }

            b2Vec2 points[NYA_PHYSICS2D_CHAIN_MAX_POINTS];
            for (u32 i = 0; i < options->point_count; i++) points[i] = _nya_physics2d_to_meters(options->points[i]);

            b2SurfaceMaterial material = b2DefaultSurfaceMaterial();
            material.friction          = options->friction;
            material.restitution       = options->restitution;

            b2ChainDef chain_def = b2DefaultChainDef();

            chain_def.points        = points;
            chain_def.count         = (int)options->point_count;
            chain_def.materials     = &material;
            chain_def.materialCount = 1;
            chain_def.isLoop        = false;

            // Same reason as the shape flag above: terrain is what a falling pickup lands on, and a
            // chain that sensors cannot see makes a trigger laid along the ground inert.
            chain_def.enableSensorEvents = true;

            (void)b2CreateChain(body, &chain_def);
            return true;
        }

        case NYA_PHYSICS2D_SHAPE_COUNT:
        default: {
            nya_log_error("Entity '%s' asked for physics shape %d, which is not a shape.", name, (s32)options->shape);
            return false;
        }
    }
}

void _nya_physics2d_collect_hits(NYA_Physics2DSystem* system) {
    b2ContactEvents events = b2World_GetContactEvents(system->world);

    u32 available = (u32)nya_max(events.hitCount, 0);
    u32 kept      = nya_min(available, (u32)NYA_PHYSICS2D_MAX_HITS);

    for (u32 i = 0; i < kept; i++) {
        const b2ContactHitEvent* event = &events.hitEvents[i];

        NYA_Entity* a = b2Body_GetUserData(b2Shape_GetBody(event->shapeIdA));
        NYA_Entity* b = b2Body_GetUserData(b2Shape_GetBody(event->shapeIdB));

        f32x2 point = _nya_physics2d_to_world(b2ToVec2(event->point));

        system->hits[i] = (NYA_PhysicsHit){
            .dimension = NYA_PHYSICS_2D,
            .kind      = NYA_PHYSICS_HIT_IMPACT,

            // A body with no entity behind it cannot happen through this API, but a null handle is
            // a better answer than dereferencing whatever a future direct b2CreateBody left there.
            .a = a != nullptr ? a->handle : NYA_ENTITY_HANDLE_NONE,
            .b = b != nullptr ? b->handle : NYA_ENTITY_HANDLE_NONE,

            // Widened to three components with a zero z, which is not a fiction: the 2D world is
            // the z = 0 plane. See physics_types.h for why one hit type serves both solvers.
            .point  = { point.x, point.y, 0.0F },
            .normal = { event->normal.x, event->normal.y, 0.0F },

            // Speed, so the world/metre conversion applies once. The normal is a unit vector and is
            // dimensionless, which is why it is copied across rather than scaled.
            .approach_speed = event->approachSpeed * system->pixels_per_meter,
        };
    }

    system->hit_count = kept;

    // Logged rather than swallowed: a burst past the ceiling means the reaction to these is already
    // being rationed, and silently dropping the tail makes that look like a physics bug instead.
    if (available > kept) {
        nya_warn("Physics produced %u hits this step, past the %d that fit; %u were dropped.", available, NYA_PHYSICS2D_MAX_HITS, available - kept);
    }

    _nya_physics2d_collect_sensor_events(system);
}

void _nya_physics2d_collect_sensor_events(NYA_Physics2DSystem* system) {
    b2SensorEvents events = b2World_GetSensorEvents(system->world);

    u32 begin_count = (u32)nya_max(events.beginCount, 0);
    u32 end_count   = (u32)nya_max(events.endCount, 0);

    /*
     * Counted rather than derived from what was written.
     *
     * An event can go unreported for two unrelated reasons — the list was full, or the pair was one
     * of the destroyed shapes an end event is allowed to name — and only the first is worth telling
     * anyone about. Subtracting what was appended from what arrived conflates them, and reports a
     * perfectly ordinary despawn inside a trigger as a dropped event.
     */
    u32 dropped = 0;

    for (u32 i = 0; i < begin_count; i++) {
        if (system->hit_count >= NYA_PHYSICS2D_MAX_HITS) {
            dropped += begin_count - i;
            break;
        }

        const b2SensorBeginTouchEvent* event = &events.beginEvents[i];

        (void)_nya_physics2d_sensor_hit_write(system, NYA_PHYSICS_HIT_SENSOR_ENTER, event->sensorShapeId, event->visitorShapeId);
    }

    for (u32 i = 0; i < end_count; i++) {
        if (system->hit_count >= NYA_PHYSICS2D_MAX_HITS) {
            dropped += end_count - i;
            break;
        }

        const b2SensorEndTouchEvent* event = &events.endEvents[i];

        /*
         * Either shape may already be destroyed, which is exactly what an exit caused by a despawn
         * looks like — and is why upstream documents this check on the end event and not the begin
         * one. Skipping the pair entirely rather than reporting half of it: an exit whose sensor is
         * gone has nobody left to tell.
         */
        if (!b2Shape_IsValid(event->sensorShapeId)) continue;
        if (!b2Shape_IsValid(event->visitorShapeId)) continue;

        (void)_nya_physics2d_sensor_hit_write(system, NYA_PHYSICS_HIT_SENSOR_EXIT, event->sensorShapeId, event->visitorShapeId);
    }

    if (dropped > 0) {
        nya_warn(
            "Physics produced %u sensor events this step and the hit list was already full; %u were dropped.",
            begin_count + end_count,
            dropped
        );
    }
}

b8 _nya_physics2d_sensor_hit_write(NYA_Physics2DSystem* system, NYA_PhysicsHitKind kind, b2ShapeId sensor_shape, b2ShapeId visitor_shape) {
    NYA_Entity* sensor  = b2Body_GetUserData(b2Shape_GetBody(sensor_shape));
    NYA_Entity* visitor = b2Body_GetUserData(b2Shape_GetBody(visitor_shape));

    // Nothing to deliver to. Both sides having no entity is not reachable through this API, but a
    // sensor pair where neither side is an entity would be a hit nobody could react to anyway.
    if (sensor == nullptr && visitor == nullptr) return false;

    /*
     * The midpoint of the two bodies.
     *
     * Box2D reports no geometry for a sensor overlap — only that it happened, and between which two
     * shapes. The honest options are "nowhere" and "somewhere between them", and the second is what a
     * pickup effect actually needs. It is deliberately not either body's own position: privileging
     * one would make the point jump depending on which side of the pair a caller happened to be.
     */
    f32x2 sensor_position  = sensor != nullptr ? sensor->position.xy : f32x2_zero;
    f32x2 visitor_position = visitor != nullptr ? visitor->position.xy : f32x2_zero;

    f32x2 point = sensor != nullptr && visitor != nullptr ? (sensor_position + visitor_position) * 0.5F
                                                          : (sensor != nullptr ? sensor_position : visitor_position);

    system->hits[system->hit_count++] = (NYA_PhysicsHit){
        .dimension = NYA_PHYSICS_2D,
        .kind      = kind,

        // Sensor first, visitor second, which is the one place the two sides of a hit are ordered.
        // A pickup's callback reads `entity` as itself and `other` as whatever walked in, and that
        // only holds because the sensor is always A.
        .a = sensor != nullptr ? sensor->handle : NYA_ENTITY_HANDLE_NONE,
        .b = visitor != nullptr ? visitor->handle : NYA_ENTITY_HANDLE_NONE,

        .point = { point.x, point.y, 0.0F },

        // A sensor resolves nothing, so there is no contact normal and no closing speed to report.
        // Zeroed rather than left at whatever the previous occupant of this slot held.
        .normal         = f32x3_zero,
        .approach_speed = 0.0F,
    };

    return true;
}

void _nya_physics2d_dispatch_collisions(const NYA_Physics2DSystem* system) {
    for (u32 i = 0; i < system->hit_count; i++) {
        const NYA_PhysicsHit* hit = &system->hits[i];

        /*
         * Both sides, each told about the other, and each resolved immediately before it is called.
         *
         * A callback may despawn either entity — that is the ordinary reaction to a hard enough
         * impact — so the second lookup cannot reuse the first's pointer. Handles are what make that
         * checkable rather than a use after free.
         */
        for (u32 side = 0; side < 2; side++) {
            NYA_EntityHandle self_handle  = side == 0 ? hit->a : hit->b;
            NYA_EntityHandle other_handle = side == 0 ? hit->b : hit->a;

            NYA_Entity* self = nya_entity_get(self_handle);
            if (self == nullptr) continue;

            NYA_EntityOnCollisionFn on_collision = nya_callback_get(self->on_collision);
            if (on_collision == nullptr) continue;

            // Null when the other body has no entity, which this API cannot produce but a direct
            // b2CreateBody could. The callback is handed it rather than being skipped, because "hit
            // something that is not an entity" is still a collision.
            on_collision(self, nya_entity_get(other_handle), hit);
        }
    }
}

bool _nya_physics2d_point_query_callback(b2ShapeId shape, void* context) {
    NYA_Physics2DPointQuery* query = context;

    // The tree reports whatever overlaps a shape's *bounding box*, which for anything but an
    // unrotated box is larger than the shape. Without this the corners of a tilted crate would
    // register as hits on empty space.
    if (!b2Shape_TestPoint(shape, query->point)) return true;

    NYA_Entity* entity = b2Body_GetUserData(b2Shape_GetBody(shape));
    if (entity == nullptr) return true;

    query->result = entity->handle;

    // False stops the traversal: the first real hit is the answer.
    return false;
}
