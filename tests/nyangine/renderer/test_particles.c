/**
 * Particles: the pool, the emission shapes, and the swap-with-last that keeps it packed.
 *
 * Two things here are worth testing and the rest follows from them. The pool: a particle has no
 * identity, dies by being overwritten with the last live one, and the loop that does it must not skip
 * the particle it just moved into the slot. And the sampling: uniform over a sphere is not what three
 * uniform components give, and a burst that gets it wrong is diamond shaped in a way that is obvious
 * on screen and invisible in a debugger.
 *
 * Headless: drawing is stubbed, integration is not.
 **/

#include "nyangine/nyangine.c"
#include "nyangine/nyangine.h"

#include "SDL3/SDL_init.h"

/** Ages every particle past its lifetime in one tick, whatever the burst chose. */
#define LONG_ENOUGH 100.0F

static u32 killed_by_callback = 0;

/** Ends a particle the only way a callback can: by ageing it past its lifetime. */
static void kill_at_half(NYA_Particle* particle, f32 t, f32 delta_time_s, void* user_data) {
  nya_unused(delta_time_s, user_data);

  if (t < 0.5F) return;

  particle->age_s = particle->lifetime_s;
  killed_by_callback++;
}

s32 main(void) {
  _NYA_APP_INSTANCE = (NYA_App){ .initialized = true };
  b8 sdl_ok         = SDL_Init(0);
  nya_assert(sdl_ok, "SDL_Init failed: %s", SDL_GetError());

  NYA_Arena* arena = nya_arena_create(.name = "test_particles");
  defer nya_arena_destroy(arena);

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: emission fills the pool and refuses past it
  // ─────────────────────────────────────────────────────────────────────────────
  {
    NYA_ParticleSystem* system = nya_particles_create(arena, 32);

    nya_assert(nya_particles_count(system) == 0, "a fresh system is empty");

    nya_assert(nya_particles_emit(system, (NYA_ParticleBurst){ .count = 10 }) == 10, "ten fit");
    nya_assert(nya_particles_count(system) == 10, "and are live");

    // Past the ceiling: what fits is created, the rest is counted. An effect that loses a few under
    // load is working correctly, so this is not an error.
    nya_assert(nya_particles_emit(system, (NYA_ParticleBurst){ .count = 40 }) == 22, "only the room left is used");
    nya_assert(nya_particles_count(system) == 32, "the pool is full");
    nya_assert(system->dropped == 18, "and the shortfall is counted, got " FMTu32, system->dropped);

    nya_assert(nya_particles_emit(system, (NYA_ParticleBurst){ .count = 1 }) == 0, "a full pool takes nothing");

    nya_particles_clear(system);
    nya_assert(nya_particles_count(system) == 0, "clear empties it");

    printf("  PASSED\n");
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: particles retire when their time is up, and the pool stays packed
  // ─────────────────────────────────────────────────────────────────────────────
  {
    NYA_ParticleSystem* system = nya_particles_create(arena, 64);

    // Two bursts with very different lifetimes, so the short ones die while the long ones live and
    // the swap-with-last actually has to move something.
    (void)nya_particles_emit(system, (NYA_ParticleBurst){ .count = 20, .lifetime_s = { 0.1F, 0.1F }, .user_id = 1 });
    (void)nya_particles_emit(system, (NYA_ParticleBurst){ .count = 20, .lifetime_s = { 10.0F, 10.0F }, .user_id = 2 });

    nya_assert(nya_particles_count(system) == 40, "forty live");

    nya_particles_update(system, 0.2F);

    /*
     * The regression this exists for.
     *
     * Killing swaps the last live particle into the dead one's slot, so the loop must not advance
     * past it — the moved particle has not been updated and, if it is also dead, would survive.
     * Twenty short-lived particles interleaved with twenty long-lived ones is exactly the shape that
     * catches an off-by-one there.
     */
    nya_assert(nya_particles_count(system) == 20, "the short lived ones are gone, got " FMTu32, nya_particles_count(system));

    for (u32 i = 0; i < nya_particles_count(system); i++) {
      nya_assert(system->particles[i].user_id == 2, "and every survivor is from the long lived burst");
      nya_assert(system->particles[i].age_s > 0.0F, "each of which was actually updated");
    }

    nya_particles_update(system, LONG_ENOUGH);
    nya_assert(nya_particles_count(system) == 0, "and eventually everything retires");

    printf("  PASSED\n");
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: gravity and damping
  // ─────────────────────────────────────────────────────────────────────────────
  {
    NYA_ParticleSystem* system = nya_particles_create(arena, 8);

    (void)nya_particles_emit(system, (NYA_ParticleBurst){
      .count      = 1,
      .lifetime_s = { 10.0F, 10.0F },
      .speed      = { 0.0F, 0.0F },
      .gravity    = { 0.0F, 100.0F, 0.0F },
    });

    nya_particles_update(system, 0.1F);

    // Positive y is down the screen for a 2D system, which is the convention the whole 2D half uses.
    nya_assert(system->particles[0].velocity.y > 0.0F, "gravity pulls down the screen, got %f", (f64)system->particles[0].velocity.y);
    nya_assert(system->particles[0].position.y > 0.0F, "and moves it");

    nya_particles_clear(system);

    /*
     * Damping as an exponential, not a subtraction.
     *
     * `v -= v * damping * dt` reverses direction the moment `damping * dt` exceeds one, so a heavily
     * damped particle at a low frame rate springs backwards. A damping of 10 over a tenth of a second
     * is exactly that case: the naive form would land on zero, and anything larger would overshoot.
     */
    (void)nya_particles_emit(system, (NYA_ParticleBurst){
      .count      = 1,
      .lifetime_s = { 10.0F, 10.0F },
      .shape      = NYA_PARTICLE_SHAPE_CONE,
      .direction  = { 1.0F, 0.0F, 0.0F },
      .spread     = 0.0F,
      .speed      = { 100.0F, 100.0F },
      .damping    = 10.0F,
    });

    nya_assert(system->particles[0].velocity.x == 100.0F, "a cone of zero spread goes straight along its axis");

    nya_particles_update(system, 0.5F);

    nya_assert(system->particles[0].velocity.x > 0.0F, "damping slows it without reversing it, got %f", (f64)system->particles[0].velocity.x);
    nya_assert(system->particles[0].velocity.x < 10.0F, "and slows it a lot, got %f", (f64)system->particles[0].velocity.x);

    printf("  PASSED\n");
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: a 2D system emits in the plane, a 3D one does not
  // ─────────────────────────────────────────────────────────────────────────────
  {
    NYA_ParticleSystem* flat = nya_particles_create(arena, 256);

    (void)nya_particles_emit(flat, (NYA_ParticleBurst){ .count = 200, .shape = NYA_PARTICLE_SHAPE_SPHERE, .radius = 5.0F });

    // Flattened, so a sphere becomes a disc. Without this a point burst on screen loses most of its
    // particles into a z that nothing draws — the effect is simply thinner than it was authored.
    for (u32 i = 0; i < nya_particles_count(flat); i++) {
      nya_assert(flat->particles[i].position.z == 0.0F, "a 2D burst stays in the plane");
      nya_assert(flat->particles[i].velocity.z == 0.0F, "and so does its velocity");
    }

    NYA_ParticleSystem* volume = nya_particles_create(arena, 256);
    nya_particles_space_set(volume, NYA_PARTICLE_SPACE_3D);

    (void)nya_particles_emit(volume, (NYA_ParticleBurst){ .count = 200, .shape = NYA_PARTICLE_SHAPE_SPHERE, .radius = 5.0F });

    u32 off_plane = 0;
    for (u32 i = 0; i < nya_particles_count(volume); i++) off_plane += fabsf(volume->particles[i].velocity.z) > 1.0F;

    nya_assert(off_plane > 100, "a 3D burst uses all three axes, got " FMTu32 " of 200", off_plane);

    printf("  PASSED\n");
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: a sphere burst is round, not diamond shaped
  // ─────────────────────────────────────────────────────────────────────────────
  {
    NYA_ParticleSystem* system = nya_particles_create(arena, 4096);
    nya_particles_space_set(system, NYA_PARTICLE_SPACE_3D);

    (void)nya_particles_emit(system, (NYA_ParticleBurst){ .count = 4000, .speed = { 1.0F, 1.0F } });

    /*
     * Uniform on the sphere, which three uniform components normalised is not.
     *
     * That fills a cube and normalises it, crowding the corners and thinning the axes — an explosion
     * that is visibly diamond shaped. The check is that each axis carries roughly a third of the
     * total squared length, which is what uniformity means and what the cube version fails.
     */
    f32x3 squared = f32x3_zero;
    for (u32 i = 0; i < nya_particles_count(system); i++) {
      f32x3 v  = system->particles[i].velocity;
      squared += v * v;
    }

    f32 total = squared.x + squared.y + squared.z;

    nya_assert(total > 0.0F, "something was emitted");

    for (u32 axis = 0; axis < 3; axis++) {
      f32 share = squared[axis] / total;
      nya_assert(share > 0.30F && share < 0.36F, "axis " FMTu32 " carries a third of the energy, got %f", axis, (f64)share);
    }

    printf("  PASSED\n");
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: the callback can steer and can end a particle
  // ─────────────────────────────────────────────────────────────────────────────
  {
    NYA_ParticleSystem* system = nya_particles_create(arena, 16);

    killed_by_callback = 0;
    nya_particles_on_update_set(system, kill_at_half, nullptr);

    (void)nya_particles_emit(system, (NYA_ParticleBurst){ .count = 5, .lifetime_s = { 1.0F, 1.0F } });

    // Under half: nothing ends.
    nya_particles_update(system, 0.4F);
    nya_assert(nya_particles_count(system) == 5, "nothing has reached half life yet");
    nya_assert(killed_by_callback == 0, "so the callback has ended nothing");

    // Past half: every one of them ends, on the same tick it asked to rather than the next.
    nya_particles_update(system, 0.2F);
    nya_assert(killed_by_callback == 5, "the callback ended all five, got " FMTu32, killed_by_callback);
    nya_assert(nya_particles_count(system) == 0, "and they went immediately, not a frame later");

    printf("  PASSED\n");
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: the same seed gives the same effect
  // ─────────────────────────────────────────────────────────────────────────────
  {
    NYA_ParticleSystem* left  = nya_particles_create(arena, 64);
    NYA_ParticleSystem* right = nya_particles_create(arena, 64);

    nya_particles_seed(left, 0x1234ABCD);
    nya_particles_seed(right, 0x1234ABCD);

    NYA_ParticleBurst burst = { .count = 32, .speed = { 10.0F, 200.0F }, .lifetime_s = { 0.1F, 2.0F } };

    (void)nya_particles_emit(left, burst);
    (void)nya_particles_emit(right, burst);

    // Reproducible, which is what makes a replay or a deterministic test possible at all.
    for (u32 i = 0; i < 32; i++) {
      nya_assert(left->particles[i].velocity.x == right->particles[i].velocity.x, "particle " FMTu32 " matches", i);
      nya_assert(left->particles[i].lifetime_s == right->particles[i].lifetime_s, "and so does its lifetime");
    }

    printf("  PASSED\n");
  }

  printf("PASSED: test_particles\n");
  return 0;
}
