/**
 * @file entities.h
 *
 * What the things in this game are, and what can be said about them.
 *
 * The engine deliberately knows nothing about either: NYA_Entity carries a `u32 type` it never
 * interprets and a `void* user_data` it never looks inside. This file is the game's half of that
 * contract — one enum naming the kinds, one naming the flags, and a create/destroy pair per kind in
 * its own translation unit beside this one.
 *
 * ```c
 * NYA_EntityHandle crate = gny_entity_box_create(point, GNY_ENTITY_FLAG_CULL_WHEN_LOST | GNY_ENTITY_FLAG_SELECTABLE);
 *
 * nya_entity_foreach (entity) {
 *     if (!gny_entity_is(entity, GNY_ENTITY_BOX)) continue;
 *     ...
 * }
 * ```
 *
 * ## Where these live on an entity
 *
 * The kind goes in `NYA_Entity.type` and the flags in `NYA_Entity.user_flags`, which the engine
 * provides as a game-defined `u32` and `u64` and never interprets. Two fields rather than one packed
 * field: a flag word runs out long before a kind enum does, and packing both would make every
 * `entity->type == GNY_ENTITY_BOX` comparison silently false the day the first flag was set.
 *
 * Plain integers rather than a struct behind `user_data`, because crates here are spawned and
 * despawned by the hundred out of an arena that never frees an individual block — an allocation per
 * entity would be a leak shaped like a design.
 * */
#pragma once

// NYA_Rectf, which a camera viewport is, comes from nyangine/math/math_shapes.h via this umbrella.
#include "nyangine/nyangine.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * TYPES
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

typedef enum GNY_EntityKind GNY_EntityKind;

/*
 * Two lines, and they have to be in this order.
 *
 * The underlying type belongs on the forward declaration as well as on the definition — C treats a
 * fixed-underlying-type enum and a plain one as conflicting declarations of the same tag. But an
 * opaque enum with a fixed type is only allowed as a *standalone* declaration, so it cannot be
 * folded into the typedef the way GNY_EntityKind's is.
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
     *
     * Both under one kind because the marker has no behaviour of its own — it is a child transform
     * with an on_render, and giving it its own kind would mean a bitset in the entity index for a
     * thing there is one of.
     * */
    GNY_ENTITY_LEDGE,

    /**
     * A networked player, one per connected peer. See gny_net_spawn_player.
     *
     * Its own kind rather than a crate that happens to be driven, because a player has **no physics
     * body** — it is predicted, and the solver would overwrite the prediction every step. Everything
     * keyed on GNY_ENTITY_BOX reads a body without asking (gny_entity_box_count and the crate's
     * on_render both read the sleep state), so a player wearing the crate's kind logged an error per
     * player per frame and would have kept doing it silently in a release build.
     * */
    GNY_ENTITY_PLAYER,

    GNY_ENTITY_KIND_COUNT,
};

/**
 * What is *true* of a thing, independent of what it is.
 *
 * Deliberately behavioural rather than descriptive — each of these is read somewhere and changes
 * what happens. A flag nothing branches on is a comment with extra steps, so this enum stays short.
 * */
// @reflect
enum GNY_EntityFlags : u64 {
    GNY_ENTITY_FLAG_NONE = 0,

    /**
     * Despawns itself once it falls past GNY_WORLD_KILL_Y.
     *
     * The terrain is finite, so anything that misses its ends keeps falling forever: never sleeping,
     * because it never stops accelerating, and costing a solver step for the rest of the session.
     * Carried as a flag rather than checked per kind so a future kind opts in by saying so.
     * */
    GNY_ENTITY_FLAG_CULL_WHEN_LOST = 1ULL << 0,

    /**
     * Plays a sound when it is struck hard enough. Read by the crate's on_collision.
     *
     * A flag rather than something every crate does, so a burst spawned for a stress test can be
     * silent while the ones dropped by hand are not.
     * */
    GNY_ENTITY_FLAG_AUDIBLE = 1ULL << 1,

