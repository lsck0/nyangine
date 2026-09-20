/**
 * @file vendor_monocypher.h
 *
 * Monocypher: X25519, BLAKE2b and XChaCha20-Poly1305 in one audited C file, which is all the network
 * handshake and packet encryption need. An archive like ufbx, so its code stays out of the unity build.
 * */
#pragma once

#include "nyangine/nyangine.h"
#include "build/hooks.h"
#include "build/flags.h"
#include "build/vendor/vendor_common.h"

#define MONOCYPHER_SOURCE_DIRECTORY "./vendor/monocypher/src"
#define MONOCYPHER_SOURCE           MONOCYPHER_SOURCE_DIRECTORY "/monocypher.c"

#define MONOCYPHER_BUILD_LINUX_X86_64   "./vendor/monocypher/build-linux-x86_64/"
#define MONOCYPHER_BUILD_WINDOWS_X86_64 "./vendor/monocypher/build-windows-x86_64/"

#define MONOCYPHER_O_LINUX_X86_64   MONOCYPHER_BUILD_LINUX_X86_64 "monocypher.o"
#define MONOCYPHER_O_WINDOWS_X86_64 MONOCYPHER_BUILD_WINDOWS_X86_64 "monocypher.o"

#define MONOCYPHER_A_LINUX_X86_64   MONOCYPHER_BUILD_LINUX_X86_64 "libmonocypher.a"
#define MONOCYPHER_A_WINDOWS_X86_64 MONOCYPHER_BUILD_WINDOWS_X86_64 "libmonocypher.a"

// clang-format off

#define MONOCYPHER_CFLAGS "-c", "-O2", "-std=c99", NYA_VENDOR_SECTIONS, "-I" MONOCYPHER_SOURCE_DIRECTORY

// clang-format on

NYA_VendorRule vendor_monocypher_linux_x86_64 = {
    .name = "monocypher (linux-x86_64)",

    .includes     = { "-I" MONOCYPHER_SOURCE_DIRECTORY, },
    .linker_flags = { MONOCYPHER_A_LINUX_X86_64, },

    .parts = {
        &(NYA_BuildRule){
            .name        = "vendor_monocypher_linux_x86_64_directory",
            .policy      = NYA_BUILD_ONCE,
            .is_metarule = true,
            .output_file = MONOCYPHER_BUILD_LINUX_X86_64,

            .command         = { .working_directory = MONOCYPHER_BUILD_LINUX_X86_64, },
            .pre_build_hooks = { &hook_create_build_directory, },
        },
        &(NYA_BuildRule){
            .name        = "vendor_monocypher_linux_x86_64_compile",
            .policy      = NYA_BUILD_IF_OUTDATED,
            .input_file  = MONOCYPHER_SOURCE,
            .output_file = MONOCYPHER_O_LINUX_X86_64,

            .command = {
                .program   = CC,
                // -fPIC because the game DLL links this too.
                .arguments = { MONOCYPHER_CFLAGS, "-fPIC", MONOCYPHER_SOURCE, "-o", MONOCYPHER_O_LINUX_X86_64, },
            },
        },
        &(NYA_BuildRule){
            .name        = "vendor_monocypher_linux_x86_64_archive",
            .policy      = NYA_BUILD_IF_OUTDATED,
            .input_file  = MONOCYPHER_O_LINUX_X86_64,
            .output_file = MONOCYPHER_A_LINUX_X86_64,

            .command = {
                .program   = "ar",
                .arguments = { "rcs", MONOCYPHER_A_LINUX_X86_64, MONOCYPHER_O_LINUX_X86_64, },
            },
        },
    },
};

NYA_VendorRule vendor_monocypher_windows_x86_64 = {
    .name = "monocypher (windows-x86_64)",

    .includes = { "-I" MONOCYPHER_SOURCE_DIRECTORY, },

    // bcrypt for BCryptGenRandom, where the keys the handshake generates come from.
    .linker_flags = { MONOCYPHER_A_WINDOWS_X86_64, "-lbcrypt", },

    .parts = {
        &(NYA_BuildRule){
            .name        = "vendor_monocypher_windows_x86_64_directory",
            .policy      = NYA_BUILD_ONCE,
            .is_metarule = true,
            .output_file = MONOCYPHER_BUILD_WINDOWS_X86_64,

            .command         = { .working_directory = MONOCYPHER_BUILD_WINDOWS_X86_64, },
            .pre_build_hooks = { &hook_create_build_directory, },
        },
        &(NYA_BuildRule){
            .name        = "vendor_monocypher_windows_x86_64_compile",
            .policy      = NYA_BUILD_IF_OUTDATED,
            .input_file  = MONOCYPHER_SOURCE,
            .output_file = MONOCYPHER_O_WINDOWS_X86_64,

            .command = {
                .program   = CC,
                .arguments = { FLAGS_TARGET_WINDOWS_X86_64 MONOCYPHER_CFLAGS, MONOCYPHER_SOURCE, "-o", MONOCYPHER_O_WINDOWS_X86_64, },
            },
        },
        &(NYA_BuildRule){
            .name        = "vendor_monocypher_windows_x86_64_archive",
            .policy      = NYA_BUILD_IF_OUTDATED,
            .input_file  = MONOCYPHER_O_WINDOWS_X86_64,
            .output_file = MONOCYPHER_A_WINDOWS_X86_64,

            .command = {
                .program   = NYA_WINDOWS_AR,
                .arguments = { "rcs", MONOCYPHER_A_WINDOWS_X86_64, MONOCYPHER_O_WINDOWS_X86_64, },
            },
        },
    },
};
