#include "build/build.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API DECLARATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/** One translation unit to check, and the flags it is really compiled with. */
typedef struct {
    NYA_ConstCString source;

    /** Everything after `--`, terminated by a nullptr. The vendor includes are appended to it. */
    NYA_ConstCString flags[64];

    /**
     * Whether this unit compiles against the vendored libraries.
     *
     * False for the build system, which is compiled with FLAGS_BUILD_TOOL and reaches none of them —
     * and must not be handed their include paths, because on a fresh checkout they do not exist yet.
     * */
    b8 uses_vendors;
} CheckUnit;

/**
 * Appends every vendor's compile flags and include paths to `arguments`, from `at` onward.
 *
 * Includes and cflags only, deliberately: NYA_BuildRule's own vendor splice would add the linker
 * flags too, and everything after `--` is a *compile* command line. An archive path there is an
 * input file rather than a library, and clang-tidy is entitled to refuse a command line with
 * fifteen of them.
 *
 * Returns the index one past the last thing written.
 * */
NYA_INTERNAL u32 _check_append_vendor_flags(NYA_ConstCString* arguments, u32 at);

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PUBLIC API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

void check_runner(NYA_ArgCommand* command) {
    nya_assert(command != nullptr);

    NYA_ArgParameter* filters = command->parameters[0];
    nya_assert(filters != nullptr);
    nya_assert(nya_string_equals(filters->name, "sources"));

    NYA_ArgParameter* strict = command->parameters[1];
    nya_assert(strict != nullptr);
    nya_assert(nya_string_equals(strict->name, "strict"));

    /*
     * The three roots, each with the flag set its own build rule uses.
     *
     * main.c is checked in the *debug* configuration rather than the release one, because that is
     * where NYA_EXECUTION_MODE=0 puts the hot reload host — the dlopen half of main.c is dead text
     * under release flags, and analysing a file with two thirds of it preprocessed away is how a bug
     * in it survives every check anyone runs.
     */
    CheckUnit units[] = {
        {
         .source       = BINARY_SOURCE_PATH,
         .flags        = { CFLAGS, WARNINGS, INCLUDE_PATHS, FLAGS_PLUGINS, FLAGS_DEBUG },
         .uses_vendors = true,
         },
        {
         .source       = DLL_SOURCE_PATH,
         .flags        = { CFLAGS, WARNINGS, INCLUDE_PATHS, FLAGS_PLUGINS, FLAGS_DEVELOPER },
         .uses_vendors = true,
         },
        {
         .source       = "./build.c",
         .flags        = { CFLAGS, WARNINGS, INCLUDE_PATHS, FLAGS_BUILD_TOOL },
         .uses_vendors = false,
         },
    };

    NYA_ArrayᐸNYA_BuildRulePointerᐳ* rules = nya_array_create(nya_arena_global, NYA_BuildRulePointer);

    for (u32 i = 0; i < nya_carray_length(units); i++) {
        CheckUnit* unit = &units[i];

        b8 should_check = filters->values_count == 0;
        for (u32 filter_index = 0; filter_index < filters->values_count; filter_index++) {
            if (nya_string_contains(unit->source, filters->values[filter_index].as_string)) {
                should_check = true;
                break;
            }
        }
        if (!should_check) continue;

        NYA_BuildRule* rule = nya_arena_alloc(nya_arena_global, sizeof(NYA_BuildRule));

        *rule = (NYA_BuildRule){
            .name   = nya_string_to_cstring(nya_arena_global, nya_string_sprintf(nya_arena_global, "check:%s", unit->source)),
            .policy = NYA_BUILD_ALWAYS,

            .command = {
                .program   = "clang-tidy",
                .arguments = {
                    unit->source,

                    // Suppresses the "N warnings generated" trailer clang prints per translation
                    // unit. The findings themselves are unaffected.
                    "--quiet",
                },
            },
        };

        u32 at = 0;
        while (rule->command.arguments[at] != nullptr) at++;

        /*
         * Findings are warnings by default, and clang-tidy exits zero on warnings.
         *
         * So the plain command reports and succeeds. That is the right default for a tree that has
         * a backlog: a command which cannot be run without failing is a command nobody runs, and the
         * findings it would have surfaced go unread. --strict is the gate, for CI and for anyone who
         * has just cleared a file and wants it to stay clear.
         */
        if (strict->value.as_b8) rule->command.arguments[at++] = "--warnings-as-errors=*";

        // Everything past here is the compile command line, not clang-tidy's own.
        rule->command.arguments[at++] = "--";

        for (u32 flag = 0; flag < nya_carray_length(unit->flags) && unit->flags[flag] != nullptr; flag++) {
            rule->command.arguments[at++] = unit->flags[flag];
        }

        if (unit->uses_vendors) at = _check_append_vendor_flags(rule->command.arguments, at);

        nya_assert(at < NYA_COMMAND_MAX_ARGUMENTS, "check command line for %s does not fit in NYA_COMMAND_MAX_ARGUMENTS", unit->source);

        nya_array_push_back(rules, rule);
    }

    if (rules->length == 0) {
        nya_log_warn("Nothing to check: no translation unit matched.");
        return;
    }

    /*
     * Parallel for the same reason the tests are: each of these is a whole-program analysis of a
     * unity build, which takes appreciably longer than compiling the same file does. Three units is
     * not many, but they are the three slowest commands in this build system.
     */
    NYA_EXPECT(nya_build_parallel(rules->items, (u32)rules->length, 0), "while running clang-tidy");

    nya_log_info("Checked " FMTu64 " translation units.", rules->length);
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

u32 _check_append_vendor_flags(NYA_ConstCString* arguments, u32 at) {
    // The host's own target, because that is the one whose vendor archives and generated headers
    // actually exist on this machine. Cross compiled headers would analyse fine and then send anyone
    // running this on Linux looking for a mingw sysroot.
    NYA_VendorRule* vendors[] = {
#if OS_WINDOWS
        NYA_PROJECT_VENDORS_WINDOWS_X86_64,
#else
        NYA_PROJECT_VENDORS_LINUX_X86_64,
#endif
        nullptr,
    };

    for (u32 i = 0; vendors[i] != nullptr; i++) {
        for (u32 flag = 0; flag < NYA_VENDOR_MAX_FLAGS && vendors[i]->cflags[flag] != nullptr; flag++) {
            arguments[at++] = vendors[i]->cflags[flag];
        }

        for (u32 include = 0; include < NYA_VENDOR_MAX_FLAGS && vendors[i]->includes[include] != nullptr; include++) {
            arguments[at++] = vendors[i]->includes[include];
        }
    }

    return at;
}
