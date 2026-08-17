/**
 * @file core_entity.h
 *
 * Entities: one flat struct per thing in the world, addressed by generational handle.
 *
 * Deliberately not an ECS. There are no components, no archetypes and no queries — every entity is
 * the same fat struct and unused fields simply sit there. For a game of this size that trades a
 * little memory for the ability to read a whole entity in one place, and it can be replaced later
 * without changing how anything refers to an entity, because references are handles rather than
 * pointers or indices.
 *
 * ```c
 * NYA_EntityHandle player = nya_entity_spawn(.name = "player", .position = { 0, 1, 0 });
 *
 * NYA_Entity* entity = nya_entity_get(player);   // null once it is despawned
 * if (entity) entity->velocity.y -= 9.81F * delta_time_s;
 *
 * nya_entity_despawn_deferred(player);           // applied at the simulation barrier
 * ```
 *
 * **Borrow pointers, hold handles.** `NYA_Entity*` stays valid for as long as that entity lives,
 * because the table never moves, but it says nothing about whether the entity still exists. Anything
 * that outlives the current scope — a deferred command, a field on another entity — stores a handle.
 * */
#pragma once

#include "nyangine/core/core_callback.h"
#include "nyangine/core/core_event.h"
#include "nyangine/physics/physics2d.h"
#include "nyangine/physics/physics3d.h"
#include "nyangine/core/core_types.h"
#include "nyangine/math/math_quaternion.h"
#include "nyangine/math/math_tween.h"
#include "nyangine/math/math_vector.h"
// The entity carries its own appearance, which is a sprite, an animator and an atlas by value.
#include "nyangine/renderer/render2d_sprite.h"

// Only ever a pointer here, and core_window.h is a far heavier include than one opaque type is
// worth — the same arrangement render2d.h uses.
typedef struct NYA_Window NYA_Window;

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * CONSTANTS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/**
 * Entity slots, allocated up front.
 *
 * Fixed rather than growable so an NYA_Entity* stays put.
 *
 * Handles already cover identity and lifetime, so a reallocating array would be safe *across*
 * frames. What it would not be safe for is a single callback body: on_update is handed a raw
 * NYA_Entity*, and code as ordinary as "spawn a bullet, then set my own cooldown" would be a use
 * after free whenever that spawn crossed a growth boundary. Silent, and only sometimes.
 *
 * Override with -DNYA_ENTITY_MAX=<n> per game. Costs sizeof(NYA_Entity) x n up front, allocated
 * once from the entity arena. If a hard ceiling is the wrong shape, the answer is chunked blocks
 * rather than one reallocating array: those grow without ever moving what already exists.
 * */
#ifndef NYA_ENTITY_MAX
#define NYA_ENTITY_MAX 8192
#endif

/**
 * World units across one cell of the spatial index.
 *
 * The one number that decides whether the grid helps. Too small and a query walks hundreds of empty
 * cells; too large and every cell holds everything and the index degenerates to the linear scan it
 * replaced. A few times the size of a typical entity is the usual answer — at the demo's 32 unit
 * crates, 128 puts roughly a dozen per cell.
 * */
#ifndef NYA_ENTITY_GRID_CELL_SIZE
#define NYA_ENTITY_GRID_CELL_SIZE 128.0F
#endif

/**
 * Hash buckets. Must be a power of two; the hash is masked rather than divided.
 *
 * This is a spatial *hash*, not a dense array of cells, so the world has no bounds and negative
 * coordinates cost nothing. The price is collisions: two distant cells can share a bucket, which is
 * why every query re-checks the cell an entity actually sits in rather than trusting the bucket.
 * */
#ifndef NYA_ENTITY_GRID_BUCKETS
#define NYA_ENTITY_GRID_BUCKETS 4096
#endif

static_assert((NYA_ENTITY_GRID_BUCKETS & (NYA_ENTITY_GRID_BUCKETS - 1)) == 0, "NYA_ENTITY_GRID_BUCKETS must be a power of two");

/**
 * How far outside the view nya_system_entity_render still draws, in world units.
 *
 * The index knows where an entity *is*, not how big it is, so an entity whose origin has left the
 * screen may still be half on it. Without a margin those pop out at the edge; with one they leave
 * properly. Set it to a little more than the largest entity's radius.
 * */
#ifndef NYA_ENTITY_RENDER_CULL_MARGIN
#define NYA_ENTITY_RENDER_CULL_MARGIN 128.0F
#endif

/**
 * Kinds the index has a bitset for. A kind at or above this still works, just without the shortcut.
 *
 * One bitset per kind is NYA_ENTITY_MAX bits — a kilobyte at the default — so sixty four kinds is
 * sixty four kilobytes. Generous for a game and cheap enough not to think about; a game with more
 * than sixty four kinds of thing has bigger questions than this table.
 * */
#ifndef NYA_ENTITY_KIND_MAX
#define NYA_ENTITY_KIND_MAX 64
#endif

/** Bits in NYA_Entity.flags, and so bitsets in the index. Fixed by the field's width. */
#define NYA_ENTITY_FLAG_COUNT 64

/** Words of 64 bits needed to cover every slot. */
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

enum NYA_EntityState {
    NYA_ENTITY_STATE_NONE = 0,

    /** Cleared to leave an entity in the world but skip its update. */
    NYA_ENTITY_STATE_ACTIVE = 1 << 0,

