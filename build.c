/**
 * @file build.c
 *
 * The build system's entry point, and nothing else.
 *
 * What can be built lives in src/build: the per host project rules under on_linux/ and on_windows/,
 * the asset pipeline in asset/, the tool's own rebuild in rebuild.h, everything else in misc.h, and
 * the command line that dispatches them in cli.h and cli.c. This file is what runs them in the
 * right order — parse, rebuild, bring up the vendors, dispatch — and it is deliberately the only
 * place where that order is written down.
 *
 * Bootstrap it with the command in the README. It recompiles itself from then on.
 * */
#include "nyangine/nyangine.h"
#include "build/build.h"

#include "nyangine/nyangine.c"
#include "build/build.c"

s32 main(s32 argc, NYA_CString argv[]) {
    // The build tool itself is compiled without libbacktrace, since it is what builds it. This
    // still wires up the fault handlers and the crash sink, just with no symbolization.
    nya_backtrace_init();

    parser.executable_name = argv[0];

    NYA_ArgCommand* command;
    NYA_Error       parse_result = nya_args_parse(&parser, argc, argv, &command);
    if (!parse_result.ok) {
        (void)fprintf(stderr, "Error: %s\n\n", parse_result.message);
        nya_args_print_usage(&parser, nullptr);
        return EXIT_FAILURE;
    }

    // Ahead of the self rebuild and the vendor build, both of which run compilers whose output goes
    // to stdout, which is where the completion script is going. Also means generating completions on
    // a fresh checkout costs nothing.
    if (command == &completions && !help_flag.value.as_b8) {
        NYA_EXPECT(nya_args_run_command(command), "while generating completions");
        return EXIT_SUCCESS;
    }

    // Give the rebuilt tool symbolized stack traces, once there is a libbacktrace to give it.
    //
    // Chicken and egg: this tool is what *builds* libbacktrace, so on a fresh checkout the archive
    // does not exist yet and the flags below would fail the link. base_backtrace.h keys off
    // __has_include("backtrace.h") and degrades to a null backend, so the first build has no traces
    // and every rebuild after the vendors exist has them. That is why crashes in the build system
    // print "no stack trace available" exactly once per clean checkout.
    if (nya_filesystem_exists(BACKTRACE_A_HOST)) {
        u32 count = 0;
        while (count < NYA_COMMAND_MAX_ARGUMENTS && build_rebuild_command.arguments[count] != nullptr) count++;

        NYA_ConstCString extra[]   = { BACKTRACE_INCLUDES_HOST, BACKTRACE_A_HOST };
        u32              extra_len = (u32)(sizeof(extra) / sizeof(extra[0]));

        nya_assert(count + extra_len < NYA_COMMAND_MAX_ARGUMENTS, "No room to add libbacktrace to the rebuild command.");
        for (u32 i = 0; i < extra_len; i++) build_rebuild_command.arguments[count + i] = extra[i];
    }

    if (!skip_self_rebuild_flag.value.as_b8) nya_rebuild_yourself(&argc, argv, build_rebuild_command);

    if (help_flag.value.as_b8) {
        nya_args_print_usage(&parser, command);
        return EXIT_SUCCESS;
    }

    // Every vendor, before anything else runs. From here on no rule has to care whether what it
    // links against exists yet. Vendor parts are NYA_BUILD_ONCE, so this is nearly free once the
    // artifacts are on disk.
    nya_vendor_detect_nprocs();
    NYA_EXPECT(nya_vendor_build_all(NYA_VENDORS), "while building vendor dependencies");

    NYA_Error run_result = nya_args_run_command(command);
    if (!run_result.ok) {
        (void)fprintf(stderr, "Error: %s\n\n", run_result.message);
        nya_args_print_usage(&parser, command);
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}
