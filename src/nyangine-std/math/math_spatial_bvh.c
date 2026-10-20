/**
 * @file math_spatial_bvh.c
 *
 * The BVH: a binary tree of boxes over a permutation of the items, built top down. Each split sorts the centroids
 * into NYA_BVH_BINS buckets along the widest axis and takes the bucket boundary with the lowest surface area cost,
 * which costs one pass over the items per level where a full sweep sorts them.
 * */
#include "nyangine-core/nyangine.h"

/*
 * -----------------------------------------------------------------------------------------------------------------
 * INTERNAL
 * -----------------------------------------------------------------------------------------------------------------
 */

/** A traversal pops one node and pushes two, and a node below NYA_BVH_DEPTH_MAX is a leaf. */
#define _NYA_BVH_STACK_MAX (NYA_BVH_DEPTH_MAX + 2)

/**
 * The most items a leaf keeps when splitting would not pay by the cost model. Past it the build splits anyway,
 * so a cluster of identical boxes cannot make one leaf every query scans.
 * */
#define _NYA_BVH_LEAF_SIZE_FORCED (NYA_BVH_LEAF_SIZE * 4)

/**
 * What the probability of a query reaching a node is weighed by. Surface area in 3D; in 2D a flat box has none, and
 * a random line meets a convex shape in proportion to its perimeter, so half of that.
 * */
NYA_INTERNAL f32 _nya_bvh_cost_area(const NYA_Bvh3D* bvh, NYA_Box3f box) {
    if (nya_box3_is_empty(box)) return 0.0F;
    if (bvh->dimensions == 3) return nya_box3_surface_area(box);

    return (box.max.x - box.min.x) + (box.max.y - box.min.y);
}

typedef struct {
    NYA_Box3f box;
    u32       count;
} _NYA_BvhBin;

/** The bin a centroid falls in. NaN goes to the first bin rather than through an undefined cast. */
NYA_INTERNAL u32 _nya_bvh_bin(f32 centroid, f32 minimum, f32 scale) {
    f32 slot = (centroid - minimum) * scale;

    if (!(slot >= 0.0F)) return 0;
    if (slot >= (f32)(NYA_BVH_BINS - 1)) return NYA_BVH_BINS - 1;

    return (u32)slot;
}

/** The split boundary with the lowest cost, as the first bin of the right side. Zero when no split puts items on both sides. */
NYA_INTERNAL u32 _nya_bvh_split_find(const NYA_Bvh3D* bvh, const _NYA_BvhBin* bins, OUT f32* out_cost) {
    f32 right_area[NYA_BVH_BINS];
    u32 right_count[NYA_BVH_BINS];

    NYA_Box3f accumulated = NYA_BOX3F_EMPTY;
    u32       counted     = 0;

    for (u32 i = NYA_BVH_BINS - 1; i > 0; i--) {
        accumulated    = nya_box3_union(accumulated, bins[i].box);
        counted       += bins[i].count;
        right_area[i]  = _nya_bvh_cost_area(bvh, accumulated);
        right_count[i] = counted;
    }

    u32 best      = 0;
    f32 best_cost = F32_MAX;

    accumulated = NYA_BOX3F_EMPTY;
    counted     = 0;

    for (u32 i = 1; i < NYA_BVH_BINS; i++) {
        accumulated  = nya_box3_union(accumulated, bins[i - 1].box);
        counted     += bins[i - 1].count;

        if (counted == 0 || right_count[i] == 0) continue;

        f32 cost = (_nya_bvh_cost_area(bvh, accumulated) * (f32)counted) + (right_area[i] * (f32)right_count[i]);
        if (cost >= best_cost) continue;

        best      = i;
        best_cost = cost;
    }

    *out_cost = best_cost;

    return best;
}

/**
 * Where to split the items of `node`, writing the boundary into `order`: the count that goes left, or zero for a leaf.
 * */
