/**
 * @file vendor_sqlvec.h
 *
 * sqlite-vec, vector search for SQLite. Built into a static archive, one object.
 *
 * Gives SQL a `vec0` virtual table and a set of `vec_*` functions: brute force k nearest neighbour
 * over float, int8 and binary vectors, with L2, cosine and hamming distance. Everything lives in
 * ordinary SQLite tables, so a vector index is part of the same file as the rest of the game's data
 * and needs no second process. Registered by the sqlite plugin, see plugins/sqlite/sql.c.
 *
 * Named sqlvec throughout — the submodule directory, the archive and the macros — while the upstream
 * project is `asg017/sqlite-vec` and its source file keeps its own name. The short form is what the
 * rest of this tree calls it.
 *
 * Two things upstream does that this rule copies rather than invents:
 *
 * - **The header is generated.** `sqlite-vec.h.tmpl` carries `${VERSION}` placeholders that
 *   `make sqlite-vec.h` fills in with envsubst, and `sqlite-vec.c` includes the result on its first
 *   line. That is the one build step here that shells out to the submodule's own Makefile, and it is
 *   why `gettext` is a host dependency — envsubst comes from it.
 * - **AVX is opt in.** Upstream greps /proc/cpuinfo to decide. This tree targets x86-64 and already
 *   compiles the engine with `-mavx -mavx2`, so the flag is simply on. `SQLITE_VEC_ENABLE_AVX` is
 *   what selects the vectorized distance kernels; without it the scalar fallbacks are used and
 *   nothing else changes.
 *
 * SQLITE_CORE and SQLITE_VEC_STATIC are on `cflags` rather than only on the build parts, because a
 * consumer that includes sqlite-vec.h needs both: SQLITE_CORE picks sqlite3.h over sqlite3ext.h, and
 * without it the header would drag in the extension dispatch macros that rewrite every `sqlite3_*`
 * call in the including file into an indirect one through a pointer nothing sets. SQLITE_VEC_STATIC
 * turns SQLITE_VEC_API into nothing instead of `__declspec(dllexport)`.
 *
 * Link order matters: this archive calls into libsqlite3.a, so it has to appear before it. See the
 * vendor list in vendor.h.
 * */
#pragma once

#include "nyangine/nyangine.h"
#include "build/hooks.h"
#include "build/toolchain.h"
#include "build/vendor/vendor_common.h"
// For SQLITE_BUILD_*: sqlite-vec compiles against sqlite3.h, which only exists once sqlite has been
// configured, so the path to it belongs to that vendor rather than being spelled out again here.
#include "build/vendor/vendor_sqlite.h"

#define SQLVEC_SOURCE_DIRECTORY "./vendor/sqlvec"
#define SQLVEC_SOURCE           SQLVEC_SOURCE_DIRECTORY "/sqlite-vec.c"
#define SQLVEC_HEADER           SQLVEC_SOURCE_DIRECTORY "/sqlite-vec.h"
#define SQLVEC_VERSION_FILE     SQLVEC_SOURCE_DIRECTORY "/VERSION"

#define SQLVEC_BUILD_LINUX_X86_64   SQLVEC_SOURCE_DIRECTORY "/build-linux-x86_64/"
#define SQLVEC_BUILD_WINDOWS_X86_64 SQLVEC_SOURCE_DIRECTORY "/build-windows-x86_64/"

#define SQLVEC_O_LINUX_X86_64   SQLVEC_BUILD_LINUX_X86_64 "sqlite-vec.o"
#define SQLVEC_O_WINDOWS_X86_64 SQLVEC_BUILD_WINDOWS_X86_64 "sqlite-vec.o"

#define SQLVEC_A_LINUX_X86_64   SQLVEC_BUILD_LINUX_X86_64 "libsqlvec.a"
#define SQLVEC_A_WINDOWS_X86_64 SQLVEC_BUILD_WINDOWS_X86_64 "libsqlvec.a"

// clang-format off

/** What both targets compile the one source with, target flags aside. */
#define SQLVEC_CFLAGS               \
    "-c", "-O3", "-std=c11",        \
    "-mavx", "-mavx2",              \
    "-DSQLITE_VEC_ENABLE_AVX",      \
    "-DSQLITE_CORE",                \
    "-DSQLITE_VEC_STATIC",          \
    "-I" SQLVEC_SOURCE_DIRECTORY

// clang-format on

/*
 * Generating the header is target independent — one file, no target in it — so both rules below
 * carry the same part and whichever target is built first does the work; the second finds the header
 * newer than VERSION and skips. Spelled out twice rather than shared because NYA_VendorRule has no
 * notion of a part belonging to two vendors.
 */

