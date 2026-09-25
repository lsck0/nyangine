/**
 * @file render_particles.h
 *
 * ```c
 * NYA_ParticleSystem* sparks = nya_particles_create(world->allocator, 2048);
 *
 * nya_particles_emit(sparks, (NYA_ParticleBurst){
 *     .position     = { hit.point.x, hit.point.y, 0 },
 *     .count        = 24,
 *     .speed        = { 60.0F, 180.0F },
 *     .lifetime_s   = { 0.2F, 0.6F },
 *     .size         = { 2.0F, 5.0F },
 *     .color_start  = NYA_COLOR_YELLOW,
 *     .color_end    = { 1.0F, 0.2F, 0.0F, 0.0F },
 *     .gravity      = { 0, 400, 0 },
 * });
 *
 * nya_particles_update(sparks, delta_time_s);   // once a tick
 * nya_particles_draw(window, sparks);           // from a layer's on_render
 * ```
 * */
#pragma once

#include "nyangine-std/base/base_arena.h"
#include "nyangine-std/base/base_attributes.h"
#include "nyangine-std/base/base_types.h"
#include "nyangine-std/math/math_random.h"
#include "nyangine-std/math/math_vector.h"
#include "nyangine-core/renderer/render_color.h"

typedef struct NYA_Window NYA_Window;

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * TYPES
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

typedef enum NYA_ParticleSpace     NYA_ParticleSpace;
typedef enum NYA_ParticleShape     NYA_ParticleShape;
typedef struct NYA_Particle        NYA_Particle;
typedef struct NYA_ParticleBurst   NYA_ParticleBurst;
typedef struct NYA_ParticleSystem  NYA_ParticleSystem;

/** A wind field the particles may drift on. Defined in render_wind.h; only ever held here by pointer. */
typedef struct NYA_WindField       NYA_WindField;

/** A set of force fields the particles accelerate under. Defined in render_force.h; only ever held here by pointer. */
typedef struct NYA_ForceSet        NYA_ForceSet;

enum NYA_ParticleSpace {
    /**
     * Drawn through render2d, in world pixels, ignoring z. The default.
     * */
    NYA_PARTICLE_SPACE_2D = 0,

    /**
     * Drawn through render3d as camera-facing quads.
     * */
    NYA_PARTICLE_SPACE_3D,

    NYA_PARTICLE_SPACE_COUNT,
};

/** Where in a burst's volume a particle starts, and which way it initially goes. */
enum NYA_ParticleShape {
    /** All from one point, in a uniformly random direction. Sparks, hits, puffs. */
    NYA_PARTICLE_SHAPE_POINT = 0,

    /** From anywhere inside a sphere of `radius`, moving outward from the centre. An explosion. */
    NYA_PARTICLE_SHAPE_SPHERE,

    /**
     * Within `spread` radians of `direction`. A cone in 3D, a fan in 2D.
     * */
    NYA_PARTICLE_SHAPE_CONE,

    /** From anywhere inside a box of `volume`, keeping `direction`. Rain, dust, a smoke column. */
    NYA_PARTICLE_SHAPE_BOX,

    NYA_PARTICLE_SHAPE_COUNT,
};

/**
 * One live particle. Public so an on_update callback can steer it.
 * */
struct NYA_Particle {
    f32x3 position;

    /** Where it was a tick ago. Draws interpolate from here, see nya_app_tick_alpha. */
    f32x3 position_previous;
    f32x3 velocity;

    /** Applied every tick. Copied from the burst so one system can hold several behaviours. */
    f32x3 acceleration;

    /** Seconds lived, and how long it gets. `age / lifetime_s` is the interpolation parameter. */
    f32 age_s;
    f32 lifetime_s;

    f32 size_start;
    f32 size_end;

    NYA_Color color_start;
    NYA_Color color_end;

    /** Radians, and radians per second. Ignored in 3D, where the quad always faces the camera. */
    f32 rotation;
    f32 angular_velocity;

    /**
     * Fraction of velocity shed per second, as a multiplier applied continuously.
     * */
    f32 damping;

    /** Whatever the game wants. Never interpreted; same contract as NYA_Entity.user_data. */
    u32 user_id;
};

/**
 * A description of particles to spawn. Everything has a usable default, so `{ .count = 20 }` works.
 * */
struct NYA_ParticleBurst {
    NYA_ParticleShape shape;

    /** Where the burst happens. For a 2D system, z is ignored on the way out. */
    f32x3 position;

    /** How many to spawn. Capped by whatever room the pool has; see nya_particles_emit. */
    u32 count;

    /** SPHERE: how far from `position` a particle may start. */
    f32 radius;

    /**
     * BOX: full extents of the spawn volume.
     * */
    f32x3 volume;

    /** CONE and BOX: which way the particles go. Zero is read as straight up in the space's sense. */
    f32x3 direction;

    /** CONE: half angle, in radians. */
    f32 spread;

    /** World units per second, sampled per particle. Zero maximum is read as `{ 50, 100 }`. */
    f32x2 speed;

    /** Seconds. Zero maximum is read as `{ 0.5, 1.0 }`. */
    f32x2 lifetime_s;

    /** World units. Zero maximum is read as `{ 2, 4 }`, and the end size defaults to the start. */
    f32x2 size;
    f32x2 size_end;

    /** Zero alpha on the end colour is the usual fade out. A zeroed start colour is read as white. */
    NYA_Color color_start;
    NYA_Color color_end;

    /** World units per second squared. For a 2D system remember that positive y is down the screen. */
    f32x3 gravity;

