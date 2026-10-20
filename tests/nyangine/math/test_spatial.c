/**
 * The spatial hash, the loose trees and the BVH, each against a linear scan over the same boxes.
 *
 * Every index is an optimisation of the scan, so the law is that it answers exactly what the scan answers: the same
 * set for a box or radius query, the same item and distance for a ray, the same k distances and ids for nearest.
 * Ties break by id in both, which is what makes "exactly" checkable. The boxes mix points, small, large and huge
 * items, some outside the trees' world, since those are the cases each index handles specially.
 **/

#include "nyangine-core/nyangine.c"
#include "nyangine-core/nyangine.h"

/** Seeds per dimension count. Each runs a few hundred queries of each kind against all three indices. */
#define SEEDS 24

#define ITEMS_MAX   700
#define QUERIES     40
#define WORLD_SIZE  100.0F
#define RESULTS_MAX ITEMS_MAX

/** splitmix64: one seed in, a reproducible stream out, so a failing seed replays. */
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

static f32 rng_range(Rng* rng, f32 low, f32 high) {
    return low + ((high - low) * rng_unit(rng));
}

static u32 rng_below(Rng* rng, u32 bound) {
    return (u32)(rng_next(rng) % bound);
}

/**
 * Points, small, large and huge items, a few centred outside the world. A `uniform` world has only the first two, the
 * one a spatial hash is for: a single huge item widens every hash query until the scan is cheaper than the cells.
 * */
static NYA_Box3f random_box(Rng* rng, u32 dimensions, b8 uniform) {
    f32   roll   = rng_unit(rng);
    f32   size   = roll < 0.35F ? 0.0F : (roll < 0.8F || uniform) ? 2.0F : roll < 0.95F ? 10.0F : 60.0F;
    b8    away   = rng_unit(rng) < 0.05F;
    f32x3 center = { 0 };
    f32x3 half   = { 0 };

    for (u32 axis = 0; axis < dimensions; axis++) {
        center[axis] = away ? rng_range(rng, -50.0F, WORLD_SIZE + 50.0F) : rng_range(rng, 0.0F, WORLD_SIZE);
        half[axis]   = rng_range(rng, 0.0F, size);
    }

    return nya_box3_from_center(center, half);
}

static f32x3 random_point(Rng* rng, u32 dimensions, f32 margin) {
    f32x3 point = { 0 };
    for (u32 axis = 0; axis < dimensions; axis++) point[axis] = rng_range(rng, -margin, WORLD_SIZE + margin);

    return point;
}

static NYA_Ray3f random_ray(Rng* rng, u32 dimensions) {
    NYA_Ray3f ray = { .origin = random_point(rng, dimensions, 30.0F) };

    for (u32 axis = 0; axis < dimensions; axis++) {
        // an axis that does not move is the slab test's special case, so it is common here.
        ray.direction[axis] = rng_unit(rng) < 0.25F ? 0.0F : rng_range(rng, -1.0F, 1.0F);
    }

    if (nya_vector_dot(ray.direction, ray.direction) == 0.0F) ray.direction.x = 1.0F;

    return ray;
}

/*
 * The oracle.
 */

typedef struct {
    u32       dimensions;
    b8        uniform;
    u32       count;
    NYA_Box3f boxes[ITEMS_MAX];
    b8        live[ITEMS_MAX];
} World;

static b8 hit_before(NYA_SpatialHit a, NYA_SpatialHit b) {
    return a.distance < b.distance || (a.distance == b.distance && a.id < b.id);
}

static u32 oracle_query(const World* world, b8 radius, NYA_Box3f box, NYA_Spheref sphere, u32* out) {
    u32 total = 0;

    for (u32 id = 0; id < world->count; id++) {
        if (!world->live[id]) continue;

        b8 accepted = radius ? nya_sphere_overlaps_box3(sphere, world->boxes[id]) : nya_box3_overlaps(box, world->boxes[id]);
        if (accepted) out[total++] = id;
    }

    return total;
}