NYA_VendorRule vendor_sqlvec_linux_x86_64 = {
    .name = "sqlvec (linux-x86_64)",

    .cflags       = { "-DSQLITE_CORE", "-DSQLITE_VEC_STATIC", },
    .includes     = { "-I" SQLVEC_SOURCE_DIRECTORY, },
    .linker_flags = { SQLVEC_A_LINUX_X86_64, },

    .parts = {
        &(NYA_BuildRule){
            .name = "vendor_sqlvec_linux_x86_64_directory",
            // See the same rule in vendor_sqlean.h: no policy means NYA_BUILD_ALWAYS, and a metarule
            // is dispatched before the policy is read, so this ran on every ./build regardless.
            .policy      = NYA_BUILD_ONCE,
            .is_metarule = true,
            .output_file = SQLVEC_BUILD_LINUX_X86_64,

            .command         = { .working_directory = SQLVEC_BUILD_LINUX_X86_64, },
            .pre_build_hooks = { &hook_create_build_directory, },
        },
        &(NYA_BuildRule){
            .name = "vendor_sqlvec_header",
            // Keyed on VERSION because that is what the template reads: bumping the submodule to a
            // new release has to regenerate the header, and "does sqlite-vec.h exist" would say yes.
            .policy      = NYA_BUILD_IF_OUTDATED,
            .input_file  = SQLVEC_VERSION_FILE,
            .output_file = SQLVEC_HEADER,

            // Upstream's own recipe. It needs envsubst, git and date, all of which the Makefile
            // calls out to, which is why this shells out instead of substituting the placeholders
            // here — a reimplementation would silently stop filling in whatever the template gains
            // next.
            .command = {
                .working_directory = SQLVEC_SOURCE_DIRECTORY,
                .program           = "make",
                .arguments         = { "sqlite-vec.h", },
            },
        },
        &(NYA_BuildRule){
            .name        = "vendor_sqlvec_linux_x86_64_compile",
            .policy      = NYA_BUILD_IF_OUTDATED,
            .input_file  = SQLVEC_SOURCE,
            .output_file = SQLVEC_O_LINUX_X86_64,

            .command = {
                .program   = CC,
                .arguments = {
                    SQLVEC_CFLAGS,
                    "-fPIC",
                    "-I" SQLITE_BUILD_LINUX_X86_64,
                    SQLVEC_SOURCE,
                    "-o", SQLVEC_O_LINUX_X86_64,
                },
            },
        },
        &(NYA_BuildRule){
            .name        = "vendor_sqlvec_linux_x86_64_archive",
            .policy      = NYA_BUILD_IF_OUTDATED,
            .input_file  = SQLVEC_O_LINUX_X86_64,
            .output_file = SQLVEC_A_LINUX_X86_64,

            .command = {
                .program   = "ar",
                .arguments = { "rcs", SQLVEC_A_LINUX_X86_64, SQLVEC_O_LINUX_X86_64, },
            },
        },
    },
};

NYA_VendorRule vendor_sqlvec_windows_x86_64 = {
    .name = "sqlvec (windows-x86_64)",

    .cflags       = { "-DSQLITE_CORE", "-DSQLITE_VEC_STATIC", },
    .includes     = { "-I" SQLVEC_SOURCE_DIRECTORY, },
    .linker_flags = { SQLVEC_A_WINDOWS_X86_64, },

    .parts = {
        &(NYA_BuildRule){
            .name        = "vendor_sqlvec_windows_x86_64_directory",
            .policy      = NYA_BUILD_ONCE,
            .is_metarule = true,
            .output_file = SQLVEC_BUILD_WINDOWS_X86_64,

            .command         = { .working_directory = SQLVEC_BUILD_WINDOWS_X86_64, },
            .pre_build_hooks = { &hook_create_build_directory, },
        },
        &(NYA_BuildRule){
            .name        = "vendor_sqlvec_header_windows",
            .policy      = NYA_BUILD_IF_OUTDATED,
            .input_file  = SQLVEC_VERSION_FILE,
            .output_file = SQLVEC_HEADER,

            .command = {
                .working_directory = SQLVEC_SOURCE_DIRECTORY,
                .program           = "make",
                .arguments         = { "sqlite-vec.h", },
            },
        },
        &(NYA_BuildRule){
            .name        = "vendor_sqlvec_windows_x86_64_compile",
            .policy      = NYA_BUILD_IF_OUTDATED,
            .input_file  = SQLVEC_SOURCE,
            .output_file = SQLVEC_O_WINDOWS_X86_64,

            // No -fPIC: position independent code is the default and meaningless for a PE, and
            // mingw-w64's gcc warns that the flag is ignored.
            .command = {
                .program   = NYA_WINDOWS_CC,
                .arguments = {
                    SQLVEC_CFLAGS,
                    "-I" SQLITE_BUILD_WINDOWS_X86_64,
                    SQLVEC_SOURCE,
                    "-o", SQLVEC_O_WINDOWS_X86_64,
                },
            },
        },
        &(NYA_BuildRule){
            .name        = "vendor_sqlvec_windows_x86_64_archive",
            .policy      = NYA_BUILD_IF_OUTDATED,
            .input_file  = SQLVEC_O_WINDOWS_X86_64,
            .output_file = SQLVEC_A_WINDOWS_X86_64,

            .command = {
                .program   = NYA_WINDOWS_AR,
                .arguments = { "rcs", SQLVEC_A_WINDOWS_X86_64, SQLVEC_O_WINDOWS_X86_64, },
            },
        },
    },
};
