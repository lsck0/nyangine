/**
 * @file core_entity.h
 *
 * Entities: one flat struct per thing in the world, addressed by generational handle.
 *
 * Deliberately not an ECS. There are no components, no archetypes and no queries — every entity is
 * the same fat struct — unused fields just sit there. References are handles, not pointers, so the
 * implementation can change without touching call sites.
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
 * **Borrow pointers, hold handles.** `NYA_Entity*` stays valid while the entity lives — the table
 * never moves — but says nothing about whether it still exists. Anything that outlives the current
 * scope stores a handle instead.
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
// The entity carries its own appearance: sprite, animator and atlas by value.
#include "nyangine/renderer/render2d_sprite.h"

// Pointer only; core_window.h is a far heavier include than one opaque type is worth.
typedef struct NYA_Window NYA_Window;

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * CONSTANTS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/**
 * Entity slots, allocated up front and fixed rather than growable so an NYA_Entity* stays put —
 * on_update holds a raw pointer, and "spawn a bullet, then set my own cooldown" crossing a growth
 * boundary would be a silent use-after-free.
 *
 * Override with -DNYA_ENTITY_MAX=<n>. Costs sizeof(NYA_Entity) x n up front. If a hard ceiling is
 * wrong, use chunked blocks rather than a reallocating array — those grow without moving anything.
 * */
#ifndef NYA_ENTITY_MAX
#define NYA_ENTITY_MAX 8192
#endif

/**
 * World units across one cell of the spatial index. Too small and a query walks empty cells; too
 * large and every cell holds everything, degenerating to the linear scan it replaced — a few times a
 * typical entity's size works well (32 unit crates at the demo's scale, 128 here, roughly a dozen
 * entities per cell).
 * */
#ifndef NYA_ENTITY_GRID_CELL_SIZE
#define NYA_ENTITY_GRID_CELL_SIZE 128.0F
#endif

/**
 * Hash buckets; must be a power of two since the hash is masked rather than divided. A spatial hash,
 * not a dense array of cells, so the world has no bounds — collisions mean distant cells can share a
 * bucket, so every query re-checks the cell an entity actually sits in.
 * */
#ifndef NYA_ENTITY_GRID_BUCKETS
#define NYA_ENTITY_GRID_BUCKETS 4096
#endif

static_assert((NYA_ENTITY_GRID_BUCKETS & (NYA_ENTITY_GRID_BUCKETS - 1)) == 0, "NYA_ENTITY_GRID_BUCKETS must be a power of two");

/**
 * How far outside the view nya_system_entity_render still draws, in world units. The index knows
 * where an entity *is*, not how big it is, so one whose origin left the screen may still be half on
 * it and pop at the edge without a margin. Set to a little more than the largest entity's radius.
 * */
#ifndef NYA_ENTITY_RENDER_CULL_MARGIN
#define NYA_ENTITY_RENDER_CULL_MARGIN 128.0F
#endif

/**
 * Kinds the index has a bitset for; a kind at or above this still works, just without the shortcut.
 * One bitset per kind is NYA_ENTITY_MAX bits — a kilobyte at the default, so 64 kinds is 64
 * kilobytes, cheap enough not to think about.
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
     * Despawn requested, waiting on the simulation barrier. Set by nya_entity_despawn_deferred; the
     * entity stays fully valid until the barrier runs, so this flag is how mid-update code can tell
     * the difference.
     * */
    NYA_ENTITY_STATE_DESPAWNING = 1 << 3,
};

