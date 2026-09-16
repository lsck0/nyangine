#include "nyangine/nyangine.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API DECLARATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/** World units to metres and back. The whole unit boundary described in physics2d.h. */
NYA_INTERNAL b2Vec2 _nya_physics2d_to_meters(f32x2 world);
NYA_INTERNAL f32    _nya_physics2d_scalar_to_meters(f32 world);
NYA_INTERNAL f32x2  _nya_physics2d_to_world(b2Vec2 meters);

/** Null, and logged, when the handle does not resolve or the entity carries no body. */
NYA_INTERNAL NYA_Physics2DBody* _nya_physics2d_body_of(const NYA_Entity* entity, NYA_ConstCString operation);

/** Builds and attaches the shape described by `options`. False when the dimensions are nonsense. */
NYA_INTERNAL b8 _nya_physics2d_shape_create(b2BodyId body, const NYA_Entity* entity, const NYA_Physics2DBodyOptions* options);

/** b2OverlapResultFcn for nya_physics2d_entity_at: narrows a broadphase hit to a real point test. */
NYA_INTERNAL bool _nya_physics2d_point_query_callback(b2ShapeId shape, void* context);

/** b2PreSolveFcn: discards the contact when one side is a one-way surface passed the right way. */
NYA_INTERNAL bool _nya_physics2d_pre_solve(b2ShapeId shape_a, b2ShapeId shape_b, b2Pos point, b2Vec2 normal, void* context);

/**
 * Whether a contact between `surface` and `mover` should be solved.
 * */
NYA_INTERNAL b8 _nya_physics2d_one_way_admits(const NYA_Entity* surface, const NYA_Entity* mover, b2Vec2 normal);

/** Copies the step's hit events out of Box2D's transient buffer, in world units and entity handles. */
NYA_INTERNAL void _nya_physics2d_collect_hits(NYA_Physics2DSystem* system);

/**
 * Appends this step's sensor begin and end overlaps to the hit list.
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

    /* Single threaded on purpose; see nya_physics2d_one_way_set. */
    world_def.workerCount = 1;

    system->world = b2CreateWorld(&world_def);

    // read back rather than hardcoded, so it follows Box2D.
    system->contact_recycle_distance = b2World_GetContactRecycleDistance(system->world);

    // installed unconditionally: pre-solve is the only place to veto a contact, and the callback returns at once
    // for pairs without a one-way side, which is almost all of them.
    b2World_SetPreSolveCallback(system->world, _nya_physics2d_pre_solve, nullptr);

    nya_log_info("Physics2D system initialized (%.1f world units per metre, %u sub steps).", (f64)system->pixels_per_meter, system->sub_step_count);
}

void nya_system_physics2d_deinit(void) {
    NYA_Physics2DSystem* system = &nya_world()->physics2d_system;
    if (!system->initialized) return;

    // destroys every body, so `initialized` is cleared first: entity teardown runs afterwards and must not
    // detach ids already freed.
    system->initialized = false;
    b2DestroyWorld(system->world);

    *system = (NYA_Physics2DSystem){ 0 };

    nya_log_info("Physics system deinitialized.");
}

