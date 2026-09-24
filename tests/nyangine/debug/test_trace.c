/**
 * Tracing: the window and GPU group arithmetic, the table's order, self time under nesting, per feature GPU memory,
 * and the Chrome trace format.
 **/

#include "nyangine/nyangine.c"
#include "nyangine/nyangine.h"

/** Burns roughly `ns` of CPU, so a scope has something to measure without sleeping off the thread. */
static void spin_ns(u64 ns) {
    u64 until = nya_clock_get_monotonic_ns() + ns;
    while (nya_clock_get_monotonic_ns() < until) {}
}

s32 main(void) {
    // ── A window's average and maximum.
    {
        u32 samples[] = { 4, 8, 0, 12 };
        f64 average   = -1.0;
        u32 maximum   = 0;

        nya_trace_window(samples, 4, &average, &maximum);
        nya_check(average == 6.0 && maximum == 12, "the mean and the largest of four");

        nya_trace_window(samples, 0, &average, &maximum);
        nya_check(average == 0.0 && maximum == 0, "an empty window is zero, not a division by zero");
    }

    // ── A GPU group starts when the queue was free.
    {
        nya_check(nya_trace_gpu_elapsed(100, 50, 180) == 80, "submitted to an idle queue: from submission");
        nya_check(nya_trace_gpu_elapsed(100, 150, 180) == 30, "submitted behind a running group: from its completion");
        nya_check(nya_trace_gpu_elapsed(100, 200, 180) == 0, "stamped out of order: nothing rather than a wrap");
    }

    // ── The table's order.
    {
        NYA_TraceStats rows[] = {
            { .name = "bloom", .cpu_ms = 0.5, .gpu_ms = 0.2, .has_gpu = true, .vram_bytes = 100 },
            { .name = "shadows", .cpu_ms = 1.5, .gpu_ms = 0.0, .has_gpu = false, .vram_bytes = 900, .ram_bytes = 5 },
            { .name = "antialiasing", .cpu_ms = 0.5, .gpu_ms = 0.9, .has_gpu = true, .ram_bytes = 50 },
        };

        nya_trace_sort(rows, 3, NYA_TRACE_SORT_CPU);
        nya_check(strcmp(rows[0].name, "shadows") == 0 && strcmp(rows[1].name, "bloom") == 0, "most CPU first, ties keep their order");

        nya_trace_sort(rows, 3, NYA_TRACE_SORT_GPU);
        nya_check(strcmp(rows[0].name, "antialiasing") == 0 && strcmp(rows[2].name, "shadows") == 0, "no GPU time sorts below a measured zero");

        nya_trace_sort(rows, 3, NYA_TRACE_SORT_VRAM);
        nya_check(strcmp(rows[0].name, "shadows") == 0, "most VRAM first");

        nya_trace_sort(rows, 3, NYA_TRACE_SORT_RAM);
        nya_check(strcmp(rows[0].name, "antialiasing") == 0, "most RAM first");

        nya_trace_sort(rows, 3, NYA_TRACE_SORT_NAME);
        nya_check(strcmp(rows[0].name, "antialiasing") == 0 && strcmp(rows[2].name, "shadows") == 0, "names alphabetically");
    }

    // ── A scope charges its own time, not its children's, and only while tracing is on.
    {
        nya_check(!nya_trace_active(), "off until something asks");

        {
            nya_trace_scope(NYA_TRACE_SHADOWS);
            nya_check(nya_trace_feature_current() == NYA_TRACE_SHADOWS, "the scope is current inside it");
        }
        nya_check(nya_trace_feature_current() == NYA_TRACE_OTHER, "and the outer feature is back after it");

        nya_trace_request();
        nya_trace_frame_end();
        nya_check(nya_trace_active(), "a request turns tracing on for the next frame");

        for (u32 frame = 0; frame < 4; frame++) {
            nya_trace_request();

            {
                /* The child spins five times the parent, not half of it. What is under test is that a nested scope's time lands on the child and is taken off the parent. Asserting that with a parent that spins longer needs a tight upper bound on a wall clock measurement — 2 ms of spin had to read under 2.9 — and a sanitized build on a loaded machine oversleeps straight through it. This way the two are separated by the whole of the child's spin whatever the machine does: attributed, the parent reads about 1 ms against the child's 5; folded in, it would read about 6. */
                nya_trace_scope(NYA_TRACE_SCENE);
                spin_ns(1'000'000);

                {
                    nya_trace_scope(NYA_TRACE_PARTICLES);
                    spin_ns(5'000'000);
                    nya_trace_draws(3);
                }
            }

            nya_trace_gpu(NYA_TRACE_BLOOM, 1000, 1500);
            nya_trace_frame_end();
        }

        NYA_TraceStats rows[NYA_TRACE_FEATURE_MAX];
        u32            count = nya_trace_stats(rows, NYA_TRACE_FEATURE_MAX, NYA_TRACE_SORT_NAME);

        const NYA_TraceStats* scene = nullptr, *particles = nullptr, *bloom = nullptr;
        for (u32 i = 0; i < count; i++) {
            if (rows[i].feature == NYA_TRACE_SCENE) scene = &rows[i];
            if (rows[i].feature == NYA_TRACE_PARTICLES) particles = &rows[i];
            if (rows[i].feature == NYA_TRACE_BLOOM) bloom = &rows[i];
        }

        nya_check(scene != nullptr && particles != nullptr && bloom != nullptr, "every feature with time has a row");
        // Lower bounds only: a spin can overrun on a loaded machine, it cannot finish early.
        nya_check(particles->cpu_ms >= 4.5, "the child's own five milliseconds, got %.2f", particles->cpu_ms);
        nya_check(scene->cpu_ms >= 0.9, "and the parent's own one, got %.2f", scene->cpu_ms);
        nya_check(scene->cpu_ms < particles->cpu_ms, "the parent's time is its own, with the child's taken off it (%.2f against %.2f)",
                  scene->cpu_ms, particles->cpu_ms);
        nya_check(particles->calls == 1.0F && particles->draws == 3.0F, "one scope and three draws a frame");
        nya_check(bloom->has_gpu && fabs(bloom->gpu_ms - 0.0005) < 1e-9 && !scene->has_gpu, "GPU time only where a group was charged");

        // two frames without a request and it is off again.
        nya_trace_frame_end();
        nya_trace_frame_end();
        nya_check(!nya_trace_active(), "tracing stops once nothing asks");
    }

    // ── GPU memory is counted against the scope that created it.
    {
        const void* handle = (const void*)(uintptr_t)0x20000ULL;

        {
            nya_trace_scope(NYA_TRACE_SHADOWS);
            _nya_gpu_memory_track(NYA_GPU_MEMORY_TEXTURE, handle, 4096);
        }

        nya_check(nya_gpu_memory_feature_bytes(NYA_TRACE_SHADOWS) == 4096, "created inside the shadows scope");

        _nya_gpu_memory_untrack(handle);
        nya_check(nya_gpu_memory_feature_bytes(NYA_TRACE_SHADOWS) == 0, "released outside it, still uncounted from it");
    }

    // ── A registered feature, and the Chrome trace it writes as.
    {
        NYA_TraceFeature crowds = nya_trace_feature_register("crowds", "crowd");
        nya_check(crowds == NYA_TRACE_ENGINE_FEATURES, "the first game feature follows the engine's");
        nya_check(nya_trace_feature_register("crowds", nullptr) == crowds, "registering a name twice returns its id");

        NYA_TraceEvent events[] = {
            { .started_ns = 2'000, .ended_ns = 5'500, .thread = 7, .feature = NYA_TRACE_SHADOWS },
            { .started_ns = 6'000, .ended_ns = 7'000, .thread = NYA_TRACE_GPU_THREAD, .feature = crowds },
        };
        NYA_ConstCString names[NYA_TRACE_FEATURE_MAX];
        for (u32 f = 0; f < nya_trace_feature_count(); f++) names[f] = nya_trace_feature_name((NYA_TraceFeature)f);

        u64 needed = nya_trace_capture_format(events, 2, names, nya_trace_feature_count(), 1'000, nullptr, 0);

        char text[512];
        nya_check(needed < sizeof(text), "the size is counted without a buffer");
        nya_check(nya_trace_capture_format(events, 2, names, nya_trace_feature_count(), 1'000, text, sizeof(text)) == needed, "and matches what is written");

        nya_check(strstr(text, "{\"name\":\"shadows\",\"cat\":\"cpu\",\"ph\":\"X\",\"pid\":1,\"tid\":7,\"ts\":1.000,\"dur\":3.500},") != nullptr,
                  "microseconds since the epoch, comma between events");
        nya_check(strstr(text, "{\"name\":\"crowds\",\"cat\":\"gpu\",\"ph\":\"X\",\"pid\":1,\"tid\":0,\"ts\":5.000,\"dur\":1.000}\n]}") != nullptr,
                  "a GPU group under thread zero, and no comma after the last");

        char small[16];
        nya_check(nya_trace_capture_format(events, 2, names, nya_trace_feature_count(), 1'000, small, sizeof(small)) == needed && small[15] == '\0',
                  "a short buffer is cut and terminated, and still reports the full size");
    }

    return nya_check_failures() == 0 ? 0 : 1;
}
