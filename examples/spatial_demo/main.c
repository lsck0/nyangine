/**
 * @file examples/spatial_demo/main.c
 *
 * A scout crosses a walled field by Jump Point Search and, at every step, asks the spatial indices what is around:
 * the units within sight from a loose quadtree, the three nearest from a 2D BVH, and whether the next stretch of path
 * is blocked by a crate from a spatial hash. A 3D part does the same over a volume of drones with the octree and the
 * 3D BVH. It prints what it found and a map of the walk, and exits.
 *
 * ```
 * ./build run example spatial_demo
 * ```
 *
 * Every answer is checked against the slow way before it is printed, so a wrong index fails the run rather than
 * printing a plausible map.
 * */
#include "nyangine-core/nyangine.h"

#include "nyangine-core/nyangine.c"

/* THE FIELD */

#define FIELD_WIDTH  72
#define FIELD_HEIGHT 24

/** Wall density, in percent of cells, plus three long walls with a gap each so the path has to turn. */
#define FIELD_RUBBLE_PERCENT 18

#define UNITS          1500
#define CRATES         200
#define SIGHT_CELLS    4.0F
#define NEAREST_UNITS  3U
#define DRONES         2000
#define DRONE_VOLUME_M 200.0F

typedef struct {
    u64 state;
} Rng;

__attr_no_sanitize("unsigned-integer-overflow") NYA_INTERNAL u64 rng_next(Rng* rng) {
    u64 z = (rng->state += 0x9E3779B97F4A7C15ULL);
    z     = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z     = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;

    return z ^ (z >> 31);
}

NYA_INTERNAL f32 rng_unit(Rng* rng) {
    return (f32)(rng_next(rng) >> 40) / (f32)(1ULL << 24);
}

NYA_INTERNAL NYA_NavGrid* field_create(NYA_Arena* arena, Rng* rng) {
    NYA_NavGrid* grid = nullptr;
    NYA_EXPECT(nya_nav_grid_create(arena, FIELD_WIDTH, FIELD_HEIGHT, &grid));

    for (s32 y = 0; y < FIELD_HEIGHT; y++) {
        for (s32 x = 0; x < FIELD_WIDTH; x++) {
            if (rng_next(rng) % 100 < FIELD_RUBBLE_PERCENT) nya_nav_cost_set(grid, x, y, NYA_NAV_BLOCKED);
        }
    }

    for (s32 wall = 1; wall <= 3; wall++) {
        s32 x   = (FIELD_WIDTH * wall) / 4;
        s32 gap = (s32)(rng_next(rng) % (FIELD_HEIGHT - 4)) + 2;

        for (s32 y = 0; y < FIELD_HEIGHT; y++) nya_nav_cost_set(grid, x, y, (y == gap || y == gap + 1) ? NYA_NAV_COST_DEFAULT : NYA_NAV_BLOCKED);
    }

    return grid;
}

NYA_INTERNAL u32 path_cost(const NYA_NavPoint* path, u32 length) {
    u32 cost = 0;
    for (u32 i = 1; i < length; i++) cost += (path[i].x != path[i - 1].x && path[i].y != path[i - 1].y) ? _NYA_NAV_STEP_DIAGONAL : _NYA_NAV_STEP_ORTHOGONAL;

    return cost;
}

/* THE WALK */

typedef struct {
    u32 steps;
    u32 units_seen;
    u32 crates_ahead;
    f32 nearest_total;
} Walk;

