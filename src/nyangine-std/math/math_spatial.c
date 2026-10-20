/**
 * @file math_spatial.c
 *
 * What the three indices share (the 2D to 3D lift, the result buffer, the k nearest heap) and the spatial hash.
 * */
#include "nyangine-core/nyangine.h"

/*
 * -----------------------------------------------------------------------------------------------------------------
 * INTERNAL
 * -----------------------------------------------------------------------------------------------------------------
 */

/** No item, no node, no child. Never a valid index: every capacity is checked to stay below it. */
#define _NYA_SPATIAL_NONE U32_MAX

/** The cell coordinate a hash clamps to, far enough from the s32 range that a neighbourhood around it cannot wrap. */
#define _NYA_SPATIAL_HASH_CELL_LIMIT (1 << 28)

/**
 * How far a ray segment's box is widened, as a fraction of a cell, so the rounding in computing a point on the ray
 * cannot put a hit in a cell the walk did not look at. Holds while coordinates stay under about 1e4 cells.
 * */
#define _NYA_SPATIAL_HASH_RAY_SLACK (1.0F / 1024.0F)

NYA_INTERNAL NYA_Box3f _nya_spatial_box_from_2d(NYA_Box2f box) {
    return (NYA_Box3f){ .min = { box.min.x, box.min.y, 0.0F }, .max = { box.max.x, box.max.y, 0.0F } };
}

NYA_INTERNAL f32x3 _nya_spatial_point_from_2d(f32x2 point) {
    return (f32x3){ point.x, point.y, 0.0F };
}

NYA_INTERNAL NYA_Ray3f _nya_spatial_ray_from_2d(NYA_Ray2f ray) {
    return (NYA_Ray3f){ .origin = _nya_spatial_point_from_2d(ray.origin), .direction = _nya_spatial_point_from_2d(ray.direction) };
}

NYA_INTERNAL NYA_Spheref _nya_spatial_sphere_from_2d(NYA_Circlef circle) {
    return (NYA_Spheref){ .center = _nya_spatial_point_from_2d(circle.center), .radius = circle.radius };
}

/** Lanes a dimension count uses, so a 2D index never widens along z. */
NYA_INTERNAL f32x3 _nya_spatial_axis_mask(u32 dimensions) {
    nya_assert(dimensions == 2 || dimensions == 3);

    return (f32x3){ 1.0F, 1.0F, dimensions == 3 ? 1.0F : 0.0F };
}

/** Writes while there is room and counts regardless, so the caller learns how short its buffer was. */
NYA_INTERNAL void _nya_spatial_emit(u32* out_ids, u32 capacity, u32* total, u32 id) {
    if (*total < capacity) out_ids[*total] = id;
    (*total)++;
}

NYA_INTERNAL b8 _nya_spatial_hit_before(NYA_SpatialHit a, NYA_SpatialHit b) {
    return a.distance < b.distance || (a.distance == b.distance && a.id < b.id);
}

/** A closer hit replaces a farther one, ties going to the smaller id, so the answer does not depend on visit order. */
NYA_INTERNAL void _nya_spatial_closest_offer(NYA_SpatialHit* best, b8* found, NYA_SpatialHit hit) {
    if (*found && !_nya_spatial_hit_before(hit, *best)) return;

    *best  = hit;
    *found = true;
}

/*
 * The k nearest as a max-heap on (squared distance, id): the root is the worst kept, the bound a search prunes by.
 */

NYA_INTERNAL void _nya_spatial_heap_sift_down(NYA_SpatialHit* heap, u32 count, u32 at) {
    for (;;) {
        u32 left  = (2 * at) + 1;
        u32 right = left + 1;
        u32 worst = at;

        if (left < count && _nya_spatial_hit_before(heap[worst], heap[left])) worst = left;
        if (right < count && _nya_spatial_hit_before(heap[worst], heap[right])) worst = right;
        if (worst == at) return;

        NYA_SpatialHit swap = heap[at];
        heap[at]            = heap[worst];
        heap[worst]         = swap;

        at = worst;
    }
}

