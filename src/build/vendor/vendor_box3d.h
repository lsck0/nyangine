/**
 * @file vendor_box3d.h
 *
 * Box3D, 3D physics. cmake, built static out of tree.
 * */
#pragma once

#include "nyangine/nyangine.h"

#include "build/hooks/hooks.h"
#include "build/toolchain.h"
#include "build/vendor/vendor_common.h"

// clang-format off

#define BOX3D_SOURCE               "./vendor/box3d"
#define BOX3D_BUILD_LINUX_X86_64   "./vendor/box3d/build-linux-x86_64"
#define BOX3D_BUILD_WINDOWS_X86_64 "./vendor/box3d/build-windows-x86_64"

#define BOX3D_A_LINUX_X86_64   BOX3D_BUILD_LINUX_X86_64 "/src/libbox3d.a"
#define BOX3D_A_WINDOWS_X86_64 BOX3D_BUILD_WINDOWS_X86_64 "/src/libbox3d.a"

#define BOX3D_CMAKE_COMMON      \
    NYA_CMAKE_STATIC,           \
    "-DBOX3D_BUILD_DOCS=OFF",   \
    "-DBOX3D_SAMPLES=OFF",      \
    "-DBOX3D_UNIT_TESTS=OFF",   \
    "-DBOX3D_BENCHMARKS=OFF"

// clang-format on

NYA_VendorRule vendor_box3d_linux_x86_64 = {
    .name = "box3d (linux-x86_64)",

    .includes     = { "-I./vendor/box3d/include/", },
    .linker_flags = { BOX3D_A_LINUX_X86_64, },

    .parts = {
        &(NYA_BuildRule){
            .name        = "vendor_box3d_linux_x86_64_configure",
            .policy      = NYA_BUILD_ONCE,
            .output_file = BOX3D_A_LINUX_X86_64,

            .command = {
                .program   = "cmake",
                .arguments = { "-S", BOX3D_SOURCE, "-B", BOX3D_BUILD_LINUX_X86_64, BOX3D_CMAKE_COMMON, },
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
    .name = "box3d (windows-x86_64)",

    .includes     = { "-I./vendor/box3d/include/", },
    .linker_flags = { BOX3D_A_WINDOWS_X86_64, },

    .parts = {
        &(NYA_BuildRule){
            .name        = "vendor_box3d_windows_x86_64_configure",
            .policy      = NYA_BUILD_ONCE,
            .output_file = BOX3D_A_WINDOWS_X86_64,

            .command = {
                .program   = "cmake",
                .arguments = { "-S", BOX3D_SOURCE, "-B", BOX3D_BUILD_WINDOWS_X86_64, BOX3D_CMAKE_COMMON, NYA_CMAKE_WINDOWS_TOOLCHAIN, },
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