NYA_INTERNAL u32 _nya_bvh_partition(NYA_Bvh3D* bvh, const NYA_BvhNode* node) {
    const u32 first = node->first;
    const u32 count = node->count;

    NYA_Box3f centroids = NYA_BOX3F_EMPTY;
    for (u32 i = first; i < first + count; i++) {
        f32x3 centroid = bvh->centroids[bvh->order[i]];
        centroids      = nya_box3_union(centroids, (NYA_Box3f){ .min = centroid, .max = centroid });
    }

    u32 axis = 0;
    for (u32 a = 1; a < bvh->dimensions; a++) {
        if (centroids.max[a] - centroids.min[a] > centroids.max[axis] - centroids.min[axis]) axis = a;
    }

    f32 extent = centroids.max[axis] - centroids.min[axis];

    // every centroid in one place: no plane separates them, so halve the run to keep leaves small.
    if (!(extent > 0.0F)) return count > _NYA_BVH_LEAF_SIZE_FORCED ? count / 2 : 0;

    _NYA_BvhBin bins[NYA_BVH_BINS];
    for (u32 i = 0; i < NYA_BVH_BINS; i++) bins[i] = (_NYA_BvhBin){ .box = NYA_BOX3F_EMPTY };

    f32 scale = (f32)NYA_BVH_BINS / extent;
    for (u32 i = first; i < first + count; i++) {
        u32 id  = bvh->order[i];
        u32 bin = _nya_bvh_bin(bvh->centroids[id][axis], centroids.min[axis], scale);

        bins[bin].box = nya_box3_union(bins[bin].box, bvh->boxes[id]);
        bins[bin].count++;
    }

    f32 split_cost = 0.0F;
    u32 split      = _nya_bvh_split_find(bvh, bins, &split_cost);

    if (split == 0) return count > _NYA_BVH_LEAF_SIZE_FORCED ? count / 2 : 0;

    // the costs are relative to the node's own area: a leaf tests every item, a split pays one more node test.
    f32 leaf_cost = _nya_bvh_cost_area(bvh, node->box) * (f32)count;
    f32 node_cost = _nya_bvh_cost_area(bvh, node->box) + split_cost;

    if (node_cost >= leaf_cost && count <= _NYA_BVH_LEAF_SIZE_FORCED) return 0;

    u32 low  = first;
    u32 high = first + count;

    while (low < high) {
        u32 id = bvh->order[low];

        if (_nya_bvh_bin(bvh->centroids[id][axis], centroids.min[axis], scale) < split) {
            low++;
            continue;
        }

        high--;
        bvh->order[low]  = bvh->order[high];
        bvh->order[high] = id;
    }

    u32 left = low - first;
    nya_assert(left > 0 && left < count, "a chosen split puts items on both sides, got %u of %u", left, count);

    return left;
}

NYA_INTERNAL NYA_Box3f _nya_bvh_leaf_box(const NYA_Bvh3D* bvh, const NYA_BvhNode* node) {
    NYA_Box3f box = NYA_BOX3F_EMPTY;
    for (u32 i = node->first; i < node->first + node->count; i++) box = nya_box3_union(box, bvh->boxes[bvh->order[i]]);

    return box;
}

