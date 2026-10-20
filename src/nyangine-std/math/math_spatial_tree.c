/**
 * @file math_spatial_tree.c
 *
 * The loose quadtree and octree. A node's loose box is twice its cell, so an item whose half size is at most a
 * node's half size fits that node wherever its centre falls in the cell: insertion picks a depth from the size and a
 * path from the centre, and an item is in exactly one node.
 * */
#include "nyangine-core/nyangine.h"

/*
 * -----------------------------------------------------------------------------------------------------------------
 * INTERNAL
 * -----------------------------------------------------------------------------------------------------------------
 */

/** A traversal pops one node and pushes at most eight, so the stack never holds more than this. */
#define _NYA_LOOSE_TREE_STACK_MAX ((NYA_LOOSE_TREE_DEPTH_MAX + 1) * 8)

/** Nodes per item the storage is sized for. A path of single child nodes per item past that only stops early. */
#define _NYA_LOOSE_TREE_NODES_PER_ITEM 2

/** The fewest nodes a tree gets, so a tree of a handful of items can still subdivide. */
#define _NYA_LOOSE_TREE_NODES_MIN 64

/** A node on the traversal stack, with the key it was ordered by: entry distance for a ray, squared box distance for nearest. */
typedef struct {
    u32 node;
    f32 key;
} _NYA_LooseTreeVisit;

NYA_INTERNAL NYA_Box3f _nya_loose_tree_node_box(const NYA_Octree* tree, const NYA_LooseTreeNode* node) {
    f32x3 half = (f32x3){ node->half_size, node->half_size, node->half_size } * 2.0F * _nya_spatial_axis_mask(tree->dimensions);

    return (NYA_Box3f){ .min = node->center - half, .max = node->center + half };
}

NYA_INTERNAL b8 _nya_loose_tree_in_cell(const NYA_Octree* tree, const NYA_LooseTreeNode* node, f32x3 point) {
    for (u32 axis = 0; axis < tree->dimensions; axis++) {
        if (!(point[axis] >= node->center[axis] - node->half_size && point[axis] <= node->center[axis] + node->half_size)) return false;
    }

    return true;
}

NYA_INTERNAL f32 _nya_loose_tree_half_extent(const NYA_Octree* tree, NYA_Box3f box) {
    f32 extent = 0.0F;
    for (u32 axis = 0; axis < tree->dimensions; axis++) extent = nya_max(extent, (box.max[axis] - box.min[axis]) * 0.5F);

    return extent;
}

NYA_INTERNAL void _nya_loose_tree_root_reset(NYA_Octree* tree, NYA_Box3f bounds) {
    f32x3 size = (bounds.max - bounds.min) * _nya_spatial_axis_mask(tree->dimensions);

    tree->nodes[0] = (NYA_LooseTreeNode){
        .center        = nya_box3_center(bounds) * _nya_spatial_axis_mask(tree->dimensions),
        .half_size     = nya_max(nya_max(size.x, size.y), size.z) * 0.5F,
        .parent        = _NYA_SPATIAL_NONE,
        .first_item    = _NYA_SPATIAL_NONE,
    };

    for (u32 i = 0; i < 8; i++) tree->nodes[0].children[i] = _NYA_SPATIAL_NONE;
}

/** A free node, from the free list first, or none when storage is spent. The free list threads through `parent`. */
NYA_INTERNAL u32 _nya_loose_tree_node_take(NYA_Octree* tree, u32 parent, u32 octant) {
    u32 index = _NYA_SPATIAL_NONE;

    if (tree->node_free != _NYA_SPATIAL_NONE) {
        index           = tree->node_free;
        tree->node_free = tree->nodes[index].parent;
    } else if (tree->node_count < tree->node_capacity) {
        index = tree->node_count++;
    } else {
        return _NYA_SPATIAL_NONE;
    }

    const NYA_LooseTreeNode* above = &tree->nodes[parent];
    f32                      half  = above->half_size * 0.5F;

    f32x3 offset = {
        (octant & 1U) != 0 ? half : -half,
        (octant & 2U) != 0 ? half : -half,
        (octant & 4U) != 0 ? half : -half,
    };

    tree->nodes[index] = (NYA_LooseTreeNode){
        .center     = above->center + (offset * _nya_spatial_axis_mask(tree->dimensions)),
        .half_size  = half,
        .depth      = above->depth + 1,
        .parent     = parent,
        .first_item = _NYA_SPATIAL_NONE,
    };

    for (u32 i = 0; i < 8; i++) tree->nodes[index].children[i] = _NYA_SPATIAL_NONE;

    tree->nodes[parent].children[octant] = index;

    return index;
}

