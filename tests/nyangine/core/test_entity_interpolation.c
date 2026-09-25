/**
 * Drawing entities between ticks: the render transform, the capture at the top of a tick, and every jump that
 * must not sweep.
 **/

#include "nyangine-core/nyangine.c"
#include "nyangine-core/nyangine.h"

#include "SDL3/SDL_init.h"

#define TICK_NS 16'000'000

static b8 near_enough(f32 a, f32 b) {
    return fabsf(a - b) < 0.001F;
}

static b8 near_position(f32x3 a, f32x3 b) {
    return near_enough(a.x, b.x) && near_enough(a.y, b.y) && near_enough(a.z, b.z);
}

/** Puts the frame `alpha` of the way from the last tick to the next, as the app's clock would. */
static void alpha_set(f32 alpha) {
    _NYA_APP_INSTANCE.options.time_step_ns       = TICK_NS;
    _NYA_APP_INSTANCE.frame_stats.time_behind_ns = (s64)(alpha * (f32)TICK_NS);
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

    // The alpha is the frame's place between ticks, clamped, and one without a time step.
    {
        nya_check(nya_app_tick_alpha() == 1.0F, "no time step draws the current tick");

        alpha_set(0.25F);
        nya_check(near_enough(nya_app_tick_alpha(), 0.25F), "a quarter tick behind, got %f", (f64)nya_app_tick_alpha());

        // inside a tick the debt is at least a step, so anything read there is the current tick.
        _NYA_APP_INSTANCE.frame_stats.time_behind_ns = 3 * TICK_NS;
        nya_check(nya_app_tick_alpha() == 1.0F, "a debt past one tick clamps to the current tick");
    }

    // A tick's motion is drawn in proportion to the alpha, from where the capture left it.
    {
        NYA_EntityHandle handle = nya_entity_spawn(.name = "mover", .position = { 4.0F, 0.0F, 0.0F }, .velocity = { 10.0F, 0.0F, -20.0F });
        NYA_Entity*      entity = nya_entity_get(handle);

        nya_system_entity_transforms_capture();
        nya_system_entity_update(0.1F);

        nya_check(near_position(entity->position, (f32x3){ 5.0F, 0.0F, -2.0F }), "the tick moved it");

        alpha_set(0.0F);
        nya_check(near_position(nya_entity_render_position(entity), (f32x3){ 4.0F, 0.0F, 0.0F }), "at zero it draws where the tick began");

        alpha_set(0.5F);
        f32x3 half = nya_entity_render_position(entity);
        nya_check(near_position(half, (f32x3){ 4.5F, 0.0F, -1.0F }), "halfway draws halfway, got " FMTf32x3, FMTf32x3_ARG(half));

        // the next tick starts from the end of this one.
        nya_system_entity_transforms_capture();
        alpha_set(0.0F);
        nya_check(near_position(nya_entity_render_position(entity), entity->position), "a capture moves the start to the current tick");

        nya_entity_clear();
    }

    // Rotation takes the short way and stays a unit quaternion.
    {
        NYA_EntityHandle handle = nya_entity_spawn(.name = "spinner");
        NYA_Entity*      entity = nya_entity_get(handle);

        NYA_Quaternion quarter = nya_quaternion_from_axis_angle((f32x3){ 0.0F, 0.0F, 1.0F }, (f32)M_PI * 0.5F);

        entity->rotation_previous = nya_quaternion_identity;

        // the same rotation with every sign flipped, which a component lerp would swing the long way to.
        entity->rotation = nya_quaternion_scale(quarter, -1.0F);

        alpha_set(0.5F);
        NYA_Quaternion half = nya_entity_render_rotation(entity);

        nya_check(near_enough(nya_quaternion_length(half), 1.0F), "unit length, got %f", (f64)nya_quaternion_length(half));

        f32 angle = nya_quaternion_angle_between(half, nya_quaternion_identity);
        nya_check(near_enough(angle, (f32)M_PI * 0.25F), "an eighth turn at halfway, got %f", (f64)angle);

        alpha_set(1.0F);
        f32 rest = nya_quaternion_angle_between(nya_entity_render_rotation(entity), quarter);
        nya_check(rest < 0.001F, "one draws the current rotation, off by %f", (f64)rest);

        nya_entity_clear();
    }

    // A spawn draws where it spawned, even into a slot whose last owner was elsewhere.
    {
        NYA_EntityHandle first = nya_entity_spawn(.name = "first", .position = { 900.0F, 900.0F, 0.0F });
        nya_entity_despawn(first);

        NYA_EntityHandle second = nya_entity_spawn(.name = "second", .position = { -3.0F, 7.0F, 1.0F });
        nya_check(second.index == first.index, "the slot is reused");

        alpha_set(0.0F);
        nya_check(near_position(nya_entity_render_position(nya_entity_get(second)), (f32x3){ -3.0F, 7.0F, 1.0F }), "and draws at its spawn");

        nya_entity_clear();
    }

    // A snap draws the jump at once, and carries the children with it.
    {
        NYA_EntityHandle parent_handle = nya_entity_spawn(.name = "parent", .position = { 0.0F, 0.0F, 0.0F });
        NYA_EntityHandle child_handle  = nya_entity_spawn(.name = "child", .position = { 0.0F, 2.0F, 0.0F });
        nya_check(nya_entity_parent_set(child_handle, parent_handle), "parenting should succeed");

        nya_system_entity_transforms_capture();

        NYA_Entity* parent = nya_entity_get(parent_handle);
        NYA_Entity* child  = nya_entity_get(child_handle);

        parent->position = (f32x3){ 500.0F, 0.0F, 0.0F };
        nya_entity_transform_snap(parent);

        alpha_set(0.0F);
        nya_check(near_position(nya_entity_render_position(parent), (f32x3){ 500.0F, 0.0F, 0.0F }), "the parent draws at the jump");

        f32x3 drawn = nya_entity_render_position(child);
        nya_check(near_position(drawn, (f32x3){ 500.0F, 2.0F, 0.0F }), "the child lands with it, got " FMTf32x3, FMTf32x3_ARG(drawn));

        // the end of the tick composes the same place, so nothing sweeps on the next capture either.
        nya_system_entity_transforms_update();
        nya_system_entity_transforms_capture();
        nya_check(near_position(nya_entity_render_position(child), (f32x3){ 500.0F, 2.0F, 0.0F }), "and stays there after the tick");

        nya_entity_clear();
    }

    // A zero length move teleports, so it snaps.
    {
        NYA_EntityHandle handle = nya_entity_spawn(.name = "mover");
        NYA_Entity*      entity = nya_entity_get(handle);

        nya_entity_move_to(entity, (f32x3){ 0.0F, 64.0F, 0.0F }, 0.0F, NYA_EASE_LINEAR);

        alpha_set(0.0F);
        nya_check(near_position(nya_entity_render_position(entity), (f32x3){ 0.0F, 64.0F, 0.0F }), "a zero duration move is a jump");

        nya_entity_clear();
    }

    // Both solvers' teleports snap, and their readback is drawn between ticks like any other motion.
    {
        NYA_EntityHandle crate = nya_entity_spawn(.name = "crate", .position = { 0.0F, 0.0F, 0.0F });
        nya_check(nya_physics2d_body_attach(crate, .type = NYA_PHYSICS_BODY_DYNAMIC, .shape = NYA_PHYSICS2D_SHAPE_BOX, .size = { 16.0F, 16.0F }),
                  "a 2D body attaches");

        NYA_Entity* flat = nya_entity_get(crate);

        nya_system_entity_transforms_capture();
        nya_physics2d_teleport(flat, (f32x2){ 300.0F, -40.0F }, 1.0F);

        alpha_set(0.0F);
        nya_check(near_position(nya_entity_render_position(flat), (f32x3){ 300.0F, -40.0F, 0.0F }), "a 2D teleport draws at once");

        NYA_EntityHandle cube = nya_entity_spawn(.name = "cube", .position = { 0.0F, 10.0F, 0.0F });
        nya_check(nya_physics3d_body_attach(cube, .type = NYA_PHYSICS_BODY_DYNAMIC, .shape = NYA_PHYSICS3D_SHAPE_BOX, .size = { 1.0F, 1.0F, 1.0F }),
                  "a 3D body attaches");

        NYA_Entity*    solid   = nya_entity_get(cube);
        NYA_Quaternion upended = nya_quaternion_from_axis_angle((f32x3){ 1.0F, 0.0F, 0.0F }, 2.0F);

        nya_physics3d_teleport(solid, (f32x3){ -8.0F, 30.0F, 5.0F }, upended);

        nya_check(near_position(nya_entity_render_position(solid), (f32x3){ -8.0F, 30.0F, 5.0F }), "a 3D teleport draws at once");
        nya_check(nya_quaternion_approx_equals(nya_entity_render_rotation(solid), upended, 0.001F), "rotation included");

        // a falling body, captured then stepped, draws between the two heights.
        nya_system_entity_transforms_capture();
        nya_system_physics3d_update(1.0F / 60.0F);

        f32 before = solid->position_previous.y;
        f32 after  = solid->position.y;
        nya_check(after < before, "gravity moved it, %f to %f", (f64)before, (f64)after);

        alpha_set(0.5F);
        nya_check(near_enough(nya_entity_render_position(solid).y, (before + after) * 0.5F), "and it draws halfway");

        nya_entity_clear();
    }

    return nya_check_failures() == 0 ? 0 : 1;
}
