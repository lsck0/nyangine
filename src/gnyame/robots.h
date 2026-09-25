/**
 * @file robots.h
 *
 * Drones that learn to fly to the player, the GDD's robot programming in miniature. Five fly a genome evolved
 * with NEAT, one flies a DQN policy, and both brains train on the job system while the drones fly the best
 * brain so far. A nav flow field around the terrain and the map gives them the next point to fly to; the brain
 * only decides how to thrust there.
 *
 * ```c
 * // registered as the "robots" system; creates everything the tick NYA_CONFIG.game.robots.enabled turns on
 * gny_robots_update(delta_time_s);
 *
 * // the steering both brains learn, and what a drone does with it every tick
 * f32 senses[GNY_ROBOT_SENSES];
 * gny_robot_sense(body, gny_robots_waypoint(robots, body.position, player, i), senses);
 * gny_robot_move(&body, gny_robot_neat_thrust(robots->brain, senses), delta_time_s);
 * ```
 *
 * The best genome and its fitness are saved to GNY_ROBOT_SAVE_FILE and flown from the first frame of the next
 * run, and every run is a row in the GNY_ROBOT_DATABASE_FILE sqlite database, which the HUD reads for its
 * record line.
 *
 * Only one training job exists at a time. While it runs it owns `neat`, `dqn` and `episode`; the main thread
 * reads them again only once nya_job_is_done says so. Everything else is the main thread's.
 * */
#pragma once

#include "nyangine-core/nyangine.h"

typedef struct GNY_RobotBody GNY_RobotBody;
typedef struct GNY_RobotRun  GNY_RobotRun;
typedef struct GNY_Robots    GNY_Robots;

/** A point mass that thrusts. The whole of the physics a brain has to learn. */
struct GNY_RobotBody {
    f32x2 position;
    f32x2 velocity;
};

/**
 * One finished training run, which is one row of GNY_ROBOT_DATABASE_FILE. The table is this struct:
 * the schema, the insert and the drift check all come from the reflection below, so growing a column
 * is adding a field here. See db/db_orm.h.
 * */
// @reflect
struct GNY_RobotRun {
    /** Assigned by the database, so a run about to be written leaves it zero. */
    u32 id; // @key

    /** Generations trained by this run and every run before it. */
    u32 generations;

    f64 fitness;
    u64 dqn_steps;
    f64 dqn_score;

    /** When the run ended, UTC. Written here rather than left to the column's default, so the value the game stored is the value the game can see. */
    char ended[NYA_CLOCK_FORMAT_MAX_LENGTH];
};

struct GNY_Robots {
    /** The DQN, the nav grid, the RNG and the database. Its own arena, freed by gny_robots_destroy. */
    NYA_Arena* allocator;

    /** Holds `brain` alone, cleared before each better genome is copied in. */
    NYA_Arena* brain_allocator;

    /*
     * Training, owned by the job while one runs.
     */
    NYA_Neat*     neat;
    NYA_NNDQN*    dqn;
    NYA_RNG*      rng;
    GNY_RobotBody episode;
    f32x2         episode_target;
    u32           episode_step;

    /** The DQN's trial score, and the step count it was last taken at. */
    f64 job_dqn_score;
    u64 job_dqn_scored_at;

    NYA_JobHandle job;

    /** What the running job was asked for, and what it took. Written before submit and by the job. */
    u32 job_generations;
    u32 job_dqn_steps;
    f64 job_ms;

    /** Fractional generations and gradient steps the rates have earned and no job has run yet. */
    f32 generation_debt;
    f32 dqn_step_debt;
    f32 train_timer_s;

    /*
     * What the drones fly and the HUD shows, the main thread's.
     */

    /** The fittest genome seen, from the save file or from this run. Null until there is one. */
    NYA_NeatNetwork* brain;
    f64              brain_fitness;

    /** Generations trained by earlier runs, so the count carries on. */
    u32 generations_before;