NYA_INTERNAL void _nya_spatial_heap_offer(NYA_SpatialHit* heap, u32* count, u32 k, NYA_SpatialHit hit) {
    nya_assert(*count <= k);

    if (*count < k) {
        u32 at      = (*count)++;
        heap[at]    = hit;

        while (at > 0) {
            u32 parent = (at - 1) / 2;
            if (!_nya_spatial_hit_before(heap[parent], heap[at])) break;

            NYA_SpatialHit swap = heap[parent];
            heap[parent]        = heap[at];
            heap[at]            = swap;

            at = parent;
        }

        return;
    }

    if (!_nya_spatial_hit_before(hit, heap[0])) return;

    heap[0] = hit;
    _nya_spatial_heap_sift_down(heap, *count, 0);
}

/** The squared distance a candidate must beat, or the largest float while the heap is not full. */
NYA_INTERNAL f32 _nya_spatial_heap_bound(const NYA_SpatialHit* heap, u32 count, u32 k) {
    return count < k ? F32_MAX : heap[0].distance;
}

/** Heap order to ascending order in place, squared distances to distances. */
NYA_INTERNAL void _nya_spatial_heap_finish(NYA_SpatialHit* heap, u32 count) {
    for (u32 end = count; end > 1; end--) {
        NYA_SpatialHit swap = heap[0];
        heap[0]             = heap[end - 1];
        heap[end - 1]       = swap;

        _nya_spatial_heap_sift_down(heap, end - 1, 0);
    }

    for (u32 i = 0; i < count; i++) heap[i].distance = sqrtf(heap[i].distance);

    for (u32 i = 1; i < count; i++) nya_assert(!_nya_spatial_hit_before(heap[i], heap[i - 1]), "nearest hits come out sorted");
}

/*
 * The spatial hash.
 */

NYA_INTERNAL s32 _nya_spatial_hash_cell(const NYA_SpatialHash3D* hash, f32 coordinate) {
    f32 scaled = floorf(coordinate * hash->cell_size_inverse);

    // written so NaN falls into the first branch: a broken position lands in some cell rather than a cast's UB.
    if (!(scaled >= (f32)-_NYA_SPATIAL_HASH_CELL_LIMIT)) return -_NYA_SPATIAL_HASH_CELL_LIMIT;
    if (scaled > (f32)_NYA_SPATIAL_HASH_CELL_LIMIT) return _NYA_SPATIAL_HASH_CELL_LIMIT;

    return (s32)scaled;
}

/** The three multipliers are xxHash32's primes; the shift folds the high bits the mask would drop into the low ones. */
__attr_no_sanitize("unsigned-integer-overflow") NYA_INTERNAL u32 _nya_spatial_hash_bucket(const NYA_SpatialHash3D* hash, s32 x, s32 y, s32 z) {
    u32 mixed = ((u32)x * 0x9E3779B1U) ^ ((u32)y * 0x85EBCA77U) ^ ((u32)z * 0xC2B2AE3DU);
    mixed ^= mixed >> 15;

    return mixed & (hash->bucket_count - 1);
}

/** Links `id` into its centre's bucket. The count and the widening bounds are the caller's. */
NYA_INTERNAL void _nya_spatial_hash_file(NYA_SpatialHash3D* hash, u32 id, NYA_Box3f box) {
    nya_assert(!nya_box3_is_empty(box), "item %u's box has min <= max on every axis", id);

    f32x3 center = nya_box3_center(box);
    s32*  cell   = &hash->cells[(u64)id * 3];

    cell[0] = _nya_spatial_hash_cell(hash, center.x);
    cell[1] = _nya_spatial_hash_cell(hash, center.y);
    cell[2] = hash->dimensions == 3 ? _nya_spatial_hash_cell(hash, center.z) : 0;

    u32 bucket = _nya_spatial_hash_bucket(hash, cell[0], cell[1], cell[2]);
    u32 head   = hash->bucket_head[bucket];

    hash->boxes[id]           = box;
    hash->present[id]         = true;
    hash->previous[id]        = _NYA_SPATIAL_NONE;
    hash->next[id]            = head;
    hash->bucket_head[bucket] = id;
    if (head != _NYA_SPATIAL_NONE) hash->previous[head] = id;
}

typedef struct {
    s32 min[3];
    s32 max[3];
} _NYA_SpatialCellRange;