    /** Cleared to keep simulating an entity that is not drawn. */
    NYA_ENTITY_STATE_VISIBLE = 1 << 1,

    /** Never moves. A hint for whatever ends up doing spatial partitioning. */
    NYA_ENTITY_STATE_STATIC = 1 << 2,

    /**
     * Despawn has been requested and is waiting on the simulation barrier.
     *
     * Set by nya_entity_despawn_deferred. The entity is still fully valid until the barrier runs,
     * so anything mid update sees a consistent world; this flag is how it can tell the difference.
     * */
    NYA_ENTITY_STATE_DESPAWNING = 1 << 3,
};

/**
 * A uniform grid over entity positions, rebuilt every tick.
 *
 * Intrusive and allocation free: `buckets` holds the first entity slot in each bucket and `next`
 * chains the rest, one entry per entity slot. Inserting is two writes, clearing is one memset of
 * `buckets`, and nothing is allocated after init — which is what makes rebuilding the whole thing
 * every tick cheaper than maintaining it incrementally for a world where everything moves.
 *
 * **It indexes positions, not bounds.** An entity is in exactly one cell, the one its origin falls
 * in, however large it is. A query that needs overlap rather than containment has to expand its
 * rectangle by the largest entity's radius; the grid does not know how big anything is.
 *
 * Physics already has its own broadphase over everything with a rigid body, so this earns its place
 * on the cases that one cannot answer: entities with no body at all, and queries filtered by the
 * game's `type`, which Box2D knows nothing about.
 * */
struct NYA_EntityGrid {
    /** First entity slot in each bucket, or NYA_ENTITY_GRID_EMPTY. */
    u32* buckets;

    /** Next entity slot in the same bucket, indexed by slot. NYA_ENTITY_GRID_EMPTY ends the chain. */
    u32* next;

    f32 cell_size;

    /** Entities indexed by the last rebuild. For an overlay, and for noticing an index nobody refreshed. */
    u32 count;
};

/**
 * Bitsets saying which slots hold which kind and which flags.
 *
 * What makes "every camera" cost the number of cameras rather than the number of entities. Walking
 * the slot table and testing `type` was correct and got slower with the world: at eight hundred
 * crates, finding the two cameras meant eight hundred comparisons, every system, every tick.
 *
 * A bitset turns that into a scan of NYA_ENTITY_BITSET_WORDS words — a hundred and twenty eight at
 * the default — whatever the world contains, and each word that is zero skips sixty four slots at
 * once. In a world of crates the camera bitset is almost entirely zero words.
 *
 * ## Maintained, not rebuilt
 *
 * Updated on spawn, on despawn and on every flag change, rather than rebuilt once a tick like the
 * spatial grid. That difference is deliberate: a grid that is a tick stale gives slightly wrong
 * positions, which is invisible, while an index that is a tick stale makes a query *miss an entity*,
 * which is a bug that shows up as something not happening for one frame and is miserable to find.
 *
 * The price is that flags cannot be written directly. nya_entity_flag_enable and friends are the
 * only supported way, and `NYA_Entity.flags` is read-only to everyone else.
 * */
struct NYA_EntityIndex {
    /** Occupied slots. Every query is masked by this, so a freed slot can never be returned. */
    u64* live;

    /** One bitset per kind, indexed by `type`. Kinds at or above NYA_ENTITY_KIND_MAX are not here. */
    u64* kinds;

    /** One bitset per flag bit. A multi-bit query walks the first bit's set and checks the rest. */
    u64* flags;
};

/**
 * Walks the entities matching a query. Built by the nya_entity_foreach_* macros; not held by hand.
 *
 * `entity` is the current match and null once there are none left, which is what the macros test.
 * */
struct NYA_EntityIter {
    /** The bitset being walked — a kind's, a flag's, or `live` when the query has no shortcut. */
    const u64* bits;

    /** Every one of these must be set on the entity. Zero matches everything. */
    u64 require_flags;

    u32 require_type;
    b8  check_type;

    u32 word;

    /**
     * Words worth scanning, from the entity table's high water mark.
     *
     * The bitsets cover every slot, but slots past the high water mark have never held anything, so
     * scanning them is scanning guaranteed zeroes. A world of a few hundred entities is five words,
     * not a hundred and twenty eight.
     * */
    u32 word_count;

    /** Bits of the current word not yet visited. Consumed lowest first. */
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

    NYA_Entity* entities;
    b8*         occupied;
    u32*        generations;

    /**
     * Indices of free slots, most recently freed first.
     *
     * A stack rather than a scan for the first empty slot: with NYA_ENTITY_MAX slots that scan is
     * what turns spawning a few thousand entities from linear into quadratic.
     * */
    u32* free_slots;
    u32  free_count;

    u32 count;

    /** Highest slot ever occupied. Iteration stops here rather than at NYA_ENTITY_MAX. */
    u32 high_water_mark;

    NYA_EntityGrid  grid;
    NYA_EntityIndex index;

    /**
     * Who the cursor is currently on, or NYA_ENTITY_HANDLE_NONE.
     *
     * Held here rather than as a bit on the entity because the *transition* is what matters and a bit
     * per entity cannot describe one: firing "left" requires knowing who was hovered before, and
     * scanning every entity for a stale flag every frame is the thing this one handle replaces.
     *
     * Zero is NYA_ENTITY_HANDLE_NONE, and generations start at one, so a zeroed system already means
     * nothing is hovered.
     * */
    NYA_EntityHandle hovered;
};

