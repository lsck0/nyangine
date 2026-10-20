/**
 * Scopes: the stack they keep, self time under nesting, arena bytes charged per thread, tags and their sink, and the
 * table the frames close into. The crash report's scope block is tested in test_crash_report.c.
 **/

// a short window, so a handful of frames fills it and an average is the per frame figure.
#define NYA_SCOPE_HISTORY 4

#include "nyangine-core/nyangine.c"
#include "nyangine-core/nyangine.h"

#include "SDL3/SDL_init.h"
#include "SDL3/SDL_thread.h"

#if !NYA_SCOPE_MEASURED
#error "test_scope.c reads measured scopes; the suite's mode should have switched them on"
#endif

/** What an allocation may grow by past its request: alignment plus the sanitizer's red zone. */
#define PADDING_SLACK_BYTES 128

#define WORKER_COUNT       4
#define WORKER_ALLOCATIONS 8
#define WORKER_BLOCK_BYTES 4096

/** Burns roughly `ns` of CPU, so a scope has something to measure without sleeping off the thread. */
static void spin_ns(u64 ns) {
    u64 until = nya_clock_get_monotonic_ns() + ns;
    while (nya_clock_get_monotonic_ns() < until) {}
}

static NYA_ScopeStats rows[NYA_SCOPE_SITE_MAX];

/** The row named `name` in the table as it is now, or null. */
static const NYA_ScopeStats* row_find(NYA_ConstCString name) {
    u32 count = nya_scope_stats(rows, NYA_SCOPE_SITE_MAX, NYA_SCOPE_SORT_NAME);

    for (u32 i = 0; i < count; i++) {
        if (strcmp(rows[i].name, name) == 0) return &rows[i];
    }

    return nullptr;
}

static void window_close(void) {
    for (u32 i = 0; i < NYA_SCOPE_HISTORY; i++) nya_scope_frame_end();
}

/** What the sink heard, summed. */
static u32 sink_calls_tagged   = 0;
static u32 sink_calls_inherited = 0;
static u64 sink_self_ns        = 0;
static u32 sink_last_tag       = 0;

static void sink_record(NYA_ScopeTag tag, u64 started_ns, u64 ended_ns, u64 self_ns, b8 tagged) {
    nya_check(ended_ns >= started_ns && self_ns <= ended_ns - started_ns, "a scope's own time fits inside its span");

    if (tagged) sink_calls_tagged++;
    else sink_calls_inherited++;

    sink_self_ns  += self_ns;
    sink_last_tag  = tag;
}

static s32 SDLCALL worker(void* data) {
    nya_unused(data);

    nya_scope("scope_test_worker");

    NYA_Arena* arena = nya_arena_create(.name = "scope_test_worker", .region_size = nya_kibyte_to_byte(64));
    for (u32 i = 0; i < WORKER_ALLOCATIONS; i++) nya_check(nya_arena_alloc(arena, WORKER_BLOCK_BYTES) != nullptr, "the worker's block");
    nya_arena_destroy(arena);

    return 0;
}

