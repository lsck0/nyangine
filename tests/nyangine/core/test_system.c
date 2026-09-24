/**
 * The system registry: ordering by `after` and `before`, the phases, turning a system off and on,
 * unregistering one, changing the registry from inside a run, and the loud failures that stand in for
 * the compile time check C does not have.
 **/

#include "nyangine/nyangine.c"
#include "nyangine/nyangine.h"

#define LOG_MAX 32

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

/** What the log holds, joined, so a failure says what actually ran instead of just "false". */
static NYA_ConstCString log_text(void) {
    static char buffer[LOG_MAX * 24];

    u64 length = 0;
    for (u32 i = 0; i < log_count && length < sizeof(buffer); i++) {
        s32 written = snprintf(&buffer[length], sizeof(buffer) - length, "%s%s", i > 0 ? " " : "", log_entries[i]);
        if (written > 0) length += (u64)written;
    }

    buffer[nya_min(length, (u64)sizeof(buffer) - 1)] = '\0';
    return buffer;
}

/* One trio of systems, each logging its own name so a test can read the order calls happened in. */

static NYA_Error a_init(void) {
    log_push("a_init");
    return NYA_OK;
}
static void a_tick(f32 delta_time_s) {
    nya_unused(delta_time_s);
    log_push("a_tick");
}
static void a_deinit(void) {
    log_push("a_deinit");
}

static NYA_Error b_init(void) {
    log_push("b_init");
    return NYA_OK;
}
static void b_tick(f32 delta_time_s) {
    nya_unused(delta_time_s);
    log_push("b_tick");
}
static void b_deinit(void) {
    log_push("b_deinit");
}

static NYA_Error c_init(void) {
    log_push("c_init");
    return NYA_OK;
}
static void c_tick(f32 delta_time_s) {
    nya_unused(delta_time_s);
    log_push("c_tick");
}
static void c_deinit(void) {
    log_push("c_deinit");
}

/* One system per phase, for the test that the three are separate lists over one order. */

static void a_frame(f32 delta_time_s) {
    nya_unused(delta_time_s);
    log_push("a_frame");
}
static void a_render(f32 delta_time_s) {
    nya_unused(delta_time_s);
    log_push("a_render");
}
static void b_render(f32 delta_time_s) {
    nya_unused(delta_time_s);
    log_push("b_render");
}

/* A failing init, for the optional and unwind tests. */

static NYA_Error fails_init(void) {
    log_push("fails_init");
    return nya_error(NYA_ERROR_NOT_OK, "no device");
}
static void fails_deinit(void) {
    log_push("fails_deinit");
}

/* Systems that change the registry from inside their own callback, for the barrier tests. */

static void late_tick(f32 delta_time_s) {
    nya_unused(delta_time_s);
    log_push("late_tick");
}

static void registers_late_tick(f32 delta_time_s) {
    nya_unused(delta_time_s);
    log_push("registrar_tick");

    // the reason a registration from here queues rather than applies.
    nya_assert(nya_system_registry_is_running(), "a system's own tick is inside a run");

    // guarded, because this runs every tick and a duplicate name is an assert.
    if (nya_system_registry_count() < 2) {
        nya_system_register((NYA_SystemEntry){ .name = "late", .tick = nya_callback(late_tick) });
    }
}

static void disables_b(f32 delta_time_s) {
    nya_unused(delta_time_s);
    log_push("a_tick");
    nya_system_disable("b");
}

/** Long enough that the clock resolves it, so the accounting test reads a real number. */
#define SLOW_TICK_NS 200'000

static void slow_tick(f32 delta_time_s) {
    nya_unused(delta_time_s);

    u64 started_ns = nya_clock_get_monotonic_ns();
    while (nya_clock_get_monotonic_ns() - started_ns < SLOW_TICK_NS) {}
}

/** Runs a phase from inside a phase, which the registry refuses. */
static void reenters(f32 delta_time_s) {
    nya_unused(delta_time_s);
    nya_system_registry_run(NYA_SYSTEM_PHASE_TICK, 0.016F);
}

