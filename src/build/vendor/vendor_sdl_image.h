/**
 * @file vendor_sdl_image.h
 *
 * SDL3_image. cmake, static, with its image codecs vendored so nothing is expected from the host.
 * */
#pragma once

#include "nyangine/nyangine.h"

#include "build/hooks/hooks.h"

// clang-format off

#define SDL_IMAGE_SOURCE               "./vendor/sdl-image"
#define SDL_IMAGE_BUILD_LINUX_X86_64   "./vendor/sdl-image/build-linux-x86_64"
#define SDL_IMAGE_BUILD_WINDOWS_X86_64 "./vendor/sdl-image/build-windows-x86_64"

#define SDL_IMAGE_A_LINUX_X86_64   SDL_IMAGE_BUILD_LINUX_X86_64 "/libSDL3_image.a"
#define SDL_IMAGE_A_WINDOWS_X86_64 SDL_IMAGE_BUILD_WINDOWS_X86_64 "/libSDL3_image.a"

#define SDL_IMAGE_CMAKE_COMMON      \
    NYA_CMAKE_STATIC,               \
    "-DSDLIMAGE_VENDORED=ON",       \
    "-DSDLIMAGE_DEPS_SHARED=OFF",   \
    "-DSDLIMAGE_SAMPLES=OFF",       \
    "-DSDLIMAGE_INSTALL=OFF"

// clang-format on

NYA_VendorRule vendor_sdl_image_linux_x86_64 = {
    .name = "sdl-image (linux-x86_64)",

    .includes     = { "-I./vendor/sdl-image/include/", },
    .linker_flags = { SDL_IMAGE_A_LINUX_X86_64, },

    .parts = {
        &(NYA_BuildRule){
            .name        = "vendor_sdl_image_linux_x86_64_configure",
            .policy      = NYA_BUILD_ONCE,
            .output_file = SDL_IMAGE_A_LINUX_X86_64,

            .command = {
                .program   = "cmake",
                .arguments = {
                    "-S", SDL_IMAGE_SOURCE,
                    "-B", SDL_IMAGE_BUILD_LINUX_X86_64,
                    SDL_IMAGE_CMAKE_COMMON,
                    "-DCMAKE_PREFIX_PATH=" SDL_BUILD_LINUX_X86_64,
                },
            },

            .pre_build_hooks = { &hook_absolutize_cmake_prefix_path, },
        },
        &(NYA_BuildRule){
            .name        = "vendor_sdl_image_linux_x86_64_compile",
            .policy      = NYA_BUILD_ONCE,
            .output_file = SDL_IMAGE_A_LINUX_X86_64,

            .command = {
                .program   = "cmake",
                .arguments = { "--build", SDL_IMAGE_BUILD_LINUX_X86_64, "--config", "Release", "--", "-j", NPROCS, },
            },
        },
    },
};

NYA_VendorRule vendor_sdl_image_windows_x86_64 = {
    .name = "sdl-image (windows-x86_64)",

    .includes     = { "-I./vendor/sdl-image/include/", },
    .linker_flags = { SDL_IMAGE_A_WINDOWS_X86_64, },

    .parts = {
        &(NYA_BuildRule){
            .name        = "vendor_sdl_image_windows_x86_64_configure",
            .policy      = NYA_BUILD_ONCE,
            .output_file = SDL_IMAGE_A_WINDOWS_X86_64,

            .command = {
                .program   = "cmake",
                .arguments = {
                    "-S", SDL_IMAGE_SOURCE,
                    "-B", SDL_IMAGE_BUILD_WINDOWS_X86_64,
                    SDL_IMAGE_CMAKE_COMMON,
                    NYA_CMAKE_WINDOWS_TOOLCHAIN,
                    "-DCMAKE_PREFIX_PATH=" SDL_BUILD_WINDOWS_X86_64,
                },
            },

            .pre_build_hooks = { &hook_absolutize_cmake_prefix_path, },
        },
        &(NYA_BuildRule){
            .name        = "vendor_sdl_image_windows_x86_64_compile",
            .policy      = NYA_BUILD_ONCE,
            .output_file = SDL_IMAGE_A_WINDOWS_X86_64,

            .command = {
                .program   = "cmake",
                .arguments = { "--build", SDL_IMAGE_BUILD_WINDOWS_X86_64, "--config", "Release", "--", "-j", NPROCS, },
            },
        },
    },
};
