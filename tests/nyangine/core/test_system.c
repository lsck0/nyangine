/**
 * The system registry: ordering by `after`, and the loud failures that stand in for the compile
 * time check C does not have.
 *
 * No SDL, no app instance, nothing else brought up first — the registry is a plain array with no
 * dependency on anything else in core, so this is the rare core test that can start straight in.
 **/

#include "nyangine/nyangine.c"
#include "nyangine/nyangine.h"

#define LOG_MAX 16

static NYA_ConstCString log_entries[LOG_MAX];
static u32              log_count = 0;

static void log_reset(void) {
    log_count = 0;
}

static void log_push(NYA_ConstCString what) {
    nya_assert(log_count < LOG_MAX);
    log_entries[log_count] = what;
    log_count++;
}

static b8 log_equals(u32 count, NYA_ConstCString const* expected) {
    if (log_count != count) return false;
    for (u32 i = 0; i < count; i++) {
        if (!nya_string_equals(log_entries[i], expected[i])) return false;
    }
    return true;
}

/* One trio of systems, each logging its own name so a test can read the order calls happened in. */

static NYA_Error a_init(void) {
    log_push("a_init");
    return NYA_OK;
}
static void a_update(f32 delta_time_s) {
    nya_unused(delta_time_s);
    log_push("a_update");
}
static void a_deinit(void) {
    log_push("a_deinit");
}

static NYA_Error b_init(void) {
    log_push("b_init");
    return NYA_OK;
}
static void b_update(f32 delta_time_s) {
    nya_unused(delta_time_s);
    log_push("b_update");
}
static void b_deinit(void) {
    log_push("b_deinit");
}

