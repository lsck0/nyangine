#include "nyangine/nyangine.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API DECLARATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/** World units to metres and back. The unit boundary described in physics3d.h. */
NYA_INTERNAL b3Vec3 _nya_physics3d_to_meters(f32x3 world);
NYA_INTERNAL f32    _nya_physics3d_scalar_to_meters(f32 world);
NYA_INTERNAL f32x3  _nya_physics3d_to_world(b3Vec3 meters);

/** Box3D stores the vector part of a quaternion separately. */
NYA_INTERNAL b3Quat         _nya_physics3d_to_b3_quat(NYA_Quaternion rotation);
NYA_INTERNAL NYA_Quaternion _nya_physics3d_from_b3_quat(b3Quat rotation);

/** Null, and logged, when the handle does not resolve or the entity carries no 3D body. */
NYA_INTERNAL NYA_Physics3DBody* _nya_physics3d_body_of(const NYA_Entity* entity, NYA_ConstCString operation);

/** Builds and attaches the shape described by `options`. False when the dimensions are nonsense. */
NYA_INTERNAL b8 _nya_physics3d_shape_create(b3BodyId body, const NYA_Entity* entity, const NYA_Physics3DBodyOptions* options,
                                            OUT b3MeshData** out_mesh, OUT b3HeightFieldData** out_height_field);

/** Copies the step's hit events out of Box3D's transient buffer, in world units and entity handles. */
NYA_INTERNAL void _nya_physics3d_collect_hits(NYA_Physics3DSystem* system);

/** Appends this step's sensor begin and end overlaps to the hit list. See the 2D counterpart. */
NYA_INTERNAL void _nya_physics3d_collect_sensor_events(NYA_Physics3DSystem* system);

/** Writes one sensor overlap into the hit list. False when neither shape belongs to an entity. */
NYA_INTERNAL b8 _nya_physics3d_sensor_hit_write(NYA_Physics3DSystem* system, NYA_PhysicsHitKind kind, b3ShapeId sensor_shape, b3ShapeId visitor_shape);

/** Runs on_collision for both sides of every hit this step produced. */
NYA_INTERNAL void _nya_physics3d_dispatch_collisions(const NYA_Physics3DSystem* system);

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

void nya_system_physics3d_init(void) {
    NYA_Physics3DSystem* system = &nya_world()->physics3d_system;

    // see the identical note in physics2d.c: taken once because Box3D bakes both into its world def at
    // creation and offers no way to change sub_step_count later.
    const NYA_ConfigEnginePhysics* config = &nya_config_engine()->physics;

    *system = (NYA_Physics3DSystem){
        .initialized     = true,
        .enabled         = true,
        .units_per_meter = NYA_PHYSICS3D_UNITS_PER_METER,
        .gravity = config->gravity > 0.0F ? (f32x3){ 0.0F, -config->gravity * NYA_PHYSICS3D_UNITS_PER_METER, 0.0F } : NYA_PHYSICS3D_GRAVITY_DEFAULT,
        .sub_step_count = config->sub_steps > 0 ? config->sub_steps : NYA_PHYSICS3D_SUB_STEPS,
        .hit_threshold  = NYA_PHYSICS3D_HIT_THRESHOLD,
    };

    b3WorldDef world_def = b3DefaultWorldDef();
    world_def.gravity    = _nya_physics3d_to_meters(system->gravity);

    world_def.hitEventThreshold = _nya_physics3d_scalar_to_meters(system->hit_threshold);

    // single threaded, as in the 2D world: for a few hundred bodies the island dispatch costs more
    // than the solve.
    world_def.workerCount = 1;

    system->world = b3CreateWorld(&world_def);
    nya_assert(b3World_IsValid(system->world), "more than B3_MAX_WORLDS (%d) worlds at once; see BOX3D_MAX_WORLDS", B3_MAX_WORLDS);

    nya_log_info("Physics3D system initialized (%.1f world units per metre, %u sub steps).", (f64)system->units_per_meter, system->sub_step_count);
}

void nya_system_physics3d_deinit(void) {
    NYA_Physics3DSystem* system = &nya_world()->physics3d_system;
    if (!system->initialized) return;

    // cleared first, so entity teardown does not hand ids back to a world that already freed them.
    system->initialized = false;
    b3DestroyWorld(system->world);

    *system = (NYA_Physics3DSystem){ 0 };

    nya_log_info("Physics3D system deinitialized.");
}

