/**
 * @file math_spatial.h
 *
 * Three spatial indices over axis aligned boxes, each in 2D and 3D, answering the same four questions: which items
 * overlap a box, which lie within a radius, which one a ray hits first, and which k are nearest a point.
 *
 * - A spatial hash (NYA_SpatialHash2D, NYA_SpatialHash3D): uniform cells over an unbounded world. The fastest add,
 *   remove and move, for many small items of similar size that move every frame.
 * - A loose quadtree and octree (NYA_Quadtree, NYA_Octree): nodes twice their cell, so an item sits at the depth its
 *   size picks and never straddles. For mixed sizes inside a known world, still updated per item.
 * - A BVH (NYA_Bvh2D, NYA_Bvh3D): built in one pass from an array with a binned surface area heuristic and refitted
 *   when the items move. The fastest queries, for a set rebuilt or refitted as a whole.
 *
 * ```c
 * NYA_SpatialHash3D* hash = nullptr;
 * NYA_EXPECT(nya_spatial_hash3d_create(arena, (NYA_SpatialHashOptions){ .capacity = 1024, .cell_size = 8.0F }, &hash));
 *
 * nya_spatial_hash3d_add(hash, 7, nya_box3_from_center(position, (f32x3){ 0.5F, 0.5F, 0.5F }));
 *
 * u32 found[64];
 * u32 total = nya_spatial_hash3d_query_radius(hash, (NYA_Spheref){ .center = player, .radius = 20.0F }, found, 64);
 * ```
 *
 * Every index lives in the arena it was created in; destroy hands its storage back to that arena. Item ids are the caller's own dense
 * indices below `capacity` (an entity slot, an array index), so no handle table sits between them and the slot.
 * A query writes at most `capacity` ids and returns how many matched, so a result above the capacity says the
 * buffer was short. Ray and nearest queries measure against the item boxes; an exact shape test is the caller's.
 *
 * The 2D indices are the 3D ones with z pinned to zero: one implementation, and the 2D query costs a lane of
 * arithmetic it does not need. A separate 2D build halves the box size and was not worth a second copy of each.
 * */
#pragma once

#include "nyangine-std/base/base_arena.h"
#include "nyangine-std/base/base_error.h"
#include "nyangine-std/base/base_types.h"
#include "nyangine-std/math/math_shapes.h"

/*
 * -----------------------------------------------------------------------------------------------------------------
 * CONSTANTS
 * -----------------------------------------------------------------------------------------------------------------
 */

/** The deepest a loose tree goes, and the default. Twelve levels halve a 4 km world to a metre cell. */
#ifndef NYA_LOOSE_TREE_DEPTH_MAX
#define NYA_LOOSE_TREE_DEPTH_MAX 12
#endif

/** Items a BVH leaf holds before the build considers splitting it. */
#ifndef NYA_BVH_LEAF_SIZE
#define NYA_BVH_LEAF_SIZE 4
#endif

/** Buckets the binned build sorts centroids into per split. */
#ifndef NYA_BVH_BINS
#define NYA_BVH_BINS 16
#endif

/** The deepest a BVH is built. A node there is a leaf whatever it holds, which bounds every traversal stack. */
#ifndef NYA_BVH_DEPTH_MAX
#define NYA_BVH_DEPTH_MAX 48
#endif

/*
 * -----------------------------------------------------------------------------------------------------------------
 * TYPES
 * -----------------------------------------------------------------------------------------------------------------
 */

typedef struct NYA_SpatialHit         NYA_SpatialHit;
typedef struct NYA_SpatialHashOptions NYA_SpatialHashOptions;
typedef struct NYA_LooseTreeOptions2D NYA_LooseTreeOptions2D;
typedef struct NYA_LooseTreeOptions3D NYA_LooseTreeOptions3D;
typedef struct NYA_SpatialHash3D      NYA_SpatialHash3D;
typedef struct NYA_SpatialHash2D      NYA_SpatialHash2D;
typedef struct NYA_LooseTreeNode      NYA_LooseTreeNode;
typedef struct NYA_Octree             NYA_Octree;
typedef struct NYA_Quadtree           NYA_Quadtree;
typedef struct NYA_BvhNode            NYA_BvhNode;
typedef struct NYA_Bvh3D              NYA_Bvh3D;
typedef struct NYA_Bvh2D              NYA_Bvh2D;

/** One item a ray or a nearest query found, and how far: along the ray, or from the point to its box. */
struct NYA_SpatialHit {
    u32 id;
    f32 distance;
};