/** Returns emptied nodes to the free list, from `node` up to but never including the root. */
NYA_INTERNAL void _nya_loose_tree_prune(NYA_Octree* tree, u32 node) {
    while (node != 0 && tree->nodes[node].subtree_count == 0) {
        u32 parent = tree->nodes[node].parent;

        b8 unlinked = false;
        for (u32 i = 0; i < 8; i++) {
            if (tree->nodes[parent].children[i] != node) continue;

            tree->nodes[parent].children[i] = _NYA_SPATIAL_NONE;
            unlinked                        = true;
        }
        nya_assert(unlinked, "node %u is a child of its parent %u", node, parent);

        tree->nodes[node].parent = tree->node_free;
        tree->node_free          = node;

        node = parent;
    }
}

NYA_INTERNAL void _nya_loose_tree_count(NYA_Octree* tree, u32 node, s32 change) {
    for (u32 at = node; at != _NYA_SPATIAL_NONE; at = tree->nodes[at].parent) {
        nya_assert(change > 0 || tree->nodes[at].subtree_count > 0);

        tree->nodes[at].subtree_count = (u32)((s64)tree->nodes[at].subtree_count + change);
    }
}

NYA_INTERNAL u32 _nya_loose_tree_octant(const NYA_Octree* tree, const NYA_LooseTreeNode* node, f32x3 point) {
    u32 octant = 0;

    if (point.x >= node->center.x) octant |= 1U;
    if (point.y >= node->center.y) octant |= 2U;
    if (tree->dimensions == 3 && point.z >= node->center.z) octant |= 4U;

    return octant;
}

NYA_INTERNAL void _nya_loose_tree_link(NYA_Octree* tree, u32 id, u32 node) {
    u32 head = tree->nodes[node].first_item;

    tree->item_node[id]          = node;
    tree->previous[id]           = _NYA_SPATIAL_NONE;
    tree->next[id]               = head;
    tree->nodes[node].first_item = id;
    if (head != _NYA_SPATIAL_NONE) tree->previous[head] = id;

    _nya_loose_tree_count(tree, node, 1);
}

NYA_INTERNAL void _nya_loose_tree_add(NYA_Octree* tree, u32 id, NYA_Box3f box) {
    nya_assert(tree != nullptr);
    nya_assert(id < tree->capacity, "id %u is past the tree's capacity of %u", id, tree->capacity);
    nya_assert(tree->item_node[id] == _NYA_SPATIAL_NONE, "id %u is already in the tree", id);
    nya_assert(!nya_box3_is_empty(box), "an item's box has min <= max on every axis");

    f32x3 center = nya_box3_center(box);
    f32   extent = _nya_loose_tree_half_extent(tree, box);
    u32   node   = 0;

    // only the root can be handed a centre outside its cell; it keeps such an item rather than lose it.
    b8 inside = _nya_loose_tree_in_cell(tree, &tree->nodes[0], center);

    while (inside && tree->nodes[node].depth < tree->depth_max && extent <= tree->nodes[node].half_size * 0.5F) {
        u32 octant = _nya_loose_tree_octant(tree, &tree->nodes[node], center);
        u32 child  = tree->nodes[node].children[octant];

        if (child == _NYA_SPATIAL_NONE) child = _nya_loose_tree_node_take(tree, node, octant);
        if (child == _NYA_SPATIAL_NONE) break;

        node = child;
    }

    tree->boxes[id] = box;
    _nya_loose_tree_link(tree, id, node);
    tree->count++;
}