void nya_system_physics2d_update(f32 delta_time_s) {
    nya_perf_time_this_function();

    NYA_Physics2DSystem* system = &nya_world()->physics2d_system;

    // cleared first, so a tick that does not step reports no hits instead of repeating the last ones.
    system->hit_count = 0;

    if (!system->initialized || !system->enabled) return;
    if (delta_time_s <= 0.0F) return;

    system->step_count++;

    /* The drop-through windows, before the step. */
    u32 dropping = 0;

    nya_entity_foreach (entity) {
        if (entity->physics2d.drop_through_s <= 0.0F) continue;

        entity->physics2d.drop_through_s -= delta_time_s;
        if (entity->physics2d.drop_through_s < 0.0F) entity->physics2d.drop_through_s = 0.0F;

        if (entity->physics2d.drop_through_s > 0.0F) dropping++;
    }

    // contact recycling is off while anything is dropping; see contact_recycling_suspended.
    b8 suspend = dropping > 0;
    if (suspend != system->contact_recycling_suspended) {
        b2World_SetContactRecycleDistance(system->world, suspend ? 0.0F : system->contact_recycle_distance);
        system->contact_recycling_suspended = suspend;
    }

    u64 started_ns = nya_clock_get_monotonic_ns();
    b2World_Step(system->world, delta_time_s, (int)system->sub_step_count);
    system->last_step_time_s = (f32)nya_time_ns_to_s(nya_clock_get_monotonic_ns() - started_ns);

    // before copying transforms: Box2D's event buffer is only valid until the next step.
    _nya_physics2d_collect_hits(system);

    /* The solver is authoritative, so its result is copied out as is. */
    nya_entity_foreach (entity) {
        if (!entity->physics2d.attached) continue;
        if (!b2Body_IsAwake(entity->physics2d.id)) continue;

        b2Vec2 position = b2ToVec2(b2Body_GetPosition(entity->physics2d.id));
        f32x2  world    = _nya_physics2d_to_world(position);

        entity->position.x = world.x;
        entity->position.y = world.y;

        // the only angular freedom in 2D is roll about the screen's z axis.
        f32 angle        = b2Rot_GetAngle(b2Body_GetRotation(entity->physics2d.id));
        entity->rotation = nya_quaternion_from_euler(0.0F, 0.0F, angle);

        // mirrored so readers get the simulated velocity. integration is skipped for bodies, so this is a report.
        f32x2 velocity      = _nya_physics2d_to_world(b2Body_GetLinearVelocity(entity->physics2d.id));
        entity->velocity.x  = velocity.x;
        entity->velocity.y  = velocity.y;
        entity->velocity.z  = 0.0F;
        entity->angular_velocity.z = b2Body_GetAngularVelocity(entity->physics2d.id);
    }

    // last, so callbacks see this tick's transforms.
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
        nya_log_warn("Changing the physics scale with %u bodies already created; those keep their old size.", system->body_count);
    }

    system->pixels_per_meter = pixels_per_meter;

    // gravity is stored in world units, so the metric value changes with the scale.
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
        // a chain has no interior and so no mass. Box2D only rejects this inside the solver in a debug build.
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

    // Box2D v3 locks the degree of freedom instead of using infinite inertia, so linear responses are unchanged.
    body_def.motionLocks.angularZ = options.lock_rotation;

    // the entity table never moves, so the pointer is the cheapest way back from a shape. the handle is read off
    // the entity, so it cannot go stale.
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
        .one_way  = options.one_way,
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

    // guarded: teardown destroys the world before unwinding entities, so reaching here without a world is normal.
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

    // angular impulse is kg m^2 / s, so the conversion applies twice.
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

    // written through now, so a read in the same tick sees the new position, even for a sleeping body.
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
    // no body means nothing holds it up, a truthful answer for callers iterating mixed entities.
    if (entity == nullptr || !entity->physics2d.attached) return false;

    // the world owns physics.
    if (!nya_world_exists() || !nya_world()->physics2d_system.initialized) return false;

    NYA_Physics2DBody* body = (NYA_Physics2DBody*)&entity->physics2d;

    // cached for the tick, so repeated or ignored questions cost nothing.
    u64 step = nya_world()->physics2d_system.step_count + 1;
    if (body->grounded_step == step) return body->grounded;

    b2ContactData contacts[NYA_PHYSICS2D_MAX_CONTACTS_PER_BODY];

    int count = b2Body_GetContactData(body->id, contacts, NYA_PHYSICS2D_MAX_CONTACTS_PER_BODY);

    b8 grounded = false;

    for (int i = 0; i < count && !grounded; i++) {
        const b2ContactData* contact = &contacts[i];

        // no points: the narrowphase rejected the pair.
        if (contact->manifold.pointCount == 0) continue;

        /*
         * The manifold normal points from A to B, so which side we are sets its sign. As A, a normal pointing down the
         * screen means the other shape is below, which in a y-down world is standing on it.
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

void nya_physics2d_one_way_set(NYA_Entity* entity, NYA_Physics2DOneWay direction) {
    NYA_Physics2DBody* body = _nya_physics2d_body_of(entity, "set a one-way direction");
    if (body == nullptr) return;

    nya_assert(direction < NYA_PHYSICS2D_ONE_WAY_COUNT, "not a one-way direction: %d", (int)direction);

    body->one_way = direction;

    // woken: a sleeping body solves no contacts, so it would not fall through a platform made passable under it.
    if (b2Body_IsValid(body->id)) b2Body_SetAwake(body->id, true);
}

NYA_Physics2DOneWay nya_physics2d_one_way(const NYA_Entity* entity) {
    if (entity == nullptr || !entity->physics2d.attached) return NYA_PHYSICS2D_ONE_WAY_NONE;

    return entity->physics2d.one_way;
}

void nya_physics2d_drop_through(NYA_Entity* entity, f32 seconds) {
    NYA_Physics2DBody* body = _nya_physics2d_body_of(entity, "drop through a one-way surface");
    if (body == nullptr) return;

    // replaced: holding the button must not bank a longer fall.
    body->drop_through_s = seconds > 0.0F ? seconds : 0.0F;

    // a body resting on a platform sleeps and is never solved, so the request would never be read.
    if (b2Body_IsValid(body->id)) b2Body_SetAwake(body->id, true);
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

    // negative would count every resting contact, thousands per second.
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

NYA_EntityHandle nya_physics2d_raycast(f32x2 origin, f32x2 direction, OUT f32x2* out_point, OUT f32x2* out_normal) {
    NYA_Physics2DSystem* system = &nya_world()->physics2d_system;
    if (!system->initialized) return NYA_ENTITY_HANDLE_NONE;

    b2RayResult result = b2World_CastRayClosest(
        system->world,
        b2ToPos(_nya_physics2d_to_meters(origin)),
        _nya_physics2d_to_meters(direction),
        b2DefaultQueryFilter()
    );

    if (!result.hit) return NYA_ENTITY_HANDLE_NONE;

    NYA_Entity* entity = b2Body_GetUserData(b2Shape_GetBody(result.shapeId));
    if (entity == nullptr) return NYA_ENTITY_HANDLE_NONE;

    if (out_point != nullptr) *out_point = _nya_physics2d_to_world(b2ToVec2(result.point));

    // the normal is unit and dimensionless; converting it would stop it being unit.
    if (out_normal != nullptr) *out_normal = (f32x2){ result.normal.x, result.normal.y };

    return entity->handle;
}

NYA_EntityHandle nya_physics2d_entity_at(f32x2 point) {
    NYA_Physics2DSystem* system = &nya_world()->physics2d_system;
    if (!system->initialized) return NYA_ENTITY_HANDLE_NONE;

    b2Vec2 meters = _nya_physics2d_to_meters(point);

    /*
     * A zero-sized AABB, since Box2D v3 has no point query. It returns every shape whose box contains the point,
     * and b2Shape_TestPoint narrows each.
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

    // const in, mutable out: reading a velocity should not need a mutable entity.
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
     * Pre-solve events, which one-way surfaces need and Box2D leaves off because they run per contact per step.
     * The flag has to be on the moving body: Box2D ignores it on static shapes, and a ledge's visitor must opt in.
     */
    shape_def.enablePreSolveEvents = options->type == NYA_PHYSICS_BODY_DYNAMIC;

    /* On for every shape, not just sensors, since Box2D needs one side of a pair to ask. */
    shape_def.enableSensorEvents = true;

    // the solver only measures approach speed where one shape asked, so a crate is heard landing on terrain that
    // did not.
    shape_def.enableHitEvents = !options->ignore_hits;

    switch (options->shape) {
        case NYA_PHYSICS2D_SHAPE_BOX: {
            if (options->size.x <= 0.0F || options->size.y <= 0.0F) {
                nya_log_error("Entity '%s' asked for a box body of size " FMTf32x2 "; both extents must be positive.", name,
                              FMTf32x2_ARG(options->size));
                return false;
            }

            // b2MakeBox takes half extents; `size` is full width and height, like every draw call.
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

            // upright, since a capsule is usually a character: caps above and below the centre, `length` apart.
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
                // four is Box2D's minimum for an open chain: the end segments are ghosts that only supply neighbour normals.
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

            // terrain is where pickups land, so a chain sensors cannot see would make ground triggers inert.
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

            // unreachable through this API, but safer than dereferencing a body created directly.
            .a = a != nullptr ? a->handle : NYA_ENTITY_HANDLE_NONE,
            .b = b != nullptr ? b->handle : NYA_ENTITY_HANDLE_NONE,

            // z zero: the 2D world is the z = 0 plane. see physics_types.h.
            .point  = { point.x, point.y, 0.0F },
            .normal = { event->normal.x, event->normal.y, 0.0F },

            // a speed converts once; the unit normal does not convert.
            .approach_speed = event->approachSpeed * system->pixels_per_meter,
        };
    }

    system->hit_count = kept;

    // logged: silently dropping hits past the ceiling looks like a physics bug.
    if (available > kept) {
        nya_log_warn("Physics produced %u hits this step, past the %d that fit; %u were dropped.", available, NYA_PHYSICS2D_MAX_HITS, available - kept);
    }

    _nya_physics2d_collect_sensor_events(system);
}

