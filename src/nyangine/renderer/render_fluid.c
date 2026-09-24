#include "nyangine/nyangine.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * CONSTANTS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/** Field arrays of f32 per volume. Counted here so nya_fluid_memory_bytes and the header agree. */
#define _NYA_FLUID_FIELD_COUNT 13

/**
 * What a vorticity gradient has to reach before it is normalized. Below it the cell is in still fluid
 * and the direction is numerical noise, so the confinement force there is dropped rather than pointed
 * somewhere arbitrary.
 * */
#define _NYA_FLUID_VORTICITY_EPSILON 1.0e-5F

/** How the boundary rule treats the field it is closing: mirror it, or mirror and negate one component. */
enum {
    _NYA_FLUID_BOUND_SCALAR = 0,
    _NYA_FLUID_BOUND_VELOCITY_X,
    _NYA_FLUID_BOUND_VELOCITY_Y,
    _NYA_FLUID_BOUND_VELOCITY_Z,
};

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API DECLARATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/** The live volumes. A static table, so a program with no fluid in it allocates and registers nothing. */
typedef struct _NYA_FluidRegistry _NYA_FluidRegistry;

struct _NYA_FluidRegistry {
    NYA_Fluid* volumes[NYA_FLUID_VOLUMES_MAX];
    u32        count;

    /** Bytes held by every live volume, shown as a gauge beside the renderer's. */
    u64 bytes;

    /** Whether the ceiling and the gauge have been registered. Done on the first create, never undone. */
    b8 registered;
};

NYA_INTERNAL _NYA_FluidRegistry _nya_fluid_registry = { 0 };

/** The flat index of cell (i, j, k), borders included. */
NYA_INTERNAL u32 _nya_fluid_index(const NYA_Fluid* fluid, u32 i, u32 j, u32 k) __attr_no_discard;

/**
 * Exchanges two field arrays. A stage that reads a whole field while writing it reads out of the
 * other one, so the two swap rather than one being copied into the other.
 * */
NYA_INTERNAL void _nya_fluid_fields_swap(f32** a, f32** b);

/** Closes the field's six boundary planes, then its edges and corners. See NYA_FLUID_BOUND_*. */
NYA_INTERNAL void _nya_fluid_bounds_set(NYA_Fluid* fluid, f32* field, u32 bound);

/** `iterations` Gauss-Seidel sweeps of `field = (field_source + a * neighbours) / c`. */
NYA_INTERNAL void _nya_fluid_linear_solve(NYA_Fluid* fluid, f32* field, const f32* field_source, f32 a, f32 c, u32 bound,
                                          u32 iterations);

/** Semi-Lagrangian advection: every cell takes what was at the point the flow came from. */
NYA_INTERNAL void _nya_fluid_advect(NYA_Fluid* fluid, f32* field, const f32* field_source, f32 delta_time_s, u32 bound);

/** Removes the divergence from the velocity field, which is what makes it look like a fluid. */
NYA_INTERNAL void _nya_fluid_project(NYA_Fluid* fluid);

/** Buoyancy, weight and gravity, integrated into the velocity field. */
NYA_INTERNAL void _nya_fluid_forces_add(NYA_Fluid* fluid, f32 delta_time_s);

/** Pushes back the swirl the grid is about to dissipate. See NYA_FluidOptions.vorticity. */
NYA_INTERNAL void _nya_fluid_vorticity_add(NYA_Fluid* fluid, f32 delta_time_s);

/** Holds every solid cell at rest and empty, which is what makes an obstacle an obstacle. */
NYA_INTERNAL void _nya_fluid_obstacles_enforce(NYA_Fluid* fluid);

/** Trilinear sample of `field` at grid coordinates, clamped into the addressable range. */
NYA_INTERNAL f32 _nya_fluid_sample(const NYA_Fluid* fluid, const f32* field, f32 x, f32 y, f32 z) __attr_no_discard;

/** A world position in the grid's coordinates, where cell (i, j, k) is centred on (i, j, k). */
NYA_INTERNAL f32x3 _nya_fluid_grid_position(const NYA_Fluid* fluid, f32x3 position) __attr_no_discard;

/** The half-open cell range a world box covers, clamped to the interior. False when it covers none. */
NYA_INTERNAL b8 _nya_fluid_box_range(const NYA_Fluid* fluid, f32x3 min, f32x3 max, OUT u32 out_low[3],
                                     OUT u32 out_high[3]) __attr_no_discard;

/** Marks or unmarks every cell in a world box, keeping `obstacle_count` exact. */
NYA_INTERNAL void _nya_fluid_obstacle_box_apply(NYA_Fluid* fluid, f32x3 min, f32x3 max, b8 solid);

/** The colour a cell's density and temperature draw as, under this window's options. */
NYA_INTERNAL NYA_Color _nya_fluid_cell_color(const NYA_Fluid* fluid, NYA_FluidRenderOptions options, u32 index) __attr_no_discard;

/** The render options with every zero replaced by its default. */
NYA_INTERNAL NYA_FluidRenderOptions _nya_fluid_render_options_resolved(NYA_FluidRenderOptions options) __attr_no_discard;

NYA_INTERNAL void _nya_fluid_draw_2d(NYA_Window* window, const NYA_Fluid* fluid, NYA_FluidRenderOptions options);
NYA_INTERNAL void _nya_fluid_draw_3d(NYA_Window* window, const NYA_Fluid* fluid, NYA_FluidRenderOptions options);

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PUBLIC API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/*
 * ─────────────────────────────────────────────────────────
 * LIFETIME
 * ─────────────────────────────────────────────────────────
 */