NYA_INTERNAL void _nya_loose_tree_remove(NYA_Octree* tree, u32 id) {
    nya_assert(tree != nullptr);
    nya_assert(id < tree->capacity && tree->item_node[id] != _NYA_SPATIAL_NONE, "id %u is not in the tree", id);

    u32 node     = tree->item_node[id];
    u32 previous = tree->previous[id];
    u32 next     = tree->next[id];

    if (previous != _NYA_SPATIAL_NONE) {
        tree->next[previous] = next;
    } else {
        nya_assert(tree->nodes[node].first_item == id, "an item without a predecessor heads its node");
        tree->nodes[node].first_item = next;
    }

    if (next != _NYA_SPATIAL_NONE) tree->previous[next] = previous;

    tree->item_node[id] = _NYA_SPATIAL_NONE;
    tree->count--;

    _nya_loose_tree_count(tree, node, -1);
    _nya_loose_tree_prune(tree, node);
}

NYA_INTERNAL void _nya_loose_tree_move(NYA_Octree* tree, u32 id, NYA_Box3f box) {
    nya_assert(tree != nullptr);
    nya_assert(id < tree->capacity && tree->item_node[id] != _NYA_SPATIAL_NONE, "id %u is not in the tree", id);

    const NYA_LooseTreeNode* node = &tree->nodes[tree->item_node[id]];

    // the root never keeps an item it could hand down, so a root item always reinserts.
    b8 fits = tree->item_node[id] != 0 && _nya_loose_tree_in_cell(tree, node, nya_box3_center(box))
           && _nya_loose_tree_half_extent(tree, box) <= node->half_size;

    if (!fits) {
        _nya_loose_tree_remove(tree, id);
        _nya_loose_tree_add(tree, id, box);
        return;
    }

    tree->boxes[id] = box;
}

NYA_INTERNAL u32 _nya_loose_tree_query(const NYA_Octree* tree, const _NYA_SpatialQuery* query, OUT u32* out_ids, u32 capacity) {
    nya_assert(tree != nullptr);
    nya_assert(out_ids != nullptr || capacity == 0);

    u32 stack[_NYA_LOOSE_TREE_STACK_MAX];
    u32 depth = 0;
    u32 total = 0;

    stack[depth++] = 0;

    while (depth > 0) {
        const NYA_LooseTreeNode* node = &tree->nodes[stack[--depth]];

        if (node->subtree_count == 0) continue;
        if (node != tree->nodes && !_nya_spatial_query_accepts(query, _nya_loose_tree_node_box(tree, node))) continue;

        for (u32 id = node->first_item; id != _NYA_SPATIAL_NONE; id = tree->next[id]) {
            if (_nya_spatial_query_accepts(query, tree->boxes[id])) _nya_spatial_emit(out_ids, capacity, &total, id);
        }

        for (u32 i = 0; i < 8; i++) {
            if (node->children[i] == _NYA_SPATIAL_NONE) continue;

            nya_assert(depth < _NYA_LOOSE_TREE_STACK_MAX);
            stack[depth++] = node->children[i];
        }
    }

    return total;
}

/** Pushes `count` children so the smallest key is popped first. */
NYA_INTERNAL void _nya_loose_tree_push_ordered(_NYA_LooseTreeVisit* stack, u32* depth, _NYA_LooseTreeVisit* children, u32 count) {
    for (u32 i = 1; i < count; i++) {
        _NYA_LooseTreeVisit moving = children[i];

        u32 at = i;
        while (at > 0 && children[at - 1].key < moving.key) {
            children[at] = children[at - 1];
            at--;
        }
        children[at] = moving;
    }

    for (u32 i = 0; i < count; i++) {
        nya_assert(*depth < _NYA_LOOSE_TREE_STACK_MAX);
        stack[(*depth)++] = children[i];
    }
}