NYA_INTERNAL _NYA_SpatialCellRange _nya_spatial_hash_range(const NYA_SpatialHash3D* hash, NYA_Box3f box) {
    _NYA_SpatialCellRange range = { 0 };

    for (u32 axis = 0; axis < hash->dimensions; axis++) {
        range.min[axis] = _nya_spatial_hash_cell(hash, box.min[axis]);
        range.max[axis] = _nya_spatial_hash_cell(hash, box.max[axis]);
    }

    return range;
}

/** Saturating, since three spans near the clamp limit multiply past 64 bits. */
NYA_INTERNAL u64 _nya_spatial_hash_range_cells(_NYA_SpatialCellRange range) {
    u64 cells = 1;

    for (u32 axis = 0; axis < 3; axis++) {
        u64 span = (u64)((s64)range.max[axis] - (s64)range.min[axis] + 1);

        cells = cells > U64_MAX / span ? U64_MAX : cells * span;
    }

    return cells;
}

/** The query box grown by every item's reach, since items are filed by centre. */
NYA_INTERNAL NYA_Box3f _nya_spatial_hash_widen(const NYA_SpatialHash3D* hash, NYA_Box3f box) {
    return (NYA_Box3f){ .min = box.min - hash->reach, .max = box.max + hash->reach };
}

/** Whether walking `cells` buckets would cost more than looking at every slot once. */
NYA_INTERNAL b8 _nya_spatial_hash_scan_cheaper(const NYA_SpatialHash3D* hash, u64 cells) {
    return cells > (u64)hash->count;
}

typedef enum {
    _NYA_SPATIAL_QUERY_BOX,
    _NYA_SPATIAL_QUERY_RADIUS,
    _NYA_SPATIAL_QUERY_COUNT,
} _NYA_SpatialQueryKind;

typedef struct {
    _NYA_SpatialQueryKind kind;
    NYA_Box3f             box;
    NYA_Spheref           sphere;
} _NYA_SpatialQuery;

NYA_INTERNAL b8 _nya_spatial_query_accepts(const _NYA_SpatialQuery* query, NYA_Box3f item) {
    switch (query->kind) {
        case _NYA_SPATIAL_QUERY_BOX:    return nya_box3_overlaps(query->box, item);
        case _NYA_SPATIAL_QUERY_RADIUS: return nya_sphere_overlaps_box3(query->sphere, item);
        case _NYA_SPATIAL_QUERY_COUNT:
        default:                        nya_unreachable();
    }
}

NYA_INTERNAL u32 _nya_spatial_hash_query(const NYA_SpatialHash3D* hash, const _NYA_SpatialQuery* query, OUT u32* out_ids, u32 capacity) {
    nya_assert(hash != nullptr);
    nya_assert(out_ids != nullptr || capacity == 0);

    u32 total = 0;
    if (hash->count == 0) return 0;

    _NYA_SpatialCellRange range = _nya_spatial_hash_range(hash, _nya_spatial_hash_widen(hash, query->box));

    if (_nya_spatial_hash_scan_cheaper(hash, _nya_spatial_hash_range_cells(range))) {
        for (u32 id = 0; id < hash->capacity; id++) {
            if (hash->present[id] && _nya_spatial_query_accepts(query, hash->boxes[id])) _nya_spatial_emit(out_ids, capacity, &total, id);
        }

        return total;
    }

    for (s32 z = range.min[2]; z <= range.max[2]; z++) {
        for (s32 y = range.min[1]; y <= range.max[1]; y++) {
            for (s32 x = range.min[0]; x <= range.max[0]; x++) {
                for (u32 id = hash->bucket_head[_nya_spatial_hash_bucket(hash, x, y, z)]; id != _NYA_SPATIAL_NONE; id = hash->next[id]) {
                    const s32* cell = &hash->cells[(u64)id * 3];

                    // another cell sharing the bucket; skipping it is what keeps a result from holding an item twice.
                    if (cell[0] != x || cell[1] != y || cell[2] != z) continue;

                    if (_nya_spatial_query_accepts(query, hash->boxes[id])) _nya_spatial_emit(out_ids, capacity, &total, id);
                }
            }
        }
    }

    return total;
}

