/**
 * The scene graph over the flat entity table: parenting, propagation, and what despawn does to it.
 *
 * The design decision everything here checks is that `position`/`rotation`/`scale` stay the **world**
 * transform. Physics writes them and every query reads them, so parenting cannot make them local —
 * instead a child keeps its offset and the propagation pass writes them from the parent's. The
 * consequences are what is asserted: parenting never moves anything, moving a parent carries its
 * children, and moving a child does not disturb its parent.
 *
 * The two cases that would be silent failures rather than wrong pictures are the cycle refusal —
 * which would make the propagation walk recurse until the stack ran out — and despawn leaving a
 * sibling list naming a slot that has been reused.
 *
 * Headless: entities are plain data.
 **/

#include "nyangine/nyangine.c"
#include "nyangine/nyangine.h"

#include "SDL3/SDL_init.h"

/** Positions are composed through quaternions, so exact equality is the wrong test. */
static b8 near_enough(f32 a, f32 b) {
    return fabsf(a - b) < 0.001F;
}

static b8 same_handle(NYA_EntityHandle a, NYA_EntityHandle b) {
    return a.index == b.index && a.generation == b.generation;
}

/** One tick of just the propagation, which is what nya_system_entity_update ends with. */
static void propagate(void) {
    nya_system_entity_transforms_update();
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

    // ── Parenting keeps the child exactly where it is.
    {
        NYA_EntityHandle tank   = nya_entity_spawn(.name = "tank", .position = { 100.0F, 0.0F, 0.0F }, .scale = { 1, 1, 1 });
        NYA_EntityHandle turret = nya_entity_spawn(.name = "turret", .position = { 100.0F, -20.0F, 0.0F }, .scale = { 1, 1, 1 });

        nya_check(nya_entity_parent_set(turret, tank), "parenting should succeed");

        NYA_Entity* child = nya_entity_get(turret);

        nya_check(near_enough(child->position.x, 100.0F) && near_enough(child->position.y, -20.0F),
                  "parenting must not move the child, got (%f, %f)", (f64)child->position.x, (f64)child->position.y);

        // The offset it captured is what it will be rebuilt from.
        nya_check(near_enough(child->local_position.y, -20.0F), "and it should have captured the offset, got %f",
                  (f64)child->local_position.y);

        nya_check(same_handle(nya_entity_parent(child), tank), "the parent should read back");
        nya_check(nya_entity_get(tank)->child_count == 1, "and be counted, got " FMTu32, nya_entity_get(tank)->child_count);

        nya_entity_clear();
    }

    // ── Moving a parent carries its children; moving a child does not move the parent.
    {
        NYA_EntityHandle tank   = nya_entity_spawn(.name = "tank", .position = { 100.0F, 0.0F, 0.0F }, .scale = { 1, 1, 1 });
        NYA_EntityHandle turret = nya_entity_spawn(.name = "turret", .position = { 100.0F, -20.0F, 0.0F }, .scale = { 1, 1, 1 });

        (void)nya_entity_parent_set(turret, tank);

        nya_entity_get(tank)->position.x += 10.0F;
        propagate();

        nya_check(near_enough(nya_entity_get(turret)->position.x, 110.0F), "the child should follow, got %f",
                  (f64)nya_entity_get(turret)->position.x);
        nya_check(near_enough(nya_entity_get(turret)->position.y, -20.0F), "keeping its offset, got %f",
                  (f64)nya_entity_get(turret)->position.y);

        // A child moved through its local offset stays put relative to the parent afterwards.
        nya_entity_get(turret)->local_position.y = -30.0F;
        propagate();

        nya_check(near_enough(nya_entity_get(turret)->position.y, -30.0F), "a local move should show, got %f",
                  (f64)nya_entity_get(turret)->position.y);
        nya_check(near_enough(nya_entity_get(tank)->position.x, 110.0F), "and must not disturb the parent");

        nya_entity_clear();
    }

    // ── Rotating a parent swings its children around it.
    {
        NYA_EntityHandle hub  = nya_entity_spawn(.name = "hub", .position = { 0, 0, 0 }, .scale = { 1, 1, 1 },
                                                .rotation = nya_quaternion_identity);
        NYA_EntityHandle arm  = nya_entity_spawn(.name = "arm", .position = { 10.0F, 0.0F, 0.0F }, .scale = { 1, 1, 1 },
                                                .rotation = nya_quaternion_identity);

        (void)nya_entity_parent_set(arm, hub);

        // A quarter turn about z takes +x to +y.
        nya_entity_get(hub)->rotation = nya_quaternion_from_axis_angle((f32x3){ 0, 0, 1 }, 1.5707963F);
        propagate();

        NYA_Entity* child = nya_entity_get(arm);

        nya_check(fabsf(child->position.x) < 0.01F, "the arm should have swung off the x axis, got %f", (f64)child->position.x);
        nya_check(near_enough(child->position.y, 10.0F) || fabsf(child->position.y - 10.0F) < 0.01F,
                  "and onto the y axis, got %f", (f64)child->position.y);

        nya_entity_clear();
    }

    // ── Scale composes, and a chain three deep resolves in one pass rather than one level per tick.
    {
        NYA_EntityHandle a = nya_entity_spawn(.name = "a", .position = { 0, 0, 0 }, .scale = { 1, 1, 1 }, .rotation = nya_quaternion_identity);
        NYA_EntityHandle b = nya_entity_spawn(.name = "b", .position = { 10.0F, 0, 0 }, .scale = { 1, 1, 1 }, .rotation = nya_quaternion_identity);
        NYA_EntityHandle c = nya_entity_spawn(.name = "c", .position = { 20.0F, 0, 0 }, .scale = { 1, 1, 1 }, .rotation = nya_quaternion_identity);

        (void)nya_entity_parent_set(b, a);
        (void)nya_entity_parent_set(c, b);

        // Doubling the root doubles both offsets, which is what makes the chain a single pass rather
        // than one level per tick: c is 10 from b, b is 10 from a, so c ends at 40.
        nya_entity_get(a)->scale = (f32x3){ 2.0F, 2.0F, 2.0F };
        propagate();

        nya_check(near_enough(nya_entity_get(b)->position.x, 20.0F), "b should be scaled to 20, got %f",
                  (f64)nya_entity_get(b)->position.x);
        nya_check(near_enough(nya_entity_get(c)->position.x, 40.0F), "and c to 40 in the same pass, got %f",
                  (f64)nya_entity_get(c)->position.x);
        nya_check(near_enough(nya_entity_get(c)->scale.x, 2.0F), "with the scale carried down, got %f",
                  (f64)nya_entity_get(c)->scale.x);

        nya_entity_clear();
    }

    // ── Unparenting leaves the child where it is.
    {
        NYA_EntityHandle parent = nya_entity_spawn(.name = "parent", .position = { 50.0F, 0, 0 }, .scale = { 1, 1, 1 });
        NYA_EntityHandle child  = nya_entity_spawn(.name = "child", .position = { 60.0F, 0, 0 }, .scale = { 1, 1, 1 });

        (void)nya_entity_parent_set(child, parent);
        nya_entity_parent_clear(child);

        nya_check(near_enough(nya_entity_get(child)->position.x, 60.0F), "unparenting must not move it, got %f",
                  (f64)nya_entity_get(child)->position.x);
        nya_check(!nya_entity_is_valid(nya_entity_parent(nya_entity_get(child))), "and it should be a root again");
        nya_check(nya_entity_get(parent)->child_count == 0, "with the parent's count back to zero");

        // Moving the old parent no longer does anything to it.
        nya_entity_get(parent)->position.x = 500.0F;
        propagate();
        nya_check(near_enough(nya_entity_get(child)->position.x, 60.0F), "and it must not follow any more");

        nya_entity_clear();
    }

    // ── Cycles are refused, which is what stops the propagation walk recursing forever.
    {
        NYA_EntityHandle a = nya_entity_spawn(.name = "a", .scale = { 1, 1, 1 });
        NYA_EntityHandle b = nya_entity_spawn(.name = "b", .scale = { 1, 1, 1 });
        NYA_EntityHandle c = nya_entity_spawn(.name = "c", .scale = { 1, 1, 1 });

        (void)nya_entity_parent_set(b, a);
        (void)nya_entity_parent_set(c, b);

        nya_check(!nya_entity_parent_set(a, a), "an entity cannot be its own parent");
        nya_check(!nya_entity_parent_set(a, b), "nor a child of its own child");
        nya_check(!nya_entity_parent_set(a, c), "nor of its grandchild");

        // The refusal must leave the tree alone rather than half applied.
        nya_check(!nya_entity_is_valid(nya_entity_parent(nya_entity_get(a))), "a should still be a root");
        nya_check(same_handle(nya_entity_parent(nya_entity_get(b)), a), "and b still under a");

        nya_check(nya_entity_is_ancestor(a, c), "a is c's ancestor");
        nya_check(!nya_entity_is_ancestor(c, a), "and not the other way round");

        nya_entity_clear();
    }

    // ── Several children, and moving between parents.
    {
        NYA_EntityHandle left  = nya_entity_spawn(.name = "left", .scale = { 1, 1, 1 });
        NYA_EntityHandle right = nya_entity_spawn(.name = "right", .scale = { 1, 1, 1 });

        NYA_EntityHandle kids[3];
        for (u32 i = 0; i < 3; i++) {
            kids[i] = nya_entity_spawn(.name = "kid", .scale = { 1, 1, 1 });
            (void)nya_entity_parent_set(kids[i], left);
        }

        NYA_EntityHandle listed[8];
        u32              count = nya_entity_children(nya_entity_get(left), listed, 8);

        nya_check(count == 3, "three children, got " FMTu32, count);

        // Counted even past the capacity, so a caller can size a buffer from the answer.
        nya_check(nya_entity_children(nya_entity_get(left), listed, 1) == 3, "the count is the real one, not the written one");

        // Reparenting one takes it out of the old list without disturbing the others.
        (void)nya_entity_parent_set(kids[1], right);

        nya_check(nya_entity_get(left)->child_count == 2, "the old parent should have two left, got " FMTu32,
                  nya_entity_get(left)->child_count);
        nya_check(nya_entity_get(right)->child_count == 1, "and the new one should have it");
        nya_check(nya_entity_children(nya_entity_get(left), listed, 8) == 2, "and the list should agree with the count");

        nya_entity_clear();
    }

    // ── Despawning a parent takes its subtree with it.
    {
        NYA_EntityHandle tank   = nya_entity_spawn(.name = "tank", .scale = { 1, 1, 1 });
        NYA_EntityHandle turret = nya_entity_spawn(.name = "turret", .scale = { 1, 1, 1 });
        NYA_EntityHandle barrel = nya_entity_spawn(.name = "barrel", .scale = { 1, 1, 1 });

        (void)nya_entity_parent_set(turret, tank);
        (void)nya_entity_parent_set(barrel, turret);

        nya_check(nya_entity_count() == 3, "three to begin with");

        nya_entity_despawn(tank);

        nya_check(!nya_entity_is_valid(turret), "the child goes with the parent");
        nya_check(!nya_entity_is_valid(barrel), "and so does the grandchild");
        nya_check(nya_entity_count() == 0, "leaving nothing, got " FMTu32, nya_entity_count());

        nya_entity_clear();
    }

    /*
     * ── Despawning a child leaves its parent's list intact.
     *
     * The failure this guards against is silent: a sibling list still naming a despawned slot walks
     * into whatever reuses it, and the propagation then writes a transform onto an unrelated entity.
     */
    {
        NYA_EntityHandle parent = nya_entity_spawn(.name = "parent", .position = { 5.0F, 0, 0 }, .scale = { 1, 1, 1 });

        NYA_EntityHandle first  = nya_entity_spawn(.name = "first", .scale = { 1, 1, 1 });
        NYA_EntityHandle middle = nya_entity_spawn(.name = "middle", .scale = { 1, 1, 1 });
        NYA_EntityHandle last   = nya_entity_spawn(.name = "last", .scale = { 1, 1, 1 });

        (void)nya_entity_parent_set(first, parent);
        (void)nya_entity_parent_set(middle, parent);
        (void)nya_entity_parent_set(last, parent);

        // The middle of the list, which is the case that needs the walk to the node before it.
        nya_entity_despawn(middle);

        nya_check(nya_entity_get(parent)->child_count == 2, "two children left, got " FMTu32, nya_entity_get(parent)->child_count);

        NYA_EntityHandle listed[8];
        nya_check(nya_entity_children(nya_entity_get(parent), listed, 8) == 2, "and the list should have two in it");

        // The slot is reused by something that is not a child, and the parent must not adopt it.
        NYA_EntityHandle stranger = nya_entity_spawn(.name = "stranger", .position = { 900.0F, 0, 0 }, .scale = { 1, 1, 1 });

        propagate();

        nya_check(nya_entity_get(parent)->child_count == 2, "a reused slot must not join the list");
        nya_check(near_enough(nya_entity_get(stranger)->position.x, 900.0F), "and must not be moved by it, got %f",
                  (f64)nya_entity_get(stranger)->position.x);

        nya_entity_clear();
    }

    // ── nya_entity_transform_sync answers within the same tick.
    {
        NYA_EntityHandle parent = nya_entity_spawn(.name = "parent", .position = { 0, 0, 0 }, .scale = { 1, 1, 1 });
        NYA_EntityHandle child  = nya_entity_spawn(.name = "child", .position = { 10.0F, 0, 0 }, .scale = { 1, 1, 1 });

        (void)nya_entity_parent_set(child, parent);

        nya_entity_get(parent)->position.x = 100.0F;

        // Without a sync the child is still where the last propagation left it — which is the
        // documented cost of propagating once, at the end of the tick.
        nya_check(near_enough(nya_entity_get(child)->position.x, 10.0F), "the child is stale until something propagates");

        nya_entity_transform_sync(parent);
        nya_check(near_enough(nya_entity_get(child)->position.x, 110.0F), "and sync brings it up to date, got %f",
                  (f64)nya_entity_get(child)->position.x);

        nya_entity_clear();
    }

    // ── The degenerate cases.
    {
        nya_check(!nya_entity_parent_set(NYA_ENTITY_HANDLE_NONE, NYA_ENTITY_HANDLE_NONE), "nothing cannot be parented");
        nya_check(!nya_entity_is_valid(nya_entity_parent(nullptr)), "nothing has no parent");
        nya_check(nya_entity_children(nullptr, nullptr, 0) == 0, "and no children");
        nya_check(!nya_entity_is_ancestor(NYA_ENTITY_HANDLE_NONE, NYA_ENTITY_HANDLE_NONE), "and is nobody's ancestor");

        // Harmless on things that are not there.
        nya_entity_parent_clear(NYA_ENTITY_HANDLE_NONE);
        nya_entity_transform_sync(NYA_ENTITY_HANDLE_NONE);
        propagate();
    }

    return nya_check_failures() == 0 ? 0 : 1;
}
