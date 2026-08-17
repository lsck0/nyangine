/**
 * Hovering: the edges, their order, and who is holding the hover.
 *
 * `on_hover` is edge triggered — once with `entered` true when the cursor arrives, once with false when
 * it leaves — so almost everything worth checking here is about *transitions* rather than about hit
 * testing. The hit testing is nya_entity_click's, already covered by test_entity_click.c; what is new is
 * that a single stored handle has to turn a stream of per-frame positions into exactly the right pairs
 * of calls.
 *
 * The order within a move is the part a game actually depends on. A highlight is normally cleared on
 * leaving and set on entering, so moving the cursor straight from one entity to another must deliver
 * the leave *before* the enter or the new highlight is cleared by the old entity's handler. That is
 * asserted by recording the calls in sequence rather than by counting them.
 *
 * Headless: both solvers need an arena and a clock and nothing else.
 **/

#include "nyangine/nyangine.c"
#include "nyangine/nyangine.h"

#include "SDL3/SDL_init.h"

/** One recorded on_hover call, so ordering can be asserted rather than inferred from counts. */
typedef struct {
  NYA_EntityHandle entity;
  b8               entered;
} HoverCall;

#define MAX_CALLS 32

static HoverCall calls[MAX_CALLS];
static u32       call_count = 0;

static void on_hover(NYA_Entity* entity, b8 entered) {
  nya_assert(call_count < MAX_CALLS, "the test recorded more hover calls than it has room for");

  calls[call_count++] = (HoverCall){ .entity = entity->handle, .entered = entered };
}

static void reset(void) {
  call_count = 0;
  nya_memset(calls, 0, sizeof(calls));
}

/** Whether call `index` was `entered` on `entity`. */
static b8 call_was(u32 index, NYA_EntityHandle entity, b8 entered) {
  if (index >= call_count) return false;

  return calls[index].entity.index == entity.index && calls[index].entity.generation == entity.generation
      && calls[index].entered == entered;
}

/** A static box, so nothing falls out from under the cursor between two lookups. */
static NYA_EntityHandle spawn_2d(NYA_ConstCString name, f32x2 at, b8 hoverable) {
  NYA_EntityHandle entity = nya_entity_spawn(
    .name = name, .position = { at.x, at.y, 0.0F }, .on_hover = hoverable ? nya_callback(on_hover) : 0
  );

  b8 ok = nya_physics2d_body_attach(entity, .type = NYA_PHYSICS_BODY_STATIC, .shape = NYA_PHYSICS2D_SHAPE_BOX, .size = { 40.0F, 40.0F });
  nya_assert(ok);

  return entity;
}