/** One step of the walk: what the scout sees from `at`, heading for `next`. Every index answer is checked by a scan. */
NYA_INTERNAL void walk_step(const NYA_Quadtree* units, const NYA_Bvh2D* units_bvh, const NYA_Box2f* unit_boxes, const NYA_SpatialHash2D* crates,
                            const NYA_Box2f* crate_boxes, f32x2 at, f32x2 next, b8* seen, Walk* walk) {
    u32 found[UNITS];

    NYA_Circlef sight = { .center = at, .radius = SIGHT_CELLS };
    u32         count = nya_quadtree_query_radius(units, sight, found, UNITS);

    u32 scanned = 0;
    for (u32 i = 0; i < UNITS; i++) scanned += nya_box2_distance_squared(unit_boxes[i], at) <= SIGHT_CELLS * SIGHT_CELLS;
    nya_assert_eq(count, scanned);

    for (u32 i = 0; i < count; i++) {
        if (!seen[found[i]]) walk->units_seen++;
        seen[found[i]] = true;
    }

    NYA_SpatialHit nearest[NEAREST_UNITS];
    u32            near_count = nya_bvh2d_nearest(units_bvh, at, NEAREST_UNITS, nearest);
    nya_assert_eq(near_count, NEAREST_UNITS);
    walk->nearest_total += nearest[0].distance;

    NYA_Ray2f      ahead = { .origin = at, .direction = next - at };
    NYA_SpatialHit crate = { 0 };

    b8 blocked = nya_spatial_hash2d_raycast(crates, ahead, 1.0F, &crate);

    b8 scan_blocked = false;
    for (u32 i = 0; i < CRATES && !scan_blocked; i++) {
        f32 distance = 0.0F;
        scan_blocked = nya_ray2_intersect_box(ahead, crate_boxes[i], 1.0F, &distance);
    }
    nya_assert_eq(blocked, scan_blocked);

    walk->crates_ahead += blocked;
    walk->steps++;
}

NYA_INTERNAL void map_print(const NYA_NavGrid* grid, const NYA_NavPoint* path, u32 length, const NYA_Box2f* crate_boxes) {
    char rows[FIELD_HEIGHT][FIELD_WIDTH + 1];

    for (s32 y = 0; y < FIELD_HEIGHT; y++) {
        for (s32 x = 0; x < FIELD_WIDTH; x++) rows[y][x] = nya_nav_walkable(grid, x, y) ? ' ' : '#';
        rows[y][FIELD_WIDTH] = '\0';
    }

    for (u32 i = 0; i < CRATES; i++) {
        f32x2 center = (crate_boxes[i].min + crate_boxes[i].max) * 0.5F;
        s32   x      = (s32)center.x;
        s32   y      = (s32)center.y;

        if (x >= 0 && x < FIELD_WIDTH && y >= 0 && y < FIELD_HEIGHT && rows[y][x] == ' ') rows[y][x] = 'c';
    }

    for (u32 i = 0; i < length; i++) rows[path[i].y][path[i].x] = '*';

    rows[path[0].y][path[0].x]                   = 'S';
    rows[path[length - 1].y][path[length - 1].x] = 'G';

    for (s32 y = 0; y < FIELD_HEIGHT; y++) nya_log_info("|%s|", rows[y]);
}

