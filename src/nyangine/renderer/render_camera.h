/**
 * @file render_camera.h
 *
 * The four cameras, in their own file because both renderers need them and neither owns them.
 *
 * render2d.h needs the 2D pair and render3d.h needs the 3D pair, and renderer.h needs all four
 * because the batches hold them by value. Putting them in any one of those three makes the other two
 * include it, which is the cycle this file exists to cut.
 *
 * The dimension is in every name because nothing else can carry it. A camera is a projection, and a
 * projection is the one place where "2D" and "3D" stop being a stylistic label and become a
 * different number of axes with different units and a different idea of which way is up. A single
 * NYA_Camera holding a mode flag was the alternative, and it puts a perspective field of view and a
 * pixels-per-world-unit zoom in the same struct where exactly one of them means anything at a time.
 *
 * **They are used together, not instead of each other.** A 3D scene draws its world through an
 * NYA_Camera3DPerspective and then draws its HUD through render2d with no camera at all, in the same
 * frame and the same render pass — the identity 2D camera is screen pixels, which is what UI wants.
 * See render3d.h.
 * */
#pragma once

#include "nyangine/base/base_types.h"
#include "nyangine/math/math_vector.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * TYPES
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

typedef enum NYA_Camera2DKind           NYA_Camera2DKind;
typedef struct NYA_Camera2DTopDown      NYA_Camera2DTopDown;
typedef struct NYA_Camera2DIsometric    NYA_Camera2DIsometric;
typedef struct NYA_Camera2D             NYA_Camera2D;
typedef struct NYA_Camera3DPerspective  NYA_Camera3DPerspective;
typedef struct NYA_Camera3DOrthographic NYA_Camera3DOrthographic;

/**
 * Straight down at a flat world. The ordinary 2D camera.
 *
 * The identity — zero position, zoom one, no rotation — makes drawing land in screen pixels with the
 * origin at the top left, which is what everything did before there was a camera and what a UI layer
 * still wants. A game that never sets one is unaffected.
 * */
struct NYA_Camera2DTopDown {
    /** The world point that appears at the **centre** of the target, not at its corner. */
    f32x2 position;

    /** Pixels per world unit. Above one magnifies; the centre of the view stays put. */
    f32 zoom;

    /** Clockwise, in radians, about the centre of the view. */
    f32 rotation;
};

/**
 * The same flat world seen along a diagonal, so a square tile draws as a diamond.
 *
 * Coordinates handed to the draw calls are in **tile space** — whole numbers step one tile — and this
 * camera is what turns them into the staggered screen positions that read as depth. That is the
 * whole difference from the top-down camera, and it is why isometric is a camera rather than a
 * tilemap feature: an entity standing on the map has to be placed by the same projection the tiles
 * were, or it floats.
 *
 * ```c
 * // A 2:1 diamond, the classic isometric ratio.
 * nya_render2d_camera_isometric_set(window, (NYA_Camera2DIsometric){
 *     .position = { player_tile_x, player_tile_y }, .zoom = 1.0F, .tile_width = 64, .tile_height = 32,
 * });
 * ```
 *
 * There is no rotation. An isometric view is already a fixed rotation of the world, and turning it
 * further stops the tile art lining up with the grid — a game that wants four facings redraws the
 * tiles, it does not spin the camera.
 * */
struct NYA_Camera2DIsometric {
    /** The **tile** coordinate that appears at the centre of the target. Fractional is fine. */
    f32x2 position;

    /** Scales the whole projection. One draws tiles at exactly `tile_width` by `tile_height`. */
    f32 zoom;

    /**
     * Full width and height of one tile's diamond, in pixels, before zoom.
     *
     * Two numbers rather than one angle, because this is what the art is authored to: a 64x32 tile
     * sheet wants 64 and 32, and deriving them from a projection angle would mean matching a
     * transcendental function against whatever the artist actually drew. The usual ratio is 2:1.
     * */
    f32 tile_width;
    f32 tile_height;
};

/** Which of the two 2D projections a camera is. See NYA_Camera2D. */
enum NYA_Camera2DKind {
    /** No camera. Drawing lands in screen pixels — the UI case, and the default. */
    NYA_CAMERA2D_KIND_NONE = 0,

    NYA_CAMERA2D_KIND_TOP_DOWN,
    NYA_CAMERA2D_KIND_ISOMETRIC,
};

/**
 * Whichever 2D camera is currently set, tagged.
 *
 * What the batch stores and what nya_render2d_camera_get hands back. A tagged union rather than two
 * fields because exactly one is in effect at a time, and rather than two setters writing one struct
 * because the *reader* has to be able to tell which projection produced a given screen position.
 *
 * Both kinds collapse to the same thing before they reach the GPU — a 2x2 linear map plus a
 * translation — so the flush path and the screen/world inverse are one code path with two ways of
 * filling in four numbers. See _nya_render2d_camera_basis.
 * */
struct NYA_Camera2D {
    NYA_Camera2DKind kind;

    union {
        NYA_Camera2DTopDown   as_top_down;
        NYA_Camera2DIsometric as_isometric;
    };
};

/**
 * A 3D camera with vanishing points: things get smaller with distance.
 *
 * What a scene is normally looked at through. Positioned and aimed rather than rotated, because
 * "where is it and what is it looking at" is what a game actually knows — a follow camera has a
 * target, an orbit camera has a centre, and both would otherwise be computing a quaternion to hand
 * over and immediately having it turned back into the same two points.
 * */
struct NYA_Camera3DPerspective {
    f32x3 position;

    /** The point the camera aims at. Must not equal `position`, which names no direction. */
    f32x3 target;

    /**
     * Which way is up for the camera, before it is made perpendicular to the view direction.
     *
     * Positive y in a 3D scene, which is the opposite of the 2D world's y-down screen — see
     * physics3d.h, which makes the same choice for the same reason. Zero is read as positive y
     * so the common case needs no field.
     * */
    f32x3 up;

    /** Vertical field of view in **radians**. Zero is read as 60 degrees. Above ~2.6 fisheyes. */
    f32 fov_y;

    /**
     * The depth range, in world units. Zero on either is read as 0.1 and 1000.
     *
     * `near_plane` is the number that matters: depth precision is concentrated near the camera and pushing
     * this toward zero spends all of it there, which is what z-fighting on distant geometry actually
     * is. Raise it as far as the scene allows.
     * */
    f32 near_plane;
    f32 far_plane;
};

/**
 * A 3D camera without vanishing points: parallel lines stay parallel, size ignores distance.
 *
 * What a CAD view, a strategy game or a "2.5D" look wants. Note that this is *not* how you get an
 * isometric tile view — that is NYA_Camera2DIsometric, which works in tile coordinates and draws
 * sprites. This one renders real 3D geometry with an orthographic projection, which looks similar
 * and is a completely different pipeline.
 * */
struct NYA_Camera3DOrthographic {
    f32x3 position;
    f32x3 target;
    f32x3 up;

    /**
     * World units the view covers vertically. The horizontal extent follows the target's aspect.
     *
     * The orthographic counterpart of a field of view, and the reason there is no `fov_y` here: with
     * no vanishing point an angle says nothing, because the view is a box rather than a cone.
     * */
    f32 height;

    f32 near_plane;
    f32 far_plane;
};