/* A system that holds memory, for the per-owner memory line. */

#define PLUGIN_MEMORY_BYTES 4096

static u64 plugin_memory_bytes(void) {
    return PLUGIN_MEMORY_BYTES;
}

static void plugin_tick(f32 delta_time_s) {
    nya_unused(delta_time_s);
    log_push("plugin_tick");
}

/* A part that brings something into the process, and one that cannot start without it. */

static NYA_Error provider_init(void) {
    log_push("provider_init");
    return NYA_OK;
}
static void provider_deinit(void) {
    log_push("provider_deinit");
}

static NYA_Error dependent_init(void) {
    log_push("dependent_init");
    return NYA_OK;
}

/* A system whose init registers another one, which is what re-sorts the array bring-up is walking. */

static void registered_during_init_tick(f32 delta_time_s) {
    nya_unused(delta_time_s);
}

static NYA_Error registers_during_init(void) {
    log_push("registrar_init");

    // `before` rather than `after`, so the new entry sorts ahead of the one registering it: an
    // index-based bring-up would step over everything the sort moved past its cursor.
    nya_system_register((NYA_SystemEntry){ .name = "early", .before = "registrar", .init = nya_callback(a_init), .tick = nya_callback(registered_during_init_tick) });

    return NYA_OK;
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
    // TEST: no ordering constraint anywhere: init, tick and deinit follow registration
    // order, and deinit is exactly that order reversed.
    {
        _nya_system_registry_reset_for_test();
        log_reset();

        nya_system_register((NYA_SystemEntry){ .name = "a", .init = nya_callback(a_init), .tick = nya_callback(a_tick), .deinit = nya_callback(a_deinit) });
        nya_system_register((NYA_SystemEntry){ .name = "b", .init = nya_callback(b_init), .tick = nya_callback(b_tick), .deinit = nya_callback(b_deinit) });
        nya_system_register((NYA_SystemEntry){ .name = "c", .init = nya_callback(c_init), .tick = nya_callback(c_tick), .deinit = nya_callback(c_deinit) });

        nya_assert(nya_system_registry_finalize().ok);

        nya_assert(nya_system_registry_run_init().ok);
        nya_assert(log_equals(3, (NYA_ConstCString[]){ "a_init", "b_init", "c_init" }), "init should follow registration order, got: %s", log_text());

        log_reset();
        nya_system_registry_run(NYA_SYSTEM_PHASE_TICK, 0.016F);
        nya_assert(log_equals(3, (NYA_ConstCString[]){ "a_tick", "b_tick", "c_tick" }), "tick should follow the same order, got: %s", log_text());

        log_reset();
        nya_system_registry_run_deinit();
        nya_assert(log_equals(3, (NYA_ConstCString[]){ "c_deinit", "b_deinit", "a_deinit" }), "deinit should be exactly reversed, got: %s", log_text());
    }

    // TEST: an `after` chain resolves in dependency order, even registered out of order.
    {
        _nya_system_registry_reset_for_test();
        log_reset();

        // registered third, first, second, so finalize has to reorder.
        nya_system_register((NYA_SystemEntry){ .name = "c", .after = "b", .init = nya_callback(c_init) });
        nya_system_register((NYA_SystemEntry){ .name = "a", .init = nya_callback(a_init) });
        nya_system_register((NYA_SystemEntry){ .name = "b", .after = "a", .init = nya_callback(b_init) });

        nya_assert(nya_system_registry_finalize().ok);

        nya_assert(nya_string_equals(nya_system_registry_at(0)->name, "a"));
        nya_assert(nya_string_equals(nya_system_registry_at(1)->name, "b"));
        nya_assert(nya_string_equals(nya_system_registry_at(2)->name, "c"));

        nya_assert(nya_system_registry_run_init().ok);
        nya_assert(log_equals(3, (NYA_ConstCString[]){ "a_init", "b_init", "c_init" }), "the chain should run a, then b, then c, got: %s", log_text());
    }

    // TEST: `before` puts a system ahead of one registered earlier, which `after` alone
    // cannot express. This is what keeps the game's systems inside the engine's tick.
    {
        _nya_system_registry_reset_for_test();
        log_reset();

        nya_system_register((NYA_SystemEntry){ .name = "a", .tick = nya_callback(a_tick) });
        nya_system_register((NYA_SystemEntry){ .name = "c", .after = "a", .tick = nya_callback(c_tick) });

        // registered last, runs in the middle: b has to land between a and c.
        nya_system_register((NYA_SystemEntry){ .name = "b", .after = "a", .before = "c", .tick = nya_callback(b_tick) });

        nya_assert(nya_system_registry_finalize().ok);

        nya_system_registry_run(NYA_SYSTEM_PHASE_TICK, 0.016F);
        nya_assert(log_equals(3, (NYA_ConstCString[]){ "a_tick", "b_tick", "c_tick" }), "before should pull b ahead of c, got: %s", log_text());
    }

    // TEST: the three phases are three lists over one order, and a system only appears
    // in the phases it has a callback for.
    {
        _nya_system_registry_reset_for_test();
        log_reset();

        nya_system_register((NYA_SystemEntry){ .name = "a", .frame = nya_callback(a_frame), .tick = nya_callback(a_tick), .render = nya_callback(a_render) });
        nya_system_register((NYA_SystemEntry){ .name = "b", .after = "a", .tick = nya_callback(b_tick), .render = nya_callback(b_render) });

        nya_assert(nya_system_registry_finalize().ok);

        nya_system_registry_run(NYA_SYSTEM_PHASE_FRAME, 0.016F);
        nya_assert(log_equals(1, (NYA_ConstCString[]){ "a_frame" }), "only a has frame work, got: %s", log_text());

        log_reset();
        nya_system_registry_run(NYA_SYSTEM_PHASE_TICK, 0.016F);
        nya_assert(log_equals(2, (NYA_ConstCString[]){ "a_tick", "b_tick" }), "both tick, in order, got: %s", log_text());

        log_reset();
        nya_system_registry_run(NYA_SYSTEM_PHASE_RENDER, 0.016F);
        nya_assert(log_equals(2, (NYA_ConstCString[]){ "a_render", "b_render" }), "both render, same order, got: %s", log_text());
    }

    // TEST: disable stops the phase callbacks and nothing else. The system stays
    // registered, stays initialized, and picks up again on enable.
    {
        _nya_system_registry_reset_for_test();
        log_reset();

        nya_system_register((NYA_SystemEntry){ .name = "a", .init = nya_callback(a_init), .tick = nya_callback(a_tick), .deinit = nya_callback(a_deinit) });
        nya_system_register((NYA_SystemEntry){ .name = "b", .after = "a", .init = nya_callback(b_init), .tick = nya_callback(b_tick), .deinit = nya_callback(b_deinit) });

        nya_assert(nya_system_registry_finalize().ok);
        nya_assert(nya_system_registry_run_init().ok);

        nya_assert(nya_system_is_enabled("a"), "a system is registered enabled");

        nya_system_disable("a");
        nya_assert(!nya_system_is_enabled("a"));
        nya_assert(nya_system_registry_initialized_at(0), "disabling must not tear the system down");
        nya_assert(nya_system_registry_count() == 2, "disabling must not remove the system");

        log_reset();
        nya_system_registry_run(NYA_SYSTEM_PHASE_TICK, 0.016F);
        nya_assert(log_equals(1, (NYA_ConstCString[]){ "b_tick" }), "a is off, so only b ticks, got: %s", log_text());

        nya_system_enable("a");

        log_reset();
        nya_system_registry_run(NYA_SYSTEM_PHASE_TICK, 0.016F);
        nya_assert(log_equals(2, (NYA_ConstCString[]){ "a_tick", "b_tick" }), "a is back in its old place, got: %s", log_text());

        // and the deinit of a disabled system still runs: enablement is about the phases only.
        nya_system_disable("b");
        log_reset();
        nya_system_registry_run_deinit();
        nya_assert(log_equals(2, (NYA_ConstCString[]){ "b_deinit", "a_deinit" }), "a disabled system still tears down, got: %s", log_text());
    }

    // TEST: unregister is the partner of register: it tears the system down, takes it
    // out of the order, is idempotent, and does nothing for a name nobody registered.
    {
        _nya_system_registry_reset_for_test();
        log_reset();

        nya_system_register((NYA_SystemEntry){ .name = "a", .init = nya_callback(a_init), .tick = nya_callback(a_tick), .deinit = nya_callback(a_deinit) });
        nya_system_register((NYA_SystemEntry){ .name = "b", .init = nya_callback(b_init), .tick = nya_callback(b_tick), .deinit = nya_callback(b_deinit) });
        nya_system_register((NYA_SystemEntry){ .name = "c", .init = nya_callback(c_init), .tick = nya_callback(c_tick), .deinit = nya_callback(c_deinit) });

        nya_assert(nya_system_registry_finalize().ok);
        nya_assert(nya_system_registry_run_init().ok);

        log_reset();
        nya_system_unregister("b");
        nya_assert(log_equals(1, (NYA_ConstCString[]){ "b_deinit" }), "what came up goes down as it leaves, got: %s", log_text());
        nya_assert(nya_system_registry_count() == 2);

        // idempotent, and safe for a name that was never there.
        log_reset();
        nya_system_unregister("b");
        nya_system_unregister("nobody");
        nya_assert(log_count == 0, "a second unregister has nothing to tear down, got: %s", log_text());
        nya_assert(nya_system_registry_count() == 2);

        // the rest keep their order rather than being swapped about.
        nya_assert(nya_string_equals(nya_system_registry_at(0)->name, "a"));
        nya_assert(nya_string_equals(nya_system_registry_at(1)->name, "c"));

        log_reset();
        nya_system_registry_run(NYA_SYSTEM_PHASE_TICK, 0.016F);
        nya_assert(log_equals(2, (NYA_ConstCString[]){ "a_tick", "c_tick" }), "b is gone from the tick, got: %s", log_text());
    }

    // TEST: a system registered after finalize lands in the order `after` asks for,
    // rather than on the end.
    {
        _nya_system_registry_reset_for_test();
        log_reset();

        nya_system_register((NYA_SystemEntry){ .name = "a", .tick = nya_callback(a_tick) });
        nya_system_register((NYA_SystemEntry){ .name = "c", .after = "a", .tick = nya_callback(c_tick) });

        nya_assert(nya_system_registry_finalize().ok);

        // well after the schedule was settled, the way a plugin loaded mid-game would.
        nya_system_register((NYA_SystemEntry){ .name = "b", .after = "a", .before = "c", .tick = nya_callback(b_tick) });

        nya_system_registry_run(NYA_SYSTEM_PHASE_TICK, 0.016F);
        nya_assert(log_equals(3, (NYA_ConstCString[]){ "a_tick", "b_tick", "c_tick" }), "a late registration still sorts, got: %s", log_text());
    }

    // TEST: a registration made from inside a run takes effect at the barrier, not
    // half way down the loop, so every system in one run sees the same schedule.
    {
        _nya_system_registry_reset_for_test();
        log_reset();

        nya_system_register((NYA_SystemEntry){ .name = "registrar", .tick = nya_callback(registers_late_tick) });

        nya_assert(nya_system_registry_finalize().ok);

        nya_assert(!nya_system_registry_is_running(), "outside a run nothing is running");
        nya_system_registry_run(NYA_SYSTEM_PHASE_TICK, 0.016F);
        nya_assert(!nya_system_registry_is_running(), "and the run is over once it returns");
        nya_assert(log_equals(1, (NYA_ConstCString[]){ "registrar_tick" }), "the new system must not run in the run that added it, got: %s", log_text());
        nya_assert(nya_system_registry_count() == 2, "but it is registered by the time the run returns");

        log_reset();
        nya_system_registry_run(NYA_SYSTEM_PHASE_TICK, 0.016F);
        nya_assert(log_equals(2, (NYA_ConstCString[]){ "registrar_tick", "late_tick" }), "and runs from the next one, got: %s", log_text());
    }

    // TEST: the same for disabling. A system switched off from inside a run still runs
    // that run, because the decision belongs to the barrier.
    {
        _nya_system_registry_reset_for_test();
        log_reset();

        nya_system_register((NYA_SystemEntry){ .name = "a", .tick = nya_callback(disables_b) });
        nya_system_register((NYA_SystemEntry){ .name = "b", .after = "a", .tick = nya_callback(b_tick) });

        nya_assert(nya_system_registry_finalize().ok);

        nya_system_registry_run(NYA_SYSTEM_PHASE_TICK, 0.016F);
        nya_assert(log_equals(2, (NYA_ConstCString[]){ "a_tick", "b_tick" }), "b was already scheduled for this run, got: %s", log_text());
        nya_assert(!nya_system_is_enabled("b"), "and is off by the time the run returns");

        log_reset();
        nya_system_registry_run(NYA_SYSTEM_PHASE_TICK, 0.016F);
        nya_assert(log_equals(1, (NYA_ConstCString[]){ "a_tick" }), "so it is skipped from the next run, got: %s", log_text());
    }

    // TEST: an optional system whose init fails is skipped, stays uninitialized so its
    // deinit is skipped too, and does not stop the systems after it.
    {
        _nya_system_registry_reset_for_test();
        log_reset();

        nya_system_register((NYA_SystemEntry){ .name = "a", .init = nya_callback(a_init), .deinit = nya_callback(a_deinit) });
        nya_system_register((NYA_SystemEntry){ .name     = "fails",
                                               .after    = "a",
                                               .init     = nya_callback(fails_init),
                                               .deinit   = nya_callback(fails_deinit),
                                               .optional = true });
        nya_system_register((NYA_SystemEntry){ .name = "c", .after = "fails", .init = nya_callback(c_init), .deinit = nya_callback(c_deinit) });

        nya_assert(nya_system_registry_finalize().ok);

        nya_assert(nya_system_registry_run_init().ok, "an optional failure is not a failed bring-up");
        nya_assert(log_equals(3, (NYA_ConstCString[]){ "a_init", "fails_init", "c_init" }), "bring-up carries on past it, got: %s", log_text());

        nya_assert(nya_system_registry_initialized_at(0));
        nya_assert(!nya_system_registry_initialized_at(1), "the one that failed never came up");
        nya_assert(nya_system_registry_initialized_at(2));

        log_reset();
        nya_system_registry_run_deinit();
        nya_assert(log_equals(2, (NYA_ConstCString[]){ "c_deinit", "a_deinit" }), "and is not torn down either, got: %s", log_text());
    }

    // TEST: a mandatory system whose init fails unwinds exactly what came up, in
    // reverse, and leaves nothing after it initialized.
    {
        _nya_system_registry_reset_for_test();
        log_reset();

        nya_system_register((NYA_SystemEntry){ .name = "a", .init = nya_callback(a_init), .deinit = nya_callback(a_deinit) });
        nya_system_register((NYA_SystemEntry){ .name = "b", .after = "a", .init = nya_callback(b_init), .deinit = nya_callback(b_deinit) });
        nya_system_register((NYA_SystemEntry){ .name = "fails", .after = "b", .init = nya_callback(fails_init), .deinit = nya_callback(fails_deinit) });
        nya_system_register((NYA_SystemEntry){ .name = "c", .after = "fails", .init = nya_callback(c_init), .deinit = nya_callback(c_deinit) });

        nya_assert(nya_system_registry_finalize().ok);

        NYA_Error result = nya_system_registry_run_init();
        nya_assert(!result.ok, "a mandatory failure must be reported");

        nya_assert(log_equals(5, (NYA_ConstCString[]){ "a_init", "b_init", "fails_init", "b_deinit", "a_deinit" }),
                   "only a and b came up, so only they unwind, and c never ran, got: %s", log_text());

        for (u32 i = 0; i < nya_system_registry_count(); i++) {
            nya_assert(!nya_system_registry_initialized_at(i), "a failed bring-up leaves nothing standing");
        }
    }

    // TEST: systems are grouped by owner, and the owner's totals count its systems and
    // sum the memory they report.
    {
        _nya_system_registry_reset_for_test();
        log_reset();

        nya_system_register((NYA_SystemEntry){ .name = "engine_a", .tick = nya_callback(a_tick) });
        nya_system_register((NYA_SystemEntry){ .name = "game_a", .after = "engine_a", .tick = nya_callback(b_tick), .owner = { .kind = NYA_SYSTEM_OWNER_GAME } });
        nya_system_register((NYA_SystemEntry){ .name         = "plugin_a",
                                               .after        = "game_a",
                                               .tick         = nya_callback(plugin_tick),
                                               .owner        = { .kind = NYA_SYSTEM_OWNER_PLUGIN, .plugin = "demo" },
                                               .memory_bytes = nya_callback(plugin_memory_bytes) });

        nya_assert(nya_system_registry_finalize().ok);

        nya_assert(nya_system_owner_count() == 3, "engine, game and one plugin");

        NYA_SystemOwnerStats engine = nya_system_owner_stats_at(0);
        NYA_SystemOwnerStats game   = nya_system_owner_stats_at(1);
        NYA_SystemOwnerStats plugin = nya_system_owner_stats_at(2);

        nya_assert(nya_string_equals(engine.name, "engine"), "an unowned system belongs to the engine, got '%s'", engine.name);
        nya_assert(nya_string_equals(game.name, "game"));
        nya_assert(nya_string_equals(plugin.name, "demo"), "a plugin reports under its own name, got '%s'", plugin.name);

        nya_assert(engine.system_count == 1 && game.system_count == 1 && plugin.system_count == 1);
        nya_assert(plugin.memory_bytes == PLUGIN_MEMORY_BYTES, "the plugin's memory is its own");
        nya_assert(engine.memory_bytes == 0, "a system with no memory callback reports nothing");

        nya_system_disable("plugin_a");
        plugin = nya_system_owner_stats_at(2);
        nya_assert(plugin.enabled_count == 0, "a disabled system still counts, but not as enabled");
    }

    // TEST: accounting is off until asked for, and then a run books time against the
    // system that spent it.
    {
        _nya_system_registry_reset_for_test();
        log_reset();

        nya_system_register((NYA_SystemEntry){ .name = "a", .tick = nya_callback(slow_tick) });
        nya_assert(nya_system_registry_finalize().ok);

        nya_assert(!nya_system_accounting_is_enabled(), "nobody pays for numbers they did not ask for");

        nya_system_registry_run(NYA_SYSTEM_PHASE_TICK, 0.016F);
        nya_system_accounting_frame_end();
        nya_assert(nya_system_registry_time_ns_at(0) == 0, "and nothing is measured while it is off");

        nya_system_accounting_enable();
        nya_assert(nya_system_accounting_is_enabled());

        nya_system_registry_run(NYA_SYSTEM_PHASE_TICK, 0.016F);

        // still zero: the window only closes at the end of the frame, since the tick phase runs a
        // variable number of times per frame.
        nya_assert(nya_system_registry_time_ns_at(0) == 0, "the measured window is published, not read live");

        nya_system_accounting_frame_end();
        nya_assert(nya_system_registry_time_ns_at(0) > 0, "and then the frame's time is there to read");

        nya_system_accounting_disable();
    }

    // TEST: `after` naming a system that was never registered fails finalize loudly.
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

    // TEST: the same for `before`.
    {
        _nya_system_registry_reset_for_test();

        nya_system_register((NYA_SystemEntry){ .name = "orphan", .before = "nobody" });

        NYA_Error result = nya_system_registry_finalize();
        nya_assert(!result.ok, "a before naming nothing registered must be an error too");
        nya_assert(result.kind == NYA_ERROR_INVALID_ARGUMENT);

        NYA_ConstCString message = (NYA_ConstCString)result.message;
        nya_assert(strstr(message, "orphan") != nullptr, "the error should name the system, got: %s", message);
        nya_assert(strstr(message, "nobody") != nullptr, "and the target it could not find, got: %s", message);
    }

    // TEST: a cycle fails finalize loudly and names the systems in it.
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

    // TEST: a cycle spelled with `before` instead of `after` is caught the same way.
    {
        _nya_system_registry_reset_for_test();

        nya_system_register((NYA_SystemEntry){ .name = "x", .before = "y" });
        nya_system_register((NYA_SystemEntry){ .name = "y", .before = "x" });

        NYA_Error result = nya_system_registry_finalize();
        nya_assert(!result.ok, "a cycle through `before` is still a cycle");

        NYA_ConstCString message = (NYA_ConstCString)result.message;
        nya_assert(strstr(message, "x") != nullptr, "the cycle error should name 'x', got: %s", message);
        nya_assert(strstr(message, "y") != nullptr, "and 'y', got: %s", message);
    }

    // TEST: the ceiling warns and refuses rather than growing or corrupting state.
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

        // finalize still succeeds with data exactly at the ceiling; the refusal did not corrupt it.
        nya_assert(nya_system_registry_finalize().ok);

        nya_log_sink_clear();
    }

    // TEST: a facility is what one system brings and another is checked against.
    {
        _nya_system_registry_reset_for_test();
        log_reset();

        nya_system_register((NYA_SystemEntry){ .name     = "provider",
                                               .init     = nya_callback(provider_init),
                                               .deinit   = nya_callback(provider_deinit),
                                               .provides = NYA_SYSTEM_FACILITY_WINDOW });
        nya_system_register((NYA_SystemEntry){ .name  = "dependent",
                                               .after = "provider",
                                               .init  = nya_callback(dependent_init),
                                               .needs = NYA_SYSTEM_FACILITY_WINDOW });

        nya_assert(nya_system_registry_finalize().ok);
        nya_assert(nya_system_facilities() == 0, "nothing is up before bring-up runs");
        nya_assert(nya_system_registry_run_init().ok, "the provider comes first, so the dependent has what it asked for");

        nya_assert((nya_system_facilities() & NYA_SYSTEM_FACILITY_WINDOW) != 0, "the provider's facility is up while it is");

        static NYA_ConstCString const expected[] = { "provider_init", "dependent_init" };
        nya_assert(log_equals(2, expected), "bring-up ran '%s'", log_text());

        nya_system_registry_run_deinit();

        nya_assert(nya_system_facilities() == 0, "a facility goes away with the system that provided it");
    }

    // TEST: a facility nothing provides. A mandatory system stops bring-up by name;
    // an optional one is stepped over, exactly as a failed init is.
    {
        _nya_system_registry_reset_for_test();
        log_reset();

        nya_system_register((NYA_SystemEntry){ .name = "dependent", .init = nya_callback(dependent_init), .needs = NYA_SYSTEM_FACILITY_GPU });
        nya_assert(nya_system_registry_finalize().ok);

        NYA_Error refused = nya_system_registry_run_init();

        nya_assert(!refused.ok, "a system that needs what nothing provides cannot be brought up");
        nya_assert(log_count == 0, "and its init never ran: '%s'", log_text());
        nya_assert(nya_string_contains((NYA_ConstCString)refused.message, "dependent"), "the refusal names the system: %s", (NYA_ConstCString)refused.message);
        nya_assert(nya_string_contains((NYA_ConstCString)refused.message, "GPU"), "and what it wanted: %s", (NYA_ConstCString)refused.message);

        _nya_system_registry_reset_for_test();
        log_reset();

        nya_system_register((NYA_SystemEntry){ .name     = "dependent",
                                               .init     = nya_callback(dependent_init),
                                               .needs    = NYA_SYSTEM_FACILITY_GPU,
                                               .optional = true });
        nya_system_register((NYA_SystemEntry){ .name = "after_it", .after = "dependent", .init = nya_callback(a_init) });

        nya_assert(nya_system_registry_finalize().ok);
        nya_assert(nya_system_registry_run_init().ok, "an optional system is skipped, not fatal");

        static NYA_ConstCString const expected[] = { "a_init" };
        nya_assert(log_equals(1, expected), "bring-up ran '%s'", log_text());

        nya_system_registry_run_deinit();
    }

    // TEST: a system's init may register more systems. The one it adds is brought up
    // too, even when the sort moves it ahead of the system that added it.
    {
        _nya_system_registry_reset_for_test();
        log_reset();

        nya_system_register((NYA_SystemEntry){ .name = "registrar", .init = nya_callback(registers_during_init) });
        nya_system_register((NYA_SystemEntry){ .name = "last", .after = "registrar", .init = nya_callback(b_init) });

        nya_assert(nya_system_registry_finalize().ok);
        nya_assert(nya_system_registry_run_init().ok);

        static NYA_ConstCString const expected[] = { "registrar_init", "a_init", "b_init" };
        nya_assert(log_equals(3, expected), "bring-up ran '%s'", log_text());

        nya_assert(nya_system_registry_count() == 3, "the system registered during bring-up stayed");

        nya_system_registry_run_deinit();
    }

    // TEST: running before finalize is a programmer error, not a quiet no-op.
    {
        _nya_system_registry_reset_for_test();

        nya_system_register((NYA_SystemEntry){ .name = "a", .init = nya_callback(a_init), .tick = nya_callback(a_tick), .deinit = nya_callback(a_deinit) });

        nya_expect_crash((void)nya_system_registry_run_init());
        nya_expect_crash(nya_system_registry_run(NYA_SYSTEM_PHASE_TICK, 0.016F));
        nya_expect_crash(nya_system_registry_run_deinit());
    }

    // TEST: the contract's hard edges. A duplicate name, a system unregistered while
    // something is ordered against it, and enabling something that was never there.
    {
        _nya_system_registry_reset_for_test();

        nya_system_register((NYA_SystemEntry){ .name = "a", .tick = nya_callback(a_tick) });
        nya_system_register((NYA_SystemEntry){ .name = "b", .after = "a", .tick = nya_callback(b_tick) });

        nya_expect_crash(nya_system_register((NYA_SystemEntry){ .name = "a" }));
        nya_expect_crash(nya_system_unregister("a"));
        nya_expect_crash(nya_system_enable("nobody"));
        nya_expect_crash(nya_system_disable("nobody"));

        nya_assert(!nya_system_is_enabled("nobody"), "asking about a name nobody registered is not a crash, though");

        // a plugin system with no plugin name would report its time as the engine's.
        nya_expect_crash(nya_system_register((NYA_SystemEntry){ .name = "nameless", .owner = { .kind = NYA_SYSTEM_OWNER_PLUGIN } }));
    }

    // TEST: a phase run from inside a phase is refused. Last, because the crash is
    // caught mid-run and leaves the registry mid-run with it.
    {
        _nya_system_registry_reset_for_test();

        nya_system_register((NYA_SystemEntry){ .name = "reenters", .tick = nya_callback(reenters) });
        nya_assert(nya_system_registry_finalize().ok);

        nya_expect_crash(nya_system_registry_run(NYA_SYSTEM_PHASE_TICK, 0.016F));
    }

    _nya_system_registry_reset_for_test();

    printf("All tests passed.\n");
    return 0;
}