/*
 * ─────────────────────────────────────────────────────────
 * APPEARANCE
 * ─────────────────────────────────────────────────────────
 *
 * What an entity looks like, so that most entities need no on_render at all.
 *
 * Before this, drawing an entity meant writing a callback that read the entity's transform and
 * called the renderer — which is the same eight lines in every game, per kind of thing, and is where
 * the z-ordering and the batching went to die: nya_system_entity_render walked the spatial grid in
 * bucket order and each callback drew immediately, so draw order was neither spatial nor stable and
 * two entities sharing a texture were only batched by luck.
 *
 * Giving the entity its appearance instead lets the system sort before it draws. See
 * nya_system_entity_render_in.
 *
 * on_render still exists and still runs, *after* the visual — for the health bar over the sprite, the
 * debug outline, the thing no enum will ever cover.
 */

enum NYA_EntityVisualKind {
    /** Nothing is drawn automatically. on_render is the whole appearance, as it was before. */
    NYA_ENTITY_VISUAL_NONE = 0,

    /** One still image: `sprite`, drawn at the entity's position. */
    NYA_ENTITY_VISUAL_SPRITE,

    /**
     * A sprite whose frame comes from `animator` playing over `atlas`.
     *
     * The animator is advanced by nya_system_entity_update, and its signals are delivered to
     * on_animation — so an attack's hit frame arrives as a callback without the game running a timer.
     * */
    NYA_ENTITY_VISUAL_ANIMATION,

    /**
     * A solid box, through render3d, with the entity's own rotation.
     *
     * Here so that a 3D entity is as little work as a 2D one. It is drawn only while a 3D camera is
     * active — see nya_render3d_begin — and is silently skipped otherwise, because there is no
     * projection to draw it through and guessing one would put it somewhere arbitrary.
     * */
    NYA_ENTITY_VISUAL_CUBE,

    NYA_ENTITY_VISUAL_KIND_COUNT,
};

/**
 * How an entity draws itself. Zeroed means NONE, so this costs nothing to ignore.
 *
 * A tagged union would be smaller. It is not one because an animation needs the sprite *and* the
 * atlas *and* the animator at the same time, and the fields that would overlap — a cube's size
 * against an atlas's frame size — are two floats against a struct. Flat is readable and the entity
 * table is preallocated anyway.
 * */
struct NYA_EntityVisual {
    NYA_EntityVisualKind kind;

    /** SPRITE and ANIMATION. For ANIMATION its frame is overwritten every tick from the animator. */
    NYA_Sprite sprite;

    /** ANIMATION: which sheet the animator's frame indexes into. */
    NYA_SpriteAtlas atlas;

    NYA_SpriteAnimator animator;

    /** CUBE: full extents in world units. */
    f32x3 size;

    /** CUBE: base colour. A sprite's tint lives on the sprite. */
    NYA_Color color;

    /**
     * Draw order among everything the same render call is drawing. Lower draws first, so behind.
     *
     * Explicit rather than taken from `position.z`, because the two are different questions: a
     * top-down game sorts by `position.y` so that something further down the screen is in front,
     * and an entity's z is either unused or is a real third axis. Sorting by the wrong one puts
     * every sprite in the same plane and back at bucket order.
     *
     * Ties are broken by texture, which is what keeps sorting from destroying the batching — see
     * nya_system_entity_render_in.
     * */
    f32 z_order;
};

/*
 * ─────────────────────────────────────────────────────────
 * ENTITY STRUCT
 * ─────────────────────────────────────────────────────────
 */

struct NYA_Entity {
    NYA_EntityHandle handle;

    /**
     * Engine owned lifecycle bits: active, visible, static, despawning. See NYA_EntityState.
     *
     * Named `state` rather than `flags` so that `flags` can be the game's, which is the field a game
     * actually reaches for. These four are the engine's business and a game only rarely sets them.
     * */
    NYA_EntityState state;

    /**
     * What kind of thing this is. Game defined; core never interprets it.
     *
     * The tag half of a tagged fat struct: code switches on it to decide which of the fields below
     * actually mean anything for this entity. Same arrangement as NYA_SimRecord.type, and for the
     * same reason — the engine has no business enumerating what a game contains.
     * */
    u32 type;

    /**
     * Whatever the game wants to be true of this entity. Game defined; core never interprets it.
     *
     * The companion to `type`: that says what an entity *is*, this says what is *true* of it. Both
     * are opaque to the engine for the same reason, and both are plain integers rather than a
     * pointer so that an entity spawned and despawned by the hundred needs no allocation to carry
     * either.
     *
     * Sixty four bits rather than thirty two, and separate from `type` rather than sharing its high
     * bits, because a flag word runs out far sooner than a kind enum does — and a game that packed
     * both into one field would find every `entity->type == SOMETHING` comparison silently false the
     * day it set its first flag.
     * */
    u64 flags;

    /** Not owned. Point it at a literal or something that outlives the entity. */
    NYA_ConstCString name;

    /* ── transform ── */

    f32x3          position;
    NYA_Quaternion rotation;
    f32x3          scale;

    /* ── motion ── */

    f32x3 velocity;
    f32x3 angular_velocity;

