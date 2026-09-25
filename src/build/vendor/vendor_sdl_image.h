/**
 * @file vendor_sdl_image.h
 *
 * SDL3_image. cmake, static, decoding with the stb_image and nanosvg it carries, so nothing is expected from the host.
 * */
#pragma once

#include "nyangine-core/nyangine.h"
#include "build/hooks.h"
#include "build/flags.h"
#include "build/vendor/vendor_common.h"
// For SDL_BUILD_LINUX_X86_64 and SDL_BUILD_WINDOWS_X86_64: these link against the SDL built beside them.
#include "build/vendor/vendor_sdl.h"

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
    /* \ What a game ships: PNG, JPEG, BMP, GIF, TGA, QOI and SVG, which the icons and sized vector    \ loads use. All of them decode with stb_image or code inside SDL_image, so no codec library    \ is linked; libpng would only add APNG. Nothing here saves an image.                           \ \ WebP was 0.75 MB and nothing loads it; turn it back on here if a game does. TIFF drags in     \ libjbig, which is not vendored and exists on most systems only as a shared library. AVIF      \ drags in aom and dav1d, which are enormous and need nasm. The rest are formats of old paint   \ programs.                                                                                     \ */                                                                                            \
    "-DSDLIMAGE_BACKEND_STB=ON",    \
    "-DSDLIMAGE_PNG_LIBPNG=OFF",    \
    "-DSDLIMAGE_WEBP=OFF",          \
    "-DSDLIMAGE_TIF=OFF",           \
    "-DSDLIMAGE_AVIF=OFF",          \
    "-DSDLIMAGE_JXL=OFF",           \
    "-DSDLIMAGE_ANI=OFF",           \
    "-DSDLIMAGE_LBM=OFF",           \
    "-DSDLIMAGE_PCX=OFF",           \
    "-DSDLIMAGE_PNM=OFF",           \
    "-DSDLIMAGE_XCF=OFF",           \
    "-DSDLIMAGE_XPM=OFF",           \
    "-DSDLIMAGE_XV=OFF",            \
    "-DSDLIMAGE_BMP_SAVE=OFF",      \
    "-DSDLIMAGE_GIF_SAVE=OFF",      \
    "-DSDLIMAGE_JPG_SAVE=OFF",      \
    "-DSDLIMAGE_PNG_SAVE=OFF",      \
    "-DSDLIMAGE_TGA_SAVE=OFF"

// clang-format on

NYA_VendorRule vendor_sdl_image_linux_x86_64 = {
    .options_file  = "./src/build/vendor/vendor_sdl_image.h",
    .options_stamp = SDL_IMAGE_BUILD_LINUX_X86_64 "/nya_options.stamp",

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

            .pre_build_hooks = { &hook_absolutize_cmake_prefix_path, &hook_invalidate_stale_cmake_cache, },
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
    .options_file  = "./src/build/vendor/vendor_sdl_image.h",
    .options_stamp = SDL_IMAGE_BUILD_WINDOWS_X86_64 "/nya_options.stamp",

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

            .pre_build_hooks = { &hook_absolutize_cmake_prefix_path, &hook_invalidate_stale_cmake_cache, },
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
