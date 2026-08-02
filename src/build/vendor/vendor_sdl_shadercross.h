/**
 * @file vendor_sdl_shadercross.h
 *
 * SDL_shadercross. Produces a tool rather than a library, so it carries no consumer flags: nothing
 * links against it, the shader rules invoke the binary it builds.
 * */
#pragma once

#include "nyangine/nyangine.h"

#include "build/hooks/hooks.h"
#include "build/vendor/vendor_sdl.h"

#define SHADERCROSS_SOURCE "./vendor/sdl-shadercross"
#define SHADERCROSS_BUILD  "./vendor/sdl-shadercross/build"
#define SHADERCROSS_BINARY SHADERCROSS_BUILD "/shadercross"

NYA_VendorRule vendor_sdl_shadercross_linux_x86_64 = {
    .name = "sdl-shadercross (linux-x86_64)",

    .parts = {
        &(NYA_BuildRule){
            .name        = "vendor_sdl_shadercross_linux_x86_64_configure",
            .policy      = NYA_BUILD_ONCE,
            .output_file = SHADERCROSS_BINARY,

            .command = {
                .program   = "cmake",
                .arguments = {
                    "-S", SHADERCROSS_SOURCE,
                    "-B", SHADERCROSS_BUILD,
                    "-GNinja",
                    "-DSDLSHADERCROSS_VENDORED=ON",
                    // Point at the SDL we just built rather than whatever the host happens to have
                    // installed. Without this the configure succeeds on a developer machine with a
                    // system SDL3 and fails on a clean checkout or in CI, which is exactly the kind
                    // of difference that only shows up once it is someone else's problem.
                    "-DCMAKE_PREFIX_PATH=" SDL_BUILD_LINUX_X86_64,
                },
            },

            // cmake resolves a relative CMAKE_PREFIX_PATH against the build directory, not the
            // working directory, so it has to be made absolute first.
            .pre_build_hooks = { &hook_absolutize_cmake_prefix_path, },
        },
        &(NYA_BuildRule){
            .name        = "vendor_sdl_shadercross_linux_x86_64_compile",
            .policy      = NYA_BUILD_ONCE,
            .output_file = SHADERCROSS_BINARY,

            .command = {
                .program   = "cmake",
                .arguments = {
                    "--build", SHADERCROSS_BUILD,
                    "--config", "Release",
                    "--", "-j", NPROCS,
                },
            },
        },
    },
};