NYA_INTERNAL b8 _nya_loose_tree_raycast(const NYA_Octree* tree, NYA_Ray3f ray, f32 max_distance, OUT NYA_SpatialHit* out_hit) {
    nya_assert(tree != nullptr && out_hit != nullptr);

    _NYA_LooseTreeVisit stack[_NYA_LOOSE_TREE_STACK_MAX];
    u32                 depth = 0;

    NYA_SpatialHit best  = { 0 };
    b8             found = false;

    stack[depth++] = (_NYA_LooseTreeVisit){ .node = 0, .key = 0.0F };

    while (depth > 0) {
        _NYA_LooseTreeVisit      visit = stack[--depth];
        const NYA_LooseTreeNode* node  = &tree->nodes[visit.node];

        if (node->subtree_count == 0 || (found && visit.key > best.distance)) continue;

        for (u32 id = node->first_item; id != _NYA_SPATIAL_NONE; id = tree->next[id]) {
            f32 distance = 0.0F;
            if (nya_ray3_intersect_box(ray, tree->boxes[id], found ? best.distance : max_distance, &distance)) {
                _nya_spatial_closest_offer(&best, &found, (NYA_SpatialHit){ .id = id, .distance = distance });
            }
        }

        _NYA_LooseTreeVisit children[8];
        u32                 count = 0;

        for (u32 i = 0; i < 8; i++) {
            u32 child = node->children[i];
            if (child == _NYA_SPATIAL_NONE || tree->nodes[child].subtree_count == 0) continue;

            f32 distance = 0.0F;
            if (!nya_ray3_intersect_box(ray, _nya_loose_tree_node_box(tree, &tree->nodes[child]), found ? best.distance : max_distance, &distance)) continue;

            children[count++] = (_NYA_LooseTreeVisit){ .node = child, .key = distance };
        }

        _nya_loose_tree_push_ordered(stack, &depth, children, count);
    }

    *out_hit = best;
    return found;
}

NYA_INTERNAL u32 _nya_loose_tree_nearest(const NYA_Octree* tree, f32x3 point, u32 k, OUT NYA_SpatialHit* out_hits) {
    nya_assert(tree != nullptr);
    nya_assert(out_hits != nullptr || k == 0);

    if (k == 0) return 0;

    _NYA_LooseTreeVisit stack[_NYA_LOOSE_TREE_STACK_MAX];
    u32                 depth = 0;
    u32                 count = 0;

    stack[depth++] = (_NYA_LooseTreeVisit){ .node = 0, .key = 0.0F };

    while (depth > 0) {
        _NYA_LooseTreeVisit      visit = stack[--depth];
        const NYA_LooseTreeNode* node  = &tree->nodes[visit.node];

        if (node->subtree_count == 0 || visit.key > _nya_spatial_heap_bound(out_hits, count, k)) continue;

        for (u32 id = node->first_item; id != _NYA_SPATIAL_NONE; id = tree->next[id]) {
            _nya_spatial_heap_offer(out_hits, &count, k, (NYA_SpatialHit){ .id = id, .distance = nya_box3_distance_squared(tree->boxes[id], point) });
        }

        _NYA_LooseTreeVisit children[8];
        u32                 child_count = 0;

        for (u32 i = 0; i < 8; i++) {
            u32 child = node->children[i];
            if (child == _NYA_SPATIAL_NONE || tree->nodes[child].subtree_count == 0) continue;

            f32 distance = nya_box3_distance_squared(_nya_loose_tree_node_box(tree, &tree->nodes[child]), point);
            if (distance > _nya_spatial_heap_bound(out_hits, count, k)) continue;

            children[child_count++] = (_NYA_LooseTreeVisit){ .node = child, .key = distance };
        }

        _nya_loose_tree_push_ordered(stack, &depth, children, child_count);
    }

    _nya_spatial_heap_finish(out_hits, count);

    return count;
}

NYA_INTERNAL void _nya_loose_tree_clear(NYA_Octree* tree, NYA_Box3f bounds) {
    nya_assert(tree != nullptr);

    tree->node_count = 1;
    tree->node_free  = _NYA_SPATIAL_NONE;
    tree->count      = 0;

    _nya_loose_tree_root_reset(tree, bounds);

    nya_memset(tree->item_node, 0xFF, (u64)tree->capacity * sizeof(u32));
}

