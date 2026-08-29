/**
 * One-way surfaces: jump up through a ledge, land on it, then drop off it.
 *
 * The feature is a Box2D pre-solve callback returning false, so what is testable is the predicate
 * that decides — and the cases that matter are the ones a platformer hits every second: rising
 * through, landing on, resting, and deliberately letting go.
 *
 * ⚠ Positions are screen-space: y grows **downward**, so "up" is negative y and a falling body's
 * velocity is positive. Every sign below reads backwards if that is forgotten.
 *
 * ⚠ Two numbers here are load-bearing and were both got wrong first time. The ledge has to be
 * **thicker than one step's travel** or a fast body tunnels through it regardless of what the
 * callback says — that is a property of a discrete solver, and measuring it and calling it a pass is
 * what the first version of this file did. And the drop window has to be long enough for a free fall
 * to **clear the whole ledge**, or the window closes with the body still inside it, the contact
 * turns solid again, and it is pushed back out on top.
 *
 * Headless: the physics world needs an arena and a clock and nothing else.
 **/

#include "nyangine/nyangine.c"
#include "nyangine/nyangine.h"

#include "SDL3/SDL_init.h"

#define TICK (1.0F / 60.0F)

/** The ledge, centred at the origin. 16 units thick — see the note above. */
#define LEDGE_Y      0.0F
#define LEDGE_HALF_H 8.0F

/** The mover, 16 units square, so it rests with its centre 16 above the ledge's centre. */
#define MOVER_HALF_H 8.0F
#define RESTING_Y    (LEDGE_Y - LEDGE_HALF_H - MOVER_HALF_H)
#define UNDERSIDE_Y  (LEDGE_Y + LEDGE_HALF_H + MOVER_HALF_H)

/** Fast enough to reach the ledge, slow enough not to cross it in one step. */
#define APPROACH_SPEED 250.0F

static void step(u32 count) {
    for (u32 i = 0; i < count; i++) nya_system_physics2d_update(TICK);
}

/** Steps, returning the smallest y the entity reached — how far *up* it ever got. */
static f32 step_tracking_highest(NYA_EntityHandle handle, u32 count) {
    f32 highest = nya_entity_get(handle)->position.y;

    for (u32 i = 0; i < count; i++) {
        nya_system_physics2d_update(TICK);
        highest = nya_min(highest, nya_entity_get(handle)->position.y);
    }

    return highest;
}

/** A wide static ledge at the origin. */
static NYA_EntityHandle spawn_ledge(NYA_Physics2DOneWay direction) {
    NYA_EntityHandle ledge = nya_entity_spawn(.name = "ledge", .position = { 0.0F, LEDGE_Y, 0.0F });

    nya_physics2d_body_attach(
        ledge, .type = NYA_PHYSICS_BODY_STATIC, .shape = NYA_PHYSICS2D_SHAPE_BOX, .size = { 400.0F, LEDGE_HALF_H * 2.0F },
        .one_way = direction
    );

    return ledge;
}

/** A small dynamic box at `y`, with `velocity_y` already on it. Rotation locked, so it cannot tip. */
static NYA_EntityHandle spawn_mover(f32 y, f32 velocity_y) {
    NYA_EntityHandle mover = nya_entity_spawn(.name = "mover", .position = { 0.0F, y, 0.0F });

    nya_physics2d_body_attach(mover, .shape = NYA_PHYSICS2D_SHAPE_BOX, .size = { MOVER_HALF_H * 2.0F, MOVER_HALF_H * 2.0F },
                              .lock_rotation = true);
    nya_physics2d_velocity_set(nya_entity_get(mover), (f32x2){ 0.0F, velocity_y });

    return mover;
}