struct NYA_SpatialHashOptions {
    /** One past the largest id. */
    u32 capacity;

    /** The side of one cell. Near the typical query radius; much smaller and a query walks many empty cells. */
    f32 cell_size;
};

struct NYA_LooseTreeOptions2D {
    u32       capacity;
    NYA_Box2f bounds;

    /** Zero means NYA_LOOSE_TREE_DEPTH_MAX. */
    u32 depth_max;
};

struct NYA_LooseTreeOptions3D {
    u32       capacity;
    NYA_Box3f bounds;
    u32       depth_max;
};

/**
 * Items live in the cell their centre falls in, and a query widens by the largest half size ever added, so an item
 * is in exactly one bucket chain and nothing is reported twice. That widening never shrinks before a clear: one huge
 * item makes every later query look further.
 * */
struct NYA_SpatialHash3D {
    NYA_Arena* allocator;

    u32 capacity;
    u32 count;
    u32 dimensions;

    f32 cell_size;
    f32 cell_size_inverse;

    /** A power of two, at least twice the capacity, so a chain averages under one item. */
    u32  bucket_count;
    u32* bucket_head;

    NYA_Box3f* boxes;
    s32*       cells;
    u32*       next;
    u32*       previous;
    b8*        present;

    /** The largest half size added since the last clear, per axis. */
    f32x3 reach;

    /** The union of every box added since the last clear, which bounds a ray walk and a nearest search. */
    NYA_Box3f extent;
};

struct NYA_SpatialHash2D {
    NYA_SpatialHash3D hash;
};

/** A cell of the tree. Its loose box is its `center` plus or minus twice `half_size`. */
struct NYA_LooseTreeNode {
    f32x3 center;
    f32   half_size;
    u32   depth;
    u32   parent;
    u32   children[8];

    u32 first_item;

    /** Items here and below, so a query skips an empty subtree and a remove frees the nodes it empties. */
    u32 subtree_count;
};

/**
 * The octree. Node storage is sized from the capacity once; when it runs out, an item stays at the deepest node
 * that exists, which costs query time and never correctness.
 * */
struct NYA_Octree {
    NYA_Arena* allocator;

    u32 capacity;
    u32 dimensions;
    u32 depth_max;

    NYA_LooseTreeNode* nodes;
    u32                node_capacity;
    u32                node_count;
    u32                node_free;

    NYA_Box3f* boxes;
    u32*       item_node;
    u32*       next;
    u32*       previous;
    u32        count;
};

struct NYA_Quadtree {
    NYA_Octree tree;
};

struct NYA_BvhNode {
    NYA_Box3f box;

    /** An inner node's left child, the right one following it; a leaf's first slot in `order`. */
    u32 first;

    /** Zero for an inner node. */
    u32 count;
};

struct NYA_Bvh3D {
    NYA_Arena* allocator;

    u32 capacity;
    u32 dimensions;

    NYA_BvhNode* nodes;
    u32          node_count;

    /** Item ids in leaf order; a leaf's items are a contiguous run of it. */
    u32* order;

    NYA_Box3f* boxes;
    f32x3*     centroids;
    u32        count;
};

struct NYA_Bvh2D {
    NYA_Bvh3D bvh;
};

/*
 * -----------------------------------------------------------------------------------------------------------------
 * SPATIAL HASH
 * -----------------------------------------------------------------------------------------------------------------
 */

NYA_API NYA_Error nya_spatial_hash3d_create(NYA_Arena* arena, NYA_SpatialHashOptions options, OUT NYA_SpatialHash3D** out_hash) __attr_no_discard;
NYA_API void      nya_spatial_hash3d_destroy(NYA_SpatialHash3D* hash);
NYA_API void      nya_spatial_hash3d_clear(NYA_SpatialHash3D* hash);

/** `id` must be below the capacity and not already present. */
NYA_API void nya_spatial_hash3d_add(NYA_SpatialHash3D* hash, u32 id, NYA_Box3f box);

/**
 * Replaces the contents with `count` items, item `i` being `boxes[i]`: a clear and an add each, without the
 * per call overhead, for a set rebuilt as a whole every frame or tick.
 * */
NYA_API void nya_spatial_hash3d_build(NYA_SpatialHash3D* hash, const NYA_Box3f* boxes, u32 count);
NYA_API void nya_spatial_hash3d_remove(NYA_SpatialHash3D* hash, u32 id);
NYA_API void nya_spatial_hash3d_move(NYA_SpatialHash3D* hash, u32 id, NYA_Box3f box);

