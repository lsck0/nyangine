/**
 * Navigation: A* against the flow field, which is the design claim in 3.3 stated as a measurement.
 *
 * The claim was "A* is cheaper for one agent, a flow field is cheaper from roughly the tenth onward".
 * That is a crossover, and a crossover is exactly the kind of thing that should be measured rather than
 * asserted — so this measures a single query against a field build, and then per-agent costs at scale.
 **/

#include "nyangine/nyangine.c"
#include "nyangine/nyangine.h"

/** A grid with a few staggered walls, so a path has to route rather than run straight. */
static NYA_NavGrid* make_grid(NYA_Arena* arena, u32 size) {
    NYA_NavGrid* grid = nullptr;
    NYA_EXPECT(nya_nav_grid_create(arena, size, size, &grid));

    for (u32 w = 1; w < 6; w++) {
        s32 x    = (s32)((size * w) / 6);
        s32 from = (w % 2 == 0) ? 0 : (s32)(size / 4);
        s32 to   = (w % 2 == 0) ? (s32)((size * 3) / 4) : (s32)(size - 1);

        for (s32 y = from; y <= to; y++) nya_nav_cost_set(grid, x, y, NYA_NAV_BLOCKED);
    }

    return grid;
}

s32 main(void) {
    NYA_Arena* arena = nya_arena_create(.name = "bench_nav");
    defer      nya_arena_destroy(arena);

    const u32 sizes[] = { 32, 64, 128 };

    for (u32 s = 0; s < nya_carray_length(sizes); s++) {
        const u32 size = sizes[s];

        NYA_NavGrid* grid = make_grid(arena, size);
        NYA_NavFlow* flow = nullptr;
        NYA_EXPECT(nya_nav_flow_create(arena, grid, &flow));

        NYA_NavPoint  from = { 1, 1 };
        NYA_NavPoint  goal = { (s32)size - 2, (s32)size - 2 };
        NYA_NavPoint* path = nya_arena_alloc(arena, (u64)size * (u64)size * sizeof(NYA_NavPoint));

        char group[64];
        (void)snprintf(group, sizeof(group), "navigation, %ux%u grid", size, size);
        nya_bench_begin(group);

        nya_bench("A* one query (4-way)", 0, {
            u32 length = nya_nav_path(grid, from, goal, path, size * size, (NYA_NavOptions){ 0 });
            nya_bench_keep(length);
        });

        nya_bench("A* one query (8-way)", 0, {
            u32 length = nya_nav_path(grid, from, goal, path, size * size, (NYA_NavOptions){ .diagonal = true });
            nya_bench_keep(length);
        });

        // Paid once however many agents follow it, which is the whole argument for it.
        nya_bench("flow field build", 0, {
            nya_nav_flow_build(flow, goal);
            nya_bench_keep(flow->distance[0]);
        });

        // What an agent pays per frame once the field exists.
        nya_bench("flow lookup x1000", 1000, {
            NYA_NavPoint at = { 1, 1 };
            for (u32 i = 0; i < 1000; i++) at = nya_nav_flow_step(flow, at.x == goal.x && at.y == goal.y ? (NYA_NavPoint){ 1, 1 } : at);
            nya_bench_keep(at.x);
        });

        if (nya_bench_end() != 0) return 1;
    }

    return 0;
}
