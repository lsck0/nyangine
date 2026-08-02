/**
 * @file vendor_libbacktrace.h
 *
 * libbacktrace, the symbolization backend behind base_backtrace.h.
 *
 * Autotools rather than cmake, so this is an out of tree build: create the build directory,
 * configure into it from the source tree, then make. Both parts share the same output_file, so
 * once the archive exists the chain is skipped.
 *
 * Because it is autotools, building this on a Windows host needs `sh` and `make` on PATH, which in
 * practice means an msys2 install. Cross compiling it from Linux needs only mingw-w64.
 * */
#pragma once

#include "nyangine/nyangine.h"

#include "build/hooks/hooks.h"

// clang-format off

#define BACKTRACE_BUILD_LINUX_X86_64   "./vendor/libbacktrace/build-linux-x86_64/"
#define BACKTRACE_BUILD_WINDOWS_X86_64 "./vendor/libbacktrace/build-windows-x86_64/"

#define BACKTRACE_A_LINUX_X86_64   BACKTRACE_BUILD_LINUX_X86_64 ".libs/libbacktrace.a"
#define BACKTRACE_A_WINDOWS_X86_64 BACKTRACE_BUILD_WINDOWS_X86_64 ".libs/libbacktrace.a"

// backtrace.h ships in the source tree, but backtrace-supported.h and config.h are generated into
// the build directory, so a consumer needs both paths. base_backtrace.h keys off
// __has_include("backtrace.h"), which means these and the archive must travel together: a target
// given neither simply compiles against the null backend.
#define BACKTRACE_INCLUDES_LINUX_X86_64   "-I./vendor/libbacktrace/", "-I" BACKTRACE_BUILD_LINUX_X86_64
#define BACKTRACE_INCLUDES_WINDOWS_X86_64 "-I./vendor/libbacktrace/", "-I" BACKTRACE_BUILD_WINDOWS_X86_64

// clang-format on

NYA_VendorRule vendor_libbacktrace_linux_x86_64 = {
    .name = "libbacktrace (linux-x86_64)",

    .includes     = { BACKTRACE_INCLUDES_LINUX_X86_64, },
    .linker_flags = { BACKTRACE_A_LINUX_X86_64, },

    .parts = {
        &(NYA_BuildRule){
            .name        = "vendor_libbacktrace_linux_x86_64_configure",
            .policy      = NYA_BUILD_ONCE,
            .output_file = BACKTRACE_A_LINUX_X86_64,

            .command = {
                .working_directory = BACKTRACE_BUILD_LINUX_X86_64,
                .program           = "../configure",
                .arguments = {
                    "CC=" CC,
                    // -fPIC because the debug build links this archive into a shared object.
                    "CFLAGS=-g -O2 -fPIC",
                },
            },

            .pre_build_hooks = { &hook_create_build_directory, },
        },
        &(NYA_BuildRule){
            .name        = "vendor_libbacktrace_linux_x86_64_compile",
            .policy      = NYA_BUILD_ONCE,
            .output_file = BACKTRACE_A_LINUX_X86_64,

            .command = {
                .program   = "make",
                .arguments = { "-C", BACKTRACE_BUILD_LINUX_X86_64, "-j", NPROCS, },
            },
        },
    },
};

NYA_VendorRule vendor_libbacktrace_windows_x86_64 = {
    .name = "libbacktrace (windows-x86_64)",

    .includes     = { BACKTRACE_INCLUDES_WINDOWS_X86_64, },
    .linker_flags = { BACKTRACE_A_WINDOWS_X86_64, },

    .parts = {
        &(NYA_BuildRule){
            .name        = "vendor_libbacktrace_windows_x86_64_configure",
            .policy      = NYA_BUILD_ONCE,
            .output_file = BACKTRACE_A_WINDOWS_X86_64,

            .command = {
                .working_directory = BACKTRACE_BUILD_WINDOWS_X86_64,
                .program           = "../configure",
                .arguments = {
                    NYA_AUTOTOOLS_WINDOWS_HOST
                    "CC=" NYA_WINDOWS_CC,
                    "CFLAGS=-g -O2",
                },
            },

            .pre_build_hooks = { &hook_create_build_directory, },
        },
        &(NYA_BuildRule){
            .name        = "vendor_libbacktrace_windows_x86_64_compile",
            .policy      = NYA_BUILD_ONCE,
            .output_file = BACKTRACE_A_WINDOWS_X86_64,

            .command = {
                .program   = "make",
                .arguments = { "-C", BACKTRACE_BUILD_WINDOWS_X86_64, "-j", NPROCS, },
            },
        },
    },
};