NYA_INTERNAL void _nya_bvh_build(NYA_Bvh3D* bvh, u32 count) {
    nya_assert(bvh != nullptr);

    bvh->count      = count;
    bvh->node_count = 0;

    if (count == 0) return;

    for (u32 i = 0; i < count; i++) {
        bvh->order[i]     = i;
        bvh->centroids[i] = nya_box3_center(bvh->boxes[i]);
    }

    typedef struct {
        u32 node;
        u32 depth;
    } Pending;

    Pending pending[_NYA_BVH_STACK_MAX];
    u32     pending_count = 0;

    bvh->nodes[bvh->node_count++] = (NYA_BvhNode){ .first = 0, .count = count };
    pending[pending_count++]      = (Pending){ .node = 0, .depth = 0 };

    while (pending_count > 0) {
        Pending      at   = pending[--pending_count];
        NYA_BvhNode* node = &bvh->nodes[at.node];

        node->box = _nya_bvh_leaf_box(bvh, node);

        if (node->count <= NYA_BVH_LEAF_SIZE || at.depth + 1 >= NYA_BVH_DEPTH_MAX) continue;

        u32 left_count = _nya_bvh_partition(bvh, node);
        if (left_count == 0) continue;

        nya_assert(bvh->node_count + 2 <= 2 * bvh->capacity, "a binary tree over n items has under 2n nodes");

        u32 left = bvh->node_count;
        bvh->node_count += 2;

        bvh->nodes[left]     = (NYA_BvhNode){ .first = node->first, .count = left_count };
        bvh->nodes[left + 1] = (NYA_BvhNode){ .first = node->first + left_count, .count = node->count - left_count };

        node->first = left;
        node->count = 0;

        nya_assert(pending_count + 2 <= _NYA_BVH_STACK_MAX);
        pending[pending_count++] = (Pending){ .node = left, .depth = at.depth + 1 };
        pending[pending_count++] = (Pending){ .node = left + 1, .depth = at.depth + 1 };
    }
}

NYA_INTERNAL void _nya_bvh_refit(NYA_Bvh3D* bvh) {
    // children are always allocated after their parent, so one backward pass sees every child before its parent.
    for (u32 n = bvh->node_count; n > 0; n--) {
        NYA_BvhNode* node = &bvh->nodes[n - 1];

        if (node->count > 0) {
            node->box = _nya_bvh_leaf_box(bvh, node);
            continue;
        }

        nya_assert(node->first > n - 1, "a child comes after its parent");
        node->box = nya_box3_union(bvh->nodes[node->first].box, bvh->nodes[node->first + 1].box);
    }
}

NYA_INTERNAL u32 _nya_bvh_query(const NYA_Bvh3D* bvh, const _NYA_SpatialQuery* query, OUT u32* out_ids, u32 capacity) {
    nya_assert(bvh != nullptr);
    nya_assert(out_ids != nullptr || capacity == 0);

    if (bvh->node_count == 0) return 0;

    u32 stack[_NYA_BVH_STACK_MAX];
    u32 depth = 0;
    u32 total = 0;

    stack[depth++] = 0;

    while (depth > 0) {
        const NYA_BvhNode* node = &bvh->nodes[stack[--depth]];

        if (!_nya_spatial_query_accepts(query, node->box)) continue;

        if (node->count > 0) {
            for (u32 i = node->first; i < node->first + node->count; i++) {
                u32 id = bvh->order[i];
                if (_nya_spatial_query_accepts(query, bvh->boxes[id])) _nya_spatial_emit(out_ids, capacity, &total, id);
            }

            continue;
        }

        nya_assert(depth + 2 <= _NYA_BVH_STACK_MAX);
        stack[depth++] = node->first + 1;
        stack[depth++] = node->first;
    }

    return total;
}

typedef struct {
    u32 node;
    f32 key;
} _NYA_BvhVisit;

/** Pushes the two children of an inner node so the one with the smaller key is popped first; a missed child is not pushed. */
NYA_INTERNAL void _nya_bvh_push_ordered(_NYA_BvhVisit* stack, u32* depth, _NYA_BvhVisit a, b8 a_hit, _NYA_BvhVisit b, b8 b_hit) {
    nya_assert(*depth + 2 <= _NYA_BVH_STACK_MAX);

    if (a_hit && b_hit && a.key < b.key) {
        stack[(*depth)++] = b;
        stack[(*depth)++] = a;
        return;
    }

    if (a_hit) stack[(*depth)++] = a;
    if (b_hit) stack[(*depth)++] = b;
}

