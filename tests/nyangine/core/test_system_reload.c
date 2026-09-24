/**
 * A system survives a code reload: the entry names its callbacks instead of pointing at them, and it
 * owns the bytes of its own name instead of pointing into the image that registered it.
 *
 * This is what stands between the registry and a system coming from a plugin. Without it, a system
 * registered from the game DLL goes on calling the generation that registered it, which is a reload
 * that silently does not take.
 **/

/*
 * The reload machinery is compiled out of a test build, and the reload machinery is what this test is
 * about: without this the callback registry is a cast and there is nothing to re-resolve.
 */
#define NYA_CODE_HOT_RELOAD 1

#include "nyangine/nyangine.c"
#include "nyangine/nyangine.h"

/** Which generation of the code last ran. What the whole test reads. */
static u32 generation_ran = 0;

static void tick_v1(f32 delta_time_s) {
    nya_unused(delta_time_s);
    generation_ran = 1;
}

static void tick_v2(f32 delta_time_s) {
    nya_unused(delta_time_s);
    generation_ran = 2;
}

static NYA_Error init_v1(void) {
    generation_ran = 1;
    return NYA_OK;
}

static NYA_Error init_v2(void) {
    generation_ran = 2;
    return NYA_OK;
}

static void deinit_v1(void) {
    generation_ran = 1;
}

static void deinit_v2(void) {
    generation_ran = 2;
}

/**
 * What main.c's update_callback_pointers does, with a table in place of dlsym.
 *
 * A reload re-resolves every registered name against the new image and writes the answer back into the
 * callback registry. Nothing else in the process is touched, which is exactly the point: whoever holds
 * a handle keeps holding it and gets the new code the next time they resolve it.
 * */
static void reload(NYA_ConstCString name, void* replacement) {
    NYA_ArrayᐸNYA_Callbackᐳ* callbacks = nya_app_get()->callback_system.callbacks;

    u32 replaced = 0;

    nya_array_foreach (callbacks, callback) {
        if (callback->name == nullptr || !nya_string_equals(callback->name, name)) continue;

        callback->fn = replacement;
        replaced++;
    }

    nya_assert(replaced == 1, "the reload stand-in replaced %u callbacks called '%s', not one", replaced, name);
}