/**
 * A uniform grid over entity positions, rebuilt every tick. Intrusive and allocation free: `buckets`
 * holds the first entity slot in each bucket, `next` chains the rest — insert is two writes, clearing
 * is one memset of `buckets`, nothing allocated after init. That's what makes a full rebuild every
 * tick cheaper than incremental maintenance for a world where everything moves.
 *
 * **Indexes positions, not bounds.** An entity sits in exactly one cell, the one its origin falls in,
 * however large it is. An overlap query has to expand its rectangle by the largest entity's radius.
 *
 * Physics already broadphases everything with a rigid body; this covers what it cannot — entities
 * with no body, and queries filtered by the game's `type`, which Box2D knows nothing about.
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
 * Bitsets saying which slots hold which kind and which flags — what makes "every camera" cost the
 * number of cameras rather than the number of entities. Walking the slot table and testing `type` got
 * slower with the world: at eight hundred crates, finding the two cameras meant eight hundred
 * comparisons, every system, every tick. A bitset turns that into a scan of NYA_ENTITY_BITSET_WORDS
 * words (128 at the default), and each zero word skips sixty four slots at once — in a world of
 * crates the camera bitset is almost entirely zero.
 *
 * Maintained on spawn, despawn and every flag change, rather than rebuilt once a tick like the
 * spatial grid: a grid that is a tick stale gives slightly wrong positions (invisible), while a stale
 * index makes a query *miss an entity* — a bug that is miserable to find.
 *
 * The price: flags cannot be written directly. Use nya_entity_flag_enable and friends;
 * `NYA_Entity.flags` is read-only to everyone else.
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
     * Words worth scanning, from the entity table's high water mark. Slots past it have never held
     * anything — scanning them is scanning guaranteed zeroes. A world of a few hundred entities is
     * five words, not a hundred and twenty eight.
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
     * Indices of free slots, most recently freed first — a stack rather than a scan for the first
     * empty slot, since that scan is what turns spawning a few thousand entities from linear into
     * quadratic.
     * */
    u32* free_slots;
    u32  free_count;

    u32 count;

    /** Highest slot ever occupied. Iteration stops here rather than at NYA_ENTITY_MAX. */
    u32 high_water_mark;

    NYA_EntityGrid  grid;
    NYA_EntityIndex index;

    /**
     * Entities that currently have a parent.
     *
     * Kept so a world with no hierarchy — which is most of them, and every world this engine had
     * before there was one — pays a single comparison per tick rather than a walk over every slot.
     * */
    u32 parented_count;

    /**
     * Who the cursor is currently on, or NYA_ENTITY_HANDLE_NONE. Held here rather than as a bit on
     * the entity because the *transition* matters: firing "left" requires knowing who was hovered
     * before, which a per-entity bit cannot describe without scanning every entity for a stale flag
     * each frame.
     *
     * Zero is NYA_ENTITY_HANDLE_NONE and generations start at one, so a zeroed system already means
     * nothing is hovered.
     * */
    NYA_EntityHandle hovered;
};

/*
 * ─────────────────────────────────────────────────────────
 * APPEARANCE
 * ─────────────────────────────────────────────────────────
 *
 * What an entity looks like, so most entities need no on_render at all. Before this, drawing meant a
 * callback reading the transform and calling the renderer by hand — the same eight lines per kind of
 * thing, and where z-ordering and batching went to die: nya_system_entity_render walked the spatial
 * grid in bucket order and drew immediately, so draw order was neither spatial nor stable and shared
 * textures batched only by luck. Giving the entity its appearance lets the system sort before
 * drawing; see nya_system_entity_render_in.
 *
 * on_render still exists and runs *after* the visual — for the health bar, the debug outline,
 * whatever no enum covers.
 */

enum NYA_EntityVisualKind {
    /** Nothing is drawn automatically. on_render is the whole appearance, as it was before. */
    NYA_ENTITY_VISUAL_NONE = 0,

    /** One still image: `sprite`, drawn at the entity's position. */
    NYA_ENTITY_VISUAL_SPRITE,

    /**
     * A sprite whose frame comes from `animator` playing over `atlas`. Advanced by
     * nya_system_entity_update; its signals are delivered to on_animation, so an attack's hit frame
     * arrives as a callback without the game running a timer.
     * */
    NYA_ENTITY_VISUAL_ANIMATION,

    /**
     * A solid box, through render3d, with the entity's own rotation — so a 3D entity is as little
     * work as a 2D one. Drawn only while a 3D camera is active (see nya_render3d_begin); silently
     * skipped otherwise, since there is no projection to draw it through.
     * */
    NYA_ENTITY_VISUAL_CUBE,

    NYA_ENTITY_VISUAL_KIND_COUNT,
};

/**
 * How an entity draws itself. Zeroed means NONE, costing nothing to ignore. A tagged union would be
 * smaller, but an animation needs sprite, atlas and animator at once, and the overlapping fields (a
 * cube's size vs. an atlas's frame size) are two floats against a struct — flat is readable and the
 * entity table is preallocated anyway.
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
     * Draw order among everything the same render call is drawing; lower draws first, so behind.
     * Explicit rather than taken from `position.z`: a top-down game sorts by `position.y` so
     * something further down is in front, while z is either unused or a real third axis — sorting by
     * the wrong one collapses every sprite to one plane, back at bucket order.
     *
     * Ties are broken by texture, which keeps sorting from destroying the batching — see
     * nya_system_entity_render_in.
     *
     * Ignored when `y_sorted` is set.
     * */
    f32 z_order;

    /**
     * Take the draw order from the entity's position instead of from `z_order`.
     *
     * What a top-down or 2.5D scene wants: something standing lower on the screen is nearer the
     * camera and has to draw in front, and that relationship changes every time anything moves. With
     * this set the sort key is `position.y + y_sort_anchor`, recomputed each frame, so a character
     * walking around a tree passes behind it and then in front of it without anything managing depth.
     *
     * Per entity rather than per scene, because a scene mixes the two: the ground and the shadows
     * under everything sort by a fixed layer, and only the things standing on the ground sort by
     * where they stand.
     * */
    b8 y_sorted;

    /**
     * Added to `position.y` before sorting: where this entity's **feet** are, relative to its origin.
     *
     * The number that makes y-sorting look right rather than merely work. A sprite is normally
     * anchored at its centre or its top left, and sorting by that puts a tall tree behind a short
     * character standing beside it — what decides who is in front is where each of them *touches the
     * ground*, which is the bottom of the sprite and not its middle.
     *
     * Zero sorts by the origin, which is correct for something already anchored at its base.
     * */
    f32 y_sort_anchor;
};

