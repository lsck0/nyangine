/**
 * @file vendor_sqlean.h
 *
 * sqlean, a set of SQLite extensions. Built into a static archive, one object.
 *
 * sqlean's own Makefile only produces loadable shared extensions (.so / .dll), which is the opposite
 * of how the rest of this tree is linked, so none of it is used. The supported way to use these
 * statically is the way SQLite intends: compile the extension sources into the program and register
 * them with sqlite3_auto_extension rather than loading them at runtime.
 *
 * What gets compiled is src/nyangine/plugins/sqlite/sqlean_extensions.c, a single file in *this*
 * repository that includes the vendored sources it wants and exposes one entry point,
 * `nya_sqlean_init`. Read that file for which extensions are in, which are deliberately out, and
 * why. It is compiled here rather than as part of the engine's translation unit so that third party
 * code is not held to the engine's warning settings, and so that the engine's unity build stays
 * engine code.
 *
 * The archive is always built and always linked, even when NYA_PLUGIN_SQLITE is off. That costs
 * nothing: an archive member is only pulled into the link if something references it, and with the
 * plugin off nothing references `nya_sqlean_init`.
 *
 * Link order matters: this archive calls into libsqlite3.a, so it has to appear before it. See the
 * vendor list in vendor.h.
 * */
#pragma once

#include "nyangine/nyangine.h"
#include "build/hooks.h"
#include "build/toolchain.h"
#include "build/vendor/vendor_common.h"
// For SQLITE_BUILD_*: the extensions compile against sqlite3ext.h, which only exists once sqlite has
// been configured, so the path to it belongs to that vendor rather than being spelled out again.
#include "build/vendor/vendor_sqlite.h"

#define SQLEAN_SOURCE "./vendor/sqlean/src"

/** The one file compiled here. In this repository, not in the submodule; see the note above. */
#define SQLEAN_GLUE_SOURCE "./src/nyangine/plugins/sqlite/sqlean_extensions.c"

#define SQLEAN_BUILD_LINUX_X86_64   "./vendor/sqlean/build-linux-x86_64/"
#define SQLEAN_BUILD_WINDOWS_X86_64 "./vendor/sqlean/build-windows-x86_64/"

#define SQLEAN_O_LINUX_X86_64   SQLEAN_BUILD_LINUX_X86_64 "sqlean_extensions.o"
#define SQLEAN_O_WINDOWS_X86_64 SQLEAN_BUILD_WINDOWS_X86_64 "sqlean_extensions.o"

#define SQLEAN_A_LINUX_X86_64   SQLEAN_BUILD_LINUX_X86_64 "libsqlean.a"
#define SQLEAN_A_WINDOWS_X86_64 SQLEAN_BUILD_WINDOWS_X86_64 "libsqlean.a"

// clang-format off

/**
 * What both targets compile the glue with, target flags aside.
 *
 * SQLITE_CORE is the whole point: it tells sqlite3ext.h that this is compiled into the program, so
 * SQLITE_EXTENSION_INIT1 expands to nothing and every sqlite3_* call is a direct one. Without it the
 * extensions would call through a dispatch pointer that only a runtime loader ever sets.
 *
 * -std=c11 rather than the engine's c2y: this is third party code written against an older standard,
 * and there is no reason for it to be dragged forward. Same for the warning level — -w, because
 * these warnings are not actionable here. They belong upstream, and printing them on every clean
 * build only trains everyone to scroll past the build output.
 * */
#define SQLEAN_CFLAGS               \
    "-c", "-O2", "-std=c11", "-w",  \
    "-DSQLITE_CORE",                \
    "-I" SQLEAN_SOURCE

// clang-format on

