/**
 * @file vendor_sdl_ttf.h
 *
 * SDL3_ttf. cmake, static, with freetype and harfbuzz vendored rather than taken from the host.
 * */
#pragma once

#include "nyangine/nyangine.h"
#include "build/hooks.h"
#include "build/toolchain.h"
#include "build/vendor/vendor_common.h"
// For SDL_BUILD_LINUX_X86_64 and SDL_BUILD_WINDOWS_X86_64: these link against the SDL built beside them.
#include "build/vendor/vendor_sdl.h"

// clang-format off

#define SDL_TTF_SOURCE               "./vendor/sdl-ttf"
#define SDL_TTF_BUILD_LINUX_X86_64   "./vendor/sdl-ttf/build-linux-x86_64"
#define SDL_TTF_BUILD_WINDOWS_X86_64 "./vendor/sdl-ttf/build-windows-x86_64"

#define SDL_TTF_A_LINUX_X86_64   SDL_TTF_BUILD_LINUX_X86_64 "/libSDL3_ttf.a"
#define SDL_TTF_A_WINDOWS_X86_64 SDL_TTF_BUILD_WINDOWS_X86_64 "/libSDL3_ttf.a"

#define SDL_TTF_CMAKE_COMMON    \
    NYA_CMAKE_STATIC,           \
    "-DSDLTTF_VENDORED=ON",     \
    "-DSDLTTF_HARFBUZZ=ON",     \
    "-DSDLTTF_SAMPLES=OFF",     \
    "-DSDLTTF_INSTALL=OFF"

// clang-format on

NYA_VendorRule vendor_sdl_ttf_linux_x86_64 = {
    .name = "sdl-ttf (linux-x86_64)",

    .includes     = { "-I./vendor/sdl-ttf/include/", },
    // The codecs each library vendors are separate archives, and a static link needs every one of
    // them. Order matters: a dependency must follow whatever refers to it.
    .linker_flags = {
        // These vendor C++ codecs (harfbuzz, libgme), so the C++ runtime has to come along.
        "-lstdc++",
        SDL_TTF_A_LINUX_X86_64,
        SDL_TTF_BUILD_LINUX_X86_64 "/external/harfbuzz-build/libharfbuzz.a",
        SDL_TTF_BUILD_LINUX_X86_64 "/external/freetype-build/libfreetype.a",
        SDL_TTF_BUILD_LINUX_X86_64 "/external/plutosvg-guild/libplutosvg.a",
        SDL_TTF_BUILD_LINUX_X86_64 "/external/plutovg-build/libplutovg.a",
    },

    .parts = {
        &(NYA_BuildRule){
            .name        = "vendor_sdl_ttf_linux_x86_64_configure",
            .policy      = NYA_BUILD_ONCE,
            .output_file = SDL_TTF_A_LINUX_X86_64,

            .command = {
                .program   = "cmake",
                .arguments = {
                    "-S", SDL_TTF_SOURCE,
                    "-B", SDL_TTF_BUILD_LINUX_X86_64,
                    SDL_TTF_CMAKE_COMMON,
                    "-DCMAKE_PREFIX_PATH=" SDL_BUILD_LINUX_X86_64,
                },
            },

            .pre_build_hooks = { &hook_invalidate_stale_cmake_cache, &hook_absolutize_cmake_prefix_path, },
        },
        &(NYA_BuildRule){
            .name        = "vendor_sdl_ttf_linux_x86_64_compile",
            .policy      = NYA_BUILD_ONCE,
            .output_file = SDL_TTF_A_LINUX_X86_64,

            .command = {
                .program   = "cmake",
                .arguments = { "--build", SDL_TTF_BUILD_LINUX_X86_64, "--config", "Release", "--", "-j", NPROCS, },
            },
        },
    },
};

NYA_VendorRule vendor_sdl_ttf_windows_x86_64 = {
    .name = "sdl-ttf (windows-x86_64)",

    .includes     = { "-I./vendor/sdl-ttf/include/", },
    .linker_flags = {
        "-lstdc++",
        // freetype and harfbuzz reach for the win32 path and uuid helpers on this target.
        "-lshlwapi", "-lrpcrt4",
        SDL_TTF_A_WINDOWS_X86_64,
        SDL_TTF_BUILD_WINDOWS_X86_64 "/external/harfbuzz-build/libharfbuzz.a",
        SDL_TTF_BUILD_WINDOWS_X86_64 "/external/freetype-build/libfreetype.a",
        SDL_TTF_BUILD_WINDOWS_X86_64 "/external/plutosvg-guild/libplutosvg.a",
        SDL_TTF_BUILD_WINDOWS_X86_64 "/external/plutovg-build/libplutovg.a",
    },

    .parts = {
        &(NYA_BuildRule){
            .name        = "vendor_sdl_ttf_windows_x86_64_configure",
            .policy      = NYA_BUILD_ONCE,
            .output_file = SDL_TTF_A_WINDOWS_X86_64,

            .command = {
                .program   = "cmake",
                .arguments = {
                    "-S", SDL_TTF_SOURCE,
                    "-B", SDL_TTF_BUILD_WINDOWS_X86_64,
                    SDL_TTF_CMAKE_COMMON,
                    NYA_CMAKE_WINDOWS_TOOLCHAIN,
                    "-DCMAKE_PREFIX_PATH=" SDL_BUILD_WINDOWS_X86_64,
                },
            },

            .pre_build_hooks = { &hook_invalidate_stale_cmake_cache, &hook_absolutize_cmake_prefix_path, },
        },
        &(NYA_BuildRule){
            .name        = "vendor_sdl_ttf_windows_x86_64_compile",
            .policy      = NYA_BUILD_ONCE,
            .output_file = SDL_TTF_A_WINDOWS_X86_64,

            .command = {
                .program   = "cmake",
                .arguments = { "--build", SDL_TTF_BUILD_WINDOWS_X86_64, "--config", "Release", "--", "-j", NPROCS, },
            },
        },
    },
};
