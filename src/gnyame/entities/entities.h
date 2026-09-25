/**
 * @file entities.h
 *
 * ```c
 * NYA_EntityHandle crate = gny_entity_box_create(point, GNY_ENTITY_FLAG_CULL_WHEN_LOST | GNY_ENTITY_FLAG_SELECTABLE);
 *
 * nya_entity_foreach (entity) {
 *     if (!gny_entity_is(entity, GNY_ENTITY_BOX)) continue;
 *     ...
 * }
 * ```
 * */
#pragma once

// NYA_Rectf, which a camera viewport is, comes from nyangine/math/math_shapes.h via this umbrella.
#include "nyangine-core/nyangine.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * TYPES
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

typedef enum GNY_EntityKind GNY_EntityKind;

/*
 * Two lines, and they have to be in this order.
 */
enum GNY_EntityFlags : u64;
typedef enum GNY_EntityFlags GNY_EntityFlags;

/** What a thing *is*. Exactly one per entity, and the reason it has the fields it has. */
// @reflect
enum GNY_EntityKind {
    GNY_ENTITY_NONE = 0,

    /** The one static chain body every crate lands on. See gny_terrain_generate. */
    GNY_ENTITY_TERRAIN,

    /** A dynamic box. Spawned by clicking, despawned by falling out of the world. */
    GNY_ENTITY_BOX,

    /** Where the world is looked at from, and heard from. Exactly one. See entity_camera.c. */
    GNY_ENTITY_CAMERA,

    /** Anything in the 3D demo: the cube and the ground it lands on. See layer_cube3d.c. */
    GNY_ENTITY_CUBE3D,

    /** A static box built from a tilemap's collision layer. See nya_tilemap_collision_build. */
    GNY_ENTITY_TILEMAP,

    /**
     * A one-way platform, and the marker riding on the moving one.
     * */
    GNY_ENTITY_LEDGE,

    /**
     * A networked player, one per connected peer. See gny_net_spawn_player.
     * */
    GNY_ENTITY_PLAYER,

    /** A learning drone. Moved by the robots system, which owns its body. See robots.h. */
    GNY_ENTITY_ROBOT,

    GNY_ENTITY_KIND_COUNT,
};

/**
 * What is *true* of a thing, independent of what it is.
 * */
// @reflect
enum GNY_EntityFlags : u64 {
    GNY_ENTITY_FLAG_NONE = 0,

    /**
     * Despawns itself once it falls past GNY_WORLD_KILL_Y.
     * */
    GNY_ENTITY_FLAG_CULL_WHEN_LOST = 1ULL << 0,

    /**
     * Plays a sound when it is struck hard enough. Read by the crate's on_collision.
     * */
    GNY_ENTITY_FLAG_AUDIBLE = 1ULL << 1,

    /**
     * Reads the keyboard in its own update and moves itself.
     * */
    GNY_ENTITY_FLAG_PLAYER_CONTROLLED = 1ULL << 2,

    /**
     * The camera follows this entity, easing toward it every tick.
     * */
    GNY_ENTITY_FLAG_CAMERA_TARGET = 1ULL << 3,

    /**
     * The camera whose view fills the window. Exactly one, enforced by gny_entity_camera_primary_set.
     * */
    GNY_ENTITY_FLAG_CAMERA_PRIMARY = 1ULL << 4,
};

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * KIND AND FLAGS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

static_assert(sizeof(GNY_EntityFlags) == sizeof(u64), "GNY_EntityFlags must fit NYA_Entity.user_flags exactly");

/** GNY_ENTITY_NONE for a null entity, so a failed lookup reads as "nothing" rather than faulting. */
GNY_EntityKind gny_entity_kind(const NYA_Entity* entity);

GNY_EntityFlags gny_entity_flags(const NYA_Entity* entity);

/** Reads better than a comparison at a call site, and is null safe for the same reason. */
b8 gny_entity_is(const NYA_Entity* entity, GNY_EntityKind kind);

/** True when every bit in `flags` is set. GNY_ENTITY_FLAG_NONE is true for any entity. */
b8 gny_entity_flag_check(const NYA_Entity* entity, GNY_EntityFlags flags);

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * BOX
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/** What a crate is spawned with unless a caller wants otherwise: it falls out of the world, and it is heard landing. */
#define GNY_ENTITY_BOX_DEFAULT_FLAGS (GNY_ENTITY_FLAG_CULL_WHEN_LOST | GNY_ENTITY_FLAG_AUDIBLE)

/**
 * Spawns a crate at `position` and attaches its rigid body.
 * */
NYA_EntityHandle gny_entity_box_create(f32x2 position, GNY_EntityFlags flags);

/**
 * Removes one crate, at the next simulation barrier.
 * */
void gny_entity_box_destroy(NYA_EntityHandle box);

/** Removes every crate and leaves the terrain. Deferred, for the reason above. */
void gny_entity_box_destroy_all(void);

