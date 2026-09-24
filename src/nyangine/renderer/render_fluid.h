/**
 * @file render_fluid.h
 *
 * Eulerian fluid: a grid of velocity, density and temperature stepped with the incompressible
 * Navier-Stokes equations, for smoke, fire and shallow water. One solver covers both dimensions; see
 * "2D is 3D one cell deep" below.
 *
 * Overview:
 *   nya_fluid_create / _destroy            the volume, allocated once at the size given here
 *   nya_fluid_clear                        empties the fields, keeping the obstacles and the options
 *   nya_fluid_options_set / _options       the solver's knobs, changeable between steps
 *   nya_fluid_step                         one step of the simulation, from an explicit timestep
 *   nya_fluid_emit                         density, heat and velocity into a sphere of the grid
 *   nya_fluid_obstacle_box_set / _clear    a solid box, in world units
 *   nya_fluid_obstacles_clear              every obstacle at once
 *   nya_fluid_density_at /
 *     _temperature_at / _velocity_at       the fields sampled at a world point
 *   nya_fluid_checksum                     the whole state in one u64, for replay tests
 *   nya_fluid_cell_count / _memory_bytes /
 *     _step_time_s                         what this volume costs
 *   nya_fluid_count / _at                  the live volumes, for the overlay and the harness
 *   nya_fluid_render_options_set /
 *     nya_fluid_render_options             per window, off until a window asks for it
 *   nya_fluid_draw                         through render2d or render3d, by the volume's space
 *
 * ```c
 * // once, at load
 * NYA_Fluid* smoke = nya_fluid_create(world->allocator, (NYA_FluidOptions){
 *     .space       = NYA_FLUID_SPACE_3D,
 *     .width       = 32,
 *     .height      = 48,
 *     .depth       = 32,
 *     .cell_size   = 0.5F,
 *     .origin      = { -8.0F, 0.0F, -8.0F },
 *     .buoyancy    = 3.0F,
 *     .vorticity   = 1.0F,
 *     .dissipation = 0.4F,
 * });
 *
 * nya_fluid_render_options_set(window, (NYA_FluidRenderOptions){ .enabled = true });
 *
 * // once a tick
 * nya_fluid_emit(smoke, (NYA_FluidEmitter){
 *     .position    = { 0.0F, 1.0F, 0.0F },
 *     .radius      = 1.0F,
 *     .density     = 6.0F * delta_time_s,
 *     .temperature = 9.0F * delta_time_s,
 *     .velocity    = { 0.0F, 2.0F, 0.0F },
 * });
 * nya_fluid_step(smoke, delta_time_s);
 *
 * // from a layer's on_render, between nya_render3d_begin and _end for a 3D volume
 * nya_fluid_draw(window, smoke);
 * ```
 *
 * ## Why a grid and not SPH particles
 *
 * Smoke and fire are what this engine needs a fluid for, and they are a density field rather than a
 * set of droplets. A grid produces that field directly, keeps it divergence free, and costs the same
 * every frame whatever the fluid is doing.
 *
 * Smoothed particle hydrodynamics was rejected on four counts, all of them house rules rather than
 * taste:
 *
 *   - Fixed capacity. A grid is allocated once and every step touches exactly the same cells. SPH
 *     rebuilds a neighbour structure every frame and its cost follows how the particles clump, which
 *     is the shape of cost this codebase does not accept.
 *   - Determinism. A grid sweep is a fixed loop in a fixed order, so the same inputs give the same
 *     floats. An SPH neighbour list is gathered in whatever order particles currently sit in, and a
 *     different summation order is a different sum.
 *   - Bounded work. Semi-Lagrangian advection is unconditionally stable, so a long frame is a
 *     coarser step and never an explosion. SPH is CFL limited and a fast particle demands substeps,
 *     which is unbounded work driven by the data.
 *   - The look. Volumetric smoke out of SPH means splatting the particles into a grid to render
 *     them, so the grid is paid for anyway.
 *
 * What would bring SPH back: a small volume of liquid moving through a large empty space, a boat's
 * wake across a lake, where a grid would have to cover the whole lake to simulate the wake. Nothing
 * in scope needs that. If it ever does, it is a second module beside this one and not a rewrite of
 * it, because the two answer different questions.
 *
 * Also rejected: a lattice Boltzmann solver, which is elegant and trivially parallel but stores
 * nineteen distribution values per cell against this solver's thirteen fields, and is only
 * conditionally stable at the low viscosities stylized smoke wants.
 *
 * ## 2D is 3D one cell deep
 *
 * There is one solver and it is three dimensional. A 2D volume is a grid whose depth is one, whose
 * `w` velocity nothing ever drives, and whose two z boundary planes mirror the single interior
 * plane. Every term then collapses by itself: the seven point Laplacian's two z neighbours both
 * equal the centre cell, so `p_left + p_right + p_up + p_down + 2p - div = 6p` is exactly the 2D
 * `p_left + p_right + p_up + p_down - div = 4p`; the 3D curl with `w = 0` and no variation in z is
 * exactly the 2D scalar curl in its z component; and advection's third axis samples zero.
 *
 * The price is memory and boundary work: a 2D grid carries three z planes where one would do, so it
 * is three times the size of a solver written for two dimensions, and the two mirror planes are a
 * whole cross-section to refresh after every sweep rather than an edge. That is the cost of not
 * maintaining two solvers, and a 2D grid is small to begin with.
 *
 * The equality is exact rather than approximate, and there is a test that asserts it bit for bit. The
 * cost of keeping it that way is that the solver holds no `if (2D)` on any arithmetic path: the third
 * axis is flattened once, in nya_fluid_create and nya_fluid_emit, because a ternary inside a loop
 * lets the compiler contract the two branches differently and the two spaces then disagree in the
 * last bits of every float.
 *
 * ## Why the fields are collocated and not staggered
 *
 * Velocity lives at cell centres beside density, not on cell faces. A staggered (MAC) grid has less
 * numerical dissipation and is what an offline solver uses, but it needs three grids of different
 * sizes, a different boundary rule per component, and interpolation at every sample. Collocated
 * storage makes every field one array of the same shape, which is what lets the same loop advect
 * velocity, density and temperature. The artefact it trades for, a checkerboard mode in the
 * pressure, is invisible under the dissipation stylized smoke wants anyway.
 *
 * ## What the projection removes, and what it leaves
 *
 * Velocity, pressure and density share a cell centre, so the divergence and the pressure gradient are
 * both central differences over two cells while the pressure Laplacian the solve inverts spans one.
 * The two do not compose exactly, and what survives is the checkerboard mode the wide difference
 * cannot see. More sweeps do not touch it, which is the shape to recognise: a residual that falls
 * with the sweep count is an under-converged solve, one that does not is this.
 *
 * Measured on the 32x48x32 benchmark grid, a rising plume, confinement off: the divergence entering
 * a step is 2.09, and leaving it 2.36 at 4 sweeps, 1.39 at 8, 0.93 at 20, 0.87 at 40 and 0.89 at 80.
 * That last 1.6% of the fastest speed in the field is the floor. With confinement at 1 the floor is
 * 5.5%, because the confinement force is a cell-scale field and most of its divergence lands in
 * exactly the mode the projection is blind to.
 *
 * It does not accumulate: the mode is the highest frequency the grid holds, so advection and any
 * dissipation at all remove it within a few steps, and what reaches the screen is a fluid. The cure,
 * if it is ever needed, is a staggered MAC grid, which is the rejected alternative above and a
 * different module rather than a patch to this one.
 *
 * ## Determinism
 *
 * The solver draws no randomness, reads no clock and uses no threads, and every loop runs in index
 * order, so a volume is a pure function of its options and the calls made into it. That is what lets
 * `nya_fluid_checksum` be an oracle: the simulation harness in src/nyangine/testing steps a volume
 * from a seed and asserts the same seed gives the same checksum. Floating point is only reproducible
 * within one build, so a checksum is never compared across compilers or optimization levels.
 * */
