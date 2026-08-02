/**
 * @file vendor_sdl_mixer.h
 *
 * SDL3_mixer. cmake, static, with its audio codecs vendored so nothing is expected from the host.
 * */
#pragma once

#include "nyangine/nyangine.h"

#include "build/hooks/hooks.h"

// clang-format off

#define SDL_MIXER_SOURCE               "./vendor/sdl-mixer"
#define SDL_MIXER_BUILD_LINUX_X86_64   "./vendor/sdl-mixer/build-linux-x86_64"
#define SDL_MIXER_BUILD_WINDOWS_X86_64 "./vendor/sdl-mixer/build-windows-x86_64"

#define SDL_MIXER_A_LINUX_X86_64   SDL_MIXER_BUILD_LINUX_X86_64 "/libSDL3_mixer.a"
#define SDL_MIXER_A_WINDOWS_X86_64 SDL_MIXER_BUILD_WINDOWS_X86_64 "/libSDL3_mixer.a"

#define SDL_MIXER_CMAKE_COMMON      \
    NYA_CMAKE_STATIC,               \
    "-DSDLMIXER_VENDORED=ON",       \
    "-DSDLMIXER_DEPS_SHARED=OFF",   \
    "-DSDLMIXER_EXAMPLES=OFF",      \
    "-DSDLMIXER_INSTALL=OFF"

// clang-format on

NYA_VendorRule vendor_sdl_mixer_linux_x86_64 = {
    .name = "sdl-mixer (linux-x86_64)",

    .includes     = { "-I./vendor/sdl-mixer/include/", },
    .linker_flags = { SDL_MIXER_A_LINUX_X86_64, },

    .parts = {
        &(NYA_BuildRule){
            .name        = "vendor_sdl_mixer_linux_x86_64_configure",
            .policy      = NYA_BUILD_ONCE,
            .output_file = SDL_MIXER_A_LINUX_X86_64,

            .command = {
                .program   = "cmake",
                .arguments = {
                    "-S", SDL_MIXER_SOURCE,
                    "-B", SDL_MIXER_BUILD_LINUX_X86_64,
                    SDL_MIXER_CMAKE_COMMON,
                    "-DCMAKE_PREFIX_PATH=" SDL_BUILD_LINUX_X86_64,
                },
            },

            .pre_build_hooks = { &hook_absolutize_cmake_prefix_path, },
        },
        &(NYA_BuildRule){
            .name        = "vendor_sdl_mixer_linux_x86_64_compile",
            .policy      = NYA_BUILD_ONCE,
            .output_file = SDL_MIXER_A_LINUX_X86_64,

            .command = {
                .program   = "cmake",
                .arguments = { "--build", SDL_MIXER_BUILD_LINUX_X86_64, "--config", "Release", "--", "-j", NPROCS, },
            },
        },
    },
};

NYA_VendorRule vendor_sdl_mixer_windows_x86_64 = {
    .name = "sdl-mixer (windows-x86_64)",

    .includes     = { "-I./vendor/sdl-mixer/include/", },
    .linker_flags = { SDL_MIXER_A_WINDOWS_X86_64, },

    .parts = {
        &(NYA_BuildRule){
            .name        = "vendor_sdl_mixer_windows_x86_64_configure",
            .policy      = NYA_BUILD_ONCE,
            .output_file = SDL_MIXER_A_WINDOWS_X86_64,

            .command = {
                .program   = "cmake",
                .arguments = {
                    "-S", SDL_MIXER_SOURCE,
                    "-B", SDL_MIXER_BUILD_WINDOWS_X86_64,
                    SDL_MIXER_CMAKE_COMMON,
                    NYA_CMAKE_WINDOWS_TOOLCHAIN,
                    "-DCMAKE_PREFIX_PATH=" SDL_BUILD_WINDOWS_X86_64,
                },
            },

            .pre_build_hooks = { &hook_absolutize_cmake_prefix_path, },
        },
        &(NYA_BuildRule){
            .name        = "vendor_sdl_mixer_windows_x86_64_compile",
            .policy      = NYA_BUILD_ONCE,
            .output_file = SDL_MIXER_A_WINDOWS_X86_64,

            .command = {
                .program   = "cmake",
                .arguments = { "--build", SDL_MIXER_BUILD_WINDOWS_X86_64, "--config", "Release", "--", "-j", NPROCS, },
            },
        },
    },
};
