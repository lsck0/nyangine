/**
 * @file vendor_sdl_shadercross.h
 *
 * SDL_shadercross. Builds a tool, not a library, so it has no consumer flags. The shader rules run
 * the binary.
 * */
#pragma once

#include "nyangine-core/nyangine.h"
#include "build/hooks.h"
#include "build/flags.h"
#include "build/vendor/vendor_common.h"
#include "build/vendor/vendor_sdl.h"

#define SHADERCROSS_SOURCE "./vendor/sdl-shadercross"

/* Keyed by host, not target: shadercross is a build-time tool and its .dxil/.msl/.spv output does not depend on the target, so one binary per machine. */
#if OS_WINDOWS
#define SHADERCROSS_BUILD    "./vendor/sdl-shadercross/build-windows-x86_64"
#define SHADERCROSS_BINARY   SHADERCROSS_BUILD "/shadercross.exe"
#define SHADERCROSS_HOST_SDL SDL_BUILD_WINDOWS_X86_64
#else
#define SHADERCROSS_BUILD    "./vendor/sdl-shadercross/build-linux-x86_64"
#define SHADERCROSS_BINARY   SHADERCROSS_BUILD "/shadercross"
#define SHADERCROSS_HOST_SDL SDL_BUILD_LINUX_X86_64
#endif

/* Where shadercross finds libSDL3_shadercross, SPIRV-Cross and DXC. They are not on the loader path and the binary has no RPATH, so without this it exits 127 with no diagnostic. Carries its own trailing comma since the Windows expansion is empty (as FLAGS_TARGET_WINDOWS_X86_64). Empty on Windows: NYA_Command _putenv's environment entries into this process (command_windows.c), so PATH= would replace the build tool's PATH for every later command. The Windows loader searches the .exe's directory first, so the configure below puts the DLLs there. */
#if OS_WINDOWS
#define SHADERCROSS_LIBRARY_PATH
#else
#define SHADERCROSS_LIBRARY_PATH                                                                                                                     \
    "LD_LIBRARY_PATH=" SHADERCROSS_BUILD ":" SHADERCROSS_BUILD "/external/SPIRV-Cross:" SHADERCROSS_BUILD                                            \
    "/external/DirectXShaderCompiler/lib:" SHADERCROSS_HOST_SDL,
#endif

/* SPIRV-Cross, vendored inside sdl-shadercross and linked into the build tool so nya_asset_compile_shaders cross-compiles every .spv to GLSL ES 300 in-process for a later GLES3/WebGL2 backend. Wired like libbacktrace: SHADERCROSS_SPIRV_CROSS_INCLUDE resolves <spirv_cross_c.h>, _LINK is for the linker, _SO is the path main() probes, and the flags are appended only when the artifact exists. Host only — a Windows host cannot build shadercross (DXC will not compile under MinGW) and takes the compiled shaders from a Linux machine. */
#if OS_WINDOWS
#define SHADERCROSS_SPIRV_CROSS_INCLUDE
#define SHADERCROSS_SPIRV_CROSS_LINK
#define SHADERCROSS_SPIRV_CROSS_SO ""
#else
#define SHADERCROSS_SPIRV_CROSS_DIRECTORY SHADERCROSS_BUILD "/external/SPIRV-Cross"
#define SHADERCROSS_SPIRV_CROSS_SO        SHADERCROSS_SPIRV_CROSS_DIRECTORY "/libspirv-cross-c-shared.so"
#define SHADERCROSS_SPIRV_CROSS_INCLUDE   "-I" SHADERCROSS_SOURCE "/external/SPIRV-Cross"
// $ORIGIN, not the build directory: the tool runs from the tree root as ./build, and an $ORIGIN rpath
// still resolves when the tree is not the main checkout (a worktree symlinks vendor/ back to it).
#define SHADERCROSS_SPIRV_CROSS_LINK                                                                                                                 \
    "-L" SHADERCROSS_SPIRV_CROSS_DIRECTORY, "-lspirv-cross-c-shared",                                                                                \
    "-Wl,-rpath,$ORIGIN/vendor/sdl-shadercross/build-linux-x86_64/external/SPIRV-Cross"
#endif

/* Windows only: puts every DLL in the tree, subprojects included, next to shadercross.exe, replacing LD_LIBRARY_PATH. Absolute via %CWD%/hook_expand_cwd, since a relative output directory resolves per target. */
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
                    // point at the SDL just built, not a system SDL3 that exists on dev machines but not in CI.
                    "-DCMAKE_PREFIX_PATH=" SHADERCROSS_HOST_SDL,
                },
            },

            // cmake resolves a relative CMAKE_PREFIX_PATH against the build directory, so it must be made absolute first; hook_expand_cwd does the same for the runtime output dir above.
            .pre_build_hooks = { &hook_absolutize_cmake_prefix_path, &hook_expand_cwd, &hook_invalidate_stale_cmake_cache, },
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

/* DXC does not compile under MinGW, so a Windows host does not build shadercross and uses shaders compiled on a Linux host instead. See nya_asset_compile_shaders. */
#if OS_WINDOWS
#define SHADERCROSS_HOST_VENDOR
#else
#define SHADERCROSS_HOST_VENDOR &vendor_sdl_shadercross_host,
#endif