#pragma once

#include "nyangine/base/base_arena.h"
#include "nyangine/base/base_attributes.h"
#include "nyangine/base/base_types.h"
#include "nyangine/math/math_vector.h"
#include "nyangine/renderer/render_color.h"

typedef struct NYA_Window NYA_Window;

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * CONSTANTS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/**
 * Volumes that may exist at once. A scene has a handful of effects, not a table of them: the 3D demo
 * runs one column of smoke and the 2D game one, and eight leaves room for a scene that wants a fire
 * per torch before the ceiling is the thing that tells you to merge them into one grid.
 * */
#ifndef NYA_FLUID_VOLUMES_MAX
#define NYA_FLUID_VOLUMES_MAX 8
#endif

/**
 * The most cells an edge may have. Past 256 a single-threaded Gauss-Seidel solve is no longer a
 * frame's worth of work at any iteration count worth running, so the bound is where the algorithm
 * stops being the right one rather than where the memory runs out.
 * */
#ifndef NYA_FLUID_DIMENSION_MAX
#define NYA_FLUID_DIMENSION_MAX 256
#endif

/**
 * The most cells one volume may hold, borders included. 4 Mi cells is 212 MiB across the thirteen
 * fields, which is already past what this engine budgets for everything else put together; the bound
 * exists so a typo in a dimension fails at `nya_fluid_create` instead of at the allocator.
 * */
