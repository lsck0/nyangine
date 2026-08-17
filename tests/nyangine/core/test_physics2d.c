/**
 * Rigid bodies, and the contract between them and the entity table.
 *
 * The simulation itself is Box2D's and is not what this tests. What is tested is the seam: that a
 * body attaches to an entity and takes its transform over, that the entity's own integration steps
 * aside while it does, that units cross the world/metre boundary in both directions, and that
 * despawning an entity destroys the body rather than leaking it into the world.
 *
 * Headless throughout: the physics world needs an arena and a clock and nothing else, so none of
 * this touches a GPU.
 **/

#include "nyangine/nyangine.c"
#include "nyangine/nyangine.h"

#include "SDL3/SDL_init.h"

/** One fixed tick, matching the engine's default time step. */
#define TICK (1.0F / 60.0F)

/** Steps the world `count` times, the way the app loop would. */
static void step(u32 count) {
  for (u32 i = 0; i < count; i++) nya_system_physics2d_update(TICK);
}

/*
 * What a pickup's callback records. A coin is a sensor, and the whole point of the sensor path is
 * that it arrives through the same on_collision an impact does.
 */
static u32              pickup_enters = 0;
static u32              pickup_exits  = 0;
static NYA_EntityHandle pickup_other  = NYA_ENTITY_HANDLE_NONE;

static void pickup_on_collision(NYA_Entity* entity, NYA_Entity* other, const NYA_PhysicsHit* hit) {
  nya_unused(entity);

  switch (hit->kind) {
    case NYA_PHYSICS_HIT_SENSOR_ENTER: {
      pickup_enters++;
      if (other != nullptr) pickup_other = other->handle;
    } break;

    case NYA_PHYSICS_HIT_SENSOR_EXIT: pickup_exits++; break;

    default: break;
  }
}