static NYA_Error c_init(void) {
    log_push("c_init");
    return NYA_OK;
}
static void c_update(f32 delta_time_s) {
    nya_unused(delta_time_s);
    log_push("c_update");
}
static void c_deinit(void) {
    log_push("c_deinit");
}

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
    // TEST: no `after` anywhere — init, update and deinit follow registration order,
    // and deinit is exactly that order reversed.
    // ─────────────────────────────────────────────────────────────────────────────
    {
        _nya_system_registry_reset_for_test();
        log_reset();

        nya_system_register((NYA_SystemEntry){ .name = "a", .init = a_init, .update = a_update, .deinit = a_deinit });
        nya_system_register((NYA_SystemEntry){ .name = "b", .init = b_init, .update = b_update, .deinit = b_deinit });
        nya_system_register((NYA_SystemEntry){ .name = "c", .init = c_init, .update = c_update, .deinit = c_deinit });

        nya_assert(nya_system_registry_finalize().ok);

        nya_assert(nya_system_registry_run_init().ok);
        nya_assert(log_equals(3, (NYA_ConstCString[]){ "a_init", "b_init", "c_init" }), "init should follow registration order");

        log_reset();
        nya_system_registry_run_update(0.016F);
        nya_assert(log_equals(3, (NYA_ConstCString[]){ "a_update", "b_update", "c_update" }), "update should follow the same order");

        log_reset();
        nya_system_registry_run_deinit();
        nya_assert(log_equals(3, (NYA_ConstCString[]){ "c_deinit", "b_deinit", "a_deinit" }), "deinit should be exactly reversed");
    }

    // ─────────────────────────────────────────────────────────────────────────────
    // TEST: an `after` chain resolves in dependency order, even registered out of order.
    // ─────────────────────────────────────────────────────────────────────────────
    {
        _nya_system_registry_reset_for_test();
        log_reset();

        // Registered third, first, second — finalize has to reorder this, not just trust the list.
        nya_system_register((NYA_SystemEntry){ .name = "c", .after = "b", .init = c_init });
        nya_system_register((NYA_SystemEntry){ .name = "a", .init = a_init });
        nya_system_register((NYA_SystemEntry){ .name = "b", .after = "a", .init = b_init });

        nya_assert(nya_system_registry_finalize().ok);

        nya_assert(nya_string_equals(nya_system_registry_name_at(0), "a"));
        nya_assert(nya_string_equals(nya_system_registry_name_at(1), "b"));
        nya_assert(nya_string_equals(nya_system_registry_name_at(2), "c"));

        nya_assert(nya_system_registry_run_init().ok);
        nya_assert(log_equals(3, (NYA_ConstCString[]){ "a_init", "b_init", "c_init" }), "the chain should run a, then b, then c");
    }

    // ─────────────────────────────────────────────────────────────────────────────
    // TEST: `after` naming a system that was never registered fails finalize loudly.
    // ─────────────────────────────────────────────────────────────────────────────
    {
        _nya_system_registry_reset_for_test();

        nya_system_register((NYA_SystemEntry){ .name = "orphan", .after = "nobody" });

        NYA_Error result = nya_system_registry_finalize();
        nya_assert(!result.ok, "an after naming nothing registered must be an error, not a no-op");
        nya_assert(result.kind == NYA_ERROR_INVALID_ARGUMENT);

        NYA_ConstCString message = (NYA_ConstCString)result.message;
        nya_assert(strstr(message, "orphan") != nullptr, "the error should name the system, got: %s", message);
        nya_assert(strstr(message, "nobody") != nullptr, "and the target it could not find, got: %s", message);
    }

    // ─────────────────────────────────────────────────────────────────────────────
    // TEST: a cycle fails finalize loudly and names the systems in it.
    // ─────────────────────────────────────────────────────────────────────────────
    {
        _nya_system_registry_reset_for_test();

        nya_system_register((NYA_SystemEntry){ .name = "x", .after = "y" });
        nya_system_register((NYA_SystemEntry){ .name = "y", .after = "x" });

        NYA_Error result = nya_system_registry_finalize();
        nya_assert(!result.ok, "a cycle must be an error");

        NYA_ConstCString message = (NYA_ConstCString)result.message;
        nya_assert(strstr(message, "x") != nullptr, "the cycle error should name 'x', got: %s", message);
        nya_assert(strstr(message, "y") != nullptr, "and 'y', got: %s", message);
    }

    // ─────────────────────────────────────────────────────────────────────────────
    // TEST: the ceiling warns and refuses rather than growing or corrupting state.
    // ─────────────────────────────────────────────────────────────────────────────
    {
        _nya_system_registry_reset_for_test();

        warning_count = 0;
        nya_log_sink_add(count_warnings, nullptr);

        u8 names[NYA_SYSTEM_REGISTRY_MAX + 1][8];
        for (u32 i = 0; i < NYA_SYSTEM_REGISTRY_MAX; i++) {
            (void)snprintf((char*)names[i], sizeof(names[i]), "s%u", i);
            nya_system_register((NYA_SystemEntry){ .name = (NYA_ConstCString)names[i] });
        }

        nya_assert(nya_system_registry_count() == NYA_SYSTEM_REGISTRY_MAX, "every system up to the ceiling should have been accepted");
        nya_assert(warning_count == 0, "filling the registry exactly to the ceiling should not warn");

        // One more, past the ceiling.
        (void)snprintf((char*)names[NYA_SYSTEM_REGISTRY_MAX], sizeof(names[NYA_SYSTEM_REGISTRY_MAX]), "s%u", (u32)NYA_SYSTEM_REGISTRY_MAX);
        nya_system_register((NYA_SystemEntry){ .name = (NYA_ConstCString)names[NYA_SYSTEM_REGISTRY_MAX] });

        nya_assert(nya_system_registry_count() == NYA_SYSTEM_REGISTRY_MAX, "the count must not grow past the ceiling");
        nya_assert(warning_count == 1, "refusing the extra registration should warn exactly once, warned %u times", warning_count);

        // Finalize still succeeds over exactly-at-the-ceiling data — the refusal did not corrupt it.
        nya_assert(nya_system_registry_finalize().ok);

        nya_log_sink_clear();
    }

    // ─────────────────────────────────────────────────────────────────────────────
    // TEST: init_at/deinit_at, for a caller unwinding only as far as it actually got
    // (core_app.c's own bring-up, which run_init/run_deinit do not fit — see their doc).
    // ─────────────────────────────────────────────────────────────────────────────
    {
        _nya_system_registry_reset_for_test();
        log_reset();

        nya_system_register((NYA_SystemEntry){ .name = "a", .init = a_init, .deinit = a_deinit });
        nya_system_register((NYA_SystemEntry){ .name = "b", .init = b_init, .deinit = b_deinit });
        nya_system_register((NYA_SystemEntry){ .name = "c", .init = c_init, .deinit = c_deinit });

        nya_assert(nya_system_registry_finalize().ok);

        nya_assert(nya_system_registry_init_at(0) == a_init);
        nya_assert(nya_system_registry_init_at(1) == b_init);
        nya_assert(nya_system_registry_init_at(2) == c_init);
        nya_assert(nya_system_registry_deinit_at(0) == a_deinit);
        nya_assert(nya_system_registry_deinit_at(1) == b_deinit);
        nya_assert(nya_system_registry_deinit_at(2) == c_deinit);

        // Hand-rolled bring-up that stops after "b", the way a failure between b and c would: only
        // what actually came up should be unwound.
        (void)nya_system_registry_init_at(0)();
        (void)nya_system_registry_init_at(1)();

        log_reset();
        for (u32 i = 2; i > 0; i--) {
            NYA_SystemDeinitFn deinit = nya_system_registry_deinit_at(i - 1);
            if (deinit != nullptr) deinit();
        }
        nya_assert(log_equals(2, (NYA_ConstCString[]){ "b_deinit", "a_deinit" }), "only a and b came up, so only they should unwind");
    }

    // ─────────────────────────────────────────────────────────────────────────────
    // TEST: running before finalize is a programmer error, not a quiet no-op.
    // ─────────────────────────────────────────────────────────────────────────────
    {
        _nya_system_registry_reset_for_test();

        nya_system_register((NYA_SystemEntry){ .name = "a", .init = a_init, .update = a_update, .deinit = a_deinit });

        nya_expect_crash((void)nya_system_registry_run_init());
        nya_expect_crash(nya_system_registry_run_update(0.016F));
        nya_expect_crash(nya_system_registry_run_deinit());
    }

    _nya_system_registry_reset_for_test();

    printf("All tests passed.\n");
    return 0;
}
