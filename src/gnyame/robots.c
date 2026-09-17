/**
 * @file robots.c
 *
 * The learning drones' brains, their training job, the nav grid they follow, and what is kept between runs. See
 * robots.h. The drone entity itself is entities/entity_robot.c.
 * */
#include "gnyame/gnyame.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API DECLARATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/** Either brain flying the trial. */
typedef f32x2 (*GNY_RobotPilot)(void* brain, const f32 senses[GNY_ROBOT_SENSES]);

/** The scripted flights both brains are scored on. See gny_robot_neat_trial. */
NYA_INTERNAL f64 _gny_robot_trial(GNY_RobotPilot pilot, void* brain);

NYA_INTERNAL f32x2 _gny_robot_neat_pilot(void* brain, const f32 senses[GNY_ROBOT_SENSES]);
NYA_INTERNAL f32x2 _gny_robot_dqn_pilot(void* brain, const f32 senses[GNY_ROBOT_SENSES]);

/** The genome evolution starts from: a bias, the four senses and two thrust outputs, with no connections. */
NYA_INTERNAL NYA_NeatNetwork* _gny_robots_seed(NYA_Arena* arena);

/** Both brains' trainers and the DQN's first practice flight. A null seed picks a random one. */
NYA_INTERNAL void _gny_robots_trainers_create(GNY_Robots* robots, u32 population, NYA_ConstCString rng_seed);

/** Starts the DQN's practice flight toward a new random point. Job side. */
NYA_INTERNAL void _gny_robots_episode_reset(GNY_Robots* robots);

/** One practice transition, remembered, and one gradient step. Job side. */
NYA_INTERNAL void _gny_robots_dqn_step(GNY_Robots* robots);

/** Takes a finished job's results: a fitter genome for the drones, and the numbers the HUD shows. */
NYA_INTERNAL void _gny_robots_collect(GNY_Robots* robots);

/** Flies every drone one tick toward `goal`. */
NYA_INTERNAL void _gny_robots_fly(GNY_Robots* robots, f32x2 goal, f32 delta_time_s);

/** Rebuilds the grid when the terrain changed and the flow when the goal moved to another cell. */
NYA_INTERNAL void _gny_robots_nav_follow(GNY_Robots* robots, f32x2 goal);

/** The best genome to the save file and the run to the history. */
NYA_INTERNAL void _gny_robots_save(GNY_Robots* robots);

/** The same back. Neither a missing file nor a missing database is an error: that is the first run. */
NYA_INTERNAL void _gny_robots_load(GNY_Robots* robots);

NYA_INTERNAL const NYA_ConstCString _GNY_ROBOT_SENSE_LABELS[GNY_ROBOT_SENSES] = { "dx", "dy", "vx", "vy" };

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * THE TASK
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

void gny_robot_sense(GNY_RobotBody body, f32x2 target, OUT f32 senses[GNY_ROBOT_SENSES]) {
    f32x2 offset   = (target - body.position) / GNY_ROBOT_SENSE_RANGE;
    f32x2 velocity = body.velocity / GNY_ROBOT_MAX_SPEED;

    senses[0] = nya_clamp(offset.x, -1.0F, 1.0F);
    senses[1] = nya_clamp(offset.y, -1.0F, 1.0F);
    senses[2] = nya_clamp(velocity.x, -1.0F, 1.0F);
    senses[3] = nya_clamp(velocity.y, -1.0F, 1.0F);
}

void gny_robot_move(GNY_RobotBody* body, f32x2 thrust, f32 delta_time_s) {
    nya_assert(body != nullptr);

    thrust = (f32x2){ nya_clamp(thrust.x, -1.0F, 1.0F), nya_clamp(thrust.y, -1.0F, 1.0F) };

    body->velocity += thrust * (GNY_ROBOT_ACCELERATION * delta_time_s);
    body->velocity *= nya_max(0.0F, 1.0F - (GNY_ROBOT_DRAG * delta_time_s));

    f32 speed = nya_vector_length(body->velocity);
    if (speed > GNY_ROBOT_MAX_SPEED) body->velocity *= GNY_ROBOT_MAX_SPEED / speed;

    body->position += body->velocity * delta_time_s;
}

