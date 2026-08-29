#include "nyangine/nyangine.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * INTERNALS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

typedef struct {
    NYA_ConstCString group;
    u32              cases;
    b8               failed;

    /** The first case's per-item cost, so later ones can be reported relative to it. */
    f64 baseline_ns;
} _NYA_BenchState;

NYA_INTERNAL _NYA_BenchState _nya_bench_state = { 0 };

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PUBLIC API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

void nya_bench_begin(NYA_ConstCString group) {
    _nya_bench_state = (_NYA_BenchState){ .group = group != nullptr ? group : "bench" };

    (void)printf("\n== %s ==\n", _nya_bench_state.group);
    (void)printf("  %-26s %13s %12s %12s %8s %9s\n", "case", "ns/iter", "ns/item", "median", "spread", "vs first");
    (void)fflush(stdout);
}

NYA_INTERNAL int _nya_bench_compare(const void* left, const void* right) {
    f64 a = *(const f64*)left;
    f64 b = *(const f64*)right;

    if (a < b) return -1;
    if (a > b) return 1;

    return 0;
}

void nya_bench_report(NYA_ConstCString name, f64* samples, u32 sample_count, u64 batch, u64 items) {
    if (samples == nullptr || sample_count == 0) {
        (void)printf("  %-26s %13s\n", name, "no samples");
        _nya_bench_state.failed = true;
        return;
    }

    qsort(samples, sample_count, sizeof(f64), _nya_bench_compare);

    /*
     * The best sample is the result, and the median is printed beside it.
     *
     * A benchmark competes with every other process on the machine, so a slow sample is contamination
     * and a fast one is not — the minimum is the closest thing to what the code would do alone. The
     * median is there so a case where the two disagree, which means the machine was busy, is visible
     * rather than quietly believed.
     */
    f64 best   = samples[0];
    f64 median = samples[sample_count / 2];
    f64 worst  = samples[sample_count - 1];

    f64 per_item = items > 0 ? best / (f64)items : 0.0;
    f64 spread   = best > 0.0 ? worst / best : 0.0;

    if (_nya_bench_state.cases == 0) _nya_bench_state.baseline_ns = best;

    char relative[32] = "-";
    if (_nya_bench_state.cases > 0 && best > 0.0) {
        (void)snprintf(relative, sizeof(relative), "%.2fx", _nya_bench_state.baseline_ns / best);
    }

    char per_item_text[32] = "-";
    if (items > 0) (void)snprintf(per_item_text, sizeof(per_item_text), "%.3f", per_item);

    (void)printf("  %-26s %13.1f %12s %12.1f %7.2fx %9s\n", name, best, per_item_text, median, spread, relative);

    // A wide spread means the numbers are contaminated, and saying so is more useful than a footnote
    // nobody reads. Not a failure: a busy machine is not a broken benchmark.
    if (spread > 2.0) {
        (void)printf("  %-26s   (noisy: worst round was %.2fx the best; batch %llu)\n", "", spread, (unsigned long long)batch);
    }

    (void)fflush(stdout);
    _nya_bench_state.cases++;
}

s32 nya_bench_end(void) {
    (void)printf("  %u case%s.\n", _nya_bench_state.cases, _nya_bench_state.cases == 1 ? "" : "s");
    (void)fflush(stdout);

    return _nya_bench_state.failed ? 1 : 0;
}
