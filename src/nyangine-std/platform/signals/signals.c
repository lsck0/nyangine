#include <signal.h>

#include "nyangine-std/base/base.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API DECLARATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/** What nya_signals_budget_step read from the environment, and what is left of it. Zero is unbounded. */
NYA_INTERNAL struct {
    b8          read;
    b8          done;
    const void* loop;
    u64         frames_left;
    u64         deadline_ns;
} _NYA_SIGNALS_BUDGET;

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PUBLIC API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

void nya_signals_budget_step(const void* loop) {
    if (!_NYA_SIGNALS_BUDGET.read) {
        NYA_ConstCString frames  = getenv("NYA_EXIT_AFTER_FRAMES");
        NYA_ConstCString seconds = getenv("NYA_EXIT_AFTER_SECONDS");

        _NYA_SIGNALS_BUDGET.read        = true;
        _NYA_SIGNALS_BUDGET.loop        = loop;
        _NYA_SIGNALS_BUDGET.frames_left = frames != nullptr ? strtoull(frames, nullptr, 10) : 0;
        _NYA_SIGNALS_BUDGET.deadline_ns = seconds != nullptr ? nya_clock_get_monotonic_ns() + (u64)(strtod(seconds, nullptr) * 1e9) : 0;
        _NYA_SIGNALS_BUDGET.done        = _NYA_SIGNALS_BUDGET.frames_left == 0 && _NYA_SIGNALS_BUDGET.deadline_ns == 0;
    }

    if (_NYA_SIGNALS_BUDGET.done || loop != _NYA_SIGNALS_BUDGET.loop) return;

    b8 out_of_frames = _NYA_SIGNALS_BUDGET.frames_left > 0 && --_NYA_SIGNALS_BUDGET.frames_left == 0;
    b8 out_of_time   = _NYA_SIGNALS_BUDGET.deadline_ns > 0 && nya_clock_get_monotonic_ns() >= _NYA_SIGNALS_BUDGET.deadline_ns;
    if (!out_of_frames && !out_of_time) return;

    _NYA_SIGNALS_BUDGET.done = true;
    nya_log_info("The run budget is spent (NYA_EXIT_AFTER_%s); quitting.", out_of_frames ? "FRAMES" : "SECONDS");

#if OS_WINDOWS
    // the engine's own handler hangs off the console control handler, which raise does not reach.
    NYA_SignalHandler handler = NYA_SIGNALS_TO_CALLBACK_MAP[NYA_SIGNAL_INTERRUPT];
    if (handler != nullptr) {
        handler(NYA_SIGNAL_INTERRUPT);
        return;
    }
#endif

    (void)raise(SIGINT);
}