    /* ── interpolated motion ──
     *
     * The other way to move something: velocity says how fast, this says where to end up and by when.
     * Driven by nya_system_entity_update alongside the velocity integration, and set through
     * nya_entity_move_to rather than field by field — `target_origin` has to be captured at the moment
     * the move is asked for, and a plain assignment to `target_position` could not do that.
     *
     * The two are not exclusive but they do fight: while a move is running the interpolation writes
     * `position` outright, so any velocity integrated into it that tick is overwritten. See
     * nya_entity_move_to.
     */

    /** Where the entity is heading. Meaningless unless `target_duration_s` is above zero. */
    f32x3 target_position;

    /**
     * Where it started, captured when the move was requested.
     *
     * Kept rather than recomputed, because the interpolation is an absolute lerp from origin to target
     * rather than a step toward the target from wherever the entity currently is. That difference is
     * what makes easing possible at all: an eased step from the current position re-eases the shrinking
     * remainder every tick and converges to something that is not the requested curve.
     * */
    f32x3 target_origin;

    /** Total seconds the move takes. Zero means no move is running, which is what stops the update. */
    f32 target_duration_s;

    /** Seconds elapsed into it. Clamped to `target_duration_s`, at which point the move ends. */
    f32 target_elapsed_s;

    NYA_EaseType target_ease;

    /* ── physics ──
     *
     * Two solvers, two fields, and an entity is expected to use at most one of them.
     *
     * They are separate rather than a tagged union because they are separate simulations: Box2D and
     * Box3D each own a world, and neither knows the other exists. Both being attached is not checked
     * for and is not useful — two solvers writing one transform is the same fight the velocity
     * integration already steps aside from, decided by whichever runs second.
     */

    /**
     * The 2D rigid body simulating this entity, if it has one. See physics2d.h.
     *
     * Zeroed, and `attached` false, for the ordinary entity that moves by having its velocity
     * integrated. While it is attached the solver owns the transform and the integration above is
     * skipped, because two things writing one position is a fight the frame rate decides.
     *
     * Attach it with nya_physics2d_body_attach rather than by filling this in: the body has to exist
     * in the world before the entity can point at it, and despawning has to destroy it.
     * */
    NYA_Physics2DBody physics2d;

    /**
     * The 3D rigid body, if it has one. See physics3d.h.
     *
     * The same contract as the 2D one in every respect, against a different solver: attach with
     * nya_physics3d_body_attach, the solver owns position *and* the full rotation quaternion while it
     * is attached, and despawning destroys it.
     * */
    NYA_Physics3DBody physics3d;

    /* ── appearance ── */

    /**
     * What this entity looks like. Drawn by nya_system_entity_render before its on_render runs.
     *
     * Zeroed is NYA_ENTITY_VISUAL_NONE, which draws nothing — so an entity that had an on_render
     * before this existed behaves exactly as it did.
     * */
    NYA_EntityVisual visual;

    /**
     * What this entity emits. Zeroed emits nothing, which is the common case and costs nothing.
     *
     * Read by nya_system_entity_lights rather than drawn by the entity itself, because lighting is
     * one pass over the whole scene and cannot be assembled a draw call at a time.
     * */
    NYA_Light2D light;

    /**
     * Whatever the game needs to hang off this entity.
     *
     * Not owned and not freed on despawn. The engine has no business knowing what a player or a
     * projectile is, and this is the seam where that stays true.
     * */
    void* user_data;

    /* ── per entity callbacks, by handle so they survive a hot reload ── */

    NYA_CallbackHandle on_spawn;
    NYA_CallbackHandle on_despawn;
    NYA_CallbackHandle on_update;

    /**
     * Draws this entity. Run by nya_system_entity_render, which a game calls itself.
     *
     * Unlike the other three this one is not driven by the engine's own loop, because *when* to draw
     * entities is a decision only the game can make: they belong inside whatever camera and render
     * target the drawing layer has set up, and the engine has no idea which layer that is.
     * */
    NYA_CallbackHandle on_render;

    /**
     * Struck something hard enough to count. Run by the physics step, once per hit per entity.
     *
     * Both sides of a hit are called, each with the other as `other`, so a pair that both care will
     * both hear about it — which is why anything that should happen *once* per collision has to say
     * so, usually by acting only for the lower of the two handle indices.
     *
     * Only hits above nya_physics2d_hit_threshold arrive here. A resting stack generates contacts every
     * step and none of them are events; see physics2d.h.
     * */
    NYA_CallbackHandle on_collision;

    /**
     * An animation started, looped, hit a marked frame, or finished.
     *
     * Run by nya_system_entity_update, once per signal, only for an entity whose visual is an
     * ANIMATION. This is the hook an attack hangs off: play the swing and the sound on the input, and
     * land the hit when the frame carrying the marker comes up — a frame number the artist owns and
     * can retime without the game noticing.
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
     * Pointed at and clicked. Run by nya_entity_click, which a game calls itself.
     *
     * Not driven by the engine's input handling, for the same reason on_render is not driven by its
     * frame loop: a click arrives in screen pixels and only the game knows which camera turns those
     * into a world point — or, in 3D, into a ray.
     *
     * Works in both dimensions. It used to work in one: nya_entity_click took an f32x2 and asked
     * nya_physics2d_entity_at, so a 3D entity's on_click could never fire at all — nothing in the
     * engine was able to reach it, and the 3D scene in gnyame did its own raycast and never called
     * the callback. See the ray-taking overload of nya_entity_click.
     * */
    NYA_CallbackHandle on_click;

