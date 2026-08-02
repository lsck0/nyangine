/**
 * @file vendor_sdl.h
 *
 * SDL3, built static from the vendored submodule.
 * */
#pragma once

#include "nyangine/nyangine.h"

// clang-format off

#define SDL_SOURCE               "./vendor/sdl"
#define SDL_BUILD_LINUX_X86_64   "./vendor/sdl/build-linux-x86_64/"
#define SDL_BUILD_WINDOWS_X86_64 "./vendor/sdl/build-window-x86_64/"

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
                    "-DCMAKE_SYSTEM_NAME=Windows",
                    "-DCMAKE_C_COMPILER=/usr/bin/x86_64-w64-mingw32-gcc",
                    "-DCMAKE_CXX_COMPILER=/usr/bin/x86_64-w64-mingw32-g++",
                    "-DCMAKE_LINKER=/usr/bin/x86_64-w64-mingw32-ld",
                    "-DCMAKE_RC_COMPILER=/usr/bin/x86_64-w64-mingw32-windres",
                    "-DCMAKE_FIND_ROOT_PATH=/usr/x86_64-w64-mingw32",
                    "-DCMAKE_FIND_ROOT_PATH_MODE_INCLUDE=BOTH",
                    "-DCMAKE_FIND_ROOT_PATH_MODE_LIBRARY=ONLY",
                    "-DCMAKE_FIND_ROOT_PATH_MODE_PROGRAM=BOTH",
                },
            },
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
