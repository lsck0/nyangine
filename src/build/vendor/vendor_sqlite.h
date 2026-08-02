/**
 * @file vendor_sqlite.h
 *
 * SQLite. Autotools, built out of tree into a static archive.
 *
 * Like libbacktrace this needs `sh` and `make`, so building it on a Windows host means msys2.
 * */
#pragma once

#include "nyangine/nyangine.h"

#include "build/hooks/hooks.h"

#define SQLITE_BUILD_LINUX_X86_64   "./vendor/sqlite/build-linux-x86_64/"
#define SQLITE_BUILD_WINDOWS_X86_64 "./vendor/sqlite/build-windows-x86_64/"

#define SQLITE_A_LINUX_X86_64   SQLITE_BUILD_LINUX_X86_64 "libsqlite3.a"
#define SQLITE_A_WINDOWS_X86_64 SQLITE_BUILD_WINDOWS_X86_64 "libsqlite3.a"

NYA_VendorRule vendor_sqlite_linux_x86_64 = {
    .name = "sqlite (linux-x86_64)",

    .includes     = { "-I" SQLITE_BUILD_LINUX_X86_64, },
    .linker_flags = { SQLITE_A_LINUX_X86_64, },

    .parts = {
        &(NYA_BuildRule){
            .name        = "vendor_sqlite_linux_x86_64_configure",
            .policy      = NYA_BUILD_ONCE,
            .output_file = SQLITE_A_LINUX_X86_64,

            .command = {
                .working_directory = SQLITE_BUILD_LINUX_X86_64,
                .program           = "../configure",
                .arguments         = { "--disable-shared", "--enable-static", "--disable-tcl", "CC=" CC, "CFLAGS=-O2 -fPIC", },
            },

            .pre_build_hooks = { &hook_create_build_directory, },
        },
        &(NYA_BuildRule){
            .name        = "vendor_sqlite_linux_x86_64_compile",
            .policy      = NYA_BUILD_ONCE,
            .output_file = SQLITE_A_LINUX_X86_64,

            .command = {
                .program   = "make",
                .arguments = { "-C", SQLITE_BUILD_LINUX_X86_64, "libsqlite3.a", "-j", NPROCS, },
            },
        },
    },
};

NYA_VendorRule vendor_sqlite_windows_x86_64 = {
    .name = "sqlite (windows-x86_64)",

    .includes     = { "-I" SQLITE_BUILD_WINDOWS_X86_64, },
    .linker_flags = { SQLITE_A_WINDOWS_X86_64, },

    .parts = {
        &(NYA_BuildRule){
            .name        = "vendor_sqlite_windows_x86_64_configure",
            .policy      = NYA_BUILD_ONCE,
            .output_file = SQLITE_A_WINDOWS_X86_64,

            .command = {
                .working_directory = SQLITE_BUILD_WINDOWS_X86_64,
                .program           = "../configure",
                .arguments = {
                    "--disable-shared", "--enable-static", "--disable-tcl",
                    NYA_AUTOTOOLS_WINDOWS_HOST
                    "CC=" NYA_WINDOWS_CC,
                    "CFLAGS=-O2",
                },
            },

            .pre_build_hooks = { &hook_create_build_directory, },
        },
        &(NYA_BuildRule){
            .name        = "vendor_sqlite_windows_x86_64_compile",
            .policy      = NYA_BUILD_ONCE,
            .output_file = SQLITE_A_WINDOWS_X86_64,

            .command = {
                .program   = "make",
                .arguments = { "-C", SQLITE_BUILD_WINDOWS_X86_64, "libsqlite3.a", "-j", NPROCS, },
            },
        },
    },
};