#ifndef NYA_FLUID_CELLS_MAX
#define NYA_FLUID_CELLS_MAX (4ULL * 1024ULL * 1024ULL)
#endif

/**
 * Gauss-Seidel sweeps in the pressure projection, when NYA_FluidOptions.pressure_iterations is zero.
 *
 * The solve is what makes the velocity field divergence free, and it is the whole cost centre: each
 * sweep touches every cell once, and the step runs two projections. Measured on the 32x48x32 demo
 * volume, 4 sweeps leaves visible compression (smoke piles up against nothing and the column stalls),
 * 8 still drifts, 20 is where another sweep stops changing the picture, and 40 costs twice as much
 * for a difference nobody can see. Stam's 1999 paper picks the same number for the same reason.
 *
 * Raise it for a grid much larger than 64 on an edge, where information has further to travel per
 * sweep; lower it for a volume that is decoration rather than motion.
 * */
#ifndef NYA_FLUID_PRESSURE_ITERATIONS
#define NYA_FLUID_PRESSURE_ITERATIONS 20
#endif

/**
 * The most sweeps the solve will ever run, whatever it is asked for. Bounded so a config typo is a
 * loud assertion rather than a frame that never ends.
 * */
#ifndef NYA_FLUID_PRESSURE_ITERATIONS_MAX
#define NYA_FLUID_PRESSURE_ITERATIONS_MAX 128
#endif

/**
 * The largest timestep one call to nya_fluid_step integrates, in seconds. Semi-Lagrangian advection
 * is stable at any step, so this is not a stability limit: it stops a stalled frame or a debugger
 * breakpoint from teleporting the whole field in one step, which looks like the effect popped.
 * */
#ifndef NYA_FLUID_STEP_SECONDS_MAX
#define NYA_FLUID_STEP_SECONDS_MAX 0.1F
#endif

/** World units per cell when NYA_FluidOptions.cell_size is zero. */
#define NYA_FLUID_CELL_SIZE 1.0F

/**
 * The ceiling every field is clamped to after a step. Sources add without asking what is already
 * there, so an emitter left running would otherwise reach infinity and every later sample would be
 * NaN. The value itself is arbitrary and only has to be far above what a visible effect uses, which
 * is about 4.
 * */
#define NYA_FLUID_FIELD_MAX 1000.0F

/** Buoyancy, vorticity, dissipation and cooling when their option fields are zero. */
#define NYA_FLUID_BUOYANCY    0.0F
#define NYA_FLUID_VORTICITY   0.0F
#define NYA_FLUID_DISSIPATION 0.0F
#define NYA_FLUID_COOLING     0.0F

