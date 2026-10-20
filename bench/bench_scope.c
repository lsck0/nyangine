/**
 * What a scope costs in the build this is compiled as. Benchmarks build as release, so this is the name only stack a
 * shipping game keeps for its crash reports; bench_scope_measured.c includes this file with the measuring switched
 * on. The loop with no scope is the floor every other case is read against.
 **/

#include "nyangine-core/nyangine.c"
#include "nyangine-core/nyangine.h"

/** Bytes an allocation case asks for: a small record, the size a scope is most often charged. */
#define BENCH_SCOPE_ALLOCATION_BYTES 64

/** Allocations between resets of the arena, so it never grows past one region. */
#define BENCH_SCOPE_RESET_EVERY 65536

s32 main(void) {
    nya_assert(NYA_SCOPE_ENABLED);

    NYA_Arena* arena = nya_arena_create(.name = "bench_scope", .region_size = nya_mebyte_to_byte(64));
    defer      nya_arena_destroy(arena);

    u64 counter = 0;

    nya_bench_begin(NYA_SCOPE_MEASURED ? "scope, measured" : "scope, release: names only");

    nya_bench("no scope (floor)", 0, {
        counter++;
        nya_bench_keep(counter);
    });

    nya_bench("one scope", 0, {
        nya_scope("bench_one");
        counter++;
        nya_bench_keep(counter);
    });

    nya_bench("two nested scopes", 0, {
        nya_scope("bench_outer");
        {
            nya_scope("bench_inner");
            counter++;
            nya_bench_keep(counter);
        }
    });

    nya_bench("one tagged scope, no sink", 0, {
        nya_scope_tagged("bench_tagged", 3);
        counter++;
        nya_bench_keep(counter);
    });

    nya_bench("run time named scope", 0, {
        nya_scope_named("bench_named");
        counter++;
        nya_bench_keep(counter);
    });

    nya_bench("arena alloc, no scope", 0, {
        nya_bench_keep(nya_arena_alloc(arena, BENCH_SCOPE_ALLOCATION_BYTES));
        if (++counter % BENCH_SCOPE_RESET_EVERY == 0) nya_arena_free_all(arena);
    });

    nya_bench("arena alloc inside a scope", 0, {
        nya_scope("bench_alloc");
        nya_bench_keep(nya_arena_alloc(arena, BENCH_SCOPE_ALLOCATION_BYTES));
        if (++counter % BENCH_SCOPE_RESET_EVERY == 0) nya_arena_free_all(arena);
    });

    nya_bench("frame end, every site", 0, { nya_scope_frame_end(); });

    return nya_bench_end();
}
