#include "nyangine/nyangine.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API DECLARATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/** A uniform sample in [0, 1). The one primitive everything else here is built from. */
NYA_INTERNAL f32 _nya_particles_unit(NYA_ParticleSystem* system);

/** A uniform sample in `[range.x, range.y]`, or `fallback` when the range is empty. */
NYA_INTERNAL f32 _nya_particles_range(NYA_ParticleSystem* system, f32x2 range, f32x2 fallback);

/** A uniformly distributed point on the unit sphere. */
NYA_INTERNAL f32x3 _nya_particles_direction(NYA_ParticleSystem* system);

/** A direction within `spread` radians of `axis`. */
NYA_INTERNAL f32x3 _nya_particles_cone(NYA_ParticleSystem* system, f32x3 axis, f32 spread);

/** Retires the particle in `index`, moving the last live one into its slot. */
NYA_INTERNAL void _nya_particles_kill(NYA_ParticleSystem* system, u32 index);

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PUBLIC API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

NYA_ParticleSystem* nya_particles_create(NYA_Arena* arena, u32 capacity) {
    nya_assert(arena != nullptr);
    nya_assert(capacity > 0, "a particle system needs room for at least one particle");

    NYA_ParticleSystem* system = nya_arena_alloc(arena, sizeof(NYA_ParticleSystem));

    *system = (NYA_ParticleSystem){
        .allocator = arena,
        .space     = NYA_PARTICLE_SPACE_2D,
        .capacity  = capacity,
        .particles = nya_arena_alloc(arena, capacity * sizeof(NYA_Particle)),
        /* A fixed seed, so runs produce the same sparks unless asked otherwise. See nya_particles_seed. */
        .rng = nya_rng_create_in(arena, "0FEEDFACE0C0FFEE"),
    };

    return system;
}

void nya_particles_space_set(NYA_ParticleSystem* system, NYA_ParticleSpace space) {
    nya_assert(system != nullptr);

    system->space = space;
}

void nya_particles_casts_shadow_set(NYA_ParticleSystem* system, b8 casts_shadow) {
    nya_assert(system != nullptr);

    system->casts_shadow = casts_shadow;
}

void nya_particles_texture_set(NYA_ParticleSystem* system, NYA_ConstCString texture) {
    nya_assert(system != nullptr);

    system->texture = texture;
}

void nya_particles_on_update_set(NYA_ParticleSystem* system, NYA_ParticleUpdateFn on_update, void* user_data) {
    nya_assert(system != nullptr);

    system->on_update           = on_update;
    system->on_update_user_data = user_data;
}

void nya_particles_seed(NYA_ParticleSystem* system, u64 seed) {
    nya_assert(system != nullptr);

    // through the hex spelling nya_rng_create_in takes, so there is one seeding path.
    char text[32];
    (void)snprintf(text, sizeof(text), "%016llX", (unsigned long long)seed);

    system->rng = nya_rng_create_in(system->allocator, text);
}

