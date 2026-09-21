/**
 * The Navier-Stokes step: grid size against time per step and bytes held, which is the pair of numbers
 * TODO.md's budget section records for every renderer change.
 *
 * Each row is one whole step at the default twenty pressure sweeps: forces, vorticity confinement,
 * two projections, four advections and the scalar pass. The grids are the ones a scene would actually
 * use, since the cost is linear in cells and a number taken on a toy grid predicts nothing.
 **/

#include "nyangine/nyangine.c"
#include "nyangine/nyangine.h"

/** Steps taken before measuring, so the field is full of fluid rather than empty. */
#define WARMUP_STEPS 30

/** The engine's own tick, which is the step a game would hand it. */
#define STEP_S (1.0F / 60.0F)

/** One grid to measure, and what it is meant to represent. */
typedef struct Case Case;

struct Case {
    NYA_ConstCString name;
    NYA_FluidSpace   space;
    u32              width;
    u32              height;
    u32              depth;
};

/**
 * The largest |divergence| left, ignoring a margin of cells at every wall. The margin is the point:
 * a reflected boundary and a collocated grid cannot both be satisfied in the cell next to the wall,
 * so the number with the margin at zero says what the boundary costs and the number with it at three
 * says what the solver actually achieved.
 * */
static f32 worst_divergence_beyond(const NYA_Fluid* fluid, u32 margin) {
    u32 step_x = 1;
    u32 step_y = fluid->stride_x;
    u32 step_z = fluid->stride_x * fluid->stride_y;

    f32 worst = 0.0F;

    u32 low_z  = fluid->depth > (2 * margin) ? 1 + margin : 1;
    u32 high_z = fluid->depth > (2 * margin) ? fluid->depth - margin : fluid->depth;

    for (u32 k = low_z; k <= high_z; k++) {
        for (u32 j = 1 + margin; j + margin <= fluid->height; j++) {
            for (u32 i = 1 + margin; i + margin <= fluid->width; i++) {
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

/** Everywhere, walls included. */
static f32 worst_divergence(const NYA_Fluid* fluid) { return worst_divergence_beyond(fluid, 0); }

/** The fastest anything is moving, so the residual above can be read as a share of it. */
static f32 worst_speed(const NYA_Fluid* fluid) {
    f32 worst = 0.0F;

    for (u32 index = 0; index < fluid->cell_count; index++) {
        f32 speed = fabsf(fluid->velocity_x[index]) + fabsf(fluid->velocity_y[index]) + fabsf(fluid->velocity_z[index]);
        worst     = nya_max(worst, speed);
    }

    return worst;
}

s32 main(void) {
    const Case cases[] = {
        { "2D 48x32", NYA_FLUID_SPACE_2D, 48, 32, 1 },
        { "2D 64x48", NYA_FLUID_SPACE_2D, 64, 48, 1 },
        { "2D 128x96", NYA_FLUID_SPACE_2D, 128, 96, 1 },
        { "2D 192x144", NYA_FLUID_SPACE_2D, 192, 144, 1 },
        { "3D 12x18x12", NYA_FLUID_SPACE_3D, 12, 18, 12 },
        { "3D 16x24x16", NYA_FLUID_SPACE_3D, 16, 24, 16 },
        { "3D 32x48x32", NYA_FLUID_SPACE_3D, 32, 48, 32 },
        { "3D 48x48x48", NYA_FLUID_SPACE_3D, 48, 48, 48 },
        { "3D 64x64x64", NYA_FLUID_SPACE_3D, 64, 64, 64 },
    };

    for (u32 c = 0; c < nya_carray_length(cases); c++) {
        const Case* test = &cases[c];

        NYA_Arena* arena = nya_arena_create(.name = "bench_fluid");
        defer      nya_arena_destroy(arena);

        NYA_Fluid* fluid = nya_fluid_create(arena, (NYA_FluidOptions){
                                                       .space       = test->space,
                                                       .width       = test->width,
                                                       .height      = test->height,
                                                       .depth       = test->depth,
                                                       .cell_size   = 1.0F,
                                                       .buoyancy    = 4.0F,
                                                       .vorticity   = 1.0F,
                                                       .dissipation = 0.2F,
                                                       .cooling     = 0.5F,
                                                   });

        f32x3 source = { (f32)test->width * 0.5F, 4.0F, (f32)test->depth * 0.5F };

        NYA_FluidEmitter emitter = {
            .position    = source,
            .radius      = 4.0F,
            .density     = 0.5F,
            .temperature = 2.0F,
            .velocity    = { 0.0F, 6.0F, 0.0F },
        };

        for (u32 step = 0; step < WARMUP_STEPS; step++) {
            nya_fluid_emit(fluid, emitter);
            nya_fluid_step(fluid, STEP_S);
        }

        char group[96];
        (void)snprintf(group, sizeof(group), "fluid step, %s, %u pressure sweeps", test->name, NYA_FLUID_PRESSURE_ITERATIONS);
        nya_bench_begin(group);

        // one operation is one whole step, so the reported per-operation time is milliseconds per step.
        nya_bench("step", 1, {
            nya_fluid_emit(fluid, emitter);
            nya_fluid_step(fluid, STEP_S);
            nya_bench_keep(fluid->density[fluid->cell_count / 2]);
        });

        if (nya_bench_end() != 0) return 1;

        f32 residual = worst_divergence(fluid);
        f32 speed    = worst_speed(fluid);

        printf("    %s: %u cells, %.2f MiB held, %.2f MiB resident, worst divergence %.4f of speed %.2f (%.1f%%)\n", test->name,
               nya_fluid_cell_count(fluid), (f64)nya_fluid_memory_bytes(fluid) / (1024.0 * 1024.0),
               (f64)nya_arena_resident_bytes(arena) / (1024.0 * 1024.0), (f64)residual, (f64)speed,
               speed > 0.0F ? (f64)(residual / speed) * 100.0 : 0.0);

        nya_fluid_destroy(fluid);
    }

    /*
     * What the pressure iteration count buys. The residual is the divergence the projection failed to
     * remove, as a share of the fastest thing in the field, which is what decides whether the smoke
     * swirls or piles up against nothing. This is the measurement NYA_FLUID_PRESSURE_ITERATIONS is
     * chosen from, so it lives beside the step cost rather than in a comment nobody can re-run.
     */
    {
        const u32 sweeps[] = { 4, 8, 20, 40, 80 };

        NYA_Arena* arena = nya_arena_create(.name = "bench_fluid_sweeps");
        defer      nya_arena_destroy(arena);

        nya_bench_begin("fluid step, 3D 32x48x32, by pressure sweep count");

        for (u32 s = 0; s < nya_carray_length(sweeps); s++) {
            // confinement off here on purpose: its force is a cell-scale field whose divergence the
            // projection cannot see (see render_fluid.h), and leaving it on hides what the sweeps do.
            NYA_Fluid* fluid = nya_fluid_create(arena, (NYA_FluidOptions){
                                                           .space               = NYA_FLUID_SPACE_3D,
                                                           .width               = 32,
                                                           .height              = 48,
                                                           .depth               = 32,
                                                           .cell_size           = 1.0F,
                                                           .buoyancy            = 4.0F,
                                                           .dissipation         = 0.2F,
                                                           .cooling             = 0.5F,
                                                           .pressure_iterations = sweeps[s],
                                                       });

            NYA_FluidEmitter emitter = {
                .position    = { 16.0F, 4.0F, 16.0F },
                .radius      = 4.0F,
                .density     = 0.5F,
                .temperature = 2.0F,
                .velocity    = { 0.0F, 6.0F, 0.0F },
            };

            for (u32 step = 0; step < WARMUP_STEPS; step++) {
                nya_fluid_emit(fluid, emitter);
                nya_fluid_step(fluid, STEP_S);
            }

            char name[32];
            (void)snprintf(name, sizeof(name), "%u sweeps", sweeps[s]);

            nya_bench(name, 1, {
                nya_fluid_emit(fluid, emitter);
                nya_fluid_step(fluid, STEP_S);
                nya_bench_keep(fluid->density[fluid->cell_count / 2]);
            });

            // what the projection was handed against what it left behind, on the same field.
            nya_fluid_emit(fluid, emitter);

            f32 before = worst_divergence(fluid);

            nya_fluid_step(fluid, STEP_S);

            f32 residual = worst_divergence(fluid);
            f32 speed    = worst_speed(fluid);

            printf("    %u sweeps: divergence %.4f into the step, %.4f out (%.4f away from the walls), speed %.2f\n", sweeps[s],
                   (f64)before, (f64)residual, (f64)worst_divergence_beyond(fluid, 3), (f64)speed);

            nya_fluid_destroy(fluid);
        }

        if (nya_bench_end() != 0) return 1;
    }

    return 0;
}
