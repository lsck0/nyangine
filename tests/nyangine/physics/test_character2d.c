/**
 * The 2D character controller: the forgiveness windows, and the jump behaviours built on them.
 *
 * Coyote time and jump buffering are the two things here that are pure feel, and both are invisible
 * when they work — which is exactly why they need a test. The one worth pinning hardest is that a
 * single press cannot produce two jumps: both windows must be consumed on success, and leaving either
 * armed gives a free second jump the instant the character lands.
 **/

#include "nyangine/nyangine.c"
#include "nyangine/nyangine.h"

#include "SDL3/SDL_init.h"

#define TICK (1.0F / 60.0F)

/** A character standing on a wide static floor, so grounding is real rather than mocked. */
typedef struct {
    NYA_EntityHandle body;
    NYA_EntityHandle floor;
} Scene;

static Scene scene_create(f32 start_y) {
    Scene scene = { 0 };

    scene.floor = nya_entity_spawn(.name = "floor", .position = { 0.0F, 200.0F, 0.0F });
    nya_physics2d_body_attach(scene.floor, .type = NYA_PHYSICS_BODY_STATIC, .shape = NYA_PHYSICS2D_SHAPE_BOX,
                              .size = { 2000.0F, 40.0F });

    scene.body = nya_entity_spawn(.name = "player", .position = { 0.0F, start_y, 0.0F });
    nya_physics2d_body_attach(scene.body, .shape = NYA_PHYSICS2D_SHAPE_BOX, .size = { 24.0F, 40.0F }, .lock_rotation = true);

    return scene;
}

static void scene_destroy(Scene* scene) {
    nya_entity_despawn(scene->body);
    nya_entity_despawn(scene->floor);
}

/** Runs the controller and the solver together, the way a fixed tick would. */
static void tick(NYA_CharacterController2D* controller, Scene* scene, NYA_CharacterInput2D input, u32 count) {
    for (u32 i = 0; i < count; i++) {
        nya_character2d_update(controller, scene->body, input, TICK);
        nya_system_physics2d_update(TICK);
    }
}