NYA_Fluid* nya_fluid_create(NYA_Arena* arena, NYA_FluidOptions options) {
    nya_assert(arena != nullptr);
    nya_assert(options.space < NYA_FLUID_SPACE_COUNT, "unknown fluid space " FMTu32, (u32)options.space);
    nya_assert(_nya_fluid_registry.count < NYA_FLUID_VOLUMES_MAX, "more than " "NYA_FLUID_VOLUMES_MAX fluid volumes");

    u32 width  = nya_max(options.width, 1U);
    u32 height = nya_max(options.height, 1U);

    // a 2D volume is a 3D one exactly one cell deep; see the header. Forced rather than asserted, so a
    // caller can reuse one options struct for both spaces.
    u32 depth = options.space == NYA_FLUID_SPACE_2D ? 1U : nya_max(options.depth, 1U);

    nya_assert(width <= NYA_FLUID_DIMENSION_MAX && height <= NYA_FLUID_DIMENSION_MAX && depth <= NYA_FLUID_DIMENSION_MAX,
               "a fluid grid edge is at most NYA_FLUID_DIMENSION_MAX cells, got " FMTu32 "x" FMTu32 "x" FMTu32,
               width, height, depth);

    u32 stride_x = width + 2;
    u32 stride_y = height + 2;
    u32 stride_z = depth + 2;

    u64 cell_count = (u64)stride_x * (u64)stride_y * (u64)stride_z;
    nya_assert(cell_count <= NYA_FLUID_CELLS_MAX, "a fluid grid holds at most NYA_FLUID_CELLS_MAX cells, got " FMTu64,
               cell_count);

    NYA_Fluid* fluid = nya_arena_alloc(arena, sizeof(NYA_Fluid));

    u64 field_bytes = cell_count * sizeof(f32);

    *fluid = (NYA_Fluid){
        .allocator  = arena,
        .options    = options,
        .width      = width,
        .height     = height,
        .depth      = depth,
        .stride_x   = stride_x,
        .stride_y   = stride_y,
        .stride_z   = stride_z,
        .cell_count = (u32)cell_count,

        .velocity_x = nya_arena_alloc(arena, field_bytes),
        .velocity_y = nya_arena_alloc(arena, field_bytes),
        .velocity_z = nya_arena_alloc(arena, field_bytes),

        .velocity_x_previous = nya_arena_alloc(arena, field_bytes),
        .velocity_y_previous = nya_arena_alloc(arena, field_bytes),
        .velocity_z_previous = nya_arena_alloc(arena, field_bytes),

        .density          = nya_arena_alloc(arena, field_bytes),
        .density_previous = nya_arena_alloc(arena, field_bytes),

        .temperature          = nya_arena_alloc(arena, field_bytes),
        .temperature_previous = nya_arena_alloc(arena, field_bytes),

        .pressure   = nya_arena_alloc(arena, field_bytes),
        .divergence = nya_arena_alloc(arena, field_bytes),

        .curl_magnitude = nya_arena_alloc(arena, field_bytes),

        .obstacle = nya_arena_alloc(arena, cell_count),
    };

    // defaults the header promises, resolved once so nothing downstream has to test for zero.
    if (fluid->options.cell_size <= 0.0F) fluid->options.cell_size = NYA_FLUID_CELL_SIZE;
    if (fluid->options.pressure_iterations == 0) fluid->options.pressure_iterations = NYA_FLUID_PRESSURE_ITERATIONS;

    if (nya_vector_length(fluid->options.up) <= 0.0F) {
        // 2D world space has y growing down the screen, so up is negative there.
        fluid->options.up = options.space == NYA_FLUID_SPACE_2D ? (f32x3){ 0.0F, -1.0F, 0.0F } : (f32x3){ 0.0F, 1.0F, 0.0F };
    }

    fluid->options.up = nya_vector_normalize(fluid->options.up);

    if (options.space == NYA_FLUID_SPACE_2D) {
        // the third axis is flattened once, here, rather than guarded against at every write. A 2D
        // volume's z velocity then stays exactly zero, which is what makes the solver's two spaces
        // the same arithmetic instead of merely the same algorithm.
        fluid->options.up.z      = 0.0F;
        fluid->options.gravity.z = 0.0F;
    }

    nya_fluid_clear(fluid);
    nya_memset(fluid->obstacle, 0, cell_count);

    _nya_fluid_registry.volumes[_nya_fluid_registry.count] = fluid;
    _nya_fluid_registry.count++;
    _nya_fluid_registry.bytes += nya_fluid_memory_bytes(fluid);

    // registered on the first volume created, for the same reason base_arena.c registers its own
    // ceiling lazily: a zeroed table needs no init, and a program with no fluid in it pays nothing.
    if (!_nya_fluid_registry.registered) {
        _nya_fluid_registry.registered = true;
        nya_ceiling_register("fluid_volumes", NYA_FLUID_VOLUMES_MAX, &_nya_fluid_registry.count);
        nya_gauge_register("fluid_cells", &_nya_fluid_registry.bytes);
    }

    nya_assert(fluid->cell_count == stride_x * stride_y * stride_z, "the cell count and the strides disagree");
    nya_assert(_nya_fluid_registry.count <= NYA_FLUID_VOLUMES_MAX, "the volume table overran");

    return fluid;
}

void nya_fluid_destroy(NYA_Fluid* fluid) {
    if (fluid == nullptr) return;

    u32 count_before = _nya_fluid_registry.count;

    for (u32 i = 0; i < _nya_fluid_registry.count; i++) {
        if (_nya_fluid_registry.volumes[i] != fluid) continue;

        _nya_fluid_registry.bytes -= nya_fluid_memory_bytes(fluid);
        _nya_fluid_registry.count--;
        _nya_fluid_registry.volumes[i] = _nya_fluid_registry.volumes[_nya_fluid_registry.count];

        _nya_fluid_registry.volumes[_nya_fluid_registry.count] = nullptr;
        break;
    }

    // arena owned, so nothing is freed here. Destroying a volume twice has to be a no-op, which is
    // why this asserts the table shrank by at most one rather than exactly one.
    nya_assert(_nya_fluid_registry.count == count_before || _nya_fluid_registry.count + 1 == count_before,
               "destroying one volume changed the table by more than one row");
}

void nya_fluid_clear(NYA_Fluid* fluid) {
    nya_assert(fluid != nullptr);
    nya_assert(fluid->cell_count > 0, "a volume with no cells");

    u64 field_bytes = (u64)fluid->cell_count * sizeof(f32);

    nya_memset(fluid->velocity_x, 0, field_bytes);
    nya_memset(fluid->velocity_y, 0, field_bytes);
    nya_memset(fluid->velocity_z, 0, field_bytes);
    nya_memset(fluid->velocity_x_previous, 0, field_bytes);
    nya_memset(fluid->velocity_y_previous, 0, field_bytes);
    nya_memset(fluid->velocity_z_previous, 0, field_bytes);
    nya_memset(fluid->density, 0, field_bytes);
    nya_memset(fluid->density_previous, 0, field_bytes);
    nya_memset(fluid->pressure, 0, field_bytes);
    nya_memset(fluid->divergence, 0, field_bytes);
    nya_memset(fluid->curl_magnitude, 0, field_bytes);

    // temperature starts at ambient rather than zero, so an unlit volume has no buoyancy anywhere.
    for (u32 i = 0; i < fluid->cell_count; i++) {
        fluid->temperature[i]          = fluid->options.ambient_temperature;
        fluid->temperature_previous[i] = fluid->options.ambient_temperature;
    }

    fluid->step_count  = 0;
    fluid->step_time_s = 0.0F;
}

/*
 * ─────────────────────────────────────────────────────────
 * OPTIONS
 * ─────────────────────────────────────────────────────────
 */

void nya_fluid_options_set(NYA_Fluid* fluid, NYA_FluidOptions options) {
    nya_assert(fluid != nullptr);
    nya_assert(options.pressure_iterations <= NYA_FLUID_PRESSURE_ITERATIONS_MAX,
               "a pressure solve runs at most NYA_FLUID_PRESSURE_ITERATIONS_MAX sweeps, asked for " FMTu32,
               options.pressure_iterations);

    // the allocation's own shape cannot move, so those four fields are taken from what exists.
    options.space     = fluid->options.space;
    options.width     = fluid->width;
    options.height    = fluid->height;
    options.depth     = fluid->depth;
    options.cell_size = fluid->options.cell_size;

    if (options.pressure_iterations == 0) options.pressure_iterations = NYA_FLUID_PRESSURE_ITERATIONS;

    if (nya_vector_length(options.up) <= 0.0F) options.up = fluid->options.up;

    options.up = nya_vector_normalize(options.up);

    // flattened for the reason nya_fluid_create flattens it.
    if (options.space == NYA_FLUID_SPACE_2D) {
        options.up.z      = 0.0F;
        options.gravity.z = 0.0F;
    }

    fluid->options = options;
}

NYA_FluidOptions nya_fluid_options(const NYA_Fluid* fluid) {
    nya_assert(fluid != nullptr);
    nya_assert(fluid->options.cell_size > 0.0F, "a volume with no cell size");

    return fluid->options;
}

void nya_fluid_wind_set(NYA_Fluid* fluid, const NYA_WindField* field, f32 influence) {
    nya_assert(fluid != nullptr);

    fluid->wind           = field;
    fluid->wind_influence = influence;
    // wind_time_s is left alone, so toggling the wind does not jump the field's phase.
}

/*
 * ─────────────────────────────────────────────────────────
 * THE STEP
 * ─────────────────────────────────────────────────────────
 */

