/**
 * @file core_entity.h
 *
 * ```c
 * NYA_EntityHandle player = nya_entity_spawn(.name = "player", .position = { 0, 1, 0 });
 *
 * NYA_Entity* entity = nya_entity_get(player);   // null once it is despawned
 * if (entity) entity->velocity.y -= 9.81F * delta_time_s;
 *
 * nya_entity_despawn_deferred(player);           // applied at the simulation barrier
 * ```
 * */
#pragma once

#include "nyangine/core/core_callback.h"
#include "nyangine/core/core_event.h"
#include "nyangine/physics/physics2d.h"
#include "nyangine/physics/physics3d.h"
#include "nyangine/core/core_tween.h"
#include "nyangine/core/core_types.h"
#include "nyangine/math/math_matrix.h"
#include "nyangine/math/math_quaternion.h"
#include "nyangine/math/math_tween.h"
#include "nyangine/math/math_vector.h"
// the entity carries its appearance by value.
#include "nyangine/renderer/render2d_sprite.h"

// pointer only, to avoid including core_window.h.
typedef struct NYA_Window NYA_Window;

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * CONSTANTS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/**
 * Entity slots, reserved up front and never moved, so an NYA_Entity* stays valid: on_update holds a raw
 * pointer, and a spawn that grew the table would leave it dangling.
 * */
#ifndef NYA_ENTITY_MAX
#define NYA_ENTITY_MAX 8192
#endif

/**
 * Slots committed at a time as spawning reaches past the committed part of the table. A world with a few
 * hundred entities then holds a few hundred kilobytes rather than the whole reservation.
 * */
#ifndef NYA_ENTITY_COMMIT_SLOTS
#define NYA_ENTITY_COMMIT_SLOTS 256
#endif

/**
 * World units across one cell of the spatial index. Too small and queries walk empty cells; too large and
 * the index degrades to a linear scan. A few times a typical entity's size works well.
 * */
#ifndef NYA_ENTITY_GRID_CELL_SIZE
#define NYA_ENTITY_GRID_CELL_SIZE 128.0F
#endif

/**
 * Hash buckets; a power of two since the hash is masked. A hash rather than a dense grid, so the world is
 * unbounded; distant cells can share a bucket, so queries recheck the actual cell.
 * */
#ifndef NYA_ENTITY_GRID_BUCKETS
#define NYA_ENTITY_GRID_BUCKETS 4096
#endif

static_assert((NYA_ENTITY_GRID_BUCKETS & (NYA_ENTITY_GRID_BUCKETS - 1)) == 0, "NYA_ENTITY_GRID_BUCKETS must be a power of two");

/**
 * How far outside the view nya_system_entity_render still draws, in world units. The index stores origins,
 * not sizes, so without a margin large entities pop at the screen edge. A little more than the largest
 * entity's radius.
 * */
#ifndef NYA_ENTITY_RENDER_CULL_MARGIN
#define NYA_ENTITY_RENDER_CULL_MARGIN 128.0F
#endif

/**
 * Kinds with an index bitset. Higher kinds still work without the shortcut. Each bitset is NYA_ENTITY_MAX
 * bits, a kilobyte at the default.
 * */
#ifndef NYA_ENTITY_KIND_MAX
#define NYA_ENTITY_KIND_MAX 64
#endif

/** Bits in NYA_Entity.flags, and so bitsets in the index. */
#define NYA_ENTITY_FLAG_COUNT 64

/** 64-bit words needed to cover every slot. */
#define NYA_ENTITY_BITSET_WORDS ((NYA_ENTITY_MAX + 63) / 64)

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * TYPES
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

typedef struct NYA_EntityIndex          NYA_EntityIndex;
typedef struct NYA_EntityIter           NYA_EntityIter;
typedef struct NYA_EntityGrid           NYA_EntityGrid;
typedef enum NYA_EntityState           NYA_EntityState;
typedef struct NYA_Entity             NYA_Entity;
typedef struct NYA_EntitySystem       NYA_EntitySystem;
typedef struct NYA_EntitySpawnOptions NYA_EntitySpawnOptions;
typedef enum NYA_EntityVisualKind      NYA_EntityVisualKind;
typedef struct NYA_EntityVisual        NYA_EntityVisual;

// @reflect
enum NYA_EntityState {
    NYA_ENTITY_STATE_NONE = 0,

    /** Cleared to leave an entity in the world but skip its update. */
    NYA_ENTITY_STATE_ACTIVE = 1 << 0,

    /** Cleared to keep simulating an entity that is not drawn. */
    NYA_ENTITY_STATE_VISIBLE = 1 << 1,

    /** Never moves. A hint for spatial partitioning. */
    NYA_ENTITY_STATE_STATIC = 1 << 2,

