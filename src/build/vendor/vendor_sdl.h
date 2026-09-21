/**
 * @file vendor_sdl.h
 *
 * SDL3, built static from the vendored submodule.
 * */
#pragma once

#include "nyangine/nyangine.h"
#include "build/hooks.h"
#include "build/flags.h"
#include "build/vendor/vendor_common.h"

// clang-format off

#define SDL_SOURCE               "./vendor/sdl"
#define SDL_BUILD_LINUX_X86_64   "./vendor/sdl/build-linux-x86_64/"
#define SDL_BUILD_WINDOWS_X86_64 "./vendor/sdl/build-windows-x86_64/"

#define SDL_A_LINUX_X86_64   SDL_BUILD_LINUX_X86_64 "libSDL3.a"
#define SDL_A_WINDOWS_X86_64 SDL_BUILD_WINDOWS_X86_64 "libSDL3.a"

/*
 * What the IF_OUTDATED rules below compare this recipe against, written by hook_stamp_output_file.
 *
 * Not the archive: cmake and ninja leave it alone when no source changed, so a no-op build left the
 * archive older than the recipe that triggered it and the rule was outdated again immediately —
 * firing on every `./build` invocation for the rest of the checkout's life, `./build stats`
 * included, since main walks the vendor rules before dispatching any subcommand.
 */
#define SDL_CONFIGURED_LINUX_X86_64   SDL_BUILD_LINUX_X86_64 "nya_configured.stamp"
#define SDL_CONFIGURED_WINDOWS_X86_64 SDL_BUILD_WINDOWS_X86_64 "nya_configured.stamp"
#define SDL_COMPILED_LINUX_X86_64     SDL_BUILD_LINUX_X86_64 "nya_compiled.stamp"
#define SDL_COMPILED_WINDOWS_X86_64   SDL_BUILD_WINDOWS_X86_64 "nya_compiled.stamp"

#define SDL_INCLUDES_LINUX_X86_64 "-I./vendor/sdl/include/"
#define SDL_LINKER_LINUX_X86_64   "-L" SDL_BUILD_LINUX_X86_64, "-lSDL3"

/*
 * This file, as an input the built library depends on.
 *
 * A vendor rule is NYA_BUILD_ONCE keyed on its own archive, so changing a cmake option here used to
 * change nothing at all: the archive was still there, the rule was skipped, and the option quietly
 * applied to nobody who had built once already. Turning SDL_RENDER on is exactly that kind of change,
 * and one that does nothing until a rebuild is not a change. NYA_BUILD_IF_OUTDATED against this file
 * is the dependency that was always there and was never written down.
 *
 * It costs about 160 ms on a build where this file is the newer of the two, since cmake and ninja are
 * then asked and both answer "nothing to do". That is the price of asking the tool rather than
 * guessing from the archive's existence, and it is the right way round.
 *
 * Only SDL carries it, because only SDL's options moved. Every other vendor header has the same gap;
 * see TODO.md.
 */
#define SDL_OPTIONS_FILE "./src/build/vendor/vendor_sdl.h"

/*
 * Off is what the engine never calls: OpenGL, camera, haptic, dialogs, tray, notifications, OpenXR and
 * io_uring. Video is wayland and x11 (see nya_app_init_with_options), so no KMSDRM; offscreen and dummy
 * stay for headless runs. Audio keeps pipewire, pulse and alsa.
 *
 * SDL_RENDER is on for exactly one caller: the crash window, which draws through SDL_Renderer because
 * it has to come up after the GPU device it would otherwise draw with is gone. It was off here until a
 * test opened the window and found SDL_CreateWindowAndRenderer answering "SDL not built with rendering
 * support" in every build ever shipped, which meant every crash fell through to the plain message box.
 * Everything else draws through SDL_GPU; see tests/nyangine/debug/test_crash_report.c.
 *
 * Lean and mean drops the software blitters, RLE and YUV. SDL_HAVE_BLIT_N keeps the fast format
 * conversion that decoded images and glyphs go through on their way to RGBA32.
 *
 * SDL_DYNAPI stays on, so a Steam runtime can still swap in a newer SDL. It references every public
 * function, which is why most of SDL survives --gc-sections and only options shrink it.
 */
