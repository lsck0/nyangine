/**
 * @file vendor_sdl_shadercross.h
 *
 * SDL_shadercross. Produces a tool rather than a library, so it carries no consumer flags: nothing
 * links against it, the shader rules invoke the binary it builds.
 * */
#pragma once

#include "nyangine/nyangine.h"

#include "build/hooks/hooks.h"
#include "build/toolchain.h"
#include "build/vendor/vendor_common.h"
#include "build/vendor/vendor_sdl.h"

#define SHADERCROSS_SOURCE "./vendor/sdl-shadercross"
#define SHADERCROSS_BUILD  "./vendor/sdl-shadercross/build-linux-x86_64"
#define SHADERCROSS_BINARY SHADERCROSS_BUILD "/shadercross"

/*
 * Where shadercross finds its own shared libraries.
 *
 * It links against libSDL3_shadercross, SPIRV-Cross and DXC, all of which its build tree produces
 * and none of which are installed anywhere the loader looks. The binary carries no RPATH pointing
 * back at them either, so running it without this fails with a bare exit code 127 out of the dynamic
 * loader — no diagnostic, nothing naming the missing library.
 *
 * Linux only, which is all shadercross is built for here; see NYA_VENDORS_ALL.
 * */
#define SHADERCROSS_LIBRARY_PATH                                                                                                                     \
    "LD_LIBRARY_PATH=" SHADERCROSS_BUILD ":" SHADERCROSS_BUILD "/external/SPIRV-Cross:" SHADERCROSS_BUILD                                            \
    "/external/DirectXShaderCompiler/lib:" SDL_BUILD_LINUX_X86_64

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
            .pre_build_hooks = { &hook_invalidate_stale_cmake_cache, &hook_absolutize_cmake_prefix_path, },
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