    /**
     * Pointed at, without clicking. Run by nya_entity_hover, which a game calls itself.
     *
     * Driven by the game for the same reason on_click is: hovering is a cursor in screen pixels, and
     * only the game knows which camera turns those into a world point or a ray.
     *
     * **Edge triggered.** It runs once with `entered` true when the cursor arrives and once with false
     * when it leaves, rather than every frame the cursor rests on it. That is what the callback is
     * almost always used for — turn a highlight on, turn it off, show a tooltip, hide it — and a
     * per-frame version would make the common case count its own repeats to find the edges. A game that
     * genuinely wants "while hovered" has it already: nya_entity_hovered says who it is, every frame.
     *
     * Exactly one entity is hovered at a time, the same topmost-wins rule a click uses.
     * */
    NYA_CallbackHandle on_hover;
};

typedef void (*NYA_EntityOnSpawnFn)(NYA_Entity* entity);
typedef void (*NYA_EntityOnDespawnFn)(NYA_Entity* entity);
typedef void (*NYA_EntityOnUpdateFn)(NYA_Entity* entity, f32 delta_time_s);
typedef void (*NYA_EntityOnRenderFn)(NYA_Entity* entity, NYA_Window* window);

/**
 * `other` is whatever it struck, and is null when that body has no entity behind it.
 *
 * The hit is the engine's, valid only for the tick it is delivered in — copy anything from it that
 * has to outlive the call. See NYA_PhysicsHit.
 * */
typedef void (*NYA_EntityOnCollisionFn)(NYA_Entity* entity, NYA_Entity* other, const NYA_PhysicsHit* hit);

/**
 * `world_point` is where the click landed, in three dimensions.
 *
 * f32x3 for both dimensions rather than one signature each, and z is zero for a 2D click. That is
 * not a fiction: the 2D world *is* the z = 0 plane and 2D entities already sit there — the same
 * reasoning, and the same shape, as NYA_PhysicsHit. See physics_types.h.
 *
 * One signature is what lets a single on_click serve an entity whichever solver is simulating it,
 * which matters for a game putting a 2D interface over a 3D scene.
 * */
typedef void (*NYA_EntityOnClickFn)(NYA_Entity* entity, f32x3 world_point, u8 button);

/**
 * `entered` is true when the cursor arrived on this entity and false when it left.
 *
 * No world point, unlike NYA_EntityOnClickFn. A click happens *at* a place and what was struck is the
 * substance of it; hovering is a state with two edges, and the position on leaving is by definition
 * somewhere the entity no longer is. A callback that wanted the cursor can read it — and one that
 * wants it every frame wants nya_entity_hovered rather than this.
 * */
typedef void (*NYA_EntityOnHoverFn)(NYA_Entity* entity, b8 entered);

/** The signal is by value: it is four words, and it does not outlive the call. */
typedef void (*NYA_EntityOnAnimationFn)(NYA_Entity* entity, NYA_SpriteAnimationSignal signal);

/**
 * What an entity starts as. Everything is optional.
 *
 * Scale defaults to 1 and rotation to identity rather than to zero, because a zeroed transform
 * produces an entity of no size facing nowhere, which is never what anyone meant.
 * */
struct NYA_EntitySpawnOptions {
    NYA_ConstCString name;
    NYA_EntityState  state;
    u32              type;

    /** Game defined, and the reason the engine's own bits are called `state`. See NYA_Entity.flags. */
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

    /**
     * Draws this entity. Run by nya_system_entity_render, which a game calls itself.
     *
     * Unlike the other three this one is not driven by the engine's own loop, because *when* to draw
     * entities is a decision only the game can make: they belong inside whatever camera and render
     * target the drawing layer has set up, and the engine has no idea which layer that is.
     * */
    NYA_CallbackHandle on_render;
    NYA_CallbackHandle on_collision;
    NYA_CallbackHandle on_click;
    NYA_CallbackHandle on_hover;
    NYA_CallbackHandle on_animation;