    /**
     * Despawn requested, waiting for the simulation barrier. The entity stays valid until then; this flag is
     * how code mid-update can tell.
     * */
    NYA_ENTITY_STATE_DESPAWNING = 1 << 3,
};

/**
 * A uniform grid over entity positions, rebuilt every tick. Intrusive: `buckets` holds each bucket's first
 * slot and `next` chains the rest, so insert is two writes and clearing is one memset. That is cheaper than
 * incremental updates when everything moves.
 * */
struct NYA_EntityGrid {
    /** First entity slot in each bucket, or NYA_ENTITY_GRID_EMPTY. */
    u32* buckets;

    /** Next entity slot in the same bucket, indexed by slot. NYA_ENTITY_GRID_EMPTY ends the chain. */
    u32* next;

    f32 cell_size;

    /** Entities indexed by the last rebuild. */
    u32 count;
};

/**
 * Bitsets of which slots hold which kind and flags, so "every camera" costs the number of cameras, not the
 * number of entities. A zero word skips 64 slots.
 * */
struct NYA_EntityIndex {
    /** Occupied slots. Every query is masked by this, so a freed slot is never returned. */
    u64* live;

    /** One bitset per kind, indexed by `type`, below NYA_ENTITY_KIND_MAX. */
    u64* kinds;

    /** One bitset per flag bit. A multi-bit query walks one bit's set and checks the rest. */
    u64* flags;
};

/**
 * Walks the entities matching a query. Built by the nya_entity_foreach_* macros. `entity` is the current
 * match, null when done.
 * */
struct NYA_EntityIter {
    /** The bitset being walked: a kind's, a flag's, or `live`. */
    const u64* bits;

    /** Every one of these must be set on the entity. Zero matches everything. */
    u64 require_flags;

    u32 require_type;
    b8  check_type;

    u32 word;

    /** Words worth scanning, up to the table's high water mark. Beyond it every word is zero. */
    u32 word_count;

    /** Bits of the current word not yet visited, lowest first. */
    u64 remaining;

    NYA_Entity* entity;
};

/*
 * ─────────────────────────────────────────────────────────
 * SYSTEM STRUCT
 * ─────────────────────────────────────────────────────────
 */

struct NYA_EntitySystem {
    NYA_Arena* allocator;

    /** NYA_ENTITY_MAX slots of address space, committed from the front. See NYA_ENTITY_COMMIT_SLOTS. */
    NYA_Entity* entities;
    b8*         occupied;
    u32*        generations;

    /** Slots backed by memory. Never shrinks, so pointers into the table stay valid. */
    u32 committed_slots;

    /** Slots handed out at least once. Past this the table was never touched, and spawning takes the next. */
    u32 touched_slots;

    /**
     * Despawned slots, most recently freed first. A stack, since scanning for an empty slot makes mass spawning
     * quadratic.
     * */
    u32* free_slots;
    u32  free_count;

    u32 count;

    /** Highest slot ever occupied. Iteration stops here rather than at NYA_ENTITY_MAX. */
    u32 high_water_mark;

    NYA_EntityGrid  grid;
    NYA_EntityIndex index;

    /** Entities that currently have a parent. */
    u32 parented_count;

    /**
     * Who the cursor is on, or NYA_ENTITY_HANDLE_NONE. Kept here because firing "left" needs the previous
     * hover, which a per-entity bit cannot give without a scan.
     * */
    NYA_EntityHandle hovered;
};

/*
 * ─────────────────────────────────────────────────────────
 * APPEARANCE
 * ─────────────────────────────────────────────────────────
 *
 * What an entity looks like, so most entities need no on_render. Knowing every appearance lets the
 * system sort by depth and texture before drawing, which gives stable order and batching. See
 * nya_system_entity_render_in.
 *
 * on_render still runs after the visual, for health bars, debug outlines and anything no enum covers.
 */

// @reflect
enum NYA_EntityVisualKind {
    /** Nothing drawn automatically; on_render is the whole appearance. */
    NYA_ENTITY_VISUAL_NONE = 0,

    /** One still image: `sprite`, drawn at the entity's position. */
    NYA_ENTITY_VISUAL_SPRITE,

    /**
     * A sprite framed by `animator` over `atlas`. Advanced by nya_system_entity_update, with signals delivered
     * to on_animation, so a hit frame arrives as a callback.
     * */
    NYA_ENTITY_VISUAL_ANIMATION,

    /** A solid box through render3d, with the entity's rotation. Drawn only while a 3D camera is active. */
    NYA_ENTITY_VISUAL_CUBE,

    NYA_ENTITY_VISUAL_KIND_COUNT,
};

/**
 * How an entity draws itself; zeroed is NONE. Flat rather than a tagged union, because an animation needs
 * sprite, atlas and animator together.
 * */
