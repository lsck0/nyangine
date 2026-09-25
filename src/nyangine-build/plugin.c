#include "nyangine-build/build.h"

/* `./build plugin keygen` and `./build plugin sign`, which are code that compiles and runs another program. Signing needs the engine's Ed25519, and this build tool is compiled without the crypto module (see nyangine.c: crypto is on the project's include line, not the tool's). So the signing itself lives in tools/plugin_signer.c — a full-engine program — and these handlers build it the way an example is built and run it with the arguments the parser already checked. */

/* CONSTANTS */

/** The signing program's source, and the binary it compiles to at the repository root. */
#define PLUGIN_SIGNER_SOURCE "./tools/plugin_signer.c"
#define PLUGIN_SIGNER_BINARY "nya_plugin_signer" HOST_EXECUTABLE_SUFFIX

/* PRIVATE API DECLARATION */

/** The rule that compiles tools/plugin_signer.c into PLUGIN_SIGNER_BINARY, exactly as an example is built. */
NYA_INTERNAL NYA_BuildRule _plugin_signer_build_rule(void);

/** Runs the built signer with `arguments` (nullptr terminated), and ends the process on a non-zero exit. */
NYA_INTERNAL void _plugin_signer_run(NYA_Arena* arena, const NYA_ConstCString* arguments);

/* PUBLIC API IMPLEMENTATION */

void plugin_keygen_runner(NYA_ArgCommand* command) {
    nya_assert(command != nullptr);

    NYA_ArgParameter* seed_flag = command->parameters[0];
    nya_assert(seed_flag != nullptr && nya_string_equals(seed_flag->name, "seed"));

    NYA_ConstCString seed_path = seed_flag->was_matched ? seed_flag->value.as_string : seed_flag->default_value.as_string;

    NYA_Arena* arena = nya_arena_create(.name = "plugin_keygen");
    defer      nya_arena_destroy(arena);

    _plugin_signer_run(arena, (const NYA_ConstCString[]){ "keygen", "--seed", seed_path, nullptr });
}

void plugin_sign_runner(NYA_ArgCommand* command) {
    nya_assert(command != nullptr);

    NYA_ArgParameter* directory_arg  = command->parameters[0];
    NYA_ArgParameter* seed_flag      = command->parameters[1];
    NYA_ArgParameter* publisher_flag = command->parameters[2];
    nya_assert(directory_arg != nullptr && nya_string_equals(directory_arg->name, "directory"));
    nya_assert(seed_flag != nullptr && nya_string_equals(seed_flag->name, "seed"));
    nya_assert(publisher_flag != nullptr && nya_string_equals(publisher_flag->name, "publisher"));

    if (!directory_arg->was_matched) {
        (void)fprintf(stderr, "Error: no plugin directory named. Usage: ./build plugin sign <directory> --seed <file>\n");
        exit(EXIT_FAILURE);
    }

    NYA_ConstCString directory = directory_arg->value.as_string;
    NYA_ConstCString seed_path = seed_flag->was_matched ? seed_flag->value.as_string : seed_flag->default_value.as_string;

    NYA_Arena* arena = nya_arena_create(.name = "plugin_sign");
    defer      nya_arena_destroy(arena);

    if (publisher_flag->was_matched) {
        _plugin_signer_run(arena, (const NYA_ConstCString[]){ "sign", directory, "--seed", seed_path, "--publisher",
                                                              publisher_flag->value.as_string, nullptr });
    } else {
        _plugin_signer_run(arena, (const NYA_ConstCString[]){ "sign", directory, "--seed", seed_path, nullptr });
    }
}

/* PRIVATE API IMPLEMENTATION */

NYA_BuildRule _plugin_signer_build_rule(void) {
    // The same flags, plugins and vendors the project links, by naming the same macros an example does. A hand copied list here is the drift that makes a tool fail to compile on a header the game has.
    return (NYA_BuildRule){
        .name        = "build_plugin_signer",
        .policy      = NYA_BUILD_ALWAYS,
        .output_file = PLUGIN_SIGNER_BINARY,

        .command = {
            .program   = CC,
            .arguments = {
                PLUGIN_SIGNER_SOURCE,
                "-o", PLUGIN_SIGNER_BINARY,
                CFLAGS,
                WARNINGS,
                INCLUDE_PATHS,
                FLAGS_PLUGINS,
                LINKER_FLAGS,
                FLAGS_DEBUG,
                FLAGS_HOST_NATIVE,
            },
        },

        .pre_build_hooks = { &hook_add_version_flag, },
#if OS_WINDOWS
        .vendors         = { NYA_PROJECT_VENDORS_WINDOWS_X86_64, },
#else
        .vendors         = { NYA_PROJECT_VENDORS_LINUX_X86_64, },
#endif
        // The engine's generated headers have to exist to compile it; the same two an example depends on.
        .dependencies    = { &build_shaders, &index_assets, },
    };
}

void _plugin_signer_run(NYA_Arena* arena, const NYA_ConstCString* arguments) {
    nya_assert(arena != nullptr && arguments != nullptr);

    NYA_BuildRule build = _plugin_signer_build_rule();

    NYA_BuildRule run = {
        .name        = "run_plugin_signer",
        .policy      = NYA_BUILD_ALWAYS,
        .output_file = PLUGIN_SIGNER_BINARY,

        .command = {
            // Not bare: a program with no separator is looked up on PATH, and this one is in the working directory.
            .program     = "./" PLUGIN_SIGNER_BINARY,
            .environment = { SANITIZER_ENVIRONMENT, },
        },

        .dependencies = { &build, },
    };

    u32 count = 0;
    while (arguments[count] != nullptr) {
        nya_assert(count < NYA_COMMAND_MAX_ARGUMENTS, "the signer was given more arguments than a command can hold");
        run.command.arguments[count] = arguments[count];
        count++;
    }

    NYA_EXPECT(nya_build(&run), "while running the plugin signer");
}
