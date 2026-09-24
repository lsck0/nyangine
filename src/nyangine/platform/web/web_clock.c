#include "nyangine/platform/web/web_clock.h"

#if OS_WASM

#include <emscripten/emscripten.h>

// The two numbers the page can read, each one line of JS. EM_JS emits a C-callable function whose body
// runs in the module's JS scope; performance.now and Date.now are both there. floor, so a whole
// millisecond crosses the boundary rather than a double the caller would have to round anyway.
// clang-format off
EM_JS(double, _nya_web_clock_performance_now, (void), { return Math.floor(performance.now()); })
EM_JS(double, _nya_web_clock_date_now, (void), { return Date.now(); })
// clang-format on

u64 nya_web_clock_monotonic_ms(void) {
    return (u64)_nya_web_clock_performance_now();
}

u64 nya_web_clock_wall_ms(void) {
    return (u64)_nya_web_clock_date_now();
}

#else // native fallback, so the same call compiles and runs off wasm

#include "nyangine/os/os_time.h"

u64 nya_web_clock_monotonic_ms(void) {
    return nya_os_time_monotonic_ns() / 1'000'000ULL;
}

u64 nya_web_clock_wall_ms(void) {
    return nya_os_time_wall_ns() / 1'000'000ULL;
}

#endif // OS_WASM
