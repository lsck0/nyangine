/**
 * @file vendor_ufbx.h
 *
 * ufbx, an FBX reader. One source file, built into a static archive, one object.
 *
 * FBX is a proprietary format with no specification, several generations of container and a great
 * many exporters that each disagree slightly about what they emit. ufbx is the answer to that: a
 * single C file that reads binary and ASCII FBX from 6100 up, normalises the scene it finds — indices
 * triangulated on request, geometry transforms applied, units and axes converted — and depends on
 * nothing. That last part is what makes it vendorable here at all.
 *
 * Built as an archive rather than compiled into the engine's unity build. ufbx is around twenty
 * thousand lines and the engine is one translation unit, so including it would put its whole parse on
 * every incremental rebuild of the game — and its internal helpers into the same namespace as
 * everything else. It changes only when the submodule moves, which is exactly what NYA_BUILD_IF_OUTDATED
 * against the source file expresses.
 *
 * `UFBX_NO_SCENE_EVALUATION`, `UFBX_NO_SUBDIVISION` and `UFBX_NO_TESSELLATION` cut the parts of the
 * library the engine does not call: animation evaluation, Catmull-Clark subdivision and NURBS. They
 * are opt-*out* macros upstream provides for exactly this, and together they are most of the compiled
 * size. Skinning and blend shapes stay in, since a mesh asset that could not be skinned would have to
 * be rebuilt the day anything is animated.
 *
 * -O2 rather than the -O3 the other vendors take: ufbx's own build notes call out that its bounds
 * checking is what keeps a malformed file from becoming a crash, and there is no reason to spend
 * compile time on a parser that runs once per model load.
 * */
#pragma once

#include "nyangine/nyangine.h"
#include "build/hooks.h"
#include "build/toolchain.h"
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
 * What both targets compile the one source with, target flags aside.
 *
 * The three NO_ macros are also on `cflags` below, not only here: ufbx.h declares the functions those
 * features add, and a consumer compiled without them would see prototypes for symbols the archive does
 * not contain. Agreeing on both sides is what keeps that a compile error rather than a link error.
 * */
#define UFBX_CFLAGS                 \
    "-c", "-O2", "-std=c11",        \
    "-DUFBX_NO_SCENE_EVALUATION",   \
    "-DUFBX_NO_SUBDIVISION",        \
    "-DUFBX_NO_TESSELLATION",       \
    "-I" UFBX_SOURCE_DIRECTORY

// clang-format on

NYA_VendorRule vendor_ufbx_linux_x86_64 = {
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
                .program   = CC,
                // -fPIC because the game DLL links this too, and a non-PIC object in a shared library
                // is a link error rather than something that shows up later.
                .arguments = { UFBX_CFLAGS, "-fPIC", UFBX_SOURCE, "-o", UFBX_O_LINUX_X86_64, },
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
                .program   = NYA_WINDOWS_CC,
                .arguments = { UFBX_CFLAGS, UFBX_SOURCE, "-o", UFBX_O_WINDOWS_X86_64, },
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
