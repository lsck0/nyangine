/**
 * The three spatial indices against a linear scan, at 1k, 10k and 100k items, for each of the four queries and for
 * building or updating the index. Items are small boxes at a fixed density, so a query of the same size finds about
 * the same number of items at every count and the scan's linear cost is the thing being compared against.
 **/

#include "nyangine-core/nyangine.c"
#include "nyangine-core/nyangine.h"

/** Queries per timed iteration, from one fixed list, so every case answers the same questions. */
#define QUERIES 64

/** Items per unit of volume: four to a cube of side four, about what a crowd or a debris field holds. */
#define SPACING_M 4.0F

#define ITEM_HALF_MAX_M 1.0F
#define QUERY_HALF_M    4.0F
#define NEAREST_K       8

typedef struct {
    u64 state;
} Rng;

__attr_no_sanitize("unsigned-integer-overflow") static u64 rng_next(Rng* rng) {
    u64 z = (rng->state += 0x9E3779B97F4A7C15ULL);
    z     = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z     = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;

    return z ^ (z >> 31);
}

static f32 rng_unit(Rng* rng) {
    return (f32)(rng_next(rng) >> 40) / (f32)(1ULL << 24);
}

static f32x3 rng_point(Rng* rng, f32 side) {
    return (f32x3){ rng_unit(rng) * side, rng_unit(rng) * side, rng_unit(rng) * side };
}

/*
 * The scan every index is measured against.
 */

static u32 scan_box(const NYA_Box3f* boxes, u32 count, NYA_Box3f query, u32* out, u32 capacity) {
    u32 total = 0;
    for (u32 i = 0; i < count; i++) {
        if (!nya_box3_overlaps(query, boxes[i])) continue;
        if (total < capacity) out[total] = i;
        total++;
    }
    return total;
}

static u32 scan_radius(const NYA_Box3f* boxes, u32 count, NYA_Spheref query, u32* out, u32 capacity) {
    u32 total = 0;
    for (u32 i = 0; i < count; i++) {
        if (!nya_sphere_overlaps_box3(query, boxes[i])) continue;
        if (total < capacity) out[total] = i;
        total++;
    }
    return total;
}

static f32 scan_ray(const NYA_Box3f* boxes, u32 count, NYA_Ray3f ray, f32 max_distance) {
    f32 best = max_distance;
    for (u32 i = 0; i < count; i++) {
        f32 distance = 0.0F;
        if (nya_ray3_intersect_box(ray, boxes[i], best, &distance)) best = distance;
    }
    return best;
}

/** The k nearest by insertion into a sorted prefix: what a caller without an index writes. */
static u32 scan_nearest(const NYA_Box3f* boxes, u32 count, f32x3 point, u32 k, NYA_SpatialHit* out) {
    u32 kept = 0;
    for (u32 i = 0; i < count; i++) {
        f32 distance = nya_box3_distance_squared(boxes[i], point);
        if (kept == k && distance >= out[k - 1].distance) continue;

        u32 at = kept < k ? kept++ : k - 1;
        while (at > 0 && out[at - 1].distance > distance) {
            out[at] = out[at - 1];
            at--;
        }
        out[at] = (NYA_SpatialHit){ .id = i, .distance = distance };
    }
    return kept;
}