s32 main(void) {
    _NYA_APP_INSTANCE = (NYA_App){ .initialized = true };

    nya_system_callback_init();
    defer nya_system_callback_deinit();

    // TEST: a phase callback follows the reload. The registry is never told anything changed; it resolves the handle again and gets the new image's function.
    {
        _nya_system_registry_reset_for_test();

        nya_system_register((NYA_SystemEntry){ .name = "reloadable", .tick = nya_callback(tick_v1) });
        nya_assert(nya_system_registry_finalize().ok);

        NYA_CallbackHandle before = nya_system_registry_at(0)->tick;

        generation_ran = 0;
        nya_system_registry_run(NYA_SYSTEM_PHASE_TICK, 0.016F);
        nya_assert(generation_ran == 1, "the registered generation should run first, ran %u", generation_ran);

        reload("tick_v1", (void*)tick_v2);

        generation_ran = 0;
        nya_system_registry_run(NYA_SYSTEM_PHASE_TICK, 0.016F);
        nya_assert(generation_ran == 2, "after the reload the new generation should run, ran %u", generation_ran);

        // the entry did not change: what changed is what the name resolves to, which is the difference between a handle and a pointer.
        nya_assert(nya_system_registry_at(0)->tick == before, "the entry's handle must be the same handle after a reload");
        nya_assert(nya_system_registry_runs_phase_at(0, NYA_SYSTEM_PHASE_TICK), "and it must still count as having tick work");
    }

    // TEST: the lifetime pair follows too, so a system brought up by one generation is torn down by the one that is loaded when it goes.
    {
        _nya_system_registry_reset_for_test();

        nya_system_register((NYA_SystemEntry){ .name = "lifetime", .init = nya_callback(init_v1), .deinit = nya_callback(deinit_v1) });
        nya_assert(nya_system_registry_finalize().ok);

        reload("init_v1", (void*)init_v2);
        reload("deinit_v1", (void*)deinit_v2);

        generation_ran = 0;
        nya_assert(nya_system_registry_run_init().ok);
        nya_assert(generation_ran == 2, "init should come from the loaded generation, ran %u", generation_ran);

        generation_ran = 0;
        nya_system_registry_run_deinit();
        nya_assert(generation_ran == 2, "and so should deinit, ran %u", generation_ran);
    }

    // TEST: the registry keeps its own copy of every name it is given, so a name that lived in the caller's storage is still readable once that storage is gone. In a reloading build that storage is a string literal in the image being replaced.
    {
        _nya_system_registry_reset_for_test();

        char name[NYA_SYSTEM_NAME_MAX];
        char after[NYA_SYSTEM_NAME_MAX];

        (void)snprintf(name, sizeof(name), "%s", "borrowed");
        (void)snprintf(after, sizeof(after), "%s", "anchor");

        nya_system_register((NYA_SystemEntry){ .name = "anchor", .tick = nya_callback(tick_v1) });
        nya_system_register((NYA_SystemEntry){ .name = name, .after = after, .tick = nya_callback(tick_v2) });

        // the caller's bytes, overwritten with something else entirely. An entry still pointing at them would now be a system called "gone".
        (void)snprintf(name, sizeof(name), "%s", "gone");
        (void)snprintf(after, sizeof(after), "%s", "gone");

        nya_assert(nya_system_registry_finalize().ok, "the copied `after` must still resolve");

        nya_assert(nya_string_equals(nya_system_registry_at(0)->name, "anchor"), "got '%s'", nya_system_registry_at(0)->name);
        nya_assert(nya_string_equals(nya_system_registry_at(1)->name, "borrowed"), "got '%s'", nya_system_registry_at(1)->name);
        nya_assert(nya_string_equals(nya_system_registry_at(1)->after, "anchor"), "got '%s'", nya_system_registry_at(1)->after);

        // and every operation that takes a name still reaches it.
        nya_system_disable("borrowed");
        nya_assert(!nya_system_is_enabled("borrowed"));
        nya_system_enable("borrowed");

        generation_ran = 0;
        nya_system_registry_run(NYA_SYSTEM_PHASE_TICK, 0.016F);
        nya_assert(generation_ran == 2, "'borrowed' runs after 'anchor', so it wrote last, ran %u", generation_ran);
    }

    // TEST: the copies survive the two things that move rows: the sort's permutation and the shift an unregister leaves behind. Both used to leave an entry naming whichever row now sits where it used to.
    {
        _nya_system_registry_reset_for_test();

        // registered backwards, so finalize has to permute all three.
        nya_system_register((NYA_SystemEntry){ .name = "third", .after = "second", .tick = nya_callback(tick_v1) });
        nya_system_register((NYA_SystemEntry){ .name = "first", .tick = nya_callback(tick_v1) });
        nya_system_register((NYA_SystemEntry){ .name = "second", .after = "first", .tick = nya_callback(tick_v1) });

        nya_assert(nya_system_registry_finalize().ok);

        nya_assert(nya_string_equals(nya_system_registry_at(0)->name, "first"), "got '%s'", nya_system_registry_at(0)->name);
        nya_assert(nya_string_equals(nya_system_registry_at(1)->name, "second"), "got '%s'", nya_system_registry_at(1)->name);
        nya_assert(nya_string_equals(nya_system_registry_at(2)->name, "third"), "got '%s'", nya_system_registry_at(2)->name);
        nya_assert(nya_string_equals(nya_system_registry_at(1)->after, "first"), "got '%s'", nya_system_registry_at(1)->after);
        nya_assert(nya_string_equals(nya_system_registry_at(2)->after, "second"), "got '%s'", nya_system_registry_at(2)->after);

        // "third" names "second", so "second" cannot go; "third" can, and taking it shifts nothing. Taking "first" out from under the other two is what the shift has to survive.
        nya_system_unregister("third");
        nya_system_unregister("second");
        nya_system_unregister("first");

        nya_system_register((NYA_SystemEntry){ .name = "a", .tick = nya_callback(tick_v1) });
        nya_system_register((NYA_SystemEntry){ .name = "b", .tick = nya_callback(tick_v1) });
        nya_system_register((NYA_SystemEntry){ .name = "c", .tick = nya_callback(tick_v1) });

        nya_system_unregister("a");

        nya_assert(nya_system_registry_count() == 2);
        nya_assert(nya_string_equals(nya_system_registry_at(0)->name, "b"), "got '%s'", nya_system_registry_at(0)->name);
        nya_assert(nya_string_equals(nya_system_registry_at(1)->name, "c"), "got '%s'", nya_system_registry_at(1)->name);

        nya_assert(nya_system_is_enabled("b") && nya_system_is_enabled("c"), "and both are still reachable by name");
    }

    // TEST: a name the registry cannot hold is a registration site's mistake, and is refused loudly rather than truncated into a system nothing can name.
    {
        _nya_system_registry_reset_for_test();

        char too_long[NYA_SYSTEM_NAME_MAX + 8];
        nya_memset(too_long, 'x', sizeof(too_long) - 1);
        too_long[sizeof(too_long) - 1] = '\0';

        nya_expect_crash(nya_system_register((NYA_SystemEntry){ .name = (NYA_ConstCString)too_long }));
    }

    _nya_system_registry_reset_for_test();

    printf("All tests passed.\n");
    return 0;
}
