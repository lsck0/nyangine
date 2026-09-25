/**
 * Fluid: the grid's shape, the invariants a Navier-Stokes step has to keep (bounded fields, no
 * divergence, buoyancy along the volume's own up axis), obstacles, and the determinism the
 * simulation harness rests on.
 **/

#include "nyangine-core/nyangine.c"
#include "nyangine-core/nyangine.h"

#include "SDL3/SDL_init.h"

/** The step every test integrates with, and the engine's own tick. */
#define STEP_S (1.0F / 60.0F)

/**
 * How much divergence a bounded pressure solve may leave, as a share of the fastest thing in the
 * field. Measured at 1.6% on the benchmark's 32x48x32 grid; ten percent is loose enough that a
 * different grid or a different compiler does not make this flap, and tight enough that a projection
 * which stopped working fails it. See render_fluid.h on what the collocated scheme cannot remove.
 * */
#define DIVERGENCE_SHARE_MAX 0.10F

/** Steps the simulation section takes. Long enough for a plume to cross the grid several times. */
#define SIMULATION_STEPS 600

/** What the render options are stored on. Static, since a window is far larger than the stack wants. */
static NYA_Window window;

/** What a simulated fluid runs on, handed through NYA_SimulationRun.user_data. */
typedef struct FluidScenario FluidScenario;

struct FluidScenario {
    NYA_Fluid* two_dimensional;
    NYA_Fluid* three_dimensional;

    /** Every checksum the run produced, so a replay can be compared step for step. */
    u64 checksum;
};

/** The total density in a volume's interior. Advection moves it about; nothing may create it. */
static f32 interior_density(const NYA_Fluid* fluid) {
  f32 total = 0.0F;

  for (u32 k = 1; k <= fluid->depth; k++) {
    for (u32 j = 1; j <= fluid->height; j++) {
      for (u32 i = 1; i <= fluid->width; i++) {
        u32 index = i + (j * fluid->stride_x) + (k * fluid->stride_x * fluid->stride_y);
        total += fluid->density[index];
      }
    }
  }

  return total;
}

/** The largest |divergence| anywhere in the interior, which is what the projection drives to zero. */
static f32 worst_divergence(const NYA_Fluid* fluid) {
  u32 step_x = 1;
  u32 step_y = fluid->stride_x;
  u32 step_z = fluid->stride_x * fluid->stride_y;

  f32 worst = 0.0F;

  for (u32 k = 1; k <= fluid->depth; k++) {
    for (u32 j = 1; j <= fluid->height; j++) {
      for (u32 i = 1; i <= fluid->width; i++) {
        u32 index = i + (j * step_y) + (k * step_z);

        f32 divergence = (fluid->velocity_x[index + step_x] - fluid->velocity_x[index - step_x]) +
                         (fluid->velocity_y[index + step_y] - fluid->velocity_y[index - step_y]) +
                         (fluid->velocity_z[index + step_z] - fluid->velocity_z[index - step_z]);

        worst = nya_max(worst, fabsf(divergence * 0.5F));
      }
    }
  }

  return worst;
}

/** The fastest anything is moving, which the divergence left behind is judged against. */
static f32 worst_speed(const NYA_Fluid* fluid) {
  f32 worst = 0.0F;

  for (u32 index = 0; index < fluid->cell_count; index++) {
    f32 speed = fabsf(fluid->velocity_x[index]) + fabsf(fluid->velocity_y[index]) + fabsf(fluid->velocity_z[index]);
    worst     = nya_max(worst, speed);
  }

  return worst;
}

