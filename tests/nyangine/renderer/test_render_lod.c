/**
 * Level of detail: chain registration, selection, and the inertness that makes it safe to route every
 * draw through.
 *
 * The property that matters most is the last one — a mesh with no chain must come back *unchanged*, so
 * `nya_render3d_mesh` can resolve unconditionally without every call site knowing whether LOD is in use.
 * The second is that a badly ordered chain is refused rather than sorted, because sorting would hide the
 * mistake instead of fixing it.
 **/

#include "nyangine/nyangine.c"
#include "nyangine/nyangine.h"

#define TREE     "mesh_tree"
#define TREE_MID "mesh_tree_mid"
#define TREE_FAR "mesh_tree_far"
#define ROCK     "mesh_rock"

s32 main(void) {
    nya_render3d_lod_clear();

    // ── An unregistered handle passes straight through. This is what makes resolution unconditional.
    {
        nya_check(nya_render3d_lod_count() == 0, "nothing registered yet");
        nya_check(!nya_render3d_lod_registered(TREE), "and the tree is not");
        nya_check(nya_render3d_lod_select(TREE, 0.0F) == TREE, "an unregistered handle comes back as itself");
        nya_check(nya_render3d_lod_select(TREE, 1e9F) == TREE, "at any distance");
        nya_check(nya_render3d_lod_select(nullptr, 5.0F) == nullptr, "and null stays null");
    }

    // ── A well formed chain selects by distance, and the last level's range is the draw distance.
    {
        b8 ok = nya_render3d_lod_register(TREE,
                                          (NYA_Render3DLodLevel[]){
                                              { .handle = TREE, .max_distance = 30.0F },
                                              { .handle = TREE_MID, .max_distance = 90.0F },
                                              { .handle = TREE_FAR, .max_distance = 250.0F },
                                          },
                                          3);
        nya_check(ok, "a well formed chain should register");
        nya_check(nya_render3d_lod_count() == 1, "and be counted");
        nya_check(nya_render3d_lod_registered(TREE), "and be findable");

        nya_check(nya_render3d_lod_select(TREE, 0.0F) == TREE, "at the eye it is full detail");
        nya_check(nya_render3d_lod_select(TREE, 29.9F) == TREE, "just inside the first range too");
        nya_check(nya_render3d_lod_select(TREE, 30.0F) == TREE, "the boundary belongs to the nearer level");
        nya_check(nya_render3d_lod_select(TREE, 30.1F) == TREE_MID, "just past it drops a level");
        nya_check(nya_render3d_lod_select(TREE, 89.0F) == TREE_MID, "and stays there");
        nya_check(nya_render3d_lod_select(TREE, 91.0F) == TREE_FAR, "then drops again");
        nya_check(nya_render3d_lod_select(TREE, 249.0F) == TREE_FAR, "up to the last range");

        // Past the final level is not a handle at all — that is the draw distance.
        nya_check(nya_render3d_lod_select(TREE, 251.0F) == nullptr, "past the last level it is not drawn");
        nya_check(nya_render3d_lod_select(TREE, 1e6F) == nullptr, "however far past");

        nya_check(nya_render3d_lod_level_at(TREE, 10.0F) == 0, "level 0 near");
        nya_check(nya_render3d_lod_level_at(TREE, 50.0F) == 1, "level 1 mid");
        nya_check(nya_render3d_lod_level_at(TREE, 200.0F) == 2, "level 2 far");
        nya_check(nya_render3d_lod_level_at(TREE, 400.0F) == NYA_RENDER3D_LOD_LEVELS, "and past the end");
    }

    // ── The squared entry point agrees with the plain one, since the draw path uses it.
    {
        nya_check(nya_render3d_lod_select_squared(TREE, 10.0F * 10.0F) == nya_render3d_lod_select(TREE, 10.0F),
                  "squared and plain should agree near");
        nya_check(nya_render3d_lod_select_squared(TREE, 200.0F * 200.0F) == nya_render3d_lod_select(TREE, 200.0F),
                  "and far");
        nya_check(nya_render3d_lod_select_squared(TREE, 400.0F * 400.0F) == nullptr, "and past the end");
    }

    // ── A badly formed chain is refused rather than quietly fixed.
    {
        nya_check(!nya_render3d_lod_register(ROCK,
                                             (NYA_Render3DLodLevel[]){
                                                 { .handle = TREE_MID, .max_distance = 90.0F },
                                                 { .handle = TREE, .max_distance = 30.0F },
                                             },
                                             2),
                  "levels out of order must be refused, not sorted");

        nya_check(!nya_render3d_lod_register(ROCK,
                                             (NYA_Render3DLodLevel[]){
                                                 { .handle = TREE, .max_distance = 30.0F },
                                                 { .handle = TREE_MID, .max_distance = 30.0F },
                                             },
                                             2),
                  "two levels at the same distance are ambiguous and must be refused");

        nya_check(!nya_render3d_lod_register(ROCK, (NYA_Render3DLodLevel[]){ { .handle = nullptr, .max_distance = 10.0F } }, 1),
                  "a null handle must be refused");

        nya_check(!nya_render3d_lod_register(ROCK, (NYA_Render3DLodLevel[]){ { .handle = TREE, .max_distance = 0.0F } }, 1),
                  "a zero range must be refused");

        nya_check(!nya_render3d_lod_register(nullptr, (NYA_Render3DLodLevel[]){ { .handle = TREE, .max_distance = 10.0F } }, 1),
                  "a null base must be refused");

        nya_check(!nya_render3d_lod_register(ROCK, nullptr, 1), "null levels must be refused");
        nya_check(!nya_render3d_lod_register(ROCK, (NYA_Render3DLodLevel[]){ { .handle = TREE, .max_distance = 1.0F } }, 0),
                  "a zero count must be refused");

        nya_check(!nya_render3d_lod_registered(ROCK), "and none of that should have registered anything");
        nya_check(nya_render3d_lod_count() == 1, "the count should be untouched, got %u", nya_render3d_lod_count());
    }

    // ── A single-level chain is a pure draw distance, which is a use in its own right.
    {
        nya_check(nya_render3d_lod_register(ROCK, (NYA_Render3DLodLevel[]){ { .handle = ROCK, .max_distance = 50.0F } }, 1),
                  "a one-level chain should register");

        nya_check(nya_render3d_lod_select(ROCK, 49.0F) == ROCK, "inside the range it draws");
        nya_check(nya_render3d_lod_select(ROCK, 51.0F) == nullptr, "outside it does not");
    }

    // ── Re-registering replaces rather than accumulating.
    {
        u32 before = nya_render3d_lod_count();

        nya_check(nya_render3d_lod_register(TREE, (NYA_Render3DLodLevel[]){ { .handle = TREE_FAR, .max_distance = 5.0F } }, 1),
                  "re-registering should succeed");
        nya_check(nya_render3d_lod_count() == before, "and not add a second chain");
        nya_check(nya_render3d_lod_select(TREE, 1.0F) == TREE_FAR, "the new chain should be in force");
        nya_check(nya_render3d_lod_select(TREE, 10.0F) == nullptr, "including its shorter range");
    }

    // ── Unregistering restores pass-through.
    {
        nya_render3d_lod_unregister(TREE);
        nya_check(!nya_render3d_lod_registered(TREE), "it should be gone");
        nya_check(nya_render3d_lod_select(TREE, 1e6F) == TREE, "and select as itself again at any distance");

        nya_render3d_lod_unregister("never registered");

        nya_render3d_lod_clear();
        nya_check(nya_render3d_lod_count() == 0, "clear should empty the table");
        nya_check(nya_render3d_lod_select(ROCK, 1.0F) == ROCK, "and everything passes through again");
    }

    // ── The table refuses more chains than it holds, rather than overwriting one.
    {
        nya_render3d_lod_clear();

        static char names[NYA_RENDER3D_LOD_CHAINS + 4][16];
        u32         registered = 0;

        for (u32 i = 0; i < NYA_RENDER3D_LOD_CHAINS + 4; i++) {
            (void)snprintf(names[i], sizeof(names[i]), "mesh_%u", i);
            if (nya_render3d_lod_register(names[i], (NYA_Render3DLodLevel[]){ { .handle = names[i], .max_distance = 10.0F } }, 1)) {
                registered++;
            }
        }

        nya_check(registered == NYA_RENDER3D_LOD_CHAINS, "exactly the table's worth should register, got %u", registered);
        nya_check(nya_render3d_lod_count() == NYA_RENDER3D_LOD_CHAINS, "and the count should agree");

        nya_render3d_lod_clear();
    }

    return nya_check_failures() == 0 ? 0 : 1;
}
