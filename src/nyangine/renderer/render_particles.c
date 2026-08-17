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
        /*
         * A fixed seed rather than the clock, so two runs of the same game produce the same sparks
         * unless something asks otherwise. See nya_particles_seed.
         *
         * Uppercase hex because that is the only spelling nya_rng_create_in accepts — it panics on
         * anything else rather than hashing it, so a descriptive seed like "particles" is not an
         * option however much it would read better here.
         */
        .rng = nya_rng_create_in(arena, "0FEEDFACE0C0FFEE"),
    };

    return system;
}

void nya_particles_space_set(NYA_ParticleSystem* system, NYA_ParticleSpace space) {
    nya_assert(system != nullptr);

    system->space = space;
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

    // Through the hex spelling, because that is the seed form nya_rng_create_in takes and going via
    // it means one seeding path rather than two that could diverge.
    char text[32];
    (void)snprintf(text, sizeof(text), "%016llX", (unsigned long long)seed);

    system->rng = nya_rng_create_in(system->allocator, text);
}

u32 nya_particles_emit(NYA_ParticleSystem* system, NYA_ParticleBurst burst) {
    nya_assert(system != nullptr);

    if (burst.count == 0) return 0;

    u32 room  = system->capacity - system->count;
    u32 spawn = nya_min(burst.count, room);

    // Counted rather than raised. An effect that loses a few particles under load is working
    // correctly, and a burst that failed would be an error nobody could act on mid frame.
    if (spawn < burst.count) system->dropped += burst.count - spawn;

    // Zero is a field nobody filled in rather than a value anyone means, for every one of these.
    f32x3 axis = burst.direction;
    if (axis.x == 0.0F && axis.y == 0.0F && axis.z == 0.0F) {
        // Up, in whichever sense the space means it: negative y on a y-down screen, positive y in a
        // 3D scene. The two worlds genuinely disagree about which way up is, so this has to ask.
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
                // The direction is picked first and the offset lies along it, so a particle starting
                // near the rim is already moving outward — which is what makes an explosion read as
                // one rather than as a ball that expands uniformly.
                direction = _nya_particles_direction(system);

                // Cube rooted, so the points are uniform through the volume rather than crowded at
                // the centre, which is what a plain uniform radius gives.
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

        // Flattened for a 2D system, so a sphere becomes a disc and a cone a fan. Without this a
        // point burst on screen loses most of its particles into a z nothing draws.
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
            // Defaults to the start size, so a burst that says nothing about the end does not shrink
            // every particle to nothing.
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
            // Not advancing `i`: the kill moved the last live particle into this slot, and that one
            // has not been updated yet.
            _nya_particles_kill(system, i);
            continue;
        }

        particle->velocity += particle->acceleration * delta_time_s;

        /*
         * Damping as an exponential rather than a subtraction.
         *
         * `v -= v * damping * dt` is the obvious form and reverses direction the moment
         * `damping * dt` exceeds one — so a heavily damped particle at a low frame rate visibly
         * springs backwards. The exponential cannot, whatever the timestep.
         */
        if (particle->damping > 0.0F) particle->velocity *= expf(-particle->damping * delta_time_s);

        particle->position += particle->velocity * delta_time_s;
        particle->rotation += particle->angular_velocity * delta_time_s;

        if (system->on_update != nullptr) {
            system->on_update(particle, particle->age_s / particle->lifetime_s, delta_time_s, system->on_update_user_data);

            // The callback is allowed to end a particle by ageing it past its lifetime, which is the
            // only way it has to say so — checked here rather than next tick, so the particle does
            // not get one more frame of drawing after it asked to stop.
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

    // No projection to draw through, so nothing is drawn. Same rule as NYA_ENTITY_VISUAL_CUBE.
    if (system->space == NYA_PARTICLE_SPACE_3D && !nya_render3d_active(window)) return;

    /*
     * The system's texture resolved once, not once per particle.
     *
     * It was inside the loop, which is a dictionary lookup and a string hash per particle per pass — for
     * a plume of a couple of hundred, drawn once for the camera and once per shadow cascade, that is over
     * a thousand lookups a frame all returning the same answer.
     *
     * Only valid for this call, which is why it is not stored on the system: a hot reload replaces the
     * GPU texture behind the handle, and a cached binding would outlive it.
     */
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
            /*
             * A billboard, which is what a particle is.
             *
             * This drew a small *cube* until there was a billboard primitive to call, and said so: a
             * screen-facing quad needs the camera's right and up vectors to build its corners, and
             * render3d did not expose them. A cube reads correctly from every angle and is the wrong
             * shape for everything a particle system is for — smoke, fire, a spark, a glow are all a flat
             * image that always faces the viewer.
             *
             * The rotation is the particle's own, applied in the view plane. It is what stops a crowd of
             * identical puffs looking stamped from one die, and it is the reason a cube could not simply
             * be made small enough to pass.
             */
            /*
             * The system's texture, which the 3D path used to drop on the floor.
             *
             * nya_particles_texture_set is documented as the texture *every* particle draws with, and this
             * branch ignored it — so a 3D system with a sprite drew flat squares and said nothing. That is
             * the difference between a plume and a stack of quads, since an untextured billboard is a hard
             * edged square by construction.
             */
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
                    // Half, so the quad turns about its middle rather than its corner — a particle
                    // rotating about a corner orbits instead of spinning.
                    .origin = { 0.5F, 0.5F },
                    .tint   = color,
                }
            );

            continue;
        }

        nya_render2d_rect_rotated(window, center, (f32x2){ size, size }, particle->rotation, color);
    }
}