/** Offers every item filed in one cell to the ray. */
NYA_INTERNAL void _nya_spatial_hash_ray_cell(const NYA_SpatialHash3D* hash, s32 x, s32 y, s32 z, NYA_Ray3f ray, f32 max_distance, NYA_SpatialHit* best,
                                             b8* found) {
    for (u32 id = hash->bucket_head[_nya_spatial_hash_bucket(hash, x, y, z)]; id != _NYA_SPATIAL_NONE; id = hash->next[id]) {
        const s32* cell = &hash->cells[(u64)id * 3];
        if (cell[0] != x || cell[1] != y || cell[2] != z) continue;

        f32 distance = 0.0F;
        if (nya_ray3_intersect_box(ray, hash->boxes[id], *found ? best->distance : max_distance, &distance)) {
            _nya_spatial_closest_offer(best, found, (NYA_SpatialHit){ .id = id, .distance = distance });
        }
    }
}

NYA_INTERNAL b8 _nya_spatial_hash_raycast(const NYA_SpatialHash3D* hash, NYA_Ray3f ray, f32 max_distance, OUT NYA_SpatialHit* out_hit) {
    nya_assert(hash != nullptr && out_hit != nullptr);

    if (hash->count == 0) return false;

    // every box is inside the extent, so the walk only has to cover where the ray is inside it.
    f32 enter = 0.0F;
    f32 leave = max_distance;
    for (u32 axis = 0; axis < 3; axis++) {
        if (!_nya_ray_slab(ray.origin[axis], ray.direction[axis], hash->extent.min[axis], hash->extent.max[axis], &enter, &leave)) return false;
    }

    NYA_SpatialHit best  = { 0 };
    b8             found = false;

    f32x3 slack = (hash->reach + (hash->cell_size * _NYA_SPATIAL_HASH_RAY_SLACK)) * _nya_spatial_axis_mask(hash->dimensions);

    // per axis, the ray distance between two cell boundaries, and the next boundary crossed after `enter`.
    f32 delta[3] = { F32_MAX, F32_MAX, F32_MAX };
    f32 next[3]  = { F32_MAX, F32_MAX, F32_MAX };

    f32x3 start     = ray.origin + (ray.direction * enter);
    f32   crossings = 1.0F;
    f32   per_step  = 1.0F;

    for (u32 axis = 0; axis < hash->dimensions; axis++) {
        // a segment lies in one cell, so its widened box spans that cell and the slack on either side.
        per_step *= 2.0F + ceilf(2.0F * slack[axis] * hash->cell_size_inverse);

        f32 direction = ray.direction[axis];
        if (direction == 0.0F) continue;

        s32 cell     = _nya_spatial_hash_cell(hash, start[axis]);
        f32 boundary = (f32)(direction > 0.0F ? cell + 1 : cell) * hash->cell_size;

        delta[axis] = hash->cell_size / fabsf(direction);
        next[axis]  = enter + ((boundary - start[axis]) / direction);

        crossings += (fabsf(direction) * (leave - enter) * hash->cell_size_inverse) + 1.0F;
    }

    // a long walk through a sparse world, or a ray that does not move, costs more than testing everything.
    if (crossings * per_step > (f32)hash->count || nya_vector_dot(ray.direction, ray.direction) == 0.0F) {
        for (u32 id = 0; id < hash->capacity; id++) {
            f32 distance = 0.0F;
            if (hash->present[id] && nya_ray3_intersect_box(ray, hash->boxes[id], found ? best.distance : max_distance, &distance)) {
                _nya_spatial_closest_offer(&best, &found, (NYA_SpatialHit){ .id = id, .distance = distance });
            }
        }

        *out_hit = best;
        return found;
    }

    // the segments [from, to] tile [enter, leave] whatever the rounding in `next`, which is what makes the walk exact.
    const u64 steps_max = (2 * (u64)crossings) + 8;

    f32 from = enter;
    for (u64 step = 0;; step++) {
        nya_assert(step < steps_max, "a ray walk crosses about %f cells, not %llu", (f64)crossings, (unsigned long long)step);

        // nothing in a later segment can be closer than what is already found.
        if (found && from > best.distance) break;

        f32 to = nya_min(nya_min(next[0], next[1]), nya_min(next[2], leave));

        f32x3     a       = ray.origin + (ray.direction * from);
        f32x3     b       = ray.origin + (ray.direction * to);
        NYA_Box3f segment = { .min = nya_min(a, b) - slack, .max = nya_max(a, b) + slack };

        _NYA_SpatialCellRange range = _nya_spatial_hash_range(hash, segment);

        for (s32 z = range.min[2]; z <= range.max[2]; z++) {
            for (s32 y = range.min[1]; y <= range.max[1]; y++) {
                for (s32 x = range.min[0]; x <= range.max[0]; x++) _nya_spatial_hash_ray_cell(hash, x, y, z, ray, max_distance, &best, &found);
            }
        }

        if (to >= leave) break;

        u32 axis = next[0] <= next[1] ? (next[0] <= next[2] ? 0 : 2) : (next[1] <= next[2] ? 1 : 2);
        next[axis] += delta[axis];
        from = to;
    }

    *out_hit = best;
    return found;
}

