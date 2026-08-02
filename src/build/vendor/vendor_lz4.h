/**
 * @file vendor_lz4.h
 *
 * lz4. Plain Makefile that builds in tree, so the two targets would fight over the same archive
 * path. The Windows one is renamed after the fact to keep both available at once.
 * */
#pragma once

#include "nyangine/nyangine.h"

#include "build/hooks/hooks.h"

#define LZ4_LIB "./vendor/lz4/lib"
#define LZ4_A   LZ4_LIB "/liblz4.a"
#define LZ4_A_LIN                                                                                                                                    \
    "./vendor/lz4"                                                                                                                                   \
    "/liblz4-linux.a"
#define LZ4_A_WIN                                                                                                                                    \
    "./vendor/lz4"                                                                                                                                   \
    "/liblz4-windows.a"

NYA_VendorRule vendor_lz4_linux_x86_64 = {
    .name = "lz4 (linux-x86_64)",

    .includes     = { "-I./vendor/lz4/lib/", },
    .linker_flags = { LZ4_A_LIN, },

    .parts = {
        &(NYA_BuildRule){
            .name        = "vendor_lz4_linux_x86_64_compile",
            .policy      = NYA_BUILD_ONCE,
            .output_file = LZ4_A_LIN,

            .command = {
                .program   = "make",
                .arguments = { "-C", LZ4_LIB, "liblz4.a", "-j", NPROCS, "CC=" CC, },
            },
        },
        &(NYA_BuildRule){
            .name        = "vendor_lz4_linux_x86_64_rename",
            .policy      = NYA_BUILD_ONCE,
            .is_metarule = true,
            .input_file  = LZ4_A,
            .output_file = LZ4_A_LIN,

            // Copied, not moved: lz4 leaves lib/liblz4.a as a relative symlink into cachedObjs,
            // so a move would succeed and leave a dangling link one directory up.
            .post_build_hooks = { &hook_copy_file, },
        },
    },
};

NYA_VendorRule vendor_lz4_windows_x86_64 = {
    .name = "lz4 (windows-x86_64)",

    .includes     = { "-I./vendor/lz4/lib/", },
    .linker_flags = { LZ4_A_WIN, },

    .parts = {
        &(NYA_BuildRule){
            .name        = "vendor_lz4_windows_x86_64_clean",
            .policy      = NYA_BUILD_ONCE,
            .output_file = LZ4_A_WIN,

            .command = {
                .program   = "make",
                .arguments = { "-C", LZ4_LIB, "clean", },
            },
        },
        &(NYA_BuildRule){
            .name        = "vendor_lz4_windows_x86_64_compile",
            .policy      = NYA_BUILD_ONCE,
            .output_file = LZ4_A_WIN,

            .command = {
                .program   = "make",
                .arguments = { "-C", LZ4_LIB, "liblz4.a", "-j", NPROCS, "CC=" NYA_WINDOWS_CC, "AR=" NYA_WINDOWS_AR, },
            },
        },
        &(NYA_BuildRule){
            .name        = "vendor_lz4_windows_x86_64_rename",
            .policy      = NYA_BUILD_ONCE,
            .is_metarule = true,
            .input_file  = LZ4_A,
            .output_file = LZ4_A_WIN,

            // hook rather than `mv`, which does not exist on Windows.
            .post_build_hooks = { &hook_copy_file, },
        },
    },
};