void _nya_physics2d_collect_sensor_events(NYA_Physics2DSystem* system) {
    b2SensorEvents events = b2World_GetSensorEvents(system->world);

    u32 begin_count = (u32)nya_max(events.beginCount, 0);
    u32 end_count   = (u32)nya_max(events.endCount, 0);

    /* Counted rather than derived from what was written. */
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
         * Either shape may already be destroyed, which is what an exit caused by a despawn looks like (upstream
         * documents this for end events). The pair is skipped, since there is nobody left to tell.
         */
        if (!b2Shape_IsValid(event->sensorShapeId)) continue;
        if (!b2Shape_IsValid(event->visitorShapeId)) continue;

        (void)_nya_physics2d_sensor_hit_write(system, NYA_PHYSICS_HIT_SENSOR_EXIT, event->sensorShapeId, event->visitorShapeId);
    }

    if (dropped > 0) {
        nya_log_warn(
            "Physics produced %u sensor events this step and the hit list was already full; %u were dropped.",
            begin_count + end_count,
            dropped
        );
    }
}

b8 _nya_physics2d_sensor_hit_write(NYA_Physics2DSystem* system, NYA_PhysicsHitKind kind, b2ShapeId sensor_shape, b2ShapeId visitor_shape) {
    NYA_Entity* sensor  = b2Body_GetUserData(b2Shape_GetBody(sensor_shape));
    NYA_Entity* visitor = b2Body_GetUserData(b2Shape_GetBody(visitor_shape));

    // nothing to deliver to.
    if (sensor == nullptr && visitor == nullptr) return false;

    /* The midpoint of the two bodies. */
    f32x2 sensor_position  = sensor != nullptr ? sensor->position.xy : f32x2_zero;
    f32x2 visitor_position = visitor != nullptr ? visitor->position.xy : f32x2_zero;

    f32x2 point = sensor != nullptr && visitor != nullptr ? (sensor_position + visitor_position) * 0.5F
                                                          : (sensor != nullptr ? sensor_position : visitor_position);

    system->hits[system->hit_count++] = (NYA_PhysicsHit){
        .dimension = NYA_PHYSICS_2D,
        .kind      = kind,

        // sensor first, visitor second, so a pickup's callback reads itself as `entity`.
        .a = sensor != nullptr ? sensor->handle : NYA_ENTITY_HANDLE_NONE,
        .b = visitor != nullptr ? visitor->handle : NYA_ENTITY_HANDLE_NONE,

        .point = { point.x, point.y, 0.0F },

        // a sensor resolves nothing: no normal, no closing speed. zeroed over the slot's previous contents.
        .normal         = f32x3_zero,
        .approach_speed = 0.0F,
    };

    return true;
}