/** How much density a cell needs before it is drawn at all, when the render option is zero. */
#define NYA_FLUID_DRAW_THRESHOLD 0.02F

/** How opaque the densest cell is drawn, when NYA_FluidRenderOptions.opacity is zero. */
#define NYA_FLUID_DRAW_OPACITY 0.85F

/** The density that draws at full opacity, when NYA_FluidRenderOptions.density_full is zero. */
#define NYA_FLUID_DRAW_DENSITY_FULL 1.0F

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * TYPES
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

typedef enum NYA_FluidSpace           NYA_FluidSpace;
typedef struct NYA_FluidOptions       NYA_FluidOptions;
typedef struct NYA_FluidEmitter       NYA_FluidEmitter;
typedef struct NYA_FluidRenderOptions NYA_FluidRenderOptions;
typedef struct NYA_Fluid              NYA_Fluid;

/** A wind field the volume may drift on. Defined in render_wind.h; only ever held here by pointer. */
typedef struct NYA_WindField          NYA_WindField;

/** Which renderer a volume draws through, and how its grid is laid out in the world. */
enum NYA_FluidSpace {
    /**
     * A grid in the xy plane, drawn through render2d in world pixels. Depth is forced to one and the
     * default up axis is negative y, because 2D world space has y growing down the screen.
     * */
    NYA_FLUID_SPACE_2D = 0,

    /** A box in world space, drawn through render3d as additive camera-facing splats. */
    NYA_FLUID_SPACE_3D,

    NYA_FLUID_SPACE_COUNT,
};

/**
 * What a volume is and how it behaves. Everything has a usable default, so
 * `{ .width = 64, .height = 64 }` is a working 2D volume.
 *
 * `space`, the three dimensions and `cell_size` are read once at nya_fluid_create and fixed for the
 * volume's life, since they decide the allocation. Everything else may change between steps.
 * */
struct NYA_FluidOptions {
    NYA_FluidSpace space;

    /**
     * Interior cells per axis. A border cell on each side is added on top and is not addressable.
     * Zero is read as one. `depth` is forced to one for a 2D volume, whatever is passed.
     * */
    u32 width;
    u32 height;
    u32 depth;

    /** World units per cell. Zero is NYA_FLUID_CELL_SIZE. */
    f32 cell_size;

    /** Where the grid's minimum corner sits in the world. For a 2D volume, z is ignored. */
    f32x3 origin;

    /**
     * Which way buoyancy lifts hot fluid. Zero is read as `{ 0, 1, 0 }` in 3D and `{ 0, -1, 0 }` in
     * 2D, which is up the screen.
     * */
    f32x3 up;

    /**
     * Upward acceleration per unit of temperature above `ambient_temperature`, in world units per
     * second squared. This is the term that makes smoke rise and fire climb.
     * */
    f32 buoyancy;

    /** Downward acceleration per unit of density, so heavy smoke settles. Usually well under buoyancy. */
    f32 weight;

    /**
     * Vorticity confinement strength: how hard the swirl the grid is about to dissipate is pushed
     * back in. Zero is off and costs nothing.
     *
     * The force it adds per step is `vorticity * cell_size * delta_time_s * |curl|`, and |curl| is
     * itself velocity per cell, so it grows with how fast the fluid is already moving. Around 1 gives
     * a plume visible curl at the emitter speeds the demos use; at 6 the force is the size of the
     * emitter's own and the field boils. It also costs accuracy: see below.
     * */
    f32 vorticity;

    /** Velocity diffusion, the fluid's thickness. Zero skips the diffusion solve entirely. */
    f32 viscosity;

    /** Density and temperature diffusion. Zero skips that solve too. */
    f32 diffusion;

    /** Fraction of density lost per second, so a puff fades instead of accumulating forever. */
    f32 dissipation;

    /** Fraction of the gap to `ambient_temperature` closed per second. This is what makes fire fall off. */
    f32 cooling;

    /** What temperature the fluid is pulled back to, and what buoyancy measures against. */
    f32 ambient_temperature;

    /** Constant acceleration on the whole field, in world units per second squared. */
    f32x3 gravity;