struct NYA_EntityVisual {
    NYA_EntityVisualKind kind;

    /** SPRITE and ANIMATION. An animation overwrites the frame every tick. */
    NYA_Sprite sprite;

    /** ANIMATION: the sheet the animator's frame indexes. */
    NYA_SpriteAtlas atlas;

    NYA_SpriteAnimator animator;

    /** CUBE: full extents in world units. */
    f32x3 size;

    /** CUBE: base colour. A sprite's tint lives on the sprite. */
    NYA_Color color;

    /**
     * Draw order among one render call's entities; lower draws first. Explicit rather than `position.z`, since
     * a top-down game sorts by y and z may be a real axis.
     * */
    f32 z_order;

    /** Take the draw order from the position instead of `z_order`. */
    b8 y_sorted;

    /** Added to `position.y` before sorting: where the entity's feet are relative to its origin. */
    f32 y_sort_anchor;
};

/*
 * ─────────────────────────────────────────────────────────
 * ENTITY STRUCT
 * ─────────────────────────────────────────────────────────
 */

struct NYA_Entity {
    NYA_EntityHandle handle;

    /** Engine lifecycle bits (active, visible, static, despawning). Named `state` so `flags` is free for the game. */
    NYA_EntityState state;

    /**
     * What kind of thing this is. Game defined; the engine never interprets it. Code switches on it to decide
     * which fields matter, as with NYA_SimRecord.type.
     * */
    u32 type;

    /**
     * What the game says is true of this entity. Game defined, never interpreted. `type` says what an entity is,
     * this what is true of it. Integers, so spawning needs no allocation.
     * */
    u64 flags;

    /** Not owned. Point it at a literal or something that outlives the entity. */
    NYA_ConstCString name;

    /* transform */

    f32x3          position;
    NYA_Quaternion rotation;
    f32x3          scale;

    /**
     * The transform at the start of this tick, before anything moved it. Draws interpolate from here, see
     * nya_entity_render_position; something that jumps calls nya_entity_transform_snap.
     * */
    f32x3          position_previous;
    NYA_Quaternion rotation_previous;

    /*
     * Hierarchy, by handle so links survive despawns.
     *
     * `position`, `rotation` and `scale` stay the world transform, since physics writes them and everything
     * reads them. A parented entity keeps its offset in the `local_*` fields, and
     * nya_system_entity_transforms_update derives the world transform from the parent.
     *
     * Children are an intrusive list (`first_child`, `next_sibling`), newest first, so hierarchies allocate
     * nothing. Sibling order affects nothing.
     */

    NYA_EntityHandle parent;
    NYA_EntityHandle first_child;
    NYA_EntityHandle next_sibling;

    /** Direct children only. Maintained by the parenting calls. */
    u32 child_count;

    /** This entity's transform relative to its parent. Meaningless without a parent. */
    f32x3          local_position;
    NYA_Quaternion local_rotation;
    f32x3          local_scale;

    /* motion */

    f32x3 velocity;
    f32x3 angular_velocity;

    /*
     * Interpolated motion: where to end up and by when, set through nya_entity_move_to. The interpolation is a
     * core_tween.h tween; the entity keeps only the value it writes and the handle of the running move. It
     * overrides velocity for the position while it runs.
     */

    /**
     * Where the tween writes. Applied to `position`, or to a kinematic body's velocity, once per tick by
     * nya_system_entity_update.
     * */
    f32x3 move_position;

    /** The running move, or NYA_TWEEN_NONE. Generational, so an arrived move stops resolving. */
    NYA_Tween move_tween;

    /** Set for one tick after a body-backed move arrives, so its velocity is cleared. */
    b8 move_settling;

    /*
     * Physics. Two solvers, two fields, at most one used. Box2D and Box3D are separate worlds, and attaching both
     * would have two solvers fight over one transform.
     */

    /**
     * The 2D rigid body, if any. See physics2d.h. While attached the solver owns the transform and velocity
     * integration is skipped. Attach with nya_physics2d_body_attach; despawning destroys the body.
     * */
    NYA_Physics2DBody physics2d;

    /** The 3D rigid body, if any. See physics3d.h. Same contract; the solver also owns the full rotation. */
    NYA_Physics3DBody physics3d;

    /* appearance */

    /** What this entity looks like, drawn by nya_system_entity_render before on_render. Zeroed draws nothing. */
    NYA_EntityVisual visual;

    /**
     * What this entity emits; zeroed emits nothing. Read by nya_system_entity_lights, since lighting is one pass
     * over the scene.
     * */
    NYA_Light2D light;

    /** Whatever the game hangs off this entity. Not owned and not freed on despawn. */
    void* user_data;

    /* per entity callbacks, by handle so they survive a hot reload */

