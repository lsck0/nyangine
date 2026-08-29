/**
 * @file render_particles.h
 *
 * Particles: an emitter is a description, a system is a pool of live ones, and both dimensions share
 * the pool.
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
 *
 * ## Why 2D and 3D are one system
 *
 * Because a particle is a position, a velocity, a lifetime and a colour in both, and the only thing
 * that differs is the last step — whether it is drawn as a screen-facing quad through render2d or as
 * a billboarded quad through render3d. Splitting them would duplicate the integration, the pool, the
 * lifetime bookkeeping and the emission shapes to change one draw call.
 *
 * So a particle always has three position components. A 2D emitter simply leaves z at zero, which is
 * where the 2D world already is — the same reasoning NYA_PhysicsHit uses for its f32x3 point.
 *
 * NYA_ParticleSpace on the system decides which way it draws. It is on the *system*, not the burst,
 * because one draw call cannot be half 2D.
 *
 * ## What this deliberately is not
 *
 * Not a GPU system. Particles are integrated on the CPU and fed through the ordinary batches, which
 * costs about a microsecond per thousand and buys the whole thing being debuggable, orderable against
 * everything else drawn, and identical on every backend. A compute-shader system is a different
 * design, and the number that would justify it is somewhere past a hundred thousand.
 *
 * Not a curve editor either. Colour and size interpolate linearly from start to end over a
 * particle's life. Anything richer belongs in a per-particle callback, which is what `on_update` is.
 * */
#pragma once

#include "nyangine/base/base_arena.h"
#include "nyangine/base/base_attributes.h"
#include "nyangine/base/base_types.h"
#include "nyangine/math/math_random.h"
#include "nyangine/math/math_vector.h"
#include "nyangine/renderer/render_color.h"

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

enum NYA_ParticleSpace {
    /**
     * Drawn through render2d, in world pixels, ignoring z. The default.
     *
     * z is still integrated — a 2D emitter that sets a z velocity gets one, it simply does not affect
     * where the particle lands on screen. That is occasionally useful as a fake depth to drive size
     * or alpha from, and costs nothing to leave in.
     * */
    NYA_PARTICLE_SPACE_2D = 0,

    /**
     * Drawn through render3d as camera-facing quads.
     *
     * Only draws while a 3D camera is active, for the same reason NYA_ENTITY_VISUAL_CUBE does: there
     * is no projection otherwise, and guessing one puts the particles somewhere arbitrary.
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
     *
     * A spread of zero is a straight line and a spread of pi is the whole sphere, which makes POINT
     * the same thing as a cone of pi — kept separate anyway, because "sparks in every direction" is
     * what most callers want and should not require knowing that.
     * */
    NYA_PARTICLE_SHAPE_CONE,

    /** From anywhere inside a box of `volume`, keeping `direction`. Rain, dust, a smoke column. */
    NYA_PARTICLE_SHAPE_BOX,

    NYA_PARTICLE_SHAPE_COUNT,
};

/**
 * One live particle. Public so an on_update callback can steer it.
 *
 * Plain data in a flat pool. There is no per-particle allocation and no handle: a particle is
 * identified by nothing at all, because nothing outlives it.
 * */
struct NYA_Particle {
    f32x3 position;
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
     *
     * Zero is a vacuum. Around 2 brings a spark to a near stop in half a second, which is what most
     * impact effects want — without it they sail off the screen at their initial speed.
     * */
    f32 damping;

    /** Whatever the game wants. Never interpreted; same contract as NYA_Entity.user_data. */
    u32 user_id;
};

/**
 * A description of particles to spawn. Everything has a usable default, so `{ .count = 20 }` works.
 *
 * Ranges are `{ minimum, maximum }` and are sampled per particle. That is the whole reason a burst
 * is a struct rather than a dozen arguments: an effect is almost entirely made of ranges, and a
 * burst with one number for each looks synthetic — every particle the same size living the same
 * time reads as a grid rather than as debris.
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
     *
     * Named apart from `size` because that one is the *particle's* size range and this is the
     * emitter's volume — two different things a burst needs at once, which is exactly why one of them
     * could not simply be called `size`.
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
 *
 * The escape hatch for anything the burst cannot describe — curves, attraction toward a point, a
 * colour that depends on speed. `t` is the particle's age over its lifetime, in [0, 1], because that
 * is the number every such rule is written against.
 *
 * Setting `age_s` past `lifetime_s` kills the particle at the end of this tick, which is how a
 * callback ends one early.
 * */
