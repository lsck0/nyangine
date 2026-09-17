/**
 * @file entity_robot.c
 *
 * The learning drone as the world sees it: a glowing arrow with a line to where it is flying. Its body and its brain
 * are the robots system's, see robots.h.
 * */
#include "gnyame/gnyame.h"

NYA_EntityHandle gny_entity_robot_create(f32x2 position) {
    return nya_entity_spawn(
        .name      = "robot",
        .type      = GNY_ENTITY_ROBOT,
        .position  = { position.x, position.y, 0.0F },
        .state     = NYA_ENTITY_STATE_ACTIVE | NYA_ENTITY_STATE_VISIBLE,
        .on_render = nya_callback(gny_entity_robot_on_render),
        .light     = { .radius = GNY_ROBOT_SIZE * 5.0F, .intensity = 0.6F, .color = GNY_ROBOT_NEAT_COLOR }
    );
}

void gny_entity_robot_on_render(NYA_Entity* entity, NYA_Window* window) {
    GNY_Robots* robots = gny_world()->robots;
    if (robots == nullptr) return;

    for (u32 i = 0; i < GNY_ROBOT_DRONES; i++) {
        if (robots->drones[i].index != entity->handle.index || robots->drones[i].generation != entity->handle.generation) continue;

        NYA_Color     color = i < GNY_ROBOT_NEAT_DRONES ? GNY_ROBOT_NEAT_COLOR : GNY_ROBOT_DQN_COLOR;
        GNY_RobotBody body  = robots->bodies[i];

        nya_render2d_line(window, body.position, robots->waypoints[i], 1.0F, (NYA_Color){ color.r, color.g, color.b, 0.35F });

        // an arrow along the velocity, so a brain that has not learned to stop is seen sliding sideways.
        f32   angle   = atan2f(body.velocity.y, body.velocity.x);
        f32x2 forward = (f32x2){ cosf(angle), sinf(angle) } * GNY_ROBOT_SIZE;
        f32x2 side    = (f32x2){ -forward.y, forward.x } * 0.6F;

        nya_render2d_triangle(window, body.position + forward, body.position - (forward * 0.6F) + side, body.position - (forward * 0.6F) - side, color);
        return;
    }
}