void nya_particles_clear(NYA_ParticleSystem* system) {
    nya_assert(system != nullptr);

    // Just the count. The pool keeps whatever was in it, and nothing reads past `count`.
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
    // The maximum is what decides whether a range was filled in, not the minimum — a range of
    // { 0, 0.6 } is a perfectly ordinary "up to 0.6 seconds" and must not be read as absent.
    f32x2 chosen = range.y > 0.0F ? range : fallback;

    if (chosen.y <= chosen.x) return chosen.x;

    return chosen.x + (_nya_particles_unit(system) * (chosen.y - chosen.x));
}

f32x3 _nya_particles_direction(NYA_ParticleSystem* system) {
    /*
     * Uniform on the sphere, which is not what picking three uniform components gives.
     *
     * That produces points in a cube, normalised — which crowds the corners and leaves the axes
     * sparse, visible as an explosion that is slightly diamond shaped. Sampling z uniformly and the
     * azimuth uniformly is Archimedes' theorem and is exactly uniform.
     */
    f32 z         = (_nya_particles_unit(system) * 2.0F) - 1.0F;
    f32 azimuth   = _nya_particles_unit(system) * 2.0F * (f32)M_PI;
    f32 planar    = sqrtf(nya_max(1.0F - (z * z), 0.0F));

    return (f32x3){ planar * cosf(azimuth), planar * sinf(azimuth), z };
}

f32x3 _nya_particles_cone(NYA_ParticleSystem* system, f32x3 axis, f32 spread) {
    if (spread <= 0.0F) return axis;

    // Uniform over the cap rather than over the angle, for the same reason as above: uniform in the
    // angle piles particles up along the axis and thins them at the rim.
    f32 cosine  = 1.0F - (_nya_particles_unit(system) * (1.0F - cosf(spread)));
    f32 sine    = sqrtf(nya_max(1.0F - (cosine * cosine), 0.0F));
    f32 azimuth = _nya_particles_unit(system) * 2.0F * (f32)M_PI;

    // A basis around the axis. Crossed against whichever world axis it is least aligned with, so the
    // cross product cannot collapse — the same trap nya_render3d_line documents.
    f32x3 reference = fabsf(axis.y) < 0.9F ? (f32x3){ 0.0F, 1.0F, 0.0F } : (f32x3){ 1.0F, 0.0F, 0.0F };

    f32x3 right = nya_vector_normalize(nya_vector_cross(axis, reference));
    f32x3 up    = nya_vector_cross(axis, right);

    return nya_vector_normalize((axis * cosine) + (right * (sine * cosf(azimuth))) + (up * (sine * sinf(azimuth))));
}

void _nya_particles_kill(NYA_ParticleSystem* system, u32 index) {
    system->count--;

    // Swap with the last live one rather than shifting everything down, which keeps the pool packed
    // in constant time. Nothing may hold a pointer to a particle across an update because of this,
    // and nothing needs to: a particle has no identity to hold on to.
    if (index != system->count) system->particles[index] = system->particles[system->count];
}