void _nya_physics2d_dispatch_collisions(const NYA_Physics2DSystem* system) {
    for (u32 i = 0; i < system->hit_count; i++) {
        const NYA_PhysicsHit* hit = &system->hits[i];

        /* Both sides, each told about the other, each resolved right before its call. */
        for (u32 side = 0; side < 2; side++) {
            NYA_EntityHandle self_handle  = side == 0 ? hit->a : hit->b;
            NYA_EntityHandle other_handle = side == 0 ? hit->b : hit->a;

            NYA_Entity* self = nya_entity_get(self_handle);
            if (self == nullptr) continue;

            NYA_EntityOnCollisionFn on_collision = nya_callback_get(self->on_collision);
            if (on_collision == nullptr) continue;

            // null when the other body has no entity; still a collision, so the callback runs.
            on_collision(self, nya_entity_get(other_handle), hit);
        }
    }
}

/** The axis a one-way direction admits passage along, in Box2D coordinates. */
NYA_INTERNAL b2Vec2 _nya_physics2d_one_way_axis(NYA_Physics2DOneWay direction) {
    switch (direction) {
        case NYA_PHYSICS2D_ONE_WAY_UP:    return (b2Vec2){ 0.0F, -1.0F };
        case NYA_PHYSICS2D_ONE_WAY_DOWN:  return (b2Vec2){ 0.0F, 1.0F };
        case NYA_PHYSICS2D_ONE_WAY_LEFT:  return (b2Vec2){ -1.0F, 0.0F };
        case NYA_PHYSICS2D_ONE_WAY_RIGHT: return (b2Vec2){ 1.0F, 0.0F };

        case NYA_PHYSICS2D_ONE_WAY_NONE:
        case NYA_PHYSICS2D_ONE_WAY_COUNT:
        default: return (b2Vec2){ 0.0F, 0.0F };
    }
}