NYA_INTERNAL void _nya_spatial_hash_nearest_cell(const NYA_SpatialHash3D* hash, s32 x, s32 y, s32 z, f32x3 point, u32 k, NYA_SpatialHit* heap,
                                                 u32* count) {
    for (u32 id = hash->bucket_head[_nya_spatial_hash_bucket(hash, x, y, z)]; id != _NYA_SPATIAL_NONE; id = hash->next[id]) {
        const s32* cell = &hash->cells[(u64)id * 3];
        if (cell[0] != x || cell[1] != y || cell[2] != z) continue;

        _nya_spatial_heap_offer(heap, count, k, (NYA_SpatialHit){ .id = id, .distance = nya_box3_distance_squared(hash->boxes[id], point) });
    }
}

/**
 * Shells of cells outward from the point's own. After shell `s` every unvisited item is filed at least `s` cells
 * away, so its box is at least `s` cells less its reach from the point, and the search stops once the k-th is
 * closer than that. Past the item count's worth of cells it gives up and looks at every item.
 * */
NYA_INTERNAL u32 _nya_spatial_hash_nearest(const NYA_SpatialHash3D* hash, f32x3 point, u32 k, OUT NYA_SpatialHit* out_hits) {
    nya_assert(hash != nullptr);
    nya_assert(out_hits != nullptr || k == 0);

    if (k == 0 || hash->count == 0) return 0;

    u32 count = 0;

    s32 center[3] = { 0, 0, 0 };
    for (u32 axis = 0; axis < hash->dimensions; axis++) center[axis] = _nya_spatial_hash_cell(hash, point[axis]);

    // the shell that covers every filed item, and the most cells the shells may cost before a scan is cheaper.
    _NYA_SpatialCellRange occupied = _nya_spatial_hash_range(hash, hash->extent);
    s32                   cover    = 0;
    for (u32 axis = 0; axis < hash->dimensions; axis++) {
        cover = nya_max(cover, nya_max(center[axis] - occupied.min[axis], occupied.max[axis] - center[axis]));
    }

    f32 reach   = nya_max(nya_max(hash->reach.x, hash->reach.y), hash->reach.z);
    u64 visited = 0;
    b8  scan    = false;

    for (s32 shell = 0; shell <= cover; shell++) {
        s32 z_shell = hash->dimensions == 3 ? shell : 0;

        for (s32 dz = -z_shell; dz <= z_shell; dz++) {
            for (s32 dy = -shell; dy <= shell; dy++) {
                b8 face = shell == 0 || abs(dy) == shell || (hash->dimensions == 3 && abs(dz) == shell);

                // off the shell's faces only its two x ends are new.
                for (s32 dx = -shell; dx <= shell; dx += face ? 1 : 2 * shell) {
                    _nya_spatial_hash_nearest_cell(hash, center[0] + dx, center[1] + dy, center[2] + dz, point, k, out_hits, &count);
                    visited++;
                }
            }
        }

        f32 cleared = ((f32)shell * hash->cell_size) - reach;
        // strictly: an unvisited item exactly at the bound with a smaller id would still win the tie.
        if (count == k && cleared > 0.0F && _nya_spatial_heap_bound(out_hits, count, k) < cleared * cleared) break;

        if (_nya_spatial_hash_scan_cheaper(hash, visited)) {
            scan = true;
            break;
        }
    }

    if (scan) {
        count = 0;
        for (u32 id = 0; id < hash->capacity; id++) {
            if (!hash->present[id]) continue;

            _nya_spatial_heap_offer(out_hits, &count, k, (NYA_SpatialHit){ .id = id, .distance = nya_box3_distance_squared(hash->boxes[id], point) });
        }
    }

    _nya_spatial_heap_finish(out_hits, count);

    return count;
}

