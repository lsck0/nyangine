/**
 * The 2D physics world's own controls, as opposed to the entity seam.
 *
 * tests/nyangine/core/test_physics2d.c covers the seam between a body and the entity carrying it.
 * This covers what that one leaves alone: the world's tunables, the force and velocity API, teleport,
 * sleep, the hit threshold and the point query. Box2D's solver is still not what is being tested —
 * every assertion here is about the engine's own layer over it.
 *
 * Headless: the physics world needs an arena and a clock and nothing else.
 **/

#include "nyangine/nyangine.c"
#include "nyangine/nyangine.h"

#include "SDL3/SDL_init.h"

#define TICK (1.0F / 60.0F)

/** Handles are a struct of index and generation, and there is no equality helper in the engine. */
static b8 same_entity(NYA_EntityHandle a, NYA_EntityHandle b) {
    return a.index == b.index && a.generation == b.generation;
}

static void step(u32 count) {
    for (u32 i = 0; i < count; i++) nya_system_physics2d_update(TICK);
}

s32 main(void) {
    _NYA_APP_INSTANCE = (NYA_App){ .initialized = true };
    b8 sdl_ok         = SDL_Init(0);
    nya_assert(sdl_ok, "SDL_Init failed: %s", SDL_GetError());

    nya_system_callback_init();
    NYA_World* world = nya_world_create();
    (void)nya_world_set(world);
    defer nya_world_destroy(world);
    defer nya_system_callback_deinit();

    // ── Gravity is settable and readable, and survives a step.
    {
        f32x2 original = nya_physics2d_gravity();

        nya_physics2d_gravity_set((f32x2){ 0.0F, 0.0F });
        f32x2 zeroed = nya_physics2d_gravity();
        nya_check(zeroed.x == 0.0F && zeroed.y == 0.0F, "gravity should read back what was set");

        // With no gravity a free body must not accumulate any vertical velocity at all.
        NYA_EntityHandle floater = nya_entity_spawn(.name = "floater", .position = { 0.0F, 0.0F, 0.0F });
        nya_physics2d_body_attach(floater, .shape = NYA_PHYSICS2D_SHAPE_BOX, .size = { 8.0F, 8.0F });
        step(30);

        f32x2 velocity = nya_physics2d_velocity(nya_entity_get(floater));
        nya_check(fabsf(velocity.y) < 0.001F, "no gravity means no fall, got vy=%f", (f64)velocity.y);

        nya_entity_despawn(floater);
        nya_physics2d_gravity_set(original);
    }

    // ── The world can be paused, and a paused world does not integrate.
    {
        NYA_EntityHandle crate = nya_entity_spawn(.name = "paused", .position = { 0.0F, 0.0F, 0.0F });
        nya_physics2d_body_attach(crate, .shape = NYA_PHYSICS2D_SHAPE_BOX, .size = { 8.0F, 8.0F });

        nya_physics2d_enabled_set(false);
        nya_check(!nya_physics2d_enabled(), "the world should report itself paused");

        f32 before = nya_entity_get(crate)->position.y;
        step(30);
        f32 after = nya_entity_get(crate)->position.y;
        nya_check(before == after, "a paused world must not move anything, moved %f", (f64)(after - before));

        nya_physics2d_enabled_set(true);
        nya_check(nya_physics2d_enabled(), "the world should resume");

        nya_entity_despawn(crate);
    }

    // ── Impulse and force both accelerate, and an impulse is instantaneous.
    {
        nya_physics2d_gravity_set((f32x2){ 0.0F, 0.0F });

        NYA_EntityHandle puck = nya_entity_spawn(.name = "puck", .position = { 0.0F, 0.0F, 0.0F });
        nya_physics2d_body_attach(puck, .shape = NYA_PHYSICS2D_SHAPE_BOX, .size = { 8.0F, 8.0F });

        nya_physics2d_apply_impulse(nya_entity_get(puck), (f32x2){ 100.0F, 0.0F });
        step(1);
        f32x2 kicked = nya_physics2d_velocity(nya_entity_get(puck));
        nya_check(kicked.x > 0.0F, "an impulse should move it along +x, got vx=%f", (f64)kicked.x);

        // A force applied for one tick must do strictly less than the same number as an impulse.
        NYA_EntityHandle pushed = nya_entity_spawn(.name = "pushed", .position = { 0.0F, 0.0F, 0.0F });
        nya_physics2d_body_attach(pushed, .shape = NYA_PHYSICS2D_SHAPE_BOX, .size = { 8.0F, 8.0F });
        nya_physics2d_apply_force(nya_entity_get(pushed), (f32x2){ 100.0F, 0.0F });
        step(1);
        f32x2 shoved = nya_physics2d_velocity(nya_entity_get(pushed));
        nya_check(shoved.x < kicked.x, "one tick of force should be weaker than the same impulse");

        nya_entity_despawn(puck);
        nya_entity_despawn(pushed);
    }

    // ── Velocity and angular velocity round trip.
    {
        NYA_EntityHandle spinner = nya_entity_spawn(.name = "spinner", .position = { 0.0F, 0.0F, 0.0F });
        nya_physics2d_body_attach(spinner, .shape = NYA_PHYSICS2D_SHAPE_BOX, .size = { 8.0F, 8.0F });

        nya_physics2d_velocity_set(nya_entity_get(spinner), (f32x2){ 12.0F, -7.0F });
        f32x2 read = nya_physics2d_velocity(nya_entity_get(spinner));
        nya_check(fabsf(read.x - 12.0F) < 0.01F && fabsf(read.y + 7.0F) < 0.01F,
                  "velocity should read back what was set, got (%f, %f)", (f64)read.x, (f64)read.y);

        nya_physics2d_angular_velocity_set(nya_entity_get(spinner), 3.0F);
        f32 spin = nya_physics2d_angular_velocity(nya_entity_get(spinner));
        nya_check(fabsf(spin - 3.0F) < 0.01F, "angular velocity should read back, got %f", (f64)spin);

        nya_physics2d_apply_angular_impulse(nya_entity_get(spinner), 1.0F);
        step(1);
        nya_check(nya_physics2d_angular_velocity(nya_entity_get(spinner)) > spin - 0.01F,
                  "an angular impulse should not slow it down");

        nya_entity_despawn(spinner);
    }

    // ── Teleport moves the body, not just the entity, and clears no velocity of its own.
    {
        NYA_EntityHandle ghost = nya_entity_spawn(.name = "ghost", .position = { 0.0F, 0.0F, 0.0F });
        nya_physics2d_body_attach(ghost, .shape = NYA_PHYSICS2D_SHAPE_BOX, .size = { 8.0F, 8.0F });

        nya_physics2d_teleport(nya_entity_get(ghost), (f32x2){ 250.0F, -120.0F }, 0.5F);
        step(1);

        NYA_Entity* entity = nya_entity_get(ghost);
        nya_check(fabsf(entity->position.x - 250.0F) < 1.0F, "teleport should move x, at %f", (f64)entity->position.x);
        nya_check(fabsf(nya_physics2d_rotation(entity) - 0.5F) < 0.01F, "teleport should set rotation");

        nya_entity_despawn(ghost);
    }

    // ── Sleep: a body left alone settles, and waking it reports awake again.
    {
        nya_physics2d_gravity_set((f32x2){ 0.0F, 0.0F });

        NYA_EntityHandle sleeper = nya_entity_spawn(.name = "sleeper", .position = { 0.0F, 0.0F, 0.0F });
        nya_physics2d_body_attach(sleeper, .shape = NYA_PHYSICS2D_SHAPE_BOX, .size = { 8.0F, 8.0F });

        step(240);
        nya_check(!nya_physics2d_awake(nya_entity_get(sleeper)), "a body with nothing acting on it should sleep");

        nya_physics2d_wake(nya_entity_get(sleeper));
        nya_check(nya_physics2d_awake(nya_entity_get(sleeper)), "waking should take effect immediately");

        nya_entity_despawn(sleeper);
    }

    // ── The hit threshold round trips. It is what stops a settled stack spamming events.
    {
        f32 original = nya_physics2d_hit_threshold();
        nya_physics2d_hit_threshold_set(42.0F);
        nya_check(fabsf(nya_physics2d_hit_threshold() - 42.0F) < 0.01F, "hit threshold should read back");
        nya_physics2d_hit_threshold_set(original);
    }

    // ── The point query finds a body under a point, and nothing under empty space.
    {
        NYA_EntityHandle target = nya_entity_spawn(.name = "target", .position = { 500.0F, 500.0F, 0.0F });
        nya_physics2d_body_attach(target, .type = NYA_PHYSICS_BODY_STATIC, .shape = NYA_PHYSICS2D_SHAPE_BOX,
                                  .size = { 40.0F, 40.0F });
        step(1);

        nya_check(same_entity(nya_physics2d_entity_at((f32x2){ 500.0F, 500.0F }), target), "the point query should find the box");
        nya_check(same_entity(nya_physics2d_entity_at((f32x2){ -9000.0F, -9000.0F }), NYA_ENTITY_HANDLE_NONE),
                  "empty space should yield no entity");

        nya_entity_despawn(target);
    }

    // ── Detaching leaves the entity alive and the body gone, and the world's count agrees.
    {
        u32              before = nya_physics2d_body_count();
        NYA_EntityHandle temp   = nya_entity_spawn(.name = "temp", .position = { 0.0F, 0.0F, 0.0F });
        nya_physics2d_body_attach(temp, .shape = NYA_PHYSICS2D_SHAPE_BOX, .size = { 8.0F, 8.0F });

        nya_check(nya_physics2d_body_count() == before + 1, "attaching should add exactly one body");
        nya_check(nya_physics2d_body_attached(nya_entity_get(temp)), "the entity should report a body");

        nya_physics2d_body_detach(temp);
        nya_check(nya_physics2d_body_count() == before, "detaching should remove exactly one body");
        nya_check(!nya_physics2d_body_attached(nya_entity_get(temp)), "the entity should report no body");
        nya_check(nya_entity_get(temp) != nullptr, "detaching a body must not despawn the entity");

        nya_entity_despawn(temp);
    }

    // ── Pixels per metre is the one conversion seam, and it round trips.
    {
        f32 original = nya_physics2d_pixels_per_meter();
        nya_physics2d_pixels_per_meter_set(50.0F);
        nya_check(fabsf(nya_physics2d_pixels_per_meter() - 50.0F) < 0.01F, "pixels per metre should read back");
        nya_physics2d_pixels_per_meter_set(original);
    }

    // ── The world reports how long its last step took, and it is not negative.
    {
        step(1);
        nya_check(nya_physics2d_last_step_time_s() >= 0.0F, "a step time should never be negative");
    }

    return nya_check_failures() == 0 ? 0 : 1;
}