NYA_INTERNAL NYA_Error _nya_loose_tree_validate(NYA_Arena* arena, u32 capacity, NYA_Box3f bounds, u32 depth_max, const void* out_tree) {
    if (arena == nullptr || out_tree == nullptr) return nya_error(NYA_ERROR_INVALID_ARGUMENT, "no arena or no out pointer");
    if (capacity == 0 || capacity > U32_MAX / (4 * _NYA_LOOSE_TREE_NODES_PER_ITEM)) return nya_error(NYA_ERROR_INVALID_ARGUMENT, "a loose tree holds 1 to 2^29 items, not %u", capacity);
    if (nya_box3_is_empty(bounds)) return nya_error(NYA_ERROR_INVALID_ARGUMENT, "a loose tree's bounds have min <= max on every axis");
    if (depth_max > NYA_LOOSE_TREE_DEPTH_MAX) return nya_error(NYA_ERROR_INVALID_ARGUMENT, "a loose tree is at most %u deep, not %u", NYA_LOOSE_TREE_DEPTH_MAX, depth_max);

    return NYA_OK;
}

NYA_INTERNAL void _nya_loose_tree_init(NYA_Arena* arena, u32 capacity, NYA_Box3f bounds, u32 depth_max, u32 dimensions, OUT NYA_Octree* tree) {
    u32 nodes = nya_max(capacity * _NYA_LOOSE_TREE_NODES_PER_ITEM, (u32)_NYA_LOOSE_TREE_NODES_MIN);
    u64 n     = capacity;

    *tree = (NYA_Octree){
        .allocator     = arena,
        .capacity      = capacity,
        .dimensions    = dimensions,
        .depth_max     = depth_max == 0 ? NYA_LOOSE_TREE_DEPTH_MAX : depth_max,
        .nodes         = nya_arena_alloc(arena, (u64)nodes * sizeof(NYA_LooseTreeNode)),
        .node_capacity = nodes,
        .boxes         = nya_arena_alloc(arena, n * sizeof(NYA_Box3f)),
        .item_node     = nya_arena_alloc(arena, n * sizeof(u32)),
        .next          = nya_arena_alloc(arena, n * sizeof(u32)),
        .previous      = nya_arena_alloc(arena, n * sizeof(u32)),
    };

    _nya_loose_tree_clear(tree, bounds);
}

NYA_INTERNAL void _nya_loose_tree_release(NYA_Octree* tree) {
    NYA_Arena* arena = tree->allocator;
    u64        n     = tree->capacity;

    nya_arena_free(arena, tree->nodes, (u64)tree->node_capacity * sizeof(NYA_LooseTreeNode));
    nya_arena_free(arena, tree->boxes, n * sizeof(NYA_Box3f));
    nya_arena_free(arena, tree->item_node, n * sizeof(u32));
    nya_arena_free(arena, tree->next, n * sizeof(u32));
    nya_arena_free(arena, tree->previous, n * sizeof(u32));
}

/*
 * -----------------------------------------------------------------------------------------------------------------
 * OCTREE
 * -----------------------------------------------------------------------------------------------------------------
 */

NYA_Error nya_octree_create(NYA_Arena* arena, NYA_LooseTreeOptions3D options, OUT NYA_Octree** out_tree) {
    NYA_TRY(_nya_loose_tree_validate(arena, options.capacity, options.bounds, options.depth_max, out_tree));

    NYA_Octree* tree = nya_arena_alloc(arena, sizeof(NYA_Octree));
    _nya_loose_tree_init(arena, options.capacity, options.bounds, options.depth_max, 3, tree);

    *out_tree = tree;

    return NYA_OK;
}

void nya_octree_destroy(NYA_Octree* tree) {
    if (tree == nullptr) return;

    NYA_Arena* arena = tree->allocator;

    _nya_loose_tree_release(tree);
    nya_arena_free(arena, tree, sizeof(NYA_Octree));
}

void nya_octree_clear(NYA_Octree* tree) {
    nya_assert(tree != nullptr);

    const NYA_LooseTreeNode* root = &tree->nodes[0];
    f32x3                    half = { root->half_size, root->half_size, root->half_size };

    _nya_loose_tree_clear(tree, (NYA_Box3f){ .min = root->center - half, .max = root->center + half });
}

void nya_octree_add(NYA_Octree* tree, u32 id, NYA_Box3f box) {
    _nya_loose_tree_add(tree, id, box);
}

void nya_octree_remove(NYA_Octree* tree, u32 id) {
    _nya_loose_tree_remove(tree, id);
}