    NYA_CallbackHandle on_spawn;
    NYA_CallbackHandle on_despawn;
    NYA_CallbackHandle on_update;

    /**
     * Draws this entity. Run by nya_system_entity_render, which the game calls from inside its camera and
     * render target.
     * */
    NYA_CallbackHandle on_render;

    /**
     * Struck something hard enough to count. Run by the physics step for both sides, so something that should
     * happen once per collision acts only for the lower handle index. Only hits above
     * nya_physics2d_hit_threshold arrive. See physics2d.h.
     * */
    NYA_CallbackHandle on_collision;

    /**
     * An animation started, looped, hit a marked frame, or finished. Run by nya_system_entity_update per signal,
     * for ANIMATION visuals. Attacks hang off this, so the artist can retime a hit frame without code changes.
     *
     * ```c
     * void goblin_on_animation(NYA_Entity* entity, NYA_SpriteAnimationSignal signal) {
     *     if (signal.kind == NYA_SPRITE_ANIMATION_STARTED) nya_audio_play_sound(SWING_WAV, ...);
     *     if (signal.kind == NYA_SPRITE_ANIMATION_EVENT && signal.id == ATTACK_CONNECTS) strike(entity);
     *     if (signal.kind == NYA_SPRITE_ANIMATION_FINISHED) goblin_idle(entity);
     * }
     * ```
     * */
    NYA_CallbackHandle on_animation;

    /**
     * Clicked. Run by nya_entity_click, which the game calls, because only the game knows which camera turns
     * screen pixels into a world point or ray.
     * */
    NYA_CallbackHandle on_click;

    /** Hovered. Run by nya_entity_hover, which the game calls, for the same reason as on_click. */
    NYA_CallbackHandle on_hover;
};

typedef void (*NYA_EntityOnSpawnFn)(NYA_Entity* entity);
typedef void (*NYA_EntityOnDespawnFn)(NYA_Entity* entity);
typedef void (*NYA_EntityOnUpdateFn)(NYA_Entity* entity, f32 delta_time_s);
typedef void (*NYA_EntityOnRenderFn)(NYA_Entity* entity, NYA_Window* window);

/**
 * `other` is what it struck, null when that body has no entity. The hit is only valid during the call. See
 * NYA_PhysicsHit.
 * */
typedef void (*NYA_EntityOnCollisionFn)(NYA_Entity* entity, NYA_Entity* other, const NYA_PhysicsHit* hit);

/**
 * `world_point` is where the click landed. z is zero for a 2D click, since the 2D world is the z = 0 plane,
 * so one on_click serves both solvers.
 * */
typedef void (*NYA_EntityOnClickFn)(NYA_Entity* entity, f32x3 world_point, u8 button);

/**
 * `entered` is true when the cursor arrived, false when it left. No position: hovering is a state, and the
 * leaving point is off the entity. For per-frame behaviour use nya_entity_hovered.
 * */
typedef void (*NYA_EntityOnHoverFn)(NYA_Entity* entity, b8 entered);

/** By value; it does not outlive the call. */
typedef void (*NYA_EntityOnAnimationFn)(NYA_Entity* entity, NYA_SpriteAnimationSignal signal);

/**
 * What an entity starts as; everything optional. Scale defaults to 1 and rotation to identity, since zeroes
 * would make an entity of no size.
 * */
struct NYA_EntitySpawnOptions {
    NYA_ConstCString name;
    NYA_EntityState  state;
    u32              type;

    /** Game defined. See NYA_Entity.flags. */
    u64 flags;

    f32x3          position;
    NYA_Quaternion rotation;
    f32x3          scale;
    f32x3          velocity;
    f32x3          angular_velocity;

    void* user_data;

    NYA_CallbackHandle on_spawn;
    NYA_CallbackHandle on_despawn;
    NYA_CallbackHandle on_update;

    /** Draws this entity. See NYA_Entity.on_render. */
    NYA_CallbackHandle on_render;
    NYA_CallbackHandle on_collision;
    NYA_CallbackHandle on_click;
    NYA_CallbackHandle on_hover;
    NYA_CallbackHandle on_animation;

    /** What it looks like. Zeroed draws nothing. */
    NYA_EntityVisual visual;

    /** What it emits. Zeroed emits nothing. See NYA_Light2D. */
    NYA_Light2D light;
};

#define _NYA_ENTITY_DEFAULT_OPTIONS                                                                                                                  \
    .state = NYA_ENTITY_STATE_ACTIVE | NYA_ENTITY_STATE_VISIBLE, .scale = { 1.0F, 1.0F, 1.0F }, .rotation = { 0.0F, 0.0F, 0.0F, 1.0F }

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * FUNCTIONS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/*
 * ─────────────────────────────────────────────────────────
 * SYSTEM FUNCTIONS
 * ─────────────────────────────────────────────────────────
 */

