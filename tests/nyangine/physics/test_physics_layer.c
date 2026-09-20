/**
 * Named collision layers: the registry, and what both solvers do with a filtered pair.
 **/

#include "nyangine/nyangine.c"
#include "nyangine/nyangine.h"

#include "SDL3/SDL_init.h"

#define TICK (1.0F / 60.0F)

/** Steps a dropped body takes before the test looks at where it ended up. */
#define DROP_STEPS 90

static void step2d(u32 count) {
    for (u32 i = 0; i < count; i++) nya_system_physics2d_update(TICK);
}

static void step3d(u32 count) {
    for (u32 i = 0; i < count; i++) nya_system_physics3d_update(TICK);
}

s32 main(void) {
    setvbuf(stdout, nullptr, _IONBF, 0);

    _NYA_APP_INSTANCE = (NYA_App){ .initialized = true };

    b8 sdl_ok = SDL_Init(0);
    nya_assert(sdl_ok, "SDL_Init failed: %s", SDL_GetError());

    nya_system_callback_init();
    defer nya_system_callback_deinit();

    NYA_World* world = nya_world_create();
    (void)nya_world_set(world);
    defer nya_world_destroy(world);

    printf("TEST: the registry\n");
    {
        nya_check(nya_physics_layer_count() == 1, "a fresh world holds only the default layer, got %u", nya_physics_layer_count());

        nya_check(nya_physics_layer(NYA_PHYSICS_LAYER_DEFAULT_NAME) == NYA_PHYSICS_LAYER_DEFAULT, "the default layer is bit zero");
        nya_check(nya_string_equals(nya_physics_layer_name(0), NYA_PHYSICS_LAYER_DEFAULT_NAME), "bit zero is named '%s'",
                  NYA_PHYSICS_LAYER_DEFAULT_NAME);

        NYA_PhysicsLayerMask ground = nya_physics_layer("ground");
        NYA_PhysicsLayerMask actor  = nya_physics_layer("actor");

        nya_check(ground == (NYA_PhysicsLayerMask)1 << 1, "the first registered layer is bit one");
        nya_check(actor == (NYA_PhysicsLayerMask)1 << 2, "the second registered layer is bit two");

        // idempotent: a name always gives the same bit, which is what late binding by name means.
        nya_check(nya_physics_layer("ground") == ground, "registering 'ground' twice gives the same bit");
        nya_check(nya_physics_layer_count() == 3, "three layers are registered, got %u", nya_physics_layer_count());

        NYA_PhysicsLayerMask found = NYA_PHYSICS_LAYER_NONE;

        nya_check(nya_physics_layer_find("actor", &found) && found == actor, "find resolves a registered name");
        nya_check(!nya_physics_layer_find("nothing_is_called_this", &found), "find refuses an unregistered name");
        nya_check(found == NYA_PHYSICS_LAYER_NONE, "a refused find leaves no layer behind");
        nya_check(nya_physics_layer_count() == 3, "find registered nothing, got %u layers", nya_physics_layer_count());

        nya_check(nya_physics_layer_name(nya_physics_layer_count()) == nullptr, "there is no name past the last layer");

        // the variadic convenience, on top of the same call.
        NYA_PhysicsLayerMask both = nya_physics_layers("ground", "actor");
        nya_check(both == (ground | actor), "a list of names is the or of their bits");
    }

    printf("TEST: the overlap rule\n");
    {
        NYA_PhysicsLayerMask ground = nya_physics_layer("ground");
        NYA_PhysicsLayerMask actor  = nya_physics_layer("actor");

        nya_check(nya_physics_layer_mask_overlaps(ground, actor, actor, ground), "both sides wanting it means they meet");
        nya_check(!nya_physics_layer_mask_overlaps(ground, actor, actor, NYA_PHYSICS_LAYER_NONE), "one side refusing is enough to keep them apart");
        nya_check(!nya_physics_layer_mask_overlaps(ground, NYA_PHYSICS_LAYER_NONE, actor, ground), "the other side refusing is too");
        nya_check(nya_physics_layer_mask_overlaps(ground, NYA_PHYSICS_LAYER_ALL, actor, NYA_PHYSICS_LAYER_ALL), "everything meets everything");
    }

    printf("TEST: a 2D body falls through a floor on a layer it does not collide with\n");
    {
        NYA_PhysicsLayerMask ground = nya_physics_layer("ground");
        NYA_PhysicsLayerMask actor  = nya_physics_layer("actor");
        NYA_PhysicsLayerMask ghost  = nya_physics_layer("ghost");

        NYA_EntityHandle floor = nya_entity_spawn(.name = "floor", .position = { 0.0F, 200.0F, 0.0F });
        nya_check(nya_physics2d_body_attach(floor, .type = NYA_PHYSICS_BODY_STATIC, .size = { 800.0F, 20.0F }, .layers = ground,
                                            .collides_with = NYA_PHYSICS_LAYER_ALL),
                  "the floor takes a body");

        NYA_EntityHandle lands = nya_entity_spawn(.name = "lands", .position = { 0.0F, 0.0F, 0.0F });
        nya_check(nya_physics2d_body_attach(lands, .size = { 16.0F, 16.0F }, .layers = actor, .collides_with = ground), "the lander takes a body");

        NYA_EntityHandle falls = nya_entity_spawn(.name = "falls", .position = { 64.0F, 0.0F, 0.0F });
        nya_check(nya_physics2d_body_attach(falls, .size = { 16.0F, 16.0F }, .layers = ghost, .collides_with = actor), "the ghost takes a body");

        step2d(DROP_STEPS);

        // positive y is down in the 2D world; see physics2d.h.
        f32 landed  = nya_entity_get(lands)->position.y;
        f32 dropped = nya_entity_get(falls)->position.y;

        // the floor's top surface is at y = 190; positive y is down.
        nya_check(landed < 190.0F, "the lander should be resting on the floor, got y=%f", (f64)landed);
        nya_check(dropped > 210.0F, "the ghost should have fallen straight through the floor, got y=%f", (f64)dropped);
        nya_check(dropped > landed + 100.0F, "the ghost should be far below the lander, got %f against %f", (f64)dropped, (f64)landed);

        printf("TEST: moving it onto the floor's mask stops it\n");
        {
            NYA_Entity* ghost_entity = nya_entity_get(falls);

            nya_physics2d_layers_set(ghost_entity, actor, ground);

            nya_check(nya_physics2d_layers(ghost_entity) == actor, "the body reports the layers it was moved to");
            nya_check(nya_physics2d_collides_with(ghost_entity) == ground, "the body reports the mask it was given");

            // back above the floor, then dropped again: this time it has to land.
            nya_physics2d_teleport(ghost_entity, (f32x2){ 64.0F, 0.0F }, 0.0F);
            nya_physics2d_velocity_set(ghost_entity, f32x2_zero);

            step2d(DROP_STEPS);

            f32 y = nya_entity_get(falls)->position.y;
            nya_check(y < 200.0F, "refiltered onto the floor's mask it should land, got y=%f", (f64)y);
        }

        printf("TEST: a raycast only sees the layers it asked for\n");
        {
            f32x2 origin    = { 0.0F, 0.0F };
            f32x2 direction = { 0.0F, 400.0F };

            NYA_EntityHandle any = nya_physics2d_raycast(origin, direction, &(f32x2){ 0 }, nullptr);
            nya_check(nya_entity_is_valid(any), "an unfiltered ray should strike something");

            NYA_EntityHandle only_ground = nya_physics2d_raycast(origin, direction, ground, &(f32x2){ 0 }, nullptr);
            nya_check(nya_entity_is_valid(only_ground), "a ray for the ground layer should strike the floor");

            NYA_Entity* struck = nya_entity_get(only_ground);
            nya_check(struck != nullptr && (nya_physics2d_layers(struck) & ground) != 0, "the struck body should be on the ground layer");

            // a layer nothing is on: the ray has to come back empty rather than fall back to anything.
            NYA_PhysicsLayerMask nobody      = nya_physics_layer("nobody_is_here");
            NYA_EntityHandle     struck_none = nya_physics2d_raycast(origin, direction, nobody, nullptr, nullptr);

            nya_check(!nya_entity_is_valid(struck_none), "a ray for an empty layer should strike nothing");

            NYA_EntityHandle at_none = nya_physics2d_entity_at((f32x2){ 0.0F, 200.0F }, nobody);
            nya_check(!nya_entity_is_valid(at_none), "a point query for an empty layer should find nothing");
        }

        nya_entity_despawn(floor);
        nya_entity_despawn(lands);
        nya_entity_despawn(falls);
    }

    printf("TEST: the same in 3D\n");
    {
        NYA_PhysicsLayerMask ground = nya_physics_layer("ground");
        NYA_PhysicsLayerMask actor  = nya_physics_layer("actor");
        NYA_PhysicsLayerMask ghost  = nya_physics_layer("ghost");

        NYA_EntityHandle floor = nya_entity_spawn(.name = "floor3d", .position = { 0.0F, 0.0F, 0.0F });
        nya_check(nya_physics3d_body_attach(floor, .type = NYA_PHYSICS_BODY_STATIC, .shape = NYA_PHYSICS3D_SHAPE_BOX,
                                            .size = { 40.0F, 1.0F, 40.0F }, .layers = ground, .collides_with = NYA_PHYSICS_LAYER_ALL),
                  "the 3D floor takes a body");

        NYA_EntityHandle lands = nya_entity_spawn(.name = "lands3d", .position = { 0.0F, 8.0F, 0.0F });
        nya_check(nya_physics3d_body_attach(lands, .shape = NYA_PHYSICS3D_SHAPE_BOX, .size = { 1.0F, 1.0F, 1.0F }, .layers = actor,
                                            .collides_with = ground),
                  "the 3D lander takes a body");

        NYA_EntityHandle falls = nya_entity_spawn(.name = "falls3d", .position = { 4.0F, 8.0F, 0.0F });
        nya_check(nya_physics3d_body_attach(falls, .shape = NYA_PHYSICS3D_SHAPE_BOX, .size = { 1.0F, 1.0F, 1.0F }, .layers = ghost,
                                            .collides_with = actor),
                  "the 3D ghost takes a body");

        step3d(DROP_STEPS);

        // negative y is down in the 3D world, the opposite of the 2D one; see physics3d.h.
        f32 landed  = nya_entity_get(lands)->position.y;
        f32 dropped = nya_entity_get(falls)->position.y;

        // the floor's top surface is at y = 0.5; negative y is down in the 3D world.
        nya_check(landed > 0.0F, "the 3D lander should be resting on the floor, got y=%f", (f64)landed);
        nya_check(dropped < -0.5F, "the 3D ghost should have fallen straight through the floor, got y=%f", (f64)dropped);
        nya_check(dropped < landed - 4.0F, "the 3D ghost should be far below the lander, got %f against %f", (f64)dropped, (f64)landed);

        NYA_Entity* ghost_entity = nya_entity_get(falls);

        nya_physics3d_layers_set(ghost_entity, actor, ground);

        nya_check(nya_physics3d_layers(ghost_entity) == actor, "the 3D body reports the layers it was moved to");
        nya_check(nya_physics3d_collides_with(ghost_entity) == ground, "the 3D body reports the mask it was given");

        nya_entity_despawn(floor);
        nya_entity_despawn(lands);
        nya_entity_despawn(falls);
    }

    printf("TEST: an entity with no body is on no layer\n");
    {
        NYA_EntityHandle bare = nya_entity_spawn(.name = "bare");

        nya_check(nya_physics2d_layers(nya_entity_get(bare)) == NYA_PHYSICS_LAYER_NONE, "no 2D body means no layers");
        nya_check(nya_physics3d_layers(nya_entity_get(bare)) == NYA_PHYSICS_LAYER_NONE, "no 3D body means no layers");
        nya_check(nya_physics2d_layers(nullptr) == NYA_PHYSICS_LAYER_NONE, "a null entity is on no layer");

        nya_entity_despawn(bare);
    }

    printf("TEST: the registry refuses a sixty-fifth layer rather than reusing a bit\n");
    {
        char name[32];

        while (nya_physics_layer_count() < NYA_PHYSICS_LAYER_MAX) {
            (void)snprintf(name, sizeof(name), "filler_%u", nya_physics_layer_count());
            nya_check(nya_physics_layer(name) != NYA_PHYSICS_LAYER_NONE, "'%s' should have been registered", name);
        }

        nya_check(nya_physics_layer_count() == NYA_PHYSICS_LAYER_MAX, "the registry should be full, holds %u", nya_physics_layer_count());

        // an operating error, not a crash: too many layers is a content problem.
        nya_check(nya_physics_layer("one_too_many") == NYA_PHYSICS_LAYER_NONE, "a full registry hands out no bit");
        nya_check(nya_physics_layer_count() == NYA_PHYSICS_LAYER_MAX, "a refused registration changed the count");

        // a name already registered still resolves, full or not.
        nya_check(nya_physics_layer("ground") != NYA_PHYSICS_LAYER_NONE, "a known name still resolves when the registry is full");
    }

    if (nya_check_failures() > 0) {
        printf("FAILED: test_physics_layer (%u failures)\n", nya_check_failures());
        return EXIT_FAILURE;
    }

    printf("PASSED: test_physics_layer (0 failures)\n");

    return EXIT_SUCCESS;
}
