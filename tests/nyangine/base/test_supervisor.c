/**
 * The supervisor decision: the restart count, the rolling window, the give-up past the cap, the reset
 * after a quiet window, and the backoff schedule between restarts.
 *
 * Driven entirely through nya_supervisor_should_restart with a `now_s` passed in, so every case here is
 * deterministic and touches no clock, no environment and no process — the split base_supervisor.h
 * describes. The jittered delay (nya_supervisor_backoff_ms) is exercised once at the end, only for the
 * one thing that is not deterministic about it: that it stays inside the window nya_supervisor_window_ms
 * reports. The re-exec itself is the syscall half and is not under test here.
 **/

#include "nyangine-core/nyangine.h"

#include "nyangine-core/nyangine.c"

int main(void) {
    // TEST: a zeroed policy is off, and an off supervisor never restarts.
    {
        NYA_Supervisor off;
        nya_supervisor_init(&off, (NYA_SupervisorPolicy){ 0 });

        nya_assert(!nya_supervisor_enabled(&off), "a zeroed policy is opt-out");
        nya_assert(!nya_supervisor_should_restart(&off, 1000, false), "a disabled supervisor never restarts");
        nya_assert(nya_supervisor_restart_count(&off) == 0, "and counts nothing");
    }

    // TEST: a clean exit is never restarted, even when the policy is on.
    {
        NYA_Supervisor supervisor;
        nya_supervisor_init(&supervisor, (NYA_SupervisorPolicy){ .enabled = true, .max_restarts = 3, .window_s = 60 });

        nya_assert(!nya_supervisor_should_restart(&supervisor, 1000, true), "a clean exit is final");
        nya_assert(nya_supervisor_restart_count(&supervisor) == 0, "and spends no budget");
    }

    // TEST: at most max_restarts inside the window, then give up so the crash surfaces.
    {
        NYA_Supervisor supervisor;
        nya_supervisor_init(&supervisor, (NYA_SupervisorPolicy){ .enabled = true, .max_restarts = 3, .window_s = 60 });

        nya_assert(nya_supervisor_should_restart(&supervisor, 1000, false), "restart 1 of 3");
        nya_assert(nya_supervisor_should_restart(&supervisor, 1010, false), "restart 2 of 3");
        nya_assert(nya_supervisor_should_restart(&supervisor, 1020, false), "restart 3 of 3");
        nya_assert(nya_supervisor_restart_count(&supervisor) == 3, "three restarts counted");
        nya_assert(!nya_supervisor_should_restart(&supervisor, 1030, false), "the fourth is where it gives up");
        nya_assert(nya_supervisor_restart_count(&supervisor) == 3, "and a refusal counts nothing");
    }

    // TEST: the backoff window doubles from base_ms with each restart and stops at cap_ms.
    {
        NYA_Supervisor supervisor;
        nya_supervisor_init(&supervisor, (NYA_SupervisorPolicy){ .enabled = true, .max_restarts = 50, .window_s = 100000, .base_ms = 100, .cap_ms = 1000 });

        // The window is read after each grant, over the restart just counted: 100, 200, 400, 800, then capped at 1000 and staying there.
        const u64 expected[] = { 100, 200, 400, 800, 1000, 1000, 1000 };

        for (u32 i = 0; i < nya_carray_length(expected); i++) {
            // A constant now within one huge window, so nothing resets and the count just grows.
            nya_assert(nya_supervisor_should_restart(&supervisor, 5000, false), "still under the cap");
            nya_assert(nya_supervisor_window_ms(&supervisor) == expected[i], "restart %u window was %llu, expected %llu", i + 1,
                       (unsigned long long)nya_supervisor_window_ms(&supervisor), (unsigned long long)expected[i]);
        }
    }

    // TEST: the defaults fill in when a field is left at zero.
    {
        NYA_Supervisor supervisor;
        nya_supervisor_init(&supervisor, (NYA_SupervisorPolicy){ .enabled = true });

        nya_assert(nya_supervisor_window_ms(&supervisor) == (u64)NYA_SUPERVISOR_BASE_MS, "before any restart the window is base_ms");

        // A large restart count saturates at the cap rather than shifting past the width of the type.
        supervisor.restarts = 100;
        nya_assert(nya_supervisor_window_ms(&supervisor) == (u64)NYA_SUPERVISOR_CAP_MS, "a huge count is the cap, not undefined");

        // The default budget is spent after NYA_SUPERVISOR_MAX_RESTARTS restarts in the default window.
        nya_supervisor_init(&supervisor, (NYA_SupervisorPolicy){ .enabled = true });
        for (u32 i = 0; i < (u32)NYA_SUPERVISOR_MAX_RESTARTS; i++) {
            nya_assert(nya_supervisor_should_restart(&supervisor, 1000, false), "default restart %u", i + 1);
        }
        nya_assert(!nya_supervisor_should_restart(&supervisor, 1000, false), "the default budget is spent");
    }

    // TEST: a quiet window resets the budget, so a rare crash is always recovered.
    {
        NYA_Supervisor supervisor;
        nya_supervisor_init(&supervisor, (NYA_SupervisorPolicy){ .enabled = true, .max_restarts = 2, .window_s = 10, .base_ms = 100, .cap_ms = 1000 });

        nya_assert(nya_supervisor_should_restart(&supervisor, 100, false), "restart 1 of 2");
        nya_assert(nya_supervisor_should_restart(&supervisor, 101, false), "restart 2 of 2");
        nya_assert(!nya_supervisor_should_restart(&supervisor, 102, false), "budget spent inside the window");

        // A whole window on from when it opened (100 + 10): the crashes were not a tight loop, so the count resets and the process is recovered again, its backoff starting over at base_ms.
        nya_assert(nya_supervisor_should_restart(&supervisor, 110, false), "a quiet window resets the budget");
        nya_assert(nya_supervisor_restart_count(&supervisor) == 1, "and the count starts over");
        nya_assert(nya_supervisor_window_ms(&supervisor) == 100, "as does the backoff");
    }

    // TEST: a clock that runs backwards opens a fresh window rather than reading a stale one.
    {
        NYA_Supervisor supervisor;
        nya_supervisor_init(&supervisor, (NYA_SupervisorPolicy){ .enabled = true, .max_restarts = 2, .window_s = 10 });

        nya_assert(nya_supervisor_should_restart(&supervisor, 1000, false), "restart 1 of 2");
        nya_assert(nya_supervisor_should_restart(&supervisor, 1001, false), "restart 2 of 2");
        nya_assert(!nya_supervisor_should_restart(&supervisor, 1002, false), "budget spent");

        // now_s before the window's start: treated as a fresh window, not as one still open.
        nya_assert(nya_supervisor_should_restart(&supervisor, 500, false), "a backwards clock does not trap the budget");
        nya_assert(nya_supervisor_restart_count(&supervisor) == 1, "the window reopened");
    }

    // TEST: the jittered delay stays inside the window, whatever it draws.
    {
        NYA_Supervisor supervisor;
        nya_supervisor_init(&supervisor, (NYA_SupervisorPolicy){ .enabled = true, .base_ms = 200, .cap_ms = 4000 });

        for (u32 i = 0; i < 32; i++) {
            supervisor.restarts = i + 1;

            u64 window = nya_supervisor_window_ms(&supervisor);
            u64 delay  = nya_supervisor_backoff_ms(&supervisor);

            nya_assert(delay <= window, "full jitter picks inside the window: delay %llu, window %llu", (unsigned long long)delay,
                       (unsigned long long)window);
        }
    }

    printf("PASSED: supervisor\n");

    return EXIT_SUCCESS;
}