    /**
     * Reads the keyboard in its own update and moves itself.
     *
     * On the camera today, which is what makes the arrow keys pan. It is a flag rather than
     * something the camera simply does, because "what the player drives" is a property that moves:
     * hand it to a crate and the crate is what the keys push, with nothing about the camera changing.
     *
     * Nothing enforces that only one entity has it. Two would both read the same keys and both move,
     * which is a coherent thing to want and a silly default — so it is the game's business, not a
     * rule here.
     * */
    GNY_ENTITY_FLAG_PLAYER_CONTROLLED = 1ULL << 2,

    /**
     * The camera follows this entity, easing toward it every tick.
     *
     * Set on the *followed* thing rather than stored on the camera, so "what is being watched" is
     * answerable by looking at the world rather than by asking the camera — and so an entity that
     * despawns takes the follow with it instead of leaving a dangling handle behind.
     *
     * gny_entity_camera_follow keeps it to one entity, since a camera cannot be in two places. The
     * flag alone does not: the first entity found carrying it wins, and the rest are ignored.
     * */
    GNY_ENTITY_FLAG_CAMERA_TARGET = 1ULL << 3,

    /**
     * The camera whose view fills the window. Exactly one, enforced by gny_entity_camera_primary_set.
     *
     * Every other camera renders into its own texture and is composited somewhere inside the frame —
     * a minimap, a rear view, a window onto something happening elsewhere. Which one is primary is a
     * property of the camera rather than a field on the world, so swapping views is setting a flag.
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

/** True when **every** bit in `flags` is set. Passing GNY_ENTITY_FLAG_NONE is true for any entity. */
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
 *
 * Size and initial spin come from the world's spawn counter through nya_ihash2, so the same sequence
 * of clicks builds the same pile — which is what makes a physics bug reproducible rather than a story
 * about something that happened once.
 *
 * NYA_ENTITY_HANDLE_NONE when the entity table is full or the body could not be built; both are
 * logged, and both leave nothing behind.
 * */
NYA_EntityHandle gny_entity_box_create(f32x2 position, GNY_EntityFlags flags);

/**
 * Removes one crate, at the next simulation barrier.
 *
 * Deferred, because the callers are an entity's own update and a click handled mid frame — and
 * despawning immediately from either is what nya_entity_foreach is not safe under. Harmless on a
 * handle that no longer resolves, and on one that is not a box.
 * */
void gny_entity_box_destroy(NYA_EntityHandle box);

/** Removes every crate and leaves the terrain. Deferred, for the reason above. */
void gny_entity_box_destroy_all(void);

/** How many crates are in the world, and how many of those the solver still has awake. */
u32 gny_entity_box_count(OUT u32* out_awake);

/**
 * Draws one crate, in world coordinates. Registered as the entity's on_render.
 *
 * Never called directly — nya_system_entity_render walks the table and checks visibility, and the
 * game layer calls that once from inside the camera it has set. Exported by name for the same reason
 * the update is: hot reload re-resolves it with dlsym.
 * */
void gny_entity_box_on_render(NYA_Entity* entity, NYA_Window* window);

/**
 * Turns a hard enough landing into a sound. Registered as the entity's on_collision.
 *
 * Both sides of a hit are called, so two crates striking each other would play twice — this acts
 * only for the lower of the two handle indices, which is the general answer to "once per collision".
 *
 * Exported by name, like the other hooks: hot reload re-resolves it with dlsym.
 * */
void gny_entity_box_on_collision(NYA_Entity* entity, NYA_Entity* other, const NYA_PhysicsHit* hit);

/** Removes the crate that was clicked. Registered as the entity's on_click. */
void gny_entity_box_on_click(NYA_Entity* entity, f32x3 world_point, u8 button);

/** The colour a crate draws in, derived from its slot so it is stable for the entity's whole life. */
NYA_Color gny_entity_box_color(const NYA_Entity* entity);

/**
 * Per crate update. Registered with nya_callback, so it is resolved **by name** after a hot reload.
 *
 * That is why it is declared here and defined without NYA_INTERNAL: a static or hidden symbol is not
 * in the dynamic symbol table, and update_callback_pointers in main.c would fail to find it on the
 * first reload. See the note in core_asset.c about the same trap.
 * */
void gny_entity_box_on_update(NYA_Entity* entity, f32 delta_time_s);

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * CAMERA
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

typedef struct GNY_CameraView GNY_CameraView;

/**
 * What a camera renders into, and what it is watching.
 *
 * Hung off the camera entity's `user_data` rather than packed into the transform like its position
 * and zoom, because none of it is a transform — and unlike crates there are a handful of cameras, so
 * one allocation each out of the world arena costs nothing.
 *
 * The primary camera has no `target`: it draws straight into the window. Every other camera has one,
 * and is composited into `viewport` afterwards.
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
     *
     * Per camera rather than a single global, which is the whole reason it is a handle here and not
     * only a flag on the followed entity: with more than one camera, "what is being watched" has as
     * many answers as there are cameras, and a flag cannot say which camera means which.
     *
     * GNY_ENTITY_FLAG_CAMERA_TARGET is still set on whatever is followed, as the cheap "is anything
     * watching this" answer. gny_entity_camera_follow is the only thing that writes either, so the
     * two cannot drift.
     * */
    NYA_EntityHandle follow;
};

