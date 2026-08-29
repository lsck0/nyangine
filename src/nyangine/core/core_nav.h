/**
 * @file core_nav.h
 *
 * Grid navigation: a cost field, A* over it, and a flow field for when many agents share one goal.
 *
 * ```c
 * NYA_NavGrid* grid = nullptr;
 * NYA_EXPECT(nya_nav_grid_from_tilemap(arena, map, "collision", &grid));
 *
 * NYA_NavPoint path[128];
 * u32 length = nya_nav_path(grid, (NYA_NavPoint){ 1, 1 }, (NYA_NavPoint){ 30, 20 }, path, 128, (NYA_NavOptions){ .diagonal = true });
 * ```
 *
 * **The cost field belongs here, not to the tilemap.** A tilemap cell is one `u32` of tile id and
 * nothing else, and what counts as passable is a game's question — a door is solid to one creature and
 * not another. `nya_nav_grid_from_tilemap` derives a grid from a layer once; after that the two are
 * independent, which is also what makes a destructible world cheap: punch a hole, set one cost.
 *
 * **A\* for one agent to one goal; a flow field for many agents to the same goal.** A* is cheaper for a
 * single query and a flow field is cheaper from the tenth agent onward, because it pays Dijkstra once
 * over the whole grid and then every agent is an O(1) lookup. Neither is right for both.
 *
 * Cost is a `u8` per cell where **0 means blocked** and 1 is ordinary ground; higher is passable but
 * expensive, which is how mud, or "prefer the road", is expressed. Capping at 255 rather than using a
 * float keeps the grid one byte per cell, so a 512×512 world is 256 kB and fits in cache.
 * */
#pragma once

#include "nyangine/base/base_error.h"
#include "nyangine/base/base_types.h"
#include "nyangine/core/core_tilemap.h"
#include "nyangine/math/math_vector.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * CONSTANTS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/** A cell nothing may enter. */
#define NYA_NAV_BLOCKED 0

/** What an ordinary, unremarkable cell costs. */
#define NYA_NAV_COST_DEFAULT 1

/** Returned by nya_nav_flow_distance for a cell the goal cannot reach. */
#define NYA_NAV_UNREACHABLE 0xFFFFFFFFU

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * TYPES
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

typedef struct NYA_NavGrid    NYA_NavGrid;
typedef struct NYA_NavFlow    NYA_NavFlow;
typedef struct NYA_NavPoint   NYA_NavPoint;
typedef struct NYA_NavOptions NYA_NavOptions;

/** A cell coordinate. Signed, so out-of-bounds arithmetic is checkable rather than wrapping. */
struct NYA_NavPoint {
    s32 x, y;
};

struct NYA_NavOptions {
    /** Whether diagonal moves are allowed. Off is four-neighbour. */
    b8 diagonal;

    /**
     * Whether a diagonal may pass between two blocked orthogonal neighbours.
     *
     * Off by default, and off is almost always right: a unit with any width cutting that corner clips
     * the wall, which is the single most common way grid pathing looks broken.
     * */
    b8 cut_corners;

    /**
     * The most cells to expand before giving up. Zero means the whole grid.
     *
     * A budget rather than a limit of the algorithm: an unreachable goal on a large open map expands
     * every cell before it can say so, and a frame is not the place to discover that.
     * */
    u32 max_nodes;
};

struct NYA_NavGrid {
    NYA_Arena* allocator;

    u32 width, height;

    /** One byte per cell. NYA_NAV_BLOCKED is impassable; higher is passable and more expensive. */
    u8* cost;

    /*
     * Search scratch, sized with the grid and reused.
     *
     * Kept here rather than allocated per query because the point of A* on a grid is that a hundred
     * agents can each ask once a frame, and an arena does not hand memory back — allocating per query
     * would grow it without bound.
     */
    u32* g_score;
    u32* came_from;
    u32* visit_stamp;
    u32* open_heap;
    u32* open_score;
    u32  stamp;
};

struct NYA_NavFlow {
    NYA_Arena*         allocator;
    const NYA_NavGrid* grid;

    /** Total cost from each cell to the goal, or NYA_NAV_UNREACHABLE. */
    u32* distance;

    /** Scratch for the Dijkstra sweep. */
    u32* queue;

    NYA_NavPoint goal;
    b8           built;
};

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * FUNCTIONS AND MACROS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/** An empty grid where every cell costs NYA_NAV_COST_DEFAULT. */
NYA_API NYA_Error nya_nav_grid_create(NYA_Arena* arena, u32 width, u32 height, OUT NYA_NavGrid** out_grid) __attr_no_discard;

/**
 * A grid derived from a tilemap layer: any non-empty tile on `solid_layer` is blocked.
 *
 * The same rule nya_tilemap_collision_build uses, so what an agent cannot walk through is what a body
 * cannot pass through. Fails if the layer does not exist, rather than quietly producing an open field.
 * */
NYA_API NYA_Error nya_nav_grid_from_tilemap(NYA_Arena* arena, const NYA_Tilemap* map, NYA_ConstCString solid_layer,
                                            OUT NYA_NavGrid** out_grid) __attr_no_discard;

NYA_API void nya_nav_cost_set(NYA_NavGrid* grid, s32 x, s32 y, u8 cost);
NYA_API u8   nya_nav_cost_at(const NYA_NavGrid* grid, s32 x, s32 y) __attr_no_discard;
NYA_API b8   nya_nav_walkable(const NYA_NavGrid* grid, s32 x, s32 y) __attr_no_discard;

/** Sets every cell to `cost`. */
NYA_API void nya_nav_fill(NYA_NavGrid* grid, u8 cost);

/**
 * Finds a path from `from` to `to`, writing it into `out_path` including both ends.
 *
 * Returns the number of points written, or 0 when there is no path, either end is blocked or off the
 * grid, or the budget ran out. A path longer than `capacity` is refused rather than truncated: half a
 * path leads somewhere nobody asked to go.
 * */
NYA_API u32 nya_nav_path(NYA_NavGrid* grid, NYA_NavPoint from, NYA_NavPoint to, OUT NYA_NavPoint* out_path, u32 capacity,
                         NYA_NavOptions options);

/** A flow field over `grid`. The grid must outlive it. */
NYA_API NYA_Error nya_nav_flow_create(NYA_Arena* arena, const NYA_NavGrid* grid, OUT NYA_NavFlow** out_flow) __attr_no_discard;

/**
 * Recomputes the field for `goal`: one Dijkstra sweep outward, after which every agent is a lookup.
 *
 * The cost of this is the whole grid, so it is what you do when the goal moves, not per agent.
 * */
NYA_API void nya_nav_flow_build(NYA_NavFlow* flow, NYA_NavPoint goal);

/** Total cost from `point` to the goal, or NYA_NAV_UNREACHABLE. */
NYA_API u32 nya_nav_flow_distance(const NYA_NavFlow* flow, s32 x, s32 y) __attr_no_discard;

/** The neighbouring cell to step into from `from`, or `from` itself at the goal or when stuck. */
NYA_API NYA_NavPoint nya_nav_flow_step(const NYA_NavFlow* flow, NYA_NavPoint from) __attr_no_discard;

/** The same step as a normalised direction, or zero when there is nowhere to go. */
NYA_API f32x2 nya_nav_flow_direction(const NYA_NavFlow* flow, NYA_NavPoint from) __attr_no_discard;