NYA_API u32 nya_spatial_hash3d_query_box(const NYA_SpatialHash3D* hash, NYA_Box3f box, OUT u32* out_ids, u32 capacity) __attr_no_discard;
NYA_API u32 nya_spatial_hash3d_query_radius(const NYA_SpatialHash3D* hash, NYA_Spheref sphere, OUT u32* out_ids, u32 capacity) __attr_no_discard;

/** The closest item box the ray enters within `max_distance`. Walks the cells along the ray, front to back. */
NYA_API b8 nya_spatial_hash3d_raycast(const NYA_SpatialHash3D* hash, NYA_Ray3f ray, f32 max_distance, OUT NYA_SpatialHit* out_hit) __attr_no_discard;

/** Up to `k` items nearest `point`, closest first, ties by id. Returns how many it wrote. */
NYA_API u32 nya_spatial_hash3d_nearest(const NYA_SpatialHash3D* hash, f32x3 point, u32 k, OUT NYA_SpatialHit* out_hits) __attr_no_discard;

NYA_API NYA_Error nya_spatial_hash2d_create(NYA_Arena* arena, NYA_SpatialHashOptions options, OUT NYA_SpatialHash2D** out_hash) __attr_no_discard;
NYA_API void      nya_spatial_hash2d_destroy(NYA_SpatialHash2D* hash);
NYA_API void      nya_spatial_hash2d_clear(NYA_SpatialHash2D* hash);
NYA_API void      nya_spatial_hash2d_add(NYA_SpatialHash2D* hash, u32 id, NYA_Box2f box);
NYA_API void      nya_spatial_hash2d_build(NYA_SpatialHash2D* hash, const NYA_Box2f* boxes, u32 count);
NYA_API void      nya_spatial_hash2d_remove(NYA_SpatialHash2D* hash, u32 id);
NYA_API void      nya_spatial_hash2d_move(NYA_SpatialHash2D* hash, u32 id, NYA_Box2f box);
NYA_API u32 nya_spatial_hash2d_query_box(const NYA_SpatialHash2D* hash, NYA_Box2f box, OUT u32* out_ids, u32 capacity) __attr_no_discard;
NYA_API u32 nya_spatial_hash2d_query_radius(const NYA_SpatialHash2D* hash, NYA_Circlef circle, OUT u32* out_ids, u32 capacity) __attr_no_discard;
NYA_API b8  nya_spatial_hash2d_raycast(const NYA_SpatialHash2D* hash, NYA_Ray2f ray, f32 max_distance, OUT NYA_SpatialHit* out_hit) __attr_no_discard;
NYA_API u32 nya_spatial_hash2d_nearest(const NYA_SpatialHash2D* hash, f32x2 point, u32 k, OUT NYA_SpatialHit* out_hits) __attr_no_discard;

/*
 * -----------------------------------------------------------------------------------------------------------------
 * LOOSE QUADTREE AND OCTREE
 * -----------------------------------------------------------------------------------------------------------------
 */

/** `bounds` is the world. An item outside it is kept at the root, found by every query and never lost. */
NYA_API NYA_Error nya_octree_create(NYA_Arena* arena, NYA_LooseTreeOptions3D options, OUT NYA_Octree** out_tree) __attr_no_discard;
NYA_API void      nya_octree_destroy(NYA_Octree* tree);
NYA_API void      nya_octree_clear(NYA_Octree* tree);
NYA_API void      nya_octree_add(NYA_Octree* tree, u32 id, NYA_Box3f box);
NYA_API void      nya_octree_remove(NYA_Octree* tree, u32 id);

/** In place when the item still fits its node, which is most frames for anything that moves less than its size. */
NYA_API void nya_octree_move(NYA_Octree* tree, u32 id, NYA_Box3f box);

NYA_API u32 nya_octree_query_box(const NYA_Octree* tree, NYA_Box3f box, OUT u32* out_ids, u32 capacity) __attr_no_discard;
NYA_API u32 nya_octree_query_radius(const NYA_Octree* tree, NYA_Spheref sphere, OUT u32* out_ids, u32 capacity) __attr_no_discard;
NYA_API b8  nya_octree_raycast(const NYA_Octree* tree, NYA_Ray3f ray, f32 max_distance, OUT NYA_SpatialHit* out_hit) __attr_no_discard;
NYA_API u32 nya_octree_nearest(const NYA_Octree* tree, f32x3 point, u32 k, OUT NYA_SpatialHit* out_hits) __attr_no_discard;

