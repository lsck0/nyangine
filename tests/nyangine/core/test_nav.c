/**
 * Grid navigation: A*, the flow field, and the corner rule that keeps a path off the walls.
 *
 * The properties worth pinning are optimality on a known grid, that a blocked goal fails rather than
 * returning something plausible, that corner cutting is off unless asked for, and that A* and the flow
 * field agree — they are two implementations of the same question and a disagreement is a bug in one.
 **/

#include "nyangine/nyangine.c"
#include "nyangine/nyangine.h"

/** Blocks a vertical wall at `x` from y0 to y1 inclusive. */
static void wall(NYA_NavGrid* grid, s32 x, s32 y0, s32 y1) {
    for (s32 y = y0; y <= y1; y++) nya_nav_cost_set(grid, x, y, NYA_NAV_BLOCKED);
}

s32 main(void) {
    NYA_Arena* arena = nya_arena_create(.name = "test_nav");
    defer      nya_arena_destroy(arena);

    // ── A fresh grid is open, and bad sizes are refused.
    {
        NYA_NavGrid* grid = nullptr;
        NYA_EXPECT(nya_nav_grid_create(arena, 10, 8, &grid));

        nya_check(grid->width == 10 && grid->height == 8, "the grid should be the size asked for");
        nya_check(nya_nav_walkable(grid, 0, 0) && nya_nav_walkable(grid, 9, 7), "a fresh grid is open");
        nya_check(nya_nav_cost_at(grid, 5, 5) == NYA_NAV_COST_DEFAULT, "cells start at the default cost");

        // Off the grid is blocked rather than an assert, so a search walking off the edge is ordinary.
        nya_check(!nya_nav_walkable(grid, -1, 0), "off the left is blocked");
        nya_check(!nya_nav_walkable(grid, 10, 0), "off the right is blocked");
        nya_check(!nya_nav_walkable(grid, 0, 8), "off the bottom is blocked");

        nya_check(!nya_nav_grid_create(arena, 0, 8, &grid).ok, "a zero width is refused");
        nya_check(!nya_nav_grid_create(nullptr, 4, 4, &grid).ok, "a null arena is refused");
    }

    // ── A straight run on an open grid is the shortest possible.
    {
        NYA_NavGrid* grid = nullptr;
        NYA_EXPECT(nya_nav_grid_create(arena, 20, 20, &grid));

        NYA_NavPoint path[64];
        u32 length = nya_nav_path(grid, (NYA_NavPoint){ 0, 0 }, (NYA_NavPoint){ 5, 0 }, path, 64, (NYA_NavOptions){ 0 });

        nya_check(length == 6, "a straight run of five steps is six points, got %u", length);
        nya_check(path[0].x == 0 && path[0].y == 0, "the path starts where asked");
        nya_check(path[length - 1].x == 5 && path[length - 1].y == 0, "and ends where asked");

        // Four-neighbour: a diagonal of five is ten steps. Eight-neighbour: six points.
        u32 ortho = nya_nav_path(grid, (NYA_NavPoint){ 0, 0 }, (NYA_NavPoint){ 5, 5 }, path, 64, (NYA_NavOptions){ 0 });
        u32 diag  = nya_nav_path(grid, (NYA_NavPoint){ 0, 0 }, (NYA_NavPoint){ 5, 5 }, path, 64, (NYA_NavOptions){ .diagonal = true });

        nya_check(ortho == 11, "an orthogonal-only diagonal is eleven points, got %u", ortho);
        nya_check(diag == 6, "with diagonals it is six, got %u", diag);
    }

    // ── Every step of a returned path is adjacent to the last, and none is blocked.
    {
        NYA_NavGrid* grid = nullptr;
        NYA_EXPECT(nya_nav_grid_create(arena, 30, 20, &grid));
        wall(grid, 10, 0, 15);

        NYA_NavPoint path[256];
        u32 length = nya_nav_path(grid, (NYA_NavPoint){ 2, 2 }, (NYA_NavPoint){ 25, 2 }, path, 256, (NYA_NavOptions){ .diagonal = true });

        nya_check(length > 0, "there is a way around the wall");

        u32 bad_step = 0;
        u32 through  = 0;
        for (u32 i = 0; i < length; i++) {
            if (!nya_nav_walkable(grid, path[i].x, path[i].y)) through++;
            if (i == 0) continue;

            s32 dx = abs(path[i].x - path[i - 1].x);
            s32 dy = abs(path[i].y - path[i - 1].y);
            if (dx > 1 || dy > 1 || (dx == 0 && dy == 0)) bad_step++;
        }
        nya_check(through == 0, "%u path points were inside a wall", through);
        nya_check(bad_step == 0, "%u steps were not to an adjacent cell", bad_step);
    }

    // ── A sealed goal has no path, and neither does a blocked start or end.
    {
        NYA_NavGrid* grid = nullptr;
        NYA_EXPECT(nya_nav_grid_create(arena, 12, 12, &grid));

        // Wall the goal into its own corner.
        wall(grid, 9, 0, 9);
        for (s32 x = 9; x < 12; x++) nya_nav_cost_set(grid, x, 9, NYA_NAV_BLOCKED);

        NYA_NavPoint path[256];
        nya_check(nya_nav_path(grid, (NYA_NavPoint){ 0, 0 }, (NYA_NavPoint){ 11, 0 }, path, 256, (NYA_NavOptions){ .diagonal = true }) == 0,
                  "a sealed goal has no path");

        nya_nav_cost_set(grid, 3, 3, NYA_NAV_BLOCKED);
        nya_check(nya_nav_path(grid, (NYA_NavPoint){ 3, 3 }, (NYA_NavPoint){ 0, 0 }, path, 256, (NYA_NavOptions){ 0 }) == 0,
                  "a blocked start has no path");
        nya_check(nya_nav_path(grid, (NYA_NavPoint){ 0, 0 }, (NYA_NavPoint){ 3, 3 }, path, 256, (NYA_NavOptions){ 0 }) == 0,
                  "a blocked goal has no path");
        nya_check(nya_nav_path(grid, (NYA_NavPoint){ 0, 0 }, (NYA_NavPoint){ 99, 99 }, path, 256, (NYA_NavOptions){ 0 }) == 0,
                  "a goal off the grid has no path");
    }

    // ── Corner cutting is off by default. This is the rule that keeps a wide unit off the wall.
    {
        NYA_NavGrid* grid = nullptr;
        NYA_EXPECT(nya_nav_grid_create(arena, 5, 5, &grid));

        // An inside corner: (1,0) and (0,1) blocked, so (0,0)->(1,1) is a squeeze between them.
        nya_nav_cost_set(grid, 1, 0, NYA_NAV_BLOCKED);
        nya_nav_cost_set(grid, 0, 1, NYA_NAV_BLOCKED);

        NYA_NavPoint path[64];
        u32 refused = nya_nav_path(grid, (NYA_NavPoint){ 0, 0 }, (NYA_NavPoint){ 1, 1 }, path, 64,
                                   (NYA_NavOptions){ .diagonal = true });
        u32 allowed = nya_nav_path(grid, (NYA_NavPoint){ 0, 0 }, (NYA_NavPoint){ 1, 1 }, path, 64,
                                   (NYA_NavOptions){ .diagonal = true, .cut_corners = true });

        nya_check(refused == 0, "the diagonal squeeze should be refused by default, got a path of %u", refused);
        nya_check(allowed == 2, "and allowed when asked for, got %u", allowed);
    }

    // ── Cost is honoured: an expensive direct route loses to a cheap detour.
    {
        NYA_NavGrid* grid = nullptr;
        NYA_EXPECT(nya_nav_grid_create(arena, 9, 5, &grid));

        // A band of mud straight ahead on row 0.
        for (s32 x = 1; x < 8; x++) nya_nav_cost_set(grid, x, 0, 50);

        NYA_NavPoint path[64];
        u32 length = nya_nav_path(grid, (NYA_NavPoint){ 0, 0 }, (NYA_NavPoint){ 8, 0 }, path, 64, (NYA_NavOptions){ 0 });

        nya_check(length > 9, "it should detour around the mud rather than plough through, got %u points", length);

        u32 muddy = 0;
        for (u32 i = 0; i < length; i++) {
            if (nya_nav_cost_at(grid, path[i].x, path[i].y) == 50) muddy++;
        }
        nya_check(muddy < 3, "and touch the mud as little as possible, touched %u", muddy);
    }

    // ── A path longer than the buffer is refused, not truncated.
    {
        NYA_NavGrid* grid = nullptr;
        NYA_EXPECT(nya_nav_grid_create(arena, 40, 40, &grid));

        NYA_NavPoint small[4];
        nya_check(nya_nav_path(grid, (NYA_NavPoint){ 0, 0 }, (NYA_NavPoint){ 39, 39 }, small, 4, (NYA_NavOptions){ 0 }) == 0,
                  "a path that does not fit is refused rather than half-delivered");
    }

    // ── The search budget stops an expensive failure.
    {
        NYA_NavGrid* grid = nullptr;
        NYA_EXPECT(nya_nav_grid_create(arena, 60, 60, &grid));
        wall(grid, 30, 0, 59);

        NYA_NavPoint path[8192];
        nya_check(nya_nav_path(grid, (NYA_NavPoint){ 0, 0 }, (NYA_NavPoint){ 59, 59 }, path, 8192,
                               (NYA_NavOptions){ .max_nodes = 20 }) == 0,
                  "a tiny budget should give up rather than expand the grid");
    }

    // ── Repeated queries do not leak state into each other.
    {
        NYA_NavGrid* grid = nullptr;
        NYA_EXPECT(nya_nav_grid_create(arena, 25, 25, &grid));
        wall(grid, 12, 0, 20);

        NYA_NavPoint path[512];
        u32 first = nya_nav_path(grid, (NYA_NavPoint){ 1, 1 }, (NYA_NavPoint){ 20, 1 }, path, 512, (NYA_NavOptions){ 0 });

        for (u32 i = 0; i < 50; i++) {
            u32 again = nya_nav_path(grid, (NYA_NavPoint){ 1, 1 }, (NYA_NavPoint){ 20, 1 }, path, 512, (NYA_NavOptions){ 0 });
            if (again != first) {
                nya_check(false, "query %u disagreed with the first: %u vs %u", i, again, first);
                break;
            }
        }
        nya_check(first > 0, "the repeated query should have found something");
    }

    // ── The flow field agrees with A*: same reachability, and following it arrives.
    {
        NYA_NavGrid* grid = nullptr;
        NYA_EXPECT(nya_nav_grid_create(arena, 24, 18, &grid));
        wall(grid, 8, 0, 12);
        wall(grid, 16, 5, 17);

        NYA_NavFlow* flow = nullptr;
        NYA_EXPECT(nya_nav_flow_create(arena, grid, &flow));

        NYA_NavPoint goal = { 22, 16 };
        nya_nav_flow_build(flow, goal);

        nya_check(nya_nav_flow_distance(flow, goal.x, goal.y) == 0, "the goal is zero from itself");
        nya_check(nya_nav_flow_distance(flow, 8, 0) == NYA_NAV_UNREACHABLE, "a wall cell is unreachable");

        // Walking the field from a far corner must arrive, and must not loop forever.
        NYA_NavPoint at    = { 0, 0 };
        u32          steps = 0;
        while ((at.x != goal.x || at.y != goal.y) && steps < 4096) {
            NYA_NavPoint next = nya_nav_flow_step(flow, at);
            if (next.x == at.x && next.y == at.y) break;
            at = next;
            steps++;
        }
        nya_check(at.x == goal.x && at.y == goal.y, "following the field should arrive, stopped at (%d, %d)", at.x, at.y);

        // And A* agrees the same place is reachable.
        NYA_NavPoint path[2048];
        u32 length = nya_nav_path(grid, (NYA_NavPoint){ 0, 0 }, goal, path, 2048, (NYA_NavOptions){ .diagonal = true });
        nya_check(length > 0, "A* should agree the goal is reachable");

        f32x2 direction = nya_nav_flow_direction(flow, (NYA_NavPoint){ 0, 0 });
        nya_check(fabsf(nya_vector_length(direction) - 1.0F) < 0.001F, "a direction should be normalised");

        f32x2 at_goal = nya_nav_flow_direction(flow, goal);
        nya_check(at_goal.x == 0.0F && at_goal.y == 0.0F, "there is nowhere to go from the goal");
    }

    // ── A goal inside a wall builds nothing rather than a misleading field.
    {
        NYA_NavGrid* grid = nullptr;
        NYA_EXPECT(nya_nav_grid_create(arena, 10, 10, &grid));
        nya_nav_cost_set(grid, 5, 5, NYA_NAV_BLOCKED);

        NYA_NavFlow* flow = nullptr;
        NYA_EXPECT(nya_nav_flow_create(arena, grid, &flow));
        nya_nav_flow_build(flow, (NYA_NavPoint){ 5, 5 });

        nya_check(nya_nav_flow_distance(flow, 0, 0) == NYA_NAV_UNREACHABLE, "a blocked goal reaches nothing");

        NYA_NavPoint stuck = nya_nav_flow_step(flow, (NYA_NavPoint){ 0, 0 });
        nya_check(stuck.x == 0 && stuck.y == 0, "and stepping goes nowhere rather than off the grid");
    }

    // ── nya_nav_fill flips the whole grid.
    {
        NYA_NavGrid* grid = nullptr;
        NYA_EXPECT(nya_nav_grid_create(arena, 6, 6, &grid));

        nya_nav_fill(grid, NYA_NAV_BLOCKED);
        nya_check(!nya_nav_walkable(grid, 3, 3), "fill should block everything");

        nya_nav_fill(grid, NYA_NAV_COST_DEFAULT);
        nya_check(nya_nav_walkable(grid, 3, 3), "and open it again");
    }

    return nya_check_failures() == 0 ? 0 : 1;
}