NYA_INTERNAL b8 _nya_bvh_raycast(const NYA_Bvh3D* bvh, NYA_Ray3f ray, f32 max_distance, OUT NYA_SpatialHit* out_hit) {
    nya_assert(bvh != nullptr && out_hit != nullptr);

    NYA_SpatialHit best  = { 0 };
    b8             found = false;

    f32 root = 0.0F;
    if (bvh->node_count == 0 || !nya_ray3_intersect_box(ray, bvh->nodes[0].box, max_distance, &root)) return false;

    _NYA_BvhVisit stack[_NYA_BVH_STACK_MAX];
    u32           depth = 0;

    stack[depth++] = (_NYA_BvhVisit){ .node = 0, .key = root };

    while (depth > 0) {
        _NYA_BvhVisit visit = stack[--depth];
        if (found && visit.key > best.distance) continue;

        const NYA_BvhNode* node  = &bvh->nodes[visit.node];
        f32                limit = found ? best.distance : max_distance;

        if (node->count > 0) {
            for (u32 i = node->first; i < node->first + node->count; i++) {
                u32 id       = bvh->order[i];
                f32 distance = 0.0F;

                if (nya_ray3_intersect_box(ray, bvh->boxes[id], found ? best.distance : max_distance, &distance)) {
                    _nya_spatial_closest_offer(&best, &found, (NYA_SpatialHit){ .id = id, .distance = distance });
                }
            }

            continue;
        }

        _NYA_BvhVisit left  = { .node = node->first };
        _NYA_BvhVisit right = { .node = node->first + 1 };

        b8 left_hit  = nya_ray3_intersect_box(ray, bvh->nodes[left.node].box, limit, &left.key);
        b8 right_hit = nya_ray3_intersect_box(ray, bvh->nodes[right.node].box, limit, &right.key);

        _nya_bvh_push_ordered(stack, &depth, left, left_hit, right, right_hit);
    }

    *out_hit = best;
    return found;
}

NYA_INTERNAL u32 _nya_bvh_nearest(const NYA_Bvh3D* bvh, f32x3 point, u32 k, OUT NYA_SpatialHit* out_hits) {
    nya_assert(bvh != nullptr);
    nya_assert(out_hits != nullptr || k == 0);

    if (k == 0 || bvh->node_count == 0) return 0;

    _NYA_BvhVisit stack[_NYA_BVH_STACK_MAX];
    u32           depth = 0;
    u32           count = 0;

    stack[depth++] = (_NYA_BvhVisit){ .node = 0, .key = nya_box3_distance_squared(bvh->nodes[0].box, point) };

    while (depth > 0) {
        _NYA_BvhVisit visit = stack[--depth];
        if (visit.key > _nya_spatial_heap_bound(out_hits, count, k)) continue;

        const NYA_BvhNode* node = &bvh->nodes[visit.node];

        if (node->count > 0) {
            for (u32 i = node->first; i < node->first + node->count; i++) {
                u32 id = bvh->order[i];
                _nya_spatial_heap_offer(out_hits, &count, k, (NYA_SpatialHit){ .id = id, .distance = nya_box3_distance_squared(bvh->boxes[id], point) });
            }

            continue;
        }

        f32           bound = _nya_spatial_heap_bound(out_hits, count, k);
        _NYA_BvhVisit left  = { .node = node->first, .key = nya_box3_distance_squared(bvh->nodes[node->first].box, point) };
        _NYA_BvhVisit right = { .node = node->first + 1, .key = nya_box3_distance_squared(bvh->nodes[node->first + 1].box, point) };

        _nya_bvh_push_ordered(stack, &depth, left, left.key <= bound, right, right.key <= bound);
    }

    _nya_spatial_heap_finish(out_hits, count);

    return count;
}