f32x2 gny_robot_neat_thrust(NYA_NeatNetwork* brain, const f32 senses[GNY_ROBOT_SENSES]) {
    if (brain == nullptr) return f32x2_zero;

    // flushed every step, in the trial too, so the drones can share one genome without sharing its memory.
    nya_nn_neat_network_flush(brain);

    for (u32 i = 0; i < GNY_ROBOT_SENSES; i++) nya_nn_neat_network_set_sensor(brain, _GNY_ROBOT_SENSE_LABELS[i], senses[i]);

    nya_nn_neat_network_run(brain);

    return (f32x2){ (f32)nya_nn_neat_network_get_output(brain, "thrust_x"), (f32)nya_nn_neat_network_get_output(brain, "thrust_y") };
}

f32x2 gny_robot_dqn_thrust(u32 action) {
    nya_assert(action < GNY_ROBOT_DQN_ACTIONS);

    if (action == 0) return f32x2_zero;

    f32 angle = (f32)(action - 1) * (f32)M_PI * 0.25F;
    return (f32x2){ cosf(angle), sinf(angle) };
}

f64 gny_robot_neat_trial(NYA_NeatNetwork* network) {
    nya_assert(network != nullptr);

    return _gny_robot_trial(_gny_robot_neat_pilot, network);
}

f64 gny_robot_dqn_score(NYA_NNDQN* dqn) {
    nya_assert(dqn != nullptr);

    return _gny_robot_trial(_gny_robot_dqn_pilot, dqn);
}

f64 _gny_robot_trial(GNY_RobotPilot pilot, void* brain) {
    f64 closeness = 0.0;
    f32 senses[GNY_ROBOT_SENSES];

    for (u32 flight = 0; flight < 8; flight++) {
        f32   angle    = (f32)flight * (f32)M_PI * 0.25F;
        f32x2 heading  = { cosf(angle), sinf(angle) };
        f32   distance = GNY_ROBOT_SENSE_RANGE * (flight % 2 == 0 ? 1.0F : 0.5F);
        f32x2 target   = heading * distance;

        // half the flights start drifting sideways, so stopping a drift is part of the task, as it is on screen.
        GNY_RobotBody body = { 0 };
        if ((flight / 2) % 2 == 1) body.velocity = (f32x2){ -heading.y, heading.x } * (GNY_ROBOT_MAX_SPEED * 0.5F);

        for (u32 step = 0; step < GNY_ROBOT_TRIAL_STEPS; step++) {
            gny_robot_sense(body, target, senses);
            gny_robot_move(&body, pilot(brain, senses), GNY_ROBOT_TRAIN_DT);

            closeness += nya_max(0.0F, 1.0F - (nya_vector_length(target - body.position) / distance));
        }
    }

    return closeness / (8.0 * GNY_ROBOT_TRIAL_STEPS);
}

f32x2 _gny_robot_neat_pilot(void* brain, const f32 senses[GNY_ROBOT_SENSES]) {
    return gny_robot_neat_thrust(brain, senses);
}

