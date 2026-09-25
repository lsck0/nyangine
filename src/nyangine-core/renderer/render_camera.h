/**
 * @file render_camera.h
 * */
#pragma once

#include "nyangine-std/base/base_attributes.h"
#include "nyangine-std/base/base_basic.h"
#include "nyangine-std/base/base_types.h"
#include "nyangine-std/math/math_vector.h"

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
 * */
struct NYA_Camera2DTopDown {
    /** The world point at the centre of the target, not its corner. */
    f32x2 position;

    /** Pixels per world unit. Above one magnifies; the centre of the view stays put. */
    f32 zoom;

    /** Clockwise, in radians, about the centre of the view. */
    f32 rotation;
};

/**
 * The same flat world seen along a diagonal, so a square tile draws as a diamond.
 *
 * ```c
 * // A 2:1 diamond, the classic isometric ratio.
 * nya_render2d_camera_isometric_set(window, (NYA_Camera2DIsometric){
 *     .position = { player_tile_x, player_tile_y }, .zoom = 1.0F, .tile_width = 64, .tile_height = 32,
 * });
 * ```
 * */
struct NYA_Camera2DIsometric {
    /** The tile coordinate at the centre of the target. Fractional is fine. */
    f32x2 position;

    /** Scales the whole projection. One draws tiles at exactly `tile_width` by `tile_height`. */
    f32 zoom;

    /**
     * Full width and height of one tile's diamond, in pixels, before zoom.
     * */
    f32 tile_width;
    f32 tile_height;
};

/** Which of the two 2D projections a camera is. See NYA_Camera2D. */
enum NYA_Camera2DKind {
    /** No camera. Drawing lands in screen pixels, for UI. The default. */
    NYA_CAMERA2D_KIND_NONE = 0,

    NYA_CAMERA2D_KIND_TOP_DOWN,
    NYA_CAMERA2D_KIND_ISOMETRIC,
};

/**
 * Whichever 2D camera is currently set, tagged.
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
 * */
struct NYA_Camera3DPerspective {
    f32x3 position;

    /** The point the camera aims at. Must not equal `position`, which names no direction. */
    f32x3 target;

    /**
     * Which way is up for the camera, before it is made perpendicular to the view direction.
     * */
    f32x3 up;

    /** Vertical field of view in radians. Zero means 60 degrees. Above about 2.6 fisheyes. */
    f32 fov_y;

    /**
     * The depth range, in world units. Zero on either is read as 0.1 and 1000.
     * */
    f32 near_plane;
    f32 far_plane;
};

/**
 * A 3D camera without vanishing points: parallel lines stay parallel, size ignores distance.
 * */
struct NYA_Camera3DOrthographic {
    f32x3 position;
    f32x3 target;
    f32x3 up;

    /**
     * World units the view covers vertically. The horizontal extent follows the target's aspect.
     * */
    f32 height;

    f32 near_plane;
    f32 far_plane;
};


/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * FUNCTIONS AND MACROS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 *
 * The 2D camera arithmetic, shared by both renderers so the headless build cannot drift. Pure
 * functions of a camera and a target size. 3D cameras go through math_matrix.h.
 */

/**
 * The camera with the values that cannot mean anything replaced by the ones that can.
 * */
NYA_API NYA_Camera2DTopDown   nya_camera2d_top_down_sanitized(NYA_Camera2DTopDown camera) __attr_no_discard;
NYA_API NYA_Camera2DIsometric nya_camera2d_isometric_sanitized(NYA_Camera2DIsometric camera) __attr_no_discard;

/**
 * The top-down camera in `camera`, or the identity for a camera that is not one.
 * */
NYA_API NYA_Camera2DTopDown nya_camera2d_top_down_or_identity(NYA_Camera2D camera) __attr_no_discard;

/**
 * The 2x2 a camera applies to a world offset, before the view centre is added.
 * */
NYA_API void nya_camera2d_basis(const NYA_Camera2D* camera, OUT f32* out_a, OUT f32* out_b, OUT f32* out_c, OUT f32* out_d);

/** The world (or tile) point the camera centres on, whichever kind it is. */
NYA_API f32x2 nya_camera2d_position(const NYA_Camera2D* camera) __attr_no_discard;

/**
 * The two conversions between the target's pixels and the camera's world.
 * */
NYA_API f32x2 nya_camera2d_screen_to_world(const NYA_Camera2D* camera, f32x2 screen, u32 target_width, u32 target_height) __attr_no_discard;
NYA_API f32x2 nya_camera2d_world_to_screen(const NYA_Camera2D* camera, f32x2 world, u32 target_width, u32 target_height) __attr_no_discard;
