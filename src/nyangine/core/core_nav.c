#include "nyangine/nyangine.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * INTERNALS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/**
 * Cost of one orthogonal and one diagonal step, in tenths.
 *
 * Integer arithmetic, so the whole search is exact and reproducible — a float g-score makes two runs
 * over the same grid disagree on ties, and a path that flickers between two equal routes reads as a
 * unit that cannot make up its mind. 14 is 10·√2 rounded, which keeps a diagonal honestly more
 * expensive than an orthogonal without ever being cheaper than two of them.
 */
#define _NYA_NAV_STEP_ORTHOGONAL 10
#define _NYA_NAV_STEP_DIAGONAL   14

NYA_INTERNAL b8 _nya_nav_in_bounds(const NYA_NavGrid* grid, s32 x, s32 y) {
    return x >= 0 && y >= 0 && (u32)x < grid->width && (u32)y < grid->height;
}

NYA_INTERNAL u32 _nya_nav_index(const NYA_NavGrid* grid, s32 x, s32 y) {
    return ((u32)y * grid->width) + (u32)x;
}

/** Octile distance, the admissible heuristic for an eight-neighbour grid at these step costs. */
NYA_INTERNAL u32 _nya_nav_heuristic(NYA_NavPoint a, NYA_NavPoint b, b8 diagonal) {
    u32 dx = (u32)abs(a.x - b.x);
    u32 dy = (u32)abs(a.y - b.y);

    if (!diagonal) return (dx + dy) * _NYA_NAV_STEP_ORTHOGONAL;

    u32 lo = dx < dy ? dx : dy;
    u32 hi = dx < dy ? dy : dx;

    return (_NYA_NAV_STEP_DIAGONAL * lo) + (_NYA_NAV_STEP_ORTHOGONAL * (hi - lo));
}

/*
 * A binary min-heap over cell indices, keyed by `open_score`.
 *
 * Written here rather than derived from base_heap.h because the comparison is against a *separate*
 * array indexed by the item, which a compare-two-items interface cannot express without a global.
 */
NYA_INTERNAL void _nya_nav_heap_push(NYA_NavGrid* grid, u32* length, u32 cell, u32 score) {
    grid->open_score[cell] = score;

    u32 i             = (*length)++;
    grid->open_heap[i] = cell;

    while (i > 0) {
        u32 parent = (i - 1) / 2;
        if (grid->open_score[grid->open_heap[parent]] <= grid->open_score[grid->open_heap[i]]) break;

        u32 swap                = grid->open_heap[parent];
        grid->open_heap[parent] = grid->open_heap[i];
        grid->open_heap[i]      = swap;

        i = parent;
    }
}