s32 main(void) {
    _NYA_APP_INSTANCE = (NYA_App){ .initialized = true };

    b8 sdl_ok = SDL_Init(0);
    nya_assert(sdl_ok, "SDL_Init failed: %s", SDL_GetError());

    nya_system_callback_init();
    NYA_World* world = nya_world_create();
    (void)nya_world_set(world);
    defer nya_world_destroy(world);
    defer nya_system_callback_deinit();

    // ── A solid ledge stops a body from below. The control the rest is measured against.
    {
        NYA_EntityHandle ledge = spawn_ledge(NYA_PHYSICS2D_ONE_WAY_NONE);
        NYA_EntityHandle mover = spawn_mover(UNDERSIDE_Y + 60.0F, -APPROACH_SPEED);

        // The *highest* point reached, not the final one: a body that bounces off the underside ends
        // up somewhere below, and so does one that tunnelled clean through and fell back past its
        // start. Only the peak tells the two apart.
        f32 highest = step_tracking_highest(mover, 90);

        nya_check(highest > LEDGE_Y, "a solid ledge must stop a body from below; it reached y=%f, past the ledge at %f",
                  (f64)highest, (f64)LEDGE_Y);

        nya_entity_despawn(mover);
        nya_entity_despawn(ledge);
    }

    // ── The same ledge, passable from below: the body goes through and comes to rest on top.
    {
        NYA_EntityHandle ledge = spawn_ledge(NYA_PHYSICS2D_ONE_WAY_UP);
        NYA_EntityHandle mover = spawn_mover(UNDERSIDE_Y + 60.0F, -APPROACH_SPEED);

        // Long enough to rise through, arc over, fall back and settle.
        step(180);

        NYA_Entity* entity = nya_entity_get(mover);

        // A tolerance, not an equality: the solver allows a little overlap at rest.
        nya_check(fabsf(entity->position.y - RESTING_Y) < 4.0F, "the body should come to rest on top, expected y≈%f got %f",
                  (f64)RESTING_Y, (f64)entity->position.y);

        nya_check(nya_physics2d_grounded(entity), "and be standing on it");

        nya_entity_despawn(mover);
        nya_entity_despawn(ledge);
    }

    // ── Falling onto a passable ledge still lands: exactly one direction is let through.
    {
        NYA_EntityHandle ledge = spawn_ledge(NYA_PHYSICS2D_ONE_WAY_UP);
        NYA_EntityHandle mover = spawn_mover(RESTING_Y - 60.0F, 0.0F);

        step(120);

        NYA_Entity* entity = nya_entity_get(mover);
        nya_check(fabsf(entity->position.y - RESTING_Y) < 4.0F, "a body falling onto an UP ledge must be caught by it, got y=%f",
                  (f64)entity->position.y);

        nya_entity_despawn(mover);
        nya_entity_despawn(ledge);
    }

    /*
     * ── nya_physics2d_drop_through lets a resting body fall off.
     *
     * This is the case that needed the contact-recycling suspension: a body sitting still is exactly
     * what Box2D skips re-examining, so before that fix the request was stored and never read.
     */
    {
        NYA_EntityHandle ledge = spawn_ledge(NYA_PHYSICS2D_ONE_WAY_UP);
        NYA_EntityHandle mover = spawn_mover(RESTING_Y - 60.0F, 0.0F);

        step(120);

        NYA_Entity* entity = nya_entity_get(mover);
        nya_check(nya_physics2d_grounded(entity), "it should be resting on the ledge first");

        // Long enough for a free fall to clear the ledge's full thickness — see the file's note.
        nya_physics2d_drop_through(entity, 0.6F);
        step(60);

        f32 dropped = nya_entity_get(mover)->position.y;
        nya_check(dropped > LEDGE_Y + LEDGE_HALF_H, "dropping through should put it clear below the ledge, got y=%f", (f64)dropped);

        // And the window closes on its own, so the next ledge down still catches it. Read off the
        // entity directly: NYA_Physics2DBody is a public struct, like every other module's state.
        nya_check(nya_entity_get(mover)->physics2d.drop_through_s == 0.0F, "the window should have run out within sixty steps");

        nya_entity_despawn(mover);
        nya_entity_despawn(ledge);
    }

    // ── A drop window that has run out no longer lets anything through.
    {
        NYA_EntityHandle ledge = spawn_ledge(NYA_PHYSICS2D_ONE_WAY_UP);
        NYA_EntityHandle mover = spawn_mover(RESTING_Y - 60.0F, 0.0F);

        // Asked for and then immediately spent, before the body has even reached the ledge.
        nya_physics2d_drop_through(nya_entity_get(mover), 0.05F);
        step(120);

        NYA_Entity* entity = nya_entity_get(mover);
        nya_check(fabsf(entity->position.y - RESTING_Y) < 4.0F, "an expired window must not keep the ledge passable, got y=%f",
                  (f64)entity->position.y);

        nya_entity_despawn(mover);
        nya_entity_despawn(ledge);
    }

    // ── The accessors, including the cases with no body behind them.
    {
        NYA_EntityHandle ledge = spawn_ledge(NYA_PHYSICS2D_ONE_WAY_DOWN);

        nya_check(nya_physics2d_one_way(nya_entity_get(ledge)) == NYA_PHYSICS2D_ONE_WAY_DOWN, "the attach option should stick");

        nya_physics2d_one_way_set(nya_entity_get(ledge), NYA_PHYSICS2D_ONE_WAY_LEFT);
        nya_check(nya_physics2d_one_way(nya_entity_get(ledge)) == NYA_PHYSICS2D_ONE_WAY_LEFT, "and the setter should replace it");

        NYA_EntityHandle bodiless = nya_entity_spawn(.name = "bodiless");
        nya_check(nya_physics2d_one_way(nya_entity_get(bodiless)) == NYA_PHYSICS2D_ONE_WAY_NONE,
                  "an entity with no body is not a one-way surface");
        nya_check(nya_physics2d_one_way(nullptr) == NYA_PHYSICS2D_ONE_WAY_NONE, "and neither is nothing at all");

        nya_entity_despawn(bodiless);
        nya_entity_despawn(ledge);
    }

    return nya_check_failures() == 0 ? 0 : 1;
}
