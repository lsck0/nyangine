/**
 * @file vendor_box2d.h
 *
 * Box2D v3, 2D physics. cmake, built static out of tree.
 * */
#pragma once

#include "nyangine/nyangine.h"
#include "build/hooks.h"
#include "build/flags.h"
#include "build/vendor/vendor_common.h"

// clang-format off

#define BOX2D_SOURCE               "./vendor/box2d"
#define BOX2D_BUILD_LINUX_X86_64   "./vendor/box2d/build-linux-x86_64"
#define BOX2D_BUILD_WINDOWS_X86_64 "./vendor/box2d/build-windows-x86_64"

#define BOX2D_A_LINUX_X86_64   BOX2D_BUILD_LINUX_X86_64 "/src/libbox2d.a"
#define BOX2D_A_WINDOWS_X86_64 BOX2D_BUILD_WINDOWS_X86_64 "/src/libbox2d.a"

// Worlds alive at once, one per NYA_World. Box2D keeps them in a static array, 344 KB at its default of
// 128, which a kernel backing .bss with huge pages makes resident whether or not a world exists.
#define BOX2D_MAX_WORLDS "8"

#define BOX2D_CMAKE_COMMON      \
    NYA_CMAKE_STATIC,           \
    "-DBOX2D_BUILD_DOCS=OFF",   \
    "-DBOX2D_SAMPLES=OFF",      \
    "-DBOX2D_UNIT_TESTS=OFF",   \
    "-DBOX2D_BENCHMARKS=OFF"

// clang-format on

NYA_VendorRule vendor_box2d_linux_x86_64 = {
    .options_file  = "./src/build/vendor/vendor_box2d.h",
    .options_stamp = BOX2D_BUILD_LINUX_X86_64 "/nya_options.stamp",

    .name = "box2d (linux-x86_64)",

    .includes     = { "-I./vendor/box2d/include/", "-DB2_MAX_WORLDS=" BOX2D_MAX_WORLDS, },
    .linker_flags = { BOX2D_A_LINUX_X86_64, },

    .parts = {
        &(NYA_BuildRule){
            .name        = "vendor_box2d_linux_x86_64_configure",
            .policy      = NYA_BUILD_ONCE,
            .output_file = BOX2D_A_LINUX_X86_64,

            .command = {
                .program   = "cmake",
                .arguments = { "-S", BOX2D_SOURCE, "-B", BOX2D_BUILD_LINUX_X86_64, BOX2D_CMAKE_COMMON, "-DCMAKE_C_FLAGS=-DB2_MAX_WORLDS=" BOX2D_MAX_WORLDS, },
            },

            .pre_build_hooks = { &hook_invalidate_stale_cmake_cache, },
        },
        &(NYA_BuildRule){
            .name        = "vendor_box2d_linux_x86_64_compile",
            .policy      = NYA_BUILD_ONCE,
            .output_file = BOX2D_A_LINUX_X86_64,

            .command = {
                .program   = "cmake",
                .arguments = { "--build", BOX2D_BUILD_LINUX_X86_64, "--config", "Release", "--", "-j", NPROCS, },
            },
        },
    },
};

// box2d's timer.c includes <Windows.h> with a capital W, which a case-sensitive mingw sysroot does not
// have. On a Windows host the filesystem is case-insensitive, and the shim would shadow the real header.
#if OS_WINDOWS
#define BOX2D_WINDOWS_C_FLAGS "-DCMAKE_C_FLAGS=-DB2_MAX_WORLDS=" BOX2D_MAX_WORLDS,
#else
#define BOX2D_WINDOWS_C_FLAGS "-DCMAKE_C_FLAGS=-I%CWD%/src/build/compat -DB2_MAX_WORLDS=" BOX2D_MAX_WORLDS,
#endif

NYA_VendorRule vendor_box2d_windows_x86_64 = {
    .options_file  = "./src/build/vendor/vendor_box2d.h",
    .options_stamp = BOX2D_BUILD_WINDOWS_X86_64 "/nya_options.stamp",

    .name = "box2d (windows-x86_64)",

    .includes     = { "-I./vendor/box2d/include/", "-DB2_MAX_WORLDS=" BOX2D_MAX_WORLDS, },
    .linker_flags = { BOX2D_A_WINDOWS_X86_64, },

    .parts = {
        &(NYA_BuildRule){
            .name        = "vendor_box2d_windows_x86_64_configure",
            .policy      = NYA_BUILD_ONCE,
            .output_file = BOX2D_A_WINDOWS_X86_64,

            .command = {
                .program   = "cmake",
                .arguments = {
                    "-S", BOX2D_SOURCE,
                    "-B", BOX2D_BUILD_WINDOWS_X86_64,
                    BOX2D_CMAKE_COMMON,
                    NYA_CMAKE_WINDOWS_TOOLCHAIN,
                    BOX2D_WINDOWS_C_FLAGS
                },
            },

            // The cwd is expanded first, so the stale check compares the arguments cmake will actually get.
            .pre_build_hooks = { &hook_expand_cwd, &hook_invalidate_stale_cmake_cache, },
        },
        &(NYA_BuildRule){
            .name        = "vendor_box2d_windows_x86_64_compile",
            .policy      = NYA_BUILD_ONCE,
            .output_file = BOX2D_A_WINDOWS_X86_64,

            .command = {
                .program   = "cmake",
                .arguments = { "--build", BOX2D_BUILD_WINDOWS_X86_64, "--config", "Release", "--", "-j", NPROCS, },
            },
        },
    },
};