void nya_fluid_step(NYA_Fluid* fluid, f32 delta_time_s) {
    nya_perf_time_this_function();
    nya_trace_scope(NYA_TRACE_FLUID);

    nya_assert(fluid != nullptr);
    nya_assert(fluid->options.pressure_iterations > 0 && fluid->options.pressure_iterations <= NYA_FLUID_PRESSURE_ITERATIONS_MAX,
               "the pressure iteration count left its bounds: " FMTu32, fluid->options.pressure_iterations);

    if (delta_time_s <= 0.0F) return;

    // a stalled frame integrates a shorter step rather than teleporting the whole field; see the constant.
    f32 step_s = nya_min(delta_time_s, NYA_FLUID_STEP_SECONDS_MAX);

    u64 started_ns = nya_clock_get_monotonic_ns();

    f32 cell_size = fluid->options.cell_size;

    _nya_fluid_forces_add(fluid, step_s);

    // The wind: a uniform push into the velocity field, sampled once at the volume's origin, so the plume leans on
    // the same air the foliage and particles read. Added alongside the other body forces, before vorticity and the
    // projection clean it up; the same whole-field loop the forces use, so the padded borders are handled downstream.
    if (fluid->wind != nullptr) {
        fluid->wind_time_s += step_s;

        f32x3 w    = nya_wind_at(fluid->wind, fluid->options.origin, fluid->wind_time_s);
        f32   push = fluid->wind_influence * step_s;

        for (u32 i = 0; i < fluid->cell_count; i++) {
            fluid->velocity_x[i] += w.x * push;
            fluid->velocity_y[i] += w.y * push;
            fluid->velocity_z[i] += w.z * push;
        }
    }

    _nya_fluid_vorticity_add(fluid, step_s);

    if (fluid->options.viscosity > 0.0F) {
        f32 a = fluid->options.viscosity * step_s / (cell_size * cell_size);
        f32 c = 1.0F + (6.0F * a);

        _nya_fluid_fields_swap(&fluid->velocity_x, &fluid->velocity_x_previous);
        _nya_fluid_fields_swap(&fluid->velocity_y, &fluid->velocity_y_previous);
        _nya_fluid_fields_swap(&fluid->velocity_z, &fluid->velocity_z_previous);

        _nya_fluid_linear_solve(fluid, fluid->velocity_x, fluid->velocity_x_previous, a, c, _NYA_FLUID_BOUND_VELOCITY_X,
                                fluid->options.pressure_iterations);
        _nya_fluid_linear_solve(fluid, fluid->velocity_y, fluid->velocity_y_previous, a, c, _NYA_FLUID_BOUND_VELOCITY_Y,
                                fluid->options.pressure_iterations);
        _nya_fluid_linear_solve(fluid, fluid->velocity_z, fluid->velocity_z_previous, a, c, _NYA_FLUID_BOUND_VELOCITY_Z,
                                fluid->options.pressure_iterations);
    }

    _nya_fluid_project(fluid);

    // advection reads the whole velocity field while writing it, so it advects out of the previous copy.
    _nya_fluid_fields_swap(&fluid->velocity_x, &fluid->velocity_x_previous);
    _nya_fluid_fields_swap(&fluid->velocity_y, &fluid->velocity_y_previous);
    _nya_fluid_fields_swap(&fluid->velocity_z, &fluid->velocity_z_previous);

    _nya_fluid_advect(fluid, fluid->velocity_x, fluid->velocity_x_previous, step_s, _NYA_FLUID_BOUND_VELOCITY_X);
    _nya_fluid_advect(fluid, fluid->velocity_y, fluid->velocity_y_previous, step_s, _NYA_FLUID_BOUND_VELOCITY_Y);
    _nya_fluid_advect(fluid, fluid->velocity_z, fluid->velocity_z_previous, step_s, _NYA_FLUID_BOUND_VELOCITY_Z);

    // twice per step on purpose: advecting a divergence-free field does not keep it divergence free.
    _nya_fluid_project(fluid);

    if (fluid->options.diffusion > 0.0F) {
        f32 a = fluid->options.diffusion * step_s / (cell_size * cell_size);
        f32 c = 1.0F + (6.0F * a);

        _nya_fluid_fields_swap(&fluid->density, &fluid->density_previous);
        _nya_fluid_fields_swap(&fluid->temperature, &fluid->temperature_previous);

        _nya_fluid_linear_solve(fluid, fluid->density, fluid->density_previous, a, c, _NYA_FLUID_BOUND_SCALAR,
                                fluid->options.pressure_iterations);
        _nya_fluid_linear_solve(fluid, fluid->temperature, fluid->temperature_previous, a, c, _NYA_FLUID_BOUND_SCALAR,
                                fluid->options.pressure_iterations);
    }

    _nya_fluid_fields_swap(&fluid->density, &fluid->density_previous);
    _nya_fluid_fields_swap(&fluid->temperature, &fluid->temperature_previous);

    _nya_fluid_advect(fluid, fluid->density, fluid->density_previous, step_s, _NYA_FLUID_BOUND_SCALAR);
    _nya_fluid_advect(fluid, fluid->temperature, fluid->temperature_previous, step_s, _NYA_FLUID_BOUND_SCALAR);

    f32 kept    = nya_clamp(1.0F - (fluid->options.dissipation * step_s), 0.0F, 1.0F);
    f32 cooled  = nya_clamp(fluid->options.cooling * step_s, 0.0F, 1.0F);
    f32 ambient = fluid->options.ambient_temperature;

    for (u32 i = 0; i < fluid->cell_count; i++) {
        f32 density     = fluid->density[i] * kept;
        f32 temperature = fluid->temperature[i] + ((ambient - fluid->temperature[i]) * cooled);

        // sources add without asking what is already there, so the ceiling is what stops an emitter
        // left running from reaching infinity and turning every later sample into a NaN.
        fluid->density[i]     = nya_clamp(density, 0.0F, NYA_FLUID_FIELD_MAX);
        fluid->temperature[i] = nya_clamp(temperature, -NYA_FLUID_FIELD_MAX, NYA_FLUID_FIELD_MAX);
    }

    _nya_fluid_obstacles_enforce(fluid);

    fluid->step_count++;
    fluid->step_time_s = (f32)(nya_clock_get_monotonic_ns() - started_ns) / 1.0e9F;
}

void nya_fluid_emit(NYA_Fluid* fluid, NYA_FluidEmitter emitter) {
    nya_assert(fluid != nullptr);
    nya_assert(fluid->options.cell_size > 0.0F, "a volume with no cell size");

    f32 radius = emitter.radius > 0.0F ? emitter.radius : fluid->options.cell_size;

    f32x3 half  = { radius, radius, radius };
    u32   low[3];
    u32   high[3];

    if (!_nya_fluid_box_range(fluid, emitter.position - half, emitter.position + half, low, high)) return;

    f32x3 centre          = _nya_fluid_grid_position(fluid, emitter.position);
    f32   radius_cells    = radius / fluid->options.cell_size;
    f32   radius_cells_sq = radius_cells * radius_cells;

    // a 2D volume has no third axis to push along. Cleared here rather than branched on inside the
    // loop so the two dimensional path runs the identical arithmetic to the three dimensional one:
    // a ternary in the loop lets the compiler contract the two branches differently, and the
    // "2D is a one-cell-deep 3D volume" test then fails in the last bits. See render_fluid.h.
    if (fluid->options.space == NYA_FLUID_SPACE_2D) emitter.velocity.z = 0.0F;

    for (u32 k = low[2]; k <= high[2]; k++) {
        for (u32 j = low[1]; j <= high[1]; j++) {
            for (u32 i = low[0]; i <= high[0]; i++) {
                f32 dx = (f32)i - centre.x;
                f32 dy = (f32)j - centre.y;
                f32 dz = (f32)k - centre.z;

                f32 distance_sq = (dx * dx) + (dy * dy) + (dz * dz);
                if (distance_sq >= radius_cells_sq) continue;

                // (1 - t^2)^2: one at the centre, zero with zero slope at the rim, so a moving emitter
                // does not leave a hard edged trail of cells behind it.
                f32 t        = distance_sq / radius_cells_sq;
                f32 falloff  = (1.0F - t) * (1.0F - t);
                u32 index    = _nya_fluid_index(fluid, i, j, k);

                if (fluid->obstacle[index] != 0) continue;

                fluid->density[index]     = nya_clamp(fluid->density[index] + (emitter.density * falloff), 0.0F, NYA_FLUID_FIELD_MAX);
                fluid->temperature[index] = nya_clamp(fluid->temperature[index] + (emitter.temperature * falloff),
                                                      -NYA_FLUID_FIELD_MAX, NYA_FLUID_FIELD_MAX);

                fluid->velocity_x[index] += emitter.velocity.x * falloff;
                fluid->velocity_y[index] += emitter.velocity.y * falloff;
                fluid->velocity_z[index] += emitter.velocity.z * falloff;
            }
        }
    }
}

/*
 * ─────────────────────────────────────────────────────────
 * OBSTACLES
 * ─────────────────────────────────────────────────────────
 */