    u32 generation;
    u32 species;
    u64 dqn_steps;
    f32 dqn_exploration;

    /** The DQN flying the NEAT trial, so the two brains are compared on one number. */
    f64 dqn_score;

    /** The DQN drone's last thrust, held while the job has the network. */
    f32x2 dqn_thrust;

    /** The drones: GNY_ROBOT_NEAT_DRONES flying `brain`, then the one flying the DQN. */
    NYA_EntityHandle drones[GNY_ROBOT_DRONES];
    GNY_RobotBody    bodies[GNY_ROBOT_DRONES];

    /** Where each drone flew toward last tick, drawn as a short line. */
    f32x2 waypoints[GNY_ROBOT_DRONES];

    /*
     * Navigation.
     */
    NYA_NavGrid* nav;
    NYA_NavFlow* flow;

    /** The cell the flow leads to, and the terrain the grid was built for. */
    NYA_NavPoint goal;
    u64          nav_terrain_seed;

    /*
     * The run history.
     */
    NYA_Database* database;

    /** GNY_RobotRun bound to `database`. Null when there is no history, exactly as `database` is. */
    NYA_OrmTable* runs_table;

    u32 runs;
    f64 record;

    /** Seconds since the drones spawned, for their orbit. */
    f32 clock_s;
};

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * THE TASK
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/** Offset to `target` and velocity, scaled by GNY_ROBOT_SENSE_RANGE and GNY_ROBOT_MAX_SPEED and clamped to [-1, 1]. */
void gny_robot_sense(GNY_RobotBody body, f32x2 target, OUT f32 senses[GNY_ROBOT_SENSES]);

/** One step: thrust in [-1, 1] per axis accelerates, drag slows, speed caps, then the position moves. */
void gny_robot_move(GNY_RobotBody* body, f32x2 thrust, f32 delta_time_s);

/** What a genome thrusts for these senses. Zero for a null brain, so a drone with none just drifts. */
f32x2 gny_robot_neat_thrust(NYA_NeatNetwork* brain, const f32 senses[GNY_ROBOT_SENSES]);

/** A DQN action as thrust: zero for coasting, else a unit vector an eighth of a turn per action. */
f32x2 gny_robot_dqn_thrust(u32 action);

/**
 * The NEAT trial: eight scripted flights to points around the drone, some starting sideways. Scores the mean
 * closeness over every step, so arriving early and staying beats overshooting. In [0, 1).
 * */
f64 gny_robot_neat_trial(NYA_NeatNetwork* network);

/** The greedy DQN policy flying the same trial. Uses the agent's tape, so never while the job has it. */
f64 gny_robot_dqn_score(NYA_NNDQN* dqn);

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * LIFETIME
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/** Builds the trainers, loads the save and the history, and spawns the drones. Null when there is no 2D scene. */
GNY_Robots* gny_robots_create(void);

/** Waits for the running job, saves the brain and a history row, despawns the drones and frees everything. */
void gny_robots_destroy(void);

/**
 * Follows the config: creates or destroys, collects a finished job, submits the next, and moves the nav goal.
 * Registered as the "robots" system.
 * */
void gny_robots_update(f32 delta_time_s);

/** Runs the generations and gradient steps written into the robots it is handed. The job's function. */
s32 gny_robots_train_job(NYA_Job* job);

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * NAVIGATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/** Blocks every cell under the terrain or on the map's collision layer. Called when the terrain changes. */
void gny_robots_nav_build(GNY_Robots* robots);

/** The nav cell a world point falls in. Off the grid is returned as is, and nya_nav_walkable says no. */
NYA_NavPoint gny_robots_nav_cell(f32x2 world);

/** The centre of a cell, in world units. */
f32x2 gny_robots_nav_center(NYA_NavPoint cell);

/** Where a drone at `position` should fly next on its way to `goal`: along the flow, then around the goal. */
f32x2 gny_robots_waypoint(const GNY_Robots* robots, f32x2 position, f32x2 goal, u32 index);
