/**
 * The learning drones, headless: the steering task, NEAT evolving on it, the DQN scored on it, the nav grid around
 * the terrain, and a whole run through the job system, the save file and the sqlite history.
 **/

#include "nyangine/nyangine.c"
#include "gnyame/gnyame.c"

#include "SDL3/SDL_init.h"

/** Robot entities alive. */
static u32 drones(void) {
    u32 count = 0;
    nya_entity_foreach_kind (GNY_ENTITY_ROBOT, drone) count++;
    return count;
}

s32 main(void) {
    SDL_SetHintWithPriority(SDL_HINT_VIDEO_DRIVER, "offscreen", SDL_HINT_OVERRIDE);

    // the brain and the history are written under the save root, so a scratch one keeps the player's out of this.
    NYA_Arena*  scratch   = nya_arena_create(.name = "test_robots_scratch");
    NYA_String* temp_root = nullptr;
    NYA_EXPECT(nya_filesystem_temp_directory(scratch, &temp_root));

    NYA_CString data_home = nya_string_to_cstring(scratch, nya_path_join(scratch, nya_string_to_cstring(scratch, temp_root), "gnyame-test-robots"));
    (void)nya_filesystem_delete_recursive(data_home);

    nya_assert(nya_host_environment_set("XDG_DATA_HOME", data_home));
    nya_assert(nya_host_environment_set("APPDATA", data_home));

    _NYA_APP_INSTANCE = (NYA_App){ .initialized = true, .options = { _NYA_APP_DEFAULT_OPTIONS } };

    b8 sdl_ok = SDL_Init(0);
    nya_assert(sdl_ok, "SDL_Init failed: %s", SDL_GetError());

    NYA_EXPECT(nya_system_save_init());
    nya_system_callback_init();
    NYA_EXPECT(nya_system_events_init());
    NYA_EXPECT(nya_system_job_init());
    nya_system_asset_init();
    nya_system_window_init();

    NYA_World* engine_world = nya_world_create();
    (void)nya_world_set(engine_world);

    defer nya_arena_destroy(scratch);
    defer nya_system_save_deinit();
    defer nya_system_callback_deinit();
    defer nya_system_events_deinit();
    defer nya_system_job_deinit();
    defer nya_system_asset_deinit();
    defer nya_system_window_deinit();
    defer nya_world_destroy(engine_world);

    GNY_World* world = nya_arena_alloc(engine_world->allocator, sizeof(GNY_World));
    *world           = (GNY_World){ .window_main = NYA_WINDOW_HANDLE_NONE, .terrain = NYA_ENTITY_HANDLE_NONE, .terrain_seed = 1 };
    nya_world_user_data_set(world);

    // a window with no menu on it, so the drones are not paused.
    world->window_main = nya_window_create("robots", 640, 360, NYA_WINDOW_NONE);
    nya_assert(nya_window_is_valid(world->window_main));

    gny_terrain_generate(world->terrain_seed);

    // ── The task: senses are scaled and clamped, speed is capped, and a missing brain does not thrust.
    {
        f32 senses[GNY_ROBOT_SENSES];
        gny_robot_sense((GNY_RobotBody){ .velocity = { GNY_ROBOT_MAX_SPEED * 3.0F, 0.0F } }, (f32x2){ GNY_ROBOT_SENSE_RANGE * 0.5F, -1e6F }, senses);
        nya_check(senses[0] == 0.5F && senses[1] == -1.0F, "the offset is scaled and clamped, got %f %f", (f64)senses[0], (f64)senses[1]);
        nya_check(senses[2] == 1.0F && senses[3] == 0.0F, "and so is the velocity");

        GNY_RobotBody body = { 0 };
        for (u32 i = 0; i < 600; i++) gny_robot_move(&body, (f32x2){ 5.0F, 0.0F }, GNY_ROBOT_TRAIN_DT);
        nya_check(nya_vector_length(body.velocity) <= GNY_ROBOT_MAX_SPEED + 1e-3F, "full thrust tops out at the speed cap, got %f",
                  (f64)nya_vector_length(body.velocity));

        nya_check(nya_vector_length(gny_robot_neat_thrust(nullptr, senses)) == 0.0F, "no brain, no thrust");
        nya_check(nya_vector_length(gny_robot_dqn_thrust(0)) == 0.0F, "action zero coasts");
        for (u32 action = 1; action < GNY_ROBOT_DQN_ACTIONS; action++) {
            nya_check(fabsf(nya_vector_length(gny_robot_dqn_thrust(action)) - 1.0F) < 1e-4F, "every other action is a unit thrust");
        }
    }

    // ── Both brains get better at the trial, which is deterministic: NEAT within a few generations, the DQN within a
    //    few thousand gradient steps.
    {
        NYA_Arena* arena = nya_arena_create(.name = "test_robots_trainers");
        defer      nya_arena_destroy(arena);

        GNY_Robots robots = { .allocator = arena };
        _gny_robots_trainers_create(&robots, 24, "B07");
        defer nya_nn_neat_destroy(robots.neat);

        nya_nn_neat_step(robots.neat);
        f64 first = nya_nn_neat_fitness_max(robots.neat);

        for (u32 generation = 1; generation < 15; generation++) nya_nn_neat_step(robots.neat);
        f64 later = nya_nn_neat_fitness_max(robots.neat);

        nya_check(later > first && later > 0.3, "NEAT fitness climbs, from %f to %f", first, later);

        NYA_NeatNetwork* best = nya_nn_neat_best(robots.neat);
        nya_check(gny_robot_neat_trial(best) == gny_robot_neat_trial(best), "the same genome scores the same twice");
        nya_check(gny_robot_neat_trial(best) < 1.0, "and no score is perfect, since every flight starts away from its point");

        f64 untrained = gny_robot_dqn_score(robots.dqn);
        for (u32 step = 0; step < 3000; step++) _gny_robots_dqn_step(&robots);
        f64 trained = gny_robot_dqn_score(robots.dqn);

        nya_check(trained > untrained + 0.05, "the DQN's score climbs too, from %f to %f", untrained, trained);
    }

    // ── The nav grid blocks the ground and leads a drone out of it and toward the goal.
    {
        NYA_Arena* arena = nya_arena_create(.name = "test_robots_nav");
        defer      nya_arena_destroy(arena);

        GNY_Robots robots = { .allocator = arena };
        NYA_EXPECT(nya_nav_grid_create(arena, GNY_ROBOT_NAV_COLUMNS, GNY_ROBOT_NAV_ROWS, &robots.nav));
        NYA_EXPECT(nya_nav_flow_create(arena, robots.nav, &robots.flow));
        gny_robots_nav_build(&robots);

        f32x2 ground = { 0.0F, nya_terrain2d_height_at(world->terrain2d, 0.0F) + 30.0F };
        f32x2 sky    = { 0.0F, GNY_ROBOT_NAV_TOP + GNY_ROBOT_NAV_CELL };

        NYA_NavPoint buried = gny_robots_nav_cell(ground);
        NYA_NavPoint open   = gny_robots_nav_cell(sky);
        nya_check(!nya_nav_walkable(robots.nav, buried.x, buried.y), "a cell under the terrain is blocked");
        nya_check(nya_nav_walkable(robots.nav, open.x, open.y), "a cell in the sky is open");

        f32x2 center = gny_robots_nav_center(open);
        NYA_NavPoint again = gny_robots_nav_cell(center);
        nya_check(again.x == open.x && again.y == open.y, "a cell's centre is in that cell");

        f32x2 goal = { -1200.0F, GNY_ROBOT_NAV_TOP + (GNY_ROBOT_NAV_CELL * 4.0F) };
        _gny_robots_nav_follow(&robots, goal);

        f32x2 up = gny_robots_waypoint(&robots, ground, goal, 0);
        nya_check(up.y < ground.y && up.x == ground.x, "a buried drone climbs straight out, got " FMTf32x2, FMTf32x2_ARG(up));

        f32x2        start = { 1200.0F, GNY_ROBOT_NAV_TOP + (GNY_ROBOT_NAV_CELL * 6.0F) };
        f32x2        next  = gny_robots_waypoint(&robots, start, goal, 0);
        NYA_NavPoint from  = gny_robots_nav_cell(start);
        NYA_NavPoint to    = gny_robots_nav_cell(next);
        nya_check(nya_nav_flow_distance(robots.flow, to.x, to.y) < nya_nav_flow_distance(robots.flow, from.x, from.y),
                  "a flying drone's waypoint is closer to the goal along the flow");

        f32x2 near  = goal + (f32x2){ 20.0F, 0.0F };
        f32   orbit = nya_vector_length(gny_robots_waypoint(&robots, near, goal, 3) - goal);
        nya_check(fabsf(orbit - GNY_ROBOT_ORBIT_RADIUS) < 1e-2F, "close in, it circles the goal, got %f", (f64)orbit);
    }

    // ── A whole run: the config turns the drones on, a job trains, and turning them off saves and records it.
    {
        NYA_CONFIG.game.robots = (GNY_ConfigRobots){ .enabled = true, .population = 12, .generations_per_second = 8.0F, .dqn_steps_per_second = 64.0F };

        gny_robots_update(GNY_ROBOT_TRAIN_INTERVAL_S);
        GNY_Robots* robots = world->robots;

        nya_check(robots != nullptr && drones() == GNY_ROBOT_DRONES, "enabling spawns every drone, got %u", drones());
        nya_check(robots->brain == nullptr && robots->runs == 0, "a first run has no brain and no history");
        nya_check(robots->job != 0 && robots->job_generations == 2 && robots->job_dqn_steps == 16, "and submits the job it has earned, %u and %u",
                  robots->job_generations, robots->job_dqn_steps);

        nya_job_wait(robots->job);
        gny_robots_update(0.0F);
        nya_check(robots->job == 0 && robots->generation == 2, "a finished job is collected, generation %u", robots->generation);
        nya_check(nya_nn_dqn_replay_count(robots->dqn) == 16, "the DQN practised a step for each gradient step it was owed");

        // the game seeds evolution randomly, and the first generations can all score zero, so train until one does not.
        for (u32 i = 0; i < 64 && robots->brain == nullptr; i++) {
            gny_robots_update(GNY_ROBOT_TRAIN_INTERVAL_S);
            nya_job_wait(robots->job);
        }

        gny_robots_update(0.0F);
        nya_check(robots->brain != nullptr && robots->brain_fitness > 0.0, "the fittest genome is what the drones fly");

        /*
         * Over a horizon, not in one tick, and any drone rather than all of them.
         *
         * `brain_fitness > 0` above is what makes this sound: a genome only scores above zero by flying
         * toward the player during its test flights, so something it flies has to move eventually. One
         * tick does not follow from that — a genome may thrust to nothing on any given tick, and which
         * genome wins depends on how many generations the training job got through, which under a loaded
         * parallel test run is not the same number twice. Asserting a single tick failed about one run in
         * ten for that reason alone.
         */
        f32x2 before[GNY_ROBOT_DRONES];
        for (u32 i = 0; i < GNY_ROBOT_DRONES; i++) before[i] = robots->bodies[i].position;

        b8 moved = false;
        for (u32 tick = 0; tick < GNY_ROBOT_MOVE_HORIZON_TICKS && !moved; tick++) {
            gny_robots_update(GNY_ROBOT_TRAIN_DT);
            for (u32 i = 0; i < GNY_ROBOT_DRONES; i++) moved |= nya_vector_length(robots->bodies[i].position - before[i]) > 0.0F;
        }

        nya_check(moved, "the drones move");

        u32 generation = robots->generation;
        f64 fitness    = robots->brain_fitness;

        NYA_CONFIG.game.robots.enabled = false;
        gny_robots_update(GNY_ROBOT_TRAIN_DT);
        nya_check(world->robots == nullptr && drones() == 0, "disabling frees everything, %u drones left", drones());
        nya_check(nya_save_exists(GNY_ROBOT_SAVE_FILE), "and saves the brain");

        NYA_CONFIG.game.robots.enabled = true;
        gny_robots_update(0.0F);
        robots = world->robots;

        nya_check(robots->brain != nullptr && robots->brain_fitness == fitness, "the next run flies the saved brain, fitness %f", robots->brain_fitness);
        nya_check(robots->generations_before == generation, "carries the generation count on");
        nya_check(robots->runs == 1 && robots->record == fitness, "and reads the first run back from the history, runs %u", robots->runs);

        gny_robots_destroy();
        nya_check(world->robots == nullptr, "destroying twice is fine");
        gny_robots_destroy();
    }

    return nya_check_failures() == 0 ? 0 : 1;
}