void nya_system_physics3d_update(f32 delta_time_s) {
    nya_perf_time_this_function();
    nya_trace_scope(NYA_TRACE_PHYSICS3D);

    NYA_Physics3DSystem* system = &nya_world()->physics3d_system;

    system->hit_count = 0;

    if (!system->initialized || !system->enabled) return;
    if (delta_time_s <= 0.0F) return;

    /*
     * Cheap when nothing is in it, which is the common case.
     */
    if (system->body_count == 0) return;

    system->step_count++;

    u64 started_ns = nya_clock_get_monotonic_ns();
    b3World_Step(system->world, delta_time_s, (int)system->sub_step_count);
    system->last_step_time_s = (f32)nya_time_ns_to_s(nya_clock_get_monotonic_ns() - started_ns);

    _nya_physics3d_collect_hits(system);

    nya_entity_foreach (entity) {
        if (!entity->physics3d.attached) continue;
        if (!b3Body_IsAwake(entity->physics3d.id)) continue;

        entity->position = _nya_physics3d_to_world(b3ToVec3(b3Body_GetPosition(entity->physics3d.id)));

        // the whole quaternion. A 3D body has three angular degrees of freedom.
        entity->rotation = _nya_physics3d_from_b3_quat(b3Body_GetRotation(entity->physics3d.id));

        entity->velocity         = _nya_physics3d_to_world(b3Body_GetLinearVelocity(entity->physics3d.id));
        b3Vec3 angular           = b3Body_GetAngularVelocity(entity->physics3d.id);
        entity->angular_velocity = (f32x3){ angular.x, angular.y, angular.z };
    }

    _nya_physics3d_dispatch_collisions(system);
}

/*
 * ─────────────────────────────────────────────────────────
 * WORLD
 * ─────────────────────────────────────────────────────────
 */

void nya_physics3d_gravity_set(f32x3 gravity) {
    NYA_Physics3DSystem* system = &nya_world()->physics3d_system;
    if (!system->initialized) return;

    system->gravity = gravity;
    b3World_SetGravity(system->world, _nya_physics3d_to_meters(gravity));
}

f32x3 nya_physics3d_gravity(void) {
    return nya_world()->physics3d_system.gravity;
}

void nya_physics3d_units_per_meter_set(f32 units_per_meter) {
    nya_assert(units_per_meter > 0.0F, "units per metre must be positive, got %f", (f64)units_per_meter);

    NYA_Physics3DSystem* system = &nya_world()->physics3d_system;

    system->units_per_meter = units_per_meter;

    // gravity and speed were converted through the old scale, so they go through the new one too.
    if (system->initialized) {
        b3World_SetGravity(system->world, _nya_physics3d_to_meters(system->gravity));
        b3World_SetHitEventThreshold(system->world, _nya_physics3d_scalar_to_meters(system->hit_threshold));
    }
}

f32 nya_physics3d_units_per_meter(void) {
    return nya_world()->physics3d_system.units_per_meter;
}

void nya_physics3d_enabled_set(b8 enabled) {
    nya_world()->physics3d_system.enabled = enabled;
}

b8 nya_physics3d_enabled(void) {
    return nya_world()->physics3d_system.enabled;
}

u32 nya_physics3d_body_count(void) {
    return nya_world()->physics3d_system.body_count;
}

f32 nya_physics3d_last_step_time_s(void) {
    return nya_world()->physics3d_system.last_step_time_s;
}

/*
 * ─────────────────────────────────────────────────────────
 * BODIES
 * ─────────────────────────────────────────────────────────
 */

b8 nya_physics3d_body_attach_with_options(NYA_EntityHandle handle, NYA_Physics3DBodyOptions options) {
    NYA_Physics3DSystem* system = &nya_world()->physics3d_system;
    if (!system->initialized) return false;

    NYA_Entity* entity = nya_entity_get(handle);
    if (entity == nullptr) {
        nya_log_error("Cannot attach a 3D body: the entity handle does not resolve.");
        return false;
    }

    NYA_ConstCString name = entity->name ? entity->name : "(unnamed)";

    if (entity->physics3d.attached) {
        nya_log_error("Entity '%s' already has a 3D physics body.", name);
        return false;
    }

    b3BodyDef body_def = b3DefaultBodyDef();

    body_def.type     = (b3BodyType)options.type;
    body_def.position = b3ToPos(_nya_physics3d_to_meters(entity->position));
    body_def.rotation = _nya_physics3d_to_b3_quat(entity->rotation);

    // seeded from the entity, so spawning with an initial throw is one spawn and one attach.
    body_def.linearVelocity  = _nya_physics3d_to_meters(entity->velocity);
    body_def.angularVelocity = (b3Vec3){ entity->angular_velocity.x, entity->angular_velocity.y, entity->angular_velocity.z };

    body_def.linearDamping  = options.linear_damping;
    body_def.angularDamping = options.angular_damping;
    body_def.gravityScale   = options.gravity_scale;
    body_def.enableSleep    = !options.never_sleep;
    body_def.isBullet       = options.is_bullet;
    body_def.userData       = entity;

    if (options.lock_rotation) {
        body_def.motionLocks.angularX = true;
        body_def.motionLocks.angularY = true;
        body_def.motionLocks.angularZ = true;
    }

    b3BodyId body = b3CreateBody(system->world, &body_def);

    b3MeshData*        mesh         = nullptr;
    b3HeightFieldData* height_field = nullptr;

    if (!_nya_physics3d_shape_create(body, entity, &options, &mesh, &height_field)) {
        // a body without a shape falls forever, so a rejected attach destroys it.
        b3DestroyBody(body);
        return false;
    }

    b3Body_EnableHitEvents(body, !options.ignore_hits);

    entity->physics3d = (NYA_Physics3DBody){
        .id           = body,
        .type         = options.type,
        .shape        = options.shape,
        .size         = options.size,
        .radius       = options.radius,
        .length       = options.length,
        .mesh         = mesh,
        .height_field = height_field,
        .layers        = options.layers,
        .collides_with = options.collides_with,
        .attached     = true,
    };

    system->body_count++;

    return true;
}

