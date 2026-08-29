/**
 * The ceiling registry: registering a pointer to an existing counter, reading it back sorted by
 * fullness, and the registry's own overflow.
 *
 * No SDL, no app instance — same as test_system.c, and for the same reason: this is a plain array
 * with no dependency on anything else in core.
 **/

#include "nyangine/nyangine.c"
#include "nyangine/nyangine.h"

/* Counts warnings the overflow test expects, without caring what any other sink does with them. */
static u32 warning_count = 0;

static void count_warnings(NYA_LogLevel level, NYA_ConstCString message, u32 length, void* user_data) {
    nya_unused(message);
    nya_unused(length);
    nya_unused(user_data);
    if (level == NYA_LOG_LEVEL_WARN) warning_count++;
}

s32 main(void) {
    // ─────────────────────────────────────────────────────────────────────────────
    // TEST: registering a few ceilings and reading them back.
    // ─────────────────────────────────────────────────────────────────────────────
    {
        _nya_ceiling_registry_reset_for_test();

        static u32 tweens = 3;
        static u32 fonts  = 1;

        nya_ceiling_register("tweens", 512, &tweens);
        nya_ceiling_register("fonts", 32, &fonts);

        nya_assert(nya_ceiling_count() == 2);

        // Fonts is 1/32 (~3%), tweens is 3/512 (~0.6%) — fonts is fuller, so it sorts first.
        nya_assert(nya_string_equals(nya_ceiling_name_at(0), "fonts"));
        nya_assert(nya_ceiling_capacity_at(0) == 32);
        nya_assert(nya_ceiling_live_at(0) == 1);

        nya_assert(nya_string_equals(nya_ceiling_name_at(1), "tweens"));
        nya_assert(nya_ceiling_capacity_at(1) == 512);
        nya_assert(nya_ceiling_live_at(1) == 3);

        // The registry publishes a pointer, not a snapshot: moving the live counter moves the read,
        // and moves the order too — tweens at 500/512 is now fuller than fonts at 1/32.
        tweens = 500;
        nya_assert(nya_string_equals(nya_ceiling_name_at(0), "tweens"), "the registry should read the counter live, not a copy taken at registration");
        nya_assert(nya_ceiling_live_at(0) == 500);
    }

    // ─────────────────────────────────────────────────────────────────────────────
    // TEST: fullness ordering holds regardless of registration order.
    // ─────────────────────────────────────────────────────────────────────────────
    {
        _nya_ceiling_registry_reset_for_test();

        static u32 mostly_empty = 1;
        static u32 mostly_full  = 9;

        // Registered emptiest first — finalize order must not just echo registration order.
        nya_ceiling_register("mostly_empty", 10, &mostly_empty);
        nya_ceiling_register("mostly_full", 10, &mostly_full);

        nya_assert(nya_string_equals(nya_ceiling_name_at(0), "mostly_full"), "the 90%% full ceiling should rank before the 10%% one");
        nya_assert(nya_string_equals(nya_ceiling_name_at(1), "mostly_empty"));

        // The ordering tracks live values, so it can flip: once the tables turn, so does the order.
        mostly_empty = 10;
        mostly_full  = 1;

        nya_assert(nya_string_equals(nya_ceiling_name_at(0), "mostly_empty"), "fullness is read live, so the order should flip too");
        nya_assert(nya_string_equals(nya_ceiling_name_at(1), "mostly_full"));
    }

    // ─────────────────────────────────────────────────────────────────────────────
    // TEST: the registry's own ceiling warns and refuses rather than growing.
    // ─────────────────────────────────────────────────────────────────────────────
    {
        _nya_ceiling_registry_reset_for_test();

        warning_count = 0;
        nya_log_sink_add(count_warnings, nullptr);

        static u32 live = 0;

        u8 names[NYA_CEILING_REGISTRY_MAX + 1][8];
        for (u32 i = 0; i < NYA_CEILING_REGISTRY_MAX; i++) {
            (void)snprintf((char*)names[i], sizeof(names[i]), "c%u", i);
            nya_ceiling_register((NYA_ConstCString)names[i], 1, &live);
        }

        nya_assert(nya_ceiling_count() == NYA_CEILING_REGISTRY_MAX, "every ceiling up to the ceiling(!) should have been accepted");
        nya_assert(warning_count == 0, "filling the registry exactly to its own ceiling should not warn");

        // One more, past the ceiling.
        (void)snprintf((char*)names[NYA_CEILING_REGISTRY_MAX], sizeof(names[NYA_CEILING_REGISTRY_MAX]), "c%u", (u32)NYA_CEILING_REGISTRY_MAX);
        nya_ceiling_register((NYA_ConstCString)names[NYA_CEILING_REGISTRY_MAX], 1, &live);

        nya_assert(nya_ceiling_count() == NYA_CEILING_REGISTRY_MAX, "the count must not grow past the ceiling");
        nya_assert(warning_count == 1, "refusing the extra registration should warn exactly once, warned %u times", warning_count);

        // The refusal did not corrupt what was already there.
        nya_assert(nya_string_equals(nya_ceiling_name_at(0), "c0"));

        nya_log_sink_clear();
    }

    _nya_ceiling_registry_reset_for_test();

    printf("All tests passed.\n");
    return 0;
}
