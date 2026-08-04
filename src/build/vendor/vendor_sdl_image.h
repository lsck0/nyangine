/**
 * @file vendor_sdl_image.h
 *
 * SDL3_image. cmake, static, with its image codecs vendored so nothing is expected from the host.
 * */
#pragma once

#include "nyangine/nyangine.h"

#include "build/hooks/hooks.h"
#include "build/toolchain.h"
#include "build/vendor/vendor_common.h"

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
    "-DSDLIMAGE_INSTALL=OFF",       \
    /*                                                                                             \
     * PNG, JPEG and WebP cover what a game ships. The rest are switched off deliberately:          \
     *                                                                                             \
     * TIFF drags in libjbig, which is not vendored and exists on most systems only as a shared     \
     * library, so a static link fails on symbols no game asked for. AVIF drags in aom and dav1d,   \
     * which are enormous, need nasm to assemble, and decode a format nothing here loads.           \
     */                                                                                            \
    "-DSDLIMAGE_TIF=OFF",           \
    "-DSDLIMAGE_AVIF=OFF",          \
    "-DSDLIMAGE_JXL=OFF"

// clang-format on

NYA_VendorRule vendor_sdl_image_linux_x86_64 = {
    .name = "sdl-image (linux-x86_64)",

    .includes     = { "-I./vendor/sdl-image/include/", },
    // The codecs each library vendors are separate archives, and a static link needs every one of
    // them. Order matters: a dependency must follow whatever refers to it.
    .linker_flags = {
        SDL_IMAGE_A_LINUX_X86_64,
        SDL_IMAGE_BUILD_LINUX_X86_64 "/external/libpng-build/libpng16.a",
        SDL_IMAGE_BUILD_LINUX_X86_64 "/external/zlib-build/libz.a",
        SDL_IMAGE_BUILD_LINUX_X86_64 "/external/libwebp-build/libwebpdemux.a",
        SDL_IMAGE_BUILD_LINUX_X86_64 "/external/libwebp-build/libwebpmux.a",
        SDL_IMAGE_BUILD_LINUX_X86_64 "/external/libwebp-build/libwebp.a",
        SDL_IMAGE_BUILD_LINUX_X86_64 "/external/libwebp-build/libsharpyuv.a",
    },

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

            .pre_build_hooks = { &hook_invalidate_stale_cmake_cache, &hook_absolutize_cmake_prefix_path, },
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
    .linker_flags = {
        SDL_IMAGE_A_WINDOWS_X86_64,
        SDL_IMAGE_BUILD_WINDOWS_X86_64 "/external/libpng-build/libpng16.a",
        // mingw builds zlib as zlibstatic rather than z.
        SDL_IMAGE_BUILD_WINDOWS_X86_64 "/external/zlib-build/libzlibstatic.a",
        SDL_IMAGE_BUILD_WINDOWS_X86_64 "/external/libwebp-build/libwebpdemux.a",
        SDL_IMAGE_BUILD_WINDOWS_X86_64 "/external/libwebp-build/libwebpmux.a",
        SDL_IMAGE_BUILD_WINDOWS_X86_64 "/external/libwebp-build/libwebp.a",
        SDL_IMAGE_BUILD_WINDOWS_X86_64 "/external/libwebp-build/libsharpyuv.a",
    },

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

            .pre_build_hooks = { &hook_invalidate_stale_cmake_cache, &hook_absolutize_cmake_prefix_path, },
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