void nya_physics3d_body_detach(NYA_EntityHandle handle) {
    NYA_Physics3DSystem* system = &nya_world()->physics3d_system;

    NYA_Entity* entity = nya_entity_get(handle);
    if (entity == nullptr || !entity->physics3d.attached) return;

    /*
     * The mesh first, and outside the check on the system below.
     */
    if (entity->physics3d.mesh != nullptr) b3DestroyMesh((b3MeshData*)entity->physics3d.mesh);
    if (entity->physics3d.height_field != nullptr) b3DestroyHeightField((b3HeightFieldData*)entity->physics3d.height_field);

    // the body id belongs to a world that is gone once the system is down.
    if (system->initialized) {
        b3DestroyBody(entity->physics3d.id);
        system->body_count--;
    }

    entity->physics3d = (NYA_Physics3DBody){ 0 };
}

b8 nya_physics3d_body_attached(const NYA_Entity* entity) {
    return entity != nullptr && entity->physics3d.attached;
}

/*
 * ─────────────────────────────────────────────────────────
 * FORCES AND STATE
 * ─────────────────────────────────────────────────────────
 */

void nya_physics3d_apply_impulse(NYA_Entity* entity, f32x3 impulse) {
    NYA_Physics3DBody* body = _nya_physics3d_body_of(entity, "apply an impulse to");
    if (body == nullptr) return;

    b3Body_ApplyLinearImpulseToCenter(body->id, _nya_physics3d_to_meters(impulse), true);
}

void nya_physics3d_apply_force(NYA_Entity* entity, f32x3 force) {
    NYA_Physics3DBody* body = _nya_physics3d_body_of(entity, "apply a force to");
    if (body == nullptr) return;

    b3Body_ApplyForceToCenter(body->id, _nya_physics3d_to_meters(force), true);
}

void nya_physics3d_apply_angular_impulse(NYA_Entity* entity, f32x3 impulse) {
    NYA_Physics3DBody* body = _nya_physics3d_body_of(entity, "apply an angular impulse to");
    if (body == nullptr) return;

    b3Body_ApplyAngularImpulse(body->id, (b3Vec3){ impulse.x, impulse.y, impulse.z }, true);
}

void nya_physics3d_velocity_set(NYA_Entity* entity, f32x3 velocity) {
    NYA_Physics3DBody* body = _nya_physics3d_body_of(entity, "set the velocity of");
    if (body == nullptr) return;

    b3Body_SetLinearVelocity(body->id, _nya_physics3d_to_meters(velocity));
}

f32x3 nya_physics3d_velocity(const NYA_Entity* entity) {
    if (entity == nullptr || !entity->physics3d.attached) return f32x3_zero;

    return _nya_physics3d_to_world(b3Body_GetLinearVelocity(entity->physics3d.id));
}

void nya_physics3d_angular_velocity_set(NYA_Entity* entity, f32x3 radians_per_second) {
    NYA_Physics3DBody* body = _nya_physics3d_body_of(entity, "set the angular velocity of");
    if (body == nullptr) return;

    // radians are dimensionless and do not cross the unit boundary.
    b3Body_SetAngularVelocity(body->id, (b3Vec3){ radians_per_second.x, radians_per_second.y, radians_per_second.z });
}

f32x3 nya_physics3d_angular_velocity(const NYA_Entity* entity) {
    if (entity == nullptr || !entity->physics3d.attached) return f32x3_zero;

    b3Vec3 angular = b3Body_GetAngularVelocity(entity->physics3d.id);

    return (f32x3){ angular.x, angular.y, angular.z };
}

