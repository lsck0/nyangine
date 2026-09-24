#include "build/build.h"

/* PRIVATE API DECLARATION */

/** A seed nobody chose, for a run nobody is replaying. The same one simulation.c draws, and why. */
NYA_INTERNAL u64 _agent_seed_fresh(void) __attr_no_discard;

/* PUBLIC API IMPLEMENTATION */

void agent_runner(NYA_ArgCommand* command) {
    nya_assert(command != nullptr);

    NYA_ArgParameter* kind_parameter     = command->parameters[0];
    NYA_ArgParameter* seed_parameter     = command->parameters[1];
    NYA_ArgParameter* episodes_parameter = command->parameters[2];
    NYA_ArgParameter* ticks_parameter    = command->parameters[3];
    NYA_ArgParameter* verbose_flag       = command->parameters[4];

    nya_assert(kind_parameter != nullptr && seed_parameter != nullptr && episodes_parameter != nullptr);
    nya_assert(ticks_parameter != nullptr && verbose_flag != nullptr);

    NYA_ConstCString kind = kind_parameter->value.as_string;

    // the parser carries a number as s64; a seed is a bit pattern, so the cast is exact either way.
    u64 seed     = seed_parameter->value.as_s64 != 0 ? (u64)seed_parameter->value.as_s64 : _agent_seed_fresh();
    u64 episodes = episodes_parameter->value.as_s64 > 0 ? (u64)episodes_parameter->value.as_s64 : 1;
    u64 ticks    = ticks_parameter->value.as_s64 > 0 ? (u64)ticks_parameter->value.as_s64 : 1;

    NYA_CString seed_text     = nya_string_to_cstring(nya_arena_global, nya_string_sprintf(nya_arena_global, "0x%016llX", (unsigned long long)seed));
    NYA_CString episodes_text = nya_string_to_cstring(nya_arena_global, nya_string_sprintf(nya_arena_global, "%llu", (unsigned long long)episodes));
    NYA_CString ticks_text    = nya_string_to_cstring(nya_arena_global, nya_string_sprintf(nya_arena_global, "%llu", (unsigned long long)ticks));

    NYA_CString object = OBJECT_DIRECTORY "/agent" OBJECT_SUFFIX;

    NYA_BuildRule compile_rule = {
        .name        = "compile_agent",
        .policy      = NYA_BUILD_ALWAYS,
        .output_file = object,

        // the codegen a test gets, for the same reason: the runner is a unity build of the engine and the game, and reads the generated strings, assets and reflection tables.
        .dependencies = { &build_shaders, &index_assets, },

        .command = {
            .program   = CC,
            .arguments = {
                AGENT_SOURCE,
                "-c", "-o", object,
                CFLAGS,
                WARNINGS,
                INCLUDE_PATHS,
                FLAGS_PLUGINS,
                // exactly what a test is built with, because it is one: the assertions are the oracle, so an agent playing a differently asserted program is testing a different program.
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
        .name        = "link_agent",
        .policy      = NYA_BUILD_ALWAYS,
        .input_file  = object,
        .output_file = AGENT_BINARY,

        .command = {
            .program   = CC,
            .arguments = {
                object,
                "-o", AGENT_BINARY,
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
        .name        = "run_agent",
        .policy      = NYA_BUILD_ALWAYS,
        .output_file = AGENT_BINARY,

        .command = {
            .program     = AGENT_BINARY,
            .arguments   = { "--kind", kind, "--seed", seed_text, "--episodes", episodes_text, "--ticks", ticks_text },
            .environment = { SANITIZER_ENVIRONMENT, },
        },

        // kept, like the simulation's: a failing seed is replayed against this exact build, and rebuilding it first would be a different program if anything changed in between.
    };

    if (verbose_flag->value.as_b8) {
        u32 count = 0;
        while (count < NYA_COMMAND_MAX_ARGUMENTS && run_rule.command.arguments[count] != nullptr) count++;

        nya_assert(count + 1 < NYA_COMMAND_MAX_ARGUMENTS, "no room for --verbose on the agent command");
        run_rule.command.arguments[count] = "--verbose";
    }

    NYA_EXPECT(nya_build(&compile_rule), "while compiling the agent runner");
    NYA_EXPECT(nya_build(&link_rule), "while linking the agent runner");

    nya_log_info("Playing gnyame with the %s agent, seed %s, %s episodes of %s ticks. Replay it with: ./build run agent --kind %s --seed %s "
                 "--episodes %s --ticks %s",
                 kind, seed_text, episodes_text, ticks_text, kind, seed_text, episodes_text, ticks_text);

    NYA_EXPECT(nya_build(&run_rule), "the agent found something; the seed above replays it");
}

/* PRIVATE API IMPLEMENTATION */

u64 _agent_seed_fresh(void) {
    // hashed rather than used raw, so two runs started in the same millisecond do not get seeds that differ only in their low bits and explore the same corner.
    u64 now = nya_clock_get_monotonic_ns();

    return nya_hash_wyhash(&now, sizeof(now));
}

NYA_ConstCString agent_completion_kind(u32 index) {
    // the same three names testing_agent.h answers to, restated here because the build tool does not link the engine's testing facilities.
    NYA_ConstCString kinds[] = { "random", "dqn", "neat" };

    if (index >= nya_carray_length(kinds)) return nullptr;

    return kinds[index];
}