s32 main(void) {
  _NYA_APP_INSTANCE = (NYA_App){ .initialized = true };

  b8 sdl_ok = SDL_Init(0);
  nya_assert(sdl_ok, "SDL_Init failed: %s", SDL_GetError());

  nya_system_callback_init();

  NYA_World* world = nya_world_create();
  (void)nya_world_set(world);

  defer nya_world_destroy(world);
  defer nya_system_callback_deinit();

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: nothing is hovered until something is
  // ─────────────────────────────────────────────────────────────────────────────
  {
    reset();

    nya_assert(!nya_entity_is_valid(nya_entity_hovered()), "a fresh world hovers nothing");

    // Zero is NYA_ENTITY_HANDLE_NONE and generations start at one, so a zeroed system already reads as
    // "nothing hovered" rather than as "entity zero" — which is what makes no explicit init necessary.
    NYA_EntityHandle nothing = nya_entity_hover((f32x2){ 500.0F, 500.0F });

    nya_assert(!nya_entity_is_valid(nothing));
    nya_assert(call_count == 0, "hovering empty space calls nothing");
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: arriving fires once, and resting on it does not fire again
  // ─────────────────────────────────────────────────────────────────────────────
  {
    reset();

    NYA_EntityHandle crate = spawn_2d("crate", (f32x2){ 100.0F, 100.0F }, true);

    NYA_EntityHandle hit = nya_entity_hover((f32x2){ 100.0F, 100.0F });

    nya_assert(hit.index == crate.index && hit.generation == crate.generation, "the hovered entity is reported");
    nya_assert(call_count == 1 && call_was(0, crate, true), "arriving fires entered once");
    nya_assert(nya_entity_hovered().index == crate.index, "and the system remembers who it is");

    /*
     * The point of edge triggering, and the reason this is cheap to call unconditionally.
     *
     * A cursor rests on one thing for many frames, and a game calling this every frame must not get a
     * callback every frame — otherwise every user of it would have to track the previous state itself,
     * which is exactly the work being centralised here.
     */
    for (u32 frame = 0; frame < 10; frame++) (void)nya_entity_hover((f32x2){ 100.0F + (f32)frame, 100.0F });

    nya_assert(call_count == 1, "resting on the same entity fires nothing further, got %u calls", call_count);

    nya_entity_despawn(crate);
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: leaving for empty space fires the false edge
  // ─────────────────────────────────────────────────────────────────────────────
  {
    reset();

    NYA_EntityHandle crate = spawn_2d("crate", (f32x2){ 100.0F, 100.0F }, true);

    (void)nya_entity_hover((f32x2){ 100.0F, 100.0F });
    (void)nya_entity_hover((f32x2){ 900.0F, 900.0F });

    nya_assert(call_count == 2, "two edges, got %u", call_count);
    nya_assert(call_was(0, crate, true) && call_was(1, crate, false), "entered then left, on the same entity");
    nya_assert(!nya_entity_is_valid(nya_entity_hovered()), "and nothing is hovered afterwards");

    nya_entity_despawn(crate);
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: moving between two entities delivers the leave before the enter
  // ─────────────────────────────────────────────────────────────────────────────
  {
    reset();

    NYA_EntityHandle left  = spawn_2d("left", (f32x2){ 0.0F, 0.0F }, true);
    NYA_EntityHandle right = spawn_2d("right", (f32x2){ 200.0F, 0.0F }, true);

    (void)nya_entity_hover((f32x2){ 0.0F, 0.0F });
    nya_assert(call_count == 1 && call_was(0, left, true));

    // Straight from one to the other, with no frame in between on empty space — which is the ordinary
    // case for two adjacent things and the one where the order matters.
    NYA_EntityHandle hit = nya_entity_hover((f32x2){ 200.0F, 0.0F });

    nya_assert(hit.index == right.index, "the new entity is the hovered one");
    nya_assert(call_count == 3, "one leave and one enter, got %u calls", call_count);

    /*
     * This ordering is the contract.
     *
     * A game that clears a highlight on leaving and sets one on entering composes correctly only in
     * this order. Reversed, the leave handler would run last and clear the highlight the enter handler
     * had just set, so the cursor would move between two entities and light up neither.
     */
    nya_assert(call_was(1, left, false), "the entity being left is told first");
    nya_assert(call_was(2, right, true), "and the entity being entered second");

    nya_entity_despawn(left);
    nya_entity_despawn(right);
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: an entity with no on_hover is still reported as hovered
  // ─────────────────────────────────────────────────────────────────────────────
  {
    reset();

    NYA_EntityHandle terrain = spawn_2d("terrain", (f32x2){ 0.0F, 0.0F }, false);

    NYA_EntityHandle hit = nya_entity_hover((f32x2){ 0.0F, 0.0F });

    /*
     * Deliberately unlike nya_entity_click, which answers NONE for an entity that declines.
     *
     * A click asks "did anything react"; a hover asks "what is under the cursor", and the terrain being
     * under it is a true and useful answer even though the terrain does not care. A caller wanting the
     * other question compares against its own entities.
     */
    nya_assert(hit.index == terrain.index && hit.generation == terrain.generation, "the handle is reported anyway");
    nya_assert(nya_entity_hovered().index == terrain.index, "and it holds the hover");
    nya_assert(call_count == 0, "but nothing is called");

    nya_entity_despawn(terrain);
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: hover_clear releases whatever is held, and is idempotent
  // ─────────────────────────────────────────────────────────────────────────────
  {
    reset();

    NYA_EntityHandle crate = spawn_2d("crate", (f32x2){ 100.0F, 100.0F }, true);

    (void)nya_entity_hover((f32x2){ 100.0F, 100.0F });

    // What the cursor leaving the window has to do. Without it the last hovered entity would keep its
    // highlight until the cursor came back, because nothing else would ever tell it otherwise.
    nya_entity_hover_clear();

    nya_assert(call_count == 2 && call_was(1, crate, false), "clearing fires the leave");
    nya_assert(!nya_entity_is_valid(nya_entity_hovered()));

    // Twice is not two leaves. Clearing an already-clear hover is a no-op, which matters because the
    // obvious place to call this is unconditionally on a focus-lost event.
    nya_entity_hover_clear();
    nya_entity_hover_clear();

    nya_assert(call_count == 2, "clearing again fires nothing, got %u calls", call_count);

    nya_entity_despawn(crate);
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: despawning the hovered entity releases the hover without calling it
  // ─────────────────────────────────────────────────────────────────────────────
  {
    reset();

    NYA_EntityHandle crate = spawn_2d("crate", (f32x2){ 100.0F, 100.0F }, true);

    (void)nya_entity_hover((f32x2){ 100.0F, 100.0F });
    nya_assert(call_count == 1 && nya_entity_hovered().index == crate.index);

    nya_entity_despawn(crate);

    /*
     * No leave, and the hover is dropped in the same call.
     *
     * on_despawn has already run by then, so telling the entity the cursor left would be a callback
     * into something half torn down for no benefit. What must not survive is the stored handle: leaving
     * it would make nya_entity_hovered report a despawned entity until the cursor next moved.
     */
    nya_assert(call_count == 1, "despawning does not fire the leave edge, got %u calls", call_count);
    nya_assert(!nya_entity_is_valid(nya_entity_hovered()), "and the hover is released immediately");

    // And the next arrival is a clean enter rather than being swallowed as "no change" against a stale
    // handle — which is what would happen if the despawn had left the handle in place and the slot were
    // reused.
    NYA_EntityHandle other = spawn_2d("other", (f32x2){ 100.0F, 100.0F }, true);

    (void)nya_entity_hover((f32x2){ 100.0F, 100.0F });

    nya_assert(call_count == 2 && call_was(1, other, true), "the replacement gets its own entered");

    nya_entity_despawn(other);
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: 3D — the ray overload drives the same edges
  // ─────────────────────────────────────────────────────────────────────────────
  {
    reset();

    NYA_EntityHandle cube = nya_entity_spawn(.name = "cube", .position = { 0.0F, 0.0F, 0.0F }, .on_hover = nya_callback(on_hover));

    nya_assert(nya_physics3d_body_attach(cube, .type = NYA_PHYSICS_BODY_STATIC, .shape = NYA_PHYSICS3D_SHAPE_BOX, .size = { 2.0F, 2.0F, 2.0F }));

    NYA_EntityHandle hit = nya_entity_hover((f32x3){ 0.0F, 0.0F, 10.0F }, (f32x3){ 0.0F, 0.0F, -20.0F });

    nya_assert(hit.index == cube.index && hit.generation == cube.generation, "the ray finds the cube");
    nya_assert(call_count == 1 && call_was(0, cube, true), "and fires entered, same as the 2D path");

    // Resting is resting in three dimensions too: a slightly different ray onto the same body is not a
    // new hover.
    (void)nya_entity_hover((f32x3){ 0.1F, 0.1F, 10.0F }, (f32x3){ 0.0F, 0.0F, -20.0F });
    nya_assert(call_count == 1, "a second ray onto the same body changes nothing, got %u calls", call_count);

    // Pointing away. The ray's length is its reach, so this is a miss and therefore a leave.
    (void)nya_entity_hover((f32x3){ 0.0F, 0.0F, 10.0F }, (f32x3){ 0.0F, 0.0F, 20.0F });

    nya_assert(call_count == 2 && call_was(1, cube, false), "a ray that hits nothing leaves");
    nya_assert(!nya_entity_is_valid(nya_entity_hovered()));

    nya_entity_despawn(cube);
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: a handler that despawns its own entity does not leave a dangling hover
  // ─────────────────────────────────────────────────────────────────────────────
  {
    reset();

    /*
     * The awkward case, and the reason the stored handle is committed before either callback runs.
     *
     * Both callbacks are game code and either may call back into the entity system. If the handle were
     * written after them, a handler asking nya_entity_hovered would be told the cursor is somewhere it
     * has already left — and the despawn below, which clears the hover, would then be overwritten by
     * the stale value on the way out.
     */
    NYA_EntityHandle crate = spawn_2d("crate", (f32x2){ 100.0F, 100.0F }, true);

    (void)nya_entity_hover((f32x2){ 100.0F, 100.0F });

    // Inside the callback the system already reports the new hover rather than the old one.
    nya_assert(nya_entity_hovered().index == crate.index);

    nya_entity_despawn(crate);

    nya_assert(!nya_entity_is_valid(nya_entity_hovered()));

    // Hovering the empty space it used to occupy must not resolve the stale handle or fire anything.
    NYA_EntityHandle after = nya_entity_hover((f32x2){ 100.0F, 100.0F });

    nya_assert(!nya_entity_is_valid(after), "the body went with the entity");
    nya_assert(call_count == 1, "and no further edge was produced, got %u calls", call_count);
  }

  nya_info("PASSED: test_entity_hover (0 failures)");

  return EXIT_SUCCESS;
}