NYA_INTERNAL NYA_Error _nya_bvh_validate(NYA_Arena* arena, u32 capacity, const void* out_bvh) {
    if (arena == nullptr || out_bvh == nullptr) return nya_error(NYA_ERROR_INVALID_ARGUMENT, "no arena or no out pointer");
    if (capacity == 0 || capacity > U32_MAX / 4) return nya_error(NYA_ERROR_INVALID_ARGUMENT, "a BVH holds 1 to 2^30 items, not %u", capacity);

    return NYA_OK;
}

NYA_INTERNAL void _nya_bvh_init(NYA_Arena* arena, u32 capacity, u32 dimensions, OUT NYA_Bvh3D* bvh) {
    u64 n = capacity;

    *bvh = (NYA_Bvh3D){
        .allocator  = arena,
        .capacity   = capacity,
        .dimensions = dimensions,
        .nodes      = nya_arena_alloc(arena, 2 * n * sizeof(NYA_BvhNode)),
        .order      = nya_arena_alloc(arena, n * sizeof(u32)),
        .boxes      = nya_arena_alloc(arena, n * sizeof(NYA_Box3f)),
        .centroids  = nya_arena_alloc(arena, n * sizeof(f32x3)),
    };
}

NYA_INTERNAL void _nya_bvh_release(NYA_Bvh3D* bvh) {
    NYA_Arena* arena = bvh->allocator;
    u64        n     = bvh->capacity;

    nya_arena_free(arena, bvh->nodes, 2 * n * sizeof(NYA_BvhNode));
    nya_arena_free(arena, bvh->order, n * sizeof(u32));
    nya_arena_free(arena, bvh->boxes, n * sizeof(NYA_Box3f));
    nya_arena_free(arena, bvh->centroids, n * sizeof(f32x3));
}

/*
 * -----------------------------------------------------------------------------------------------------------------
 * BVH
 * -----------------------------------------------------------------------------------------------------------------
 */

NYA_Error nya_bvh3d_create(NYA_Arena* arena, u32 capacity, OUT NYA_Bvh3D** out_bvh) {
    NYA_TRY(_nya_bvh_validate(arena, capacity, out_bvh));

    NYA_Bvh3D* bvh = nya_arena_alloc(arena, sizeof(NYA_Bvh3D));
    _nya_bvh_init(arena, capacity, 3, bvh);

    *out_bvh = bvh;

    return NYA_OK;
}

void nya_bvh3d_destroy(NYA_Bvh3D* bvh) {
    if (bvh == nullptr) return;

    NYA_Arena* arena = bvh->allocator;

    _nya_bvh_release(bvh);
    nya_arena_free(arena, bvh, sizeof(NYA_Bvh3D));
}

void nya_bvh3d_build(NYA_Bvh3D* bvh, const NYA_Box3f* boxes, u32 count) {
    nya_assert(bvh != nullptr && (boxes != nullptr || count == 0));
    nya_assert(count <= bvh->capacity, "%u items do not fit a BVH of %u", count, bvh->capacity);

    if (count > 0) nya_memcpy(bvh->boxes, boxes, (u64)count * sizeof(NYA_Box3f));

    _nya_bvh_build(bvh, count);
}

void nya_bvh3d_refit(NYA_Bvh3D* bvh, const NYA_Box3f* boxes, u32 count) {
    nya_assert(bvh != nullptr && (boxes != nullptr || count == 0));
    nya_assert(count == bvh->count, "a refit keeps the %u items the tree was built over, not %u", bvh->count, count);

    if (count > 0) nya_memcpy(bvh->boxes, boxes, (u64)count * sizeof(NYA_Box3f));

    _nya_bvh_refit(bvh);
}

u32 nya_bvh3d_query_box(const NYA_Bvh3D* bvh, NYA_Box3f box, OUT u32* out_ids, u32 capacity) {
    _NYA_SpatialQuery query = { .kind = _NYA_SPATIAL_QUERY_BOX, .box = box };

    return _nya_bvh_query(bvh, &query, out_ids, capacity);
}