NYA_INTERNAL u32 _nya_nav_heap_pop(NYA_NavGrid* grid, u32* length) {
    u32 top = grid->open_heap[0];

    grid->open_heap[0] = grid->open_heap[--(*length)];

    u32 i = 0;
    for (;;) {
        u32 left     = (2 * i) + 1;
        u32 right    = left + 1;
        u32 smallest = i;

        if (left < *length && grid->open_score[grid->open_heap[left]] < grid->open_score[grid->open_heap[smallest]]) smallest = left;
        if (right < *length && grid->open_score[grid->open_heap[right]] < grid->open_score[grid->open_heap[smallest]]) smallest = right;
        if (smallest == i) break;

        u32 swap                  = grid->open_heap[smallest];
        grid->open_heap[smallest] = grid->open_heap[i];
        grid->open_heap[i]        = swap;

        i = smallest;
    }

    return top;
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PUBLIC API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

NYA_Error nya_nav_grid_create(NYA_Arena* arena, u32 width, u32 height, OUT NYA_NavGrid** out_grid) {
    if (arena == nullptr || out_grid == nullptr) return nya_error(NYA_ERROR_INVALID_ARGUMENT, "no arena or no out pointer");
    if (width == 0 || height == 0) return nya_error(NYA_ERROR_INVALID_ARGUMENT, "a nav grid needs a non-zero size");

    NYA_NavGrid* grid = nya_arena_alloc(arena, sizeof(NYA_NavGrid));
    u64          cells = (u64)width * (u64)height;

    *grid = (NYA_NavGrid){
        .allocator   = arena,
        .width       = width,
        .height      = height,
        .cost        = nya_arena_alloc(arena, cells * sizeof(u8)),
        .g_score     = nya_arena_alloc(arena, cells * sizeof(u32)),
        .came_from   = nya_arena_alloc(arena, cells * sizeof(u32)),
        .visit_stamp = nya_arena_alloc(arena, cells * sizeof(u32)),
        .open_heap   = nya_arena_alloc(arena, cells * sizeof(u32)),
        .open_score  = nya_arena_alloc(arena, cells * sizeof(u32)),
    };

    // Stamps start at zero and the counter starts at zero, so the first search's stamp of 1 already
    // invalidates everything without a memset per query.
    nya_memset(grid->visit_stamp, 0, cells * sizeof(u32));
    nya_memset(grid->cost, NYA_NAV_COST_DEFAULT, cells * sizeof(u8));

    *out_grid = grid;

    return NYA_OK;
}

NYA_Error nya_nav_grid_from_tilemap(NYA_Arena* arena, const NYA_Tilemap* map, NYA_ConstCString solid_layer, OUT NYA_NavGrid** out_grid) {
    if (map == nullptr) return nya_error(NYA_ERROR_INVALID_ARGUMENT, "no tilemap");

    u32 layer = nya_tilemap_layer_find(map, solid_layer);
    if (layer == NYA_TILEMAP_LAYER_NONE) return nya_error(NYA_ERROR_NOT_FOUND, "the tilemap has no layer called '%s'", solid_layer);

    NYA_NavGrid* grid = nullptr;
    NYA_TRY(nya_nav_grid_create(arena, map->width, map->height, &grid));

    // Any non-empty tile is solid, which is the rule nya_tilemap_collision_build uses — so what an
    // agent will not walk through is what a body cannot pass through.
    for (u32 y = 0; y < map->height; y++) {
        for (u32 x = 0; x < map->width; x++) {
            b8 solid = nya_tilemap_tile_at(map, layer, (s32)x, (s32)y) != 0;

            grid->cost[_nya_nav_index(grid, (s32)x, (s32)y)] = solid ? NYA_NAV_BLOCKED : NYA_NAV_COST_DEFAULT;
        }
    }

    *out_grid = grid;

    return NYA_OK;
}

void nya_nav_cost_set(NYA_NavGrid* grid, s32 x, s32 y, u8 cost) {
    if (grid == nullptr || !_nya_nav_in_bounds(grid, x, y)) return;

    grid->cost[_nya_nav_index(grid, x, y)] = cost;
}

u8 nya_nav_cost_at(const NYA_NavGrid* grid, s32 x, s32 y) {
    // Off the grid is blocked rather than an assert, matching nya_tilemap_tile_at: a search walking off
    // the edge is ordinary, not a bug.
    if (grid == nullptr || !_nya_nav_in_bounds(grid, x, y)) return NYA_NAV_BLOCKED;

    return grid->cost[_nya_nav_index(grid, x, y)];
}

b8 nya_nav_walkable(const NYA_NavGrid* grid, s32 x, s32 y) {
    return nya_nav_cost_at(grid, x, y) != NYA_NAV_BLOCKED;
}

void nya_nav_fill(NYA_NavGrid* grid, u8 cost) {
    if (grid == nullptr) return;

    nya_memset(grid->cost, cost, (u64)grid->width * (u64)grid->height * sizeof(u8));
}

u32 nya_nav_path(NYA_NavGrid* grid, NYA_NavPoint from, NYA_NavPoint to, OUT NYA_NavPoint* out_path, u32 capacity, NYA_NavOptions options) {
    if (grid == nullptr || out_path == nullptr || capacity == 0) return 0;
    if (!nya_nav_walkable(grid, from.x, from.y) || !nya_nav_walkable(grid, to.x, to.y)) return 0;

    const u32 cells   = grid->width * grid->height;
    const u32 budget  = options.max_nodes == 0 ? cells : options.max_nodes;
    const u32 start   = _nya_nav_index(grid, from.x, from.y);
    const u32 target  = _nya_nav_index(grid, to.x, to.y);

    // A stamp per search rather than clearing the scratch: clearing three arrays over a large grid
    // costs more than the search itself on a short path.
    grid->stamp++;

    u32 open_length = 0;

    grid->g_score[start]     = 0;
    grid->came_from[start]   = start;
    grid->visit_stamp[start] = grid->stamp;

    _nya_nav_heap_push(grid, &open_length, start, _nya_nav_heuristic(from, to, options.diagonal));

    // Eight neighbours, orthogonals first so a four-neighbour search is the first half of this table.
    const s32 offsets[8][2] = { { 1, 0 }, { -1, 0 }, { 0, 1 }, { 0, -1 }, { 1, 1 }, { 1, -1 }, { -1, 1 }, { -1, -1 } };
    const u32 neighbours    = options.diagonal ? 8 : 4;

    u32 expanded = 0;
    b8  found    = false;

    while (open_length > 0) {
        u32 current = _nya_nav_heap_pop(grid, &open_length);

        if (current == target) {
            found = true;
            break;
        }

        if (++expanded > budget) break;

        s32 cx = (s32)(current % grid->width);
        s32 cy = (s32)(current / grid->width);

        for (u32 n = 0; n < neighbours; n++) {
            s32 nx = cx + offsets[n][0];
            s32 ny = cy + offsets[n][1];

            u8 cost = nya_nav_cost_at(grid, nx, ny);
            if (cost == NYA_NAV_BLOCKED) continue;

            b8 is_diagonal = offsets[n][0] != 0 && offsets[n][1] != 0;

            /*
             * A diagonal past two blocked orthogonals is refused unless asked for.
             *
             * A unit with any width cutting that corner clips the wall, which is the most common way
             * grid pathing looks broken even though the path is technically valid.
             */
            if (is_diagonal && !options.cut_corners) {
                if (!nya_nav_walkable(grid, cx + offsets[n][0], cy) && !nya_nav_walkable(grid, cx, cy + offsets[n][1])) continue;
            }

            u32 step      = is_diagonal ? _NYA_NAV_STEP_DIAGONAL : _NYA_NAV_STEP_ORTHOGONAL;
            u32 neighbour = _nya_nav_index(grid, nx, ny);
            u32 tentative = grid->g_score[current] + (step * cost);

            b8 seen = grid->visit_stamp[neighbour] == grid->stamp;
            if (seen && tentative >= grid->g_score[neighbour]) continue;

            grid->g_score[neighbour]     = tentative;
            grid->came_from[neighbour]   = current;
            grid->visit_stamp[neighbour] = grid->stamp;

            _nya_nav_heap_push(grid, &open_length, neighbour, tentative + _nya_nav_heuristic((NYA_NavPoint){ nx, ny }, to, options.diagonal));
        }
    }

    if (!found) return 0;

    // Walk the parents back, then reverse. Counted first so a path longer than the caller's buffer is
    // refused rather than truncated — half a path leads somewhere nobody asked to go.
    u32 length  = 0;
    u32 walk    = target;
    for (;;) {
        length++;
        if (walk == start) break;
        walk = grid->came_from[walk];
    }

    if (length > capacity) return 0;

    walk = target;
    for (u32 i = 0; i < length; i++) {
        out_path[length - 1 - i] = (NYA_NavPoint){ .x = (s32)(walk % grid->width), .y = (s32)(walk / grid->width) };
        walk                     = grid->came_from[walk];
    }

    return length;
}

NYA_Error nya_nav_flow_create(NYA_Arena* arena, const NYA_NavGrid* grid, OUT NYA_NavFlow** out_flow) {
    if (arena == nullptr || grid == nullptr || out_flow == nullptr) return nya_error(NYA_ERROR_INVALID_ARGUMENT, "no arena, grid or out pointer");

    NYA_NavFlow* flow  = nya_arena_alloc(arena, sizeof(NYA_NavFlow));
    u64          cells = (u64)grid->width * (u64)grid->height;

    *flow = (NYA_NavFlow){
        .allocator = arena,
        .grid      = grid,
        .distance  = nya_arena_alloc(arena, cells * sizeof(u32)),
        .queue     = nya_arena_alloc(arena, cells * sizeof(u32)),
    };

    *out_flow = flow;

    return NYA_OK;
}

void nya_nav_flow_build(NYA_NavFlow* flow, NYA_NavPoint goal) {
    if (flow == nullptr) return;

    const NYA_NavGrid* grid  = flow->grid;
    const u32          cells = grid->width * grid->height;

    for (u32 i = 0; i < cells; i++) flow->distance[i] = NYA_NAV_UNREACHABLE;

    flow->goal  = goal;
    flow->built = false;

    if (!nya_nav_walkable(grid, goal.x, goal.y)) return;

    /*
     * A uniform-cost sweep outward from the goal.
     *
     * A plain FIFO rather than a priority queue, which is only correct because every step costs the
     * same *class* of amount — cell cost multiplies the step, so a weighted grid wants the heap version.
     * Re-relaxing a cell pushes it again, which is what keeps it correct at the cost of some churn.
     */
    u32 head = 0;
    u32 tail = 0;

    u32 goal_index          = _nya_nav_index(grid, goal.x, goal.y);
    flow->distance[goal_index] = 0;
    flow->queue[tail++]        = goal_index;

    const s32 offsets[8][2] = { { 1, 0 }, { -1, 0 }, { 0, 1 }, { 0, -1 }, { 1, 1 }, { 1, -1 }, { -1, 1 }, { -1, -1 } };

    while (head < tail) {
        u32 current = flow->queue[head++];

        s32 cx = (s32)(current % grid->width);
        s32 cy = (s32)(current / grid->width);

        for (u32 n = 0; n < 8; n++) {
            s32 nx = cx + offsets[n][0];
            s32 ny = cy + offsets[n][1];

            u8 cost = nya_nav_cost_at(grid, nx, ny);
            if (cost == NYA_NAV_BLOCKED) continue;

            b8 is_diagonal = offsets[n][0] != 0 && offsets[n][1] != 0;

            // Same corner rule as A*, so an agent following the field does not clip a wall the search
            // would have routed around.
            if (is_diagonal && !nya_nav_walkable(grid, nx, cy) && !nya_nav_walkable(grid, cx, ny)) continue;

            u32 step      = is_diagonal ? _NYA_NAV_STEP_DIAGONAL : _NYA_NAV_STEP_ORTHOGONAL;
            u32 neighbour = _nya_nav_index(grid, nx, ny);
            u32 tentative = flow->distance[current] + (step * cost);

            if (tentative >= flow->distance[neighbour]) continue;

            flow->distance[neighbour] = tentative;

            // Bounded by the queue's own size; a cell re-entering is why this is not a plain BFS.
            if (tail < cells) flow->queue[tail++] = neighbour;
            else {
                head = 0;
                tail = 0;
                flow->queue[tail++] = neighbour;
            }
        }
    }

    flow->built = true;
}

u32 nya_nav_flow_distance(const NYA_NavFlow* flow, s32 x, s32 y) {
    if (flow == nullptr || !flow->built || !_nya_nav_in_bounds(flow->grid, x, y)) return NYA_NAV_UNREACHABLE;

    return flow->distance[_nya_nav_index(flow->grid, x, y)];
}

NYA_NavPoint nya_nav_flow_step(const NYA_NavFlow* flow, NYA_NavPoint from) {
    if (flow == nullptr || !flow->built) return from;

    u32 here = nya_nav_flow_distance(flow, from.x, from.y);
    if (here == NYA_NAV_UNREACHABLE || here == 0) return from;

    const s32    offsets[8][2] = { { 1, 0 }, { -1, 0 }, { 0, 1 }, { 0, -1 }, { 1, 1 }, { 1, -1 }, { -1, 1 }, { -1, -1 } };
    NYA_NavPoint best          = from;
    u32          best_distance = here;

    for (u32 n = 0; n < 8; n++) {
        s32 nx = from.x + offsets[n][0];
        s32 ny = from.y + offsets[n][1];

        b8 is_diagonal = offsets[n][0] != 0 && offsets[n][1] != 0;
        if (is_diagonal && !nya_nav_walkable(flow->grid, nx, from.y) && !nya_nav_walkable(flow->grid, from.x, ny)) continue;

        u32 there = nya_nav_flow_distance(flow, nx, ny);
        if (there >= best_distance) continue;

        best_distance = there;
        best          = (NYA_NavPoint){ nx, ny };
    }

    return best;
}

f32x2 nya_nav_flow_direction(const NYA_NavFlow* flow, NYA_NavPoint from) {
    NYA_NavPoint next = nya_nav_flow_step(flow, from);

    f32x2 delta = { (f32)(next.x - from.x), (f32)(next.y - from.y) };
    if (delta.x == 0.0F && delta.y == 0.0F) return f32x2_zero;

    return nya_vector_normalize(delta);
}