static b8 oracle_raycast(const World* world, NYA_Ray3f ray, f32 max_distance, NYA_SpatialHit* out) {
    b8 found = false;

    for (u32 id = 0; id < world->count; id++) {
        f32 distance = 0.0F;
        if (!world->live[id] || !nya_ray3_intersect_box(ray, world->boxes[id], max_distance, &distance)) continue;

        NYA_SpatialHit hit = { .id = id, .distance = distance };
        if (!found || hit_before(hit, *out)) *out = hit;
        found = true;
    }

    return found;
}

static u32 oracle_nearest(const World* world, f32x3 point, u32 k, NYA_SpatialHit* out) {
    u32 count = 0;

    for (u32 id = 0; id < world->count; id++) {
        if (!world->live[id]) continue;

        NYA_SpatialHit hit = { .id = id, .distance = sqrtf(nya_box3_distance_squared(world->boxes[id], point)) };

        // insertion into a sorted prefix of at most k.
        u32 at = count < k ? count++ : k;
        if (at == k && !hit_before(hit, out[k - 1])) continue;
        if (at == k) at = k - 1;

        while (at > 0 && hit_before(hit, out[at - 1])) {
            out[at] = out[at - 1];
            at--;
        }
        out[at] = hit;
    }

    return count;
}

static void sort_ids(u32* ids, u32 count) {
    for (u32 i = 1; i < count; i++) {
        u32 moving = ids[i];
        u32 at     = i;
        while (at > 0 && ids[at - 1] > moving) {
            ids[at] = ids[at - 1];
            at--;
        }
        ids[at] = moving;
    }
}

/*
 * The indices behind one interface, so every law is written once.
 */

typedef enum { INDEX_HASH, INDEX_TREE, INDEX_BVH, INDEX_COUNT } IndexKind;

static const char* INDEX_NAMES[INDEX_COUNT] = { "hash", "tree", "bvh" };

typedef struct {
    IndexKind kind;
    u32       dimensions;

    NYA_SpatialHash2D* hash2;
    NYA_SpatialHash3D* hash3;
    NYA_Quadtree*      quad;
    NYA_Octree*        oct;
    NYA_Bvh2D*         bvh2;
    NYA_Bvh3D*         bvh3;
} Index;

static NYA_Box2f flat(NYA_Box3f box) {
    return (NYA_Box2f){ .min = box.min.xy, .max = box.max.xy };
}

static u32 index_query(const Index* index, b8 radius, NYA_Box3f box, NYA_Spheref sphere, u32* out, u32 capacity) {
    b8          two    = index->dimensions == 2;
    NYA_Circlef circle = { .center = sphere.center.xy, .radius = sphere.radius };

    switch (index->kind) {
        case INDEX_HASH:
            if (two) return radius ? nya_spatial_hash2d_query_radius(index->hash2, circle, out, capacity) : nya_spatial_hash2d_query_box(index->hash2, flat(box), out, capacity);
            return radius ? nya_spatial_hash3d_query_radius(index->hash3, sphere, out, capacity) : nya_spatial_hash3d_query_box(index->hash3, box, out, capacity);
        case INDEX_TREE:
            if (two) return radius ? nya_quadtree_query_radius(index->quad, circle, out, capacity) : nya_quadtree_query_box(index->quad, flat(box), out, capacity);
            return radius ? nya_octree_query_radius(index->oct, sphere, out, capacity) : nya_octree_query_box(index->oct, box, out, capacity);
        case INDEX_BVH:
            if (two) return radius ? nya_bvh2d_query_radius(index->bvh2, circle, out, capacity) : nya_bvh2d_query_box(index->bvh2, flat(box), out, capacity);
            return radius ? nya_bvh3d_query_radius(index->bvh3, sphere, out, capacity) : nya_bvh3d_query_box(index->bvh3, box, out, capacity);
        case INDEX_COUNT:
        default:          nya_unreachable();
    }
}