    /** Gauss-Seidel sweeps per projection. Zero is NYA_FLUID_PRESSURE_ITERATIONS. */
    u32 pressure_iterations;
};

/**
 * A ball of fluid pushed into the grid. The amounts are absolute, not rates: a source that runs at a
 * rate multiplies by its own timestep, exactly as `nya_particles_emit` takes a count rather than a
 * count per second.
 * */
struct NYA_FluidEmitter {
    /** World position of the ball's centre. Outside the grid, it contributes nothing. */
    f32x3 position;

    /** World units. Zero is read as one cell, which is the smallest thing worth emitting into. */
    f32 radius;

    /** Added to density at the centre, falling off smoothly to zero at `radius`. */
    f32 density;

    /** Added to temperature the same way. Buoyancy reads the result. */
    f32 temperature;

    /** Added to velocity the same way, in world units per second. */
    f32x3 velocity;
};

/**
 * What a window does with fluid volumes. Off until a window says otherwise, exactly like the post
 * chain's scene features: a volume that is never drawn still steps, and a window that never asks
 * pays nothing.
 *
 * These live on the window rather than on the volume because they are a look decision belonging to
 * the surface being drawn into, and because one volume may be drawn into two windows with different
 * settings. What the volume *is* stays in NYA_FluidOptions.
 * */
struct NYA_FluidRenderOptions {
    /** Nothing is drawn until this is true. See nya_fluid_draw. */
    b8 enabled;

    /** How opaque the densest cell is. Zero is NYA_FLUID_DRAW_OPACITY. */
    f32 opacity;

    /** Density below which a cell is skipped. Zero is NYA_FLUID_DRAW_THRESHOLD. */
    f32 threshold;

    /** The density that reaches `opacity`. Zero is NYA_FLUID_DRAW_DENSITY_FULL. */
    f32 density_full;

    /** The colour of cold fluid. A zeroed colour is read as white. */
    NYA_Color cool;

    /**
     * The colour the hottest fluid reaches, mixed in by temperature. A zeroed colour leaves the
     * volume one flat shade, which is what plain smoke wants.
     * */
    NYA_Color hot;

    /** Temperature at which `hot` fully replaces `cool`. Zero is read as one. */
    f32 hot_temperature;

    /**
     * Draw every nth cell per axis. Zero and one both mean every cell. A 3D volume costs one splat
     * per drawn cell, so a stride of two is eight times fewer splats and the obvious first knob when
     * a large volume will not fit the 3D batch.
     * */
    u32 stride;
};

/**
 * One volume. Transparent, like every struct here: an overlay reads the counters and a test reads
 * the fields. The field arrays are the solver's own and are swapped between steps, so hold indices
 * rather than pointers into them across a `nya_fluid_step`.
 * */
struct NYA_Fluid {
    NYA_Arena* allocator;

    NYA_FluidOptions options;

    /** Interior cells per axis, as resolved from the options. `depth` is one for a 2D volume. */
    u32 width;
    u32 height;
    u32 depth;

    /** Cells per axis including the two border cells, so `width + 2` and so on. */
    u32 stride_x;
    u32 stride_y;
    u32 stride_z;

    /** stride_x * stride_y * stride_z. Every field array is this long. */
    u32 cell_count;

    /** Velocity per axis at cell centres, in world units per second. */
    f32* velocity_x;
    f32* velocity_y;
    f32* velocity_z;

    /** The previous step's velocity, and the solver's scratch between stages. */
    f32* velocity_x_previous;
    f32* velocity_y_previous;
    f32* velocity_z_previous;

    f32* density;
    f32* density_previous;

    f32* temperature;
    f32* temperature_previous;

    /** The projection's working set: the pressure it solves for and the divergence it solves against. */
    f32* pressure;
    f32* divergence;

    /** |curl| per cell, which vorticity confinement takes the gradient of. */
    f32* curl_magnitude;

    /** One byte per cell, non-zero where the fluid may not go. See nya_fluid_obstacle_box_set. */
    u8* obstacle;