NYA_INTERNAL void walk_field(NYA_Arena* arena) {
    Rng rng = { .state = 2026 };

    NYA_NavGrid* grid = field_create(arena, &rng);

    NYA_NavPoint from = { 0, 0 };
    NYA_NavPoint to   = { FIELD_WIDTH - 1, FIELD_HEIGHT - 1 };

    nya_nav_cost_set(grid, from.x, from.y, NYA_NAV_COST_DEFAULT);
    nya_nav_cost_set(grid, to.x, to.y, NYA_NAV_COST_DEFAULT);

    NYA_NavPoint path[FIELD_WIDTH * FIELD_HEIGHT];
    NYA_NavPoint check[FIELD_WIDTH * FIELD_HEIGHT];

    u64 jps_start = nya_clock_get_monotonic_ns();
    u32 length    = nya_nav_path_jps(grid, from, to, path, nya_carray_length(path), (NYA_NavOptions){ .diagonal = true });
    u64 jps_ns    = nya_clock_get_monotonic_ns() - jps_start;

    u64 astar_start = nya_clock_get_monotonic_ns();
    u32 astar       = nya_nav_path(grid, from, to, check, nya_carray_length(check), (NYA_NavOptions){ .diagonal = true });
    u64 astar_ns    = nya_clock_get_monotonic_ns() - astar_start;

    nya_assert(length > 0 && astar > 0, "the field always has a way through its gaps");
    nya_assert_eq(path_cost(path, length), path_cost(check, astar));

    nya_log_info("JPS: %u steps, cost %u, %llu us; A*: cost %u, %llu us", length - 1, path_cost(path, length), (unsigned long long)(jps_ns / 1000),
                 path_cost(check, astar), (unsigned long long)(astar_ns / 1000));

    // units and crates scattered over the open cells.
    NYA_Box2f unit_boxes[UNITS];
    NYA_Box2f crate_boxes[CRATES];

    for (u32 i = 0; i < UNITS; i++) {
        f32x2 center  = { rng_unit(&rng) * FIELD_WIDTH, rng_unit(&rng) * FIELD_HEIGHT };
        unit_boxes[i] = nya_box2_from_center(center, (f32x2){ 0.2F, 0.2F });
    }
    for (u32 i = 0; i < CRATES; i++) {
        f32x2 center   = { rng_unit(&rng) * FIELD_WIDTH, rng_unit(&rng) * FIELD_HEIGHT };
        crate_boxes[i] = nya_box2_from_center(center, (f32x2){ 0.4F, 0.4F });
    }

    NYA_Quadtree* units = nullptr;
    NYA_EXPECT(nya_quadtree_create(arena, (NYA_LooseTreeOptions2D){ .capacity = UNITS, .bounds = { .max = { FIELD_WIDTH, FIELD_HEIGHT } } }, &units));
    for (u32 i = 0; i < UNITS; i++) nya_quadtree_add(units, i, unit_boxes[i]);

    NYA_Bvh2D* units_bvh = nullptr;
    NYA_EXPECT(nya_bvh2d_create(arena, UNITS, &units_bvh));
    nya_bvh2d_build(units_bvh, unit_boxes, UNITS);

    NYA_SpatialHash2D* crates = nullptr;
    NYA_EXPECT(nya_spatial_hash2d_create(arena, (NYA_SpatialHashOptions){ .capacity = CRATES, .cell_size = 2.0F }, &crates));
    nya_spatial_hash2d_build(crates, crate_boxes, CRATES);

    b8   seen[UNITS] = { 0 };
    Walk walk        = { 0 };

    for (u32 i = 0; i + 1 < length; i++) {
        f32x2 at   = { (f32)path[i].x + 0.5F, (f32)path[i].y + 0.5F };
        f32x2 next = { (f32)path[i + 1].x + 0.5F, (f32)path[i + 1].y + 0.5F };

        walk_step(units, units_bvh, unit_boxes, crates, crate_boxes, at, next, seen, &walk);

        // half the units drift a little each step, which is what the quadtree's move is for.
        for (u32 u = 0; u < UNITS; u += 2) {
            f32x2 drift   = { (rng_unit(&rng) - 0.5F) * 0.2F, (rng_unit(&rng) - 0.5F) * 0.2F };
            unit_boxes[u] = (NYA_Box2f){ .min = unit_boxes[u].min + drift, .max = unit_boxes[u].max + drift };
            nya_quadtree_move(units, u, unit_boxes[u]);
        }
        nya_bvh2d_refit(units_bvh, unit_boxes, UNITS);
    }

    nya_log_info("walk: %u steps, %u of %u units sighted within %.0f cells, mean nearest %.2f cells, %u steps with a crate ahead", walk.steps, walk.units_seen,
                 UNITS, (f64)SIGHT_CELLS, (f64)(walk.nearest_total / (f32)walk.steps), walk.crates_ahead);

    map_print(grid, path, length, crate_boxes);

    nya_spatial_hash2d_destroy(crates);
    nya_bvh2d_destroy(units_bvh);
    nya_quadtree_destroy(units);
}

/* THE VOLUME */

