/**
 * @file vendor_sdl_mixer.h
 *
 * SDL3_mixer. cmake, static, with its audio codecs vendored so nothing is expected from the host.
 * */
#pragma once

#include "nyangine/nyangine.h"
#include "build/hooks.h"
#include "build/toolchain.h"
#include "build/vendor/vendor_common.h"
// For SDL_BUILD_LINUX_X86_64 and SDL_BUILD_WINDOWS_X86_64: these link against the SDL built beside them.
#include "build/vendor/vendor_sdl.h"

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
    // The codecs each library vendors are separate archives, and a static link needs every one of
    // them. Order matters: a dependency must follow whatever refers to it.
    .linker_flags = {
        // These vendor C++ codecs (harfbuzz, libgme), so the C++ runtime has to come along.
        "-lstdc++",
        SDL_MIXER_A_LINUX_X86_64,
        SDL_MIXER_BUILD_LINUX_X86_64 "/external/opusfile-build/libopusfile.a",
        SDL_MIXER_BUILD_LINUX_X86_64 "/external/opus-build/libopus.a",
        SDL_MIXER_BUILD_LINUX_X86_64 "/external/vorbis-build/lib/libvorbisfile.a",
        SDL_MIXER_BUILD_LINUX_X86_64 "/external/vorbis-build/lib/libvorbis.a",
        SDL_MIXER_BUILD_LINUX_X86_64 "/external/ogg-build/libogg.a",
        SDL_MIXER_BUILD_LINUX_X86_64 "/external/flac-build/src/libFLAC/libFLAC.a",
        SDL_MIXER_BUILD_LINUX_X86_64 "/external/libmpg123-build/ports/cmake/src/libmpg123/libmpg123.a",
        SDL_MIXER_BUILD_LINUX_X86_64 "/external/wavpack-build/libwavpack.a",
        SDL_MIXER_BUILD_LINUX_X86_64 "/external/libxmp-build/libxmp.a",
        SDL_MIXER_BUILD_LINUX_X86_64 "/external/libgme-build/gme/libgme.a",
    },

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

            .pre_build_hooks = { &hook_invalidate_stale_cmake_cache, &hook_absolutize_cmake_prefix_path, },
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
    .linker_flags = {
        "-lstdc++",
        SDL_MIXER_A_WINDOWS_X86_64,
        SDL_MIXER_BUILD_WINDOWS_X86_64 "/external/opusfile-build/libopusfile.a",
        SDL_MIXER_BUILD_WINDOWS_X86_64 "/external/opus-build/libopus.a",
        SDL_MIXER_BUILD_WINDOWS_X86_64 "/external/vorbis-build/lib/libvorbisfile.a",
        SDL_MIXER_BUILD_WINDOWS_X86_64 "/external/vorbis-build/lib/libvorbis.a",
        SDL_MIXER_BUILD_WINDOWS_X86_64 "/external/ogg-build/libogg.a",
        SDL_MIXER_BUILD_WINDOWS_X86_64 "/external/flac-build/src/libFLAC/libFLAC.a",
        SDL_MIXER_BUILD_WINDOWS_X86_64 "/external/libmpg123-build/ports/cmake/src/libmpg123/libmpg123.a",
        SDL_MIXER_BUILD_WINDOWS_X86_64 "/external/wavpack-build/libwavpack.a",
        SDL_MIXER_BUILD_WINDOWS_X86_64 "/external/libxmp-build/libxmp.a",
        SDL_MIXER_BUILD_WINDOWS_X86_64 "/external/libgme-build/gme/libgme.a",
    },

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

            .pre_build_hooks = { &hook_invalidate_stale_cmake_cache, &hook_absolutize_cmake_prefix_path, },
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