u32 nya_particles_emit(NYA_ParticleSystem* system, NYA_ParticleBurst burst) {
    nya_assert(system != nullptr);

    if (burst.count == 0) return 0;

    u32 room  = system->capacity - system->count;
    u32 spawn = nya_min(burst.count, room);

    // counted, not raised: losing a few particles under load is correct behaviour.
    if (spawn < burst.count) system->dropped += burst.count - spawn;

    // zero means unset for all of these.
    f32x3 axis = burst.direction;
    if (axis.x == 0.0F && axis.y == 0.0F && axis.z == 0.0F) {
        // up in the space's sense: negative y on a y-down screen, positive in 3D.
        axis = system->space == NYA_PARTICLE_SPACE_3D ? (f32x3){ 0.0F, 1.0F, 0.0F } : (f32x3){ 0.0F, -1.0F, 0.0F };
    }

    axis = nya_vector_normalize(axis);

    NYA_Color color_start = burst.color_start;
    if (color_start.r == 0.0F && color_start.g == 0.0F && color_start.b == 0.0F && color_start.a == 0.0F) color_start = NYA_COLOR_WHITE;

    for (u32 i = 0; i < spawn; i++) {
        NYA_Particle* particle = &system->particles[system->count + i];

        f32x3 offset    = f32x3_zero;
        f32x3 direction = axis;

        switch (burst.shape) {
            case NYA_PARTICLE_SHAPE_SPHERE: {
                // direction first, offset along it, so particles near the rim already move outward and it reads as an
                // explosion.
                direction = _nya_particles_direction(system);

                // cube rooted, so points are uniform through the volume instead of crowding the centre.
                offset = direction * (burst.radius * cbrtf(_nya_particles_unit(system)));
            } break;

            case NYA_PARTICLE_SHAPE_CONE: {
                direction = _nya_particles_cone(system, axis, burst.spread);
            } break;

            case NYA_PARTICLE_SHAPE_BOX: {
                offset = (f32x3){
                    (_nya_particles_unit(system) - 0.5F) * burst.volume.x,
                    (_nya_particles_unit(system) - 0.5F) * burst.volume.y,
                    (_nya_particles_unit(system) - 0.5F) * burst.volume.z,
                };
            } break;

            case NYA_PARTICLE_SHAPE_POINT:
            case NYA_PARTICLE_SHAPE_COUNT:
            default: {
                direction = _nya_particles_direction(system);
            } break;
        }

        // flattened in 2D, so a sphere becomes a disc; otherwise most particles move in an undrawn z.
        if (system->space == NYA_PARTICLE_SPACE_2D) {
            direction.z = 0.0F;
            offset.z    = 0.0F;

            direction = nya_vector_normalize(direction);
        }

        f32 speed      = _nya_particles_range(system, burst.speed, (f32x2){ 50.0F, 100.0F });
        f32 size_start = _nya_particles_range(system, burst.size, (f32x2){ 2.0F, 4.0F });

        *particle = (NYA_Particle){
            .position     = burst.position + offset,
            .velocity     = direction * speed,
            .acceleration = burst.gravity,
            .lifetime_s   = _nya_particles_range(system, burst.lifetime_s, (f32x2){ 0.5F, 1.0F }),
            .size_start   = size_start,
            // defaults to the start size, so particles do not shrink to nothing.
            .size_end         = burst.size_end.y > 0.0F ? _nya_particles_range(system, burst.size_end, burst.size_end) : size_start,
            .color_start      = color_start,
            .color_end        = burst.color_end,
            .rotation         = _nya_particles_range(system, burst.rotation, f32x2_zero),
            .angular_velocity = _nya_particles_range(system, burst.angular_velocity, f32x2_zero),
            .damping          = burst.damping,
            .user_id          = burst.user_id,
        };
    }

    system->count += spawn;

    return spawn;
}

void nya_particles_update(NYA_ParticleSystem* system, f32 delta_time_s) {
    nya_perf_time_this_function();

    nya_assert(system != nullptr);

    if (delta_time_s <= 0.0F) return;

    system->dropped = 0;

    for (u32 i = 0; i < system->count;) {
        NYA_Particle* particle = &system->particles[i];

        particle->age_s += delta_time_s;

        if (particle->age_s >= particle->lifetime_s) {
            // `i` stays: the kill moved an un-updated particle into this slot.
            _nya_particles_kill(system, i);
            continue;
        }

        particle->velocity += particle->acceleration * delta_time_s;

        /* Damping as an exponential, not a subtraction, so it is frame-rate independent and never overshoots zero. */
        if (particle->damping > 0.0F) particle->velocity *= expf(-particle->damping * delta_time_s);

        particle->position += particle->velocity * delta_time_s;
        particle->rotation += particle->angular_velocity * delta_time_s;

        if (system->on_update != nullptr) {
            system->on_update(particle, particle->age_s / particle->lifetime_s, delta_time_s, system->on_update_user_data);

            // a callback ends a particle by ageing it past its lifetime; checked now, so it is not drawn another frame.
            if (particle->age_s >= particle->lifetime_s) {
                _nya_particles_kill(system, i);
                continue;
            }
        }

        i++;
    }
}

