/**
 * The reconnect state machine: the attempt counter, the backoff schedule, the attempt cap, and the
 * reset on a good connect.
 *
 * Driven entirely through nya_reconnect_dropped_after, which takes the delay rather than drawing a
 * jittered one, so every case here is deterministic and touches no clock and no random source — the
 * split base_reconnect.h describes. The jittered path (nya_reconnect_dropped) is exercised once at the
 * end, only for the one thing that is not deterministic about it: that the delay stays inside the
 * window nya_reconnect_window_ms reports.
 **/

#include "nyangine-core/nyangine.h"

#include "nyangine-core/nyangine.c"

int main(void) {
    // TEST: a zeroed policy is off, and an off reconnect never schedules anything.
    {
        NYA_Reconnect off;
        nya_reconnect_init(&off, (NYA_ReconnectPolicy){ 0 });

        nya_assert(!nya_reconnect_enabled(&off), "a zeroed policy is opt-out");
        nya_assert(!nya_reconnect_dropped_after(&off, 1000, 500), "a disabled reconnect says a drop is final");
        nya_assert(!nya_reconnect_dropped(&off, 1000), "and so does the jittered entry point");
        nya_assert(!nya_reconnect_waiting(&off), "nothing was scheduled");
    }

    // TEST: the window doubles from base_ms and stops at cap_ms.
    {
        NYA_Reconnect reconnect;
        nya_reconnect_init(&reconnect, (NYA_ReconnectPolicy){ .enabled = true, .base_ms = 100, .cap_ms = 1000 });

        // attempt 0..N: 100, 200, 400, 800, then capped at 1000 and staying there.
        const u64 expected[] = { 100, 200, 400, 800, 1000, 1000, 1000 };

        for (u32 i = 0; i < nya_carray_length(expected); i++) {
            nya_assert(nya_reconnect_window_ms(&reconnect) == expected[i], "attempt %u window was %llu, expected %llu", i,
                       (unsigned long long)nya_reconnect_window_ms(&reconnect), (unsigned long long)expected[i]);

            nya_assert(nya_reconnect_dropped_after(&reconnect, 0, 0), "still under an unlimited attempt cap");
        }
    }

    // TEST: the defaults fill in when a bound is left at zero.
    {
        NYA_Reconnect reconnect;
        nya_reconnect_init(&reconnect, (NYA_ReconnectPolicy){ .enabled = true });

        nya_assert(nya_reconnect_window_ms(&reconnect) == (u64)NYA_RECONNECT_BASE_MS, "attempt 0 is base_ms");

        // A large attempt saturates at the cap rather than shifting past the width of the type.
        reconnect.attempt = 100;
        nya_assert(nya_reconnect_window_ms(&reconnect) == (u64)NYA_RECONNECT_CAP_MS, "a huge attempt is the cap, not undefined");
    }

    // TEST: a scheduled retry is due only once now_ms reaches it.
    {
        NYA_Reconnect reconnect;
        nya_reconnect_init(&reconnect, (NYA_ReconnectPolicy){ .enabled = true, .base_ms = 500, .cap_ms = 30000 });

        nya_assert(nya_reconnect_dropped_after(&reconnect, 1000, 500), "scheduled at 1500");
        nya_assert(nya_reconnect_waiting(&reconnect));

        nya_assert(!nya_reconnect_due(&reconnect, 1499), "not yet");
        nya_assert(nya_reconnect_remaining_ms(&reconnect, 1499) == 1, "one ms to go");
        nya_assert(nya_reconnect_due(&reconnect, 1500), "due at the mark");
        nya_assert(nya_reconnect_due(&reconnect, 5000), "and past it");
        nya_assert(nya_reconnect_remaining_ms(&reconnect, 1500) == 0, "nothing left once due");
    }

    // TEST: a good connect resets the backoff to base_ms.
    {
        NYA_Reconnect reconnect;
        nya_reconnect_init(&reconnect, (NYA_ReconnectPolicy){ .enabled = true, .base_ms = 100, .cap_ms = 1000 });

        nya_assert(nya_reconnect_dropped_after(&reconnect, 0, 100));
        nya_assert(nya_reconnect_dropped_after(&reconnect, 0, 200));
        nya_assert(nya_reconnect_window_ms(&reconnect) == 400, "two drops in, the window has doubled twice");

        nya_reconnect_connected(&reconnect);

        nya_assert(!nya_reconnect_waiting(&reconnect), "a connect clears any pending retry");
        nya_assert(nya_reconnect_window_ms(&reconnect) == 100, "and the next drop starts over at base_ms");
    }

    // TEST: max_attempts gives up after exactly that many retries.
    {
        NYA_Reconnect reconnect;
        nya_reconnect_init(&reconnect, (NYA_ReconnectPolicy){ .enabled = true, .base_ms = 10, .cap_ms = 100, .max_attempts = 3 });

        nya_assert(nya_reconnect_dropped_after(&reconnect, 0, 10), "retry 1 of 3");
        nya_assert(nya_reconnect_dropped_after(&reconnect, 0, 20), "retry 2 of 3");
        nya_assert(nya_reconnect_dropped_after(&reconnect, 0, 40), "retry 3 of 3");
        nya_assert(!nya_reconnect_dropped_after(&reconnect, 0, 80), "the fourth drop is where it gives up");

        // A connect in between resets the budget, so the socket gets its full run of retries again.
        nya_reconnect_connected(&reconnect);
        nya_assert(nya_reconnect_dropped_after(&reconnect, 0, 10), "the attempt budget is spent per run of failures, not for the socket's life");
    }

    // TEST: the jittered delay stays inside the window, whatever it draws.
    {
        NYA_Reconnect reconnect;
        nya_reconnect_init(&reconnect, (NYA_ReconnectPolicy){ .enabled = true, .base_ms = 200, .cap_ms = 4000 });

        for (u32 i = 0; i < 32; i++) {
            u64 window = nya_reconnect_window_ms(&reconnect);
            u64 now    = 1000000 + i;

            nya_assert(nya_reconnect_dropped(&reconnect, now), "unlimited attempts, so every drop schedules");

            u64 delay = nya_reconnect_remaining_ms(&reconnect, now);
            nya_assert(delay <= window, "full jitter picks inside the window: delay %llu, window %llu", (unsigned long long)delay,
                       (unsigned long long)window);
        }
    }

    printf("PASSED: reconnect\n");

    return EXIT_SUCCESS;
}