typedef void (*NYA_ParticleUpdateFn)(NYA_Particle* particle, f32 t, f32 delta_time_s, void* user_data);

struct NYA_ParticleSystem {
    NYA_Arena* allocator;

    NYA_ParticleSpace space;

    /**
     * The pool. Live particles are kept packed at the front, so iteration touches no dead ones.
     *
     * Killing a particle swaps the last live one into its slot, which is why nothing may hold a
     * pointer to one across an update — and why nothing needs to, since a particle has no identity.
     * */
    NYA_Particle* particles;
    u32           capacity;
    u32           count;

    /**
     * The texture every particle is drawn with, or null for a solid quad.
     *
     * One for the whole system, because one draw call has one texture — a system of mixed textures
     * is several systems, and saying so here is cheaper than discovering it as a draw call per
     * particle.
     *
     * Honoured in both spaces. It was 2D-only for a while and silently ignored in 3D, which mattered more
     * than it sounds: a 3D particle is a camera-facing quad, so without a texture it is a hard-edged
     * *square* — no arrangement of colours softens it, because the shape is the geometry. A soft radial
     * sprite is the whole difference between a stack of squares and smoke.
     * */
    NYA_ConstCString texture;

    /**
     * Whether a 3D system's billboards contribute to the shadow map. Off by default.
     *
     * The shadow pass has no idea what alpha is — it writes full depth for anything drawn into it, so a
     * translucent billboard would cast a hard-edged, fully opaque square rather than the soft puff it
     * draws as everywhere else. Additive systems (fire, sparks) never had this problem, since light does
     * not cast a shadow and additive geometry already skips the pass entirely; ordinary alpha-blended
     * systems — smoke, dust — need to opt in explicitly instead, via nya_particles_casts_shadow_set.
     * */
    b8 casts_shadow;

    NYA_ParticleUpdateFn on_update;
    void*                on_update_user_data;

    /**
     * Its own generator, so a system is reproducible independently of everything else drawing.
     *
     * A pointer rather than a value: NYA_RNG holds AVX2 state needing thirty-two byte alignment,
     * which an arena does not hand out by default — nya_rng_create_in exists precisely because
     * embedding one by value is a segfault on the first buffer refill, long after the allocation.
     * */
    NYA_RNG* rng;

    /** Particles asked for and refused because the pool was full, since the last update. */
    u32 dropped;
};

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * FUNCTIONS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/**
 * Builds a system with room for `capacity` live particles at once.
 *
 * The pool is allocated once and never grows, which is what makes emission allocation free — a burst
 * past the ceiling drops particles and counts them rather than reallocating in the middle of a frame.
 * Everything comes from `arena`, so freeing the system is freeing the arena.
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
 * Makes the system reproducible: the same seed and the same calls give the same effect.
 *
 * For a replay, a test, or a deterministic simulation. Systems seed themselves from a fixed constant
 * rather than from the clock, so two runs already match unless something reseeds them.
 * */
NYA_API void nya_particles_seed(NYA_ParticleSystem* system, u64 seed);

/**
 * Spawns a burst. Returns how many were actually created.
 *
 * Fewer than asked when the pool is full, and the shortfall is added to `dropped` rather than
 * reported as an error — an effect that occasionally loses a few particles is working correctly
 * under load, and a burst that failed would be an error nobody could act on mid-frame.
 * */
NYA_API u32 nya_particles_emit(NYA_ParticleSystem* system, NYA_ParticleBurst burst);

/**
 * Integrates every live particle by one tick and retires the ones whose time is up.
 *
 * Called once per fixed tick from a layer's on_update, not from on_render — drawing twice in a frame
 * would otherwise advance them twice.
 * */
NYA_API void nya_particles_update(NYA_ParticleSystem* system, f32 delta_time_s);

/**
 * Draws every live particle, through render2d or render3d depending on the system's space.
 *
 * Newest first is deliberately *not* imposed: particles are drawn in pool order, which after a few
 * kills is neither spawn order nor reverse. For additive or fading effects that is invisible, and
 * imposing an order would mean a sort per frame for something nobody can see.
 * */
NYA_API void nya_particles_draw(NYA_Window* window, const NYA_ParticleSystem* system);

/** Retires every particle immediately, without running anything. For a level change. */
NYA_API void nya_particles_clear(NYA_ParticleSystem* system);

NYA_API u32 nya_particles_count(const NYA_ParticleSystem* system) __attr_no_discard;
