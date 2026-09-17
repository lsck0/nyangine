/**
 * @file vendor_sqlean.h
 *
 * sqlean, a set of SQLite extensions, built into a static archive from one object.
 *
 * sqlean's Makefile only builds loadable shared extensions, so it is not used. The extension sources
 * are compiled into the program and registered with sqlite3_auto_extension, as SQLite intends for
 * static use.
 *
 * The compiled file is src/nyangine/plugins/sqlite/sqlean_extensions.c, which includes the chosen
 * vendored sources and exposes `nya_sqlean_init`; it lists which extensions are in and why. Built
 * here, not in the unity build, so third party code is not held to the engine's warnings.
 *
 * Always built and linked. With NYA_PLUGIN_SQLITE off nothing references `nya_sqlean_init`, so the
 * linker pulls in nothing.
 *
 * This archive calls into libsqlite3.a, so it must come before it in the link. See vendor.h.
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
 * SQLITE_CORE makes SQLITE_EXTENSION_INIT1 expand to nothing so sqlite3_* calls are direct. Without
 * it they go through a dispatch pointer only a runtime loader sets.
 *
 * -std=c11 and -w: third party code against an older standard, with warnings that belong upstream.
 * */
#define SQLEAN_CFLAGS               \
    "-c", "-O2", "-std=c11", "-w",  \
    NYA_VENDOR_SECTIONS,            \
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
            // ONCE keyed on the directory, like the lz4 and lua metarules. Without a policy it defaults to
            // NYA_BUILD_ALWAYS and reruns the mkdir on every ./build.
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