#define SDL_CMAKE_COMMON                        \
    "-GNinja",                                  \
    "-DCMAKE_BUILD_TYPE=Release",               \
    NYA_CMAKE_OPTIMIZE,                         \
    "-DCMAKE_POSITION_INDEPENDENT_CODE=ON",     \
    "-DSDL_SHARED=OFF",                         \
    "-DSDL_STATIC=ON",                          \
    "-DSDL_TEST_LIBRARY=OFF",                   \
    "-DSDL_TESTS=OFF",                          \
    "-DSDL_INSTALL=OFF",                        \
    "-DSDL_RENDER=ON",                          \
    "-DSDL_OPENGL=OFF",                         \
    "-DSDL_OPENGLES=OFF",                       \
    "-DSDL_CAMERA=OFF",                         \
    "-DSDL_HAPTIC=OFF",                         \
    "-DSDL_DIALOG=OFF",                         \
    "-DSDL_TRAY=OFF",                           \
    "-DSDL_NOTIFICATION=OFF",                   \
    "-DSDL_GPU_OPENXR=OFF",                     \
    "-DSDL_LIBURING=OFF",                       \
    "-DSDL_KMSDRM=OFF",                         \
    "-DSDL_JACK=OFF",                           \
    "-DSDL_SNDIO=OFF",                          \
    "-DSDL_DISKAUDIO=OFF",                      \
    "-DSDL_LEAN_AND_MEAN=ON",                   \
    "-DCMAKE_C_FLAGS=-DSDL_HAVE_BLIT_N=1"

// clang-format on

NYA_VendorRule vendor_sdl_linux_x86_64 = {
    .name = "sdl (linux-x86_64)",

    .includes     = { SDL_INCLUDES_LINUX_X86_64, },
    .linker_flags = { SDL_LINKER_LINUX_X86_64, },

    .parts = {
        &(NYA_BuildRule){
            .name        = "vendor_sdl_linux_x86_64_configure",
            .policy      = NYA_BUILD_IF_OUTDATED,
            .input_file  = SDL_OPTIONS_FILE,
            .output_file = SDL_CONFIGURED_LINUX_X86_64,

            .command = {
                .program   = "cmake",
                .arguments = {
                    "-S", SDL_SOURCE,
                    "-B", SDL_BUILD_LINUX_X86_64,
                    SDL_CMAKE_COMMON,
                },
            },

            .pre_build_hooks  = { &hook_invalidate_stale_cmake_cache, },
            .post_build_hooks = { &hook_stamp_output_file, },
        },
        &(NYA_BuildRule){
            .name        = "vendor_sdl_linux_x86_64_compile",
            .policy      = NYA_BUILD_IF_OUTDATED,
            .input_file  = SDL_OPTIONS_FILE,
            .output_file = SDL_COMPILED_LINUX_X86_64,

            .command = {
                .program   = "cmake",
                .arguments = {
                    "--build", SDL_BUILD_LINUX_X86_64,
                    "--config", "Release",
                    "--", "-j", NPROCS,
                },
            },

            .post_build_hooks = { &hook_stamp_output_file, },
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
            .policy      = NYA_BUILD_IF_OUTDATED,
            .input_file  = SDL_OPTIONS_FILE,
            .output_file = SDL_CONFIGURED_WINDOWS_X86_64,

            .command = {
                .program   = "cmake",
                .arguments = {
                    "-S", SDL_SOURCE,
                    "-B", SDL_BUILD_WINDOWS_X86_64,
                    SDL_CMAKE_COMMON,
                    /*
                     * The shared cmake macro, like every other vendor. It finds mingw-w64 on PATH, keeps
                     * FIND_ROOT_PATH_MODE_INCLUDE from reaching the host's glibc headers, and expands natively on a
                     * Windows host.
                     */
                    NYA_CMAKE_WINDOWS_TOOLCHAIN,
                },
            },

            .pre_build_hooks  = { &hook_invalidate_stale_cmake_cache, },
            .post_build_hooks = { &hook_stamp_output_file, },
        },
        &(NYA_BuildRule){
            .name        = "vendor_sdl_windows_x86_64_compile",
            .policy      = NYA_BUILD_IF_OUTDATED,
            .input_file  = SDL_OPTIONS_FILE,
            .output_file = SDL_COMPILED_WINDOWS_X86_64,

            .command = {
                .program   = "cmake",
                .arguments = {
                    "--build", SDL_BUILD_WINDOWS_X86_64,
                    "--config", "Release",
                    "--", "-j", NPROCS,
                },
            },

            .post_build_hooks = { &hook_stamp_output_file, },
        },
    },
};