/** The density-weighted centre of the interior, in world units. Zero density reads as the origin. */
static f32x3 density_centre(const NYA_Fluid* fluid) {
  f32   cell_size = fluid->options.cell_size;
  f32   total     = 0.0F;
  f32x3 weighted  = f32x3_zero;

  for (u32 k = 1; k <= fluid->depth; k++) {
    for (u32 j = 1; j <= fluid->height; j++) {
      for (u32 i = 1; i <= fluid->width; i++) {
        u32 index   = i + (j * fluid->stride_x) + (k * fluid->stride_x * fluid->stride_y);
        f32 density = fluid->density[index];

        if (density <= 0.0F) continue;

        f32x3 position = {
          fluid->options.origin.x + (((f32)i - 0.5F) * cell_size),
          fluid->options.origin.y + (((f32)j - 0.5F) * cell_size),
          fluid->options.origin.z + (((f32)k - 0.5F) * cell_size),
        };

        weighted += position * density;
        total += density;
      }
    }
  }

  if (total <= 0.0F) return f32x3_zero;

  return weighted / total;
}

/** Nothing in any field may become infinite or undefined, whatever was done to it. */
static b8 fields_are_finite(const NYA_Fluid* fluid) {
  for (u32 index = 0; index < fluid->cell_count; index++) {
    if (!isfinite(fluid->density[index])) return false;
    if (!isfinite(fluid->temperature[index])) return false;
    if (!isfinite(fluid->velocity_x[index])) return false;
    if (!isfinite(fluid->velocity_y[index])) return false;
    if (!isfinite(fluid->velocity_z[index])) return false;
  }

  return true;
}

/* THE SIMULATED RUN */

static void action_emit(NYA_SimulationRun* run) {
  FluidScenario* scenario = run->user_data;

  NYA_Fluid* fluid = nya_simulation_chance(run, 50) ? scenario->two_dimensional : scenario->three_dimensional;

  // shaped rather than uniform, so the bounds of the grid are hit on purpose and not by luck.
  nya_fluid_emit(fluid, (NYA_FluidEmitter){
                            .position    = { nya_simulation_shaped_f32(run, -2.0F, 18.0F), nya_simulation_shaped_f32(run, -2.0F, 18.0F),
                                             nya_simulation_shaped_f32(run, -2.0F, 18.0F) },
                            .radius      = nya_simulation_range_f32(run, 0.5F, 3.0F),
                            .density     = nya_simulation_range_f32(run, 0.0F, 2.0F),
                            .temperature = nya_simulation_range_f32(run, 0.0F, 4.0F),
                            .velocity    = { nya_simulation_shaped_f32(run, -20.0F, 20.0F), nya_simulation_shaped_f32(run, -20.0F, 20.0F),
                                             nya_simulation_shaped_f32(run, -20.0F, 20.0F) },
                        });
}

static void action_step(NYA_SimulationRun* run) {
  FluidScenario* scenario = run->user_data;

  nya_simulation_advance(run, run->time_step_ns);

  // shaped, so a zero step, a negative one and a frame far longer than the clamp all get taken.
  f32 delta_time_s = nya_simulation_shaped_f32(run, -0.01F, 0.5F);

  nya_fluid_step(scenario->two_dimensional, delta_time_s);
  nya_fluid_step(scenario->three_dimensional, delta_time_s);

  scenario->checksum = nya_fluid_checksum(scenario->two_dimensional) ^ nya_fluid_checksum(scenario->three_dimensional);
}

static void action_obstacle(NYA_SimulationRun* run) {
  FluidScenario* scenario = run->user_data;

  f32x3 corner = { nya_simulation_shaped_f32(run, -4.0F, 16.0F), nya_simulation_shaped_f32(run, -4.0F, 16.0F),
                   nya_simulation_shaped_f32(run, -4.0F, 16.0F) };
  f32x3 size   = { nya_simulation_range_f32(run, 0.0F, 6.0F), nya_simulation_range_f32(run, 0.0F, 6.0F),
                   nya_simulation_range_f32(run, 0.0F, 6.0F) };

  if (nya_simulation_chance(run, 50)) {
    nya_fluid_obstacle_box_set(scenario->three_dimensional, corner, corner + size);
  } else {
    nya_fluid_obstacle_box_clear(scenario->three_dimensional, corner, corner + size);
  }
}