NYA_API void nya_system_entity_init(void);
NYA_API void nya_system_entity_deinit(void);

/** Runs on_update for every active entity, and integrates velocity into the transform. */
NYA_API void nya_system_entity_update(f32 delta_time_s);

/**
 * Runs on_render for every visible entity on screen. Called by the game from the layer that owns the camera,
 * since only the game knows the coordinate space.
 * */
NYA_API void nya_system_entity_render(NYA_Window* window);

/**
 * Finds the entity whose body covers `world_point` and runs its on_click. Returns it, or
 * NYA_ENTITY_HANDLE_NONE for no hit or a hit without on_click, which is how an entity declines clicks.
 * */
NYA_API NYA_EntityHandle nya_entity_click(f32x2 world_point, u8 button) __attr_overloaded;

/**
 * The same, restricted to bodies in `layers`, so clicking the ground does not resolve to the terrain.
 * The call above is this one with NYA_PHYSICS_LAYER_ALL.
 * */
NYA_API NYA_EntityHandle nya_entity_click(f32x2 world_point, u8 button, NYA_PhysicsLayerMask layers) __attr_overloaded;

/**
 * The same for a 3D scene: the first entity along a ray. A click is a point in 2D and a line in 3D, as in
 * nya_physics2d_entity_at and nya_physics3d_raycast.
 *
 * ```c
 * NYA_Render3DRay ray = nya_render3d_screen_ray(window, (f32x2){ mouse->x, mouse->y });
 * nya_entity_click(ray.origin, ray.direction * 100.0F, mouse->button);
 * ```
 * */
NYA_API NYA_EntityHandle nya_entity_click(f32x3 origin, f32x3 direction, u8 button) __attr_overloaded;

/**
 * The same, restricted to bodies in `layers`, so a ray through scenery finds what the cursor is for. The call
 * above is this one with NYA_PHYSICS_LAYER_ALL.
 * */
NYA_API NYA_EntityHandle nya_entity_click(f32x3 origin, f32x3 direction, u8 button, NYA_PhysicsLayerMask layers) __attr_overloaded;

/**
 * Updates the hovered entity from a world-space cursor, running on_hover on changes. Returns who is hovered.
 * Callbacks fire on the edges, so calling it every frame is the intended use.
 * */
NYA_API NYA_EntityHandle nya_entity_hover(f32x2 world_point) __attr_overloaded;

/** The same, restricted to bodies in `layers`. See nya_entity_click. */
NYA_API NYA_EntityHandle nya_entity_hover(f32x2 world_point, NYA_PhysicsLayerMask layers) __attr_overloaded;

/** The same for a 3D scene. See nya_entity_click. */
NYA_API NYA_EntityHandle nya_entity_hover(f32x3 origin, f32x3 direction) __attr_overloaded;

/** The same along a ray, restricted to bodies in `layers`. See nya_entity_click. */
NYA_API NYA_EntityHandle nya_entity_hover(f32x3 origin, f32x3 direction, NYA_PhysicsLayerMask layers) __attr_overloaded;

/**
 * Says the cursor is on nothing, running on_hover(false) for the current entity. For a cursor leaving the
 * window or a menu opening over the world.
 * */
NYA_API void nya_entity_hover_clear(void);

/** Who the cursor is on, or NYA_ENTITY_HANDLE_NONE. */
NYA_API NYA_EntityHandle nya_entity_hovered(void) __attr_no_discard;

/** The value an entity sorts on: `z_order`, or where its feet are. */
NYA_API f32 nya_entity_sort_key(const NYA_Entity* entity) __attr_no_discard;

/**
 * Runs on_render only for entities positioned inside `min`..`max`. Positions, not bounds, so widen the
 * rectangle by the largest entity's radius or edges pop.
 * */
NYA_API void nya_system_entity_render_in(NYA_Window* window, f32x2 min, f32x2 max);

/*
 * ─────────────────────────────────────────────────────────
 * HIERARCHY
 * ─────────────────────────────────────────────────────────
 *
 * ```c
 * NYA_EntityHandle tank   = nya_entity_spawn(.name = "tank",   .position = { 100, 0, 0 });
 * NYA_EntityHandle turret = nya_entity_spawn(.name = "turret", .position = { 100, -20, 0 });
 *
 * // The turret keeps exactly where it is; its offset from the tank is captured here.
 * nya_entity_parent_set(turret, tank);
 *
 * // Now driving the tank carries the turret, and turning the turret does not move the tank.
 * nya_entity_get(tank)->position.x += 10.0F;
 * ```
 *
 * Nothing is recomputed when a parent moves. The hierarchy propagates once per tick at the end of
 * nya_system_entity_update, so a child's `position` is correct for rendering, queries and the next
 * tick, and one tick stale for an on_update that runs before its parent's. Call
 * nya_entity_transform_sync when that matters, usually when aiming.
 */

