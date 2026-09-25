/**
 * @file vendor_box3d.h
 *
 * Box3D, 3D physics. cmake, built static out of tree.
 * */
#pragma once

#include "nyangine-core/nyangine.h"
#include "build/hooks.h"
#include "build/flags.h"
#include "build/vendor/vendor_common.h"

// clang-format off

#define BOX3D_SOURCE               "./vendor/box3d"
#define BOX3D_BUILD_LINUX_X86_64   "./vendor/box3d/build-linux-x86_64"
#define BOX3D_BUILD_WINDOWS_X86_64 "./vendor/box3d/build-windows-x86_64"

#define BOX3D_A_LINUX_X86_64   BOX3D_BUILD_LINUX_X86_64 "/src/libbox3d.a"
#define BOX3D_A_WINDOWS_X86_64 BOX3D_BUILD_WINDOWS_X86_64 "/src/libbox3d.a"

// Worlds alive at once, one per NYA_World, for the same reason as BOX2D_MAX_WORLDS: 596 KB of .bss at 128.
#define BOX3D_MAX_WORLDS "8"

#define BOX3D_CMAKE_COMMON      \
    NYA_CMAKE_STATIC,           \
    "-DBOX3D_BUILD_DOCS=OFF",   \
    "-DBOX3D_SAMPLES=OFF",      \
    "-DBOX3D_UNIT_TESTS=OFF",   \
    "-DBOX3D_BENCHMARKS=OFF"

// clang-format on

NYA_VendorRule vendor_box3d_linux_x86_64 = {
    .options_file  = "./src/build/vendor/vendor_box3d.h",
    .options_stamp = BOX3D_BUILD_LINUX_X86_64 "/nya_options.stamp",

    .name = "box3d (linux-x86_64)",

    .includes     = { "-I./vendor/box3d/include/", "-DB3_MAX_WORLDS=" BOX3D_MAX_WORLDS, },
    .linker_flags = { BOX3D_A_LINUX_X86_64, },

    .parts = {
        &(NYA_BuildRule){
            .name        = "vendor_box3d_linux_x86_64_configure",
            .policy      = NYA_BUILD_ONCE,
            .output_file = BOX3D_A_LINUX_X86_64,

            .command = {
                .program   = "cmake",
                .arguments = { "-S", BOX3D_SOURCE, "-B", BOX3D_BUILD_LINUX_X86_64, BOX3D_CMAKE_COMMON, "-DCMAKE_C_FLAGS=-DB3_MAX_WORLDS=" BOX3D_MAX_WORLDS, },
            },

            .pre_build_hooks = { &hook_invalidate_stale_cmake_cache, },
        },
        &(NYA_BuildRule){
            .name        = "vendor_box3d_linux_x86_64_compile",
            .policy      = NYA_BUILD_ONCE,
            .output_file = BOX3D_A_LINUX_X86_64,

            .command = {
                .program   = "cmake",
                .arguments = { "--build", BOX3D_BUILD_LINUX_X86_64, "--config", "Release", "--", "-j", NPROCS, },
            },
        },
    },
};

NYA_VendorRule vendor_box3d_windows_x86_64 = {
    .options_file  = "./src/build/vendor/vendor_box3d.h",
    .options_stamp = BOX3D_BUILD_WINDOWS_X86_64 "/nya_options.stamp",

    .name = "box3d (windows-x86_64)",

    .includes     = { "-I./vendor/box3d/include/", "-DB3_MAX_WORLDS=" BOX3D_MAX_WORLDS, },
    .linker_flags = { BOX3D_A_WINDOWS_X86_64, },

    .parts = {
        &(NYA_BuildRule){
            .name        = "vendor_box3d_windows_x86_64_configure",
            .policy      = NYA_BUILD_ONCE,
            .output_file = BOX3D_A_WINDOWS_X86_64,

            .command = {
                .program   = "cmake",
                .arguments = { "-S", BOX3D_SOURCE, "-B", BOX3D_BUILD_WINDOWS_X86_64, BOX3D_CMAKE_COMMON, NYA_CMAKE_WINDOWS_TOOLCHAIN, "-DCMAKE_C_FLAGS=-DB3_MAX_WORLDS=" BOX3D_MAX_WORLDS, },
            },

            .pre_build_hooks = { &hook_invalidate_stale_cmake_cache, },
        },
        &(NYA_BuildRule){
            .name        = "vendor_box3d_windows_x86_64_compile",
            .policy      = NYA_BUILD_ONCE,
            .output_file = BOX3D_A_WINDOWS_X86_64,

            .command = {
                .program   = "cmake",
                .arguments = { "--build", BOX3D_BUILD_WINDOWS_X86_64, "--config", "Release", "--", "-j", NPROCS, },
            },
        },
    },
};
