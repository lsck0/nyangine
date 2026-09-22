#include "build/build.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API DECLARATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/**
 * A seed nobody chose, for a run nobody is replaying.
 *
 * Off the wall clock, which is the one place in this whole facility where non-determinism belongs: the
 * seed is the input, and a scheduled run wants a different one every time. It is printed by the run
 * itself, so a failure is still replayable.
 * */
NYA_INTERNAL u64 _simulation_seed_fresh(void) __attr_no_discard;

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PUBLIC API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

void simulation_runner(NYA_ArgCommand* command) {
    nya_assert(command != nullptr);

    NYA_ArgParameter* seed_parameter  = command->parameters[0];
    NYA_ArgParameter* steps_parameter = command->parameters[1];
    NYA_ArgParameter* verbose_flag    = command->parameters[2];

    nya_assert(seed_parameter != nullptr && steps_parameter != nullptr && verbose_flag != nullptr);

    // the parser carries a number as s64; a seed is a bit pattern, so the cast is exact either way.
    u64 seed  = seed_parameter->value.as_s64 != 0 ? (u64)seed_parameter->value.as_s64 : _simulation_seed_fresh();
    u64 steps = steps_parameter->value.as_s64 > 0 ? (u64)steps_parameter->value.as_s64 : 1;

    NYA_CString seed_text  = nya_string_to_cstring(nya_arena_global, nya_string_sprintf(nya_arena_global, "0x%016llX", (unsigned long long)seed));
    NYA_CString steps_text = nya_string_to_cstring(nya_arena_global, nya_string_sprintf(nya_arena_global, "%llu", (unsigned long long)steps));

    NYA_CString object = OBJECT_DIRECTORY "/simulation" OBJECT_SUFFIX;

    NYA_BuildRule compile_rule = {
        .name        = "compile_simulation",
        .policy      = NYA_BUILD_ALWAYS,
        .output_file = object,

        // the codegen a test gets, for the same reason: the runner is a unity build of the engine and
        // the game, and reads the generated strings, assets and reflection tables.
        .dependencies = { &build_shaders, &index_assets, },

        .command = {
            .program   = CC,
            .arguments = { CC_LAUNCHED,
                SIMULATION_SOURCE,
                "-c", "-o", object,
                CFLAGS,
                WARNINGS,
                INCLUDE_PATHS,
                FLAGS_PLUGINS,
                // exactly what a test is built with, because it is one: the assertions are the oracle,
                // so a simulation built with different ones is checking a different program.
                FLAGS_TEST,
                FLAGS_HOST_NATIVE_COMPILE
            },
        },

        .pre_build_hooks = { &hook_add_version_flag, &hook_create_output_directory, &hook_use_compiler_cache, },
#if OS_WINDOWS
        .vendors         = { NYA_PROJECT_VENDORS_WINDOWS_X86_64, },
#else
        .vendors         = { NYA_PROJECT_VENDORS_LINUX_X86_64, },
#endif
        .vendor_flags    = NYA_BUILD_VENDOR_FLAGS_COMPILE,
    };

    NYA_BuildRule link_rule = {
        .name        = "link_simulation",
        .policy      = NYA_BUILD_ALWAYS,
        .input_file  = object,
        .output_file = SIMULATION_BINARY,

        .command = {
            .program   = CC,
            .arguments = { CC_LAUNCHED,
                object,
                "-o", SIMULATION_BINARY,
                CFLAGS,
                FLAGS_TEST,
                FLAGS_HOST_NATIVE_LINK,
                LINKER_FLAGS,
#if !OS_WINDOWS
                "-Wl,-rpath,$ORIGIN/../../vendor/steam/redistributable_bin/linux64",
#endif
            },
        },

#if OS_WINDOWS
        .vendors      = { NYA_PROJECT_VENDORS_WINDOWS_X86_64, },
#else
        .vendors      = { NYA_PROJECT_VENDORS_LINUX_X86_64, },
#endif
        .vendor_flags = NYA_BUILD_VENDOR_FLAGS_LINK,
    };

    NYA_BuildRule run_rule = {
        .name        = "run_simulation",
        .policy      = NYA_BUILD_ALWAYS,
        .output_file = SIMULATION_BINARY,

        .command = {
            .program     = SIMULATION_BINARY,
            .arguments   = { "--seed", seed_text, "--steps", steps_text },
            .environment = { SANITIZER_ENVIRONMENT, },
        },

        // kept, unlike a test binary: a failing seed is replayed against this exact build, and
        // rebuilding it first would be a different program if anything changed in between.
    };

    if (verbose_flag->value.as_b8) {
        u32 count = 0;
        while (count < NYA_COMMAND_MAX_ARGUMENTS && run_rule.command.arguments[count] != nullptr) count++;

        nya_assert(count + 1 < NYA_COMMAND_MAX_ARGUMENTS, "no room for --verbose on the simulation command");
        run_rule.command.arguments[count] = "--verbose";
    }

    NYA_EXPECT(nya_build(&compile_rule), "while compiling the simulation");
    NYA_EXPECT(nya_build(&link_rule), "while linking the simulation");

    nya_log_info("Simulating seed %s for %s steps. Replay it with: ./build run simulation --seed %s --steps %s", seed_text, steps_text, seed_text,
                 steps_text);

    NYA_EXPECT(nya_build(&run_rule), "the simulation found something; the seed above replays it");
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

u64 _simulation_seed_fresh(void) {
    // hashed rather than used raw, so two runs started in the same millisecond do not get seeds that
    // differ only in their low bits and explore the same corner.
    u64 now = nya_clock_get_monotonic_ns();

    return nya_hash_wyhash(&now, sizeof(now));
}
