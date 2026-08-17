#include "nyangine/nyangine.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API DECLARATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/** World units to metres, and back. The whole of the unit boundary described in physics3d.h. */
NYA_INTERNAL b3Vec3 _nya_physics3d_to_meters(f32x3 world);
NYA_INTERNAL f32    _nya_physics3d_scalar_to_meters(f32 world);
NYA_INTERNAL f32x3  _nya_physics3d_to_world(b3Vec3 meters);

/** Between the engine's quaternion convention and Box3D's, which stores the vector part separately. */
NYA_INTERNAL b3Quat         _nya_physics3d_to_b3_quat(NYA_Quaternion rotation);
NYA_INTERNAL NYA_Quaternion _nya_physics3d_from_b3_quat(b3Quat rotation);

/** Null, and logged, when the handle does not resolve or the entity carries no 3D body. */
NYA_INTERNAL NYA_Physics3DBody* _nya_physics3d_body_of(const NYA_Entity* entity, NYA_ConstCString operation);

/** Builds and attaches the shape described by `options`. False when the dimensions are nonsense. */
NYA_INTERNAL b8 _nya_physics3d_shape_create(b3BodyId body, const NYA_Entity* entity, const NYA_Physics3DBodyOptions* options,
                                            OUT b3MeshData** out_mesh);

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

    *system = (NYA_Physics3DSystem){
        .initialized     = true,
        .enabled         = true,
        .units_per_meter = NYA_PHYSICS3D_UNITS_PER_METER,
        .gravity         = NYA_PHYSICS3D_GRAVITY_DEFAULT,
        .sub_step_count  = NYA_PHYSICS3D_SUB_STEPS,
        .hit_threshold   = NYA_PHYSICS3D_HIT_THRESHOLD,
    };

    b3WorldDef world_def = b3DefaultWorldDef();
    world_def.gravity    = _nya_physics3d_to_meters(system->gravity);

    world_def.hitEventThreshold = _nya_physics3d_scalar_to_meters(system->hit_threshold);

    // Single threaded, for the same reason the 2D world is: the dispatch cost is real and a scene of
    // a few hundred bodies spends more time handing islands out than solving them. See physics2d.c.
    world_def.workerCount = 1;

    system->world = b3CreateWorld(&world_def);

    nya_info("Physics3D system initialized (%.1f world units per metre, %u sub steps).", (f64)system->units_per_meter, system->sub_step_count);
}

void nya_system_physics3d_deinit(void) {
    NYA_Physics3DSystem* system = &nya_world()->physics3d_system;
    if (!system->initialized) return;

    // Cleared first, so entity teardown afterwards does not hand ids back to a world that has already
    // freed them. The same ordering trap the 2D system documents.
    system->initialized = false;
    b3DestroyWorld(system->world);

    *system = (NYA_Physics3DSystem){ 0 };

    nya_info("Physics3D system deinitialized.");
}