NYA_VendorRule vendor_sqlean_linux_x86_64 = {
    .name = "sqlean (linux-x86_64)",

    .includes     = { "-I" SQLEAN_SOURCE, },
    .linker_flags = { SQLEAN_A_LINUX_X86_64, },

    .parts = {
        &(NYA_BuildRule){
            .name = "vendor_sqlean_linux_x86_64_directory",
            // ONCE keyed on the directory itself, the way the lz4 and lua metarules are. Without a
            // policy this falls to NYA_BUILD_ALWAYS, which is the enum's zero, and a metarule short
            // circuits before any policy is consulted — so it printed a [BUILDING META] line and
            // re-ran the mkdir on every single ./build, including ones that never touch SQL.
            .policy      = NYA_BUILD_ONCE,
            .is_metarule = true,
            .output_file = SQLEAN_BUILD_LINUX_X86_64,

            .command         = { .working_directory = SQLEAN_BUILD_LINUX_X86_64, },
            .pre_build_hooks = { &hook_create_build_directory, },
        },
        &(NYA_BuildRule){
            .name = "vendor_sqlean_linux_x86_64_compile",
            // IF_OUTDATED rather than ONCE, unlike most vendor parts: the input is a file in this
            // repository that gets edited, so keying on "does the object exist" would mean adding an
            // extension and watching nothing happen.
            .policy      = NYA_BUILD_IF_OUTDATED,
            .input_file  = SQLEAN_GLUE_SOURCE,
            .output_file = SQLEAN_O_LINUX_X86_64,

            .command = {
                .program   = CC,
                .arguments = {
                    SQLEAN_CFLAGS,
                    "-fPIC",
                    "-I" SQLITE_BUILD_LINUX_X86_64,
                    SQLEAN_GLUE_SOURCE,
                    "-o", SQLEAN_O_LINUX_X86_64,
                },
            },
        },
        &(NYA_BuildRule){
            .name        = "vendor_sqlean_linux_x86_64_archive",
            .policy      = NYA_BUILD_IF_OUTDATED,
            .input_file  = SQLEAN_O_LINUX_X86_64,
            .output_file = SQLEAN_A_LINUX_X86_64,

            .command = {
                .program   = "ar",
                .arguments = { "rcs", SQLEAN_A_LINUX_X86_64, SQLEAN_O_LINUX_X86_64, },
            },
        },
    },
};

NYA_VendorRule vendor_sqlean_windows_x86_64 = {
    .name = "sqlean (windows-x86_64)",

    .includes     = { "-I" SQLEAN_SOURCE, },
    .linker_flags = { SQLEAN_A_WINDOWS_X86_64, },

    .parts = {
        &(NYA_BuildRule){
            .name        = "vendor_sqlean_windows_x86_64_directory",
            .policy      = NYA_BUILD_ONCE,
            .is_metarule = true,
            .output_file = SQLEAN_BUILD_WINDOWS_X86_64,

            .command         = { .working_directory = SQLEAN_BUILD_WINDOWS_X86_64, },
            .pre_build_hooks = { &hook_create_build_directory, },
        },
        &(NYA_BuildRule){
            .name        = "vendor_sqlean_windows_x86_64_compile",
            .policy      = NYA_BUILD_IF_OUTDATED,
            .input_file  = SQLEAN_GLUE_SOURCE,
            .output_file = SQLEAN_O_WINDOWS_X86_64,

            // No -fPIC: position independent code is the default and meaningless for a PE, and
            // mingw-w64's gcc warns that the flag is ignored.
            .command = {
                .program   = NYA_WINDOWS_CC,
                .arguments = {
                    SQLEAN_CFLAGS,
                    "-I" SQLITE_BUILD_WINDOWS_X86_64,
                    SQLEAN_GLUE_SOURCE,
                    "-o", SQLEAN_O_WINDOWS_X86_64,
                },
            },
        },
        &(NYA_BuildRule){
            .name        = "vendor_sqlean_windows_x86_64_archive",
            .policy      = NYA_BUILD_IF_OUTDATED,
            .input_file  = SQLEAN_O_WINDOWS_X86_64,
            .output_file = SQLEAN_A_WINDOWS_X86_64,

            .command = {
                .program   = NYA_WINDOWS_AR,
                .arguments = { "rcs", SQLEAN_A_WINDOWS_X86_64, SQLEAN_O_WINDOWS_X86_64, },
            },
        },
    },
};