void nya_octree_move(NYA_Octree* tree, u32 id, NYA_Box3f box) {
    _nya_loose_tree_move(tree, id, box);
}

u32 nya_octree_query_box(const NYA_Octree* tree, NYA_Box3f box, OUT u32* out_ids, u32 capacity) {
    _NYA_SpatialQuery query = { .kind = _NYA_SPATIAL_QUERY_BOX, .box = box };

    return _nya_loose_tree_query(tree, &query, out_ids, capacity);
}

u32 nya_octree_query_radius(const NYA_Octree* tree, NYA_Spheref sphere, OUT u32* out_ids, u32 capacity) {
    _NYA_SpatialQuery query = { .kind = _NYA_SPATIAL_QUERY_RADIUS, .sphere = sphere };

    return _nya_loose_tree_query(tree, &query, out_ids, capacity);
}

b8 nya_octree_raycast(const NYA_Octree* tree, NYA_Ray3f ray, f32 max_distance, OUT NYA_SpatialHit* out_hit) {
    return _nya_loose_tree_raycast(tree, ray, max_distance, out_hit);
}

u32 nya_octree_nearest(const NYA_Octree* tree, f32x3 point, u32 k, OUT NYA_SpatialHit* out_hits) {
    return _nya_loose_tree_nearest(tree, point, k, out_hits);
}

/*
 * -----------------------------------------------------------------------------------------------------------------
 * QUADTREE
 * -----------------------------------------------------------------------------------------------------------------
 */

NYA_Error nya_quadtree_create(NYA_Arena* arena, NYA_LooseTreeOptions2D options, OUT NYA_Quadtree** out_tree) {
    NYA_Box3f bounds = _nya_spatial_box_from_2d(options.bounds);

    NYA_TRY(_nya_loose_tree_validate(arena, options.capacity, bounds, options.depth_max, out_tree));

    NYA_Quadtree* tree = nya_arena_alloc(arena, sizeof(NYA_Quadtree));
    _nya_loose_tree_init(arena, options.capacity, bounds, options.depth_max, 2, &tree->tree);

    *out_tree = tree;

    return NYA_OK;
}

void nya_quadtree_destroy(NYA_Quadtree* tree) {
    if (tree == nullptr) return;

    NYA_Arena* arena = tree->tree.allocator;

    _nya_loose_tree_release(&tree->tree);
    nya_arena_free(arena, tree, sizeof(NYA_Quadtree));
}

void nya_quadtree_clear(NYA_Quadtree* tree) {
    nya_octree_clear(&tree->tree);
}

void nya_quadtree_add(NYA_Quadtree* tree, u32 id, NYA_Box2f box) {
    _nya_loose_tree_add(&tree->tree, id, _nya_spatial_box_from_2d(box));
}

void nya_quadtree_remove(NYA_Quadtree* tree, u32 id) {
    _nya_loose_tree_remove(&tree->tree, id);
}

void nya_quadtree_move(NYA_Quadtree* tree, u32 id, NYA_Box2f box) {
    _nya_loose_tree_move(&tree->tree, id, _nya_spatial_box_from_2d(box));
}

u32 nya_quadtree_query_box(const NYA_Quadtree* tree, NYA_Box2f box, OUT u32* out_ids, u32 capacity) {
    return nya_octree_query_box(&tree->tree, _nya_spatial_box_from_2d(box), out_ids, capacity);
}

u32 nya_quadtree_query_radius(const NYA_Quadtree* tree, NYA_Circlef circle, OUT u32* out_ids, u32 capacity) {
    return nya_octree_query_radius(&tree->tree, _nya_spatial_sphere_from_2d(circle), out_ids, capacity);
}

b8 nya_quadtree_raycast(const NYA_Quadtree* tree, NYA_Ray2f ray, f32 max_distance, OUT NYA_SpatialHit* out_hit) {
    return nya_octree_raycast(&tree->tree, _nya_spatial_ray_from_2d(ray), max_distance, out_hit);
}

u32 nya_quadtree_nearest(const NYA_Quadtree* tree, f32x2 point, u32 k, OUT NYA_SpatialHit* out_hits) {
    return nya_octree_nearest(&tree->tree, _nya_spatial_point_from_2d(point), k, out_hits);
}