/*
 * ─────────────────────────────────────────────────────────
 * ENTITY STRUCT
 * ─────────────────────────────────────────────────────────
 */

struct NYA_Entity {
    NYA_EntityHandle handle;

    /**
     * Engine owned lifecycle bits: active, visible, static, despawning. See NYA_EntityState. Named
     * `state` rather than `flags` so `flags` can be the game's — the field a game actually reaches
     * for.
     * */
    NYA_EntityState state;

    /**
     * What kind of thing this is. Game defined; core never interprets it. The tag half of a tagged
     * fat struct — code switches on it to decide which fields below mean anything. Same arrangement
     * as NYA_SimRecord.type, for the same reason: the engine has no business enumerating what a game
     * contains.
     * */
    u32 type;

    /**
     * Whatever the game wants to be true of this entity. Game defined; core never interprets it —
     * the companion to `type`: that says what an entity *is*, this says what is *true* of it. Both
     * are plain integers rather than pointers so an entity spawned/despawned by the hundred needs no
     * allocation to carry either.
     *
     * Sixty four bits, separate from `type` rather than sharing its high bits: a flag word runs out
     * sooner than a kind enum, and packing both into one field would make every
     * `entity->type == SOMETHING` comparison silently false the day the first flag was set.
     * */
    u64 flags;

    /** Not owned. Point it at a literal or something that outlives the entity. */
    NYA_ConstCString name;

    /* ── transform ── */

    f32x3          position;
    NYA_Quaternion rotation;
    f32x3          scale;

    /* ── hierarchy ──
     *
     * A scene graph over the flat table, by handle rather than by pointer — the table never moves, but
     * a pointer says nothing about whether the entity still exists and these links outlive despawns.
     *
     * ⚠ **The three transform fields above stay the WORLD transform.** That is deliberate and it is
     * the whole design: physics writes `position`, rendering and every query read it, and making them
     * local would have meant changing every one of those to compose a matrix first. Instead a parented
     * entity keeps its offset from its parent in the three `local_*` fields, and
     * nya_system_entity_transforms_update writes `position`/`rotation`/`scale` from the parent's.
     *
     * The children are an intrusive singly-linked list — `first_child` plus a `next_sibling` on each —
     * so a hierarchy of any shape allocates nothing. ⚠ The order of siblings is the reverse of the
     * order they were parented in: a new child is pushed at the front, because appending would mean
     * walking the list every time. Nothing about drawing or updating depends on sibling order.
     */

    NYA_EntityHandle parent;
    NYA_EntityHandle first_child;
    NYA_EntityHandle next_sibling;

    /** Direct children only, not the whole subtree. Maintained by the parenting calls. */
    u32 child_count;

    /**
     * This entity's transform **relative to its parent**. Meaningless while `parent` is NONE.
     *
     * Captured when the entity is parented, from the world transform it had at that moment — so
     * parenting never moves anything. Change these to move a child within its parent;
     * writing `position` directly on a parented entity is overwritten by the next propagation.
     * */
    f32x3          local_position;
    NYA_Quaternion local_rotation;
    f32x3          local_scale;

    /* ── motion ── */

    f32x3 velocity;
    f32x3 angular_velocity;

    /* ── interpolated motion ──
     *
     * The other way to move something: velocity says how fast, this says where to end up and by
     * when. Set through nya_entity_move_to rather than field by field.
     *
     * The interpolation itself is a core_tween.h tween — the same pool, curves, timing and pooling
     * everything else animates through — rather than a second easing loop living on the entity. What
     * stays here is only what a tween cannot know: the staging value it writes into, and the handle
     * that says a move is running.
     *
     * Not exclusive with velocity, but they fight: while a move runs the staged position is applied
     * outright, so any velocity integrated that tick is overwritten. See nya_entity_move_to.
     */

    /**
     * Where the tween writes. Applied to `position` — or to a kinematic body's velocity — once per
     * tick by nya_system_entity_update.
     *
     * Staged rather than tweened straight into `position` because a body-backed entity must not have
     * its transform written from outside: the solver owns it, and the move has to reach it as a
     * velocity instead. One indirection buys both cases the identical curve.
     * */
    f32x3 move_position;