void nya_physics3d_teleport(NYA_Entity* entity, f32x3 position, NYA_Quaternion rotation) {
    NYA_Physics3DBody* body = _nya_physics3d_body_of(entity, "teleport");
    if (body == nullptr) return;

    b3Body_SetTransform(body->id, b3ToPos(_nya_physics3d_to_meters(position)), _nya_physics3d_to_b3_quat(rotation));

    // mirrored now, so readers before the next step see the move. A sleeping body is skipped by the
    // readback loop and would never report it.
    entity->position = position;
    entity->rotation = rotation;

    nya_entity_transform_snap(entity);
}

b8 nya_physics3d_grounded(const NYA_Entity* entity) {
    if (entity == nullptr || !entity->physics3d.attached) return false;
    if (!nya_world_exists() || !nya_world()->physics3d_system.initialized) return false;

    NYA_Physics3DBody* body = (NYA_Physics3DBody*)&entity->physics3d;

    u64 step = nya_world()->physics3d_system.step_count + 1;
    if (body->grounded_step == step) return body->grounded;

    b3ContactData contacts[NYA_PHYSICS3D_MAX_CONTACTS_PER_BODY];

    int count = b3Body_GetContactData(body->id, contacts, NYA_PHYSICS3D_MAX_CONTACTS_PER_BODY);

    b8 grounded = false;

    for (int i = 0; i < count && !grounded; i++) {
        const b3ContactData* contact = &contacts[i];
        if (contact->manifolds == nullptr || contact->manifolds->pointCount == 0) continue;

        /*
         * The manifold normal points from A to B, so which shape we are decides its sign. Below is
         * negative y in 3D, hence the minus the 2D version does not have.
         */
        NYA_Entity* owner_a = b3Body_GetUserData(b3Shape_GetBody(contact->shapeIdA));

        f32 toward_other = owner_a == entity ? contact->manifolds->normal.y : -contact->manifolds->normal.y;

        grounded = -toward_other >= NYA_PHYSICS3D_GROUND_NORMAL_MIN;
    }

    body->grounded      = grounded;
    body->grounded_step = step;

    return grounded;
}

b8 nya_physics3d_awake(const NYA_Entity* entity) {
    if (entity == nullptr || !entity->physics3d.attached) return false;

    return b3Body_IsAwake(entity->physics3d.id);
}

void nya_physics3d_wake(NYA_Entity* entity) {
    NYA_Physics3DBody* body = _nya_physics3d_body_of(entity, "wake");
    if (body == nullptr) return;

    b3Body_SetAwake(body->id, true);
}

/*
 * ─────────────────────────────────────────────────────────
 * COLLISION LAYERS
 * ─────────────────────────────────────────────────────────
 */

void nya_physics3d_layers_set(NYA_Entity* entity, NYA_PhysicsLayerMask layers, NYA_PhysicsLayerMask collides_with) {
    NYA_Physics3DBody* body = _nya_physics3d_body_of(entity, "set the collision layers of");
    if (body == nullptr) return;

    body->layers        = layers;
    body->collides_with = collides_with;

    if (!b3Body_IsValid(body->id)) return;

    b3ShapeId shapes[NYA_PHYSICS3D_MAX_SHAPES_PER_BODY];

    int count = b3Body_GetShapes(body->id, shapes, NYA_PHYSICS3D_MAX_SHAPES_PER_BODY);
    nya_assert(count >= 0);

    // this API attaches exactly one shape per body, so the bound is a statement about that rather than
    // a guess; a body past it would be one Box3D grew behind our back.
    nya_assert(count < NYA_PHYSICS3D_MAX_SHAPES_PER_BODY, "a 3D body has more shapes than this API can create");

    for (int i = 0; i < count; i++) {
        b3Filter filter = b3Shape_GetFilter(shapes[i]);

        filter.categoryBits = layers;
        filter.maskBits     = collides_with;

        // true: contacts the new filter forbids are dropped now rather than surviving until the pair
        // next leaves the broadphase, which for a resting body is never.
        b3Shape_SetFilter(shapes[i], filter, true);
    }

    b3Body_SetAwake(body->id, true);
}

NYA_PhysicsLayerMask nya_physics3d_layers(const NYA_Entity* entity) {
    if (entity == nullptr || !entity->physics3d.attached) return NYA_PHYSICS_LAYER_NONE;

    return entity->physics3d.layers;
}

NYA_PhysicsLayerMask nya_physics3d_collides_with(const NYA_Entity* entity) {
    if (entity == nullptr || !entity->physics3d.attached) return NYA_PHYSICS_LAYER_NONE;

    return entity->physics3d.collides_with;
}

/*
 * ─────────────────────────────────────────────────────────
 * HITS
 * ─────────────────────────────────────────────────────────
 */

const NYA_PhysicsHit* nya_physics3d_hits(OUT u32* out_count) {
    nya_assert(out_count != nullptr);

    NYA_Physics3DSystem* system = &nya_world()->physics3d_system;

    *out_count = system->hit_count;

    return system->hits;
}