    /** How many cells are currently solid. Zero means the obstacle pass is skipped entirely. */
    u32 obstacle_count;

    /** Steps taken since creation, and how long the last one took. */
    u64 step_count;
    f32 step_time_s;

    /**
     * An optional wind field the volume drifts on, borrowed from the caller, and how strongly.
     *
     * Null is the default and means no wind — the step integrates exactly as before. Set it and each step adds a
     * wind push to the velocity field (sampled at the volume's origin), so smoke and dust from a plume lean on the
     * same air that moves the foliage, water and particles. `wind_time_s` is this volume's own field clock. The
     * push is uniform across the grid — a stylized breeze, not a per-cell wind sampling; see nya_fluid_wind_set.
     * */
    const NYA_WindField* wind;
    f32                  wind_influence;
    f32                  wind_time_s;
};

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * LIFETIME
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/**
 * Allocates a volume at the size in `options` and registers it in the live table. Every field array
 * is allocated here and never again, so a step allocates nothing.
 *
 * Crashes rather than returning null when the options are impossible: a dimension past
 * NYA_FLUID_DIMENSION_MAX, a cell count past NYA_FLUID_CELLS_MAX, or a ninth live volume. Those are
 * all programmer error, since the sizes are written in the caller's own source.
 * */
NYA_API NYA_Fluid* nya_fluid_create(NYA_Arena* arena, NYA_FluidOptions options) __attr_no_discard;

/** Removes the volume from the live table. Arena owned, so nothing is freed. Null is a no-op. */
NYA_API void nya_fluid_destroy(NYA_Fluid* fluid);

/** Zeroes every field, keeping the obstacles and the options. For a level change. */
NYA_API void nya_fluid_clear(NYA_Fluid* fluid);

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * FUNCTIONS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/**
 * Replaces the solver's knobs. `space`, the dimensions and `cell_size` are ignored, since they are
 * the allocation and cannot change; everything else takes effect at the next step.
 * */
NYA_API void             nya_fluid_options_set(NYA_Fluid* fluid, NYA_FluidOptions options);
NYA_API NYA_FluidOptions nya_fluid_options(const NYA_Fluid* fluid) __attr_no_discard;

/**
 * Integrates one step: forces, vorticity confinement, velocity diffusion, projection, advection,
 * projection again, then the scalar fields. `delta_time_s` is clamped to NYA_FLUID_STEP_SECONDS_MAX
 * and a non-positive step does nothing.
 * */
NYA_API void nya_fluid_step(NYA_Fluid* fluid, f32 delta_time_s);

/** Pushes density, heat and velocity into a ball of the grid. Outside the grid it does nothing. */
NYA_API void nya_fluid_emit(NYA_Fluid* fluid, NYA_FluidEmitter emitter);

/*
 * ─────────────────────────────────────────────────────────
 * OBSTACLES
 * ─────────────────────────────────────────────────────────
 */

/**
 * Marks every cell whose centre falls inside the world-space box as solid: fluid neither enters it
 * nor flows through it, and its velocity is held at zero.
 *
 * This is how a physics body becomes an obstacle, and it is the cheap half of that on purpose: a
 * caller already holds the body's world position and the extents it attached the shape with, so
 * marking it is a triple loop over the cells it covers and no backend query at all. Voxelising an
 * arbitrary collider instead means a point query per cell, which for a 32x48x32 grid is 49 thousand
 * queries a frame.
 *
 * Rejected: a `nya_fluid_obstacle_entity_set` taking an NYA_Entity and reading its `scale`. In this
 * engine an entity's scale is not its collider's size, since a body is attached with its own extents
 * and most callers leave the transform's scale at one, so the helper would have been silently wrong
 * more often than right. The caller passes the size it actually used.
 * */
NYA_API void nya_fluid_obstacle_box_set(NYA_Fluid* fluid, f32x3 min, f32x3 max);

/** Unmarks the same box. The partner of nya_fluid_obstacle_box_set. */
NYA_API void nya_fluid_obstacle_box_clear(NYA_Fluid* fluid, f32x3 min, f32x3 max);

