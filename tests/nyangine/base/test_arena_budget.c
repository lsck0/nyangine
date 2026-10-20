/**
 * An arena's byte budget and the fallible allocation path: refused past the budget, refused under a
 * starved fault plan, never refused on the infallible path.
 **/

#include "nyangine-core/nyangine.c"
#include "nyangine-core/nyangine.h"

/** Region and budget for the arena under test: room for exactly two regions. */
#define REGION_BYTES (64ULL * 1024ULL)
#define BUDGET_BYTES (2ULL * REGION_BYTES)

/** Past what half a region has left once half of it is taken, so each one needs a region of its own. */
#define HALF_PLUS_BYTES (REGION_BYTES / 2 + 1024ULL)

s32 main(void) {
    // TEST: try_alloc fills the budget and then refuses, leaving the arena usable.
    {
        NYA_Arena* arena = nya_arena_create(.name = "test_arena_budget", .region_size = REGION_BYTES, .budget_bytes = BUDGET_BYTES);
        defer      nya_arena_destroy(arena);

        u8* first  = nya_arena_try_alloc(arena, REGION_BYTES / 2);
        u8* second = nya_arena_try_alloc(arena, HALF_PLUS_BYTES);
        nya_check(first != nullptr && second != nullptr, "two allocations that need a region each fit inside a two region budget");

        u8* third = nya_arena_try_alloc(arena, HALF_PLUS_BYTES);
        nya_check(third == nullptr, "a third region is refused");

        NYA_Error reserved = nya_arena_try_reserve(arena, REGION_BYTES);
        nya_check(!reserved.ok && reserved.kind == NYA_ERROR_OUT_OF_MEMORY, "and a reserve says so as out of memory, got %s", NYA_ERRORKIND_NAME_MAP[reserved.kind]);

        // what is left in the first region is still served, since it needs no new region.
        nya_check(nya_arena_try_alloc(arena, 1024) != nullptr, "room inside a region is not refused");

        // the budget binds the fallible path only.
        nya_check(nya_arena_alloc(arena, REGION_BYTES) != nullptr, "the infallible path is never refused");

        nya_memset(first, 0xAB, REGION_BYTES / 2);
        nya_memset(second, 0xCD, HALF_PLUS_BYTES);
        nya_check(first[0] == 0xAB && second[HALF_PLUS_BYTES - 1] == 0xCD, "what was handed out is the caller's");
    }

    // TEST: a reserve that succeeds leaves the bytes in place for the next allocation and moves nothing.
    {
        NYA_Arena* arena = nya_arena_create(.name = "test_arena_reserve", .region_size = REGION_BYTES, .budget_bytes = BUDGET_BYTES);
        defer      nya_arena_destroy(arena);

        nya_check(nya_arena_try_reserve(arena, REGION_BYTES / 4).ok, "a quarter region reserves");

        u64 used = nya_arena_memory_usage_bytes(arena);
        nya_check(used == 0, "a reserve hands nothing out, got " FMTu64 " bytes in use", used);
        nya_check(nya_arena_stats(arena).region_count == 1, "but the region it needed is there");
    }

    // TEST: no budget is no limit, and a zero budget is the same.
    {
        NYA_Arena* arena = nya_arena_create(.name = "test_arena_unbounded", .region_size = REGION_BYTES);
        defer      nya_arena_destroy(arena);

        for (u32 i = 0; i < 8; i++) nya_check(nya_arena_try_alloc(arena, REGION_BYTES) != nullptr, "allocation " FMTu32 " goes through without a budget", i);
    }

    // TEST: the fault plan starves exactly the scripted fallible allocation, and the infallible path never asks it.
    {
        NYA_Arena* arena = nya_arena_create(.name = "test_arena_starved");
        defer      nya_arena_destroy(arena);

        NYA_OsFaultPlan plan = { .seed = 1 };
        plan.rules[NYA_OS_FAULT_ALLOCATION] = (NYA_OsFaultRule){ .fail_call = 2, .fail_kind = NYA_OS_FAULT_FAIL };

        nya_check(nya_os_fault_plan_attach(&plan), "the plan attaches");

        void* first  = nya_arena_try_alloc(arena, 64);
        void* plain  = nya_arena_alloc(arena, 64);
        void* second = nya_arena_try_alloc(arena, 64);
        void* third  = nya_arena_try_alloc(arena, 64);

        nya_os_fault_plan_detach();

        nya_check(first != nullptr && plain != nullptr && third != nullptr, "every call but the scripted one is served");
        nya_check(second == nullptr, "the second fallible call is the starved one");
        nya_check(plan.calls[NYA_OS_FAULT_ALLOCATION] == 3, "only fallible calls ask the plan, got " FMTu64, plan.calls[NYA_OS_FAULT_ALLOCATION]);
    }

    return nya_check_failures() == 0 ? 0 : 1;
}