/** A camera in a cloud of drones: what its view ray hits first and which drones are nearest, through two indices. */
NYA_INTERNAL void scan_volume(NYA_Arena* arena) {
    Rng rng = { .state = 77 };

    NYA_Box3f* boxes = nya_arena_alloc(arena, DRONES * sizeof(NYA_Box3f));
    for (u32 i = 0; i < DRONES; i++) {
        f32x3 center = (f32x3){ rng_unit(&rng), rng_unit(&rng), rng_unit(&rng) } * DRONE_VOLUME_M;
        boxes[i]     = nya_box3_from_center(center, (f32x3){ 2.0F, 2.0F, 1.0F });
    }

    NYA_Octree* tree = nullptr;
    NYA_EXPECT(nya_octree_create(arena, (NYA_LooseTreeOptions3D){ .capacity = DRONES, .bounds = { .max = { DRONE_VOLUME_M, DRONE_VOLUME_M, DRONE_VOLUME_M } } },
                                 &tree));
    for (u32 i = 0; i < DRONES; i++) nya_octree_add(tree, i, boxes[i]);

    NYA_Bvh3D* bvh = nullptr;
    NYA_EXPECT(nya_bvh3d_create(arena, DRONES, &bvh));
    nya_bvh3d_build(bvh, boxes, DRONES);

    NYA_SpatialHash3D* hash = nullptr;
    NYA_EXPECT(nya_spatial_hash3d_create(arena, (NYA_SpatialHashOptions){ .capacity = DRONES, .cell_size = 10.0F }, &hash));
    nya_spatial_hash3d_build(hash, boxes, DRONES);

    // aimed at drone 0, so the ray hits it or whatever stands in front of it.
    NYA_Ray3f view = { .origin = { 0.0F, 0.0F, 0.0F }, .direction = nya_vector_normalize(nya_box3_center(boxes[0])) };

    NYA_SpatialHit by_tree = { 0 };
    NYA_SpatialHit by_bvh  = { 0 };
    NYA_SpatialHit by_hash = { 0 };

    b8 tree_hit = nya_octree_raycast(tree, view, F32_MAX, &by_tree);
    b8 bvh_hit  = nya_bvh3d_raycast(bvh, view, F32_MAX, &by_bvh);
    b8 hash_hit = nya_spatial_hash3d_raycast(hash, view, F32_MAX, &by_hash);

    nya_assert(tree_hit == bvh_hit && bvh_hit == hash_hit && by_tree.id == by_bvh.id && by_bvh.id == by_hash.id, "three indices, one first hit");

    f32x3          middle = (f32x3){ 0.5F, 0.5F, 0.5F } * DRONE_VOLUME_M;
    NYA_SpatialHit tree_near[4];
    NYA_SpatialHit bvh_near[4];

    u32 tree_count = nya_octree_nearest(tree, middle, 4, tree_near);
    u32 bvh_count  = nya_bvh3d_nearest(bvh, middle, 4, bvh_near);

    nya_assert_eq(tree_count, bvh_count);
    for (u32 i = 0; i < tree_count; i++) nya_assert_eq(tree_near[i].id, bvh_near[i].id);

    u32 around = nya_octree_query_box(tree, nya_box3_from_center(middle, (f32x3){ 20.0F, 20.0F, 20.0F }), nullptr, 0);
    u32 also   = nya_bvh3d_query_radius(bvh, (NYA_Spheref){ .center = middle, .radius = 20.0F }, nullptr, 0);

    if (tree_hit) nya_log_info("volume: the view ray meets drone %u at %.1f m", by_tree.id, (f64)by_tree.distance);
    else nya_log_info("volume: the view ray crosses the volume without meeting a drone");
    nya_log_info("volume: nearest the centre is drone %u at %.2f m; %u drones in the 40 m cube around it, %u in the 20 m sphere", tree_near[0].id,
                 (f64)tree_near[0].distance, around, also);

    nya_spatial_hash3d_destroy(hash);
    nya_bvh3d_destroy(bvh);
    nya_octree_destroy(tree);
}

s32 main(s32 argc, NYA_CString* argv) {
    nya_unused(argc, argv);
    nya_backtrace_init();
    defer nya_backtrace_deinit();

    NYA_Arena* arena = nya_arena_create(.name = "spatial_demo");
    defer      nya_arena_destroy(arena);

    walk_field(arena);
    scan_volume(arena);

    return EXIT_SUCCESS;
}