NYA_API NYA_Error nya_quadtree_create(NYA_Arena* arena, NYA_LooseTreeOptions2D options, OUT NYA_Quadtree** out_tree) __attr_no_discard;
NYA_API void      nya_quadtree_destroy(NYA_Quadtree* tree);
NYA_API void      nya_quadtree_clear(NYA_Quadtree* tree);
NYA_API void      nya_quadtree_add(NYA_Quadtree* tree, u32 id, NYA_Box2f box);
NYA_API void      nya_quadtree_remove(NYA_Quadtree* tree, u32 id);
NYA_API void      nya_quadtree_move(NYA_Quadtree* tree, u32 id, NYA_Box2f box);
NYA_API u32       nya_quadtree_query_box(const NYA_Quadtree* tree, NYA_Box2f box, OUT u32* out_ids, u32 capacity) __attr_no_discard;
NYA_API u32       nya_quadtree_query_radius(const NYA_Quadtree* tree, NYA_Circlef circle, OUT u32* out_ids, u32 capacity) __attr_no_discard;
NYA_API b8        nya_quadtree_raycast(const NYA_Quadtree* tree, NYA_Ray2f ray, f32 max_distance, OUT NYA_SpatialHit* out_hit) __attr_no_discard;
NYA_API u32       nya_quadtree_nearest(const NYA_Quadtree* tree, f32x2 point, u32 k, OUT NYA_SpatialHit* out_hits) __attr_no_discard;

/*
 * -----------------------------------------------------------------------------------------------------------------
 * BVH
 * -----------------------------------------------------------------------------------------------------------------
 */

NYA_API NYA_Error nya_bvh3d_create(NYA_Arena* arena, u32 capacity, OUT NYA_Bvh3D** out_bvh) __attr_no_discard;
NYA_API void      nya_bvh3d_destroy(NYA_Bvh3D* bvh);

/** Replaces the whole tree with `count` items, item `i` being `boxes[i]`. */
NYA_API void nya_bvh3d_build(NYA_Bvh3D* bvh, const NYA_Box3f* boxes, u32 count);

/**
 * New boxes for the same items, the tree's shape kept. Linear and far cheaper than a build, and the tree degrades
 * as the items drift from where it was built; rebuild when queries slow down.
 * */
NYA_API void nya_bvh3d_refit(NYA_Bvh3D* bvh, const NYA_Box3f* boxes, u32 count);

NYA_API u32 nya_bvh3d_query_box(const NYA_Bvh3D* bvh, NYA_Box3f box, OUT u32* out_ids, u32 capacity) __attr_no_discard;
NYA_API u32 nya_bvh3d_query_radius(const NYA_Bvh3D* bvh, NYA_Spheref sphere, OUT u32* out_ids, u32 capacity) __attr_no_discard;
NYA_API b8  nya_bvh3d_raycast(const NYA_Bvh3D* bvh, NYA_Ray3f ray, f32 max_distance, OUT NYA_SpatialHit* out_hit) __attr_no_discard;
NYA_API u32 nya_bvh3d_nearest(const NYA_Bvh3D* bvh, f32x3 point, u32 k, OUT NYA_SpatialHit* out_hits) __attr_no_discard;

NYA_API NYA_Error nya_bvh2d_create(NYA_Arena* arena, u32 capacity, OUT NYA_Bvh2D** out_bvh) __attr_no_discard;
NYA_API void      nya_bvh2d_destroy(NYA_Bvh2D* bvh);
NYA_API void      nya_bvh2d_build(NYA_Bvh2D* bvh, const NYA_Box2f* boxes, u32 count);
NYA_API void      nya_bvh2d_refit(NYA_Bvh2D* bvh, const NYA_Box2f* boxes, u32 count);
NYA_API u32       nya_bvh2d_query_box(const NYA_Bvh2D* bvh, NYA_Box2f box, OUT u32* out_ids, u32 capacity) __attr_no_discard;
NYA_API u32       nya_bvh2d_query_radius(const NYA_Bvh2D* bvh, NYA_Circlef circle, OUT u32* out_ids, u32 capacity) __attr_no_discard;
NYA_API b8        nya_bvh2d_raycast(const NYA_Bvh2D* bvh, NYA_Ray2f ray, f32 max_distance, OUT NYA_SpatialHit* out_hit) __attr_no_discard;
NYA_API u32       nya_bvh2d_nearest(const NYA_Bvh2D* bvh, f32x2 point, u32 k, OUT NYA_SpatialHit* out_hits) __attr_no_discard;