static b8 index_raycast(const Index* index, NYA_Ray3f ray, f32 max_distance, NYA_SpatialHit* out) {
    NYA_Ray2f flat_ray = { .origin = ray.origin.xy, .direction = ray.direction.xy };
    b8        two      = index->dimensions == 2;

    switch (index->kind) {
        case INDEX_HASH: return two ? nya_spatial_hash2d_raycast(index->hash2, flat_ray, max_distance, out) : nya_spatial_hash3d_raycast(index->hash3, ray, max_distance, out);
        case INDEX_TREE: return two ? nya_quadtree_raycast(index->quad, flat_ray, max_distance, out) : nya_octree_raycast(index->oct, ray, max_distance, out);
        case INDEX_BVH:  return two ? nya_bvh2d_raycast(index->bvh2, flat_ray, max_distance, out) : nya_bvh3d_raycast(index->bvh3, ray, max_distance, out);
        case INDEX_COUNT:
        default:         nya_unreachable();
    }
}

static u32 index_nearest(const Index* index, f32x3 point, u32 k, NYA_SpatialHit* out) {
    b8 two = index->dimensions == 2;

    switch (index->kind) {
        case INDEX_HASH: return two ? nya_spatial_hash2d_nearest(index->hash2, point.xy, k, out) : nya_spatial_hash3d_nearest(index->hash3, point, k, out);
        case INDEX_TREE: return two ? nya_quadtree_nearest(index->quad, point.xy, k, out) : nya_octree_nearest(index->oct, point, k, out);
        case INDEX_BVH:  return two ? nya_bvh2d_nearest(index->bvh2, point.xy, k, out) : nya_bvh3d_nearest(index->bvh3, point, k, out);
        case INDEX_COUNT:
        default:         nya_unreachable();
    }
}

static void index_add(Index* index, u32 id, NYA_Box3f box) {
    b8 two = index->dimensions == 2;

    if (index->kind == INDEX_HASH) two ? nya_spatial_hash2d_add(index->hash2, id, flat(box)) : nya_spatial_hash3d_add(index->hash3, id, box);
    if (index->kind == INDEX_TREE) two ? nya_quadtree_add(index->quad, id, flat(box)) : nya_octree_add(index->oct, id, box);
}

static void index_remove(Index* index, u32 id) {
    b8 two = index->dimensions == 2;

    if (index->kind == INDEX_HASH) two ? nya_spatial_hash2d_remove(index->hash2, id) : nya_spatial_hash3d_remove(index->hash3, id);
    if (index->kind == INDEX_TREE) two ? nya_quadtree_remove(index->quad, id) : nya_octree_remove(index->oct, id);
}

static void index_move(Index* index, u32 id, NYA_Box3f box) {
    b8 two = index->dimensions == 2;

    if (index->kind == INDEX_HASH) two ? nya_spatial_hash2d_move(index->hash2, id, flat(box)) : nya_spatial_hash3d_move(index->hash3, id, box);
    if (index->kind == INDEX_TREE) two ? nya_quadtree_move(index->quad, id, flat(box)) : nya_octree_move(index->oct, id, box);
}

static void index_rebuild_bvh(Index* index, const World* world, b8 refit) {
    static NYA_Box2f flat_boxes[ITEMS_MAX];

    if (index->dimensions == 2) {
        for (u32 i = 0; i < world->count; i++) flat_boxes[i] = flat(world->boxes[i]);

        if (refit) nya_bvh2d_refit(index->bvh2, flat_boxes, world->count);
        else nya_bvh2d_build(index->bvh2, flat_boxes, world->count);
        return;
    }

    if (refit) nya_bvh3d_refit(index->bvh3, world->boxes, world->count);
    else nya_bvh3d_build(index->bvh3, world->boxes, world->count);
}

/*
 * The laws.
 */