s32 main(void) {
    nya_check(SDL_Init(0), "SDL_Init failed: %s", SDL_GetError());

    // TEST: scopes nest, and the stack reads outermost first with the site's own strings
    {
        nya_check(nya_scope_depth() == 0, "nothing open at the start");

        {
            nya_scope("scope_test_outer");
            nya_check(nya_scope_depth() == 1, "one open");

            {
                nya_scope_function();
                nya_check(nya_scope_depth() == 2, "two open");

                NYA_ScopeView outer = nya_scope_at(0);
                NYA_ScopeView inner = nya_scope_at(1);
                nya_check(strcmp(outer.name, "scope_test_outer") == 0, "the outermost is index zero, got '%s'", outer.name);
                nya_check(strcmp(inner.name, "main") == 0 && strcmp(inner.function, "main") == 0, "nya_scope_function names the function");
                nya_check(strstr(inner.file, "test_scope.c") != nullptr && inner.line > outer.line, "and carries where it is");
            }

            nya_check(nya_scope_depth() == 1, "the inner one closed at its block's end");
        }

        nya_check(nya_scope_depth() == 0, "and the outer one at its own");

        NYA_ScopeMark mark = nya_scope_begin("scope_test_bracket", NYA_SCOPE_TAG_NONE);
        nya_check(nya_scope_depth() == 1 && strcmp(nya_scope_at(0).name, "scope_test_bracket") == 0, "a bracket opens like a block");
        nya_scope_end(&mark);
        nya_check(nya_scope_depth() == 0, "and closes on its end");
    }

    // TEST: a scope's self time is its own, with its children's taken off, and the two add up to its total
    {
        window_close();

        for (u32 frame = 0; frame < NYA_SCOPE_HISTORY; frame++) {
            {
                // the child spins five times the parent, so the two are apart by its whole spin whatever the machine does.
                nya_scope("scope_test_parent");
                spin_ns(1'000'000);

                {
                    nya_scope("scope_test_child");
                    spin_ns(5'000'000);
                }
            }

            nya_scope_frame_end();
        }

        const NYA_ScopeStats parent = *row_find("scope_test_parent");
        const NYA_ScopeStats* child = row_find("scope_test_child");

        nya_check(child != nullptr, "both sites have a row");
        nya_check(parent.calls == 1.0F && child->calls == 1.0F, "one call a frame each");
        // lower bounds only: a spin can overrun on a loaded machine, it cannot finish early.
        nya_check(child->self_ms >= 4.5, "the child's own five milliseconds, got %.2f", child->self_ms);
        nya_check(parent.self_ms >= 0.9, "and the parent's own one, got %.2f", parent.self_ms);
        nya_check(parent.self_ms < child->self_ms, "the parent's self time has the child's taken off (%.3f against %.3f)", parent.self_ms, child->self_ms);
        nya_check(fabs(parent.total_ms - (parent.self_ms + child->total_ms)) < 1e-6, "a parent's total is its self time plus its children's, exactly");
        nya_check(child->self_max_ms >= child->self_ms, "the window's maximum bounds its average");
    }

    // TEST: arena bytes go to the innermost open scope, and the peak is what was live, not what was allocated
    {
        window_close();

        NYA_Arena* arena = nya_arena_create(.name = "scope_test", .region_size = nya_kibyte_to_byte(64));

        for (u32 frame = 0; frame < NYA_SCOPE_HISTORY; frame++) {
            {
                nya_scope("scope_test_alloc_outer");
                nya_check(nya_arena_alloc(arena, 1000) != nullptr, "the outer block");

                {
                    nya_scope("scope_test_alloc_inner");
                    void* scratch = nya_arena_alloc(arena, 4000);
                    nya_arena_free(arena, scratch, 4000);
                }

                // after the inner scope gave its block back, so the outer scope's peak is the inner one's, not this.
                nya_check(nya_arena_alloc(arena, 500) != nullptr, "the outer block after");

                NYA_ScopeView open = nya_scope_at(0);
                nya_check(open.allocated_bytes >= 5500 && open.peak_bytes >= 5000, "an open scope reads its bytes so far");
            }

            nya_arena_free_all(arena);
            nya_scope_frame_end();
        }

        nya_arena_destroy(arena);

        const NYA_ScopeStats outer = *row_find("scope_test_alloc_outer");
        const NYA_ScopeStats inner = *row_find("scope_test_alloc_inner");

        nya_check(inner.allocated_bytes >= 4000 && inner.allocated_bytes < 4000 + PADDING_SLACK_BYTES, "the inner scope's one block, got " FMTu64,
                  inner.allocated_bytes);
        nya_check(inner.allocated_self_bytes == inner.allocated_bytes, "with nothing under it, all of it its own");
        nya_check(outer.allocated_self_bytes >= 1500 && outer.allocated_self_bytes < 1500 + (2 * PADDING_SLACK_BYTES), "the outer scope's own two, got " FMTu64,
                  outer.allocated_self_bytes);
        nya_check(outer.allocated_bytes == outer.allocated_self_bytes + inner.allocated_bytes, "its total is its own plus its child's, exactly");

        nya_check(inner.peak_bytes == inner.allocated_bytes, "the inner peak is its block, freed before it closed");
        nya_check(outer.peak_bytes >= 5000 && outer.peak_bytes < 5000 + (2 * PADDING_SLACK_BYTES), "the outer peak is its first block and the child's, got " FMTu64,
                  outer.peak_bytes);
        nya_check(outer.peak_bytes < outer.allocated_bytes, "less than it allocated, since the child gave its block back");
        nya_check(outer.high_water_bytes >= outer.peak_bytes, "and the high water mark holds it");
    }

    // TEST: attribution is per thread, so a scope waiting on workers is charged none of their bytes
    {
        window_close();

        for (u32 frame = 0; frame < NYA_SCOPE_HISTORY; frame++) {
            {
                nya_scope("scope_test_waiting");

                SDL_Thread* threads[WORKER_COUNT];
                for (u32 i = 0; i < WORKER_COUNT; i++) {
                    threads[i] = SDL_CreateThread(worker, "scope_worker", nullptr);
                    nya_check(threads[i] != nullptr, "SDL_CreateThread failed: %s", SDL_GetError());
                }
                for (u32 i = 0; i < WORKER_COUNT; i++) SDL_WaitThread(threads[i], nullptr);
            }

            nya_scope_frame_end();
        }

        const NYA_ScopeStats waiting = *row_find("scope_test_waiting");
        const NYA_ScopeStats workers = *row_find("scope_test_worker");

        const u64 one_worker_bytes = (u64)WORKER_ALLOCATIONS * WORKER_BLOCK_BYTES;

        nya_check(waiting.allocated_bytes == 0, "the waiting thread allocated nothing itself, got " FMTu64, waiting.allocated_bytes);
        nya_check(workers.calls == (f32)WORKER_COUNT, "every worker's scope landed in the frame, got %.1f", (f64)workers.calls);
        nya_check(workers.allocated_bytes >= WORKER_COUNT * one_worker_bytes &&
                      workers.allocated_bytes < WORKER_COUNT * (one_worker_bytes + (WORKER_ALLOCATIONS * PADDING_SLACK_BYTES)),
                  "every worker's blocks, summed over the threads, got " FMTu64, workers.allocated_bytes);
        nya_check(workers.peak_bytes < 2 * one_worker_bytes, "a peak is one thread's live bytes, not the sum of all four, got " FMTu64, workers.peak_bytes);
    }

    // TEST: a tag reaches every untagged scope under it, and the sink hears both kinds
    {
        nya_scope_tag_sink_set(sink_record);

        {
            nya_scope_tagged("scope_test_tagged", 5);
            nya_check(nya_scope_tag_current() == 5, "the tagged scope is current");

            {
                nya_scope("scope_test_tag_child");
                nya_check(nya_scope_tag_current() == 5, "an untagged scope inherits it");
                spin_ns(100'000);
            }
        }

        {
            nya_scope("scope_test_untagged");
            nya_check(nya_scope_tag_current() == NYA_SCOPE_TAG_NONE, "outside it, no tag");
        }

        nya_scope_tag_sink_set(nullptr);

        nya_check(sink_calls_tagged == 1 && sink_calls_inherited == 1, "one tagged and one inherited close, got %u and %u", sink_calls_tagged,
                  sink_calls_inherited);
        nya_check(sink_last_tag == 5 && sink_self_ns >= 100'000, "under the tag, with the child's time");
    }

    // TEST: a run time name finds its row on every entry
    {
        window_close();

        char name[32];
        (void)snprintf(name, sizeof(name), "scope_test_layer_%d", 7);

        u32 sites_before = nya_scope_site_count();

        for (u32 i = 0; i < 3; i++) {
            nya_scope_named(name);
        }

        nya_check(nya_scope_site_count() == sites_before + 1, "three entries, one row");

        nya_scope_frame_end();

        const NYA_ScopeStats* layer = row_find("scope_test_layer_7");
        nya_check(layer != nullptr && layer->calls * NYA_SCOPE_HISTORY == 3.0F, "with all three calls in it");
    }

    // TEST: the table sorts, and the report runs on real rows
    {
        NYA_ScopeStats sorted[] = {
            { .name = "b", .self_ms = 1.0, .allocated_self_bytes = 10, .high_water_bytes = 5 },
            { .name = "a", .self_ms = 3.0, .allocated_self_bytes = 10, .high_water_bytes = 50 },
            { .name = "c", .self_ms = 2.0, .allocated_self_bytes = 90, .high_water_bytes = 1 },
        };

        nya_scope_sort(sorted, 3, NYA_SCOPE_SORT_SELF);
        nya_check(strcmp(sorted[0].name, "a") == 0 && strcmp(sorted[2].name, "b") == 0, "most self time first");

        nya_scope_sort(sorted, 3, NYA_SCOPE_SORT_ALLOCATED);
        nya_check(strcmp(sorted[0].name, "c") == 0 && strcmp(sorted[1].name, "a") == 0, "most allocated first, ties keep their order");

        nya_scope_sort(sorted, 3, NYA_SCOPE_SORT_PEAK);
        nya_check(strcmp(sorted[0].name, "a") == 0, "highest water first");

        nya_scope_sort(sorted, 3, NYA_SCOPE_SORT_NAME);
        nya_check(strcmp(sorted[0].name, "a") == 0 && strcmp(sorted[2].name, "c") == 0, "names alphabetically");

        nya_scope_report();
    }

    SDL_Quit();

    return nya_check_failures() == 0 ? 0 : 1;
}