f32x2 _gny_robot_dqn_pilot(void* brain, const f32 senses[GNY_ROBOT_SENSES]) {
    return gny_robot_dqn_thrust(nya_nn_dqn_act_greedy(brain, senses));
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * LIFETIME
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

GNY_Robots* gny_robots_create(void) {
    GNY_World* world = gny_world();
    if (world == nullptr || world->terrain2d == nullptr) return nullptr;

    NYA_Arena* allocator = nya_arena_create(.name = "robots");

    GNY_Robots* robots = nya_arena_alloc(allocator, sizeof(GNY_Robots));
    *robots            = (GNY_Robots){
                   .allocator       = allocator,
                   .brain_allocator = nya_arena_create(.name = "robots_brain"),
    };

    u32 population = NYA_CONFIG.game.robots.population > 0 ? NYA_CONFIG.game.robots.population : GNY_ROBOT_POPULATION;

    _gny_robots_trainers_create(robots, population, nullptr);

    NYA_EXPECT(nya_nav_grid_create(allocator, GNY_ROBOT_NAV_COLUMNS, GNY_ROBOT_NAV_ROWS, &robots->nav), "while creating the robots' nav grid");
    NYA_EXPECT(nya_nav_flow_create(allocator, robots->nav, &robots->flow), "while creating the robots' flow field");
    gny_robots_nav_build(robots);

    _gny_robots_load(robots);

    // in a row above the view, so they are seen flying in.
    NYA_Camera2DTopDown camera = gny_entity_camera_get();

    for (u32 i = 0; i < GNY_ROBOT_DRONES; i++) {
        f32x2 position = camera.position + (f32x2){ ((f32)i - (GNY_ROBOT_DRONES * 0.5F)) * 60.0F, -260.0F };

        robots->bodies[i] = (GNY_RobotBody){ .position = position };
        robots->drones[i] = gny_entity_robot_create(position);
    }

    nya_log_info("Robots up: %u genomes, %s brain (fitness %.3f), run %u.", population, robots->brain != nullptr ? "a saved" : "no",
                 robots->brain_fitness, robots->runs + 1);

    return robots;
}

void gny_robots_destroy(void) {
    GNY_World* world = gny_world();
    if (world == nullptr || world->robots == nullptr) return;

    GNY_Robots* robots = world->robots;

    // the job holds the trainers, so nothing is freed under it.
    if (robots->job != 0) {
        nya_job_wait(robots->job);
        _gny_robots_collect(robots);
    }

    _gny_robots_save(robots);

    nya_log_info("Robots down after %u generations and " FMTu64 " DQN steps; the best genome scored %.3f.", robots->generation, robots->dqn_steps,
                 robots->brain_fitness);

    for (u32 i = 0; i < GNY_ROBOT_DRONES; i++) {
        if (nya_entity_is_valid(robots->drones[i])) nya_entity_despawn(robots->drones[i]);
    }

    nya_sql_close(robots->database);
    nya_nn_neat_destroy(robots->neat);
    nya_arena_destroy(robots->brain_allocator);

    // last, because the robots live in it.
    nya_arena_destroy(robots->allocator);

    world->robots = nullptr;
}

void gny_robots_update(f32 delta_time_s) {
    nya_perf_time_this_function();

    GNY_World* world = gny_world();
    if (world == nullptr) return;

    const GNY_ConfigRobots* config = &NYA_CONFIG.game.robots;

    // read every tick, so saving the config file turns them on and off live.
    if (!config->enabled) {
        gny_robots_destroy();
        return;
    }

    if (world->robots == nullptr) world->robots = gny_robots_create();

    GNY_Robots* robots = world->robots;
    if (robots == nullptr) return;

    if (robots->job != 0 && nya_job_is_done(robots->job)) {
        _gny_robots_collect(robots);
        robots->job = 0;
    }

    // everything stops with the menu, training included.
    if (gny_modal_active()) return;

    /*
     * The player, or the view when there is no player to chase.
     */
    f32x2 goal = gny_entity_camera_get().position;

    nya_entity_foreach_kind (GNY_ENTITY_PLAYER, player) {
        goal = (f32x2){ player->position.x, player->position.y };
    }

    _gny_robots_nav_follow(robots, goal);
    _gny_robots_fly(robots, goal, delta_time_s);

    /*
     * The next job, once the last is back and the interval has passed.
     */
    f32 generations_per_second = config->generations_per_second > 0.0F ? config->generations_per_second : GNY_ROBOT_GENERATIONS_PER_SECOND;
    f32 dqn_steps_per_second   = config->dqn_steps_per_second > 0.0F ? config->dqn_steps_per_second : GNY_ROBOT_DQN_STEPS_PER_SECOND;

    // capped, so a slow job owes at most one job's worth instead of banking work it will never catch up on.
    robots->generation_debt = nya_min(robots->generation_debt + (generations_per_second * delta_time_s), (f32)GNY_ROBOT_MAX_GENERATIONS_PER_JOB);
    robots->dqn_step_debt   = nya_min(robots->dqn_step_debt + (dqn_steps_per_second * delta_time_s), (f32)GNY_ROBOT_MAX_DQN_STEPS_PER_JOB);
    robots->train_timer_s  += delta_time_s;

    if (robots->job != 0 || robots->train_timer_s < GNY_ROBOT_TRAIN_INTERVAL_S) return;

    robots->train_timer_s   = 0.0F;
    robots->job_generations = (u32)robots->generation_debt;
    robots->job_dqn_steps   = (u32)robots->dqn_step_debt;

    if (robots->job_generations == 0 && robots->job_dqn_steps == 0) return;

    robots->generation_debt -= (f32)robots->job_generations;
    robots->dqn_step_debt   -= (f32)robots->job_dqn_steps;

    robots->job = nya_job_submit((NYA_Job){
        .priority = NYA_JOB_PRIORITY_LOW,
        .function = nya_callback(gny_robots_train_job),
        .in_data  = robots,
    });
}

s32 gny_robots_train_job(NYA_Job* job) {
    nya_assert(job != nullptr && job->in_data != nullptr);

    GNY_Robots* robots = job->in_data;
    u64         start  = nya_clock_get_monotonic_ns();

    for (u32 i = 0; i < robots->job_generations; i++) nya_nn_neat_step(robots->neat);
    for (u32 i = 0; i < robots->job_dqn_steps; i++) _gny_robots_dqn_step(robots);

    // scored now and then rather than every job: the trial is several hundred forward passes.
    u64 steps = nya_nn_dqn_train_step_count(robots->dqn);
    if (steps / GNY_ROBOT_DQN_SCORE_EVERY != robots->job_dqn_scored_at / GNY_ROBOT_DQN_SCORE_EVERY) {
        robots->job_dqn_score     = gny_robot_dqn_score(robots->dqn);
        robots->job_dqn_scored_at = steps;
    }

    robots->job_ms = nya_time_ns_to_ms(nya_clock_get_monotonic_ns() - start);

    return 0;
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * NAVIGATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

void gny_robots_nav_build(GNY_Robots* robots) {
    nya_assert(robots != nullptr);

    GNY_World*         world = gny_world();
    const NYA_Tilemap* map   = world->tilemap;
    u32                solid = map != nullptr ? nya_tilemap_layer_find(map, "collision") : NYA_TILEMAP_LAYER_NONE;

    for (s32 y = 0; y < (s32)robots->nav->height; y++) {
        for (s32 x = 0; x < (s32)robots->nav->width; x++) {
            f32x2 min = { -GNY_TERRAIN_HALF_WIDTH + ((f32)x * GNY_ROBOT_NAV_CELL), GNY_ROBOT_NAV_TOP + ((f32)y * GNY_ROBOT_NAV_CELL) };
            f32x2 max = min + GNY_ROBOT_NAV_CELL;

            // along the bottom edge, with a drone's size of clearance, so a drone in the cell stays out of the ground.
            b8 blocked = false;
            for (u32 sample = 0; sample < 3; sample++) {
                f32 ground = nya_terrain2d_height_at(world->terrain2d, min.x + ((f32)sample * GNY_ROBOT_NAV_CELL * 0.5F));
                if (max.y + GNY_ROBOT_SIZE > ground) blocked = true;
            }

            if (!blocked && solid != NYA_TILEMAP_LAYER_NONE) {
                f32x2 first = nya_tilemap_world_to_tile(map, min);
                f32x2 last  = nya_tilemap_world_to_tile(map, max);

                for (s32 ty = (s32)floorf(first.y); ty < (s32)ceilf(last.y) && !blocked; ty++) {
                    for (s32 tx = (s32)floorf(first.x); tx < (s32)ceilf(last.x); tx++) {
                        if (nya_tilemap_tile_at(map, solid, tx, ty) != 0) blocked = true;
                    }
                }
            }

            nya_nav_cost_set(robots->nav, x, y, blocked ? NYA_NAV_BLOCKED : NYA_NAV_COST_DEFAULT);
        }
    }

    robots->nav_terrain_seed = world->terrain_seed;

    // a goal no cell can have, so the next follow builds the flow over the new grid.
    robots->goal = (NYA_NavPoint){ -1, -1 };
}

NYA_NavPoint gny_robots_nav_cell(f32x2 world) {
    return (NYA_NavPoint){
        .x = (s32)floorf((world.x + GNY_TERRAIN_HALF_WIDTH) / GNY_ROBOT_NAV_CELL),
        .y = (s32)floorf((world.y - GNY_ROBOT_NAV_TOP) / GNY_ROBOT_NAV_CELL),
    };
}

f32x2 gny_robots_nav_center(NYA_NavPoint cell) {
    return (f32x2){
        -GNY_TERRAIN_HALF_WIDTH + (((f32)cell.x + 0.5F) * GNY_ROBOT_NAV_CELL),
        GNY_ROBOT_NAV_TOP + (((f32)cell.y + 0.5F) * GNY_ROBOT_NAV_CELL),
    };
}

f32x2 gny_robots_waypoint(const GNY_Robots* robots, f32x2 position, f32x2 goal, u32 index) {
    nya_assert(robots != nullptr);

    // close in, each drone takes its own place on a circle rather than all of them the one point.
    if (nya_vector_length(goal - position) < GNY_ROBOT_ORBIT_RADIUS * 2.0F) {
        f32   angle = (robots->clock_s * GNY_ROBOT_ORBIT_SPEED) + ((f32)index * 2.0F * (f32)M_PI / (f32)GNY_ROBOT_DRONES);
        f32x2 spot  = goal + ((f32x2){ cosf(angle), sinf(angle) } * GNY_ROBOT_ORBIT_RADIUS);

        NYA_NavPoint cell = gny_robots_nav_cell(spot);
        return nya_nav_walkable(robots->nav, cell.x, cell.y) ? spot : goal;
    }

    NYA_NavPoint cell = gny_robots_nav_cell(position);

    // above or beside the grid there is nothing in the way.
    if (cell.y < 0 || cell.x < 0 || cell.x >= (s32)robots->nav->width) return goal;

    // under a regenerated terrain or inside the map, where up is always the way out.
    if (!nya_nav_walkable(robots->nav, cell.x, cell.y)) return position - (f32x2){ 0.0F, GNY_ROBOT_NAV_CELL };

    if (nya_nav_flow_distance(robots->flow, cell.x, cell.y) == NYA_NAV_UNREACHABLE) return position;

    for (u32 i = 0; i < GNY_ROBOT_NAV_LOOKAHEAD; i++) cell = nya_nav_flow_step(robots->flow, cell);

    // the goal's own cell is flown through to the goal, not to the cell's centre.
    if (cell.x == robots->goal.x && cell.y == robots->goal.y) return goal;

    return gny_robots_nav_center(cell);
}

void _gny_robots_nav_follow(GNY_Robots* robots, f32x2 goal) {
    if (robots->nav_terrain_seed != gny_world()->terrain_seed) gny_robots_nav_build(robots);

    NYA_NavPoint cell = gny_robots_nav_cell(goal);
    cell.x            = nya_clamp(cell.x, 0, (s32)robots->nav->width - 1);
    cell.y            = nya_clamp(cell.y, 0, (s32)robots->nav->height - 1);

    // a player below ground is chased from the air above them.
    while (cell.y > 0 && !nya_nav_walkable(robots->nav, cell.x, cell.y)) cell.y--;

    if (cell.x == robots->goal.x && cell.y == robots->goal.y) return;

    // the whole grid, so only when the goal crosses into another cell.
    nya_nav_flow_build(robots->flow, cell);
    robots->goal = cell;
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

void _gny_robots_trainers_create(GNY_Robots* robots, u32 population, NYA_ConstCString rng_seed) {
    robots->neat = nya_nn_neat_create((NYA_NeatConfig){
        .seed                = _gny_robots_seed(robots->allocator),
        .trial_function      = gny_robot_neat_trial,
        .activation_function = nya_nn_neat_tanh,
        .population_size     = population,
        .rng_seed            = rng_seed,

        // a seed with no connections has nothing to tune until mutation adds some, so they are added more often.
        .mutation_add_connection_chance = 0.2,
        .target_species_count           = 6,
    });

    robots->rng = nya_rng_create_in(robots->allocator, rng_seed);

    robots->dqn = nya_nn_dqn_create(robots->allocator, (NYA_NNDQNConfig){
        .state_size   = GNY_ROBOT_SENSES,
        .action_count = GNY_ROBOT_DQN_ACTIONS,
        .layers       = {
            { .kind = NYA_NN_LAYER_LINEAR, .units = 32 },
            { .kind = NYA_NN_LAYER_RELU },
            { .kind = NYA_NN_LAYER_LINEAR, .units = 32 },
            { .kind = NYA_NN_LAYER_RELU },
        },
        .layer_count       = 4,
        .replay_capacity   = 8192,
        .batch_size        = 32,
        .discount          = 0.95F,
        .exploration_steps = 4000,
        .learning_starts   = 256,
        .rng_seed          = rng_seed,
    });

    _gny_robots_episode_reset(robots);
}

NYA_NeatNetwork* _gny_robots_seed(NYA_Arena* arena) {
    NYA_NeatNetwork* seed = nya_nn_neat_network_create(arena);

    nya_nn_neat_network_push_bias(seed, "bias");
    for (u32 i = 0; i < GNY_ROBOT_SENSES; i++) nya_nn_neat_network_push_sensor(seed, _GNY_ROBOT_SENSE_LABELS[i]);
    nya_nn_neat_network_push_output(seed, "thrust_x");
    nya_nn_neat_network_push_output(seed, "thrust_y");

    return seed;
}

void _gny_robots_episode_reset(GNY_Robots* robots) {
    NYA_RNGDistribution unit = { .type = NYA_RNG_DISTRIBUTION_UNIFORM, .uniform = { .min = 0.0, .max = 1.0 } };

    f32 angle    = nya_rng_sample_f32(robots->rng, unit) * 2.0F * (f32)M_PI;
    f32 distance = GNY_ROBOT_SENSE_RANGE * (0.2F + (0.8F * nya_rng_sample_f32(robots->rng, unit)));
    f32 drift    = GNY_ROBOT_MAX_SPEED * 0.5F * nya_rng_sample_f32(robots->rng, unit);

    robots->episode        = (GNY_RobotBody){ .velocity = (f32x2){ -sinf(angle), cosf(angle) } * drift };
    robots->episode_target = (f32x2){ cosf(angle), sinf(angle) } * distance;
    robots->episode_step   = 0;
}

void _gny_robots_dqn_step(GNY_Robots* robots) {
    f32 state[GNY_ROBOT_SENSES];
    f32 next_state[GNY_ROBOT_SENSES];

    gny_robot_sense(robots->episode, robots->episode_target, state);
    f32 before = nya_vector_length(robots->episode_target - robots->episode.position);

    u32 action = nya_nn_dqn_act(robots->dqn, state);
    gny_robot_move(&robots->episode, gny_robot_dqn_thrust(action), GNY_ROBOT_TRAIN_DT);
    robots->episode_step++;

    f32 after   = nya_vector_length(robots->episode_target - robots->episode.position);
    b8  reached = after < GNY_ROBOT_REACH;

    // progress toward the point, a little against the clock, and a bonus for arriving.
    f32 reward = (((before - after) / GNY_ROBOT_SENSE_RANGE) * 4.0F) - 0.005F + (reached ? 1.0F : 0.0F);

    gny_robot_sense(robots->episode, robots->episode_target, next_state);
    nya_nn_dqn_observe(robots->dqn, state, action, reward, next_state, reached);

    // running out of steps is not terminal: the point was still reachable, the episode just ends.
    if (reached || robots->episode_step >= GNY_ROBOT_EPISODE_STEPS) _gny_robots_episode_reset(robots);

    (void)nya_nn_dqn_train_step(robots->dqn);
}

void _gny_robots_collect(GNY_Robots* robots) {
    NYA_NeatNetwork* best = nya_nn_neat_best(robots->neat);

    if (best != nullptr && best->fitness_raw > robots->brain_fitness) {
        nya_arena_free_all(robots->brain_allocator);

        robots->brain         = nya_nn_neat_network_clone(robots->brain_allocator, best);
        robots->brain_fitness = best->fitness_raw;
    }

    robots->generation      = nya_nn_neat_generation(robots->neat);
    robots->species         = nya_nn_neat_species_count(robots->neat);
    robots->dqn_steps       = nya_nn_dqn_train_step_count(robots->dqn);
    robots->dqn_exploration = nya_nn_dqn_exploration(robots->dqn);
    robots->dqn_score       = robots->job_dqn_score;
}

void _gny_robots_fly(GNY_Robots* robots, f32x2 goal, f32 delta_time_s) {
    robots->clock_s += delta_time_s;

    f32 senses[GNY_ROBOT_SENSES];

    for (u32 i = 0; i < GNY_ROBOT_DRONES; i++) {
        NYA_Entity* drone = nya_entity_get(robots->drones[i]);
        if (drone == nullptr) continue;

        GNY_RobotBody* body = &robots->bodies[i];

        // a brain that has not learned yet can fly off the map, and would be gone until it had.
        b8 recalled = nya_vector_length(goal - body->position) > GNY_ROBOT_RECALL_DISTANCE;
        if (recalled) *body = (GNY_RobotBody){ .position = goal + (f32x2){ 0.0F, -GNY_ROBOT_RECALL_HEIGHT } };

        robots->waypoints[i] = gny_robots_waypoint(robots, body->position, goal, i);
        gny_robot_sense(*body, robots->waypoints[i], senses);

        f32x2 thrust = f32x2_zero;

        if (i < GNY_ROBOT_NEAT_DRONES) {
            thrust = gny_robot_neat_thrust(robots->brain, senses);
        } else {
            // the network is the job's while it runs, so this drone holds its last thrust until the job is back.
            if (robots->job == 0) robots->dqn_thrust = gny_robot_dqn_thrust(nya_nn_dqn_act_greedy(robots->dqn, senses));
            thrust = robots->dqn_thrust;
        }

        gny_robot_move(body, thrust, delta_time_s);

        drone->position = (f32x3){ body->position.x, body->position.y, 0.0F };

        // brought back, not flown back, so it is not drawn streaking across the map.
        if (recalled) nya_entity_transform_snap(drone);
    }
}

void _gny_robots_save(GNY_Robots* robots) {
    u32 generations = robots->generations_before + robots->generation;

    if (robots->brain != nullptr) {
        NYA_Arena scratch = nya_arena_create_on_stack(.name = "robots_save");
        defer     nya_arena_destroy_on_stack(&scratch);

        NYA_Object* save = nya_object_create(&scratch);
        nya_object_set(save, NYA_SAVE_VERSION_KEY, (NYA_Value){ .type = NYA_TYPE_U32, .as_u32 = GNY_ROBOT_SAVE_VERSION });
        nya_object_set(save, "generations", (NYA_Value){ .type = NYA_TYPE_U32, .as_u32 = generations });
        nya_object_set(save, "fitness", (NYA_Value){ .type = NYA_TYPE_F64, .as_f64 = robots->brain_fitness });
        nya_object_set(save, "genome", (NYA_Value){ .type = NYA_TYPE_OBJECT, .as_object = *nya_nn_neat_network_to_object(&scratch, robots->brain) });

        NYA_Error saved = nya_save_write(GNY_ROBOT_SAVE_FILE, save, NYA_SERDE_NONE);
        if (!saved.ok) nya_log_warn("Could not save the robots' brain: %s", (NYA_ConstCString)saved.message);
    }

    // a row only for a run that trained, so opening and closing the scene does not fill the history.
    if (robots->database == nullptr || robots->generation == 0) return;

    NYA_SqlValue row[] = {
        nya_sql_s64((s64)generations),
        nya_sql_f64(robots->brain_fitness),
        nya_sql_s64((s64)robots->dqn_steps),
        nya_sql_f64(robots->dqn_score),
    };

    NYA_Error inserted = nya_sql_exec_bound(robots->database, "INSERT INTO runs (generations, fitness, dqn_steps, dqn_score) VALUES (?, ?, ?, ?)",
                                            row, nya_carray_length(row));
    if (!inserted.ok) nya_log_warn("Could not record the robots' run: %s", (NYA_ConstCString)inserted.message);
}

void _gny_robots_load(GNY_Robots* robots) {
    NYA_Arena scratch = nya_arena_create_on_stack(.name = "robots_load");
    defer     nya_arena_destroy_on_stack(&scratch);

    NYA_Object* save = nullptr;
    NYA_Error   read = nya_save_read(&scratch, GNY_ROBOT_SAVE_FILE, NYA_SERDE_NONE, &save);

    if (!read.ok && read.kind != NYA_ERROR_NOT_FOUND) nya_log_warn("Could not read the robots' brain: %s", (NYA_ConstCString)read.message);

    // an older format is dropped rather than guessed at: the drones just learn again.
    if (read.ok && nya_save_version(save) == GNY_ROBOT_SAVE_VERSION) {
        NYA_Value* genome      = nya_object_get(save, "genome");
        NYA_Value* fitness     = nya_object_get(save, "fitness");
        NYA_Value* generations = nya_object_get(save, "generations");

        b8 whole = genome != nullptr && genome->type == NYA_TYPE_OBJECT && fitness != nullptr && fitness->type == NYA_TYPE_F64;

        if (whole && nya_nn_neat_network_from_object(robots->brain_allocator, &genome->as_object, nya_nn_neat_tanh, &robots->brain).ok) {
            robots->brain_fitness      = fitness->as_f64;
            robots->generations_before = generations != nullptr && generations->type == NYA_TYPE_U32 ? generations->as_u32 : 0;
        }
    }

    /*
     * The history. The game plays on without one.
     */
    NYA_Error opened = nya_save_database_open(robots->allocator, GNY_ROBOT_DATABASE_FILE, &robots->database);

    if (opened.ok) {
        opened = nya_sql_exec(robots->database, "CREATE TABLE IF NOT EXISTS runs (id INTEGER PRIMARY KEY, generations INTEGER, fitness REAL, "
                                                "dqn_steps INTEGER, dqn_score REAL, ended TEXT DEFAULT CURRENT_TIMESTAMP)");
    }

    if (!opened.ok) {
        nya_log_warn("No robot run history: %s", (NYA_ConstCString)opened.message);
        nya_sql_close(robots->database);
        robots->database = nullptr;
        return;
    }

    NYA_SqlResult result = { 0 };
    if (!nya_sql_query(robots->database, &scratch, "SELECT COUNT(*) AS runs, MAX(fitness) AS record FROM runs", nullptr, 0, &result).ok) return;
    if (result.rows->length == 0) return;

    NYA_Value* runs   = nya_object_get(result.rows->items[0], "runs");
    NYA_Value* record = nya_object_get(result.rows->items[0], "record");

    if (runs != nullptr && runs->type == NYA_TYPE_S64) robots->runs = (u32)runs->as_s64;
    if (record != nullptr && record->type == NYA_TYPE_F64) robots->record = record->as_f64;
}