/**
 * Makes `child` follow `parent`, without moving it.
 *
 * @lua(ENTITIES)
 * */
NYA_API b8 nya_entity_parent_set(NYA_EntityHandle child, NYA_EntityHandle parent);

/**
 * Unparents, keeping the world transform. The same as parenting to NYA_ENTITY_HANDLE_NONE.
 *
 * @lua(ENTITIES)
 * */
NYA_API void nya_entity_parent_clear(NYA_EntityHandle child);

/** The parent, or NYA_ENTITY_HANDLE_NONE. */
NYA_API NYA_EntityHandle nya_entity_parent(const NYA_Entity* entity) __attr_no_discard;

/** The direct children, written into `out`. Returns the total, which may exceed `capacity`. */
NYA_API u32 nya_entity_children(const NYA_Entity* entity, OUT NYA_EntityHandle* out, u32 capacity);

/**
 * Whether `ancestor` is above `descendant`. Used to reject cycles.
 *
 * @lua(ENTITIES)
 * */
NYA_API b8 nya_entity_is_ancestor(NYA_EntityHandle ancestor, NYA_EntityHandle descendant) __attr_no_discard;

/**
 * Rewrites `entity` and its subtree from their parents' transforms, now.
 *
 * @lua(ENTITIES)
 * */
NYA_API void nya_entity_transform_sync(NYA_EntityHandle entity);

/** Propagates every parented transform. Called by nya_system_entity_update. */
NYA_API void nya_system_entity_transforms_update(void);

/**
 * The entity's world transform as a matrix, for handing to a renderer.
 * */
NYA_API f32_4x4 nya_entity_world_matrix(const NYA_Entity* entity) __attr_no_discard;

/*
 * ─────────────────────────────────────────────────────────
 * INTERPOLATION BETWEEN TICKS
 * ─────────────────────────────────────────────────────────
 *
 * ```c
 * void crate_on_render(NYA_Entity* entity, NYA_Window* window) {
 *     nya_render3d_cube(window, nya_entity_render_position(entity), size, nya_entity_render_rotation(entity), color);
 * }
 *
 * entity->position = spawn_point;
 * nya_entity_transform_snap(entity);   // a respawn draws there at once instead of sweeping across the map
 * ```
 *
 * Frames land between ticks, so a draw of `position` holds still on frames without a tick and jumps on the
 * next. Draws read the render transform instead, which moves from the previous tick toward the current one
 * by nya_app_tick_alpha. A child interpolates its own world transform, so it moves in step with its parent
 * and cuts the arc of a turning parent by a chord no wider than a tick's worth of turn.
 */

/**
 * Copies every entity's transform into its previous one. Called at the top of each tick, before physics or
 * anything else moves an entity.
 * */
NYA_API void nya_system_entity_transforms_capture(void);

/**
 * Where to draw the entity this frame, between its previous tick and its current one. The current position
 * during a tick.
 * */
NYA_API f32x3 nya_entity_render_position(const NYA_Entity* entity) __attr_no_discard;

/** The rotation to draw with this frame. Normalized linear, which a tick's worth of turn cannot tell from slerp. */
NYA_API NYA_Quaternion nya_entity_render_rotation(const NYA_Entity* entity) __attr_no_discard;

/**
 * Makes the entity and its subtree draw where they are now, without sweeping there from the previous tick.
 * For anything that jumps: teleports, respawns, a snapshot correction. Spawning and the physics teleports
 * already do it. Children are composed from the entity first, so they land with it.
 * */
NYA_API void nya_entity_transform_snap(NYA_Entity* entity);

/*
 * ─────────────────────────────────────────────────────────
 * INTERPOLATED MOTION
 * ─────────────────────────────────────────────────────────
 */

/**
 * Sends an entity to `target` over `duration_s`, along `ease`.
 *
 * ```c
 * nya_entity_move_to(nya_entity_get(chest), (f32x3){ 0, -64, 0 }, 0.4F, NYA_EASE_BACK_OUT);
 * ```
 * */
NYA_API void nya_entity_move_to(NYA_Entity* entity, f32x3 target, f32 duration_s, NYA_EaseType ease);

/**
 * The same move with the rest of NYA_TweenOptions: delay, repeat, yoyo, and a completion callback.
 * `repeat` restarts from the starting position; use `.yoyo = true` to go back and forth.
 * */
NYA_API void nya_entity_move_to_with_options(NYA_Entity* entity, f32x3 target, f32 duration_s, NYA_TweenOptions options);

/**
 * The same move at a constant speed in world units per second. Always linear, since an eased move only
 * matches a speed on average. A speed of zero or less teleports.
 * */