/** Settles the character onto the floor and returns once it is grounded. */
static b8 settle(NYA_CharacterController2D* controller, Scene* scene) {
    for (u32 i = 0; i < 240; i++) {
        tick(controller, scene, (NYA_CharacterInput2D){ 0 }, 1);
        if (controller->grounded) return true;
    }
    return false;
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

    // ── Defaults fill in, so a zeroed tuning is a working character.
    {
        NYA_CharacterTuning2D tuning = nya_character2d_tuning_defaults((NYA_CharacterTuning2D){ 0 });

        nya_check(tuning.max_speed > 0.0F && tuning.jump_speed > 0.0F, "speeds should default");
        nya_check(tuning.coyote_time_s > 0.0F && tuning.jump_buffer_s > 0.0F, "both windows should default");
        nya_check(tuning.fall_gravity_multiplier > 1.0F, "falling should be heavier than rising, got %f",
                  (f64)tuning.fall_gravity_multiplier);
        nya_check(tuning.air_control < 1.0F, "air control should be less than ground control");
    }

    // ── An entity with no body is ignored rather than asserted on.
    {
        NYA_CharacterController2D controller = { 0 };
        NYA_EntityHandle          bare       = nya_entity_spawn(.name = "bare");

        nya_character2d_update(&controller, bare, (NYA_CharacterInput2D){ 0 }, TICK);
        nya_check(!controller.grounded, "a bodyless entity leaves the controller alone");

        nya_entity_despawn(bare);
    }

    // ── Falling, landing, and the landed edge firing exactly once.
    {
        NYA_CharacterController2D controller = { 0 };
        Scene                     scene      = scene_create(0.0F);

        nya_check(settle(&controller, &scene), "the character should reach the floor");
        nya_check(controller.grounded, "and report grounded");

        u32 landings = 0;
        for (u32 i = 0; i < 60; i++) {
            tick(&controller, &scene, (NYA_CharacterInput2D){ 0 }, 1);
            if (controller.landed) landings++;
        }
        nya_check(landings == 0, "landed should not keep firing while stood still, fired %u", landings);

        scene_destroy(&scene);
    }

    // ── Jumping leaves the ground, and gravity brings it back.
    {
        NYA_CharacterController2D controller = { 0 };
        Scene                     scene      = scene_create(0.0F);
        nya_check(settle(&controller, &scene), "settled");

        f32 resting = nya_entity_get(scene.body)->position.y;

        tick(&controller, &scene, (NYA_CharacterInput2D){ .jump_pressed = true, .jump_held = true }, 1);
        nya_check(controller.jumped, "the jump should fire on the press");

        tick(&controller, &scene, (NYA_CharacterInput2D){ .jump_held = true }, 10);
        // Positive y is down the screen, so rising means a smaller y.
        nya_check(nya_entity_get(scene.body)->position.y < resting - 5.0F, "it should have risen");

        for (u32 i = 0; i < 300 && !controller.grounded; i++) tick(&controller, &scene, (NYA_CharacterInput2D){ 0 }, 1);
        nya_check(controller.grounded, "and come back down");

        scene_destroy(&scene);
    }

    // ── Coyote time: a jump still works shortly after leaving the ground.
    {
        NYA_CharacterController2D controller = { .tuning = { .coyote_time_s = 0.10F } };
        Scene                     scene      = scene_create(0.0F);
        nya_check(settle(&controller, &scene), "settled");

        // Take the floor away, then jump a few ticks later — inside the window.
        nya_entity_despawn(scene.floor);
        scene.floor = NYA_ENTITY_HANDLE_NONE;

        tick(&controller, &scene, (NYA_CharacterInput2D){ 0 }, 2);
        nya_check(!controller.grounded, "it should be airborne now");

        tick(&controller, &scene, (NYA_CharacterInput2D){ .jump_pressed = true, .jump_held = true }, 1);
        nya_check(controller.jumped, "a jump inside the coyote window should still fire");

        nya_entity_despawn(scene.body);
    }

    // ── And stops working once the window closes.
    {
        NYA_CharacterController2D controller = { .tuning = { .coyote_time_s = 0.05F } };
        Scene                     scene      = scene_create(0.0F);
        nya_check(settle(&controller, &scene), "settled");

        nya_entity_despawn(scene.floor);
        scene.floor = NYA_ENTITY_HANDLE_NONE;

        tick(&controller, &scene, (NYA_CharacterInput2D){ 0 }, 20);
        tick(&controller, &scene, (NYA_CharacterInput2D){ .jump_pressed = true, .jump_held = true }, 1);
        nya_check(!controller.jumped, "a jump well past the coyote window should not fire");

        nya_entity_despawn(scene.body);
    }

    // ── Jump buffering: a press just before landing fires on touchdown.
    {
        // Started just above the floor, so the fall is a handful of ticks — the buffer is a window in
        // real time, and a press from far above is supposed to expire rather than wait.
        NYA_CharacterController2D controller = { .tuning = { .jump_buffer_s = 0.15F } };
        Scene                     scene      = scene_create(120.0F);

        // Fall until close to the floor, then press — rather than pressing at a fixed tick, which ties
        // the test to the fall geometry. The buffer is a real-time window: a press from far above is
        // meant to expire, and that is the next case.
        for (u32 i = 0; i < 60 && nya_entity_get(scene.body)->position.y < 150.0F; i++) {
            tick(&controller, &scene, (NYA_CharacterInput2D){ 0 }, 1);
        }
        nya_check(!controller.grounded, "still in the air");

        tick(&controller, &scene, (NYA_CharacterInput2D){ .jump_pressed = true, .jump_held = true }, 1);
        nya_check(!controller.jumped, "the press should not jump in mid-air");

        b8 jumped_on_landing = false;
        for (u32 i = 0; i < 8; i++) {
            tick(&controller, &scene, (NYA_CharacterInput2D){ .jump_held = true }, 1);
            if (controller.jumped) {
                jumped_on_landing = true;
                break;
            }
        }
        nya_check(jumped_on_landing, "the buffered press should fire as it lands");

        scene_destroy(&scene);
    }

    // ── A press far too early expires rather than waiting forever.
    {
        NYA_CharacterController2D controller = { .tuning = { .jump_buffer_s = 0.05F } };
        Scene                     scene      = scene_create(-400.0F);

        tick(&controller, &scene, (NYA_CharacterInput2D){ .jump_pressed = true, .jump_held = true }, 1);

        b8 jumped = false;
        for (u32 i = 0; i < 200; i++) {
            tick(&controller, &scene, (NYA_CharacterInput2D){ .jump_held = true }, 1);
            if (controller.jumped) jumped = true;
            if (controller.grounded) break;
        }
        nya_check(!jumped, "a press that expired must not resurface on landing");

        scene_destroy(&scene);
    }

    // ⭐ One press must not produce two jumps. Both windows have to be consumed.
    {
        NYA_CharacterController2D controller = { 0 };
        Scene                     scene      = scene_create(0.0F);
        nya_check(settle(&controller, &scene), "settled");

        u32 jumps = 0;
        tick(&controller, &scene, (NYA_CharacterInput2D){ .jump_pressed = true, .jump_held = true }, 1);
        if (controller.jumped) jumps++;

        // Hold through the whole arc and back down. Exactly one jump may come out of it.
        for (u32 i = 0; i < 300; i++) {
            tick(&controller, &scene, (NYA_CharacterInput2D){ .jump_held = true }, 1);
            if (controller.jumped) jumps++;
        }
        nya_check(jumps == 1, "one press must give exactly one jump, got %u", jumps);

        scene_destroy(&scene);
    }

    // ── Variable height: releasing early gives a lower jump than holding.
    {
        Scene scene = scene_create(0.0F);

        NYA_CharacterController2D held = { 0 };
        nya_check(settle(&held, &scene), "settled");
        f32 start = nya_entity_get(scene.body)->position.y;

        tick(&held, &scene, (NYA_CharacterInput2D){ .jump_pressed = true, .jump_held = true }, 1);
        f32 highest_held = start;
        for (u32 i = 0; i < 40; i++) {
            tick(&held, &scene, (NYA_CharacterInput2D){ .jump_held = true }, 1);
            highest_held = nya_min(highest_held, nya_entity_get(scene.body)->position.y);
        }

        scene_destroy(&scene);

        Scene                     scene2  = scene_create(0.0F);
        NYA_CharacterController2D tapped  = { 0 };
        nya_check(settle(&tapped, &scene2), "settled again");
        f32 start2 = nya_entity_get(scene2.body)->position.y;

        tick(&tapped, &scene2, (NYA_CharacterInput2D){ .jump_pressed = true, .jump_held = true }, 1);
        // Released immediately.
        f32 highest_tapped = start2;
        for (u32 i = 0; i < 40; i++) {
            tick(&tapped, &scene2, (NYA_CharacterInput2D){ 0 }, 1);
            highest_tapped = nya_min(highest_tapped, nya_entity_get(scene2.body)->position.y);
        }

        // Smaller y is higher.
        nya_check(highest_held < highest_tapped, "holding should jump higher than tapping: %f vs %f",
                  (f64)highest_held, (f64)highest_tapped);

        scene_destroy(&scene2);
    }

    // ── Horizontal input accelerates rather than teleporting, and facing is remembered through a stop.
    {
        NYA_CharacterController2D controller = { 0 };
        Scene                     scene      = scene_create(0.0F);
        nya_check(settle(&controller, &scene), "settled");

        tick(&controller, &scene, (NYA_CharacterInput2D){ .move = 1.0F }, 1);
        f32 after_one = nya_physics2d_velocity(nya_entity_get(scene.body)).x;
        nya_check(after_one > 0.0F, "it should start moving right");
        nya_check(after_one < controller.tuning.max_speed, "but not reach top speed in one tick, got %f", (f64)after_one);

        tick(&controller, &scene, (NYA_CharacterInput2D){ .move = 1.0F }, 60);
        f32 settled_speed = nya_physics2d_velocity(nya_entity_get(scene.body)).x;
        nya_check(settled_speed > controller.tuning.max_speed * 0.8F, "and get there eventually, got %f", (f64)settled_speed);
        nya_check(controller.facing > 0.0F, "facing right");

        tick(&controller, &scene, (NYA_CharacterInput2D){ .move = -1.0F }, 60);
        nya_check(controller.facing < 0.0F, "facing left after turning");

        tick(&controller, &scene, (NYA_CharacterInput2D){ 0 }, 60);
        nya_check(controller.facing < 0.0F, "and still facing left after stopping");
        nya_check(fabsf(nya_physics2d_velocity(nya_entity_get(scene.body)).x) < 20.0F, "and having slowed down");

        scene_destroy(&scene);
    }

    // ── Reset clears both windows, so a respawn cannot inherit a queued jump.
    {
        NYA_CharacterController2D controller = { .buffer_left_s = 1.0F, .coyote_left_s = 1.0F, .jumping = true };

        nya_character2d_reset(&controller);
        nya_check(controller.buffer_left_s == 0.0F && controller.coyote_left_s == 0.0F, "both windows should clear");
        nya_check(!controller.jumping, "and the jump should end");

        nya_character2d_reset(nullptr);
    }

    return nya_check_failures() == 0 ? 0 : 1;
}