/** Clears every obstacle in one pass, for a body that moved or a level that changed. */
NYA_API void nya_fluid_obstacles_clear(NYA_Fluid* fluid);

/**
 * Makes the volume drift on `field`, at `influence` (world units per second of push per step, roughly). A null
 * `field` turns it off, which is the default and the exact old behaviour. The field is borrowed — the caller owns
 * it — so the volume, the particles and the foliage can all read one shared wind. The push is uniform (sampled at
 * the volume's origin), a stylized breeze; a per-cell wind sampling is a follow-up.
 * */
NYA_API void nya_fluid_wind_set(NYA_Fluid* fluid, const NYA_WindField* field, f32 influence);

/*
 * ─────────────────────────────────────────────────────────
 * SAMPLING
 * ─────────────────────────────────────────────────────────
 */

/**
 * The fields at a world point, interpolated between the surrounding cells. Outside the grid they
 * read zero, which is what a point outside a puff of smoke actually holds.
 * */
NYA_API f32   nya_fluid_density_at(const NYA_Fluid* fluid, f32x3 position) __attr_no_discard;
NYA_API f32   nya_fluid_temperature_at(const NYA_Fluid* fluid, f32x3 position) __attr_no_discard;
NYA_API f32x3 nya_fluid_velocity_at(const NYA_Fluid* fluid, f32x3 position) __attr_no_discard;

/**
 * The index of the cell a world point falls in, or false when the point is outside the interior.
 * Everything a caller can index with, in one place, so nothing reimplements the mapping.
 * */
NYA_API b8 nya_fluid_cell_index(const NYA_Fluid* fluid, f32x3 position, OUT u32* out_index) __attr_no_discard;

/*
 * ─────────────────────────────────────────────────────────
 * INTROSPECTION
 * ─────────────────────────────────────────────────────────
 */

/**
 * A hash of every field, so a replay test can assert two runs agree without comparing megabytes.
 * Only meaningful within one build: see "Determinism" above.
 * */
NYA_API u64 nya_fluid_checksum(const NYA_Fluid* fluid) __attr_no_discard;

/** Cells including borders, bytes held, and how long the last step took. */
NYA_API u32 nya_fluid_cell_count(const NYA_Fluid* fluid) __attr_no_discard;
NYA_API u64 nya_fluid_memory_bytes(const NYA_Fluid* fluid) __attr_no_discard;
NYA_API f32 nya_fluid_step_time_s(const NYA_Fluid* fluid) __attr_no_discard;

/** The live volumes, in creation order. Zero of them is the case that costs nothing. */
NYA_API u32        nya_fluid_count(void) __attr_no_discard;
NYA_API NYA_Fluid* nya_fluid_at(u32 index) __attr_no_discard;

/*
 * ─────────────────────────────────────────────────────────
 * DRAWING
 * ─────────────────────────────────────────────────────────
 */

/**
 * What this window draws fluid as. Zeroed is off, which is the state a window starts in, so fluids
 * cost a window nothing until it opts in.
 * */
NYA_API void                   nya_fluid_render_options_set(NYA_Window* window, NYA_FluidRenderOptions options);
NYA_API NYA_FluidRenderOptions nya_fluid_render_options(const NYA_Window* window) __attr_no_discard;

/**
 * Draws the volume: a 2D one as one bilinearly shaded quad per cell through render2d, a 3D one as
 * additive camera-facing splats through render3d, which the post chain then composites and bloom
 * picks up for free.
 *
 * Returns immediately when the window has fluids off, when the volume is empty, or when a 3D volume
 * is drawn outside nya_render3d_begin.
 *
 * Rejected: raymarching the volume in a shader. It is the better picture, and it needs a 3D texture
 * uploaded every frame, a new pipeline, a depth-aware composite and a shader per backend, none of
 * which this engine has yet. Splats reuse the billboard path the fire effect already goes through
 * and read correctly under a flat stylized look. Revisit when the renderer grows compute passes.
 * */
NYA_API void nya_fluid_draw(NYA_Window* window, const NYA_Fluid* fluid);