NYA_INTERNAL NYA_Error _nya_spatial_hash_validate(NYA_Arena* arena, NYA_SpatialHashOptions options, const void* out_hash) {
    if (arena == nullptr || out_hash == nullptr) return nya_error(NYA_ERROR_INVALID_ARGUMENT, "no arena or no out pointer");
    if (options.capacity == 0 || options.capacity > U32_MAX / 4) return nya_error(NYA_ERROR_INVALID_ARGUMENT, "a spatial hash holds 1 to 2^30 items, not %u", options.capacity);
    if (!(options.cell_size > 0.0F) || options.cell_size >= F32_MAX) return nya_error(NYA_ERROR_INVALID_ARGUMENT, "a cell size is positive and finite");

    return NYA_OK;
}

/** Hands the arrays back to the arena; the struct itself is the caller's to free, since the 2D one wraps it. */
NYA_INTERNAL void _nya_spatial_hash_release(NYA_SpatialHash3D* hash) {
    NYA_Arena* arena = hash->allocator;
    u64        n     = hash->capacity;

    nya_arena_free(arena, hash->bucket_head, (u64)hash->bucket_count * sizeof(u32));
    nya_arena_free(arena, hash->boxes, n * sizeof(NYA_Box3f));
    nya_arena_free(arena, hash->cells, n * 3 * sizeof(s32));
    nya_arena_free(arena, hash->next, n * sizeof(u32));
    nya_arena_free(arena, hash->previous, n * sizeof(u32));
    nya_arena_free(arena, hash->present, n * sizeof(b8));
}

NYA_INTERNAL void _nya_spatial_hash_init(NYA_Arena* arena, NYA_SpatialHashOptions options, u32 dimensions, OUT NYA_SpatialHash3D* hash) {
    u32 buckets = 1;
    while (buckets < options.capacity * 2) buckets *= 2;

    u64 n = options.capacity;

    *hash = (NYA_SpatialHash3D){
        .allocator         = arena,
        .capacity          = options.capacity,
        .dimensions        = dimensions,
        .cell_size         = options.cell_size,
        .cell_size_inverse = 1.0F / options.cell_size,
        .bucket_count      = buckets,
        .bucket_head       = nya_arena_alloc(arena, (u64)buckets * sizeof(u32)),
        .boxes             = nya_arena_alloc(arena, n * sizeof(NYA_Box3f)),
        .cells             = nya_arena_alloc(arena, n * 3 * sizeof(s32)),
        .next              = nya_arena_alloc(arena, n * sizeof(u32)),
        .previous          = nya_arena_alloc(arena, n * sizeof(u32)),
        .present           = nya_arena_alloc(arena, n * sizeof(b8)),
    };

    nya_spatial_hash3d_clear(hash);
}

/*
 * -----------------------------------------------------------------------------------------------------------------
 * SPATIAL HASH
 * -----------------------------------------------------------------------------------------------------------------
 */

NYA_Error nya_spatial_hash3d_create(NYA_Arena* arena, NYA_SpatialHashOptions options, OUT NYA_SpatialHash3D** out_hash) {
    NYA_TRY(_nya_spatial_hash_validate(arena, options, out_hash));

    NYA_SpatialHash3D* hash = nya_arena_alloc(arena, sizeof(NYA_SpatialHash3D));
    _nya_spatial_hash_init(arena, options, 3, hash);

    *out_hash = hash;

    return NYA_OK;
}

void nya_spatial_hash3d_destroy(NYA_SpatialHash3D* hash) {
    if (hash == nullptr) return;

    NYA_Arena* arena = hash->allocator;

    _nya_spatial_hash_release(hash);
    nya_arena_free(arena, hash, sizeof(NYA_SpatialHash3D));
}

void nya_spatial_hash3d_clear(NYA_SpatialHash3D* hash) {
    nya_assert(hash != nullptr);

    nya_memset(hash->bucket_head, 0xFF, (u64)hash->bucket_count * sizeof(u32));
    nya_memset(hash->present, 0, (u64)hash->capacity * sizeof(b8));

    hash->count  = 0;
    hash->reach  = f32x3_zero;
    hash->extent = NYA_BOX3F_EMPTY;
}