    /** The running move, or NYA_TWEEN_NONE. Generational, so an arrived move stops resolving. */
    NYA_Tween move_tween;

    /**
     * Set for one tick after a body-backed move arrives, to say its velocity still needs clearing.
     *
     * The solver steps before nya_system_entity_update, so a velocity set during the arrival tick is
     * not consumed until the next one — clearing it immediately drops the move's last step and leaves
     * a kinematic platform permanently short of its target.
     * */
    b8 move_settling;

    /* ── physics ──
     *
     * Two solvers, two fields; an entity is expected to use at most one. Separate rather than a
     * tagged union because they're separate simulations — Box2D and Box3D each own a world and
     * neither knows the other exists. Both being attached is unchecked and not useful: two solvers
     * writing one transform is the same fight velocity integration steps aside from, decided by
     * whichever runs second.
     */

    /**
     * The 2D rigid body simulating this entity, if it has one. See physics2d.h. Zeroed with
     * `attached` false for an ordinary entity moved by velocity integration; while attached, the
     * solver owns the transform and integration is skipped. Attach with nya_physics2d_body_attach
     * rather than filling this in directly — the body must exist in the world before the entity can
     * point at it, and despawning destroys it.
     * */
    NYA_Physics2DBody physics2d;

    /**
     * The 3D rigid body, if it has one. See physics3d.h. Same contract as the 2D one against a
     * different solver: attach with nya_physics3d_body_attach; while attached the solver owns
     * position *and* the full rotation quaternion, and despawning destroys it.
     * */
    NYA_Physics3DBody physics3d;

    /* ── appearance ── */

    /**
     * What this entity looks like. Drawn by nya_system_entity_render before its on_render runs.
     * Zeroed is NYA_ENTITY_VISUAL_NONE (draws nothing), so an entity with an on_render predating
     * this feature behaves exactly as it did.
     * */
    NYA_EntityVisual visual;

    /**
     * What this entity emits. Zeroed emits nothing — the common case, costing nothing. Read by
     * nya_system_entity_lights rather than drawn by the entity itself, since lighting is one pass
     * over the whole scene and cannot be assembled a draw call at a time.
     * */
    NYA_Light2D light;

    /**
     * Whatever the game needs to hang off this entity. Not owned, not freed on despawn — the engine
     * has no business knowing what a player or a projectile is, and this is the seam where that
     * stays true.
     * */
    void* user_data;

    /* ── per entity callbacks, by handle so they survive a hot reload ── */

    NYA_CallbackHandle on_spawn;
    NYA_CallbackHandle on_despawn;
    NYA_CallbackHandle on_update;

    /**
     * Draws this entity. Run by nya_system_entity_render, which a game calls itself. Unlike the
     * other three, not driven by the engine's own loop — *when* to draw is a decision only the game
     * can make, since entities belong inside whatever camera and render target the drawing layer
     * set up.
     * */
    NYA_CallbackHandle on_render;

    /**
     * Struck something hard enough to count. Run by the physics step, once per hit per entity — both
     * sides are called, each with the other as `other`, so anything that should happen *once* per
     * collision must act only for the lower of the two handle indices. Only hits above
     * nya_physics2d_hit_threshold arrive here; a resting stack generates contacts every step and none
     * of them are events. See physics2d.h.
     * */
    NYA_CallbackHandle on_collision;

    /**
     * An animation started, looped, hit a marked frame, or finished. Run by
     * nya_system_entity_update, once per signal, only for an entity whose visual is ANIMATION — the
     * hook an attack hangs off: play the swing on input, land the hit when the frame carrying the
     * marker comes up, a frame number the artist owns and can retime without the game noticing.
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
     * Pointed at and clicked. Run by nya_entity_click, which a game calls itself. Not driven by the
     * engine's input handling, for the same reason on_render isn't driven by its frame loop: a click
     * arrives in screen pixels and only the game knows which camera turns it into a world point or,
     * in 3D, a ray.
     *
     * Works in both dimensions now. It used to work in one: nya_entity_click took an f32x2 and asked
     * nya_physics2d_entity_at, so a 3D entity's on_click could never fire — the 3D scene in gnyame
     * did its own raycast and never called the callback. See the ray-taking overload.
     * */
    NYA_CallbackHandle on_click;

