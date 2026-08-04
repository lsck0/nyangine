/**
 * @file vendor_box2d.h
 *
 * Box2D v3, 2D physics. cmake, built static out of tree.
 * */
#pragma once

#include "nyangine/nyangine.h"

#include "build/hooks/hooks.h"
#include "build/toolchain.h"
#include "build/vendor/vendor_common.h"

// clang-format off

#define BOX2D_SOURCE               "./vendor/box2d"
#define BOX2D_BUILD_LINUX_X86_64   "./vendor/box2d/build-linux-x86_64"
#define BOX2D_BUILD_WINDOWS_X86_64 "./vendor/box2d/build-windows-x86_64"

#define BOX2D_A_LINUX_X86_64   BOX2D_BUILD_LINUX_X86_64 "/src/libbox2d.a"
#define BOX2D_A_WINDOWS_X86_64 BOX2D_BUILD_WINDOWS_X86_64 "/src/libbox2d.a"

#define BOX2D_CMAKE_COMMON      \
    NYA_CMAKE_STATIC,           \
    "-DBOX2D_BUILD_DOCS=OFF",   \
    "-DBOX2D_SAMPLES=OFF",      \
    "-DBOX2D_UNIT_TESTS=OFF",   \
    "-DBOX2D_BENCHMARKS=OFF"

// clang-format on

NYA_VendorRule vendor_box2d_linux_x86_64 = {
    .name = "box2d (linux-x86_64)",

    .includes     = { "-I./vendor/box2d/include/", },
    .linker_flags = { BOX2D_A_LINUX_X86_64, },

    .parts = {
        &(NYA_BuildRule){
            .name        = "vendor_box2d_linux_x86_64_configure",
            .policy      = NYA_BUILD_ONCE,
            .output_file = BOX2D_A_LINUX_X86_64,

            .command = {
                .program   = "cmake",
                .arguments = { "-S", BOX2D_SOURCE, "-B", BOX2D_BUILD_LINUX_X86_64, BOX2D_CMAKE_COMMON, },
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

NYA_VendorRule vendor_box2d_windows_x86_64 = {
    .name = "box2d (windows-x86_64)",

    .includes     = { "-I./vendor/box2d/include/", },
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
                    // box2d's timer.c includes <Windows.h> with a capital W. See the shim header.
                    "-DCMAKE_C_FLAGS=-I%CWD%/src/build/compat",
                },
            },

            .pre_build_hooks = { &hook_invalidate_stale_cmake_cache, &hook_expand_cwd, },
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