static void check_against_oracle(const Index* index, const World* world, Rng* rng, u64 seed) {
    static u32            got[RESULTS_MAX];
    static u32            want[RESULTS_MAX];
    static NYA_SpatialHit got_hits[RESULTS_MAX];
    static NYA_SpatialHit want_hits[RESULTS_MAX];

    const char* name = INDEX_NAMES[index->kind];
    u32         dims = world->dimensions;

    for (u32 q = 0; q < QUERIES; q++) {
        // box and radius
        for (u32 radius = 0; radius < 2; radius++) {
            f32x3       center = random_point(rng, dims, 20.0F);
            f32x3       half   = { 0 };
            for (u32 axis = 0; axis < dims; axis++) half[axis] = rng_range(rng, 0.0F, rng_unit(rng) < 0.1F ? 80.0F : 12.0F);

            NYA_Box3f   box    = nya_box3_from_center(center, half);
            NYA_Spheref sphere = { .center = center, .radius = half.x };

            u32 got_count  = index_query(index, radius, box, sphere, got, RESULTS_MAX);
            u32 want_count = oracle_query(world, radius, box, sphere, want);

            nya_check(got_count == want_count, "seed %llu %s %ud %s query: %u found, the scan found %u", (unsigned long long)seed, name, dims,
                      radius ? "radius" : "box", got_count, want_count);
            if (got_count != want_count) continue;

            sort_ids(got, got_count);
            for (u32 i = 0; i < got_count; i++) {
                nya_check(got[i] == want[i], "seed %llu %s %ud query result %u is %u, the scan has %u", (unsigned long long)seed, name, dims, i, got[i], want[i]);
            }

            // a short buffer still counts every match and writes only what fits.
            if (want_count >= 2) {
                u32 short_count = index_query(index, radius, box, sphere, got, 1);
                nya_check(short_count == want_count, "seed %llu %s a short buffer reports %u matches, not %u", (unsigned long long)seed, name, short_count, want_count);
            }
        }

        // ray
        {
            NYA_Ray3f ray          = random_ray(rng, dims);
            f32       max_distance = rng_unit(rng) < 0.5F ? F32_MAX : rng_range(rng, 1.0F, 200.0F);

            NYA_SpatialHit got_hit  = { 0 };
            NYA_SpatialHit want_hit = { 0 };

            b8 got_found  = index_raycast(index, ray, max_distance, &got_hit);
            b8 want_found = oracle_raycast(world, ray, max_distance, &want_hit);

            nya_check(got_found == want_found, "seed %llu %s %ud raycast found=%d, the scan found=%d", (unsigned long long)seed, name, dims, got_found, want_found);
            if (got_found && want_found) {
                nya_check(got_hit.id == want_hit.id && got_hit.distance == want_hit.distance, "seed %llu %s %ud raycast hit %u at %f, the scan hit %u at %f",
                          (unsigned long long)seed, name, dims, got_hit.id, (f64)got_hit.distance, want_hit.id, (f64)want_hit.distance);
            }
        }

        // nearest
        {
            const u32 ks[] = { 1, 5, 17 };
            u32       k    = ks[q % nya_carray_length(ks)];
            f32x3     at   = random_point(rng, dims, rng_unit(rng) < 0.2F ? 200.0F : 10.0F);

            u32 got_count  = index_nearest(index, at, k, got_hits);
            u32 want_count = oracle_nearest(world, at, k, want_hits);

            nya_check(got_count == want_count, "seed %llu %s %ud nearest %u returned %u, the scan %u", (unsigned long long)seed, name, dims, k, got_count, want_count);
            if (got_count != want_count) continue;

            for (u32 i = 0; i < got_count; i++) {
                nya_check(got_hits[i].id == want_hits[i].id && got_hits[i].distance == want_hits[i].distance,
                          "seed %llu %s %ud nearest %u, place %u: %u at %f, the scan has %u at %f", (unsigned long long)seed, name, dims, k, i, got_hits[i].id,
                          (f64)got_hits[i].distance, want_hits[i].id, (f64)want_hits[i].distance);
            }
        }
    }
}