void nya_spatial_hash3d_add(NYA_SpatialHash3D* hash, u32 id, NYA_Box3f box) {
    nya_assert(hash != nullptr);
    nya_assert(id < hash->capacity, "id %u is past the hash's capacity of %u", id, hash->capacity);
    nya_assert(!hash->present[id], "id %u is already in the hash", id);

    _nya_spatial_hash_file(hash, id, box);

    hash->reach  = nya_max(hash->reach, (box.max - box.min) * 0.5F);
    hash->extent = nya_box3_union(hash->extent, box);
    hash->count++;

    nya_assert(hash->count <= hash->capacity);
}

void nya_spatial_hash3d_build(NYA_SpatialHash3D* hash, const NYA_Box3f* boxes, u32 count) {
    nya_assert(hash != nullptr && (boxes != nullptr || count == 0));
    nya_assert(count <= hash->capacity, "%u items do not fit a hash of %u", count, hash->capacity);

    nya_spatial_hash3d_clear(hash);

    // the bounds stay in registers across the loop rather than going through the struct per item.
    f32x3     reach  = f32x3_zero;
    NYA_Box3f extent = NYA_BOX3F_EMPTY;

    for (u32 id = 0; id < count; id++) {
        NYA_Box3f box = boxes[id];

        _nya_spatial_hash_file(hash, id, box);

        reach  = nya_max(reach, (box.max - box.min) * 0.5F);
        extent = nya_box3_union(extent, box);
    }

    hash->reach  = reach;
    hash->extent = extent;
    hash->count  = count;
}

void nya_spatial_hash3d_remove(NYA_SpatialHash3D* hash, u32 id) {
    nya_assert(hash != nullptr);
    nya_assert(id < hash->capacity && hash->present[id], "id %u is not in the hash", id);

    const s32* cell     = &hash->cells[(u64)id * 3];
    u32        previous = hash->previous[id];
    u32        next     = hash->next[id];

    if (previous != _NYA_SPATIAL_NONE) {
        hash->next[previous] = next;
    } else {
        u32 bucket = _nya_spatial_hash_bucket(hash, cell[0], cell[1], cell[2]);
        nya_assert(hash->bucket_head[bucket] == id, "an item without a predecessor heads its bucket");
        hash->bucket_head[bucket] = next;
    }

    if (next != _NYA_SPATIAL_NONE) hash->previous[next] = previous;

    hash->present[id] = false;
    hash->count--;
}

void nya_spatial_hash3d_move(NYA_SpatialHash3D* hash, u32 id, NYA_Box3f box) {
    nya_assert(hash != nullptr);
    nya_assert(id < hash->capacity && hash->present[id], "id %u is not in the hash", id);

    f32x3      center = nya_box3_center(box);
    const s32* cell   = &hash->cells[(u64)id * 3];

    b8 same_cell = cell[0] == _nya_spatial_hash_cell(hash, center.x) && cell[1] == _nya_spatial_hash_cell(hash, center.y)
                && cell[2] == (hash->dimensions == 3 ? _nya_spatial_hash_cell(hash, center.z) : 0);

    if (!same_cell) {
        nya_spatial_hash3d_remove(hash, id);
        nya_spatial_hash3d_add(hash, id, box);
        return;
    }

    hash->boxes[id] = box;
    hash->reach     = nya_max(hash->reach, (box.max - box.min) * 0.5F);
    hash->extent    = nya_box3_union(hash->extent, box);
}

u32 nya_spatial_hash3d_query_box(const NYA_SpatialHash3D* hash, NYA_Box3f box, OUT u32* out_ids, u32 capacity) {
    _NYA_SpatialQuery query = { .kind = _NYA_SPATIAL_QUERY_BOX, .box = box };

    return _nya_spatial_hash_query(hash, &query, out_ids, capacity);
}

u32 nya_spatial_hash3d_query_radius(const NYA_SpatialHash3D* hash, NYA_Spheref sphere, OUT u32* out_ids, u32 capacity) {
    f32x3             radius = (f32x3){ sphere.radius, sphere.radius, sphere.radius } * _nya_spatial_axis_mask(hash->dimensions);
    _NYA_SpatialQuery query  = { .kind = _NYA_SPATIAL_QUERY_RADIUS, .box = nya_box3_from_center(sphere.center, radius), .sphere = sphere };

    return _nya_spatial_hash_query(hash, &query, out_ids, capacity);
}