    /**
     * Pointed at, without clicking. Run by nya_entity_hover, which a game calls itself. Driven by the
     * game for the same reason on_click is: hovering is a cursor in screen pixels, and only the game
     * knows which camera turns it into a world point or ray.
     *
     * **Edge triggered**: runs once with `entered` true on arrival and once with false on leaving,
     * not every frame the cursor rests — that's what the callback is almost always used for
     * (highlight on/off, tooltip show/hide). A game that wants "while hovered" has
     * nya_entity_hovered, every frame.
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
 * `other` is whatever it struck, null when that body has no entity behind it. The hit is the
 * engine's, valid only for the tick it is delivered — copy anything that must outlive the call. See
 * NYA_PhysicsHit.
 * */
typedef void (*NYA_EntityOnCollisionFn)(NYA_Entity* entity, NYA_Entity* other, const NYA_PhysicsHit* hit);

/**
 * `world_point` is where the click landed, in three dimensions. f32x3 for both, z zero for a 2D
 * click — not a fiction: the 2D world *is* the z = 0 plane, the same reasoning and shape as
 * NYA_PhysicsHit. See physics_types.h. One signature lets a single on_click serve an entity whichever
 * solver simulates it, which matters for a 2D interface over a 3D scene.
 * */
typedef void (*NYA_EntityOnClickFn)(NYA_Entity* entity, f32x3 world_point, u8 button);

/**
 * `entered` is true when the cursor arrived, false when it left. No world point, unlike
 * NYA_EntityOnClickFn: a click happens *at* a place, but hovering is a state with two edges and the
 * leaving position is by definition somewhere the entity no longer is. Wants it every frame? Use
 * nya_entity_hovered instead.
 * */
typedef void (*NYA_EntityOnHoverFn)(NYA_Entity* entity, b8 entered);

/** The signal is by value: it is four words, and it does not outlive the call. */
typedef void (*NYA_EntityOnAnimationFn)(NYA_Entity* entity, NYA_SpriteAnimationSignal signal);

/**
 * What an entity starts as; everything optional. Scale defaults to 1 and rotation to identity rather
 * than zero, since a zeroed transform produces an entity of no size facing nowhere.
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

    /** Draws this entity. Run by nya_system_entity_render, which a game calls itself. See NYA_Entity.on_render. */
    NYA_CallbackHandle on_render;
    NYA_CallbackHandle on_collision;
    NYA_CallbackHandle on_click;
    NYA_CallbackHandle on_hover;
    NYA_CallbackHandle on_animation;

