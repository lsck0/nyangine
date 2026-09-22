/**
 * @file vendor_ufbx.h
 *
 * ufbx, an FBX reader: one C file with no dependencies, built into a static archive. Reads binary and
 * ASCII FBX from 6100 up and normalises the scene (triangulation on request, geometry transforms,
 * units and axes).
 *
 * An archive rather than part of the unity build: it is about twenty thousand lines and would be
 * reparsed on every incremental rebuild, with its helpers in the engine's namespace. It only changes
 * when the submodule moves.
 *
 * `UFBX_NO_SCENE_EVALUATION`, `UFBX_NO_SUBDIVISION` and `UFBX_NO_TESSELLATION` drop animation
 * evaluation, Catmull-Clark subdivision and NURBS, which the engine does not call and which are most
 * of the compiled size. Skinning and blend shapes stay.
 *
 * -O2 rather than -O3: it runs once per model load and its bounds checking is what keeps a malformed
 * file from crashing.
 * */
#pragma once

#include "nyangine/nyangine.h"
#include "build/hooks.h"
#include "build/flags.h"
#include "build/vendor/vendor_common.h"

#define UFBX_SOURCE_DIRECTORY "./vendor/ufbx"
#define UFBX_SOURCE           UFBX_SOURCE_DIRECTORY "/ufbx.c"

#define UFBX_BUILD_LINUX_X86_64   UFBX_SOURCE_DIRECTORY "/build-linux-x86_64/"
#define UFBX_BUILD_WINDOWS_X86_64 UFBX_SOURCE_DIRECTORY "/build-windows-x86_64/"

#define UFBX_O_LINUX_X86_64   UFBX_BUILD_LINUX_X86_64 "ufbx.o"
#define UFBX_O_WINDOWS_X86_64 UFBX_BUILD_WINDOWS_X86_64 "ufbx.o"

#define UFBX_A_LINUX_X86_64   UFBX_BUILD_LINUX_X86_64 "libufbx.a"
#define UFBX_A_WINDOWS_X86_64 UFBX_BUILD_WINDOWS_X86_64 "libufbx.a"

// clang-format off

/**
 * What both targets compile the source with, target flags aside.
 *
 * The NO_ macros are also on `cflags`: ufbx.h declares the functions those features add, so both
 * sides must agree or a consumer sees prototypes the archive lacks and fails at link time instead of
 * compile time.
 * */
#define UFBX_CFLAGS                 \
    "-c", "-O2", "-std=c11",        \
    NYA_VENDOR_SECTIONS,            \
    "-DUFBX_NO_SCENE_EVALUATION",   \
    "-DUFBX_NO_SUBDIVISION",        \
    "-DUFBX_NO_TESSELLATION",       \
    "-I" UFBX_SOURCE_DIRECTORY

// clang-format on

NYA_VendorRule vendor_ufbx_linux_x86_64 = {
    .options_file  = "./src/build/vendor/vendor_ufbx.h",
    .options_stamp = UFBX_BUILD_LINUX_X86_64 "nya_options.stamp",

    .name = "ufbx (linux-x86_64)",

    .cflags       = { "-DUFBX_NO_SCENE_EVALUATION", "-DUFBX_NO_SUBDIVISION", "-DUFBX_NO_TESSELLATION", },
    .includes     = { "-I" UFBX_SOURCE_DIRECTORY, },
    .linker_flags = { UFBX_A_LINUX_X86_64, },

    .parts = {
        &(NYA_BuildRule){
            .name = "vendor_ufbx_linux_x86_64_directory",
            // NYA_BUILD_ONCE, and a metarule: see the same rule in vendor_sqlvec.h, where the default
            // policy meant the directory was created on every ./build.
            .policy      = NYA_BUILD_ONCE,
            .is_metarule = true,
            .output_file = UFBX_BUILD_LINUX_X86_64,

            .command         = { .working_directory = UFBX_BUILD_LINUX_X86_64, },
            .pre_build_hooks = { &hook_create_build_directory, },
        },
        &(NYA_BuildRule){
            .name        = "vendor_ufbx_linux_x86_64_compile",
            .policy      = NYA_BUILD_IF_OUTDATED,
            .input_file  = UFBX_SOURCE,
            .output_file = UFBX_O_LINUX_X86_64,

            .command = {
                .program   = VENDOR_CC,
                // -fPIC because the game DLL links this too, and a non-PIC object in a shared library
                // is a link error rather than something that shows up later.
                .arguments = { VENDOR_CC_LAUNCHED, UFBX_CFLAGS, "-fPIC", UFBX_SOURCE, "-o", UFBX_O_LINUX_X86_64, },
            },
        },
        &(NYA_BuildRule){
            .name        = "vendor_ufbx_linux_x86_64_archive",
            .policy      = NYA_BUILD_IF_OUTDATED,
            .input_file  = UFBX_O_LINUX_X86_64,
            .output_file = UFBX_A_LINUX_X86_64,

            .command = {
                .program   = "ar",
                .arguments = { "rcs", UFBX_A_LINUX_X86_64, UFBX_O_LINUX_X86_64, },
            },
        },
    },
};

NYA_VendorRule vendor_ufbx_windows_x86_64 = {
    .options_file  = "./src/build/vendor/vendor_ufbx.h",
    .options_stamp = UFBX_BUILD_WINDOWS_X86_64 "nya_options.stamp",

    .name = "ufbx (windows-x86_64)",

    .cflags       = { "-DUFBX_NO_SCENE_EVALUATION", "-DUFBX_NO_SUBDIVISION", "-DUFBX_NO_TESSELLATION", },
    .includes     = { "-I" UFBX_SOURCE_DIRECTORY, },
    .linker_flags = { UFBX_A_WINDOWS_X86_64, },

    .parts = {
        &(NYA_BuildRule){
            .name        = "vendor_ufbx_windows_x86_64_directory",
            .policy      = NYA_BUILD_ONCE,
            .is_metarule = true,
            .output_file = UFBX_BUILD_WINDOWS_X86_64,

            .command         = { .working_directory = UFBX_BUILD_WINDOWS_X86_64, },
            .pre_build_hooks = { &hook_create_build_directory, },
        },
        &(NYA_BuildRule){
            .name        = "vendor_ufbx_windows_x86_64_compile",
            .policy      = NYA_BUILD_IF_OUTDATED,
            .input_file  = UFBX_SOURCE,
            .output_file = UFBX_O_WINDOWS_X86_64,

            .command = {
                .program   = VENDOR_CC,
                .arguments = { VENDOR_CC_LAUNCHED, FLAGS_TARGET_WINDOWS_X86_64 UFBX_CFLAGS, UFBX_SOURCE, "-o", UFBX_O_WINDOWS_X86_64, },
            },
        },
        &(NYA_BuildRule){
            .name        = "vendor_ufbx_windows_x86_64_archive",
            .policy      = NYA_BUILD_IF_OUTDATED,
            .input_file  = UFBX_O_WINDOWS_X86_64,
            .output_file = UFBX_A_WINDOWS_X86_64,

            .command = {
                .program   = NYA_WINDOWS_AR,
                .arguments = { "rcs", UFBX_A_WINDOWS_X86_64, UFBX_O_WINDOWS_X86_64, },
            },
        },
    },
};