b8 _nya_physics2d_one_way_admits(const NYA_Entity* surface, const NYA_Entity* mover, b2Vec2 normal) {
    if (surface == nullptr || mover == nullptr) return true;
    if (surface->physics2d.one_way == NYA_PHYSICS2D_ONE_WAY_NONE) return true;

    // the mover asked to pass everything ("press down to drop off").
    if (mover->physics2d.drop_through_s > 0.0F) return false;

    b2Vec2 velocity = b2Body_GetLinearVelocity(mover->physics2d.id);
    b2Vec2 axis     = _nya_physics2d_one_way_axis(surface->physics2d.one_way);

    /* The sign of the mover's velocity along the passable axis decides, not its position. */
    f32 approach = (velocity.x * axis.x) + (velocity.y * axis.y);

    // strictly positive, so a body at rest on the ledge it jumped onto stays on it.
    if (approach > 0.0F) return false;

    /* Which side of the surface the mover is on, for the resting case. */
    f32 side = (normal.x * axis.x) + (normal.y * axis.y);

    // underneath: not a floor from here.
    return side >= 0.0F;
}

bool _nya_physics2d_pre_solve(b2ShapeId shape_a, b2ShapeId shape_b, b2Pos point, b2Vec2 normal, void* context) {
    nya_unused(point, context);

    NYA_Entity* a = b2Body_GetUserData(b2Shape_GetBody(shape_a));
    NYA_Entity* b = b2Body_GetUserData(b2Shape_GetBody(shape_b));

    if (a == nullptr || b == nullptr) return true;

    // neither side is one-way: the contact stands. the common case.
    if (a->physics2d.one_way == NYA_PHYSICS2D_ONE_WAY_NONE && b->physics2d.one_way == NYA_PHYSICS2D_ONE_WAY_NONE) return true;

    // Box2D's normal points from A to B; the second call negates it so both orderings share one predicate.
    if (!_nya_physics2d_one_way_admits(a, b, normal)) return false;
    if (!_nya_physics2d_one_way_admits(b, a, (b2Vec2){ -normal.x, -normal.y })) return false;

    return true;
}

bool _nya_physics2d_point_query_callback(b2ShapeId shape, void* context) {
    NYA_Physics2DPointQuery* query = context;

    // the tree reports bounding box overlaps, larger than the shape for anything rotated.
    if (!b2Shape_TestPoint(shape, query->point)) return true;

    NYA_Entity* entity = b2Body_GetUserData(b2Shape_GetBody(shape));
    if (entity == nullptr) return true;

    query->result = entity->handle;

    // false stops the traversal at the first real hit.
    return false;
}
