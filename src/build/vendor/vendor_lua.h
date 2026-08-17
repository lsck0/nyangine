/**
 * @file vendor_lua.h
 *
 * LuaJIT. Plain Makefile that builds in tree, so like lz4 the two targets would fight over the
 * same archive path and the Windows one is renamed afterwards.
 *
 * Cross compiling LuaJIT is unusual: it builds a host side code generator first, so HOST_CC has to
 * stay the host compiler while CROSS points at the target toolchain.
 * */
#pragma once

#include "nyangine/nyangine.h"
#include "build/hooks.h"
#include "build/toolchain.h"
#include "build/vendor/vendor_common.h"

#define LUAJIT_SRC "./vendor/lua/src"
#define LUAJIT_A   LUAJIT_SRC "/libluajit.a"
#define LUAJIT_A_LIN                                                                                                                                 \
    "./vendor/lua"                                                                                                                                   \
    "/libluajit-linux.a"
#define LUAJIT_A_WIN                                                                                                                                 \
    "./vendor/lua"                                                                                                                                   \
    "/libluajit-windows.a"

NYA_VendorRule vendor_lua_linux_x86_64 = {
    .name = "lua (linux-x86_64)",

    .includes     = { "-I./vendor/lua/src/", },
    .linker_flags = { LUAJIT_A_LIN, },

    .parts = {
        /*
         * Cleaned first, exactly as the Windows rule below does, and for the same reason.
         *
         * LuaJIT builds in tree, so both targets compile into ./vendor/lua/src and only the archive
         * is renamed afterwards. Its Makefile keys off the object files alone and knows nothing
         * about which toolchain produced them, so a tree left over from the Windows build is
         * "up to date" for the Linux one: make relinks those objects and libluajit-linux.a comes
         * out full of COFF. lld then skips every member with a warning, and because nothing calls
         * into Lua yet the link still succeeds — a dependency that is silently absent rather than
         * missing. Whichever target runs second has to start from an empty tree.
         */
        &(NYA_BuildRule){
            .name        = "vendor_lua_linux_x86_64_clean",
            .policy      = NYA_BUILD_ONCE,
            .output_file = LUAJIT_A_LIN,

            .command = {
                .program   = "make",
                .arguments = { "-C", LUAJIT_SRC, "clean", },
            },
        },
        &(NYA_BuildRule){
            .name        = "vendor_lua_linux_x86_64_compile",
            .policy      = NYA_BUILD_ONCE,
            .output_file = LUAJIT_A_LIN,

            .command = {
                .program   = "make",
                .arguments = { "-C", LUAJIT_SRC, "libluajit.a", "-j", NPROCS, "CC=" CC, },
            },
        },
        &(NYA_BuildRule){
            .name        = "vendor_lua_linux_x86_64_rename",
            .policy      = NYA_BUILD_ONCE,
            .is_metarule = true,
            .input_file  = LUAJIT_A,
            .output_file = LUAJIT_A_LIN,

            // Parked one directory up, outside what `make clean` sweeps: this project builds in
            // tree and its clean target globs *.a, so a sibling archive would be deleted too.
            .post_build_hooks = { &hook_move_file, },
        },
    },
};

NYA_VendorRule vendor_lua_windows_x86_64 = {
    .name = "lua (windows-x86_64)",

    .includes     = { "-I./vendor/lua/src/", },
    .linker_flags = { LUAJIT_A_WIN, },

    .parts = {
        &(NYA_BuildRule){
            .name        = "vendor_lua_windows_x86_64_clean",
            .policy      = NYA_BUILD_ONCE,
            .output_file = LUAJIT_A_WIN,

            .command = {
                .program   = "make",
                .arguments = { "-C", LUAJIT_SRC, "clean", },
            },
        },
        &(NYA_BuildRule){
            .name        = "vendor_lua_windows_x86_64_compile",
            .policy      = NYA_BUILD_ONCE,
            .output_file = LUAJIT_A_WIN,

            .command = {
                .program   = "make",
                .arguments = {
                    "-C", LUAJIT_SRC, "libluajit.a", "-j", NPROCS,
                    "TARGET_SYS=Windows",
                    // The code generator runs on the host, so it must not be built with the
                    // cross compiler. Expands to nothing on a Windows host.
                    NYA_LUAJIT_CROSS
                },
            },
        },
        &(NYA_BuildRule){
            .name        = "vendor_lua_windows_x86_64_rename",
            .policy      = NYA_BUILD_ONCE,
            .is_metarule = true,
            .input_file  = LUAJIT_A,
            .output_file = LUAJIT_A_WIN,

            // hook rather than `mv`, which does not exist on Windows.
            .post_build_hooks = { &hook_move_file, },
        },
    },
};