b8 nya_spatial_hash3d_raycast(const NYA_SpatialHash3D* hash, NYA_Ray3f ray, f32 max_distance, OUT NYA_SpatialHit* out_hit) {
    return _nya_spatial_hash_raycast(hash, ray, max_distance, out_hit);
}

u32 nya_spatial_hash3d_nearest(const NYA_SpatialHash3D* hash, f32x3 point, u32 k, OUT NYA_SpatialHit* out_hits) {
    return _nya_spatial_hash_nearest(hash, point, k, out_hits);
}

NYA_Error nya_spatial_hash2d_create(NYA_Arena* arena, NYA_SpatialHashOptions options, OUT NYA_SpatialHash2D** out_hash) {
    NYA_TRY(_nya_spatial_hash_validate(arena, options, out_hash));

    NYA_SpatialHash2D* hash = nya_arena_alloc(arena, sizeof(NYA_SpatialHash2D));
    _nya_spatial_hash_init(arena, options, 2, &hash->hash);

    *out_hash = hash;

    return NYA_OK;
}

void nya_spatial_hash2d_destroy(NYA_SpatialHash2D* hash) {
    if (hash == nullptr) return;

    NYA_Arena* arena = hash->hash.allocator;

    _nya_spatial_hash_release(&hash->hash);
    nya_arena_free(arena, hash, sizeof(NYA_SpatialHash2D));
}

void nya_spatial_hash2d_clear(NYA_SpatialHash2D* hash) {
    nya_spatial_hash3d_clear(&hash->hash);
}

void nya_spatial_hash2d_add(NYA_SpatialHash2D* hash, u32 id, NYA_Box2f box) {
    nya_spatial_hash3d_add(&hash->hash, id, _nya_spatial_box_from_2d(box));
}

void nya_spatial_hash2d_build(NYA_SpatialHash2D* hash, const NYA_Box2f* boxes, u32 count) {
    nya_assert(hash != nullptr && (boxes != nullptr || count == 0));
    nya_assert(count <= hash->hash.capacity, "%u items do not fit a hash of %u", count, hash->hash.capacity);

    nya_spatial_hash3d_clear(&hash->hash);

    f32x3     reach  = f32x3_zero;
    NYA_Box3f extent = NYA_BOX3F_EMPTY;

    for (u32 id = 0; id < count; id++) {
        NYA_Box3f box = _nya_spatial_box_from_2d(boxes[id]);

        _nya_spatial_hash_file(&hash->hash, id, box);

        reach  = nya_max(reach, (box.max - box.min) * 0.5F);
        extent = nya_box3_union(extent, box);
    }

    hash->hash.reach  = reach;
    hash->hash.extent = extent;
    hash->hash.count  = count;
}

void nya_spatial_hash2d_remove(NYA_SpatialHash2D* hash, u32 id) {
    nya_spatial_hash3d_remove(&hash->hash, id);
}

void nya_spatial_hash2d_move(NYA_SpatialHash2D* hash, u32 id, NYA_Box2f box) {
    nya_spatial_hash3d_move(&hash->hash, id, _nya_spatial_box_from_2d(box));
}

u32 nya_spatial_hash2d_query_box(const NYA_SpatialHash2D* hash, NYA_Box2f box, OUT u32* out_ids, u32 capacity) {
    return nya_spatial_hash3d_query_box(&hash->hash, _nya_spatial_box_from_2d(box), out_ids, capacity);
}

u32 nya_spatial_hash2d_query_radius(const NYA_SpatialHash2D* hash, NYA_Circlef circle, OUT u32* out_ids, u32 capacity) {
    return nya_spatial_hash3d_query_radius(&hash->hash, _nya_spatial_sphere_from_2d(circle), out_ids, capacity);
}

b8 nya_spatial_hash2d_raycast(const NYA_SpatialHash2D* hash, NYA_Ray2f ray, f32 max_distance, OUT NYA_SpatialHit* out_hit) {
    return nya_spatial_hash3d_raycast(&hash->hash, _nya_spatial_ray_from_2d(ray), max_distance, out_hit);
}

u32 nya_spatial_hash2d_nearest(const NYA_SpatialHash2D* hash, f32x2 point, u32 k, OUT NYA_SpatialHit* out_hits) {
    return nya_spatial_hash3d_nearest(&hash->hash, _nya_spatial_point_from_2d(point), k, out_hits);
}