void nya_particles_draw(NYA_Window* window, const NYA_ParticleSystem* system) {
    nya_perf_time_this_function();

    nya_assert(window != nullptr);

    if (system == nullptr || system->count == 0) return;

    // no projection, nothing to draw. same rule as NYA_ENTITY_VISUAL_CUBE.
    if (system->space == NYA_PARTICLE_SPACE_3D && !nya_render3d_active(window)) return;

    /*
     * Systems that opt out stay out of the shadow cascades: they have no alpha, so a translucent billboard would cast
     * a solid square. See NYA_ParticleSystem.casts_shadow.
     */
    if (system->space == NYA_PARTICLE_SPACE_3D) nya_render3d_shadow_cast_set(window, system->casts_shadow);

    /* The system's texture, resolved once. */
    NYA_Render3DTextureBinding texture = system->space == NYA_PARTICLE_SPACE_3D ? nya_render3d_texture_resolve(system->texture)
                                                                               : (NYA_Render3DTextureBinding){ 0 };

    for (u32 i = 0; i < system->count; i++) {
        const NYA_Particle* particle = &system->particles[i];

        f32 t = particle->lifetime_s > 0.0F ? particle->age_s / particle->lifetime_s : 1.0F;

        f32 size = nya_lerp(particle->size_start, particle->size_end, t);
        if (size <= 0.0F) continue;

        NYA_Color color = {
            nya_lerp(particle->color_start.r, particle->color_end.r, t),
            nya_lerp(particle->color_start.g, particle->color_end.g, t),
            nya_lerp(particle->color_start.b, particle->color_end.b, t),
            nya_lerp(particle->color_start.a, particle->color_end.a, t),
        };

        if (system->space == NYA_PARTICLE_SPACE_3D) {
            /* A billboard. */
            /* With the system's texture. */
            nya_render3d_billboard_resolved(window, texture, particle->position, (f32x2){ size, size }, particle->rotation, color);
            continue;
        }

        f32x2 center = { particle->position.x, particle->position.y };

        if (system->texture != nullptr) {
            nya_render2d_texture_ex(
                window, system->texture,
                (NYA_Render2DTexture){
                    .x        = center.x,
                    .y        = center.y,
                    .width    = size,
                    .height   = size,
                    .rotation = particle->rotation,
                    // half, so the quad spins about its middle.
                    .origin = { 0.5F, 0.5F },
                    .tint   = color,
                }
            );

            continue;
        }

        nya_render2d_rect_rotated(window, center, (f32x2){ size, size }, particle->rotation, color);
    }

    // back on, so what the scene draws next casts as it would have.
    if (system->space == NYA_PARTICLE_SPACE_3D) nya_render3d_shadow_cast_set(window, true);
}

void nya_particles_clear(NYA_ParticleSystem* system) {
    nya_assert(system != nullptr);

    // just the count; nothing reads past it.
    system->count   = 0;
    system->dropped = 0;
}

u32 nya_particles_count(const NYA_ParticleSystem* system) {
    return system != nullptr ? system->count : 0;
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

f32 _nya_particles_unit(NYA_ParticleSystem* system) {
    return nya_rng_sample_f32(
        system->rng, (NYA_RNGDistribution){ .type = NYA_RNG_DISTRIBUTION_UNIFORM, .uniform = { .min = 0.0, .max = 1.0 } }
    );
}

f32 _nya_particles_range(NYA_ParticleSystem* system, f32x2 range, f32x2 fallback) {
    // the maximum decides whether a range is set: { 0, 0.6 } means up to 0.6.
    f32x2 chosen = range.y > 0.0F ? range : fallback;

    if (chosen.y <= chosen.x) return chosen.x;

    return chosen.x + (_nya_particles_unit(system) * (chosen.y - chosen.x));
}

f32x3 _nya_particles_direction(NYA_ParticleSystem* system) {
    /* Uniform on the sphere, which three uniform components are not. */
    f32 z         = (_nya_particles_unit(system) * 2.0F) - 1.0F;
    f32 azimuth   = _nya_particles_unit(system) * 2.0F * (f32)M_PI;
    f32 planar    = sqrtf(nya_max(1.0F - (z * z), 0.0F));

    return (f32x3){ planar * cosf(azimuth), planar * sinf(azimuth), z };
}

f32x3 _nya_particles_cone(NYA_ParticleSystem* system, f32x3 axis, f32 spread) {
    if (spread <= 0.0F) return axis;

    // uniform over the cap, not the angle, which would crowd the axis.
    f32 cosine  = 1.0F - (_nya_particles_unit(system) * (1.0F - cosf(spread)));
    f32 sine    = sqrtf(nya_max(1.0F - (cosine * cosine), 0.0F));
    f32 azimuth = _nya_particles_unit(system) * 2.0F * (f32)M_PI;

    // a basis around the axis, crossed against the least aligned world axis. see nya_render3d_line.
    f32x3 reference = fabsf(axis.y) < 0.9F ? (f32x3){ 0.0F, 1.0F, 0.0F } : (f32x3){ 1.0F, 0.0F, 0.0F };

    f32x3 right = nya_vector_normalize(nya_vector_cross(axis, reference));
    f32x3 up    = nya_vector_cross(axis, right);

    return nya_vector_normalize((axis * cosine) + (right * (sine * cosf(azimuth))) + (up * (sine * sinf(azimuth))));
}

void _nya_particles_kill(NYA_ParticleSystem* system, u32 index) {
    system->count--;

    // swap with the last live particle, so the pool stays packed in constant time. particles have no identity, so
    // nothing holds a pointer to one.
    if (index != system->count) system->particles[index] = system->particles[system->count];
}