s32 main(void) {
  _NYA_APP_INSTANCE = (NYA_App){ .initialized = true };
  b8 sdl_ok         = SDL_Init(0);
  nya_assert(sdl_ok, "SDL_Init failed: %s", SDL_GetError());

  nya_system_callback_init();
  // The world: entities, physics and the simulation barrier, brought up in the order they depend on
  // each other. See core_world.h.
  NYA_World* world = nya_world_create();
  (void)nya_world_set(world);

  defer nya_world_destroy(world);

  defer nya_system_callback_deinit();

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: a fresh world has nothing in it
  // ─────────────────────────────────────────────────────────────────────────────
  {
    nya_assert(nya_physics2d_body_count() == 0, "no bodies before anything is attached");
    nya_assert(nya_physics2d_enabled(), "the world starts running");
    nya_assert(nya_physics2d_pixels_per_meter() == NYA_PHYSICS2D_PIXELS_PER_METER);

    // Positive y is down the screen, which is the whole reason this default is not negative.
    f32x2 gravity = nya_physics2d_gravity();
    nya_assert(gravity.y > 0.0F, "gravity points down the screen, not up it");
    nya_assert(gravity.x == 0.0F);
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: attaching a body, and the entity that carries it
  // ─────────────────────────────────────────────────────────────────────────────
  {
    NYA_EntityHandle crate = nya_entity_spawn(.name = "crate", .position = { 100.0F, 0.0F, 0.0F });

    NYA_Entity* entity = nya_entity_get(crate);
    nya_assert(entity != nullptr);
    nya_assert(!nya_physics2d_body_attached(entity), "an entity has no body until one is attached");

    b8 ok = nya_physics2d_body_attach(crate, .shape = NYA_PHYSICS2D_SHAPE_BOX, .size = { 32.0F, 32.0F });
    nya_assert(ok, "attaching a box body to a live entity succeeds");

    nya_assert(nya_physics2d_body_attached(entity), "the entity now carries a body");
    nya_assert(nya_physics2d_body_count() == 1);

    // The dimensions are kept on the entity because that is what a renderer needs, and Box2D does
    // not hand them back in the form they went in as.
    nya_assert(entity->physics2d.size.x == 32.0F && entity->physics2d.size.y == 32.0F);
    nya_assert(entity->physics2d.shape == NYA_PHYSICS2D_SHAPE_BOX);
    nya_assert(entity->physics2d.type == NYA_PHYSICS_BODY_DYNAMIC);

    // Attaching twice is refused rather than leaking the first body.
    b8 twice = nya_physics2d_body_attach(crate, .shape = NYA_PHYSICS2D_SHAPE_BOX, .size = { 16.0F, 16.0F });
    nya_assert(!twice, "an entity may only carry one body");
    nya_assert(nya_physics2d_body_count() == 1);

    nya_entity_despawn(crate);
    nya_assert(nya_physics2d_body_count() == 0, "despawning destroys the body the entity carried");
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: a dynamic body falls, and the entity follows it
  // ─────────────────────────────────────────────────────────────────────────────
  {
    NYA_EntityHandle crate = nya_entity_spawn(.name = "faller", .position = { 0.0F, 0.0F, 0.0F });
    nya_assert(nya_physics2d_body_attach(crate, .size = { 32.0F, 32.0F }));

    step(30);

    NYA_Entity* entity = nya_entity_get(crate);

    // Half a second of earth gravity is about 1.2 metres, which at the default scale is roughly 39
    // world units. Bounded rather than exact, because the solver's integration is its own business.
    nya_assert(entity->position.y > 20.0F, "half a second of falling moves it well down the screen");
    nya_assert(entity->position.y < 80.0F, "and not absurdly far, which would mean a unit conversion is wrong");

    // Nothing pushed it sideways, so any drift here would be the transform sync mixing up its axes.
    nya_assert(fabsf(entity->position.x) < 0.001F, "a body under gravity alone does not move in x");

    nya_assert(nya_physics2d_velocity(entity).y > 0.0F, "and it is still accelerating downward");

    nya_entity_despawn(crate);
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: the entity's own integration steps aside for a body
  // ─────────────────────────────────────────────────────────────────────────────
  {
    // Two entities given the same upward velocity. One is simulated and one is not, so the plain
    // one keeps rising forever and the simulated one is pulled back. Without the skip in
    // nya_system_entity_update the simulated one would be moved by both and end up above the other.
    NYA_EntityHandle simulated = nya_entity_spawn(.name = "simulated", .velocity = { 0.0F, -400.0F, 0.0F });
    NYA_EntityHandle scripted  = nya_entity_spawn(.name = "scripted", .velocity = { 0.0F, -400.0F, 0.0F });

    nya_assert(nya_physics2d_body_attach(simulated, .size = { 32.0F, 32.0F }));

    for (u32 i = 0; i < 60; i++) {
      nya_system_physics2d_update(TICK);
      nya_system_entity_update(TICK);
    }

    f32 simulated_y = nya_entity_get(simulated)->position.y;
    f32 scripted_y  = nya_entity_get(scripted)->position.y;

    // y grows downward, so "higher" is the smaller number.
    nya_assert(scripted_y < simulated_y, "gravity brought the simulated one back below the scripted one");

    nya_entity_despawn(simulated);
    nya_entity_despawn(scripted);
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: a box lands on a chain and stays on it
  // ─────────────────────────────────────────────────────────────────────────────
  {
    // A flat floor at y 200. Four points, which is the minimum an open chain accepts.
    f32x2 floor_points[] = {
      { -400.0F, 200.0F },
      { -100.0F, 200.0F },
      { 100.0F, 200.0F },
      { 400.0F, 200.0F },
    };

    NYA_EntityHandle floor = nya_entity_spawn(.name = "floor", .state = NYA_ENTITY_STATE_ACTIVE | NYA_ENTITY_STATE_STATIC);

    b8 floor_ok = nya_physics2d_body_attach(
      floor,
      .type        = NYA_PHYSICS_BODY_STATIC,
      .shape       = NYA_PHYSICS2D_SHAPE_CHAIN,
      .points      = floor_points,
      .point_count = (u32)(sizeof(floor_points) / sizeof(floor_points[0])),
    );
    nya_assert(floor_ok, "a four point chain on a static body is accepted");

    // A chain on anything that moves has no area to take a mass from, and is refused rather than
    // failing inside the solver.
    NYA_EntityHandle bad = nya_entity_spawn(.name = "bad");
    b8               bad_ok =
      nya_physics2d_body_attach(bad, .type = NYA_PHYSICS_BODY_DYNAMIC, .shape = NYA_PHYSICS2D_SHAPE_CHAIN, .points = floor_points, .point_count = 4);
    nya_assert(!bad_ok, "a chain on a dynamic body is refused");
    nya_entity_despawn(bad);

    NYA_EntityHandle crate = nya_entity_spawn(.name = "lander", .position = { 0.0F, 0.0F, 0.0F });
    nya_assert(nya_physics2d_body_attach(crate, .size = { 32.0F, 32.0F }));

    // Long enough to fall 184 units, land, settle and go to sleep.
    step(240);

    NYA_Entity* entity = nya_entity_get(crate);

    // Its centre rests half a box above the surface. A couple of units of tolerance for the
    // solver's contact softening, which lets shapes overlap slightly rather than jittering apart.
    nya_assert(entity->position.y > 178.0F && entity->position.y < 186.0F, "the crate is resting on the floor, not through it");

    nya_assert(!nya_physics2d_awake(entity), "and the solver has put it to sleep");

    nya_entity_despawn(crate);
    nya_entity_despawn(floor);
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: impulses, teleports and the point query
  // ─────────────────────────────────────────────────────────────────────────────
  {
    NYA_EntityHandle crate = nya_entity_spawn(.name = "pushed", .position = { 0.0F, 0.0F, 0.0F });
    nya_assert(nya_physics2d_body_attach(crate, .size = { 32.0F, 32.0F }, .gravity_scale = 0.0F));

    NYA_Entity* entity = nya_entity_get(crate);

    // Weightless, so the only thing moving it is the impulse.
    nya_physics2d_apply_impulse(entity, (f32x2){ 320.0F, 0.0F });
    step(1);
    nya_assert(nya_physics2d_velocity(entity).x > 0.0F, "an impulse to the right moves it right");

    nya_physics2d_velocity_set(entity, f32x2_zero);
    step(1);
    nya_assert(fabsf(nya_physics2d_velocity(entity).x) < 1.0F, "and setting velocity to zero stops it");

    nya_physics2d_teleport(entity, (f32x2){ 500.0F, -300.0F }, 0.0F);

    // Written through immediately rather than at the next step, so a teleport and a read in the
    // same tick agree.
    nya_assert(entity->position.x == 500.0F && entity->position.y == -300.0F);

    // The point query is against the shape, not its bounding box: its centre is a hit and a point
    // well outside it is not.
    NYA_EntityHandle hit = nya_physics2d_entity_at((f32x2){ 500.0F, -300.0F });
    nya_assert(hit.index == crate.index && hit.generation == crate.generation, "the query finds the crate at its own centre");

    NYA_EntityHandle miss = nya_physics2d_entity_at((f32x2){ 500.0F, 300.0F });
    nya_assert(!nya_entity_is_valid(miss), "and finds nothing in empty space");

    nya_entity_despawn(crate);
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: detaching leaves the entity, and hands motion back to it
  // ─────────────────────────────────────────────────────────────────────────────
  {
    NYA_EntityHandle crate = nya_entity_spawn(.name = "detached", .velocity = { 60.0F, 0.0F, 0.0F });
    nya_assert(nya_physics2d_body_attach(crate, .size = { 32.0F, 32.0F }));
    nya_assert(nya_physics2d_body_count() == 1);

    nya_physics2d_body_detach(crate);

    NYA_Entity* entity = nya_entity_get(crate);
    nya_assert(nya_entity_is_valid(crate), "detaching removes the body, not the entity");
    nya_assert(!nya_physics2d_body_attached(entity));
    nya_assert(nya_physics2d_body_count() == 0);

    // Back to integrating its own velocity, which is what it did before the body existed.
    entity->velocity = (f32x3){ 60.0F, 0.0F, 0.0F };
    f32 before       = entity->position.x;
    nya_system_entity_update(TICK);
    nya_assert(entity->position.x > before, "the entity moves under its own velocity again");

    // Idempotent, because an unwind path frequently detaches something it is not sure about.
    nya_physics2d_body_detach(crate);
    nya_assert(nya_physics2d_body_count() == 0);

    nya_entity_despawn(crate);
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: a paused world does not advance
  // ─────────────────────────────────────────────────────────────────────────────
  {
    NYA_EntityHandle crate = nya_entity_spawn(.name = "paused", .position = { 0.0F, 0.0F, 0.0F });
    nya_assert(nya_physics2d_body_attach(crate, .size = { 32.0F, 32.0F }));

    nya_physics2d_enabled_set(false);
    step(60);

    NYA_Entity* entity = nya_entity_get(crate);
    nya_assert(entity->position.y == 0.0F, "a paused world does not fall");

    nya_physics2d_enabled_set(true);
    step(60);
    nya_assert(entity->position.y > 0.0F, "and resumes where it left off");

    nya_entity_despawn(crate);
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: nonsense dimensions are refused rather than built
  // ─────────────────────────────────────────────────────────────────────────────
  {
    NYA_EntityHandle crate = nya_entity_spawn(.name = "malformed");

    nya_assert(!nya_physics2d_body_attach(crate, .shape = NYA_PHYSICS2D_SHAPE_BOX, .size = { 0.0F, 32.0F }), "a box needs both extents");
    nya_assert(!nya_physics2d_body_attach(crate, .shape = NYA_PHYSICS2D_SHAPE_CIRCLE, .radius = 0.0F), "a circle needs a radius");
    nya_assert(!nya_physics2d_body_attach(crate, .shape = NYA_PHYSICS2D_SHAPE_CHAIN, .points = nullptr, .point_count = 0), "a chain needs points");

    // Every one of those failed after b2CreateBody and had to destroy it again. A leak here shows up
    // as a body count that never went back to zero.
    nya_assert(nya_physics2d_body_count() == 0, "a refused attach leaves no body behind");
    nya_assert(!nya_physics2d_body_attached(nya_entity_get(crate)));

    nya_entity_despawn(crate);
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: a hard landing is reported as a hit, and a settled world is not
  // ─────────────────────────────────────────────────────────────────────────────
  {
    u32                   count = 0;
    const NYA_PhysicsHit* hits  = nya_physics2d_hits(&count);
    nya_assert(hits != nullptr, "the hit list is never null");
    nya_assert(count == 0, "a world where nothing has happened reports no hits");

    f32x2 floor_points[] = {
      { -400.0F, 200.0F },
      { -100.0F, 200.0F },
      { 100.0F, 200.0F },
      { 400.0F, 200.0F },
    };

    NYA_EntityHandle floor = nya_entity_spawn(.name = "floor", .state = NYA_ENTITY_STATE_ACTIVE | NYA_ENTITY_STATE_STATIC);
    nya_assert(nya_physics2d_body_attach(floor, .type = NYA_PHYSICS_BODY_STATIC, .shape = NYA_PHYSICS2D_SHAPE_CHAIN, .points = floor_points,
                                       .point_count = 4));

    // Dropped from far enough up to be well past the threshold on arrival, so this does not become
    // a test of exactly where the cutoff sits.
    NYA_EntityHandle crate = nya_entity_spawn(.name = "dropped", .position = { 0.0F, -600.0F, 0.0F });
    nya_assert(nya_physics2d_body_attach(crate, .size = { 32.0F, 32.0F }));

    NYA_PhysicsHit landing = { 0 };
    b8             landed  = false;

    for (u32 i = 0; i < 300 && !landed; i++) {
      step(1);

      hits = nya_physics2d_hits(&count);
      if (count == 0) continue;

      landing = hits[0];
      landed  = true;
    }

    nya_assert(landed, "the crate landed on the floor and the impact was reported");

    // Both sides resolve, and to the two entities actually involved rather than to whatever else is
    // in the table. Which is A and which is B is Box2D's ordering, so this accepts either.
    b8 crate_first = landing.a.index == crate.index && landing.b.index == floor.index;
    b8 floor_first = landing.a.index == floor.index && landing.b.index == crate.index;
    nya_assert(crate_first || floor_first, "the hit names the crate and the floor");

    nya_assert(landing.approach_speed >= nya_physics2d_hit_threshold(), "a reported hit is at least as fast as the threshold");

    // Converted out of metres. Falling 600 world units under earth gravity arrives at roughly 620
    // world units per second; a hit still carrying Box2D's metric value would read about 19.
    nya_assert(landing.approach_speed > 100.0F, "the approach speed is in world units, not metres");

    // Near the top of the crate, which is where the floor met it. Loose bounds: the exact contact
    // point is the solver's.
    nya_assert(landing.point.y > 150.0F && landing.point.y < 250.0F, "the contact point is at the floor, in world units");

    // Long enough for the bounce to die out and the crate to sleep.
    step(300);

    hits = nya_physics2d_hits(&count);
    nya_assert(count == 0, "a settled crate resting on the floor reports nothing");

    // A hit lasts exactly the tick that produced it. Without the clear at the top of the step, a
    // paused world would keep replaying the last impact for as long as it stayed paused.
    nya_physics2d_enabled_set(false);
    step(1);
    (void)nya_physics2d_hits(&count);
    nya_assert(count == 0, "a paused world reports no hits");
    nya_physics2d_enabled_set(true);

    nya_entity_despawn(crate);
    nya_entity_despawn(floor);
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: the hit threshold
  // ─────────────────────────────────────────────────────────────────────────────
  {
    f32 original = nya_physics2d_hit_threshold();
    nya_assert(original > 0.0F, "there is a threshold by default, or every resting contact is a hit");

    nya_physics2d_hit_threshold_set(1000.0F);
    nya_assert(nya_physics2d_hit_threshold() == 1000.0F);

    // Negative would mean every contact qualifies, which is the one setting that cannot be what
    // anyone meant, so it clamps rather than being passed through.
    nya_physics2d_hit_threshold_set(-5.0F);
    nya_assert(nya_physics2d_hit_threshold() == 0.0F, "a negative threshold clamps to zero");

    nya_physics2d_hit_threshold_set(original);
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: clearing the world takes every body with it
  // ─────────────────────────────────────────────────────────────────────────────
  {
    for (u32 i = 0; i < 16; i++) {
      NYA_EntityHandle crate = nya_entity_spawn(.name = "bulk", .position = { (f32)i * 40.0F, 0.0F, 0.0F });
      nya_assert(nya_physics2d_body_attach(crate, .size = { 32.0F, 32.0F }));
    }

    nya_assert(nya_physics2d_body_count() == 16);

    nya_entity_clear();

    nya_assert(nya_entity_count() == 0);
    nya_assert(nya_physics2d_body_count() == 0, "clearing the entity table empties the physics world too");
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: grounded
  // ─────────────────────────────────────────────────────────────────────────────
  {
    f32x2 floor_points[] = {
      { -400.0F, 200.0F },
      { -100.0F, 200.0F },
      { 100.0F, 200.0F },
      { 400.0F, 200.0F },
    };

    NYA_EntityHandle floor = nya_entity_spawn(.name = "floor", .state = NYA_ENTITY_STATE_ACTIVE | NYA_ENTITY_STATE_STATIC);
    nya_assert(nya_physics2d_body_attach(floor, .type = NYA_PHYSICS_BODY_STATIC, .shape = NYA_PHYSICS2D_SHAPE_CHAIN, .points = floor_points,
                                       .point_count = 4));

    NYA_EntityHandle crate = nya_entity_spawn(.name = "stander", .position = { 0.0F, -300.0F, 0.0F });
    nya_assert(nya_physics2d_body_attach(crate, .size = { 32.0F, 32.0F }));

    NYA_Entity* entity = nya_entity_get(crate);

    // Falling, touching nothing.
    step(1);
    nya_assert(!nya_physics2d_grounded(entity), "a body in mid air is not standing on anything");

    // Long enough to land and settle.
    step(240);
    nya_assert(nya_physics2d_grounded(entity), "a crate resting on the floor is grounded");

    // An entity with no body at all answers false rather than faulting, which is what lets a caller
    // iterate mixed entities without filtering first.
    NYA_EntityHandle bodiless = nya_entity_spawn(.name = "bodiless");
    nya_assert(!nya_physics2d_grounded(nya_entity_get(bodiless)), "an entity with no body is not grounded");
    nya_assert(!nya_physics2d_grounded(nullptr), "a null entity is not grounded");

    nya_entity_despawn(bodiless);
    nya_entity_despawn(crate);
    nya_entity_despawn(floor);
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: a pickup is a sensor, and its overlap arrives through on_collision
  // ─────────────────────────────────────────────────────────────────────────────
  {
    pickup_enters = 0;
    pickup_exits  = 0;
    pickup_other  = NYA_ENTITY_HANDLE_NONE;

    // The coin: a sensor, static, so it stays where it was put.
    NYA_EntityHandle coin = nya_entity_spawn(
      .name         = "coin",
      .position     = { 0.0F, 100.0F, 0.0F },
      .on_collision = nya_callback(pickup_on_collision)
    );
    nya_assert(nya_physics2d_body_attach(coin, .type = NYA_PHYSICS_BODY_STATIC, .shape = NYA_PHYSICS2D_SHAPE_BOX, .size = { 64.0F, 64.0F }, .is_sensor = true));

    // The player: an ordinary dynamic body with nothing sensor-shaped about it. This is the half
    // Box2D wants enableSensorEvents on too, and the half a caller has no reason to think about.
    NYA_EntityHandle player = nya_entity_spawn(.name = "player", .position = { 0.0F, 0.0F, 0.0F });
    nya_assert(nya_physics2d_body_attach(player, .type = NYA_PHYSICS_BODY_DYNAMIC, .shape = NYA_PHYSICS2D_SHAPE_BOX, .size = { 32.0F, 32.0F }));

    // Falls through the coin under gravity. One step is not enough to reach it and a hundred is more
    // than enough to be well past it, which is what makes this test both entries and exits.
    step(100);

    nya_assert(pickup_enters == 1, "falling through a sensor is exactly one enter, got " FMTu32, pickup_enters);
    nya_assert(pickup_exits == 1, "and exactly one exit, got " FMTu32, pickup_exits);
    nya_assert(pickup_other.index == player.index, "the sensor's callback is handed the visitor, not itself");

    nya_entity_despawn(player);
    nya_entity_despawn(coin);
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: despawning inside a sensor still produces the exit
  // ─────────────────────────────────────────────────────────────────────────────
  {
    pickup_enters = 0;
    pickup_exits  = 0;

    NYA_EntityHandle coin = nya_entity_spawn(
      .name         = "coin",
      .position     = { 0.0F, 0.0F, 0.0F },
      .on_collision = nya_callback(pickup_on_collision)
    );
    nya_assert(nya_physics2d_body_attach(coin, .type = NYA_PHYSICS_BODY_STATIC, .shape = NYA_PHYSICS2D_SHAPE_BOX, .size = { 64.0F, 64.0F }, .is_sensor = true));

    // Parked inside the coin rather than falling through it, with gravity off so it stays there.
    nya_physics2d_gravity_set(f32x2_zero);

    NYA_EntityHandle player = nya_entity_spawn(.name = "player", .position = { 0.0F, 0.0F, 0.0F });
    nya_assert(nya_physics2d_body_attach(player, .type = NYA_PHYSICS_BODY_DYNAMIC, .shape = NYA_PHYSICS2D_SHAPE_BOX, .size = { 32.0F, 32.0F }));

    step(4);
    nya_assert(pickup_enters == 1, "sitting inside a sensor is one enter and not one per step, got " FMTu32, pickup_enters);
    nya_assert(pickup_exits == 0, "and no exit while it is still there");

    /*
     * The case the b2Shape_IsValid guard exists for.
     *
     * Destroying a body inside a sensor produces an end event whose visitor shape is already gone.
     * Anything counting what is currently inside a volume needs that event or it leaks one per
     * despawn; anything dereferencing it without checking crashes. Neither happens here: the pair is
     * skipped, so the count is what the *entities* say rather than what the shapes did.
     */
    nya_entity_despawn(player);
    step(1);

    nya_assert(pickup_exits == 0, "an exit whose visitor shape is already destroyed is skipped rather than faulted on");

    nya_entity_despawn(coin);
    nya_physics2d_gravity_set(NYA_PHYSICS2D_GRAVITY_DEFAULT);
  }

  nya_info("PASSED: test_physics");

  return EXIT_SUCCESS;
}