/**
 * Spawns a camera that fills the window, and makes it the primary one.
 *
 * Not visible, because there is nothing to draw — leaving the bit set would only put it through the
 * render walk every frame. Its position and zoom are its transform; see entity_camera.c.
 * */
NYA_EntityHandle gny_entity_camera_create(f32x2 position, f32 zoom);

/**
 * Spawns a secondary camera that renders into its own texture and is drawn inside `viewport`.
 *
 * The picture-in-picture case: a view onto somewhere else in the world, composited over the primary
 * one. Give it something to follow and it becomes a camera pointed at that thing.
 *
 * The target is created on the first frame it is rendered, at the viewport's size, and recreated
 * whenever that size changes.
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
 *
 * The identity camera when there is none, which is the main menu — so a layer that draws before the
 * game exists gets screen space rather than a divide by a zoom of zero.
 * */
NYA_Camera2DTopDown gny_entity_camera_get(void);

/** Multiplies the zoom, clamped. What the mouse wheel drives; the wheel is an event, not a held key. */
void gny_entity_camera_zoom_by(f32 factor);

/**
 * Points a camera at an entity. Pass NYA_ENTITY_HANDLE_NONE for `target` to stop following.
 *
 * Writes both the camera's own `follow` handle and GNY_ENTITY_FLAG_CAMERA_TARGET on the entity, and
 * is the only thing that writes either — which is what keeps the flag honest without it having to be
 * recomputed.
 *
 * Following overrides panning for that camera while it lasts: being chased and being driven by the
 * arrow keys are two different intentions, and doing both means neither works.
 * */
void gny_entity_camera_follow(NYA_EntityHandle camera, NYA_EntityHandle target);

/*
 * ─────────────────────────────────────────────────────────
 * LEDGE
 * ─────────────────────────────────────────────────────────
 */

/**
 * A one-way platform: crates land on it from above and rise through it from below.
 *
 * A positive `patrol_distance` makes it a **kinematic** platform that slides that far to the right and
 * back forever, carrying whatever is standing on it, with a marker parented to it that rides along.
 * Zero makes it a static ledge. See entity_ledge.c for which engine features that exercises.
 * */
NYA_EntityHandle gny_entity_ledge_create(f32x2 position, f32x2 size, f32 patrol_distance);

/** Removes every ledge, and with it every marker parented to one. Deferred to the barrier. */
void gny_entity_ledge_destroy_all(void);

/**
 * Opens a drop-through window on every crate, so anything resting on a ledge falls off it.
 *
 * Returns how many were given one. See nya_physics2d_drop_through for why the window is a duration
 * rather than a single frame.
 * */
u32 gny_entity_ledge_drop_everything_through(f32 seconds);

void gny_entity_ledge_on_render(NYA_Entity* entity, NYA_Window* window);
void gny_entity_ledge_marker_on_render(NYA_Entity* entity, NYA_Window* window);

/** What `camera` is watching, or NYA_ENTITY_HANDLE_NONE. */
NYA_EntityHandle gny_entity_camera_target(NYA_EntityHandle camera);