    /** See NYA_Particle.damping. */
    f32 damping;

    /** Radians and radians per second, both sampled as ranges. Ignored by a 3D system. */
    f32x2 rotation;
    f32x2 angular_velocity;

    u32 user_id;
};

/**
 * Steers a particle after the integration each tick. Optional.
 * */
typedef void (*NYA_ParticleUpdateFn)(NYA_Particle* particle, f32 t, f32 delta_time_s, void* user_data);

struct NYA_ParticleSystem {
    NYA_Arena* allocator;

    NYA_ParticleSpace space;

    /**
     * The pool. Live particles are kept packed at the front, so iteration touches no dead ones.
     * */
    NYA_Particle* particles;
    u32           capacity;
    u32           count;

    /**
     * The texture every particle is drawn with, or null for a solid quad.
     * */
    NYA_ConstCString texture;

    /**
     * Whether a 3D system's billboards contribute to the shadow map. Off by default.
     * */
    b8 casts_shadow;

    NYA_ParticleUpdateFn on_update;
    void*                on_update_user_data;

    /**
     * Its own generator, so a system is reproducible independently of everything else drawing.
     * */
    NYA_RNG* rng;

    /** Particles asked for and refused because the pool was full, since the last update. */
    u32 dropped;

    /** The last update's step, which a draw between ticks takes a fraction of. */
    f32 tick_s;

    /**
     * An optional wind field the particles drift on, borrowed from the caller, and how quickly they follow it.
     *
     * Null is the default and means no wind — the system integrates exactly as before. Set it and every particle
     * eases its velocity toward what the field pushes at its position, so dust and smoke ride the same air that
     * moves the foliage and the water. `wind_time_s` is the field clock this system advances itself, so the wind
     * animates whether or not the caller also advances the shared field. See nya_particles_wind_set, [[render_wind]].
     * */
    const NYA_WindField* wind;
    f32                  wind_influence;
    f32                  wind_time_s;

    /**
     * An optional set of force fields the particles accelerate under, borrowed from the caller, and how strongly.
     *
     * Null is the default and means no force — the system integrates exactly as before. Set it and every tick each
     * particle takes `nya_forces_at(...) * influence` as an acceleration, added to its velocity, so a gravity well,
     * a vortex or a curl-noise stir moves the same dust the wind and the water already share. This sits beside the
     * wind rather than replacing it: a scene may run both, the force summed onto the wind-eased velocity. `force_time_s`
     * is the set clock this system advances itself, so the field animates on its own. See nya_particles_force_set,
     * [[render_force]].
     * */
    const NYA_ForceSet* forces;
    f32                 force_influence;
    f32                 force_time_s;
};

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * FUNCTIONS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/**
 * Builds a system with room for `capacity` live particles at once.
 * */
NYA_API NYA_ParticleSystem* nya_particles_create(NYA_Arena* arena, u32 capacity) __attr_no_discard;

/** Which space it draws in. NYA_PARTICLE_SPACE_2D unless this says otherwise. */
NYA_API void nya_particles_space_set(NYA_ParticleSystem* system, NYA_ParticleSpace space);

/** The texture every particle draws with, or null for solid quads. See NYA_ParticleSystem.texture. */
NYA_API void nya_particles_texture_set(NYA_ParticleSystem* system, NYA_ConstCString texture);

/** Opts a 3D system into casting a (solid) shadow. See NYA_ParticleSystem.casts_shadow. */
NYA_API void nya_particles_casts_shadow_set(NYA_ParticleSystem* system, b8 casts_shadow);

/** Installs the per particle callback. Null removes it. */
NYA_API void nya_particles_on_update_set(NYA_ParticleSystem* system, NYA_ParticleUpdateFn on_update, void* user_data);

/**
 * Makes the system drift on `field`, at `influence` (roughly how fast a particle catches up to the wind, per
 * second). A null `field` turns it off, which is the default and the exact old behaviour. The field is borrowed —
 * the caller owns it and keeps it alive — so several systems and the foliage can share one wind.
 * */
NYA_API void nya_particles_wind_set(NYA_ParticleSystem* system, const NYA_WindField* field, f32 influence);

/**
 * Makes the system accelerate under `forces`, at `influence` (a multiplier on the sampled acceleration). A null
 * `forces` turns it off, which is the default and the exact old behaviour. The set is borrowed — the caller owns it
 * and keeps it alive — so several systems, the fluid and the foliage can share one composed field. Independent of
 * and additive to the wind: a system may run both at once.
 * */
NYA_API void nya_particles_force_set(NYA_ParticleSystem* system, const NYA_ForceSet* forces, f32 influence);

/**
 * Makes the system reproducible: the same seed and the same calls give the same effect.
 * */
NYA_API void nya_particles_seed(NYA_ParticleSystem* system, u64 seed);

/**
 * Spawns a burst. Returns how many were actually created.
 * */
NYA_API u32 nya_particles_emit(NYA_ParticleSystem* system, NYA_ParticleBurst burst);

/**
 * Integrates every live particle by one tick and retires the ones whose time is up.
 * */
NYA_API void nya_particles_update(NYA_ParticleSystem* system, f32 delta_time_s);

/**
 * Draws every live particle, through render2d or render3d depending on the system's space.
 * */
NYA_API void nya_particles_draw(NYA_Window* window, const NYA_ParticleSystem* system);

/** Retires every particle immediately, without running anything. For a level change. */
NYA_API void nya_particles_clear(NYA_ParticleSystem* system);

NYA_API u32 nya_particles_count(const NYA_ParticleSystem* system) __attr_no_discard;