    /** What it looks like. Zeroed draws nothing — what an entity with an on_render wants. */
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
 * Runs on_render for every visible entity **that is on screen**. Called by the game, from inside the
 * layer that owns the camera, not by the engine's frame loop — an entity must be drawn in the same
 * coordinate space as the world around it, and the engine can't know which layer that is.
 *
 * The visible region comes from the target's size and whatever camera is set: no camera means the
 * target in screen pixels, a camera means the world rectangle mapping onto it. All four corners are
 * transformed, not two, so a rotated camera still gets a rectangle containing its view. Widened by
 * NYA_ENTITY_RENDER_CULL_MARGIN, since the grid indexes an entity's *position* and something large
 * enough can be visible while its origin is not — see NYA_EntityGrid.
 *
 * Draw order is bucket order (slot order before culling existed), neither spatial nor stable across a
 * rebuild — anything needing a depth sort should keep its own list. Entities without
 * NYA_ENTITY_STATE_VISIBLE are skipped.
 * */
NYA_API void nya_system_entity_render(NYA_Window* window);

/**
 * Finds the entity whose body covers `world_point` and runs its on_click. Returns who was clicked, or
 * NYA_ENTITY_HANDLE_NONE when the point hit nothing — or hit something with no on_click, which is how
 * an entity declines to be clickable (no separate flag, since a second way to say it is a second way
 * for the two to disagree).
 *
 * Topmost body wins; the click does not fall through, matching how nya_physics2d_entity_at answers.
 * The callback receives the point as f32x3 with z zero. See NYA_EntityOnClickFn.
 * */
NYA_API NYA_EntityHandle nya_entity_click(f32x2 world_point, u8 button) __attr_overloaded;

/**
 * The same, for a 3D scene: the first entity along a ray gets its on_click. Two overloads rather than
 * one function, since a click is a different shape in each dimension — in 2D the screen *is* the
 * world plane and the click names a point, in 3D it names a line into the volume, the same split
 * nya_physics2d_entity_at and nya_physics3d_raycast already make.
 *
 * ```c
 * NYA_Render3DRay ray = nya_render3d_screen_ray(window, (f32x2){ mouse->x, mouse->y });
 * nya_entity_click(ray.origin, ray.direction * 100.0F, mouse->button);
 * ```
 *
 * `direction` need not be normalised; its length is how far the click reaches, so a game decides
 * whether something across the map is clickable. The callback gets the point on the struck surface,
 * not the ray's origin. Closest hit wins, so a crate behind another crate cannot take the click.
 * */
NYA_API NYA_EntityHandle nya_entity_click(f32x3 origin, f32x3 direction, u8 button) __attr_overloaded;

/**
 * Tells the entity system where the cursor is, running on_hover for whatever moved under or out from
 * under it. Returns who is hovered now, or NYA_ENTITY_HANDLE_NONE. Called once a frame with the
 * cursor in world space; calling it repeatedly on the same entity does nothing, since callbacks fire
 * on the edges — cheap to call unconditionally, which is the intended use.
 *
 * The entity being left gets `false` before the entity being entered gets `true`, so clearing a
 * highlight on leaving cannot clear the newly set one. Unlike nya_entity_click this returns the
 * handle even with no on_hover: the question here is "what is under the cursor", worth answering
 * whether or not anything reacted.
 * */
NYA_API NYA_EntityHandle nya_entity_hover(f32x2 world_point) __attr_overloaded;

/** The same, for a 3D scene: the first entity along the ray is the hovered one. See nya_entity_click. */
NYA_API NYA_EntityHandle nya_entity_hover(f32x3 origin, f32x3 direction) __attr_overloaded;

/**
 * Says the cursor is on nothing, running the current entity's on_hover with false. For when the
 * pointer leaves the window, a menu opens over the world, or the game stops accepting pointer input —
 * without it the last hovered entity would keep its highlight until the cursor came back.
 * */
NYA_API void nya_entity_hover_clear(void);

/** Who the cursor is on, or NYA_ENTITY_HANDLE_NONE. What to read for "while hovered" behaviour. */
NYA_API NYA_EntityHandle nya_entity_hovered(void) __attr_no_discard;

/**
 * The value an entity sorts on, which is either its `z_order` or where its feet are.
 *
 * Public because a scene that draws something outside the entity system — a tilemap object, a
 * decal — has to be able to interleave it with the entities, and the only way to do that is to
 * compute the same number the sort uses. See NYA_EntityVisual.y_sorted.
 * */
NYA_API f32 nya_entity_sort_key(const NYA_Entity* entity) __attr_no_discard;

/**
 * Runs on_render only for entities whose position falls inside `min`..`max` — what a camera wants: at
 * eight hundred crates the untargeted form calls on_render eight hundred times regardless of how many
 * are on screen. Positions, not bounds — see NYA_EntityGrid. Expand the rectangle by the largest
 * entity's radius or things straddling the edge pop out one frame early.
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
 * **Where the work happens.** Nothing is recomputed when a parent moves; the whole hierarchy is
 * propagated once per tick at the end of nya_system_entity_update. So a child's `position` is
 * correct for everything that reads it *after* that — rendering, queries, the next tick's callbacks —
 * and is one tick stale for an on_update that runs before its parent's does in the same tick. Call
 * nya_entity_transform_sync when that matters, which is rare and usually means aiming something.
 */

/**
 * Makes `child` follow `parent`, without moving it.
 *
 * The child's current world transform is kept and its offset from the parent captured, which is what
 * "parent" almost always means — attaching a turret to a tank should not teleport the turret to the
 * tank's origin. Pass NYA_ENTITY_HANDLE_NONE as the parent to unparent, which likewise leaves the
 * child exactly where it is.
 *
 * Refused, and logged, when it would make a cycle: an entity cannot be its own ancestor, and neither
 * can it be parented to itself. False for that, or for a handle that does not resolve.
 * */
NYA_API b8 nya_entity_parent_set(NYA_EntityHandle child, NYA_EntityHandle parent);

/** Unparents, keeping the world transform. The same as parenting to NYA_ENTITY_HANDLE_NONE. */
NYA_API void nya_entity_parent_clear(NYA_EntityHandle child);

/** The parent, or NYA_ENTITY_HANDLE_NONE for a root or for nothing at all. */
NYA_API NYA_EntityHandle nya_entity_parent(const NYA_Entity* entity) __attr_no_discard;

/**
 * The direct children, written into `out`. Returns how many there are, which may exceed `capacity`.
 *
 * Direct children only — a subtree is this applied recursively, and doing it here would need an
 * allocation for the queue. Pass a null `out` with a zero capacity to count them, or read
 * `child_count`.
 * */
NYA_API u32 nya_entity_children(const NYA_Entity* entity, OUT NYA_EntityHandle* out, u32 capacity);

/** Whether `ancestor` is above `descendant` anywhere in the tree. What parenting checks for cycles. */
NYA_API b8 nya_entity_is_ancestor(NYA_EntityHandle ancestor, NYA_EntityHandle descendant) __attr_no_discard;

/**
 * Rewrites `entity` and everything under it from their parents' transforms, immediately.
 *
 * The per-tick propagation is what normally does this, and it runs at the end of the entity update —
 * so this is for the case that cannot wait: something in an on_update that moves a parent and then
 * has to read a child's world position in the same tick.
 * */
NYA_API void nya_entity_transform_sync(NYA_EntityHandle entity);

/**
 * Propagates every parented entity's transform from its parent's. Called by nya_system_entity_update.
 *
 * Roots first, then down, so a chain three deep resolves in one pass rather than one level per tick.
 * Returns immediately in a world where nothing is parented.
 * */
NYA_API void nya_system_entity_transforms_update(void);

/**
 * The entity's world transform as a matrix, for handing to a renderer.
 *
 * Built from `position`, `rotation` and `scale` — which are the world transform whether or not the
 * entity has a parent, so this needs no hierarchy walk.
 * */
NYA_API f32_4x4 nya_entity_world_matrix(const NYA_Entity* entity) __attr_no_discard;

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
 * The origin is captured here, so the curve runs from where the entity is *now* to the target —
 * calling this again mid-move restarts from the current position rather than resuming.
 *
 * While the move runs it writes `position` outright, once per tick, before on_update; velocity is
 * still integrated the same tick and then overwritten, so an entity should use one or the other, not
 * both.
 *
 * A duration of zero or less teleports to `target` immediately with no move running, so
 * `nya_entity_move_to(e, p, 0, ...)` reads like every other move.
 *
 * **This is a core_tween.h tween underneath**, which has two consequences worth knowing. Moves come
 * out of the shared pool, so a world that starts more than `NYA_TWEEN_MAX` at once has the excess
 * refused and logged rather than silently queued. And `nya_tween_cancel_target(&entity->move_position)`
 * stops a move from outside, which is what despawn does.
 *
 * **Bodies.** An entity the solver owns cannot have its position written from outside without the two
 * disagreeing:
 *
 * - no body — `position` is written directly.
 * - kinematic body — the per tick delta becomes a linear velocity given to the solver, so a moving
 *   platform pushes what stands on it rather than passing through it.
 * - dynamic or static body — ignored, and logged once. Push a dynamic body with an impulse; a static
 *   one is static.
 * */
NYA_API void nya_entity_move_to(NYA_Entity* entity, f32x3 target, f32 duration_s, NYA_EaseType ease);

/**
 * The same move with the rest of NYA_TweenOptions available: a delay before it starts, a repeat
 * count, yoyo, and a completion callback that runs on arrival.
 *
 * `options.ease` is the curve, so this subsumes nya_entity_move_to rather than sitting beside it.
 * The completion callback is handed `&entity->move_position` — the tween's write target — since that
 * is what NYA_TweenOnCompleteFn receives; recover the entity from it if the callback needs more than
 * "the move ended".
 *
 * ⚠ `repeat` restarts from the position the move began at, not from the target, so a repeating move
 * snaps back before each run rather than ping-ponging. Pass `.yoyo = true` for the ping-pong.
 * */
NYA_API void nya_entity_move_to_with_options(NYA_Entity* entity, f32x3 target, f32 duration_s, NYA_TweenOptions options);

/**
 * The same move, with duration derived from a constant speed in world units per second. Forced to
 * NYA_EASE_LINEAR — a speed and an easing curve are contradictory, since an eased move only matches
 * the requested speed on average. A speed of zero or less is a teleport, matching a duration of zero.
 * */
NYA_API void nya_entity_move_to_at_speed(NYA_Entity* entity, f32x3 target, f32 world_units_per_second);

/**
 * Abandons the move where it is; the entity keeps the position reached. Harmless on an entity that
 * is not moving, so it's safe to call from a state change that doesn't know.
 * */
NYA_API void nya_entity_move_stop(NYA_Entity* entity);

/** Whether a move is running. False the tick after it arrives. */
NYA_API b8 nya_entity_moving(const NYA_Entity* entity) __attr_no_discard;

/**
 * How far into the move the entity is, from 0 at origin to 1 at target — the eased value, not raw
 * time fraction, so it's the same number the position was built from. One for an entity that is not
 * moving, since "not moving" and "arrived" are the same state once the move ends.
 * */
NYA_API f32 nya_entity_move_progress(const NYA_Entity* entity) __attr_no_discard;

/*
 * ─────────────────────────────────────────────────────────
 * LIGHTS
 * ─────────────────────────────────────────────────────────
 */

/**
 * Collects every entity light that could reach the visible region, brightest first. Written into
 * `out` in world coordinates, capped at `capacity`, returns how many. Sorted so that when a scene has
 * more lights than the pass can carry, the ones dropped are the ones nobody would have noticed.
 *
 * Called by nya_render2d_lights_apply; exposed separately for a game lighting a render texture or
 * feeding a shader of its own, so it needn't walk the entity table by hand.
 *
 * Widened by each light's own radius before the test — a light whose *origin* is off screen still
 * spills onto it, the same margin nya_system_entity_render applies to sprites.
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
 * Rebuilds the spatial index from every live entity's current position. Called at the top of
 * nya_system_entity_update, after the physics step writes this tick's transforms, so queries during
 * update or render see the world as it is now. A game doesn't call this normally — but must, if it
 * moves entities directly and then queries in the same tick, since nothing else notices positions
 * changed.
 * */
NYA_API void nya_system_entity_grid_rebuild(void);

/**
 * Entity handles whose positions fall inside the rectangle, written into `out`. Returns how many,
 * capped at `capacity` — getting exactly `capacity` back means it was truncated. Order is bucket
 * order, neither spatial nor stable across a rebuild; sort the result if it matters.
 * */
NYA_API u32 nya_entity_query_rect(f32x2 min, f32x2 max, OUT NYA_EntityHandle* out, u32 capacity);

/** The circle inscribed in the same search, tested exactly rather than by its bounding box. */
NYA_API u32 nya_entity_query_radius(f32x2 center, f32 radius, OUT NYA_EntityHandle* out, u32 capacity);

/** nya_entity_query_rect, keeping only entities whose `type` matches. What the broadphase cannot do. */
NYA_API u32 nya_entity_query_kind(f32x2 min, f32x2 max, u32 type, OUT NYA_EntityHandle* out, u32 capacity);

/**
 * nya_entity_query_rect, keeping only entities with **every** bit of `flags` set — a kind says what a
 * thing is, flags say what is true of it, and a system usually wants the second ("everything
 * flammable near the fire", not "every crate near the fire").
 * */
NYA_API u32 nya_entity_query_flags(f32x2 min, f32x2 max, u64 flags, OUT NYA_EntityHandle* out, u32 capacity);

/*
 * ─────────────────────────────────────────────────────────
 * 3D QUERIES
 * ─────────────────────────────────────────────────────────
 */

/**
 * Every entity whose position is inside the box. The 2D queries above test x and y only, correct for
 * side-on/top-down and silently wrong in 3D — a marquee select would take everything in the column
 * above and below the box too.
 *
 * Position only, not bounds: an entity is a point here. Testing a model's extents means resolving its
 * mesh, which the entity system deliberately doesn't know about — narrow the result with
 * nya_render3d_mesh_bounds if needed.
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
 * What clicking in a 3D viewport does: nya_render3d_screen_ray gives the ray, this gives the thing
 * under the pointer. Spheres rather than boxes since an entity has no bounds here (see
 * nya_entity_query_box) — a sphere is the one shape a point can be given without asking the renderer
 * anything. `out_distance` receives the distance along the ray, for comparing hits across calls with
 * different radii.
 * */
NYA_API NYA_EntityHandle nya_entity_query_ray(f32x3 origin, f32x3 direction, f32 radius, OUT f32* out_distance) __attr_no_discard;

/*
 * ── Iterating the whole world ──
 *
 * The spatial queries above answer "what is near here"; these answer "what exists" — for systems
 * that aren't spatial at all: scoring, ageing, saving, counting.
 *
 * Macros rather than functions returning arrays, so there's no buffer to size or truncate. Same
 * iteration rules as nya_entity_foreach: spawning during one is safe, despawning only through
 * nya_entity_despawn_deferred.
 */

/**
 * Walks every live entity whose `type` is `kind`, through the index. Costs the number of matches plus
 * a scan of the kind's bitset, not the size of the world. Same iteration rules as nya_entity_foreach:
 * spawning during one is safe (the new entity may or may not be visited); despawning only through
 * nya_entity_despawn_deferred.
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
 * **Do not write NYA_Entity.flags directly.** The index carries a bitset per flag bit, updated only
 * here — a direct write leaves it disagreeing with the entity, and the symptom is a query quietly
 * skipping something, not anything that looks like a bug at the point it was caused. Reading the
 * field is fine.
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
 * Removes an entity immediately. Only safe outside iteration — anything running during an update
 * should use the deferred form, which is the whole reason the simulation has a barrier.
 * */
NYA_API void nya_entity_despawn(NYA_EntityHandle entity);

/**
 * Marks an entity to be removed at the next simulation barrier. What game code should reach for: the
 * entity stays valid for the rest of the tick, so nothing iterating the world has the ground moved
 * under it. NYA_ENTITY_STATE_DESPAWNING says what's about to happen; despawning twice is harmless.
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
 * Walks every live entity. Spawning during iteration is safe but the new entity may or may not be
 * visited this pass; despawning is only safe through nya_entity_despawn_deferred.
 * */
#define nya_entity_foreach(entity_name)                                                                                                              \
    for (u32 _nya_entity_slot = 0; _nya_entity_slot < nya_entity_slot_count(); _nya_entity_slot++)                                                   \
        for (NYA_Entity*(entity_name) = nya_entity_at_slot(_nya_entity_slot); (entity_name) != nullptr; (entity_name) = nullptr)
