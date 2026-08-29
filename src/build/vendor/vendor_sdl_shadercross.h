/**
 * @file vendor_sdl_shadercross.h
 *
 * SDL_shadercross. Produces a tool rather than a library, so it carries no consumer flags: nothing
 * links against it, the shader rules invoke the binary it builds.
 * */
#pragma once

#include "nyangine/nyangine.h"
#include "build/hooks.h"
#include "build/toolchain.h"
#include "build/vendor/vendor_common.h"
#include "build/vendor/vendor_sdl.h"

#define SHADERCROSS_SOURCE "./vendor/sdl-shadercross"

/*
 * Keyed by host, not by target: shadercross is a build-time tool, not something linked or shipped,
 * and its output (.dxil/.msl/.spv) is identical regardless of target, so one binary per machine
 * suffices. It used to be named and built as a Linux target, which is why a Windows host had it
 * compiling a Linux-named cmake tree natively and then trying to run it with LD_LIBRARY_PATH.
 */
#if OS_WINDOWS
#define SHADERCROSS_BUILD    "./vendor/sdl-shadercross/build-windows-x86_64"
#define SHADERCROSS_BINARY   SHADERCROSS_BUILD "/shadercross.exe"
#define SHADERCROSS_HOST_SDL SDL_BUILD_WINDOWS_X86_64
#else
#define SHADERCROSS_BUILD    "./vendor/sdl-shadercross/build-linux-x86_64"
#define SHADERCROSS_BINARY   SHADERCROSS_BUILD "/shadercross"
#define SHADERCROSS_HOST_SDL SDL_BUILD_LINUX_X86_64
#endif

/*
 * Where shadercross finds its own shared libraries (libSDL3_shadercross, SPIRV-Cross, DXC): none are
 * installed anywhere the loader looks and the binary carries no RPATH, so without this it fails with
 * a bare exit code 127 — no diagnostic, nothing naming the missing library.
 *
 * Carries its own trailing comma since the Windows expansion is empty and `{ , }` is not an
 * initialiser (same convention as FLAGS_TARGET_WINDOWS_X86_64).
 *
 * Empty on Windows deliberately: NYA_Command environment entries are _putenv'd into *this* process
 * before CreateProcess rather than into the child (see command_windows.c), so a PATH= here would
 * replace the build tool's own PATH for every later command — cmake, ninja, clang, make included.
 * The Windows loader searches the .exe's own directory first, so the configure below puts every
 * runtime artifact there instead.
 * */
#if OS_WINDOWS
#define SHADERCROSS_LIBRARY_PATH
#else
#define SHADERCROSS_LIBRARY_PATH                                                                                                                     \
    "LD_LIBRARY_PATH=" SHADERCROSS_BUILD ":" SHADERCROSS_BUILD "/external/SPIRV-Cross:" SHADERCROSS_BUILD                                            \
    "/external/DirectXShaderCompiler/lib:" SHADERCROSS_HOST_SDL,
#endif

/*
 * Windows only: forces every DLL in the tree, subprojects included, into the directory
 * shadercross.exe lives in (SDL3_shadercross.dll, spirv-cross-c-shared.dll, dxcompiler.dll), which is
 * what replaces LD_LIBRARY_PATH there. Absolute via %CWD%/hook_expand_cwd, because a relative output
 * directory resolves against each target's own binary directory, not this one.
 */
#if OS_WINDOWS
#define SHADERCROSS_CMAKE_RUNTIME_OUTPUT "-DCMAKE_RUNTIME_OUTPUT_DIRECTORY=%CWD%/vendor/sdl-shadercross/build-windows-x86_64",
#else
#define SHADERCROSS_CMAKE_RUNTIME_OUTPUT
#endif

NYA_VendorRule vendor_sdl_shadercross_host = {
    .name = "sdl-shadercross (host tool)",

    .parts = {
        &(NYA_BuildRule){
            .name        = "vendor_sdl_shadercross_host_configure",
            .policy      = NYA_BUILD_ONCE,
            .output_file = SHADERCROSS_BINARY,

            .command = {
                .program   = "cmake",
                .arguments = {
                    "-S", SHADERCROSS_SOURCE,
                    "-B", SHADERCROSS_BUILD,
                    "-GNinja",
                    "-DSDLSHADERCROSS_VENDORED=ON",
                    SHADERCROSS_CMAKE_RUNTIME_OUTPUT
                    // Point at the SDL we just built, not whatever the host has installed — otherwise
                    // this succeeds on a dev machine with a system SDL3 and fails in CI.
                    "-DCMAKE_PREFIX_PATH=" SHADERCROSS_HOST_SDL,
                },
            },

            // cmake resolves a relative CMAKE_PREFIX_PATH against the build directory, so it must be
            // made absolute first; hook_expand_cwd does the same for the runtime output dir above.
            .pre_build_hooks = { &hook_invalidate_stale_cmake_cache, &hook_absolutize_cmake_prefix_path, &hook_expand_cwd, },
        },
        &(NYA_BuildRule){
            .name        = "vendor_sdl_shadercross_host_compile",
            .policy      = NYA_BUILD_ONCE,
            .output_file = SHADERCROSS_BINARY,

            .command = {
                .program   = "cmake",
                .arguments = {
                    "--build", SHADERCROSS_BUILD,
                    "--config", "Release",
                    "--", "-j", NPROCS,
                },
            },
        },
    },
};
