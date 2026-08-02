/**
 * @file vendor_sdl_net.h
 *
 * SDL3_net. cmake, static. No vendored dependencies of its own, it sits on the platform sockets.
 * */
#pragma once

#include "nyangine/nyangine.h"

#include "build/hooks/hooks.h"

// clang-format off

#define SDL_NET_SOURCE               "./vendor/sdl-net"
#define SDL_NET_BUILD_LINUX_X86_64   "./vendor/sdl-net/build-linux-x86_64"
#define SDL_NET_BUILD_WINDOWS_X86_64 "./vendor/sdl-net/build-windows-x86_64"

#define SDL_NET_A_LINUX_X86_64   SDL_NET_BUILD_LINUX_X86_64 "/libSDL3_net.a"
#define SDL_NET_A_WINDOWS_X86_64 SDL_NET_BUILD_WINDOWS_X86_64 "/libSDL3_net.a"

#define SDL_NET_CMAKE_COMMON    \
    NYA_CMAKE_STATIC,           \
    "-DSDLNET_SAMPLES=OFF",     \
    "-DSDLNET_INSTALL=OFF"

// clang-format on

NYA_VendorRule vendor_sdl_net_linux_x86_64 = {
    .name = "sdl-net (linux-x86_64)",

    .includes     = { "-I./vendor/sdl-net/include/", },
    .linker_flags = { SDL_NET_A_LINUX_X86_64, },

    .parts = {
        &(NYA_BuildRule){
            .name        = "vendor_sdl_net_linux_x86_64_configure",
            .policy      = NYA_BUILD_ONCE,
            .output_file = SDL_NET_A_LINUX_X86_64,

            .command = {
                .program   = "cmake",
                .arguments = {
                    "-S", SDL_NET_SOURCE,
                    "-B", SDL_NET_BUILD_LINUX_X86_64,
                    SDL_NET_CMAKE_COMMON,
                    "-DCMAKE_PREFIX_PATH=" SDL_BUILD_LINUX_X86_64,
                },
            },

            .pre_build_hooks = { &hook_absolutize_cmake_prefix_path, },
        },
        &(NYA_BuildRule){
            .name        = "vendor_sdl_net_linux_x86_64_compile",
            .policy      = NYA_BUILD_ONCE,
            .output_file = SDL_NET_A_LINUX_X86_64,

            .command = {
                .program   = "cmake",
                .arguments = { "--build", SDL_NET_BUILD_LINUX_X86_64, "--config", "Release", "--", "-j", NPROCS, },
            },
        },
    },
};

NYA_VendorRule vendor_sdl_net_windows_x86_64 = {
    .name = "sdl-net (windows-x86_64)",

    .includes = { "-I./vendor/sdl-net/include/", },

    // ws2_32 is a system DLL, so linking it dynamically costs nothing in portability.
    .linker_flags = { SDL_NET_A_WINDOWS_X86_64, "-lws2_32", "-liphlpapi", },

    .parts = {
        &(NYA_BuildRule){
            .name        = "vendor_sdl_net_windows_x86_64_configure",
            .policy      = NYA_BUILD_ONCE,
            .output_file = SDL_NET_A_WINDOWS_X86_64,

            .command = {
                .program   = "cmake",
                .arguments = {
                    "-S", SDL_NET_SOURCE,
                    "-B", SDL_NET_BUILD_WINDOWS_X86_64,
                    SDL_NET_CMAKE_COMMON,
                    NYA_CMAKE_WINDOWS_TOOLCHAIN,
                    "-DCMAKE_PREFIX_PATH=" SDL_BUILD_WINDOWS_X86_64,
                },
            },

            .pre_build_hooks = { &hook_absolutize_cmake_prefix_path, },
        },
        &(NYA_BuildRule){
            .name        = "vendor_sdl_net_windows_x86_64_compile",
            .policy      = NYA_BUILD_ONCE,
            .output_file = SDL_NET_A_WINDOWS_X86_64,

            .command = {
                .program   = "cmake",
                .arguments = { "--build", SDL_NET_BUILD_WINDOWS_X86_64, "--config", "Release", "--", "-j", NPROCS, },
            },
        },
    },
};