static void run_seed(NYA_Arena* arena, u32 dimensions, u64 seed) {
    static World world;

    Rng rng = { .state = seed };

    world = (World){ .dimensions = dimensions, .uniform = (seed / 1000) % 2 == 0, .count = 50 + rng_below(&rng, ITEMS_MAX - 50) };
    for (u32 id = 0; id < world.count; id++) {
        world.boxes[id] = random_box(&rng, dimensions, world.uniform);
        world.live[id]  = true;
    }

    NYA_Box3f bounds = { .min = f32x3_zero, .max = { WORLD_SIZE, WORLD_SIZE, dimensions == 3 ? WORLD_SIZE : 0.0F } };

    Index indices[INDEX_COUNT];
    for (u32 kind = 0; kind < INDEX_COUNT; kind++) indices[kind] = (Index){ .kind = (IndexKind)kind, .dimensions = dimensions };

    // a cell anywhere from a fifth of a typical query to twice the world's tenth, so both sides of the scan fallback are hit.
    NYA_SpatialHashOptions hash_options = { .capacity = world.count, .cell_size = rng_range(&rng, 2.0F, 20.0F) };
    u32                    depth        = 1 + rng_below(&rng, NYA_LOOSE_TREE_DEPTH_MAX);

    if (dimensions == 2) {
        NYA_EXPECT(nya_spatial_hash2d_create(arena, hash_options, &indices[INDEX_HASH].hash2));
        NYA_EXPECT(nya_quadtree_create(arena, (NYA_LooseTreeOptions2D){ .capacity = world.count, .bounds = flat(bounds), .depth_max = depth }, &indices[INDEX_TREE].quad));
        NYA_EXPECT(nya_bvh2d_create(arena, world.count, &indices[INDEX_BVH].bvh2));
    } else {
        NYA_EXPECT(nya_spatial_hash3d_create(arena, hash_options, &indices[INDEX_HASH].hash3));
        NYA_EXPECT(nya_octree_create(arena, (NYA_LooseTreeOptions3D){ .capacity = world.count, .bounds = bounds, .depth_max = depth }, &indices[INDEX_TREE].oct));
        NYA_EXPECT(nya_bvh3d_create(arena, world.count, &indices[INDEX_BVH].bvh3));
    }

    for (u32 id = 0; id < world.count; id++) {
        index_add(&indices[INDEX_HASH], id, world.boxes[id]);
        index_add(&indices[INDEX_TREE], id, world.boxes[id]);
    }
    index_rebuild_bvh(&indices[INDEX_BVH], &world, false);

    for (u32 kind = 0; kind < INDEX_COUNT; kind++) check_against_oracle(&indices[kind], &world, &rng, seed);

    // churn: moves near and far, removes, and re-adds into freed slots.
    for (u32 id = 0; id < world.count; id++) {
        f32 roll = rng_unit(&rng);

        if (roll < 0.15F) {
            world.live[id] = false;
            index_remove(&indices[INDEX_HASH], id);
            index_remove(&indices[INDEX_TREE], id);
        } else if (roll < 0.45F) {
            NYA_Box3f moved = world.boxes[id];
            f32x3     shift = { 0 };
            for (u32 axis = 0; axis < dimensions; axis++) shift[axis] = rng_range(&rng, -1.0F, 1.0F);

            world.boxes[id] = rng_unit(&rng) < 0.7F ? (NYA_Box3f){ .min = moved.min + shift, .max = moved.max + shift } : random_box(&rng, dimensions, world.uniform);
            index_move(&indices[INDEX_HASH], id, world.boxes[id]);
            index_move(&indices[INDEX_TREE], id, world.boxes[id]);
        }
    }

    for (u32 id = 0; id < world.count; id++) {
        if (world.live[id] || rng_unit(&rng) < 0.5F) continue;

        world.live[id]  = true;
        world.boxes[id] = random_box(&rng, dimensions, world.uniform);
        index_add(&indices[INDEX_HASH], id, world.boxes[id]);
        index_add(&indices[INDEX_TREE], id, world.boxes[id]);
    }

    check_against_oracle(&indices[INDEX_HASH], &world, &rng, seed);
    check_against_oracle(&indices[INDEX_TREE], &world, &rng, seed);

    // the BVH has no remove: it is refitted over every item, removed ones included, then rebuilt over the same.
    World everything = world;
    for (u32 id = 0; id < everything.count; id++) everything.live[id] = true;

    index_rebuild_bvh(&indices[INDEX_BVH], &everything, true);
    check_against_oracle(&indices[INDEX_BVH], &everything, &rng, seed);

    index_rebuild_bvh(&indices[INDEX_BVH], &everything, false);
    check_against_oracle(&indices[INDEX_BVH], &everything, &rng, seed);

    // a clear leaves an index that finds nothing and takes every id again.
    u32       ids[4];
    NYA_Box3f all = { .min = { -F32_MAX, -F32_MAX, -F32_MAX }, .max = { F32_MAX, F32_MAX, F32_MAX } };

    if (dimensions == 2) {
        nya_spatial_hash2d_clear(indices[INDEX_HASH].hash2);
        nya_quadtree_clear(indices[INDEX_TREE].quad);
        nya_bvh2d_build(indices[INDEX_BVH].bvh2, nullptr, 0);
    } else {
        nya_spatial_hash3d_clear(indices[INDEX_HASH].hash3);
        nya_octree_clear(indices[INDEX_TREE].oct);
        nya_bvh3d_build(indices[INDEX_BVH].bvh3, nullptr, 0);
    }

    for (u32 kind = 0; kind < INDEX_COUNT; kind++) {
        u32 left = index_query(&indices[kind], false, all, (NYA_Spheref){ 0 }, ids, 4);
        nya_check(left == 0, "seed %llu %s still finds %u items after a clear", (unsigned long long)seed, INDEX_NAMES[kind], left);
    }

    index_add(&indices[INDEX_HASH], 0, world.boxes[0]);
    index_add(&indices[INDEX_TREE], 0, world.boxes[0]);

    if (dimensions == 2) {
        nya_spatial_hash2d_destroy(indices[INDEX_HASH].hash2);
        nya_quadtree_destroy(indices[INDEX_TREE].quad);
        nya_bvh2d_destroy(indices[INDEX_BVH].bvh2);
    } else {
        nya_spatial_hash3d_destroy(indices[INDEX_HASH].hash3);
        nya_octree_destroy(indices[INDEX_TREE].oct);
        nya_bvh3d_destroy(indices[INDEX_BVH].bvh3);
    }
}

