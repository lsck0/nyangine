/**
 * @file debug_physics.h
 *
 * The collision shapes, drawn over the scene they belong to.
 *
 * ```c
 * nya_render3d_begin(window, camera);
 * gny_draw_the_scene(window);
 * if (show_hitboxes) nya_debug_physics3d_draw(window);
 * nya_render3d_end(window);
 * ```
 *
 * What a body collides with is not what it looks like, and every physics bug that is really a shape bug
 * costs an afternoon until someone can see the two apart. These draw what the solver actually holds:
 * the shape, at the transform the solver last wrote, in a colour that says what kind of body it is.
 *
 * They draw and nothing else. There is no toggle here, because a toggle would be a second place to ask
 * and the caller already has the answer; call them or do not.
 *
 * Both take the shape from the body rather than from the entity, so a body whose shape disagrees with
 * the model drawn at the same place shows up as exactly that disagreement.
 *
 * ## Why this is in debug and not in physics
 *
 * Drawing needs the renderer, and the solvers do not: `physics.h` is included before `core.h` in
 * `nyangine.h`, so nothing under `physics/` can name an `NYA_Window`. Keeping the draw out here is what
 * lets a headless build, a test and a host tool link the solver without linking a renderer.
 * */
#pragma once

#include "nyangine/base/base_attributes.h"
#include "nyangine/base/base_types.h"
#include "nyangine/renderer/render_color.h"

typedef struct NYA_Window NYA_Window;

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * CONSTANTS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/**
 * Green for static, blue for kinematic, yellow for dynamic.
 *
 * The same three colours in both dimensions, so the habit carries over. A body that is asleep is drawn
 * at a third of the alpha, which is the one piece of solver state that explains "it stopped reacting"
 * and cannot be seen any other way.
 * */
#define NYA_DEBUG_PHYSICS_COLOR_STATIC    ((NYA_Color){ 0.20F, 0.90F, 0.35F, 0.85F })
#define NYA_DEBUG_PHYSICS_COLOR_KINEMATIC ((NYA_Color){ 0.30F, 0.60F, 1.00F, 0.85F })
#define NYA_DEBUG_PHYSICS_COLOR_DYNAMIC   ((NYA_Color){ 1.00F, 0.85F, 0.20F, 0.85F })

/** How much of the alpha a sleeping body keeps. */
#define NYA_DEBUG_PHYSICS_ASLEEP_ALPHA 0.33F

/** Segments in a debug circle. Twelve reads as round at the size a hitbox is looked at. */
#define NYA_DEBUG_PHYSICS_CIRCLE_SEGMENTS 12

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * FUNCTIONS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/**
 * Draws every 3D collision shape in the current world. Call between nya_render3d_begin and _end.
 *
 * Boxes, spheres and capsules are drawn as they are. A MESH or a HEIGHTFIELD is not: the triangles are
 * Box3D's copy and a terrain's outline is the terrain, which hides the scene rather than explaining it.
 *
 * Returns how many bodies it drew, which is not how many bodies there are: the shapes it skips are the
 * difference, and a HUD that prints both says so without a second walk of the entity table.
 * */
NYA_API u32 nya_debug_physics3d_draw(NYA_Window* window);

/**
 * Draws every 2D collision shape in the current world, in world units. Returns how many it drew.
 *
 * Boxes, circles and capsules are drawn as they are. A CHAIN is not, for the reason a heightfield is not.
 * */
NYA_API u32 nya_debug_physics2d_draw(NYA_Window* window);