    /** What it looks like. Zeroed draws nothing, which is what an entity with an on_render wants. */
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
 * Runs on_render for every visible entity **that is on screen**.
 *
 * Called by the game, from inside the layer that owns the camera — not by the engine's frame loop.
 * An entity has to be drawn in the same coordinate space as the world around it, and the engine
 * cannot know which of a window's layers that is.
 *
 * The visible region is derived from the target's size and whatever camera is currently set, so this
 * culls without being told anything: with no camera it is the target in screen pixels, and with one
 * it is the world rectangle that maps onto it. All four corners are transformed rather than two, so a
 * rotated camera still gets a rectangle that contains its view.
 *
 * Widened by NYA_ENTITY_RENDER_CULL_MARGIN, because the grid indexes an entity's *position* and
 * something large enough can be visible while its origin is not — see NYA_EntityGrid.
 *
 * Draw order is bucket order, which is neither spatial nor stable across a rebuild. It was slot order
 * before culling existed; anything that needs a depth sort should keep its own list rather than
 * expecting this to grow one.
 *
 * Entities without NYA_ENTITY_STATE_VISIBLE are skipped, which is what that bit is for.
 * */
NYA_API void nya_system_entity_render(NYA_Window* window);

/**
 * Finds the entity whose body covers `world_point` and runs its on_click.
 *
 * Returns who was clicked, or NYA_ENTITY_HANDLE_NONE when the point hit nothing — or hit something
 * with no on_click, which is how an entity declines to be clickable. There is no flag for that: not
 * having the callback *is* the property, and a second way to say the same thing is a second way for
 * the two to disagree.
 *
 * The topmost body wins and the click does not fall through to whatever is behind it, which matches
 * how nya_physics2d_entity_at answers and is what a picker normally wants.
 *
 * The callback receives the point as f32x3 with z zero. See NYA_EntityOnClickFn.
 * */
NYA_API NYA_EntityHandle nya_entity_click(f32x2 world_point, u8 button) __attr_overloaded;

/**
 * The same, for a 3D scene: the first entity along a ray gets its on_click.
 *
 * Two overloads rather than one function, because a click is genuinely a different shape in each
 * dimension — in 2D the screen *is* the world plane and the click names a point, while in 3D it
 * names a line into the volume. That is the same split nya_physics2d_entity_at and
 * nya_physics3d_raycast already make, and this sits on top of both.
 *
 * ```c
 * NYA_Render3DRay ray = nya_render3d_screen_ray(window, (f32x2){ mouse->x, mouse->y });
 * nya_entity_click(ray.origin, ray.direction * 100.0F, mouse->button);
 * ```
 *
 * `direction` need not be normalised; its length is how far the click reaches, so a game decides
 * whether something across the map is clickable. The callback gets the point on the surface that
 * was struck, not the ray's origin.
 *
 * Closest hit wins, so a crate behind another crate cannot take the click.
 * */
NYA_API NYA_EntityHandle nya_entity_click(f32x3 origin, f32x3 direction, u8 button) __attr_overloaded;

/**
 * Tells the entity system where the cursor is, and runs on_hover for whatever moved under or out from
 * under it. Returns who is hovered now, or NYA_ENTITY_HANDLE_NONE.
 *
 * Called once a frame with the cursor in world space, exactly like nya_entity_click is called on a
 * press. Calling it repeatedly with the cursor on the same entity does nothing: the callbacks fire on
 * the edges, so this is cheap to call unconditionally and that is how it is meant to be used.
 *
 * The entity being left gets its `false` before the entity being entered gets its `true`, so a game
 * that clears a highlight on leaving and sets one on entering cannot end up having cleared the new one.
 *
 * Unlike nya_entity_click this returns the handle even when it has no on_hover: the question a caller
 * asks here is "what is under the cursor", which is worth answering whether or not anything reacted.
 * */
NYA_API NYA_EntityHandle nya_entity_hover(f32x2 world_point) __attr_overloaded;

/** The same, for a 3D scene: the first entity along the ray is the hovered one. See nya_entity_click. */
NYA_API NYA_EntityHandle nya_entity_hover(f32x3 origin, f32x3 direction) __attr_overloaded;

/**
 * Says the cursor is on nothing, running the current entity's on_hover with false.
 *
 * For when the pointer leaves the window, a menu opens over the world, or the game stops accepting
 * pointer input. Without it the last entity hovered before the cursor left would keep its highlight
 * until the cursor came back, because nothing would ever tell it otherwise.
 * */
NYA_API void nya_entity_hover_clear(void);

/** Who the cursor is on, or NYA_ENTITY_HANDLE_NONE. What to read for "while hovered" behaviour. */
NYA_API NYA_EntityHandle nya_entity_hovered(void) __attr_no_discard;

/**
 * Runs on_render only for entities whose position falls inside `min`..`max`.
 *
 * What a camera wants: at eight hundred crates the untargeted form calls on_render eight hundred
 * times regardless of how many are on screen, and drawing something off screen costs the same as
 * drawing something on it right up until the GPU discards it.
 *
 * Positions, not bounds — see NYA_EntityGrid. Expand the rectangle by the largest entity's radius or
 * things straddling the edge pop out one frame early.
 * */
NYA_API void nya_system_entity_render_in(NYA_Window* window, f32x2 min, f32x2 max);

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
 *
 * The origin is captured here, so the curve runs from where the entity is *now* to where it is being
 * sent — calling this again mid-move restarts from the current position rather than resuming, which
 * is what "go somewhere else instead" should mean.
 *
 * While the move runs it writes `position` outright, once per tick, before on_update. Velocity is
 * still integrated on the same tick and then overwritten, so an entity should be doing one or the
 * other; giving something both is not an error and not useful either.
 *
 * A duration of zero or less puts the entity at `target` immediately and leaves no move running,
 * which makes `nya_entity_move_to(e, p, 0, ...)` a teleport that reads like every other move.
 *
 * **Bodies.** An entity the solver owns cannot have its position written from outside without the two
 * disagreeing, so:
 *
 * - no body — `position` is written directly.
 * - kinematic body — the per tick delta is turned into a linear velocity and given to the solver,
 *   which is how a moving platform pushes what stands on it rather than passing through it.
 * - dynamic or static body — ignored, and logged once. Push a dynamic body with an impulse; a static
 *   one is static.
 * */
NYA_API void nya_entity_move_to(NYA_Entity* entity, f32x3 target, f32 duration_s, NYA_EaseType ease);

/**
 * The same move, with the duration derived from a constant speed in world units per second.
 *
 * Forced to NYA_EASE_LINEAR, because a speed and an easing curve are contradictory requests — an
 * eased move covers the distance at a speed that is only equal to the requested one on average.
 *
 * A speed of zero or less is a teleport, matching a duration of zero.
 * */
NYA_API void nya_entity_move_to_at_speed(NYA_Entity* entity, f32x3 target, f32 world_units_per_second);

/**
 * Abandons the move where it is. The entity keeps the position it had reached.
 *
 * Harmless on an entity that is not moving, which is what makes it safe to call from a state change
 * that does not know.
 * */
NYA_API void nya_entity_move_stop(NYA_Entity* entity);

/** Whether a move is running. False the tick after it arrives. */
NYA_API b8 nya_entity_moving(const NYA_Entity* entity) __attr_no_discard;

/**
 * How far into the move the entity is, from 0 at the origin to 1 at the target.
 *
 * The eased value rather than the raw fraction of time, so it is the same number the position was
 * built from — which is what anything driving a second property off the same move wants. One for an
 * entity that is not moving, since "not moving" and "arrived" are the same state once the move ends.
 * */
NYA_API f32 nya_entity_move_progress(const NYA_Entity* entity) __attr_no_discard;

/*
 * ─────────────────────────────────────────────────────────
 * LIGHTS
 * ─────────────────────────────────────────────────────────
 */

/**
 * Collects every entity light that could reach the visible region, brightest first.
 *
 * Written into `out` in world coordinates, capped at `capacity`, and returns how many. Sorted by how
 * much light each contributes at all — so when a scene has more lights than the pass can carry, the
 * ones that get dropped are the ones nobody would have noticed.
 *
 * Called by nya_render2d_lights_apply, which is what a game actually uses. Exposed because a game
 * that wants to light a render texture, or to feed the list to a shader of its own, needs the same
 * query and should not have to walk the entity table by hand.
 *
 * The region is widened by each light's own radius before the test, because a light whose *origin*
 * is off screen still spills onto it — the same margin nya_system_entity_render applies to sprites,
 * for the same reason.
 * */
NYA_API u32 nya_system_entity_lights(f32x2 min, f32x2 max, OUT NYA_Light2D* out, OUT f32x2* out_positions, u32 capacity);

/*
 * ─────────────────────────────────────────────────────────
 * SPATIAL QUERIES
 * ─────────────────────────────────────────────────────────
 */

/** Sentinel for an empty bucket and the end of a chain. Slot indices are below NYA_ENTITY_MAX. */
#define NYA_ENTITY_GRID_EMPTY 0xFFFFFFFFu

/**
 * Rebuilds the spatial index from every live entity's current position.
 *
 * Called at the top of nya_system_entity_update, after the physics step has written this tick's
 * transforms, so a query during a layer's update or render sees the world as it is now. A game does
 * not call this — but it must call it itself if it moves entities directly and then queries in the
 * same tick, because nothing else notices that positions changed.
 * */
NYA_API void nya_system_entity_grid_rebuild(void);

/**
 * Entity handles whose positions fall inside the rectangle, written into `out`.
 *
 * Returns how many were written, which is capped at `capacity` — a caller that gets exactly
 * `capacity` back should assume it was truncated and ask for a smaller region or a bigger buffer.
 *
 * Order is bucket order, which is neither spatial nor stable across a rebuild. Sort the result if it
 * matters.
 * */
NYA_API u32 nya_entity_query_rect(f32x2 min, f32x2 max, OUT NYA_EntityHandle* out, u32 capacity);

/** The circle inscribed in the same search, tested exactly rather than by its bounding box. */
NYA_API u32 nya_entity_query_radius(f32x2 center, f32 radius, OUT NYA_EntityHandle* out, u32 capacity);

/** nya_entity_query_rect, keeping only entities whose `type` matches. What the broadphase cannot do. */
NYA_API u32 nya_entity_query_kind(f32x2 min, f32x2 max, u32 type, OUT NYA_EntityHandle* out, u32 capacity);

/**
 * nya_entity_query_rect, keeping only entities with **every** bit of `flags` set.
 *
 * The other half of "systems query for what they care about": a kind says what a thing is, flags say
 * what is true of it, and a system usually wants the second — "everything flammable near the fire"
 * rather than "every crate near the fire".
 * */
NYA_API u32 nya_entity_query_flags(f32x2 min, f32x2 max, u64 flags, OUT NYA_EntityHandle* out, u32 capacity);

/*
 * ─────────────────────────────────────────────────────────
 * 3D QUERIES
 * ─────────────────────────────────────────────────────────
 */

/**
 * Every entity whose position is inside the box, ignoring nothing.
 *
 * The 2D queries above test x and y and say nothing about z, which is correct for a side-on or
 * top-down game and silently wrong for a 3D one — a marquee select would take everything in the
 * column above and below the box as well.
 *
 * Position only, not bounds: an entity is a point here. Testing a model's extents means resolving
 * its mesh, which the entity system deliberately knows nothing about — a caller that needs it
 * narrows this result with nya_render3d_mesh_bounds.
 * */
NYA_API u32 nya_entity_query_box(f32x3 min, f32x3 max, OUT NYA_EntityHandle* out, u32 capacity);

/** The same, filtered to one type. */
NYA_API u32 nya_entity_query_box_kind(f32x3 min, f32x3 max, u32 type, OUT NYA_EntityHandle* out, u32 capacity);

/** The same, filtered to entities carrying every bit in `flags`. */
NYA_API u32 nya_entity_query_box_flags(f32x3 min, f32x3 max, u64 flags, OUT NYA_EntityHandle* out, u32 capacity);

/** Every entity within `radius` of `center` in three dimensions. A sphere, where the 2D one is a circle. */
NYA_API u32 nya_entity_query_sphere(f32x3 center, f32 radius, OUT NYA_EntityHandle* out, u32 capacity);

/**
 * The nearest entity a ray hits, treating each as a sphere of `radius`, or NYA_ENTITY_HANDLE_NONE.
 *
 * What clicking in a 3D viewport does: nya_render3d_screen_ray gives the ray, this gives the thing
 * under the pointer. Spheres rather than boxes because an entity has no bounds here — see
 * nya_entity_query_box — and a sphere is the one shape a point can be given without asking the
 * renderer anything.
 *
 * `out_distance` receives the distance along the ray, so a caller can compare hits across several
 * calls with different radii.
 * */
NYA_API NYA_EntityHandle nya_entity_query_ray(f32x3 origin, f32x3 direction, f32 radius, OUT f32* out_distance) __attr_no_discard;

/*
 * ── Iterating the whole world ──
 *
 * The spatial queries above answer "what is near here". These answer "what exists", which is what a
 * system that is not spatial at all wants — scoring, ageing, saving, counting.
 *
 * Macros rather than functions returning arrays, so there is no buffer to size and nothing to
 * truncate. Same iteration rules as nya_entity_foreach: spawning during one is safe, despawning is
 * only safe through nya_entity_despawn_deferred.
 */

/**
 * Walks every live entity whose `type` is `kind`, through the index.
 *
 * Costs the number of matches plus a scan of the kind's bitset, rather than the size of the world.
 * Same iteration rules as nya_entity_foreach: spawning during one is safe and the new entity may or
 * may not be visited, despawning is only safe through nya_entity_despawn_deferred.
 * */
#define nya_entity_foreach_kind(kind, entity_name)                                                                                                   \
    for (NYA_EntityIter _nya_iter = _nya_entity_iter_kind((u32)(kind)); _nya_iter.entity != nullptr; _nya_entity_iter_advance(&_nya_iter))            \
        for (NYA_Entity* entity_name = _nya_iter.entity; entity_name != nullptr; entity_name = nullptr)

/** Walks every live entity with **every** bit of `flag_bits` set. Zero matches everything. */
#define nya_entity_foreach_flags(flag_bits, entity_name)                                                                                             \
    for (NYA_EntityIter _nya_iter = _nya_entity_iter_flags((u64)(flag_bits)); _nya_iter.entity != nullptr; _nya_entity_iter_advance(&_nya_iter))      \
        for (NYA_Entity* entity_name = _nya_iter.entity; entity_name != nullptr; entity_name = nullptr)

/*
 * ── Changing flags ──
 *
 * **Do not write NYA_Entity.flags directly.** The index carries a bitset per flag bit and is updated
 * here; a direct write leaves it disagreeing with the entity, and the symptom is a query quietly
 * skipping something rather than anything that looks like a bug at the point it was caused.
 *
 * Reading the field is fine.
 */

/** Sets every bit in `flags`, leaving the rest alone. */
NYA_API void nya_entity_flag_enable(NYA_Entity* entity, u64 flags);

/** Clears every bit in `flags`, leaving the rest alone. */
NYA_API void nya_entity_flag_disable(NYA_Entity* entity, u64 flags);

/** Replaces the whole flag word. */
NYA_API void nya_entity_flags_set(NYA_Entity* entity, u64 flags);

/*
 * ── Iteration internals ──
 *
 * Built by the macros above. Public only because a macro cannot call something hidden.
 */

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
 * Removes an entity immediately.
 *
 * Only safe outside iteration. Anything running during an update should use the deferred form,
 * which is the whole reason the simulation has a barrier.
 * */
NYA_API void nya_entity_despawn(NYA_EntityHandle entity);

/**
 * Marks an entity to be removed at the next simulation barrier.
 *
 * What game code should reach for. The entity stays valid for the rest of the tick, so nothing
 * iterating the world has the ground moved under it, and NYA_ENTITY_STATE_DESPAWNING says what is
 * about to happen. Despawning twice is harmless.
 * */
NYA_API void nya_entity_despawn_deferred(NYA_EntityHandle entity);

/** Null once the entity is gone. Never store the result; store the handle. */
NYA_API NYA_Entity* nya_entity_get(NYA_EntityHandle entity) __attr_no_discard;
NYA_API b8          nya_entity_is_valid(NYA_EntityHandle entity) __attr_no_discard;

NYA_API u32 nya_entity_count(void) __attr_no_discard;

/** Removes every entity. Runs on_despawn for each. */
NYA_API void nya_entity_clear(void);

/*
 * ─────────────────────────────────────────────────────────
 * ITERATION
 * ─────────────────────────────────────────────────────────
 */

/** Null for an empty slot, so a loop over slots has to check. Stops being useful past the high water mark. */
NYA_API NYA_Entity* nya_entity_at_slot(u32 index) __attr_no_discard;
NYA_API u32         nya_entity_slot_count(void) __attr_no_discard;

/**
 * Walks every live entity.
 *
 * Spawning during iteration is safe but the new entity may or may not be visited this pass;
 * despawning during iteration is only safe through nya_entity_despawn_deferred.
 * */
#define nya_entity_foreach(entity_name)                                                                                                              \
    for (u32 _nya_entity_slot = 0; _nya_entity_slot < nya_entity_slot_count(); _nya_entity_slot++)                                                   \
        for (NYA_Entity*(entity_name) = nya_entity_at_slot(_nya_entity_slot); (entity_name) != nullptr; (entity_name) = nullptr)