static void action_sample(NYA_SimulationRun* run) {
  FluidScenario* scenario = run->user_data;

  f32x3 position = { nya_simulation_shaped_f32(run, -100.0F, 100.0F), nya_simulation_shaped_f32(run, -100.0F, 100.0F),
                     nya_simulation_shaped_f32(run, -100.0F, 100.0F) };

  // a sample anywhere, in or out of the grid, must answer rather than reach past the end of a field.
  f32   density  = nya_fluid_density_at(scenario->three_dimensional, position);
  f32x3 velocity = nya_fluid_velocity_at(scenario->three_dimensional, position);

  if (!isfinite(density) || !isfinite(velocity.x)) nya_simulation_fail(run, "a sample came back undefined");
}

static void fault_clear(NYA_SimulationRun* run) {
  FluidScenario* scenario = run->user_data;

  nya_fluid_clear(scenario->two_dimensional);
  scenario->checksum = 0;
}

static void check_finite(NYA_SimulationRun* run) {
  FluidScenario* scenario = run->user_data;

  if (!fields_are_finite(scenario->two_dimensional)) nya_simulation_fail(run, "the 2D volume holds a non-finite value");
  if (!fields_are_finite(scenario->three_dimensional)) nya_simulation_fail(run, "the 3D volume holds a non-finite value");
}

static void check_bounded(NYA_SimulationRun* run) {
  // over the live table rather than over the scenario's two, so a volume the run forgot about is still checked.
  for (u32 volume = 0; volume < nya_fluid_count(); volume++) {
    const NYA_Fluid* fluid = nya_fluid_at(volume);

    for (u32 index = 0; index < fluid->cell_count; index++) {
      if (fluid->density[index] < 0.0F) nya_simulation_fail(run, "density went negative");
      if (fluid->density[index] > NYA_FLUID_FIELD_MAX) nya_simulation_fail(run, "density passed its ceiling");
    }
  }
}

static void check_obstacles_empty(NYA_SimulationRun* run) {
  FluidScenario* scenario = run->user_data;
  const NYA_Fluid* fluid  = scenario->three_dimensional;

  u32 solid = 0;

  for (u32 index = 0; index < fluid->cell_count; index++) {
    if (fluid->obstacle[index] == 0) continue;

    solid++;
  }

  if (solid != fluid->obstacle_count) nya_simulation_fail(run, "the solid cell count and the mask disagree");
}

/** One whole run, so the same seed can be taken twice and the two compared. */
static u64 simulate(NYA_Arena* arena, u64 seed, OUT u32* out_failures) {
  FluidScenario scenario = {
    .two_dimensional = nya_fluid_create(arena, (NYA_FluidOptions){ .space = NYA_FLUID_SPACE_2D,
                                                                   .width = 24,
                                                                   .height = 24,
                                                                   .cell_size = 0.5F,
                                                                   .buoyancy = 2.0F,
                                                                   .vorticity = 4.0F,
                                                                   .dissipation = 0.5F,
                                                                   .cooling = 1.0F }),
    .three_dimensional = nya_fluid_create(arena, (NYA_FluidOptions){ .space = NYA_FLUID_SPACE_3D,
                                                                      .width = 12,
                                                                      .height = 12,
                                                                      .depth = 12,
                                                                      .cell_size = 0.5F,
                                                                      .buoyancy = 2.0F,
                                                                      .vorticity = 4.0F,
                                                                      .viscosity = 0.01F,
                                                                      .diffusion = 0.01F,
                                                                      .dissipation = 0.5F,
                                                                      .cooling = 1.0F }),
  };

  NYA_SimulationRun* run = nya_simulation_create(.seed = seed, .step_count = SIMULATION_STEPS, .user_data = &scenario);

  nya_simulation_action_add(run, "emit", 40, action_emit);
  nya_simulation_action_add(run, "step", 40, action_step);
  nya_simulation_action_add(run, "obstacle", 10, action_obstacle);
  nya_simulation_action_add(run, "sample", 10, action_sample);
  nya_simulation_fault_add(run, "clear", 2, fault_clear);

  nya_simulation_check_add(run, "fields are finite", check_finite);
  nya_simulation_check_add(run, "density stays in range", check_bounded);
  nya_simulation_check_add(run, "the solid mask and its count agree", check_obstacles_empty);

  *out_failures = nya_simulation_run(run);

  u64 checksum = scenario.checksum;

  nya_simulation_destroy(run);
  nya_fluid_destroy(scenario.two_dimensional);
  nya_fluid_destroy(scenario.three_dimensional);

  return checksum;
}