s32 main(void) {
    nya_backtrace_init();
    defer nya_backtrace_deinit();

    // TEST: the shapes the indices are written against
    {
        NYA_Box3f a = { .min = { 0, 0, 0 }, .max = { 1, 1, 1 } };
        NYA_Box3f b = { .min = { 1, 1, 1 }, .max = { 2, 2, 2 } };

        nya_check(nya_box3_overlaps(a, b), "closed boxes touching at a corner overlap");
        nya_check(nya_box3_contains(a, (f32x3){ 1, 1, 1 }), "a closed box contains its max corner");
        nya_check(nya_box3_is_empty(NYA_BOX3F_EMPTY), "the empty box is empty");
        nya_check(nya_box3_surface_area(a) == 6.0F, "a unit cube has six faces of one");
        nya_check(nya_box3_distance_squared(a, (f32x3){ 3, 1, 1 }) == 4.0F, "two units off a face is four squared");

        f32 distance = -1.0F;
        nya_check(nya_ray3_intersect_box((NYA_Ray3f){ .origin = { -1, 0.5F, 0.5F }, .direction = { 1, 0, 0 } }, a, F32_MAX, &distance) && distance == 1.0F,
                  "a ray along x enters the cube one unit out, got %f", (f64)distance);
        nya_check(!nya_ray3_intersect_box((NYA_Ray3f){ .origin = { -1, 2, 0.5F }, .direction = { 1, 0, 0 } }, a, F32_MAX, &distance),
                  "a ray parallel to a slab and outside it misses");
        nya_check(!nya_ray3_intersect_box((NYA_Ray3f){ .origin = { -1, 0.5F, 0.5F }, .direction = { 1, 0, 0 } }, a, 0.5F, &distance),
                  "a box past the ray's length is missed");
        nya_check(nya_ray3_intersect_box((NYA_Ray3f){ .origin = { 0.5F, 0.5F, 0.5F }, .direction = { 0, 0, 0 } }, a, F32_MAX, &distance) && distance == 0.0F,
                  "a ray starting inside hits at zero");

        NYA_Spheref sphere = { .center = { 0, 0, 0 }, .radius = 1.0F };
        nya_check(nya_ray3_intersect_sphere((NYA_Ray3f){ .origin = { -3, 0, 0 }, .direction = { 2, 0, 0 } }, sphere, F32_MAX, &distance) && distance == 1.0F,
                  "a ray at twice unit speed reaches the sphere after one length, got %f", (f64)distance);
        nya_check(!nya_ray3_intersect_sphere((NYA_Ray3f){ .origin = { -3, 0, 0 }, .direction = { -1, 0, 0 } }, sphere, F32_MAX, &distance),
                  "a ray leaving the sphere behind it misses");
        nya_check(nya_sphere_overlaps_box3(sphere, b) == false && nya_sphere_overlaps_box3(sphere, a), "the unit sphere reaches the near cube only");

        NYA_Box2f flat_box = nya_box2_from_rect((NYA_Rectf){ 1, 2, 0, 0 });
        nya_check(nya_box2_contains(flat_box, (f32x2){ 1, 2 }), "a zero size rectangle is a point box, not nothing");

        NYA_Box2f square = nya_box2_from_center((f32x2){ 0, 0 }, (f32x2){ 1, 1 });
        NYA_Box2f joined = nya_box2_union(square, flat_box);
        nya_check(nya_box2_overlaps(square, nya_box2_from_center((f32x2){ 2, 0 }, (f32x2){ 1, 1 })), "squares sharing an edge overlap");
        nya_check(joined.min.x == -1.0F && joined.max.y == 2.0F, "a union spans both");
        nya_check(nya_box2_distance_squared(square, (f32x2){ 4, 5 }) == 25.0F, "three and four off the corner is five");

        nya_check(nya_ray2_intersect_box((NYA_Ray2f){ .origin = { -3, 0 }, .direction = { 1, 0 } }, square, F32_MAX, &distance) && distance == 2.0F,
                  "a 2D ray enters two units out, got %f", (f64)distance);
        nya_check(nya_sphere_contains(sphere, (f32x3){ 1, 0, 0 }) && !nya_sphere_contains(sphere, (f32x3){ 1, 1, 0 }), "the surface is inside, beyond it is not");
    }

    NYA_Arena* arena = nya_arena_create(.name = "test_spatial");
    defer      nya_arena_destroy(arena);

    // TEST: every index answers what the linear scan answers
    for (u32 dimensions = 2; dimensions <= 3; dimensions++) {
        printf("TEST: %uD spatial indices match the linear scan over %u seeds\n", dimensions, SEEDS);

        for (u64 seed = 1; seed <= SEEDS; seed++) {
            run_seed(arena, dimensions, (seed * 1000) + dimensions);
            nya_arena_free_all(arena);
        }
    }

    // TEST: bad options are refused rather than asserted
    {
        NYA_SpatialHash3D* hash = nullptr;
        nya_check(!nya_spatial_hash3d_create(arena, (NYA_SpatialHashOptions){ .capacity = 0, .cell_size = 1.0F }, &hash).ok, "a zero capacity is refused");
        nya_check(!nya_spatial_hash3d_create(arena, (NYA_SpatialHashOptions){ .capacity = 4, .cell_size = 0.0F }, &hash).ok, "a zero cell is refused");

        NYA_Octree* tree = nullptr;
        nya_check(!nya_octree_create(arena, (NYA_LooseTreeOptions3D){ .capacity = 4, .bounds = NYA_BOX3F_EMPTY }, &tree).ok, "empty bounds are refused");
        nya_check(!nya_octree_create(arena, (NYA_LooseTreeOptions3D){ .capacity = 4, .bounds = { .max = { 1, 1, 1 } }, .depth_max = 99 }, &tree).ok,
                  "a depth past the maximum is refused");
    }

    return nya_check_failures() == 0 ? 0 : 1;
}
