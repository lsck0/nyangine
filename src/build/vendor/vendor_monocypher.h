/**
 * @file vendor_monocypher.h
 *
 * Monocypher: X25519, BLAKE2b, XChaCha20-Poly1305 and Argon2id in one audited C file, plus its optional
 * Ed25519 file, which is RFC 8032's signature over SHA-512 rather than the core file's BLAKE2b variant.
 * Everything the crypto module wraps. Archives like ufbx, so their code stays out of the unity build.
 *
 * The optional file gets an archive of its own rather than a second object in libmonocypher.a: it calls
 * into the core file, so it links before it, and the core archive stays exactly what it was.
 * */
#pragma once

#include "nyangine/nyangine.h"
#include "build/hooks.h"
#include "build/flags.h"
#include "build/vendor/vendor_common.h"

#define MONOCYPHER_SOURCE_DIRECTORY "./vendor/monocypher/src"
#define MONOCYPHER_SOURCE           MONOCYPHER_SOURCE_DIRECTORY "/monocypher.c"
#define MONOCYPHER_ED25519_SOURCE   MONOCYPHER_SOURCE_DIRECTORY "/optional/monocypher-ed25519.c"

#define MONOCYPHER_BUILD_LINUX_X86_64   "./vendor/monocypher/build-linux-x86_64/"
#define MONOCYPHER_BUILD_WINDOWS_X86_64 "./vendor/monocypher/build-windows-x86_64/"

#define MONOCYPHER_O_LINUX_X86_64   MONOCYPHER_BUILD_LINUX_X86_64 "monocypher.o"
#define MONOCYPHER_O_WINDOWS_X86_64 MONOCYPHER_BUILD_WINDOWS_X86_64 "monocypher.o"

#define MONOCYPHER_A_LINUX_X86_64   MONOCYPHER_BUILD_LINUX_X86_64 "libmonocypher.a"
#define MONOCYPHER_A_WINDOWS_X86_64 MONOCYPHER_BUILD_WINDOWS_X86_64 "libmonocypher.a"

#define MONOCYPHER_ED25519_O_LINUX_X86_64   MONOCYPHER_BUILD_LINUX_X86_64 "monocypher-ed25519.o"
#define MONOCYPHER_ED25519_O_WINDOWS_X86_64 MONOCYPHER_BUILD_WINDOWS_X86_64 "monocypher-ed25519.o"

#define MONOCYPHER_ED25519_A_LINUX_X86_64   MONOCYPHER_BUILD_LINUX_X86_64 "libmonocypher-ed25519.a"
#define MONOCYPHER_ED25519_A_WINDOWS_X86_64 MONOCYPHER_BUILD_WINDOWS_X86_64 "libmonocypher-ed25519.a"

// clang-format off

#define MONOCYPHER_CFLAGS "-c", "-O2", "-std=c99", NYA_VENDOR_SECTIONS, "-I" MONOCYPHER_SOURCE_DIRECTORY

// clang-format on