void nya_system_physics3d_update(f32 delta_time_s) {
    nya_perf_time_this_function();

    NYA_Physics3DSystem* system = &nya_world()->physics3d_system;

    system->hit_count = 0;

    if (!system->initialized || !system->enabled) return;
    if (delta_time_s <= 0.0F) return;

    /*
     * Cheap when nothing is in it, which is the common case.
     *
     * Both worlds step every tick and most games use one. Stepping an empty solver is a handful of
     * nanoseconds — but it is not free, and skipping it here keeps a purely 2D game from paying for
     * a 3D world it never touched.
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

        // The whole quaternion, unlike the 2D world's single roll. A 3D body has three angular
        // degrees of freedom and there is no axis to pick one out of.
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

    // Both of these were converted through the old scale and mean a *speed* and an *acceleration* in
    // world units, so they have to be pushed through the new one or the world silently changes how
    // hard it pulls.
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

    // Seeded from the entity, so spawning something with an initial throw is one spawn and one
    // attach rather than a third call afterwards. Same contract as the 2D attach.
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

    b3MeshData* mesh = nullptr;

    if (!_nya_physics3d_shape_create(body, entity, &options, &mesh)) {
        // The body exists and has no shape, which is a body that falls through the world forever.
        // Destroyed rather than left behind, so a rejected attach leaves nothing at all.
        b3DestroyBody(body);
        return false;
    }

    b3Body_EnableHitEvents(body, !options.ignore_hits);

    entity->physics3d = (NYA_Physics3DBody){
        .id       = body,
        .type     = options.type,
        .shape    = options.shape,
        .size     = options.size,
        .radius   = options.radius,
        .length   = options.length,
        .mesh     = mesh,
        .attached = true,
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
     *
     * A MESH body owns a b3MeshData that b3DestroyWorld does not free — it was created beside the world
     * rather than inside it. Entity teardown at shutdown runs after nya_system_physics3d_deinit, so
     * returning early on a dead system would leak exactly the largest allocation a body can hold.
     */
    if (entity->physics3d.mesh != nullptr) b3DestroyMesh((b3MeshData*)entity->physics3d.mesh);

    // The body id belongs to a world that no longer exists once the system is down, and handing it back
    // is a use after free rather than a no-op.
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

    // Radians are dimensionless, so unlike a linear velocity this does not cross the unit boundary.
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

    // Mirrored immediately rather than waiting for the next step, so anything reading the transform
    // between now and then sees where the entity actually is. A sleeping body would otherwise never
    // report the move at all, because the readback loop skips it.
    entity->position = position;
    entity->rotation = rotation;
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
         * The manifold normal points from shape A toward shape B, so which of the two we are decides
         * its sign. Standing on something means the *other* shape is below us — and in 3D below is
         * negative y, which is the opposite of the 2D world's y-down screen. Hence the leading minus
         * that the 2D version does not have.
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

NYA_EntityHandle nya_physics3d_raycast(f32x3 origin, f32x3 direction, OUT f32x3* out_point, OUT f32x3* out_normal) {
    NYA_Physics3DSystem* system = &nya_world()->physics3d_system;
    if (!system->initialized) return NYA_ENTITY_HANDLE_NONE;

    b3RayResult result = b3World_CastRayClosest(
        system->world,
        b3ToPos(_nya_physics3d_to_meters(origin)),
        _nya_physics3d_to_meters(direction),
        b3DefaultQueryFilter()
    );

    // A fraction of zero with no shape is how upstream says "nothing", and b3Shape_IsValid is the
    // documented way to ask rather than comparing the id against a sentinel.
    if (!b3Shape_IsValid(result.shapeId)) return NYA_ENTITY_HANDLE_NONE;

    NYA_Entity* entity = b3Body_GetUserData(b3Shape_GetBody(result.shapeId));
    if (entity == nullptr) return NYA_ENTITY_HANDLE_NONE;

    if (out_point != nullptr) *out_point = _nya_physics3d_to_world(b3ToVec3(result.point));

    // The normal is a unit vector and is dimensionless, so it does not cross the unit boundary the
    // point does. Converting it would shrink it by the scale factor and quietly stop it being unit.
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
    // Box3D splits the vector part into its own b3Vec3 where NYA_Quaternion is flat xyzw. Same four
    // numbers, same handedness, different packing — which is the entire content of this conversion.
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

    // Cast away const for the same reason the 2D module does: reading a body's state should not
    // demand a mutable entity, and every Box3D call below needs the id by value anyway.
    return (NYA_Physics3DBody*)&entity->physics3d;
}

b8 _nya_physics3d_shape_create(b3BodyId body, const NYA_Entity* entity, const NYA_Physics3DBodyOptions* options,
                               OUT b3MeshData** out_mesh) {
    NYA_ConstCString name = entity->name ? entity->name : "(unnamed)";

    *out_mesh = nullptr;

    b3ShapeDef shape_def = b3DefaultShapeDef();

    shape_def.density                    = options->density;
    shape_def.baseMaterial.friction      = options->friction;
    shape_def.baseMaterial.restitution   = options->restitution;
    shape_def.isSensor                   = options->is_sensor;
    shape_def.enableContactEvents        = true;

    // On every shape, not just the sensors — Box3D wants it on both sides of a pair and defaults it
    // off on both, which is the same footgun physics2d.c documents at length.
    shape_def.enableSensorEvents = true;

    switch (options->shape) {
        case NYA_PHYSICS3D_SHAPE_BOX: {
            if (options->size.x <= 0.0F || options->size.y <= 0.0F || options->size.z <= 0.0F) {
                nya_log_error("Entity '%s' asked for a 3D box body with a non-positive extent.", name);
                return false;
            }

            // Half extents, because `size` is the full width, height and depth — which is what a
            // renderer takes. Halving here is what keeps a body and the cube drawn for it the same
            // size, the same contract the 2D box has.
            b3BoxHull box = b3MakeBoxHull(
                _nya_physics3d_scalar_to_meters(options->size.x * 0.5F),
                _nya_physics3d_scalar_to_meters(options->size.y * 0.5F),
                _nya_physics3d_scalar_to_meters(options->size.z * 0.5F)
            );

            // `base` is the b3HullData header the rest of b3BoxHull's arrays hang off by offset. The
            // shape copies what it is given, which is why this local can be a stack temporary.
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

            // Upright about y, because in 3D y is up and a capsule is nearly always a character. The
            // 2D capsule is upright about *its* y for the same reason, which happens to be down the
            // screen — the axis is the same field, the convention around it is not.
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
             *
             * A dynamic body with no inertia tensor is not a body the solver can integrate, and Box3D's
             * answer to being given one is undefined rather than an error. Saying so here is the
             * difference between a message naming the entity and a scene where one object behaves
             * strangely.
             */
            if (options->type != NYA_PHYSICS_BODY_STATIC) {
                nya_log_error("Entity '%s' asked for a 3D mesh body that is not static; a triangle mesh has no volume to give it mass.",
                              name);
                return false;
            }

            /*
             * Converted into a scratch array rather than passed straight through.
             *
             * The vertices arrive in world units and Box3D wants metres, and _nya_physics3d_to_meters is
             * the same conversion every other shape here goes through. It also changes the type — f32x3
             * in, b3Vec3 out — so there is no arrangement in which the caller's array could be handed
             * over untouched.
             *
             * The temp arena, because this is a frame's worth of temporary — the mesh Box3D builds below
             * is the copy that lives on.
             */
            u64 point_bytes = (u64)options->vertex_count * sizeof(b3Vec3);
            u64 index_bytes = (u64)options->index_count * sizeof(s32);

            b3Vec3* points = nya_arena_alloc(nya_arena_temp, point_bytes);

            for (u32 i = 0; i < options->vertex_count; i++) points[i] = _nya_physics3d_to_meters(options->vertices[i]);

            // Box3D indexes with int32_t; the engine counts with u32. Copied rather than cast because a
            // reinterpreting cast would be a lie about signedness on the one index that overflows.
            s32* indices = nya_arena_alloc(nya_arena_temp, index_bytes);

            for (u32 i = 0; i < options->index_count; i++) indices[i] = (s32)options->indices[i];

            b3MeshDef mesh_def = {
                .vertices      = points,
                .indices       = indices,
                .vertexCount   = (int)options->vertex_count,
                .triangleCount = (int)(options->index_count / 3),

                // A grid-shaped mesh is the case this exists for, and the median split builds its BVH
                // markedly faster than the SAH does with no measurable difference in query cost.
                .useMedianSplit = true,
            };

            /*
             * Degenerate triangles reported rather than collected.
             *
             * Passing null for the array asks Box3D to skip them silently, which is the wrong default for
             * generated geometry: a heightmap with two equal neighbouring samples produces them, and they
             * are worth a line in the log rather than a shape that is quietly missing triangles.
             */
            s32 degenerate[8]  = { 0 };
            s32 degenerate_max = (s32)(sizeof(degenerate) / sizeof(degenerate[0]));

            b3MeshData* mesh = b3CreateMesh(&mesh_def, degenerate, degenerate_max);

            // Reverse order, so a bump allocator can actually reclaim both rather than only the last one.
            nya_arena_free(nya_arena_temp, indices, index_bytes);
            nya_arena_free(nya_arena_temp, points, point_bytes);

            if (mesh == nullptr) {
                nya_log_error("Entity '%s' asked for a 3D mesh body of %u triangles that Box3D would not build.", name,
                              options->index_count / 3);
                return false;
            }

            // Unit scale: the vertices went in already converted, so scaling here would apply the
            // conversion twice.
            (void)b3CreateMeshShape(body, &shape_def, mesh, (b3Vec3){ 1.0F, 1.0F, 1.0F });

            *out_mesh = mesh;
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
        nya_warn("Physics3D produced %u hits this step, past the %d that fit; %u were dropped.", available, NYA_PHYSICS3D_MAX_HITS, available - kept);
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

        // Either shape may already be destroyed, which is what an exit caused by a despawn looks
        // like. Same guard, same reason, as the 2D collector.
        if (!b3Shape_IsValid(event->sensorShapeId)) continue;
        if (!b3Shape_IsValid(event->visitorShapeId)) continue;

        (void)_nya_physics3d_sensor_hit_write(system, NYA_PHYSICS_HIT_SENSOR_EXIT, event->sensorShapeId, event->visitorShapeId);
    }

    if (dropped > 0) {
        nya_warn("Physics3D produced %u sensor events this step and the hit list was already full; %u were dropped.", begin_count + end_count,
                 dropped);
    }
}

b8 _nya_physics3d_sensor_hit_write(NYA_Physics3DSystem* system, NYA_PhysicsHitKind kind, b3ShapeId sensor_shape, b3ShapeId visitor_shape) {
    NYA_Entity* sensor  = b3Body_GetUserData(b3Shape_GetBody(sensor_shape));
    NYA_Entity* visitor = b3Body_GetUserData(b3Shape_GetBody(visitor_shape));

    if (sensor == nullptr && visitor == nullptr) return false;

    // The midpoint of the two bodies, because a sensor overlap reports no geometry. See the 2D
    // collector for why it is the midpoint and not either body's own position.
    f32x3 sensor_position  = sensor != nullptr ? sensor->position : f32x3_zero;
    f32x3 visitor_position = visitor != nullptr ? visitor->position : f32x3_zero;

    f32x3 point = sensor != nullptr && visitor != nullptr ? (sensor_position + visitor_position) * 0.5F
                                                          : (sensor != nullptr ? sensor_position : visitor_position);

    system->hits[system->hit_count++] = (NYA_PhysicsHit){
        .dimension = NYA_PHYSICS_3D,
        .kind      = kind,

        // Sensor first, visitor second. A pickup's callback reads `entity` as itself and `other` as
        // whatever walked in, and that only holds because the sensor is always A.
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

        // Both sides, each told about the other, and each resolved immediately before it is called —
        // a callback may despawn either. See _nya_physics2d_dispatch_collisions.
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