void nya_fluid_obstacle_box_set(NYA_Fluid* fluid, f32x3 min, f32x3 max) {
    nya_assert(fluid != nullptr);
    nya_assert(min.x <= max.x && min.y <= max.y && min.z <= max.z, "an obstacle box with its corners the wrong way round");

    _nya_fluid_obstacle_box_apply(fluid, min, max, true);
}

void nya_fluid_obstacle_box_clear(NYA_Fluid* fluid, f32x3 min, f32x3 max) {
    nya_assert(fluid != nullptr);
    nya_assert(min.x <= max.x && min.y <= max.y && min.z <= max.z, "an obstacle box with its corners the wrong way round");

    _nya_fluid_obstacle_box_apply(fluid, min, max, false);
}

void nya_fluid_obstacles_clear(NYA_Fluid* fluid) {
    nya_assert(fluid != nullptr);
    nya_assert(fluid->obstacle != nullptr, "a volume with no obstacle mask");

    nya_memset(fluid->obstacle, 0, fluid->cell_count);
    fluid->obstacle_count = 0;
}

/*
 * ─────────────────────────────────────────────────────────
 * SAMPLING
 * ─────────────────────────────────────────────────────────
 */

f32 nya_fluid_density_at(const NYA_Fluid* fluid, f32x3 position) {
    nya_assert(fluid != nullptr);
    nya_assert(fluid->density != nullptr, "a volume with no density field");

    f32x3 grid = _nya_fluid_grid_position(fluid, position);

    return _nya_fluid_sample(fluid, fluid->density, grid.x, grid.y, grid.z);
}

f32 nya_fluid_temperature_at(const NYA_Fluid* fluid, f32x3 position) {
    nya_assert(fluid != nullptr);
    nya_assert(fluid->temperature != nullptr, "a volume with no temperature field");

    f32x3 grid = _nya_fluid_grid_position(fluid, position);

    return _nya_fluid_sample(fluid, fluid->temperature, grid.x, grid.y, grid.z);
}

f32x3 nya_fluid_velocity_at(const NYA_Fluid* fluid, f32x3 position) {
    nya_assert(fluid != nullptr);
    nya_assert(fluid->velocity_x != nullptr, "a volume with no velocity field");

    f32x3 grid = _nya_fluid_grid_position(fluid, position);

    return (f32x3){
        _nya_fluid_sample(fluid, fluid->velocity_x, grid.x, grid.y, grid.z),
        _nya_fluid_sample(fluid, fluid->velocity_y, grid.x, grid.y, grid.z),
        _nya_fluid_sample(fluid, fluid->velocity_z, grid.x, grid.y, grid.z),
    };
}

b8 nya_fluid_cell_index(const NYA_Fluid* fluid, f32x3 position, OUT u32* out_index) {
    nya_assert(fluid != nullptr);
    nya_assert(out_index != nullptr);

    f32x3 grid = _nya_fluid_grid_position(fluid, position);

    // cell i covers [i - 0.5, i + 0.5) in grid coordinates, so the interior spans [0.5, dimension + 0.5).
    if (grid.x < 0.5F || grid.y < 0.5F) return false;
    if (grid.x >= (f32)fluid->width + 0.5F || grid.y >= (f32)fluid->height + 0.5F) return false;

    u32 k = 1;

    if (fluid->options.space != NYA_FLUID_SPACE_2D) {
        if (grid.z < 0.5F || grid.z >= (f32)fluid->depth + 0.5F) return false;
        k = (u32)(grid.z + 0.5F);
    }

    *out_index = _nya_fluid_index(fluid, (u32)(grid.x + 0.5F), (u32)(grid.y + 0.5F), k);

    return true;
}

/*
 * ─────────────────────────────────────────────────────────
 * INTROSPECTION
 * ─────────────────────────────────────────────────────────
 */

u64 nya_fluid_checksum(const NYA_Fluid* fluid) {
    nya_assert(fluid != nullptr);
    nya_assert(fluid->cell_count > 0, "a volume with no cells");

    u64 field_bytes = (u64)fluid->cell_count * sizeof(f32);

    // the five fields a caller can observe. The scratch arrays are swapped between steps, so their
    // contents depend on how many stages ran rather than on the state, and hashing them would make
    // the checksum change for reasons the caller cannot see.
    u64 hash = nya_hash_fnv1a(fluid->velocity_x, field_bytes);

    hash ^= nya_hash_fnv1a(fluid->velocity_y, field_bytes);
    hash ^= nya_hash_fnv1a(fluid->velocity_z, field_bytes);
    hash ^= nya_hash_fnv1a(fluid->density, field_bytes);
    hash ^= nya_hash_fnv1a(fluid->temperature, field_bytes);

    return hash;
}

u32 nya_fluid_cell_count(const NYA_Fluid* fluid) {
    nya_assert(fluid != nullptr);
    nya_assert(fluid->cell_count > 0, "a volume with no cells");

    return fluid->cell_count;
}

u64 nya_fluid_memory_bytes(const NYA_Fluid* fluid) {
    nya_assert(fluid != nullptr);
    nya_assert(fluid->cell_count > 0, "a volume with no cells");

    return sizeof(NYA_Fluid) + ((u64)fluid->cell_count * ((_NYA_FLUID_FIELD_COUNT * sizeof(f32)) + sizeof(u8)));
}

f32 nya_fluid_step_time_s(const NYA_Fluid* fluid) {
    nya_assert(fluid != nullptr);
    nya_assert(fluid->step_time_s >= 0.0F, "a step took negative time");

    return fluid->step_time_s;
}

u32 nya_fluid_count(void) {
    nya_assert(_nya_fluid_registry.count <= NYA_FLUID_VOLUMES_MAX, "the volume table overran");

    return _nya_fluid_registry.count;
}

NYA_Fluid* nya_fluid_at(u32 index) {
    nya_assert(index < _nya_fluid_registry.count, "fluid volume " FMTu32 " of " FMTu32, index, _nya_fluid_registry.count);
    nya_assert(_nya_fluid_registry.volumes[index] != nullptr, "a live row with no volume in it");

    return _nya_fluid_registry.volumes[index];
}

/*
 * ─────────────────────────────────────────────────────────
 * DRAWING
 * ─────────────────────────────────────────────────────────
 */

void nya_fluid_render_options_set(NYA_Window* window, NYA_FluidRenderOptions options) {
    nya_assert(window != nullptr);
    nya_assert(options.stride <= NYA_FLUID_DIMENSION_MAX, "a draw stride of " FMTu32 " would skip the whole grid", options.stride);

    window->render_system.fluid = options;
}

NYA_FluidRenderOptions nya_fluid_render_options(const NYA_Window* window) {
    nya_assert(window != nullptr);
    nya_assert(window->render_system.fluid.opacity >= 0.0F, "a negative fluid opacity was stored");

    return window->render_system.fluid;
}