s32 main(void) {
  _NYA_APP_INSTANCE = (NYA_App){ .initialized = true };
  b8 sdl_ok         = SDL_Init(0);
  nya_assert(sdl_ok, "SDL_Init failed: %s", SDL_GetError());

  NYA_Arena* arena = nya_arena_create(.name = "test_fluid");
  defer nya_arena_destroy(arena);

  // TEST: the grid is the shape the options asked for
  {
    NYA_Fluid* flat = nya_fluid_create(arena, (NYA_FluidOptions){ .space = NYA_FLUID_SPACE_2D, .width = 16, .height = 8, .depth = 99 });

    nya_assert(flat->width == 16 && flat->height == 8, "the interior is what was asked for");
    nya_assert(flat->depth == 1, "a 2D volume is one cell deep however much depth was passed, got " FMTu32, flat->depth);
    nya_assert(flat->stride_x == 18 && flat->stride_y == 10 && flat->stride_z == 3, "one border cell on each side");
    nya_assert(flat->cell_count == 18 * 10 * 3, "the cell count is the product of the strides");
    nya_assert(nya_fluid_cell_count(flat) == flat->cell_count, "and the accessor agrees");
    nya_assert(nya_fluid_memory_bytes(flat) > (u64)flat->cell_count * sizeof(f32), "the volume holds more than one field");

    NYA_Fluid* box = nya_fluid_create(arena, (NYA_FluidOptions){ .space = NYA_FLUID_SPACE_3D, .width = 4, .height = 5, .depth = 6 });

    nya_assert(box->depth == 6, "a 3D volume keeps its depth");
    nya_assert(nya_fluid_count() == 2, "both volumes are live, got " FMTu32, nya_fluid_count());

    nya_fluid_destroy(box);
    nya_assert(nya_fluid_count() == 1, "destroying one leaves the other");

    nya_fluid_destroy(box);
    nya_assert(nya_fluid_count() == 1, "destroying it again is a no-op");

    nya_fluid_destroy(flat);
    nya_assert(nya_fluid_count() == 0, "and the table empties");

    printf("  PASSED\n");
  }

  // TEST: a zero volume's up axis follows the space it is in
  {
    NYA_Fluid* flat = nya_fluid_create(arena, (NYA_FluidOptions){ .space = NYA_FLUID_SPACE_2D, .width = 4, .height = 4 });
    NYA_Fluid* box  = nya_fluid_create(arena, (NYA_FluidOptions){ .space = NYA_FLUID_SPACE_3D, .width = 4, .height = 4, .depth = 4 });

    // 2D world space grows downward, so up is negative y there and positive y in 3D.
    nya_assert(nya_fluid_options(flat).up.y < 0.0F, "a 2D volume rises up the screen");
    nya_assert(nya_fluid_options(box).up.y > 0.0F, "a 3D volume rises along positive y");
    nya_assert(nya_fluid_options(flat).cell_size == NYA_FLUID_CELL_SIZE, "a zero cell size takes the default");
    nya_assert(nya_fluid_options(box).pressure_iterations == NYA_FLUID_PRESSURE_ITERATIONS, "so does a zero iteration count");

    nya_fluid_destroy(flat);
    nya_fluid_destroy(box);

    printf("  PASSED\n");
  }

  // TEST: emission puts density where it was asked for and nowhere else
  {
    NYA_Fluid* fluid = nya_fluid_create(arena, (NYA_FluidOptions){ .width = 16, .height = 16, .cell_size = 1.0F });

    nya_assert(interior_density(fluid) == 0.0F, "a fresh volume is empty");

    nya_fluid_emit(fluid, (NYA_FluidEmitter){ .position = { 8.0F, 8.0F, 0.0F }, .radius = 2.0F, .density = 1.0F });

    nya_assert(interior_density(fluid) > 0.0F, "emitting puts density in");
    nya_assert(nya_fluid_density_at(fluid, (f32x3){ 8.0F, 8.0F, 0.0F }) > 0.5F, "densest at the centre");
    nya_assert(nya_fluid_density_at(fluid, (f32x3){ 1.0F, 1.0F, 0.0F }) == 0.0F, "and nothing far from it");

    f32 inside = interior_density(fluid);

    // far outside the grid: the falloff never reaches a cell, so nothing changes.
    nya_fluid_emit(fluid, (NYA_FluidEmitter){ .position = { 500.0F, 500.0F, 0.0F }, .radius = 1.0F, .density = 10.0F });
    nya_assert(interior_density(fluid) == inside, "an emitter outside the grid adds nothing");

    // a sample outside the grid answers rather than reading past the end of the field.
    nya_assert(nya_fluid_density_at(fluid, (f32x3){ -400.0F, 900.0F, 0.0F }) >= 0.0F, "a sample outside is still a number");

    // heat is its own field: a hot emitter warms the cells it covers and adds no smoke to them.
    f32 ambient = nya_fluid_options(fluid).ambient_temperature;

    nya_fluid_emit(fluid, (NYA_FluidEmitter){ .position = { 8.0F, 8.0F, 0.0F }, .radius = 2.0F, .temperature = 3.0F });

    nya_assert(nya_fluid_temperature_at(fluid, (f32x3){ 8.0F, 8.0F, 0.0F }) > ambient + 1.5F, "hottest at the centre, got " FMTf32,
               (f64)nya_fluid_temperature_at(fluid, (f32x3){ 8.0F, 8.0F, 0.0F }));
    nya_assert(nya_fluid_temperature_at(fluid, (f32x3){ 1.0F, 1.0F, 0.0F }) == ambient, "and ambient far from it");
    nya_assert(interior_density(fluid) == inside, "heat alone adds no density");

    u32 index = 0;
    nya_assert(nya_fluid_cell_index(fluid, (f32x3){ 8.0F, 8.0F, 0.0F }, &index), "a point inside has a cell");
    nya_assert(!nya_fluid_cell_index(fluid, (f32x3){ -8.0F, 8.0F, 0.0F }, &index), "a point outside has none");

    nya_fluid_clear(fluid);
    nya_assert(interior_density(fluid) == 0.0F, "clear empties it");

    nya_fluid_destroy(fluid);

    printf("  PASSED\n");
  }

  // TEST: a step keeps the field divergence free and never creates density
  {
    // no confinement: its force is a cell-scale field whose divergence a collocated projection cannot see, so leaving it on would measure that instead of the pressure solve. See render_fluid.h, "what the projection removes and what it leaves".
    NYA_FluidOptions options = { .width = 24, .height = 24, .cell_size = 1.0F };

    NYA_FluidOptions barely_solved     = options;
    barely_solved.pressure_iterations  = 1;

    NYA_Fluid* solved = nya_fluid_create(arena, options);
    NYA_Fluid* barely = nya_fluid_create(arena, barely_solved);

    NYA_FluidEmitter emitter = {
      .position = { 12.0F, 18.0F, 0.0F },
      .radius   = 4.0F,
      .density  = 1.0F,
      .velocity = { 6.0F, -9.0F, 0.0F },
    };

    nya_fluid_emit(solved, emitter);
    nya_fluid_emit(barely, emitter);

    f32 emitted = interior_density(solved);

    nya_assert(nya_fluid_step_time_s(solved) == 0.0F, "a volume that has not stepped has cost nothing");

    for (u32 step = 0; step < 60; step++) {
      nya_fluid_step(solved, STEP_S);
      nya_fluid_step(barely, STEP_S);
    }

    nya_assert(solved->step_count == 60, "every step was counted, got " FMTu64, solved->step_count);

    // a step over this grid takes microseconds, which the monotonic clock resolves.
    nya_assert(nya_fluid_step_time_s(solved) > 0.0F, "the last step was timed");
    nya_assert(nya_fluid_step_time_s(solved) < 1.0F, "in seconds, got " FMTf32, (f64)nya_fluid_step_time_s(solved));
    nya_assert(fields_are_finite(solved), "a minute of stepping produced no infinity");

    // the projection is what makes it look like a fluid rather than like a blur, so this is the assertion that fails if the pressure solve is ever broken rather than merely slow.
    f32 divergence = worst_divergence(solved);
    f32 speed      = worst_speed(solved);

    nya_assert(speed > 0.0F, "the field is moving at all");
    nya_assert(divergence < speed * DIVERGENCE_SHARE_MAX, "the field is nearly divergence free, worst is " FMTf32 " of " FMTf32,
               (f64)divergence, (f64)speed);
    nya_assert(divergence < worst_divergence(barely), "twenty sweeps beat one, " FMTf32 " against " FMTf32, (f64)divergence,
               (f64)worst_divergence(barely));

    // semi-Lagrangian advection loses mass at the walls and never invents any. That is the whole trade for unconditional stability, so the assertion is one-sided on purpose.
    f32 remaining = interior_density(solved);
    nya_assert(remaining <= emitted + NYA_EPSILON, "advection created density: " FMTf32 " from " FMTf32, (f64)remaining, (f64)emitted);
    nya_assert(remaining > 0.0F, "and did not lose all of it");

    nya_fluid_destroy(solved);
    nya_fluid_destroy(barely);

    printf("  PASSED\n");
  }

  // TEST: buoyancy lifts hot fluid along the volume's own up axis
  {
    for (u32 space = 0; space < NYA_FLUID_SPACE_COUNT; space++) {
      NYA_Fluid* fluid = nya_fluid_create(arena, (NYA_FluidOptions){
                                                     .space     = (NYA_FluidSpace)space,
                                                     .width     = 16,
                                                     .height    = 24,
                                                     .depth     = 16,
                                                     .cell_size = 1.0F,
                                                     .buoyancy  = 20.0F,
                                                 });

      f32x3 source = { 8.0F, 12.0F, 8.0F };

      nya_fluid_emit(fluid, (NYA_FluidEmitter){ .position = source, .radius = 2.0F, .density = 1.0F, .temperature = 4.0F });

      f32x3 before = density_centre(fluid);

      for (u32 step = 0; step < 40; step++) {
        nya_fluid_emit(fluid, (NYA_FluidEmitter){ .position = source, .radius = 2.0F, .density = 0.2F, .temperature = 1.0F });
        nya_fluid_step(fluid, STEP_S);
      }

      f32x3 after = density_centre(fluid);
      f32x3 up    = nya_fluid_options(fluid).up;

      f32 travelled = nya_vector_dot(after - before, up);
      nya_assert(travelled > 0.0F, "hot fluid rose along up, moved " FMTf32 " in space " FMTu32, (f64)travelled, space);

      nya_fluid_destroy(fluid);
    }

    printf("  PASSED\n");
  }

  // TEST: an obstacle stays empty and keeps the fluid out
  {
    NYA_Fluid* fluid = nya_fluid_create(arena, (NYA_FluidOptions){ .width = 20, .height = 20, .cell_size = 1.0F });

    nya_assert(fluid->obstacle_count == 0, "no obstacles to start");

    f32x3 min = { 8.0F, 8.0F, -1.0F };
    f32x3 max = { 12.0F, 12.0F, 1.0F };

    nya_fluid_obstacle_box_set(fluid, min, max);
    nya_assert(fluid->obstacle_count > 0, "the box marked cells solid");

    u32 solid = fluid->obstacle_count;

    // idempotent: setting the same box twice must not double the count.
    nya_fluid_obstacle_box_set(fluid, min, max);
    nya_assert(fluid->obstacle_count == solid, "setting the same box twice changes nothing");

    // driven straight at the obstacle, hard enough that nothing but the wall would stop it.
    for (u32 step = 0; step < 60; step++) {
      nya_fluid_emit(fluid, (NYA_FluidEmitter){
                                .position = { 4.0F, 10.0F, 0.0F },
                                .radius   = 2.0F,
                                .density  = 0.5F,
                                .velocity = { 40.0F, 0.0F, 0.0F },
                            });
      nya_fluid_step(fluid, STEP_S);
    }

    u32 solid_with_fluid = 0;

    for (u32 index = 0; index < fluid->cell_count; index++) {
      if (fluid->obstacle[index] == 0) continue;
      if (fluid->density[index] != 0.0F) solid_with_fluid++;
      if (fluid->velocity_x[index] != 0.0F) solid_with_fluid++;
    }

    nya_assert(solid_with_fluid == 0, FMTu32 " solid cells held fluid", solid_with_fluid);
    nya_assert(fields_are_finite(fluid), "the obstacle did not produce an infinity");

    nya_fluid_obstacle_box_clear(fluid, min, max);
    nya_assert(fluid->obstacle_count == 0, "clearing the same box empties the mask");

    nya_fluid_obstacle_box_set(fluid, min, max);
    nya_fluid_obstacles_clear(fluid);
    nya_assert(fluid->obstacle_count == 0, "and so does clearing everything");

    nya_fluid_destroy(fluid);

    printf("  PASSED\n");
  }

  // TEST: the same inputs give the same state, and a different one does not
  {
    NYA_FluidOptions options = {
      .width       = 16,
      .height      = 16,
      .cell_size   = 1.0F,
      .buoyancy    = 5.0F,
      .vorticity   = 4.0F,
      .viscosity   = 0.02F,
      .diffusion   = 0.02F,
      .dissipation = 0.3F,
      .cooling     = 0.8F,
    };

    NYA_Fluid* first  = nya_fluid_create(arena, options);
    NYA_Fluid* second = nya_fluid_create(arena, options);
    NYA_Fluid* third  = nya_fluid_create(arena, options);

    for (u32 step = 0; step < 30; step++) {
      NYA_FluidEmitter emitter = {
        .position    = { 8.0F, 12.0F, 0.0F },
        .radius      = 2.0F,
        .density     = 0.4F,
        .temperature = 1.5F,
        .velocity    = { 3.0F, -2.0F, 0.0F },
      };

      nya_fluid_emit(first, emitter);
      nya_fluid_emit(second, emitter);
      nya_fluid_emit(third, emitter);

      nya_fluid_step(first, STEP_S);
      nya_fluid_step(second, STEP_S);

      // one step shorter, which is the smallest difference an input can have.
      nya_fluid_step(third, STEP_S * 0.5F);
    }

    nya_assert(nya_fluid_checksum(first) == nya_fluid_checksum(second), "the same calls give the same state");
    nya_assert(nya_fluid_checksum(first) != nya_fluid_checksum(third), "a different timestep gives a different one");

    u64 before = nya_fluid_checksum(first);

    // a non-positive step is a no-op rather than an integration backwards.
    nya_fluid_step(first, 0.0F);
    nya_fluid_step(first, -1.0F);
    nya_assert(nya_fluid_checksum(first) == before, "a non-positive step changed nothing");

    // past the clamp, which is a shorter step and not a longer one.
    nya_fluid_step(first, 100.0F);
    nya_assert(fields_are_finite(first), "a huge step is clamped rather than survived");

    nya_fluid_destroy(first);
    nya_fluid_destroy(second);
    nya_fluid_destroy(third);

    printf("  PASSED\n");
  }

  // TEST: a 3D volume one cell deep is the 2D solver
  {
    // the claim the whole "one solver" design rests on: with depth one and no z motion, the three dimensional code reduces term for term to the two dimensional one. If this ever fails, the boundary planes stopped mirroring and the 2D look changed with it.
    NYA_FluidOptions shared = { .width = 12, .height = 12, .depth = 1, .cell_size = 1.0F, .vorticity = 3.0F, .up = { 0.0F, 1.0F, 0.0F } };

    NYA_FluidOptions flat_options = shared;
    NYA_FluidOptions box_options  = shared;

    flat_options.space = NYA_FLUID_SPACE_2D;
    box_options.space  = NYA_FLUID_SPACE_3D;

    NYA_Fluid* flat = nya_fluid_create(arena, flat_options);
    NYA_Fluid* box  = nya_fluid_create(arena, box_options);

    for (u32 step = 0; step < 20; step++) {
      NYA_FluidEmitter emitter = {
        .position    = { 6.0F, 3.0F, 0.5F },
        .radius      = 2.0F,
        .density     = 0.5F,
        .temperature = 1.0F,
        .velocity    = { 2.0F, 5.0F, 0.0F },
      };

      nya_fluid_emit(flat, emitter);
      nya_fluid_emit(box, emitter);

      nya_fluid_step(flat, STEP_S);
      nya_fluid_step(box, STEP_S);
    }

    nya_assert(nya_fluid_checksum(flat) == nya_fluid_checksum(box), "2D and a one-cell-deep 3D volume agree exactly");

    nya_fluid_destroy(flat);
    nya_fluid_destroy(box);

    printf("  PASSED\n");
  }

  // TEST: a window draws no fluid until it asks, and keeps what it asked for
  {
    nya_system_renderer_for_window_init(&window);

    NYA_FluidRenderOptions fresh = nya_fluid_render_options(&window);
    nya_assert(!fresh.enabled, "a new window has fluids off, so a volume costs it nothing");
    nya_assert(fresh.opacity == 0.0F && fresh.stride == 0, "and every knob is left to its default");

    NYA_FluidRenderOptions asked = { .enabled = true, .opacity = 0.4F, .stride = 2, .hot_temperature = 3.0F };
    nya_fluid_render_options_set(&window, asked);

    // stored as given, zeroes included: the defaults are resolved at draw time, so a later change to one reaches every window that left it zero.
    NYA_FluidRenderOptions stored = nya_fluid_render_options(&window);
    nya_assert(stored.enabled && stored.opacity == 0.4F && stored.stride == 2 && stored.hot_temperature == 3.0F, "the options read back");
    nya_assert(stored.threshold == 0.0F && stored.density_full == 0.0F, "and a zero is stored as a zero, not as its default");

    nya_system_renderer_for_window_deinit(&window);
    nya_assert(!nya_fluid_render_options(&window).enabled, "a torn down window forgets them");

    printf("  PASSED\n");
  }

  // TEST: the simulation harness drives it, and the same seed replays
  {
    const u64 seed = 0xF10D1D5EEDULL;

    u32 failures_first  = 0;
    u32 failures_second = 0;

    u64 first  = simulate(arena, seed, &failures_first);
    u64 second = simulate(arena, seed, &failures_second);

    nya_assert(failures_first == 0, FMTu32 " failures in the simulated run", failures_first);
    nya_assert(failures_second == 0, FMTu32 " failures in the replay", failures_second);
    nya_assert(first == second, "the same seed replayed a different run");

    u32 failures_other = 0;
    u64 other          = simulate(arena, seed + 1, &failures_other);

    nya_assert(failures_other == 0, FMTu32 " failures in the second seed's run", failures_other);
    nya_assert(other != first, "two seeds produced the same run");

    nya_assert(nya_fluid_count() == 0, "every volume the run made was destroyed");

    printf("  PASSED\n");
  }

  printf("test_fluid: OK\n");

  return EXIT_SUCCESS;
}