NYA_VendorRule vendor_monocypher_linux_x86_64 = {
    .options_file  = "./src/build/vendor/vendor_monocypher.h",
    .options_stamp = MONOCYPHER_BUILD_LINUX_X86_64 "nya_options.stamp",

    .name = "monocypher (linux-x86_64)",

    .includes     = { "-I" MONOCYPHER_SOURCE_DIRECTORY, },
    .linker_flags = { MONOCYPHER_ED25519_A_LINUX_X86_64, MONOCYPHER_A_LINUX_X86_64, },

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
                .program   = VENDOR_CC,
                // -fPIC because the game DLL links this too.
                .arguments = { VENDOR_CC_LAUNCHED, MONOCYPHER_CFLAGS, "-fPIC", MONOCYPHER_SOURCE, "-o", MONOCYPHER_O_LINUX_X86_64, },
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

            // `ar rcs` adds to an archive that already exists rather than replacing it, so an object that gets renamed would linger inside it. Deleting it first makes the archive say only what was just compiled.
            .pre_build_hooks = { &hook_remove_output_file, },
        },
        &(NYA_BuildRule){
            .name        = "vendor_monocypher_ed25519_linux_x86_64_compile",
            .policy      = NYA_BUILD_IF_OUTDATED,
            .input_file  = MONOCYPHER_ED25519_SOURCE,
            .output_file = MONOCYPHER_ED25519_O_LINUX_X86_64,

            .command = {
                .program   = VENDOR_CC,
                .arguments = { VENDOR_CC_LAUNCHED, MONOCYPHER_CFLAGS, "-fPIC", MONOCYPHER_ED25519_SOURCE, "-o", MONOCYPHER_ED25519_O_LINUX_X86_64, },
            },
        },
        &(NYA_BuildRule){
            .name        = "vendor_monocypher_ed25519_linux_x86_64_archive",
            .policy      = NYA_BUILD_IF_OUTDATED,
            .input_file  = MONOCYPHER_ED25519_O_LINUX_X86_64,
            .output_file = MONOCYPHER_ED25519_A_LINUX_X86_64,

            .command = {
                .program   = "ar",
                .arguments = { "rcs", MONOCYPHER_ED25519_A_LINUX_X86_64, MONOCYPHER_ED25519_O_LINUX_X86_64, },
            },

            // `ar rcs` adds to an archive that already exists rather than replacing it, so an object that gets renamed would linger inside it. Deleting it first makes the archive say only what was just compiled.
            .pre_build_hooks = { &hook_remove_output_file, },
        },
    },
};

NYA_VendorRule vendor_monocypher_windows_x86_64 = {
    .options_file  = "./src/build/vendor/vendor_monocypher.h",
    .options_stamp = MONOCYPHER_BUILD_WINDOWS_X86_64 "nya_options.stamp",

    .name = "monocypher (windows-x86_64)",

    .includes = { "-I" MONOCYPHER_SOURCE_DIRECTORY, },

    // bcrypt for BCryptGenRandom, where the keys the handshake generates come from.
    .linker_flags = { MONOCYPHER_ED25519_A_WINDOWS_X86_64, MONOCYPHER_A_WINDOWS_X86_64, "-lbcrypt", },

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
                .program   = VENDOR_CC,
                .arguments = { VENDOR_CC_LAUNCHED, FLAGS_TARGET_WINDOWS_X86_64 MONOCYPHER_CFLAGS, MONOCYPHER_SOURCE, "-o", MONOCYPHER_O_WINDOWS_X86_64, },
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

            // `ar rcs` adds to an archive that already exists rather than replacing it, so an object that gets renamed would linger inside it. Deleting it first makes the archive say only what was just compiled.
            .pre_build_hooks = { &hook_remove_output_file, },
        },
        &(NYA_BuildRule){
            .name        = "vendor_monocypher_ed25519_windows_x86_64_compile",
            .policy      = NYA_BUILD_IF_OUTDATED,
            .input_file  = MONOCYPHER_ED25519_SOURCE,
            .output_file = MONOCYPHER_ED25519_O_WINDOWS_X86_64,

            .command = {
                .program   = VENDOR_CC,
                .arguments = { VENDOR_CC_LAUNCHED, FLAGS_TARGET_WINDOWS_X86_64 MONOCYPHER_CFLAGS, MONOCYPHER_ED25519_SOURCE, "-o", MONOCYPHER_ED25519_O_WINDOWS_X86_64, },
            },
        },
        &(NYA_BuildRule){
            .name        = "vendor_monocypher_ed25519_windows_x86_64_archive",
            .policy      = NYA_BUILD_IF_OUTDATED,
            .input_file  = MONOCYPHER_ED25519_O_WINDOWS_X86_64,
            .output_file = MONOCYPHER_ED25519_A_WINDOWS_X86_64,

            .command = {
                .program   = NYA_WINDOWS_AR,
                .arguments = { "rcs", MONOCYPHER_ED25519_A_WINDOWS_X86_64, MONOCYPHER_ED25519_O_WINDOWS_X86_64, },
            },

            // `ar rcs` adds to an archive that already exists rather than replacing it, so an object that gets renamed would linger inside it. Deleting it first makes the archive say only what was just compiled.
            .pre_build_hooks = { &hook_remove_output_file, },
        },
    },
};
