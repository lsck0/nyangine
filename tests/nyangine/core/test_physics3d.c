/**
 * The 3D solver, and the contract between it and the entity table.
 **/

#include "nyangine/nyangine.c"
#include "nyangine/nyangine.h"

#include "SDL3/SDL_init.h"

/** One fixed tick, matching the engine's default time step. */
#define TICK (1.0F / 60.0F)

static void step(u32 count) {
  for (u32 i = 0; i < count; i++) nya_system_physics3d_update(TICK);
}

static u32              sensor_enters = 0;
static u32              sensor_exits  = 0;
static u32              impacts       = 0;
static NYA_EntityHandle sensor_other  = NYA_ENTITY_HANDLE_NONE;

static void record_collision(NYA_Entity* entity, NYA_Entity* other, const NYA_PhysicsHit* hit) {
  nya_unused(entity);

  // Every hit from this solver has to say which one it came out of, or a game running both cannot tell which world's units it is holding.
  nya_assert(hit->dimension == NYA_PHYSICS_3D, "a 3D hit is tagged 3D");

  switch (hit->kind) {
    case NYA_PHYSICS_HIT_SENSOR_ENTER: {
      sensor_enters++;
      if (other != nullptr) sensor_other = other->handle;
    } break;

    case NYA_PHYSICS_HIT_SENSOR_EXIT: sensor_exits++; break;
    case NYA_PHYSICS_HIT_IMPACT:      impacts++; break;
    default:                          break;
  }
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

  // TEST: a fresh world, and the defaults that differ from 2D
  {
    nya_assert(nya_physics3d_body_count() == 0, "no bodies yet");
    nya_assert(nya_physics3d_enabled(), "the world starts running");

    // One, not thirty-two. A 3D scene has no pixel scale to convert through, so the natural unit is the metre and the conversion is the identity.
    nya_assert(nya_physics3d_units_per_meter() == 1.0F, "got %f", (f64)nya_physics3d_units_per_meter());

    // Negative y, where the 2D world's gravity is positive. The two disagree about which way down is, which costs nothing because nothing is simulated in both.
    f32x3 gravity = nya_physics3d_gravity();
    nya_assert(gravity.y < 0.0F, "3D gravity points down negative y, got %f", (f64)gravity.y);
    nya_assert(gravity.x == 0.0F && gravity.z == 0.0F, "and nowhere else");

    printf("  PASSED\n");
  }

  // TEST: the solver owns the transform once a body is attached
  {
    NYA_EntityHandle floor = nya_entity_spawn(.name = "floor", .position = { 0.0F, -0.5F, 0.0F });
    nya_assert(nya_physics3d_body_attach(floor, .type = NYA_PHYSICS_BODY_STATIC, .shape = NYA_PHYSICS3D_SHAPE_BOX, .size = { 20.0F, 1.0F, 20.0F }));

    NYA_EntityHandle box = nya_entity_spawn(.name = "box", .position = { 0.0F, 5.0F, 0.0F });
    nya_assert(nya_physics3d_body_attach(box, .shape = NYA_PHYSICS3D_SHAPE_BOX, .size = { 1.0F, 1.0F, 1.0F }, .density = 500.0F));

    nya_assert(nya_physics3d_body_count() == 2, "two bodies");
    nya_assert(nya_physics3d_body_attached(nya_entity_get(box)), "and the box has one");

    // the entity's own velocity integration steps aside for a simulated body, or two writers would fight over the position.
    NYA_Entity* entity = nya_entity_get(box);
    f32         start  = entity->position.y;

    // a third of a second, well short of the ~0.96 s a 4.5 m fall takes, so it is still airborne and the velocity is meaningful.
    step(20);

    nya_assert(entity->position.y < start, "it fell, from %f to %f", (f64)start, (f64)entity->position.y);
    nya_assert(entity->velocity.y < 0.0F, "and the velocity was mirrored back onto the entity, got %f", (f64)entity->velocity.y);

    // the mirrored velocity is a report. integrating it again would draw the body a tick ahead of the solver.
    f32 solved = entity->position.y;
    nya_system_entity_update(TICK);
    nya_assert(entity->position.y == solved, "the entity update left the solver's position, got %f for %f", (f64)entity->position.y, (f64)solved);

    step(300);

    // Resting on a one metre box whose top is at y = 0, so its centre sits at half its own height.
    nya_assert(fabsf(entity->position.y - 0.5F) < 0.1F, "it came to rest on the floor, got %f", (f64)entity->position.y);
    nya_assert(nya_physics3d_grounded(entity), "and reports standing on something");

    // asking twice in one tick costs one contact query; the answer is remembered for that step.
    nya_assert(nya_physics3d_grounded(entity), "the cached answer agrees");

    nya_entity_despawn(box);
    nya_entity_despawn(floor);

    nya_assert(nya_physics3d_body_count() == 0, "despawning destroys the body with the entity");

    printf("  PASSED\n");
  }

  // TEST: rotation comes back as a whole quaternion
  {
    NYA_EntityHandle box = nya_entity_spawn(.name = "spinner", .position = { 0.0F, 10.0F, 0.0F });
    nya_assert(nya_physics3d_body_attach(box, .shape = NYA_PHYSICS3D_SHAPE_BOX, .size = { 1.0F, 1.0F, 1.0F }, .gravity_scale = 0.0F));

    NYA_Entity* entity = nya_entity_get(box);

    // about x and z at once, which a 2D body with one angular degree of freedom cannot do. That is why there is no float returning nya_physics3d_rotation.
    nya_physics3d_angular_velocity_set(entity, (f32x3){ 2.0F, 0.0F, 3.0F });

    step(30);

    f32x3 angular = nya_physics3d_angular_velocity(entity);
    nya_assert(angular.x > 0.5F && angular.z > 0.5F, "it is turning about two axes, got (%f, %f)", (f64)angular.x, (f64)angular.z);

    // moved off the identity and still unit, so the readback does not leak an unnormalised value.
    NYA_Quaternion rotation = entity->rotation;
    nya_assert(fabsf(rotation.w) < 0.999F, "the rotation left the identity, w = %f", (f64)rotation.w);

    f32 length_squared = (rotation.x * rotation.x) + (rotation.y * rotation.y) + (rotation.z * rotation.z) + (rotation.w * rotation.w);
    nya_assert(fabsf(length_squared - 1.0F) < 0.01F, "and is still unit, got %f", (f64)length_squared);

    // Teleport writes the entity immediately rather than waiting for the next step, so anything reading between now and then sees where the entity actually is.
    nya_physics3d_teleport(entity, (f32x3){ 3.0F, 4.0F, 5.0F }, nya_quaternion_identity);

    nya_assert(entity->position.x == 3.0F && entity->position.y == 4.0F && entity->position.z == 5.0F, "the teleport landed");
    nya_assert(entity->rotation.w == 1.0F, "and reset the rotation");

    nya_entity_despawn(box);

    printf("  PASSED\n");
  }

  // TEST: forces and impulses
  {
    NYA_EntityHandle box = nya_entity_spawn(.name = "pushed", .position = { 0.0F, 10.0F, 0.0F });
    nya_assert(nya_physics3d_body_attach(box, .shape = NYA_PHYSICS3D_SHAPE_SPHERE, .radius = 0.5F, .gravity_scale = 0.0F));

    NYA_Entity* entity = nya_entity_get(box);

    nya_physics3d_apply_impulse(entity, (f32x3){ 100.0F, 0.0F, 0.0F });
    step(1);

    nya_assert(nya_physics3d_velocity(entity).x > 0.0F, "an impulse moves it, got %f", (f64)nya_physics3d_velocity(entity).x);

    nya_physics3d_velocity_set(entity, f32x3_zero);
    step(1);
    nya_assert(fabsf(nya_physics3d_velocity(entity).x) < 0.01F, "setting the velocity stops it");

    nya_physics3d_apply_force(entity, (f32x3){ 0.0F, 0.0F, 500.0F });
    step(1);
    nya_assert(nya_physics3d_velocity(entity).z > 0.0F, "a force accelerates it");

    nya_assert(nya_physics3d_awake(entity), "it is awake while it is moving");

    // Detaching leaves the entity in the world and hands the transform back to its own integration.
    nya_physics3d_body_detach(box);
    nya_assert(!nya_physics3d_body_attached(entity), "the body is gone");
    nya_assert(nya_entity_is_valid(box), "and the entity is not");
    nya_assert(!nya_physics3d_grounded(entity), "an entity with no 3D body is not standing on anything");

    nya_entity_despawn(box);

    printf("  PASSED\n");
  }

  // TEST: a pickup is a sensor, and its overlap arrives through on_collision
  {
    sensor_enters = 0;
    sensor_exits  = 0;
    sensor_other  = NYA_ENTITY_HANDLE_NONE;

    NYA_EntityHandle coin = nya_entity_spawn(.name = "coin", .position = { 0.0F, 0.0F, 0.0F }, .on_collision = nya_callback(record_collision));
    nya_assert(nya_physics3d_body_attach(
      coin, .type = NYA_PHYSICS_BODY_STATIC, .shape = NYA_PHYSICS3D_SHAPE_BOX, .size = { 2.0F, 2.0F, 2.0F }, .is_sensor = true
    ));

    /* The player is an ordinary dynamic body with nothing sensor-shaped about it. */
    NYA_EntityHandle player = nya_entity_spawn(.name = "player", .position = { 0.0F, 6.0F, 0.0F });
    nya_assert(nya_physics3d_body_attach(player, .shape = NYA_PHYSICS3D_SHAPE_BOX, .size = { 0.5F, 0.5F, 0.5F }));

    // Falls straight through the sensor, which is what makes this test both an entry and an exit.
    step(180);

    nya_assert(sensor_enters == 1, "falling through a sensor is exactly one enter, got " FMTu32, sensor_enters);
    nya_assert(sensor_exits == 1, "and exactly one exit, got " FMTu32, sensor_exits);
    nya_assert(sensor_other.index == player.index, "the sensor's callback is handed the visitor, not itself");

    nya_entity_despawn(player);
    nya_entity_despawn(coin);

    printf("  PASSED\n");
  }

  // TEST: a hard landing produces an impact, and a paused world produces nothing
  {
    impacts = 0;

    NYA_EntityHandle floor = nya_entity_spawn(.name = "floor", .position = { 0.0F, -0.5F, 0.0F });
    nya_assert(nya_physics3d_body_attach(floor, .type = NYA_PHYSICS_BODY_STATIC, .shape = NYA_PHYSICS3D_SHAPE_BOX, .size = { 20.0F, 1.0F, 20.0F }));

    // dropped from well above the threshold of four metres per second, about a metre's fall.
    NYA_EntityHandle box = nya_entity_spawn(.name = "dropped", .position = { 0.0F, 8.0F, 0.0F }, .on_collision = nya_callback(record_collision));
    nya_assert(nya_physics3d_body_attach(box, .shape = NYA_PHYSICS3D_SHAPE_BOX, .size = { 1.0F, 1.0F, 1.0F }, .density = 500.0F));

    step(180);

    nya_assert(impacts > 0, "landing hard enough registers an impact");

    // Frozen rather than unwound: bodies keep their state and the hit list goes quiet, which is what makes a pause not produce the same impact over and over.
    nya_physics3d_enabled_set(false);

    u32                   count = 0;
    const NYA_PhysicsHit* hits  = nya_physics3d_hits(&count);

    step(10);

    hits = nya_physics3d_hits(&count);
    nya_assert(hits != nullptr, "the list is never null");
    nya_assert(count == 0, "a paused world produces no hits, got " FMTu32, count);

    nya_physics3d_enabled_set(true);

    nya_entity_despawn(box);
    nya_entity_despawn(floor);

    printf("  PASSED\n");
  }

  // TEST: a kinematic body throws what it hits when it is given a velocity, and does not when it is teleported along the same path
  {
    /* A pinball flipper, reduced to the one thing that makes it a flipper. Both paddles travel the same distance at the same speed; the only difference is which call moves them. A teleport sets the transform without a sweep, so the solver reads a paddle that never moved and there is no relative velocity for the contact to work with. Gravity off, so what the ball is carrying afterwards came from the paddle and nowhere else. */
    const f32x3 original_gravity = nya_physics3d_gravity();
    nya_physics3d_gravity_set(f32x3_zero);

    const f32 paddle_speed = 6.0F;

    f32 thrown[2] = { 0 };

    for (u32 by_velocity = 0; by_velocity < 2; by_velocity++) {
      NYA_EntityHandle ball = nya_entity_spawn(.name = "ball", .position = { 0.0F, 0.0F, 0.0F });
      nya_assert(nya_physics3d_body_attach(ball, .shape = NYA_PHYSICS3D_SHAPE_SPHERE, .radius = 0.2F, .restitution = 0.35F));

      NYA_EntityHandle paddle = nya_entity_spawn(.name = "paddle", .position = { -1.0F, 0.0F, 0.0F });
      nya_assert(nya_physics3d_body_attach(paddle, .type = NYA_PHYSICS_BODY_KINEMATIC, .shape = NYA_PHYSICS3D_SHAPE_BOX,
                                           .size = { 0.4F, 0.4F, 0.4F }, .restitution = 0.35F));

      // Up to and a little past the ball, one tick at a time, so the contact happens mid travel.
      for (u32 i = 0; i < 20; i++) {
        NYA_Entity* entity = nya_entity_get(paddle);

        if (by_velocity != 0) {
          nya_physics3d_velocity_set(entity, (f32x3){ paddle_speed, 0.0F, 0.0F });
        } else {
          nya_physics3d_teleport(entity, (f32x3){ entity->position.x + paddle_speed * TICK, 0.0F, 0.0F }, entity->rotation);
        }

        step(1);
      }

      thrown[by_velocity] = nya_physics3d_velocity(nya_entity_get(ball)).x;

      nya_entity_despawn(paddle);
      nya_entity_despawn(ball);
      step(1); // so the despawns are out of the world before the next paddle is built
    }

    nya_physics3d_gravity_set(original_gravity);

    // The ball leaves at least as fast as the paddle came in, which is the transfer the contact is there to make. Restitution puts it above that rather than below.
    nya_assert(thrown[1] >= paddle_speed, "a swept paddle throws the ball, got %f against a paddle at %f", (f64)thrown[1], (f64)paddle_speed);

    // And the teleported paddle does not move it at all: it travels the same distance at the same speed and the ball is left standing exactly where it was.
    nya_assert(thrown[0] == 0.0F, "a teleported paddle does not throw it, got %f", (f64)thrown[0]);

    printf("  PASSED\n");
  }

  // TEST: a settled body sleeps through a teleport, and waking it lets it fall
  {
    NYA_EntityHandle floor = nya_entity_spawn(.name = "floor", .position = { 0.0F, -0.5F, 0.0F });
    nya_assert(nya_physics3d_body_attach(floor, .type = NYA_PHYSICS_BODY_STATIC, .shape = NYA_PHYSICS3D_SHAPE_BOX, .size = { 20.0F, 1.0F, 20.0F }));

    NYA_EntityHandle box = nya_entity_spawn(.name = "sleeper", .position = { 0.0F, 0.5F, 0.0F });
    nya_assert(nya_physics3d_body_attach(box, .shape = NYA_PHYSICS3D_SHAPE_BOX, .size = { 1.0F, 1.0F, 1.0F }));

    NYA_Entity* entity = nya_entity_get(box);

    // five seconds resting on the floor, far past the half second the solver waits before it sleeps a body.
    step(300);
    nya_assert(!nya_physics3d_awake(entity), "a body at rest is put to sleep");

    // the step time is what the solver took, so it reads zero only while nothing has been stepped.
    nya_assert(nya_physics3d_last_step_time_s() > 0.0F, "the last step was timed");
    nya_assert(nya_physics3d_last_step_time_s() < 1.0F, "in seconds, got %f", (f64)nya_physics3d_last_step_time_s());

    // what a reset in the 3D scene does: moved up with no velocity, and nothing about that wakes it.
    nya_physics3d_teleport(entity, (f32x3){ 0.0F, 5.0F, 0.0F }, nya_quaternion_identity);
    nya_physics3d_velocity_set(entity, f32x3_zero);
    step(30);

    nya_assert(!nya_physics3d_awake(entity), "a teleport leaves it asleep");
    nya_assert(entity->position.y == 5.0F, "so it hangs where it was put, got %f", (f64)entity->position.y);

    nya_physics3d_wake(entity);
    nya_assert(nya_physics3d_awake(entity), "waking it wakes it");

    step(30);
    nya_assert(entity->position.y < 5.0F, "and it falls, to %f", (f64)entity->position.y);

    nya_entity_despawn(box);
    nya_entity_despawn(floor);

    printf("  PASSED\n");
  }

  // TEST: ten units to the metre make the same fall ten times as long
  {
    const f32x3 original_gravity = nya_physics3d_gravity();
    const f32   scales[2]        = { 1.0F, 10.0F };

    f32 fallen[2] = { 0 };

    for (u32 run = 0; run < 2; run++) {
      /* Earth's gravity in world units, set before the scale on purpose: the solver was given it through the old scale, so it reaches 9.81 metres per second squared only if changing the scale converts it again. */
      nya_physics3d_gravity_set((f32x3){ 0.0F, -9.81F * scales[run], 0.0F });
      nya_physics3d_units_per_meter_set(scales[run]);

      nya_assert(nya_physics3d_units_per_meter() == scales[run], "the scale reads back, got %f", (f64)nya_physics3d_units_per_meter());

      NYA_EntityHandle ball = nya_entity_spawn(.name = "dropped", .position = { 0.0F, 0.0F, 0.0F });
      nya_assert(nya_physics3d_body_attach(ball, .shape = NYA_PHYSICS3D_SHAPE_SPHERE, .radius = 0.5F * scales[run]));

      step(30);
      fallen[run] = -nya_entity_get(ball)->position.y;

      nya_entity_despawn(ball);
    }

    nya_physics3d_units_per_meter_set(1.0F);
    nya_physics3d_gravity_set(original_gravity);

    // half a second of free fall is about 1.2 metres, in whatever units a metre is.
    nya_assert(fallen[0] > 1.0F && fallen[0] < 1.5F, "a metre a unit falls about 1.2 units, got %f", (f64)fallen[0]);
    nya_assert(fabsf(fallen[1] - (fallen[0] * scales[1])) < fallen[0] * scales[1] * 0.01F, "ten to the metre falls ten times as far, got %f for %f",
               (f64)fallen[1], (f64)fallen[0]);

    printf("  PASSED\n");
  }

  // TEST: malformed bodies are refused rather than half built
  {
    NYA_EntityHandle bad = nya_entity_spawn(.name = "malformed");

    nya_assert(!nya_physics3d_body_attach(bad, .shape = NYA_PHYSICS3D_SHAPE_BOX, .size = { 0.0F, 1.0F, 1.0F }), "a box with no width is refused");
    nya_assert(!nya_physics3d_body_attach(bad, .shape = NYA_PHYSICS3D_SHAPE_SPHERE, .radius = 0.0F), "a sphere with no radius is refused");
    nya_assert(!nya_physics3d_body_attach(bad, .shape = NYA_PHYSICS3D_SHAPE_CAPSULE, .radius = 1.0F, .length = 0.0F), "a capsule with no length is refused");

    // a rejected attach leaves nothing: the body is destroyed rather than left shapeless to fall forever.
    nya_assert(nya_physics3d_body_count() == 0, "and none of them left a body");
    nya_assert(!nya_physics3d_body_attached(nya_entity_get(bad)), "nor an attachment");

    // A capsule is the one shape with two dimensions to get right, so check the good case too.
    nya_assert(nya_physics3d_body_attach(bad, .shape = NYA_PHYSICS3D_SHAPE_CAPSULE, .radius = 0.3F, .length = 1.0F), "a valid capsule attaches");
    nya_assert(nya_physics3d_body_count() == 1, "and is counted");

    // Attaching twice is refused rather than leaking the first body.
    nya_assert(!nya_physics3d_body_attach(bad, .shape = NYA_PHYSICS3D_SHAPE_SPHERE, .radius = 1.0F), "a second body is refused");
    nya_assert(nya_physics3d_body_count() == 1, "and changes nothing");

    nya_entity_despawn(bad);

    printf("  PASSED\n");
  }

  nya_log_info("PASSED: test_physics3d");

  return EXIT_SUCCESS;
}