u32 nya_bvh3d_query_radius(const NYA_Bvh3D* bvh, NYA_Spheref sphere, OUT u32* out_ids, u32 capacity) {
    _NYA_SpatialQuery query = { .kind = _NYA_SPATIAL_QUERY_RADIUS, .sphere = sphere };

    return _nya_bvh_query(bvh, &query, out_ids, capacity);
}

b8 nya_bvh3d_raycast(const NYA_Bvh3D* bvh, NYA_Ray3f ray, f32 max_distance, OUT NYA_SpatialHit* out_hit) {
    return _nya_bvh_raycast(bvh, ray, max_distance, out_hit);
}

u32 nya_bvh3d_nearest(const NYA_Bvh3D* bvh, f32x3 point, u32 k, OUT NYA_SpatialHit* out_hits) {
    return _nya_bvh_nearest(bvh, point, k, out_hits);
}

NYA_Error nya_bvh2d_create(NYA_Arena* arena, u32 capacity, OUT NYA_Bvh2D** out_bvh) {
    NYA_TRY(_nya_bvh_validate(arena, capacity, out_bvh));

    NYA_Bvh2D* bvh = nya_arena_alloc(arena, sizeof(NYA_Bvh2D));
    _nya_bvh_init(arena, capacity, 2, &bvh->bvh);

    *out_bvh = bvh;

    return NYA_OK;
}

void nya_bvh2d_destroy(NYA_Bvh2D* bvh) {
    if (bvh == nullptr) return;

    NYA_Arena* arena = bvh->bvh.allocator;

    _nya_bvh_release(&bvh->bvh);
    nya_arena_free(arena, bvh, sizeof(NYA_Bvh2D));
}

void nya_bvh2d_build(NYA_Bvh2D* bvh, const NYA_Box2f* boxes, u32 count) {
    nya_assert(bvh != nullptr && (boxes != nullptr || count == 0));
    nya_assert(count <= bvh->bvh.capacity, "%u items do not fit a BVH of %u", count, bvh->bvh.capacity);

    for (u32 i = 0; i < count; i++) bvh->bvh.boxes[i] = _nya_spatial_box_from_2d(boxes[i]);

    _nya_bvh_build(&bvh->bvh, count);
}

void nya_bvh2d_refit(NYA_Bvh2D* bvh, const NYA_Box2f* boxes, u32 count) {
    nya_assert(bvh != nullptr && (boxes != nullptr || count == 0));
    nya_assert(count == bvh->bvh.count, "a refit keeps the %u items the tree was built over, not %u", bvh->bvh.count, count);

    for (u32 i = 0; i < count; i++) bvh->bvh.boxes[i] = _nya_spatial_box_from_2d(boxes[i]);

    _nya_bvh_refit(&bvh->bvh);
}

u32 nya_bvh2d_query_box(const NYA_Bvh2D* bvh, NYA_Box2f box, OUT u32* out_ids, u32 capacity) {
    return nya_bvh3d_query_box(&bvh->bvh, _nya_spatial_box_from_2d(box), out_ids, capacity);
}

u32 nya_bvh2d_query_radius(const NYA_Bvh2D* bvh, NYA_Circlef circle, OUT u32* out_ids, u32 capacity) {
    return nya_bvh3d_query_radius(&bvh->bvh, _nya_spatial_sphere_from_2d(circle), out_ids, capacity);
}

b8 nya_bvh2d_raycast(const NYA_Bvh2D* bvh, NYA_Ray2f ray, f32 max_distance, OUT NYA_SpatialHit* out_hit) {
    return nya_bvh3d_raycast(&bvh->bvh, _nya_spatial_ray_from_2d(ray), max_distance, out_hit);
}

u32 nya_bvh2d_nearest(const NYA_Bvh2D* bvh, f32x2 point, u32 k, OUT NYA_SpatialHit* out_hits) {
    return nya_bvh3d_nearest(&bvh->bvh, _nya_spatial_point_from_2d(point), k, out_hits);
}