NYA_API void nya_entity_move_to_at_speed(NYA_Entity* entity, f32x3 target, f32 world_units_per_second);

/** Stops the move where it is. Harmless on an entity that is not moving. */
NYA_API void nya_entity_move_stop(NYA_Entity* entity);

/** Whether a move is running. False the tick after it arrives. */
NYA_API b8 nya_entity_moving(const NYA_Entity* entity) __attr_no_discard;

/**
 * Progress from 0 at the origin to 1 at the target, eased. One when not moving, since arrived and not
 * moving are the same state.
 * */
NYA_API f32 nya_entity_move_progress(const NYA_Entity* entity) __attr_no_discard;

/*
 * ─────────────────────────────────────────────────────────
 * LIGHTS
 * ─────────────────────────────────────────────────────────
 */

/**
 * Collects entity lights that could reach the visible region, brightest first, in world coordinates, up to
 * `capacity`. The dimmest are the ones dropped when there are too many.
 * */
NYA_API u32 nya_system_entity_lights(f32x2 min, f32x2 max, OUT NYA_Light2D* out, OUT f32x2* out_positions, u32 capacity);

/*
 * ─────────────────────────────────────────────────────────
 * SPATIAL QUERIES
 * ─────────────────────────────────────────────────────────
 */

/** Empty bucket and end of chain. Slot indices are below NYA_ENTITY_MAX. */
#define NYA_ENTITY_GRID_EMPTY 0xFFFFFFFFu

/**
 * Rebuilds the spatial index from every live entity. Called at the top of nya_system_entity_update, after
 * physics. Call it yourself only after moving entities directly and before querying in the same tick.
 * */
NYA_API void nya_system_entity_grid_rebuild(void);

/**
 * Entity handles positioned inside the rectangle, written into `out`. Returns how many, capped at `capacity`
 * (exactly `capacity` means truncated). Bucket order, not stable across rebuilds.
 * */
NYA_API u32 nya_entity_query_rect(f32x2 min, f32x2 max, OUT NYA_EntityHandle* out, u32 capacity);

/** The inscribed circle of the same search, tested exactly. */
NYA_API u32 nya_entity_query_radius(f32x2 center, f32 radius, OUT NYA_EntityHandle* out, u32 capacity);

/** nya_entity_query_rect filtered by `type`. */
NYA_API u32 nya_entity_query_kind(f32x2 min, f32x2 max, u32 type, OUT NYA_EntityHandle* out, u32 capacity);

/** nya_entity_query_rect filtered to entities with every bit of `flags` set. */
NYA_API u32 nya_entity_query_flags(f32x2 min, f32x2 max, u64 flags, OUT NYA_EntityHandle* out, u32 capacity);

/*
 * ─────────────────────────────────────────────────────────
 * 3D QUERIES
 * ─────────────────────────────────────────────────────────
 */

/** Every entity positioned inside the box. The 2D queries ignore z and would select a whole column in 3D. */
NYA_API u32 nya_entity_query_box(f32x3 min, f32x3 max, OUT NYA_EntityHandle* out, u32 capacity);

/** The same, filtered to one type. */
NYA_API u32 nya_entity_query_box_kind(f32x3 min, f32x3 max, u32 type, OUT NYA_EntityHandle* out, u32 capacity);

/** The same, filtered to entities carrying every bit in `flags`. */
NYA_API u32 nya_entity_query_box_flags(f32x3 min, f32x3 max, u64 flags, OUT NYA_EntityHandle* out, u32 capacity);

/** Every entity within `radius` of `center`, in 3D. */
NYA_API u32 nya_entity_query_sphere(f32x3 center, f32 radius, OUT NYA_EntityHandle* out, u32 capacity);

/**
 * The nearest entity a ray hits, treating each as a sphere of `radius`, or NYA_ENTITY_HANDLE_NONE. Pair with
 * nya_render3d_screen_ray to pick. `out_distance` receives the distance along the ray.
 * */
NYA_API NYA_EntityHandle nya_entity_query_ray(f32x3 origin, f32x3 direction, f32 radius, OUT f32* out_distance) __attr_no_discard;

/*
 * Iterating the whole world, for systems that are not spatial: scoring, saving, counting. Macros, so there
 * is no buffer to size. Spawning during one is safe; despawn only through nya_entity_despawn_deferred.
 */

/**
 * Walks every live entity of kind `kind` through the index, costing the matches plus a bitset scan. Same
 * rules as nya_entity_foreach.
 * */
// NOLINTBEGIN(bugprone-macro-parentheses): type and declarator parameters cannot be parenthesized
#define nya_entity_foreach_kind(kind, entity_name)                                                                                                   \
    for (NYA_EntityIter _nya_iter = _nya_entity_iter_kind((u32)(kind)); _nya_iter.entity != nullptr; _nya_entity_iter_advance(&_nya_iter))            \
        for (NYA_Entity* entity_name = _nya_iter.entity; entity_name != nullptr; entity_name = nullptr)