void nya_fluid_draw(NYA_Window* window, const NYA_Fluid* fluid) {
    nya_perf_time_this_function();
    nya_trace_scope(NYA_TRACE_FLUID);

    nya_assert(window != nullptr);
    nya_assert(fluid == nullptr || fluid->cell_count > 0, "a volume with no cells");

    if (fluid == nullptr) return;
    if (!window->render_system.fluid.enabled) return;

    NYA_FluidRenderOptions options = _nya_fluid_render_options_resolved(window->render_system.fluid);

    if (fluid->options.space == NYA_FLUID_SPACE_2D) {
        _nya_fluid_draw_2d(window, fluid, options);
        return;
    }

    // no projection, nothing to draw. The same rule nya_particles_draw follows for a 3D system.
    if (!nya_render3d_active(window)) return;

    _nya_fluid_draw_3d(window, fluid, options);
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * INTERNAL
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

void _nya_fluid_fields_swap(f32** a, f32** b) {
    nya_assert(a != nullptr && b != nullptr);
    nya_assert(*a != *b, "a field swapped with itself would lose the stage's input");

    f32* held = *a;

    *a = *b;
    *b = held;
}

u32 _nya_fluid_index(const NYA_Fluid* fluid, u32 i, u32 j, u32 k) {
    nya_assert(i < fluid->stride_x && j < fluid->stride_y && k < fluid->stride_z,
               "cell (" FMTu32 ", " FMTu32 ", " FMTu32 ") is outside the grid", i, j, k);

    u32 index = i + (j * fluid->stride_x) + (k * fluid->stride_x * fluid->stride_y);

    nya_assert(index < fluid->cell_count, "cell index " FMTu32 " of " FMTu32, index, fluid->cell_count);

    return index;
}

void _nya_fluid_bounds_set(NYA_Fluid* fluid, f32* field, u32 bound) {
    nya_assert(fluid != nullptr && field != nullptr);
    nya_assert(bound <= _NYA_FLUID_BOUND_VELOCITY_Z, "unknown boundary rule " FMTu32, bound);

    u32 width  = fluid->width;
    u32 height = fluid->height;
    u32 depth  = fluid->depth;

    u32 step_y = fluid->stride_x;
    u32 step_z = fluid->stride_x * fluid->stride_y;

    /*
     * A wall reflects the component that would cross it and mirrors everything else. That is what
     * makes the box closed for velocity and open (no gradient) for pressure, density and heat.
     */
    f32 sign_x = bound == _NYA_FLUID_BOUND_VELOCITY_X ? -1.0F : 1.0F;
    f32 sign_y = bound == _NYA_FLUID_BOUND_VELOCITY_Y ? -1.0F : 1.0F;
    f32 sign_z = bound == _NYA_FLUID_BOUND_VELOCITY_Z ? -1.0F : 1.0F;

    u32 last_x = width + 1;
    u32 last_y = height + 1;
    u32 last_z = depth + 1;

    /*
     * Indices are stepped rather than recomputed per cell. This runs once per Gauss-Seidel sweep, so
     * roughly forty times a step, and a two-assertion index helper in here costs more than the
     * arithmetic it was checking; the bound is asserted once per plane instead.
     */
    for (u32 k = 1; k <= depth; k++) {
        for (u32 j = 1; j <= height; j++) {
            u32 row = (k * step_z) + (j * step_y);

            field[row]              = sign_x * field[row + 1];
            field[row + last_x]     = sign_x * field[row + width];
        }
    }

    for (u32 k = 1; k <= depth; k++) {
        u32 plane = k * step_z;

        for (u32 i = 1; i <= width; i++) {
            field[plane + i]                   = sign_y * field[plane + step_y + i];
            field[plane + (last_y * step_y) + i] = sign_y * field[plane + (height * step_y) + i];
        }
    }

    /*
     * The two z planes are what reduce the solver to two dimensions when depth is one: they mirror
     * the single interior plane, so the seven point stencil's z terms cancel against the centre cell.
     * See "2D is 3D one cell deep" in the header.
     */
    for (u32 j = 1; j <= height; j++) {
        u32 row = j * step_y;

        for (u32 i = 1; i <= width; i++) {
            field[row + i]                         = sign_z * field[step_z + row + i];
            field[(last_z * step_z) + row + i]     = sign_z * field[(depth * step_z) + row + i];
        }
    }

    /*
     * Edges and corners are the average of the face cells meeting there. Advection clamps its sample
     * to half a cell inside the border, so it does reach them; leaving them at zero puts a dark seam
     * down every edge of the box.
     */
    for (u32 i = 1; i <= width; i++) {
        field[i]                                             = 0.5F * (field[step_y + i] + field[step_z + i]);
        field[(last_y * step_y) + i]                         = 0.5F * (field[(height * step_y) + i] + field[step_z + (last_y * step_y) + i]);
        field[(last_z * step_z) + i]                         = 0.5F * (field[(last_z * step_z) + step_y + i] + field[(depth * step_z) + i]);
        field[(last_z * step_z) + (last_y * step_y) + i]     = 0.5F * (field[(last_z * step_z) + (height * step_y) + i] +
                                                                   field[(depth * step_z) + (last_y * step_y) + i]);
    }

    for (u32 j = 1; j <= height; j++) {
        u32 row = j * step_y;

        field[row]                                       = 0.5F * (field[row + 1] + field[step_z + row]);
        field[row + last_x]                              = 0.5F * (field[row + width] + field[step_z + row + last_x]);
        field[(last_z * step_z) + row]                   = 0.5F * (field[(last_z * step_z) + row + 1] + field[(depth * step_z) + row]);
        field[(last_z * step_z) + row + last_x]          = 0.5F * (field[(last_z * step_z) + row + width] +
                                                          field[(depth * step_z) + row + last_x]);
    }

    for (u32 k = 1; k <= depth; k++) {
        u32 plane = k * step_z;

        field[plane]                                     = 0.5F * (field[plane + 1] + field[plane + step_y]);
        field[plane + last_x]                            = 0.5F * (field[plane + width] + field[plane + step_y + last_x]);
        field[plane + (last_y * step_y)]                 = 0.5F * (field[plane + (last_y * step_y) + 1] + field[plane + (height * step_y)]);
        field[plane + (last_y * step_y) + last_x]        = 0.5F * (field[plane + (last_y * step_y) + width] +
                                                            field[plane + (height * step_y) + last_x]);
    }

    const u32 corner_x[2] = { 0, last_x };
    const u32 corner_y[2] = { 0, last_y };
    const u32 corner_z[2] = { 0, last_z };

    for (u32 cz = 0; cz < 2; cz++) {
        for (u32 cy = 0; cy < 2; cy++) {
            for (u32 cx = 0; cx < 2; cx++) {
                u32 i = corner_x[cx];
                u32 j = corner_y[cy];
                u32 k = corner_z[cz];

                u32 inner_i = cx == 0 ? 1 : width;
                u32 inner_j = cy == 0 ? 1 : height;
                u32 inner_k = cz == 0 ? 1 : depth;

                field[_nya_fluid_index(fluid, i, j, k)] = (field[_nya_fluid_index(fluid, inner_i, j, k)] +
                                                           field[_nya_fluid_index(fluid, i, inner_j, k)] +
                                                           field[_nya_fluid_index(fluid, i, j, inner_k)]) /
                                                          3.0F;
            }
        }
    }
}

void _nya_fluid_linear_solve(NYA_Fluid* fluid, f32* field, const f32* field_source, f32 a, f32 c, u32 bound, u32 iterations) {
    nya_assert(fluid != nullptr && field != nullptr && field_source != nullptr);
    nya_assert(c != 0.0F, "a linear solve with a zero divisor would divide by zero");
    nya_assert(iterations > 0 && iterations <= NYA_FLUID_PRESSURE_ITERATIONS_MAX, "a solve of " FMTu32 " sweeps", iterations);

    f32 c_inverse = 1.0F / c;

    u32 step_y = fluid->stride_x;
    u32 step_z = fluid->stride_x * fluid->stride_y;

    const u8* obstacle = fluid->obstacle;

    // hoisted out of the loop so a volume with no obstacles runs the plain stencil and never touches
    // the mask. Both arms are the same expression with and without the wall rule, which is why this
    // is one function and not an obstacle-free copy of the solve beside an obstacle-aware one.
    b8 walled = fluid->obstacle_count > 0;

    for (u32 iteration = 0; iteration < iterations; iteration++) {
        for (u32 k = 1; k <= fluid->depth; k++) {
            for (u32 j = 1; j <= fluid->height; j++) {
                u32 row = (k * step_z) + (j * step_y);

                nya_assert(row + fluid->width + step_z < fluid->cell_count, "a solve row runs past the grid");

                for (u32 i = 1; i <= fluid->width; i++) {
                    u32 index = row + i;

                    if (walled && obstacle[index] != 0) continue;

                    f32 centre = field[index];

                    f32 neighbours =
                        (walled && obstacle[index - 1] != 0 ? centre : field[index - 1]) +
                        (walled && obstacle[index + 1] != 0 ? centre : field[index + 1]) +
                        (walled && obstacle[index - step_y] != 0 ? centre : field[index - step_y]) +
                        (walled && obstacle[index + step_y] != 0 ? centre : field[index + step_y]) +
                        (walled && obstacle[index - step_z] != 0 ? centre : field[index - step_z]) +
                        (walled && obstacle[index + step_z] != 0 ? centre : field[index + step_z]);

                    field[index] = (field_source[index] + (a * neighbours)) * c_inverse;
                }
            }
        }

        _nya_fluid_bounds_set(fluid, field, bound);
    }
}

void _nya_fluid_advect(NYA_Fluid* fluid, f32* field, const f32* field_source, f32 delta_time_s, u32 bound) {
    nya_assert(fluid != nullptr && field != nullptr && field_source != nullptr);
    nya_assert(delta_time_s > 0.0F && delta_time_s <= NYA_FLUID_STEP_SECONDS_MAX, "advecting over " FMTf32 " seconds",
               (f64)delta_time_s);

    // velocity is world units per second and the grid is indexed in cells, so one conversion covers
    // the whole trace back.
    f32 cells_per_unit = delta_time_s / fluid->options.cell_size;

    u32 step_y = fluid->stride_x;
    u32 step_z = fluid->stride_x * fluid->stride_y;

    for (u32 k = 1; k <= fluid->depth; k++) {
        for (u32 j = 1; j <= fluid->height; j++) {
            u32 row = (k * step_z) + (j * step_y);

            for (u32 i = 1; i <= fluid->width; i++) {
                u32 index = row + i;

                f32 x = (f32)i - (cells_per_unit * fluid->velocity_x[index]);
                f32 y = (f32)j - (cells_per_unit * fluid->velocity_y[index]);
                // a 2D volume's z velocity is held at exactly zero by every path that could write it,
                // so this reduces to (f32)k with no branch to make the two spaces round differently.
                f32 z = (f32)k - (cells_per_unit * fluid->velocity_z[index]);

                field[index] = _nya_fluid_sample(fluid, field_source, x, y, z);
            }
        }
    }

    _nya_fluid_bounds_set(fluid, field, bound);
}

void _nya_fluid_project(NYA_Fluid* fluid) {
    nya_assert(fluid != nullptr);
    nya_assert(fluid->options.cell_size > 0.0F, "a volume with no cell size");

    f32 cell_size = fluid->options.cell_size;

    u32 step_y = fluid->stride_x;
    u32 step_z = fluid->stride_x * fluid->stride_y;

    for (u32 k = 1; k <= fluid->depth; k++) {
        for (u32 j = 1; j <= fluid->height; j++) {
            u32 row = (k * step_z) + (j * step_y);

            for (u32 i = 1; i <= fluid->width; i++) {
                u32 index = row + i;

                f32 difference_x = fluid->velocity_x[index + 1] - fluid->velocity_x[index - 1];
                f32 difference_y = fluid->velocity_y[index + step_y] - fluid->velocity_y[index - step_y];
                f32 difference_z = fluid->velocity_z[index + step_z] - fluid->velocity_z[index - step_z];

                /*
                 * Stored already negated and already multiplied by the cell size squared, which is
                 * exactly the right hand side the Gauss-Seidel sweep below takes: the discrete
                 * Poisson equation (sum of neighbours - 6p) / h^2 = divergence rearranges to
                 * p = (sum of neighbours - h^2 * divergence) / 6.
                 */
                fluid->divergence[index] = -0.5F * cell_size * (difference_x + difference_y + difference_z);

                // cold start: a pressure left over from the previous step belongs to a velocity field
                // that no longer exists, and warm starting from it converges to the same answer no
                // faster while making the step depend on how many steps came before it.
                fluid->pressure[index] = 0.0F;
            }
        }
    }

    _nya_fluid_bounds_set(fluid, fluid->divergence, _NYA_FLUID_BOUND_SCALAR);
    _nya_fluid_bounds_set(fluid, fluid->pressure, _NYA_FLUID_BOUND_SCALAR);

    _nya_fluid_linear_solve(fluid, fluid->pressure, fluid->divergence, 1.0F, 6.0F, _NYA_FLUID_BOUND_SCALAR,
                            fluid->options.pressure_iterations);

    f32 gradient_scale = 0.5F / cell_size;

    for (u32 k = 1; k <= fluid->depth; k++) {
        for (u32 j = 1; j <= fluid->height; j++) {
            u32 row = (k * step_z) + (j * step_y);

            for (u32 i = 1; i <= fluid->width; i++) {
                u32 index = row + i;

                fluid->velocity_x[index] -= gradient_scale * (fluid->pressure[index + 1] - fluid->pressure[index - 1]);
                fluid->velocity_y[index] -= gradient_scale * (fluid->pressure[index + step_y] - fluid->pressure[index - step_y]);
                fluid->velocity_z[index] -= gradient_scale * (fluid->pressure[index + step_z] - fluid->pressure[index - step_z]);
            }
        }
    }

    _nya_fluid_bounds_set(fluid, fluid->velocity_x, _NYA_FLUID_BOUND_VELOCITY_X);
    _nya_fluid_bounds_set(fluid, fluid->velocity_y, _NYA_FLUID_BOUND_VELOCITY_Y);
    _nya_fluid_bounds_set(fluid, fluid->velocity_z, _NYA_FLUID_BOUND_VELOCITY_Z);

    _nya_fluid_obstacles_enforce(fluid);
}

void _nya_fluid_forces_add(NYA_Fluid* fluid, f32 delta_time_s) {
    nya_assert(fluid != nullptr);
    nya_assert(delta_time_s > 0.0F, "forces over a non-positive step");

    f32x3 up      = fluid->options.up;
    f32x3 gravity = fluid->options.gravity;

    f32 buoyancy = fluid->options.buoyancy;
    f32 weight   = fluid->options.weight;
    f32 ambient  = fluid->options.ambient_temperature;

    b8 buoyant = buoyancy != 0.0F || weight != 0.0F;
    b8 heavy   = gravity.x != 0.0F || gravity.y != 0.0F || gravity.z != 0.0F;

    // the case that costs nothing: a volume asked for neither buoyancy nor gravity never walks itself.
    if (!buoyant && !heavy) return;

    u32 step_y = fluid->stride_x;
    u32 step_z = fluid->stride_x * fluid->stride_y;

    for (u32 k = 1; k <= fluid->depth; k++) {
        for (u32 j = 1; j <= fluid->height; j++) {
            u32 row = (k * step_z) + (j * step_y);

            for (u32 i = 1; i <= fluid->width; i++) {
                u32 index = row + i;

                // hot fluid rises and dense fluid sinks, both along the volume's own up axis, so a
                // 2D volume whose y grows down the screen needs no special case anywhere else.
                f32 lift = (buoyancy * (fluid->temperature[index] - ambient)) - (weight * fluid->density[index]);

                fluid->velocity_x[index] += delta_time_s * ((up.x * lift) + gravity.x);
                fluid->velocity_y[index] += delta_time_s * ((up.y * lift) + gravity.y);
                fluid->velocity_z[index] += delta_time_s * ((up.z * lift) + gravity.z);
            }
        }
    }

    _nya_fluid_bounds_set(fluid, fluid->velocity_x, _NYA_FLUID_BOUND_VELOCITY_X);
    _nya_fluid_bounds_set(fluid, fluid->velocity_y, _NYA_FLUID_BOUND_VELOCITY_Y);
    _nya_fluid_bounds_set(fluid, fluid->velocity_z, _NYA_FLUID_BOUND_VELOCITY_Z);
}

void _nya_fluid_vorticity_add(NYA_Fluid* fluid, f32 delta_time_s) {
    nya_assert(fluid != nullptr);
    nya_assert(delta_time_s > 0.0F, "confinement over a non-positive step");

    // the case that costs nothing: confinement is off unless a volume asked for it.
    if (fluid->options.vorticity <= 0.0F) return;

    f32 cell_size    = fluid->options.cell_size;
    f32 half_inverse = 0.5F / cell_size;

    u32 step_y = fluid->stride_x;
    u32 step_z = fluid->stride_x * fluid->stride_y;

    /*
     * The curl goes in the velocity scratch arrays, which are free here: the next stage either swaps
     * them out for diffusion or overwrites them for advection. One dedicated array is kept for the
     * magnitude, because confinement needs its gradient after every component is known.
     */
    f32* curl_x = fluid->velocity_x_previous;
    f32* curl_y = fluid->velocity_y_previous;
    f32* curl_z = fluid->velocity_z_previous;

    for (u32 k = 1; k <= fluid->depth; k++) {
        for (u32 j = 1; j <= fluid->height; j++) {
            u32 row = (k * step_z) + (j * step_y);

            for (u32 i = 1; i <= fluid->width; i++) {
                u32 index = row + i;

                f32 x = half_inverse * ((fluid->velocity_z[index + step_y] - fluid->velocity_z[index - step_y]) -
                                        (fluid->velocity_y[index + step_z] - fluid->velocity_y[index - step_z]));
                f32 y = half_inverse * ((fluid->velocity_x[index + step_z] - fluid->velocity_x[index - step_z]) -
                                        (fluid->velocity_z[index + 1] - fluid->velocity_z[index - 1]));
                f32 z = half_inverse * ((fluid->velocity_y[index + 1] - fluid->velocity_y[index - 1]) -
                                        (fluid->velocity_x[index + step_y] - fluid->velocity_x[index - step_y]));

                curl_x[index] = x;
                curl_y[index] = y;
                curl_z[index] = z;

                fluid->curl_magnitude[index] = sqrtf((x * x) + (y * y) + (z * z));
            }
        }
    }

    _nya_fluid_bounds_set(fluid, fluid->curl_magnitude, _NYA_FLUID_BOUND_SCALAR);

    f32 strength = fluid->options.vorticity * cell_size * delta_time_s;

    for (u32 k = 1; k <= fluid->depth; k++) {
        for (u32 j = 1; j <= fluid->height; j++) {
            u32 row = (k * step_z) + (j * step_y);

            for (u32 i = 1; i <= fluid->width; i++) {
                u32 index = row + i;

                // toward the nearest vortex centre, which is where the gradient of |curl| points.
                f32 gradient_x = half_inverse * (fluid->curl_magnitude[index + 1] - fluid->curl_magnitude[index - 1]);
                f32 gradient_y = half_inverse * (fluid->curl_magnitude[index + step_y] - fluid->curl_magnitude[index - step_y]);
                f32 gradient_z = half_inverse * (fluid->curl_magnitude[index + step_z] - fluid->curl_magnitude[index - step_z]);

                f32 length = sqrtf((gradient_x * gradient_x) + (gradient_y * gradient_y) + (gradient_z * gradient_z));

                // still fluid: the direction here is rounding noise, so no force rather than an
                // arbitrary one. See _NYA_FLUID_VORTICITY_EPSILON.
                if (length < _NYA_FLUID_VORTICITY_EPSILON) continue;

                f32 inverse = 1.0F / length;

                f32 normal_x = gradient_x * inverse;
                f32 normal_y = gradient_y * inverse;
                f32 normal_z = gradient_z * inverse;

                fluid->velocity_x[index] += strength * ((normal_y * curl_z[index]) - (normal_z * curl_y[index]));
                fluid->velocity_y[index] += strength * ((normal_z * curl_x[index]) - (normal_x * curl_z[index]));
                fluid->velocity_z[index] += strength * ((normal_x * curl_y[index]) - (normal_y * curl_x[index]));
            }
        }
    }

    _nya_fluid_bounds_set(fluid, fluid->velocity_x, _NYA_FLUID_BOUND_VELOCITY_X);
    _nya_fluid_bounds_set(fluid, fluid->velocity_y, _NYA_FLUID_BOUND_VELOCITY_Y);
    _nya_fluid_bounds_set(fluid, fluid->velocity_z, _NYA_FLUID_BOUND_VELOCITY_Z);
}

void _nya_fluid_obstacles_enforce(NYA_Fluid* fluid) {
    nya_assert(fluid != nullptr);
    nya_assert(fluid->obstacle_count <= fluid->cell_count, "more solid cells than cells");

    // the case that costs nothing: a volume with no obstacles never walks the mask.
    if (fluid->obstacle_count == 0) return;

    f32 ambient = fluid->options.ambient_temperature;

    for (u32 index = 0; index < fluid->cell_count; index++) {
        if (fluid->obstacle[index] == 0) continue;

        fluid->velocity_x[index] = 0.0F;
        fluid->velocity_y[index] = 0.0F;
        fluid->velocity_z[index] = 0.0F;

        // a solid cell holds no fluid, so a sample taken inside one reads empty rather than stale.
        fluid->density[index]     = 0.0F;
        fluid->temperature[index] = ambient;
    }
}

f32 _nya_fluid_sample(const NYA_Fluid* fluid, const f32* field, f32 x, f32 y, f32 z) {
    nya_assert(fluid != nullptr && field != nullptr);
    nya_assert(fluid->stride_x >= 3 && fluid->stride_y >= 3 && fluid->stride_z >= 3, "a grid smaller than one interior cell");

    // half a cell inside the border on each side, so the eight corners of the interpolation are
    // always addressable and a fast flow clamps to the wall instead of reading off the end.
    f32 clamped_x = nya_clamp(x, 0.5F, (f32)fluid->width + 0.5F);
    f32 clamped_y = nya_clamp(y, 0.5F, (f32)fluid->height + 0.5F);
    f32 clamped_z = nya_clamp(z, 0.5F, (f32)fluid->depth + 0.5F);

    u32 i0 = (u32)clamped_x;
    u32 j0 = (u32)clamped_y;
    u32 k0 = (u32)clamped_z;

    f32 tx = clamped_x - (f32)i0;
    f32 ty = clamped_y - (f32)j0;
    f32 tz = clamped_z - (f32)k0;

    f32 sx = 1.0F - tx;
    f32 sy = 1.0F - ty;
    f32 sz = 1.0F - tz;

    // stepped rather than indexed eight times: this runs four times per cell per step, and the index
    // helper's own assertions cost more here than the arithmetic they check. The clamp above is what
    // makes every one of the eight corners addressable, and the assertion below is that bound.
    u32 step_y = fluid->stride_x;
    u32 step_z = fluid->stride_x * fluid->stride_y;

    u32 corner = i0 + (j0 * step_y) + (k0 * step_z);

    nya_assert(corner + 1 + step_y + step_z < fluid->cell_count, "a sample's far corner is outside the grid");

    f32 low = sz * ((sy * ((sx * field[corner]) + (tx * field[corner + 1]))) +
                    (ty * ((sx * field[corner + step_y]) + (tx * field[corner + step_y + 1]))));

    f32 high = tz * ((sy * ((sx * field[corner + step_z]) + (tx * field[corner + step_z + 1]))) +
                     (ty * ((sx * field[corner + step_z + step_y]) + (tx * field[corner + step_z + step_y + 1]))));

    return low + high;
}

f32x3 _nya_fluid_grid_position(const NYA_Fluid* fluid, f32x3 position) {
    nya_assert(fluid != nullptr);
    nya_assert(fluid->options.cell_size > 0.0F, "a volume with no cell size");

    f32 inverse = 1.0F / fluid->options.cell_size;

    // a 2D volume has one interior plane and z means nothing, so every sample lands on that plane.
    // Without this, a caller passing z = 0 lands halfway into the border plane and reads half the
    // density that is actually there.
    f32 z = fluid->options.space == NYA_FLUID_SPACE_2D ? 1.0F : ((position.z - fluid->options.origin.z) * inverse) + 0.5F;

    // interior cell i is centred on grid coordinate i, and spans world [origin + (i-1)*h, origin + i*h].
    return (f32x3){
        ((position.x - fluid->options.origin.x) * inverse) + 0.5F,
        ((position.y - fluid->options.origin.y) * inverse) + 0.5F,
        z,
    };
}

b8 _nya_fluid_box_range(const NYA_Fluid* fluid, f32x3 min, f32x3 max, OUT u32 out_low[3], OUT u32 out_high[3]) {
    nya_assert(fluid != nullptr);
    nya_assert(out_low != nullptr && out_high != nullptr);

    f32x3 low  = _nya_fluid_grid_position(fluid, min);
    f32x3 high = _nya_fluid_grid_position(fluid, max);

    const f32 limits[3] = { (f32)fluid->width, (f32)fluid->height, (f32)fluid->depth };
    const f32 lows[3]   = { low.x, low.y, low.z };
    const f32 highs[3]  = { high.x, high.y, high.z };

    for (u32 axis = 0; axis < 3; axis++) {
        // a 2D volume's single interior plane, whatever z the caller passed.
        if (axis == 2 && fluid->options.space == NYA_FLUID_SPACE_2D) {
            out_low[axis]  = 1;
            out_high[axis] = 1;
            continue;
        }

        if (highs[axis] < 0.5F || lows[axis] > limits[axis] + 0.5F) return false;

        f32 first = nya_clamp(floorf(lows[axis] + 0.5F), 1.0F, limits[axis]);
        f32 last  = nya_clamp(floorf(highs[axis] + 0.5F), 1.0F, limits[axis]);

        out_low[axis]  = (u32)first;
        out_high[axis] = (u32)last;

        nya_assert(out_low[axis] <= out_high[axis], "a clamped cell range came out inverted on axis " FMTu32, axis);
    }

    return true;
}

void _nya_fluid_obstacle_box_apply(NYA_Fluid* fluid, f32x3 min, f32x3 max, b8 solid) {
    nya_assert(fluid != nullptr);
    nya_assert(fluid->obstacle_count <= fluid->cell_count, "more solid cells than cells");

    u32 low[3];
    u32 high[3];

    if (!_nya_fluid_box_range(fluid, min, max, low, high)) return;

    u8 value = solid ? 1 : 0;

    for (u32 k = low[2]; k <= high[2]; k++) {
        for (u32 j = low[1]; j <= high[1]; j++) {
            for (u32 i = low[0]; i <= high[0]; i++) {
                u32 index = _nya_fluid_index(fluid, i, j, k);

                if (fluid->obstacle[index] == value) continue;

                fluid->obstacle[index] = value;

                // kept exact here rather than recounted, so the "no obstacles" fast path is free.
                if (solid) {
                    fluid->obstacle_count++;
                } else {
                    fluid->obstacle_count--;
                }
            }
        }
    }

    nya_assert(fluid->obstacle_count <= fluid->cell_count, "the solid cell count overran the grid");
}

NYA_FluidRenderOptions _nya_fluid_render_options_resolved(NYA_FluidRenderOptions options) {
    nya_assert(options.opacity >= 0.0F, "a negative fluid opacity");
    nya_assert(options.threshold >= 0.0F, "a negative fluid draw threshold");

    if (options.opacity <= 0.0F) options.opacity = NYA_FLUID_DRAW_OPACITY;
    if (options.threshold <= 0.0F) options.threshold = NYA_FLUID_DRAW_THRESHOLD;
    if (options.density_full <= 0.0F) options.density_full = NYA_FLUID_DRAW_DENSITY_FULL;
    if (options.hot_temperature <= 0.0F) options.hot_temperature = 1.0F;
    if (options.stride == 0) options.stride = 1;

    if (options.cool.r == 0.0F && options.cool.g == 0.0F && options.cool.b == 0.0F && options.cool.a == 0.0F) {
        options.cool = (NYA_Color){ 1.0F, 1.0F, 1.0F, 1.0F };
    }

    return options;
}

NYA_Color _nya_fluid_cell_color(const NYA_Fluid* fluid, NYA_FluidRenderOptions options, u32 index) {
    nya_assert(fluid != nullptr);
    nya_assert(index < fluid->cell_count, "cell index " FMTu32 " of " FMTu32, index, fluid->cell_count);

    f32 density = fluid->density[index];
    f32 heat    = (fluid->temperature[index] - fluid->options.ambient_temperature) / options.hot_temperature;

    f32 mix   = nya_clamp(heat, 0.0F, 1.0F);
    f32 alpha = nya_clamp(density / options.density_full, 0.0F, 1.0F) * options.opacity;

    return (NYA_Color){
        nya_lerp(options.cool.r, options.hot.r, mix),
        nya_lerp(options.cool.g, options.hot.g, mix),
        nya_lerp(options.cool.b, options.hot.b, mix),
        alpha,
    };
}

void _nya_fluid_draw_2d(NYA_Window* window, const NYA_Fluid* fluid, NYA_FluidRenderOptions options) {
    nya_assert(window != nullptr && fluid != nullptr);
    nya_assert(fluid->options.space == NYA_FLUID_SPACE_2D, "a 3D volume drawn through the 2D path");

    f32 cell_size = fluid->options.cell_size;
    f32 size      = cell_size * (f32)options.stride;

    for (u32 j = 1; j <= fluid->height; j += options.stride) {
        for (u32 i = 1; i <= fluid->width; i += options.stride) {
            u32 index = _nya_fluid_index(fluid, i, j, 1);

            if (fluid->density[index] < options.threshold) continue;

            /*
             * Shaded from the four cells around each corner rather than flat per cell. It is the same
             * vertex count through the same batch, and without it a coarse grid reads as a mosaic
             * instead of as smoke.
             */
            NYA_Color corners[4];

            for (u32 corner = 0; corner < 4; corner++) {
                u32 corner_i = i + ((corner == 1 || corner == 2) ? options.stride : 0);
                u32 corner_j = j + ((corner == 2 || corner == 3) ? options.stride : 0);

                corner_i = nya_min(corner_i, fluid->width);
                corner_j = nya_min(corner_j, fluid->height);

                corners[corner] = _nya_fluid_cell_color(fluid, options, _nya_fluid_index(fluid, corner_i, corner_j, 1));
            }

            f32 x = fluid->options.origin.x + ((f32)(i - 1) * cell_size);
            f32 y = fluid->options.origin.y + ((f32)(j - 1) * cell_size);

            nya_render2d_rect_gradient(window, x, y, size, size, corners);
        }
    }
}

void _nya_fluid_draw_3d(NYA_Window* window, const NYA_Fluid* fluid, NYA_FluidRenderOptions options) {
    nya_assert(window != nullptr && fluid != nullptr);
    nya_assert(fluid->options.space == NYA_FLUID_SPACE_3D, "a 2D volume drawn through the 3D path");

    /*
     * Additive, so the splats need no sorting: addition commutes and the order cells come out of the
     * grid in is therefore invisible. It is also what makes a hot volume glow, since bloom reads the
     * scene after this and a stack of overlapping splats exceeds one.
     */
    nya_render3d_blend_set(window, NYA_RENDER3D_BLEND_ADDITIVE);

    // resolved once for the whole volume, the same reason nya_particles_draw resolves before its loop.
    NYA_Render3DTextureBinding texture = nya_render3d_texture_resolve(nullptr);

    f32 cell_size = fluid->options.cell_size;

    // wider than the cell, so neighbouring splats overlap into a field instead of a lattice of dots.
    f32x2 size = { cell_size * (f32)options.stride * 1.5F, cell_size * (f32)options.stride * 1.5F };

    for (u32 k = 1; k <= fluid->depth; k += options.stride) {
        for (u32 j = 1; j <= fluid->height; j += options.stride) {
            for (u32 i = 1; i <= fluid->width; i += options.stride) {
                u32 index = _nya_fluid_index(fluid, i, j, k);

                if (fluid->density[index] < options.threshold) continue;

                NYA_Color color = _nya_fluid_cell_color(fluid, options, index);

                f32x3 centre = {
                    fluid->options.origin.x + (((f32)i - 0.5F) * cell_size),
                    fluid->options.origin.y + (((f32)j - 0.5F) * cell_size),
                    fluid->options.origin.z + (((f32)k - 0.5F) * cell_size),
                };

                nya_render3d_billboard_resolved(window, texture, centre, size, 0.0F, color);
            }
        }
    }

    // back to the default rather than to a remembered value: render3d exposes no getter for the
    // blend, and every caller in this engine draws additive geometry last for that reason.
    nya_render3d_blend_set(window, NYA_RENDER3D_BLEND_ALPHA);
}