/** How many crates are in the world, and how many of those the solver still has awake. */
/** How many crates exist and, through `out_awake` (may be null), how many the solver considers awake. */
u32 gny_entity_box_count(OUT u32* out_awake);

/**
 * Draws one crate, in world coordinates. Registered as the entity's on_render.
 * */
void gny_entity_box_on_render(NYA_Entity* entity, NYA_Window* window);

/**
 * Turns a hard enough landing into a sound. Registered as the entity's on_collision.
 * */
void gny_entity_box_on_collision(NYA_Entity* entity, NYA_Entity* other, const NYA_PhysicsHit* hit);

/** Removes the crate that was clicked. Registered as the entity's on_click. */
void gny_entity_box_on_click(NYA_Entity* entity, f32x3 world_point, u8 button);

/** The colour a crate draws in, derived from its slot so it is stable for the entity's whole life. */
NYA_Color gny_entity_box_color(const NYA_Entity* entity);

/** Per crate update. Registered with nya_callback, so it is resolved by name after a hot reload. */
void gny_entity_box_on_update(NYA_Entity* entity, f32 delta_time_s);

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * CAMERA
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

typedef struct GNY_CameraView GNY_CameraView;

/**
 * What a camera renders into, and what it is watching.
 * */
struct GNY_CameraView {
    /**
     * The offscreen target, or a zeroed texture for the primary camera.
     *
     * Recreated when `viewport` changes size, since a GPU texture cannot be resized.
     * */
    NYA_RenderTexture target;

    /** Where the target is drawn on screen, in window pixels. Ignored for the primary camera. */
    NYA_Rectf viewport;

    /**
     * The entity this camera eases toward, or NYA_ENTITY_HANDLE_NONE.
     * */
    NYA_EntityHandle follow;
};

/**
 * Spawns a camera that fills the window, and makes it the primary one.
 * */
NYA_EntityHandle gny_entity_camera_create(f32x2 position, f32 zoom);

/**
 * Spawns a secondary camera that renders into its own texture and is drawn inside `viewport`.
 * */
NYA_EntityHandle gny_entity_camera_create_view(f32x2 position, f32 zoom, NYA_Rectf viewport);

/** The camera as the renderer wants it, for a specific camera entity. */
NYA_Camera2DTopDown gny_entity_camera_of(const NYA_Entity* entity);

/** The camera's view, or null for a camera that somehow has none. */
GNY_CameraView* gny_entity_camera_view(const NYA_Entity* entity);

/** The primary camera, or NYA_ENTITY_HANDLE_NONE before one exists. */
NYA_EntityHandle gny_entity_camera_primary(void);

/** Makes one camera primary and clears the flag from every other. */
void gny_entity_camera_primary_set(NYA_EntityHandle camera);

/** Pans on held keys and moves the audio listener. Registered as the camera's on_update. */
void gny_entity_camera_on_update(NYA_Entity* entity, f32 delta_time_s);

/**
 * The camera as the renderer wants it.
 * */
NYA_Camera2DTopDown gny_entity_camera_get(void);

/** Multiplies the zoom, clamped. What the mouse wheel drives; the wheel is an event, not a held key. */
void gny_entity_camera_zoom_by(f32 factor);

/**
 * Points a camera at an entity. Pass NYA_ENTITY_HANDLE_NONE for `target` to stop following.
 * */
void gny_entity_camera_follow(NYA_EntityHandle camera, NYA_EntityHandle target);

/*
 * ─────────────────────────────────────────────────────────
 * LEDGE
 * ─────────────────────────────────────────────────────────
 */

/**
 * A one-way platform: crates land on it from above and rise through it from below.
 * */
NYA_EntityHandle gny_entity_ledge_create(f32x2 position, f32x2 size, f32 patrol_distance);

/** Removes every ledge, and with it every marker parented to one. Deferred to the barrier. */
void gny_entity_ledge_destroy_all(void);

/**
 * Opens a drop-through window on every crate, so anything resting on a ledge falls off it.
 * */
u32 gny_entity_ledge_drop_everything_through(f32 seconds);

void gny_entity_ledge_on_render(NYA_Entity* entity, NYA_Window* window);
void gny_entity_ledge_marker_on_update(NYA_Entity* entity, f32 delta_time_s);
void gny_entity_ledge_marker_on_animation(NYA_Entity* entity, NYA_SpriteAnimationSignal signal);

/*
 * ─────────────────────────────────────────────────────────
 * ROBOT
 * ─────────────────────────────────────────────────────────
 */

/** Spawns a drone. It has no body and no update: gny_robots_update moves it. */
NYA_EntityHandle gny_entity_robot_create(f32x2 position);

/** Draws a drone pointing where it flies, and a line to where it is flying to. */
void gny_entity_robot_on_render(NYA_Entity* entity, NYA_Window* window);

/** What `camera` is watching, or NYA_ENTITY_HANDLE_NONE. */
NYA_EntityHandle gny_entity_camera_target(NYA_EntityHandle camera);