void nya_physics3d_hit_threshold_set(f32 world_units_per_second) {
    NYA_Physics3DSystem* system = &nya_world()->physics3d_system;

    system->hit_threshold = world_units_per_second;

    if (system->initialized) b3World_SetHitEventThreshold(system->world, _nya_physics3d_scalar_to_meters(world_units_per_second));
}

f32 nya_physics3d_hit_threshold(void) {
    return nya_world()->physics3d_system.hit_threshold;
}

/*
 * ─────────────────────────────────────────────────────────
 * QUERIES
 * ─────────────────────────────────────────────────────────
 */

NYA_EntityHandle nya_physics3d_raycast(f32x3 origin, f32x3 direction, OUT f32x3* out_point, OUT f32x3* out_normal) __attr_overloaded {
    return nya_physics3d_raycast(origin, direction, NYA_PHYSICS_LAYER_ALL, out_point, out_normal);
}

NYA_EntityHandle nya_physics3d_raycast(f32x3 origin, f32x3 direction, NYA_PhysicsLayerMask layers, OUT f32x3* out_point, OUT f32x3* out_normal)
    __attr_overloaded {
    NYA_Physics3DSystem* system = &nya_world()->physics3d_system;
    if (!system->initialized) return NYA_ENTITY_HANDLE_NONE;

    // the ray is in every layer and meets the ones asked for, so a body's own mask cannot hide it from
    // a query that named its layer.
    b3QueryFilter filter = b3DefaultQueryFilter();

    filter.categoryBits = NYA_PHYSICS_LAYER_ALL;
    filter.maskBits     = layers;

    b3RayResult result = b3World_CastRayClosest(
        system->world,
        b3ToPos(_nya_physics3d_to_meters(origin)),
        _nya_physics3d_to_meters(direction),
        filter
    );

    // a zero fraction with no shape means no hit. b3Shape_IsValid is the documented check.
    if (!b3Shape_IsValid(result.shapeId)) return NYA_ENTITY_HANDLE_NONE;

    NYA_Entity* entity = b3Body_GetUserData(b3Shape_GetBody(result.shapeId));
    if (entity == nullptr) return NYA_ENTITY_HANDLE_NONE;

    if (out_point != nullptr) *out_point = _nya_physics3d_to_world(b3ToVec3(result.point));

    // the normal is unit length and dimensionless, so it is not scaled.
    if (out_normal != nullptr) *out_normal = (f32x3){ result.normal.x, result.normal.y, result.normal.z };

    return entity->handle;
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

b3Vec3 _nya_physics3d_to_meters(f32x3 world) {
    f32 scale = nya_world()->physics3d_system.units_per_meter;

    return (b3Vec3){ world.x / scale, world.y / scale, world.z / scale };
}

f32 _nya_physics3d_scalar_to_meters(f32 world) {
    return world / nya_world()->physics3d_system.units_per_meter;
}

f32x3 _nya_physics3d_to_world(b3Vec3 meters) {
    f32 scale = nya_world()->physics3d_system.units_per_meter;

    return (f32x3){ meters.x * scale, meters.y * scale, meters.z * scale };
}

b3Quat _nya_physics3d_to_b3_quat(NYA_Quaternion rotation) {
    // Box3D keeps the vector part in its own b3Vec3; NYA_Quaternion is flat xyzw. Same numbers and
    // handedness.
    return (b3Quat){ .v = { rotation.x, rotation.y, rotation.z }, .s = rotation.w };
}

NYA_Quaternion _nya_physics3d_from_b3_quat(b3Quat rotation) {
    return (NYA_Quaternion){ .x = rotation.v.x, .y = rotation.v.y, .z = rotation.v.z, .w = rotation.s };
}

NYA_Physics3DBody* _nya_physics3d_body_of(const NYA_Entity* entity, NYA_ConstCString operation) {
    if (entity == nullptr) {
        nya_log_error("Cannot %s a null entity.", operation);
        return nullptr;
    }

    if (!entity->physics3d.attached) {
        nya_log_error("Cannot %s entity '%s': it has no 3D physics body.", operation, entity->name ? entity->name : "(unnamed)");
        return nullptr;
    }

    // reading body state should not need a mutable entity, and Box3D takes the id by value.
    return (NYA_Physics3DBody*)&entity->physics3d;
}

b8 _nya_physics3d_shape_create(b3BodyId body, const NYA_Entity* entity, const NYA_Physics3DBodyOptions* options,
                               OUT b3MeshData** out_mesh, OUT b3HeightFieldData** out_height_field) {
    NYA_ConstCString name = entity->name ? entity->name : "(unnamed)";

    *out_mesh         = nullptr;
    *out_height_field = nullptr;

    b3ShapeDef shape_def = b3DefaultShapeDef();

    shape_def.density                    = options->density;
    shape_def.baseMaterial.friction      = options->friction;
    shape_def.baseMaterial.restitution   = options->restitution;
    shape_def.isSensor                   = options->is_sensor;
    shape_def.enableContactEvents        = true;

    // the whole point of layers: Box3D rejects the pair in the broadphase, so a filtered pair never
    // reaches the narrowphase, the hit list or on_collision.
    shape_def.filter.categoryBits = options->layers;
    shape_def.filter.maskBits     = options->collides_with;

    // on every shape: Box3D needs it on both sides of a pair and defaults it off.
    shape_def.enableSensorEvents = true;

    switch (options->shape) {
        case NYA_PHYSICS3D_SHAPE_BOX: {
            if (options->size.x <= 0.0F || options->size.y <= 0.0F || options->size.z <= 0.0F) {
                nya_log_error("Entity '%s' asked for a 3D box body with a non-positive extent.", name);
                return false;
            }

            // `size` is the full extent, as the renderer takes it, so halve it here.
            b3BoxHull box = b3MakeBoxHull(
                _nya_physics3d_scalar_to_meters(options->size.x * 0.5F),
                _nya_physics3d_scalar_to_meters(options->size.y * 0.5F),
                _nya_physics3d_scalar_to_meters(options->size.z * 0.5F)
            );

            // b3BoxHull's arrays hang off `base` by offset. The shape copies it, so a stack temporary is fine.
            (void)b3CreateHullShape(body, &shape_def, &box.base);
            return true;
        }

        case NYA_PHYSICS3D_SHAPE_SPHERE: {
            if (options->radius <= 0.0F) {
                nya_log_error("Entity '%s' asked for a sphere body of radius %.3f; it must be positive.", name, (f64)options->radius);
                return false;
            }

            b3Sphere sphere = { .center = { 0.0F, 0.0F, 0.0F }, .radius = _nya_physics3d_scalar_to_meters(options->radius) };
            (void)b3CreateSphereShape(body, &shape_def, &sphere);
            return true;
        }

        case NYA_PHYSICS3D_SHAPE_CAPSULE: {
            if (options->radius <= 0.0F || options->length <= 0.0F) {
                nya_log_error("Entity '%s' asked for a capsule body with radius %.3f and length %.3f; both must be positive.", name,
                              (f64)options->radius, (f64)options->length);
                return false;
            }

            // upright about y, because a capsule is nearly always a character and y is up in 3D.
            f32       half    = _nya_physics3d_scalar_to_meters(options->length * 0.5F);
            b3Capsule capsule = {
                .center1 = { 0.0F, -half, 0.0F },
                .center2 = { 0.0F, half, 0.0F },
                .radius  = _nya_physics3d_scalar_to_meters(options->radius),
            };

            (void)b3CreateCapsuleShape(body, &shape_def, &capsule);
            return true;
        }

        case NYA_PHYSICS3D_SHAPE_MESH: {
            if (options->vertices == nullptr || options->indices == nullptr || options->vertex_count < 3 || options->index_count < 3) {
                nya_log_error("Entity '%s' asked for a 3D mesh body with %u vertices and %u indices; it needs at least three of each.",
                              name, options->vertex_count, options->index_count);
                return false;
            }

            if (options->index_count % 3 != 0) {
                nya_log_error("Entity '%s' asked for a 3D mesh body with %u indices, which is not a whole number of triangles.", name,
                              options->index_count);
                return false;
            }

            /*
             * Rejected rather than quietly made static.
             */
            if (options->type != NYA_PHYSICS_BODY_STATIC) {
                nya_log_error("Entity '%s' asked for a 3D mesh body that is not static; a triangle mesh has no volume to give it mass.",
                              name);
                return false;
            }

            /*
             * Converted into a scratch array rather than passed straight through.
             */
            u64 point_bytes = (u64)options->vertex_count * sizeof(b3Vec3);
            u64 index_bytes = (u64)options->index_count * sizeof(s32);

            b3Vec3* points = nya_arena_alloc(nya_arena_temp, point_bytes);

            for (u32 i = 0; i < options->vertex_count; i++) points[i] = _nya_physics3d_to_meters(options->vertices[i]);

            // Box3D indexes with int32_t, the engine with u32. Copied so an overflowing index is not
            // silently reinterpreted.
            s32* indices = nya_arena_alloc(nya_arena_temp, index_bytes);

            for (u32 i = 0; i < options->index_count; i++) indices[i] = (s32)options->indices[i];

            b3MeshDef mesh_def = {
                .vertices      = points,
                .indices       = indices,
                .vertexCount   = (int)options->vertex_count,
                .triangleCount = (int)(options->index_count / 3),

                // median split builds a grid mesh BVH much faster than SAH with no measurable query cost.
                .useMedianSplit = true,
            };

            /*
             * Degenerate triangles reported rather than collected.
             */
            s32 degenerate[8]  = { 0 };
            s32 degenerate_max = (s32)(sizeof(degenerate) / sizeof(degenerate[0]));

            b3MeshData* mesh = b3CreateMesh(&mesh_def, degenerate, degenerate_max);

            // reverse order, so a bump allocator reclaims both.
            nya_arena_free(nya_arena_temp, indices, index_bytes);
            nya_arena_free(nya_arena_temp, points, point_bytes);

            if (mesh == nullptr) {
                nya_log_error("Entity '%s' asked for a 3D mesh body of %u triangles that Box3D would not build.", name,
                              options->index_count / 3);
                return false;
            }

            // unit scale: the vertices are already converted.
            (void)b3CreateMeshShape(body, &shape_def, mesh, (b3Vec3){ 1.0F, 1.0F, 1.0F });

            *out_mesh = mesh;
            return true;
        }

        case NYA_PHYSICS3D_SHAPE_HEIGHTFIELD: {
            if (options->heights == nullptr || options->height_count_x < 2 || options->height_count_z < 2) {
                nya_log_error("Entity '%s' asked for a 3D heightfield of %ux%u; it needs at least two grid points on each axis.",
                              name, options->height_count_x, options->height_count_z);
                return false;
            }

            if (options->height_cell_size.x <= 0.0F || options->height_cell_size.y <= 0.0F) {
                nya_log_error("Entity '%s' asked for a 3D heightfield with a cell size of %fx%f; both must be positive.", name,
                              (f64)options->height_cell_size.x, (f64)options->height_cell_size.y);
                return false;
            }

            // a heightfield is a surface with no volume to give a body mass.
            if (options->type != NYA_PHYSICS_BODY_STATIC) {
                nya_log_error("Entity '%s' asked for a 3D heightfield body that is not static; a surface has no volume.", name);
                return false;
            }

            u32 point_count = options->height_count_x * options->height_count_z;

            /* Engine heights are world units, Box3D wants metres. */
            u64  height_bytes = (u64)point_count * sizeof(f32);
            f32* heights      = nya_arena_alloc(nya_arena_temp, height_bytes);

            f32 lowest  = _nya_physics3d_scalar_to_meters(options->heights[0]);
            f32 highest = lowest;

            for (u32 i = 0; i < point_count; i++) {
                heights[i] = _nya_physics3d_scalar_to_meters(options->heights[i]);

                lowest  = nya_min(lowest, heights[i]);
                highest = nya_max(highest, heights[i]);
            }

            /*
             * The quantisation range, from the data.
             *
             * Box3D stores heights as uint16_t in this range, so a wider range wastes precision and a narrower
             * one clamps geometry. A level surface is widened slightly to avoid dividing by zero.
             */
            if (highest - lowest < 1e-4F) highest = lowest + 1e-4F;

            b3HeightFieldDef height_def = {
                .heights = heights,
                .scale   = { _nya_physics3d_scalar_to_meters(options->height_cell_size.x), 1.0F,
                             _nya_physics3d_scalar_to_meters(options->height_cell_size.y) },

                .countX = (int)options->height_count_x,
                .countZ = (int)options->height_count_z,

                .globalMinimumHeight = lowest,
                .globalMaximumHeight = highest,
            };

            b3HeightFieldData* height_field = b3CreateHeightField(&height_def);

            nya_arena_free(nya_arena_temp, heights, height_bytes);

            if (height_field == nullptr) {
                nya_log_error("Entity '%s' asked for a %ux%u 3D heightfield that Box3D would not build.", name,
                              options->height_count_x, options->height_count_z);
                return false;
            }

            (void)b3CreateHeightFieldShape(body, &shape_def, height_field);

            *out_height_field = height_field;
            return true;
        }

        case NYA_PHYSICS3D_SHAPE_COUNT:
        default: {
            nya_log_error("Entity '%s' asked for 3D physics shape %d, which is not a shape.", name, (s32)options->shape);
            return false;
        }
    }
}

void _nya_physics3d_collect_hits(NYA_Physics3DSystem* system) {
    b3ContactEvents events = b3World_GetContactEvents(system->world);

    u32 available = (u32)nya_max(events.hitCount, 0);
    u32 kept      = nya_min(available, (u32)NYA_PHYSICS3D_MAX_HITS);

    for (u32 i = 0; i < kept; i++) {
        const b3ContactHitEvent* event = &events.hitEvents[i];

        NYA_Entity* a = b3Body_GetUserData(b3Shape_GetBody(event->shapeIdA));
        NYA_Entity* b = b3Body_GetUserData(b3Shape_GetBody(event->shapeIdB));

        system->hits[i] = (NYA_PhysicsHit){
            .dimension = NYA_PHYSICS_3D,
            .kind      = NYA_PHYSICS_HIT_IMPACT,

            .a = a != nullptr ? a->handle : NYA_ENTITY_HANDLE_NONE,
            .b = b != nullptr ? b->handle : NYA_ENTITY_HANDLE_NONE,

            .point  = _nya_physics3d_to_world(b3ToVec3(event->point)),
            .normal = { event->normal.x, event->normal.y, event->normal.z },

            .approach_speed = event->approachSpeed * system->units_per_meter,
        };
    }

    system->hit_count = kept;

    if (available > kept) {
        nya_log_warn("Physics3D produced %u hits this step, past the %d that fit; %u were dropped.", available, NYA_PHYSICS3D_MAX_HITS, available - kept);
    }

    _nya_physics3d_collect_sensor_events(system);
}

void _nya_physics3d_collect_sensor_events(NYA_Physics3DSystem* system) {
    b3SensorEvents events = b3World_GetSensorEvents(system->world);

    u32 begin_count = (u32)nya_max(events.beginCount, 0);
    u32 end_count   = (u32)nya_max(events.endCount, 0);

    u32 dropped = 0;

    for (u32 i = 0; i < begin_count; i++) {
        if (system->hit_count >= NYA_PHYSICS3D_MAX_HITS) {
            dropped += begin_count - i;
            break;
        }

        const b3SensorBeginTouchEvent* event = &events.beginEvents[i];

        (void)_nya_physics3d_sensor_hit_write(system, NYA_PHYSICS_HIT_SENSOR_ENTER, event->sensorShapeId, event->visitorShapeId);
    }

    for (u32 i = 0; i < end_count; i++) {
        if (system->hit_count >= NYA_PHYSICS3D_MAX_HITS) {
            dropped += end_count - i;
            break;
        }

        const b3SensorEndTouchEvent* event = &events.endEvents[i];

        // either shape may already be destroyed when the exit comes from a despawn.
        if (!b3Shape_IsValid(event->sensorShapeId)) continue;
        if (!b3Shape_IsValid(event->visitorShapeId)) continue;

        (void)_nya_physics3d_sensor_hit_write(system, NYA_PHYSICS_HIT_SENSOR_EXIT, event->sensorShapeId, event->visitorShapeId);
    }

    if (dropped > 0) {
        nya_log_warn("Physics3D produced %u sensor events this step and the hit list was already full; %u were dropped.", begin_count + end_count,
                 dropped);
    }
}

b8 _nya_physics3d_sensor_hit_write(NYA_Physics3DSystem* system, NYA_PhysicsHitKind kind, b3ShapeId sensor_shape, b3ShapeId visitor_shape) {
    NYA_Entity* sensor  = b3Body_GetUserData(b3Shape_GetBody(sensor_shape));
    NYA_Entity* visitor = b3Body_GetUserData(b3Shape_GetBody(visitor_shape));

    if (sensor == nullptr && visitor == nullptr) return false;

    // a sensor overlap reports no geometry, so use the midpoint of the two bodies.
    f32x3 sensor_position  = sensor != nullptr ? sensor->position : f32x3_zero;
    f32x3 visitor_position = visitor != nullptr ? visitor->position : f32x3_zero;

    f32x3 point = sensor != nullptr && visitor != nullptr ? (sensor_position + visitor_position) * 0.5F
                                                          : (sensor != nullptr ? sensor_position : visitor_position);

    system->hits[system->hit_count++] = (NYA_PhysicsHit){
        .dimension = NYA_PHYSICS_3D,
        .kind      = kind,

        // sensor first: a pickup callback reads `entity` as itself and `other` as the visitor.
        .a = sensor != nullptr ? sensor->handle : NYA_ENTITY_HANDLE_NONE,
        .b = visitor != nullptr ? visitor->handle : NYA_ENTITY_HANDLE_NONE,

        .point          = point,
        .normal         = f32x3_zero,
        .approach_speed = 0.0F,
    };

    return true;
}

void _nya_physics3d_dispatch_collisions(const NYA_Physics3DSystem* system) {
    for (u32 i = 0; i < system->hit_count; i++) {
        const NYA_PhysicsHit* hit = &system->hits[i];

        // both sides, each resolved right before its call, since a callback may despawn either.
        for (u32 side = 0; side < 2; side++) {
            NYA_EntityHandle self_handle  = side == 0 ? hit->a : hit->b;
            NYA_EntityHandle other_handle = side == 0 ? hit->b : hit->a;

            NYA_Entity* self = nya_entity_get(self_handle);
            if (self == nullptr) continue;

            NYA_EntityOnCollisionFn on_collision = nya_callback_get(self->on_collision);
            if (on_collision == nullptr) continue;

            on_collision(self, nya_entity_get(other_handle), hit);
        }
    }
}
