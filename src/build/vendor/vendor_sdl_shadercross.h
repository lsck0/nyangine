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
 * Keyed by host, not by target, because shadercross is neither linked into anything nor shipped: it
 * is a tool that runs on the machine doing the build and turns .hlsl into .dxil, .msl and .spv. The
 * shaders it emits are identical whatever the build is targeting, so there is exactly one of these
 * per machine and it has to be a binary that machine can execute.
 *
 * It used to be named and built as a Linux target, which is why a Windows host had it compiling a
 * Linux-named cmake tree natively and then trying to run it with LD_LIBRARY_PATH.
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
 * Where shadercross finds its own shared libraries.
 *
 * It links against libSDL3_shadercross, SPIRV-Cross and DXC, all of which its build tree produces
 * and none of which are installed anywhere the loader looks. The binary carries no RPATH pointing
 * back at them either, so running it without this fails with a bare exit code 127 out of the dynamic
 * loader — no diagnostic, nothing naming the missing library.
 *
 * Carries its own trailing comma, because the Windows expansion is empty and `{ , }` is not an
 * initialiser. Same convention as FLAGS_TARGET_WINDOWS_X86_64.
 *
 * Nothing on Windows, deliberately, and not because Windows needs no help finding DLLs. A PATH=
 * entry here would be actively harmful: NYA_Command environment entries are _putenv'd into *this*
 * process before CreateProcess rather than into the child (see command_windows.c), so setting PATH
 * for shadercross would replace the build tool's own PATH for every command that runs after it —
 * cmake, ninja, clang and make included. The Windows loader searches the directory holding the .exe
 * first, so the configure below puts every runtime artifact there instead.
 * */
#if OS_WINDOWS
#define SHADERCROSS_LIBRARY_PATH
#else
#define SHADERCROSS_LIBRARY_PATH                                                                                                                     \
    "LD_LIBRARY_PATH=" SHADERCROSS_BUILD ":" SHADERCROSS_BUILD "/external/SPIRV-Cross:" SHADERCROSS_BUILD                                            \
    "/external/DirectXShaderCompiler/lib:" SHADERCROSS_HOST_SDL,
#endif

/*
 * Windows only: force every executable and DLL in the tree, subprojects included, into the one
 * directory shadercross.exe lives in.
 *
 * On Windows a shared library is a RUNTIME artifact, so this catches SDL3_shadercross.dll,
 * spirv-cross-c-shared.dll and dxcompiler.dll alike and puts them where the loader looks first.
 * That is what replaces LD_LIBRARY_PATH, and it also means the build never has to know which
 * subdirectory each subproject would otherwise have chosen.
 *
 * Absolute via %CWD%, expanded by hook_expand_cwd: a relative output directory is resolved against
 * each target's own binary directory, which for a subproject is not this one.
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
                    // Point at the SDL we just built rather than whatever the host happens to have
                    // installed. Without this the configure succeeds on a developer machine with a
                    // system SDL3 and fails on a clean checkout or in CI, which is exactly the kind
                    // of difference that only shows up once it is someone else's problem.
                    //
                    // The host's SDL, since this tool runs here: on Windows that is the same build
                    // the game links against, because a Windows host only ever targets Windows.
                    "-DCMAKE_PREFIX_PATH=" SHADERCROSS_HOST_SDL,
                },
            },

            // cmake resolves a relative CMAKE_PREFIX_PATH against the build directory, not the
            // working directory, so it has to be made absolute first. hook_expand_cwd does the same
            // for the runtime output directory above, which is subject to the same rule.
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