s32 main(void) {
    NYA_Arena* arena = nya_arena_create(.name = "bench_spatial");
    defer      nya_arena_destroy(arena);

    const u32 counts[] = { 1000, 10000, 100000 };

    for (u32 c = 0; c < nya_carray_length(counts); c++) {
        const u32 count = counts[c];
        const f32 side  = cbrtf((f32)count) * SPACING_M;

        Rng rng = { .state = count };

        NYA_Box3f* boxes = nya_arena_alloc(arena, (u64)count * sizeof(NYA_Box3f));
        NYA_Box3f* moved = nya_arena_alloc(arena, (u64)count * sizeof(NYA_Box3f));

        for (u32 i = 0; i < count; i++) {
            f32x3 half = { rng_unit(&rng), rng_unit(&rng), rng_unit(&rng) };
            boxes[i]   = nya_box3_from_center(rng_point(&rng, side), half * ITEM_HALF_MAX_M);

            // a frame's worth of motion: everything shifts by up to a tenth of its size.
            f32x3 shift = (f32x3){ rng_unit(&rng), rng_unit(&rng), rng_unit(&rng) } * 0.2F - 0.1F;
            moved[i]    = (NYA_Box3f){ .min = boxes[i].min + shift, .max = boxes[i].max + shift };
        }

        NYA_Box3f   box_queries[QUERIES];
        NYA_Spheref sphere_queries[QUERIES];
        NYA_Ray3f   ray_queries[QUERIES];
        f32x3       point_queries[QUERIES];

        for (u32 q = 0; q < QUERIES; q++) {
            f32x3 center      = rng_point(&rng, side);
            box_queries[q]    = nya_box3_from_center(center, (f32x3){ QUERY_HALF_M, QUERY_HALF_M, QUERY_HALF_M });
            sphere_queries[q] = (NYA_Spheref){ .center = center, .radius = QUERY_HALF_M };
            ray_queries[q]    = (NYA_Ray3f){ .origin = rng_point(&rng, side), .direction = nya_vector_normalize(rng_point(&rng, 2.0F) - 1.0F) };
            point_queries[q]  = rng_point(&rng, side);
        }

        NYA_SpatialHash3D* hash = nullptr;
        NYA_Octree*        tree = nullptr;
        NYA_Bvh3D*         bvh  = nullptr;

        // the cell is the query's size, the hash's own advice.
        NYA_EXPECT(nya_spatial_hash3d_create(arena, (NYA_SpatialHashOptions){ .capacity = count, .cell_size = QUERY_HALF_M * 2.0F }, &hash));
        NYA_EXPECT(nya_octree_create(arena, (NYA_LooseTreeOptions3D){ .capacity = count, .bounds = { .min = f32x3_zero, .max = { side, side, side } } }, &tree));
        NYA_EXPECT(nya_bvh3d_create(arena, count, &bvh));

        for (u32 i = 0; i < count; i++) {
            nya_spatial_hash3d_add(hash, i, boxes[i]);
            nya_octree_add(tree, i, boxes[i]);
        }
        nya_bvh3d_build(bvh, boxes, count);

        u32*            found = nya_arena_alloc(arena, (u64)count * sizeof(u32));
        NYA_SpatialHit  hits[NEAREST_K];
        char            group[96];
        u32             flip = 0;

        (void)snprintf(group, sizeof(group), "%u items: build or update the whole set", count);
        nya_bench_begin(group);

        nya_bench("hash: clear and add all", count, {
            nya_spatial_hash3d_clear(hash);
            for (u32 i = 0; i < count; i++) nya_spatial_hash3d_add(hash, i, boxes[i]);
        });
        nya_bench("hash: move all", count, {
            for (u32 i = 0; i < count; i++) nya_spatial_hash3d_move(hash, i, (flip++ & 1U) != 0 ? boxes[i] : moved[i]);
        });
        nya_bench("octree: clear and add all", count, {
            nya_octree_clear(tree);
            for (u32 i = 0; i < count; i++) nya_octree_add(tree, i, boxes[i]);
        });
        nya_bench("octree: move all", count, {
            for (u32 i = 0; i < count; i++) nya_octree_move(tree, i, (flip++ & 1U) != 0 ? boxes[i] : moved[i]);
        });
        nya_bench("bvh: build", count, { nya_bvh3d_build(bvh, boxes, count); });
        nya_bench("bvh: refit", count, { nya_bvh3d_refit(bvh, (flip++ & 1U) != 0 ? boxes : moved, count); });

        if (nya_bench_end() != 0) return 1;

        // the timed builds left every index over `boxes` or `moved`; put all three back on `boxes`.
        nya_spatial_hash3d_clear(hash);
        nya_octree_clear(tree);
        for (u32 i = 0; i < count; i++) {
            nya_spatial_hash3d_add(hash, i, boxes[i]);
            nya_octree_add(tree, i, boxes[i]);
        }
        nya_bvh3d_build(bvh, boxes, count);

        (void)snprintf(group, sizeof(group), "%u items: box query x%u", count, QUERIES);
        nya_bench_begin(group);
        nya_bench("scan", QUERIES, { for (u32 q = 0; q < QUERIES; q++) nya_bench_keep(scan_box(boxes, count, box_queries[q], found, count)); });
        nya_bench("hash", QUERIES, { for (u32 q = 0; q < QUERIES; q++) nya_bench_keep(nya_spatial_hash3d_query_box(hash, box_queries[q], found, count)); });
        nya_bench("octree", QUERIES, { for (u32 q = 0; q < QUERIES; q++) nya_bench_keep(nya_octree_query_box(tree, box_queries[q], found, count)); });
        nya_bench("bvh", QUERIES, { for (u32 q = 0; q < QUERIES; q++) nya_bench_keep(nya_bvh3d_query_box(bvh, box_queries[q], found, count)); });
        if (nya_bench_end() != 0) return 1;

        (void)snprintf(group, sizeof(group), "%u items: radius query x%u", count, QUERIES);
        nya_bench_begin(group);
        nya_bench("scan", QUERIES, { for (u32 q = 0; q < QUERIES; q++) nya_bench_keep(scan_radius(boxes, count, sphere_queries[q], found, count)); });
        nya_bench("hash", QUERIES, { for (u32 q = 0; q < QUERIES; q++) nya_bench_keep(nya_spatial_hash3d_query_radius(hash, sphere_queries[q], found, count)); });
        nya_bench("octree", QUERIES, { for (u32 q = 0; q < QUERIES; q++) nya_bench_keep(nya_octree_query_radius(tree, sphere_queries[q], found, count)); });
        nya_bench("bvh", QUERIES, { for (u32 q = 0; q < QUERIES; q++) nya_bench_keep(nya_bvh3d_query_radius(bvh, sphere_queries[q], found, count)); });
        if (nya_bench_end() != 0) return 1;

        (void)snprintf(group, sizeof(group), "%u items: closest ray hit x%u", count, QUERIES);
        nya_bench_begin(group);
        NYA_SpatialHit hit = { 0 };
        nya_bench("scan", QUERIES, { for (u32 q = 0; q < QUERIES; q++) nya_bench_keep(scan_ray(boxes, count, ray_queries[q], F32_MAX)); });
        nya_bench("hash", QUERIES, { for (u32 q = 0; q < QUERIES; q++) nya_bench_keep(nya_spatial_hash3d_raycast(hash, ray_queries[q], F32_MAX, &hit)); });
        nya_bench("octree", QUERIES, { for (u32 q = 0; q < QUERIES; q++) nya_bench_keep(nya_octree_raycast(tree, ray_queries[q], F32_MAX, &hit)); });
        nya_bench("bvh", QUERIES, { for (u32 q = 0; q < QUERIES; q++) nya_bench_keep(nya_bvh3d_raycast(bvh, ray_queries[q], F32_MAX, &hit)); });
        if (nya_bench_end() != 0) return 1;

        (void)snprintf(group, sizeof(group), "%u items: %u nearest x%u", count, NEAREST_K, QUERIES);
        nya_bench_begin(group);
        nya_bench("scan", QUERIES, { for (u32 q = 0; q < QUERIES; q++) nya_bench_keep(scan_nearest(boxes, count, point_queries[q], NEAREST_K, hits)); });
        nya_bench("hash", QUERIES, { for (u32 q = 0; q < QUERIES; q++) nya_bench_keep(nya_spatial_hash3d_nearest(hash, point_queries[q], NEAREST_K, hits)); });
        nya_bench("octree", QUERIES, { for (u32 q = 0; q < QUERIES; q++) nya_bench_keep(nya_octree_nearest(tree, point_queries[q], NEAREST_K, hits)); });
        nya_bench("bvh", QUERIES, { for (u32 q = 0; q < QUERIES; q++) nya_bench_keep(nya_bvh3d_nearest(bvh, point_queries[q], NEAREST_K, hits)); });
        if (nya_bench_end() != 0) return 1;

        nya_spatial_hash3d_destroy(hash);
        nya_octree_destroy(tree);
        nya_bvh3d_destroy(bvh);
    }

    return 0;
}