// NOLINTEND(bugprone-macro-parentheses)

/** Walks every live entity with every bit of `flag_bits` set. Zero matches everything. */
// NOLINTBEGIN(bugprone-macro-parentheses): type and declarator parameters cannot be parenthesized
#define nya_entity_foreach_flags(flag_bits, entity_name)                                                                                             \
    for (NYA_EntityIter _nya_iter = _nya_entity_iter_flags((u64)(flag_bits)); _nya_iter.entity != nullptr; _nya_entity_iter_advance(&_nya_iter))      \
        for (NYA_Entity* entity_name = _nya_iter.entity; entity_name != nullptr; entity_name = nullptr)
// NOLINTEND(bugprone-macro-parentheses)

/*
 * Changing flags. Do not write NYA_Entity.flags directly: the index keeps a bitset per flag, updated only
 * here, and a stale bitset makes queries silently skip entities. Reading is fine.
 */

/** Sets every bit in `flags`, leaving the rest alone. */
NYA_API void nya_entity_flag_enable(NYA_Entity* entity, u64 flags);

/** Clears every bit in `flags`, leaving the rest alone. */
NYA_API void nya_entity_flag_disable(NYA_Entity* entity, u64 flags);

/** Replaces the whole flag word. */
NYA_API void nya_entity_flags_set(NYA_Entity* entity, u64 flags);

/* Iteration internals, public only because the macros call them. */

NYA_API NYA_EntityIter _nya_entity_iter_kind(u32 type) __attr_no_discard;
NYA_API NYA_EntityIter _nya_entity_iter_flags(u64 flags) __attr_no_discard;
NYA_API void           _nya_entity_iter_advance(NYA_EntityIter* iter);

/*
 * ─────────────────────────────────────────────────────────
 * LIFETIME
 * ─────────────────────────────────────────────────────────
 */

#define nya_entity_spawn(...) nya_entity_spawn_with_options((NYA_EntitySpawnOptions){ _NYA_ENTITY_DEFAULT_OPTIONS, __VA_ARGS__ })

/** NYA_ENTITY_HANDLE_NONE when the table is full. */
NYA_API NYA_EntityHandle nya_entity_spawn_with_options(NYA_EntitySpawnOptions options) __attr_no_discard;

/**
 * Removes an entity immediately. Only safe outside iteration; code running during an update uses the
 * deferred form.
 * */
NYA_API void nya_entity_despawn(NYA_EntityHandle entity);

/**
 * Removes an entity at the next simulation barrier, so iteration is never disturbed. Despawning twice is
 * harmless.
 *
 * The form a script gets, and the only one it gets: a plugin's hook may well be running inside an
 * update, and the barrier is what makes removing something mid-iteration safe. Bound as
 * `nya.entity.despawn` rather than the derived name, because from Lua there is no other despawn to
 * tell it apart from.
 *
 * @lua(ENTITIES, nya.entity.despawn)
 * */
NYA_API void nya_entity_despawn_deferred(NYA_EntityHandle entity);

/** Null once the entity is gone. Store the handle, not the result. */
NYA_API NYA_Entity* nya_entity_get(NYA_EntityHandle entity) __attr_no_discard;

/**
 * Whether the handle still resolves to a live entity.
 *
 * @lua(ENTITIES)
 * */
NYA_API b8 nya_entity_is_valid(NYA_EntityHandle entity) __attr_no_discard;

/**
 * How many entities are alive.
 *
 * @lua(ENTITIES)
 * */
NYA_API u32 nya_entity_count(void) __attr_no_discard;

/** Removes every entity. Runs on_despawn for each. */
NYA_API void nya_entity_clear(void);

/*
 * ─────────────────────────────────────────────────────────
 * ITERATION
 * ─────────────────────────────────────────────────────────
 */

/** Null for an empty slot. Only meaningful below the high water mark. */
NYA_API NYA_Entity* nya_entity_at_slot(u32 index) __attr_no_discard;
NYA_API u32         nya_entity_slot_count(void) __attr_no_discard;

/**
 * Walks every live entity. A spawn during iteration may or may not be visited; despawn only through
 * nya_entity_despawn_deferred.
 * */
#define nya_entity_foreach(entity_name)                                                                                                              \
    for (u32 _nya_entity_slot = 0; _nya_entity_slot < nya_entity_slot_count(); _nya_entity_slot++)                                                   \
        for (NYA_Entity*(entity_name) = nya_entity_at_slot(_nya_entity_slot); (entity_name) != nullptr; (entity_name) = nullptr)
