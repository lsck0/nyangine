/**
 * @file vendor_sdl.h
 *
 * SDL3, built static from the vendored submodule.
 * */
#pragma once

#include "nyangine/nyangine.h"
#include "build/hooks.h"
#include "build/toolchain.h"
#include "build/vendor/vendor_common.h"

// clang-format off

#define SDL_SOURCE               "./vendor/sdl"
#define SDL_BUILD_LINUX_X86_64   "./vendor/sdl/build-linux-x86_64/"
#define SDL_BUILD_WINDOWS_X86_64 "./vendor/sdl/build-windows-x86_64/"

#define SDL_A_LINUX_X86_64   SDL_BUILD_LINUX_X86_64 "libSDL3.a"
#define SDL_A_WINDOWS_X86_64 SDL_BUILD_WINDOWS_X86_64 "libSDL3.a"

#define SDL_INCLUDES_LINUX_X86_64 "-I./vendor/sdl/include/"
#define SDL_LINKER_LINUX_X86_64   "-L" SDL_BUILD_LINUX_X86_64, "-lSDL3"

#define SDL_CMAKE_COMMON                        \
    "-GNinja",                                  \
    "-DCMAKE_BUILD_TYPE=Release",               \
    "-DCMAKE_POSITION_INDEPENDENT_CODE=ON",     \
    "-DSDL_SHARED=OFF",                         \
    "-DSDL_STATIC=ON"

// clang-format on

NYA_VendorRule vendor_sdl_linux_x86_64 = {
    .name = "sdl (linux-x86_64)",

    .includes     = { SDL_INCLUDES_LINUX_X86_64, },
    .linker_flags = { SDL_LINKER_LINUX_X86_64, },

    .parts = {
        &(NYA_BuildRule){
            .name        = "vendor_sdl_linux_x86_64_configure",
            .policy      = NYA_BUILD_ONCE,
            .output_file = SDL_A_LINUX_X86_64,

            .command = {
                .program   = "cmake",
                .arguments = {
                    "-S", SDL_SOURCE,
                    "-B", SDL_BUILD_LINUX_X86_64,
                    SDL_CMAKE_COMMON,
                },
            },

            .pre_build_hooks = { &hook_invalidate_stale_cmake_cache, },
        },
        &(NYA_BuildRule){
            .name        = "vendor_sdl_linux_x86_64_compile",
            .policy      = NYA_BUILD_ONCE,
            .output_file = SDL_A_LINUX_X86_64,

            .command = {
                .program   = "cmake",
                .arguments = {
                    "--build", SDL_BUILD_LINUX_X86_64,
                    "--config", "Release",
                    "--", "-j", NPROCS,
                },
            },
        },
    },
};

NYA_VendorRule vendor_sdl_windows_x86_64 = {
    .name = "sdl (windows-x86_64)",

    .includes = { "-I./vendor/sdl/include/", },

    // The win32 system libraries sit here rather than on the target's own flags because SDL is the
    // only reason the project needs any of them.
    .linker_flags = {
        "-L" SDL_BUILD_WINDOWS_X86_64, "-lSDL3",
        // -lhid is what SDL's HIDAPI game controller backend needs for HidD_*/HidP_*. Without it the
        // link fails only at the very end, on symbols from SDL_hidapi_*.c, which reads like an SDL
        // build problem rather than a missing system library on the link line.
        "-lcomdlg32", "-ldxguid", "-lgdi32", "-lhid", "-limm32", "-lkernel32",
        "-lole32", "-loleaut32", "-lsetupapi", "-luser32", "-luuid",
        "-lversion", "-lwinmm",
    },

    .parts = {
        &(NYA_BuildRule){
            .name        = "vendor_sdl_windows_x86_64_configure",
            .policy      = NYA_BUILD_ONCE,
            .output_file = SDL_A_WINDOWS_X86_64,

            .command = {
                .program   = "cmake",
                .arguments = {
                    "-S", SDL_SOURCE,
                    "-B", SDL_BUILD_WINDOWS_X86_64,
                    SDL_CMAKE_COMMON,
                    /*
                     * The shared macro, like every other cmake vendor here.
                     *
                     * This rule used to spell the toolchain out inline, and it was the only one that
                     * did. Three things came of that. The compiler paths were absolute
                     * (/usr/bin/x86_64-w64-mingw32-gcc), so a mingw-w64 installed anywhere else was
                     * not found. FIND_ROOT_PATH_MODE_INCLUDE was BOTH, which is exactly what the
                     * shared macro's own comment warns against — a find_package that reaches the
                     * host's /usr/include mixes glibc headers into a mingw compile. And because
                     * nothing here expanded NYA_CMAKE_WINDOWS_TOOLCHAIN, a Windows host — where that
                     * macro is native and takes no cross compiler at all — configured SDL against
                     * mingw paths that do not exist on it, so the one dependency everything else
                     * links against could not be built there.
                     *
                     * Verified equivalent before switching: configuring both ways produces a
                     * byte identical include-config-release/build_config/SDL_build_config.h.
                     */
                    NYA_CMAKE_WINDOWS_TOOLCHAIN,
                },
            },

            .pre_build_hooks = { &hook_invalidate_stale_cmake_cache, },
        },
        &(NYA_BuildRule){
            .name        = "vendor_sdl_windows_x86_64_compile",
            .policy      = NYA_BUILD_ONCE,
            .output_file = SDL_A_WINDOWS_X86_64,

            .command = {
                .program   = "cmake",
                .arguments = {
                    "--build", SDL_BUILD_WINDOWS_X86_64,
                    "--config", "Release",
                    "--", "-j", NPROCS,
                },
            },
        },
    },
};
